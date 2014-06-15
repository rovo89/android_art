/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "space.h"

#include "base/logging.h"
#include "gc/accounting/heap_bitmap.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "runtime.h"
#include "thread-inl.h"

namespace art {
namespace gc {
namespace space {

Space::Space(const std::string& name, GcRetentionPolicy gc_retention_policy)
    : name_(name), gc_retention_policy_(gc_retention_policy) { }

void Space::Dump(std::ostream& os) const {
  os << GetName() << ":" << GetGcRetentionPolicy();
}

std::ostream& operator<<(std::ostream& os, const Space& space) {
  space.Dump(os);
  return os;
}

DlMallocSpace* Space::AsDlMallocSpace() {
  LOG(FATAL) << "Unreachable";
  return nullptr;
}

RosAllocSpace* Space::AsRosAllocSpace() {
  LOG(FATAL) << "Unreachable";
  return nullptr;
}

ZygoteSpace* Space::AsZygoteSpace() {
  LOG(FATAL) << "Unreachable";
  return nullptr;
}

BumpPointerSpace* Space::AsBumpPointerSpace() {
  LOG(FATAL) << "Unreachable";
  return nullptr;
}

AllocSpace* Space::AsAllocSpace() {
  LOG(FATAL) << "Unimplemented";
  return nullptr;
}

ContinuousMemMapAllocSpace* Space::AsContinuousMemMapAllocSpace() {
  LOG(FATAL) << "Unimplemented";
  return nullptr;
}

DiscontinuousSpace::DiscontinuousSpace(const std::string& name,
                                       GcRetentionPolicy gc_retention_policy) :
    Space(name, gc_retention_policy) {
  // TODO: Fix this if we ever support objects not in the low 32 bit.
  const size_t capacity = static_cast<size_t>(std::numeric_limits<uint32_t>::max());
  live_bitmap_.reset(accounting::LargeObjectBitmap::Create("large live objects", nullptr,
                                                           capacity));
  CHECK(live_bitmap_.get() != nullptr);
  mark_bitmap_.reset(accounting::LargeObjectBitmap::Create("large marked objects", nullptr,
                                                           capacity));
  CHECK(mark_bitmap_.get() != nullptr);
}

collector::ObjectBytePair ContinuousMemMapAllocSpace::Sweep(bool swap_bitmaps) {
  accounting::ContinuousSpaceBitmap* live_bitmap = GetLiveBitmap();
  accounting::ContinuousSpaceBitmap* mark_bitmap = GetMarkBitmap();
  // If the bitmaps are bound then sweeping this space clearly won't do anything.
  if (live_bitmap == mark_bitmap) {
    return collector::ObjectBytePair(0, 0);
  }
  SweepCallbackContext scc(swap_bitmaps, this);
  if (swap_bitmaps) {
    std::swap(live_bitmap, mark_bitmap);
  }
  // Bitmaps are pre-swapped for optimization which enables sweeping with the heap unlocked.
  accounting::ContinuousSpaceBitmap::SweepWalk(
      *live_bitmap, *mark_bitmap, reinterpret_cast<uintptr_t>(Begin()),
      reinterpret_cast<uintptr_t>(End()), GetSweepCallback(), reinterpret_cast<void*>(&scc));
  return scc.freed;
}

// Returns the old mark bitmap.
void ContinuousMemMapAllocSpace::BindLiveToMarkBitmap() {
  CHECK(!HasBoundBitmaps());
  accounting::ContinuousSpaceBitmap* live_bitmap = GetLiveBitmap();
  if (live_bitmap != mark_bitmap_.get()) {
    accounting::ContinuousSpaceBitmap* mark_bitmap = mark_bitmap_.release();
    Runtime::Current()->GetHeap()->GetMarkBitmap()->ReplaceBitmap(mark_bitmap, live_bitmap);
    temp_bitmap_.reset(mark_bitmap);
    mark_bitmap_.reset(live_bitmap);
  }
}

bool ContinuousMemMapAllocSpace::HasBoundBitmaps() const {
  return temp_bitmap_.get() != nullptr;
}

void ContinuousMemMapAllocSpace::UnBindBitmaps() {
  CHECK(HasBoundBitmaps());
  // At this point, the temp_bitmap holds our old mark bitmap.
  accounting::ContinuousSpaceBitmap* new_bitmap = temp_bitmap_.release();
  Runtime::Current()->GetHeap()->GetMarkBitmap()->ReplaceBitmap(mark_bitmap_.get(), new_bitmap);
  CHECK_EQ(mark_bitmap_.release(), live_bitmap_.get());
  mark_bitmap_.reset(new_bitmap);
  DCHECK(temp_bitmap_.get() == nullptr);
}

void ContinuousMemMapAllocSpace::SwapBitmaps() {
  live_bitmap_.swap(mark_bitmap_);
  // Swap names to get more descriptive diagnostics.
  std::string temp_name(live_bitmap_->GetName());
  live_bitmap_->SetName(mark_bitmap_->GetName());
  mark_bitmap_->SetName(temp_name);
}

AllocSpace::SweepCallbackContext::SweepCallbackContext(bool swap_bitmaps, space::Space* space)
    : swap_bitmaps(swap_bitmaps), space(space), self(Thread::Current()) {
}

}  // namespace space
}  // namespace gc
}  // namespace art
