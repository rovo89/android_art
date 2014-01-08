/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include "malloc_space.h"

#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/heap.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "runtime.h"
#include "thread.h"
#include "thread_list.h"
#include "utils.h"

namespace art {
namespace gc {
namespace space {

size_t MallocSpace::bitmap_index_ = 0;

MallocSpace::MallocSpace(const std::string& name, MemMap* mem_map,
                         byte* begin, byte* end, byte* limit, size_t growth_limit)
    : ContinuousMemMapAllocSpace(name, mem_map, begin, end, limit, kGcRetentionPolicyAlwaysCollect),
      recent_free_pos_(0), lock_("allocation space lock", kAllocSpaceLock),
      growth_limit_(growth_limit) {
  size_t bitmap_index = bitmap_index_++;
  static const uintptr_t kGcCardSize = static_cast<uintptr_t>(accounting::CardTable::kCardSize);
  CHECK(IsAligned<kGcCardSize>(reinterpret_cast<uintptr_t>(mem_map->Begin())));
  CHECK(IsAligned<kGcCardSize>(reinterpret_cast<uintptr_t>(mem_map->End())));
  live_bitmap_.reset(accounting::SpaceBitmap::Create(
      StringPrintf("allocspace %s live-bitmap %d", name.c_str(), static_cast<int>(bitmap_index)),
      Begin(), Capacity()));
  DCHECK(live_bitmap_.get() != NULL) << "could not create allocspace live bitmap #" << bitmap_index;
  mark_bitmap_.reset(accounting::SpaceBitmap::Create(
      StringPrintf("allocspace %s mark-bitmap %d", name.c_str(), static_cast<int>(bitmap_index)),
      Begin(), Capacity()));
  DCHECK(live_bitmap_.get() != NULL) << "could not create allocspace mark bitmap #" << bitmap_index;
  for (auto& freed : recent_freed_objects_) {
    freed.first = nullptr;
    freed.second = nullptr;
  }
}

MemMap* MallocSpace::CreateMemMap(const std::string& name, size_t starting_size, size_t* initial_size,
                                  size_t* growth_limit, size_t* capacity, byte* requested_begin) {
  // Sanity check arguments
  if (starting_size > *initial_size) {
    *initial_size = starting_size;
  }
  if (*initial_size > *growth_limit) {
    LOG(ERROR) << "Failed to create alloc space (" << name << ") where the initial size ("
        << PrettySize(*initial_size) << ") is larger than its capacity ("
        << PrettySize(*growth_limit) << ")";
    return NULL;
  }
  if (*growth_limit > *capacity) {
    LOG(ERROR) << "Failed to create alloc space (" << name << ") where the growth limit capacity ("
        << PrettySize(*growth_limit) << ") is larger than the capacity ("
        << PrettySize(*capacity) << ")";
    return NULL;
  }

  // Page align growth limit and capacity which will be used to manage mmapped storage
  *growth_limit = RoundUp(*growth_limit, kPageSize);
  *capacity = RoundUp(*capacity, kPageSize);

  std::string error_msg;
  MemMap* mem_map = MemMap::MapAnonymous(name.c_str(), requested_begin, *capacity,
                                         PROT_READ | PROT_WRITE, &error_msg);
  if (mem_map == NULL) {
    LOG(ERROR) << "Failed to allocate pages for alloc space (" << name << ") of size "
               << PrettySize(*capacity) << ": " << error_msg;
    return NULL;
  }
  return mem_map;
}

void MallocSpace::SwapBitmaps() {
  live_bitmap_.swap(mark_bitmap_);
  // Swap names to get more descriptive diagnostics.
  std::string temp_name(live_bitmap_->GetName());
  live_bitmap_->SetName(mark_bitmap_->GetName());
  mark_bitmap_->SetName(temp_name);
}

mirror::Class* MallocSpace::FindRecentFreedObject(const mirror::Object* obj) {
  size_t pos = recent_free_pos_;
  // Start at the most recently freed object and work our way back since there may be duplicates
  // caused by dlmalloc reusing memory.
  if (kRecentFreeCount > 0) {
    for (size_t i = 0; i + 1 < kRecentFreeCount + 1; ++i) {
      pos = pos != 0 ? pos - 1 : kRecentFreeMask;
      if (recent_freed_objects_[pos].first == obj) {
        return recent_freed_objects_[pos].second;
      }
    }
  }
  return nullptr;
}

void MallocSpace::RegisterRecentFree(mirror::Object* ptr) {
  recent_freed_objects_[recent_free_pos_] = std::make_pair(ptr, ptr->GetClass());
  recent_free_pos_ = (recent_free_pos_ + 1) & kRecentFreeMask;
}

void MallocSpace::SetGrowthLimit(size_t growth_limit) {
  growth_limit = RoundUp(growth_limit, kPageSize);
  growth_limit_ = growth_limit;
  if (Size() > growth_limit_) {
    end_ = begin_ + growth_limit;
  }
}

void* MallocSpace::MoreCore(intptr_t increment) {
  CheckMoreCoreForPrecondition();
  byte* original_end = end_;
  if (increment != 0) {
    VLOG(heap) << "MallocSpace::MoreCore " << PrettySize(increment);
    byte* new_end = original_end + increment;
    if (increment > 0) {
      // Should never be asked to increase the allocation beyond the capacity of the space. Enforced
      // by mspace_set_footprint_limit.
      CHECK_LE(new_end, Begin() + Capacity());
      CHECK_MEMORY_CALL(mprotect, (original_end, increment, PROT_READ | PROT_WRITE), GetName());
    } else {
      // Should never be asked for negative footprint (ie before begin). Zero footprint is ok.
      CHECK_GE(original_end + increment, Begin());
      // Advise we don't need the pages and protect them
      // TODO: by removing permissions to the pages we may be causing TLB shoot-down which can be
      // expensive (note the same isn't true for giving permissions to a page as the protected
      // page shouldn't be in a TLB). We should investigate performance impact of just
      // removing ignoring the memory protection change here and in Space::CreateAllocSpace. It's
      // likely just a useful debug feature.
      size_t size = -increment;
      CHECK_MEMORY_CALL(madvise, (new_end, size, MADV_DONTNEED), GetName());
      CHECK_MEMORY_CALL(mprotect, (new_end, size, PROT_NONE), GetName());
    }
    // Update end_
    end_ = new_end;
  }
  return original_end;
}

// Returns the old mark bitmap.
accounting::SpaceBitmap* MallocSpace::BindLiveToMarkBitmap() {
  accounting::SpaceBitmap* live_bitmap = GetLiveBitmap();
  accounting::SpaceBitmap* mark_bitmap = mark_bitmap_.release();
  temp_bitmap_.reset(mark_bitmap);
  mark_bitmap_.reset(live_bitmap);
  return mark_bitmap;
}

bool MallocSpace::HasBoundBitmaps() const {
  return temp_bitmap_.get() != nullptr;
}

void MallocSpace::UnBindBitmaps() {
  CHECK(HasBoundBitmaps());
  // At this point, the temp_bitmap holds our old mark bitmap.
  accounting::SpaceBitmap* new_bitmap = temp_bitmap_.release();
  CHECK_EQ(mark_bitmap_.release(), live_bitmap_.get());
  mark_bitmap_.reset(new_bitmap);
  DCHECK(temp_bitmap_.get() == NULL);
}

MallocSpace* MallocSpace::CreateZygoteSpace(const char* alloc_space_name, bool low_memory_mode) {
  // For RosAlloc, revoke thread local runs before creating a new
  // alloc space so that we won't mix thread local runs from different
  // alloc spaces.
  RevokeAllThreadLocalBuffers();
  end_ = reinterpret_cast<byte*>(RoundUp(reinterpret_cast<uintptr_t>(end_), kPageSize));
  DCHECK(IsAligned<accounting::CardTable::kCardSize>(begin_));
  DCHECK(IsAligned<accounting::CardTable::kCardSize>(end_));
  DCHECK(IsAligned<kPageSize>(begin_));
  DCHECK(IsAligned<kPageSize>(end_));
  size_t size = RoundUp(Size(), kPageSize);
  // Trim the heap so that we minimize the size of the Zygote space.
  Trim();
  // TODO: Not hardcode these in?
  const size_t starting_size = kPageSize;
  const size_t initial_size = 2 * MB;
  // Remaining size is for the new alloc space.
  const size_t growth_limit = growth_limit_ - size;
  const size_t capacity = Capacity() - size;
  VLOG(heap) << "Begin " << reinterpret_cast<const void*>(begin_) << "\n"
             << "End " << reinterpret_cast<const void*>(end_) << "\n"
             << "Size " << size << "\n"
             << "GrowthLimit " << growth_limit_ << "\n"
             << "Capacity " << Capacity();
  SetGrowthLimit(RoundUp(size, kPageSize));
  SetFootprintLimit(RoundUp(size, kPageSize));
  // FIXME: Do we need reference counted pointers here?
  // Make the two spaces share the same mark bitmaps since the bitmaps span both of the spaces.
  VLOG(heap) << "Creating new AllocSpace: ";
  VLOG(heap) << "Size " << GetMemMap()->Size();
  VLOG(heap) << "GrowthLimit " << PrettySize(growth_limit);
  VLOG(heap) << "Capacity " << PrettySize(capacity);
  // Remap the tail.
  std::string error_msg;
  UniquePtr<MemMap> mem_map(GetMemMap()->RemapAtEnd(end_, alloc_space_name,
                                                    PROT_READ | PROT_WRITE, &error_msg));
  CHECK(mem_map.get() != nullptr) << error_msg;
  void* allocator = CreateAllocator(end_, starting_size, initial_size, low_memory_mode);
  // Protect memory beyond the initial size.
  byte* end = mem_map->Begin() + starting_size;
  if (capacity - initial_size > 0) {
    CHECK_MEMORY_CALL(mprotect, (end, capacity - initial_size, PROT_NONE), alloc_space_name);
  }
  MallocSpace* alloc_space = CreateInstance(alloc_space_name, mem_map.release(), allocator,
                                            end_, end, limit_, growth_limit);
  SetLimit(End());
  live_bitmap_->SetHeapLimit(reinterpret_cast<uintptr_t>(End()));
  CHECK_EQ(live_bitmap_->HeapLimit(), reinterpret_cast<uintptr_t>(End()));
  mark_bitmap_->SetHeapLimit(reinterpret_cast<uintptr_t>(End()));
  CHECK_EQ(mark_bitmap_->HeapLimit(), reinterpret_cast<uintptr_t>(End()));
  VLOG(heap) << "zygote space creation done";
  return alloc_space;
}

void MallocSpace::Dump(std::ostream& os) const {
  os << GetType()
      << " begin=" << reinterpret_cast<void*>(Begin())
      << ",end=" << reinterpret_cast<void*>(End())
      << ",size=" << PrettySize(Size()) << ",capacity=" << PrettySize(Capacity())
      << ",name=\"" << GetName() << "\"]";
}

struct SweepCallbackContext {
  bool swap_bitmaps;
  Heap* heap;
  space::MallocSpace* space;
  Thread* self;
  size_t freed_objects;
  size_t freed_bytes;
};

static void SweepCallback(size_t num_ptrs, mirror::Object** ptrs, void* arg) {
  SweepCallbackContext* context = static_cast<SweepCallbackContext*>(arg);
  space::AllocSpace* space = context->space;
  Thread* self = context->self;
  Locks::heap_bitmap_lock_->AssertExclusiveHeld(self);
  // If the bitmaps aren't swapped we need to clear the bits since the GC isn't going to re-swap
  // the bitmaps as an optimization.
  if (!context->swap_bitmaps) {
    accounting::SpaceBitmap* bitmap = context->space->GetLiveBitmap();
    for (size_t i = 0; i < num_ptrs; ++i) {
      bitmap->Clear(ptrs[i]);
    }
  }
  // Use a bulk free, that merges consecutive objects before freeing or free per object?
  // Documentation suggests better free performance with merging, but this may be at the expensive
  // of allocation.
  context->freed_objects += num_ptrs;
  context->freed_bytes += space->FreeList(self, num_ptrs, ptrs);
}

static void ZygoteSweepCallback(size_t num_ptrs, mirror::Object** ptrs, void* arg) {
  SweepCallbackContext* context = static_cast<SweepCallbackContext*>(arg);
  Locks::heap_bitmap_lock_->AssertExclusiveHeld(context->self);
  accounting::CardTable* card_table = context->heap->GetCardTable();
  // If the bitmaps aren't swapped we need to clear the bits since the GC isn't going to re-swap
  // the bitmaps as an optimization.
  if (!context->swap_bitmaps) {
    accounting::SpaceBitmap* bitmap = context->space->GetLiveBitmap();
    for (size_t i = 0; i < num_ptrs; ++i) {
      bitmap->Clear(ptrs[i]);
    }
  }
  // We don't free any actual memory to avoid dirtying the shared zygote pages.
  for (size_t i = 0; i < num_ptrs; ++i) {
    // Need to mark the card since this will update the mod-union table next GC cycle.
    card_table->MarkCard(ptrs[i]);
  }
}

void MallocSpace::Sweep(bool swap_bitmaps, size_t* freed_objects, size_t* freed_bytes) {
  DCHECK(freed_objects != nullptr);
  DCHECK(freed_bytes != nullptr);
  accounting::SpaceBitmap* live_bitmap = GetLiveBitmap();
  accounting::SpaceBitmap* mark_bitmap = GetMarkBitmap();
  // If the bitmaps are bound then sweeping this space clearly won't do anything.
  if (live_bitmap == mark_bitmap) {
    return;
  }
  SweepCallbackContext scc;
  scc.swap_bitmaps = swap_bitmaps;
  scc.heap = Runtime::Current()->GetHeap();
  scc.self = Thread::Current();
  scc.space = this;
  scc.freed_objects = 0;
  scc.freed_bytes = 0;
  if (swap_bitmaps) {
    std::swap(live_bitmap, mark_bitmap);
  }
  // Bitmaps are pre-swapped for optimization which enables sweeping with the heap unlocked.
  accounting::SpaceBitmap::SweepWalk(*live_bitmap, *mark_bitmap,
                                     reinterpret_cast<uintptr_t>(Begin()),
                                     reinterpret_cast<uintptr_t>(End()),
                                     IsZygoteSpace() ? &ZygoteSweepCallback : &SweepCallback,
                                     reinterpret_cast<void*>(&scc));
  *freed_objects += scc.freed_objects;
  *freed_bytes += scc.freed_bytes;
}

}  // namespace space
}  // namespace gc
}  // namespace art
