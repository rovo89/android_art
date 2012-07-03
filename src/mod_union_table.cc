/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "heap.h"
#include "heap_bitmap.h"
#include "mark_sweep.h"
#include "mod_union_table.h"
#include "space.h"
#include "stl_util.h"
#include "UniquePtr.h"

namespace art {

class MarkIfReachesAllocspaceVisitor {
 public:
  explicit MarkIfReachesAllocspaceVisitor(MarkSweep* const mark_sweep, SpaceBitmap* bitmap)
    : mark_sweep_(mark_sweep),
      bitmap_(bitmap) {
  }

  // Extra parameters are required since we use this same visitor signature for checking objects.
  void operator ()(const Object* obj, const Object* ref, MemberOffset offset, bool is_static) const {
    // TODO: Optimize?
    // TODO: C++0x auto
    const Spaces& spaces = mark_sweep_->heap_->GetSpaces();
    for (Spaces::const_iterator cur = spaces.begin(); cur != spaces.end(); ++cur) {
      if ((*cur)->IsAllocSpace() && (*cur)->Contains(ref)) {
        bitmap_->Set(obj);
      }
    }

    // Avoid warnings.
    (void)obj;(void)offset;(void)is_static;
  }

 private:
  MarkSweep* const mark_sweep_;
  SpaceBitmap* bitmap_;
};

class ModUnionVisitor {
 public:
  explicit ModUnionVisitor(MarkSweep* const mark_sweep, SpaceBitmap* bitmap)
    : mark_sweep_(mark_sweep),
      bitmap_(bitmap) {
  }

  void operator ()(Object* obj) const {
    DCHECK(obj != NULL);
    // We don't have an early exit since we use the visitor pattern, an early
    // exit should significantly speed this up.
    MarkIfReachesAllocspaceVisitor visitor(mark_sweep_, bitmap_);
    mark_sweep_->VisitObjectReferences(obj, visitor);
  }
 private:
  MarkSweep* const mark_sweep_;
  SpaceBitmap* bitmap_;
};

class ModUnionClearCardVisitor {
 public:
  explicit ModUnionClearCardVisitor(std::vector<byte*>* cleared_cards)
    : cleared_cards_(cleared_cards) {
  }

  void operator ()(byte* card) const {
    cleared_cards_->push_back(card);
  }
 private:
  std::vector<byte*>* cleared_cards_;
};

ModUnionTableBitmap::ModUnionTableBitmap(Heap* heap) : heap_(heap) {
  // Prevent fragmentation of the heap which is caused by resizing of the vector.
  // TODO: Make a new vector which uses madvise (basically same as a mark stack).
  cleared_cards_.reserve(4 * KB);
}

ModUnionTableBitmap::~ModUnionTableBitmap() {
  STLDeleteValues(&bitmaps_);
}

void ModUnionTableBitmap::Init() {
  const std::vector<Space*>& spaces = heap_->GetSpaces();

  // Create one heap bitmap per image space.
  for (size_t i = 0; i < spaces.size(); ++i) {
    if (spaces[i]->IsImageSpace()) {
      // Allocate the mod-union table
      // The mod-union table is only needed when we have an image space since it's purpose is to cache image roots.
      UniquePtr<SpaceBitmap> bitmap(SpaceBitmap::Create("mod-union table bitmap", spaces[i]->Begin(), spaces[i]->Capacity()));
      if (bitmap.get() == NULL) {
        LOG(FATAL) << "Failed to create mod-union bitmap";
      }

      bitmaps_.Put(spaces[i], bitmap.release());
    }
  }
}

void ModUnionTableBitmap::ClearCards() {
  CardTable* card_table = heap_->GetCardTable();
  for (BitmapMap::iterator cur = bitmaps_.begin(); cur != bitmaps_.end(); ++cur) {
    const Space* space = cur->first;
    ModUnionClearCardVisitor visitor(&cleared_cards_);
    // Clear dirty cards in the this image space and update the corresponding mod-union bits.
    card_table->VisitClear(space->Begin(), space->End(), visitor);
  }
}

void ModUnionTableBitmap::Update(MarkSweep* mark_sweep) {
  CardTable* card_table = heap_->GetCardTable();

  while (!cleared_cards_.empty()) {
    byte* card = cleared_cards_.back();
    cleared_cards_.pop_back();

    // Find out which bitmap the card maps to.
    SpaceBitmap* bitmap = 0;
    const Space* space = 0;
    for (BitmapMap::iterator cur = bitmaps_.begin(); cur != bitmaps_.end(); ++cur) {
      space = cur->first;
      if (space->Contains(reinterpret_cast<Object*>(card_table->AddrFromCard(card)))) {
        bitmap = cur->second;
        break;
      }
    }
    DCHECK(bitmap != NULL);

    uintptr_t start = reinterpret_cast<uintptr_t>(card_table->AddrFromCard(card));
    uintptr_t end = reinterpret_cast<uintptr_t>(card_table->AddrFromCard(card + 1));

    // Clear the mod-union bitmap range corresponding to this card so that we
    // don't have any objects marked which do not reach the alloc space.
    bitmap->VisitRange(start, end, SpaceBitmap::ClearVisitor(bitmap));

    // At this point we need to update the mod-union bitmap to contain all the
    // objects which reach the alloc space.
    ModUnionVisitor visitor(mark_sweep, bitmap);
    space->GetLiveBitmap()->VisitMarkedRange(start, end, visitor);
  }
}

void ModUnionTableBitmap::MarkReferences(MarkSweep* mark_sweep) {
  // Some tests have no image space, and therefore no mod-union bitmap.
  for (BitmapMap::iterator cur = bitmaps_.begin(); cur != bitmaps_.end(); ++cur) {
    const Space* space = cur->first;
    uintptr_t begin = reinterpret_cast<uintptr_t>(space->Begin());
    uintptr_t end = reinterpret_cast<uintptr_t>(space->End());
    cur->second->VisitRange(begin, end, MarkSweep::ScanImageRootVisitor, mark_sweep);
  }
}

}  // namespace art

