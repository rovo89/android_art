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

#include "mod_union_table.h"

#include "base/stl_util.h"
#include "card_table-inl.h"
#include "heap.h"
#include "heap_bitmap.h"
#include "mark_sweep.h"
#include "mark_sweep-inl.h"
#include "mirror/object-inl.h"
#include "mirror/class-inl.h"
#include "mirror/field-inl.h"
#include "mirror/object_array-inl.h"
#include "space.h"
#include "space_bitmap-inl.h"
#include "thread.h"
#include "UniquePtr.h"

using namespace art::mirror;

namespace art {

class MarkIfReachesAllocspaceVisitor {
 public:
  explicit MarkIfReachesAllocspaceVisitor(Heap* const heap, SpaceBitmap* bitmap)
    : heap_(heap),
      bitmap_(bitmap) {
  }

  // Extra parameters are required since we use this same visitor signature for checking objects.
  void operator ()(const Object* obj, const Object* ref, const MemberOffset& /* offset */, bool /* is_static */) const {
    // TODO: Optimize?
    // TODO: C++0x auto
    const Spaces& spaces = heap_->GetSpaces();
    for (Spaces::const_iterator cur = spaces.begin(); cur != spaces.end(); ++cur) {
      if ((*cur)->IsAllocSpace() && (*cur)->Contains(ref)) {
        bitmap_->Set(obj);
        break;
      }
    }
  }

 private:
  Heap* const heap_;
  SpaceBitmap* bitmap_;
};

class ModUnionVisitor {
 public:
  explicit ModUnionVisitor(Heap* const heap, SpaceBitmap* bitmap)
    : heap_(heap),
      bitmap_(bitmap) {
  }

  void operator ()(const Object* obj) const
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_,
                            Locks::mutator_lock_) {
    DCHECK(obj != NULL);
    // We don't have an early exit since we use the visitor pattern, an early exit should
    // significantly speed this up.
    MarkIfReachesAllocspaceVisitor visitor(heap_, bitmap_);
    MarkSweep::VisitObjectReferences(obj, visitor);
  }
 private:
  Heap* const heap_;
  SpaceBitmap* bitmap_;
};

class ModUnionClearCardSetVisitor {
 public:
  explicit ModUnionClearCardSetVisitor(std::set<byte*>* const cleared_cards)
    : cleared_cards_(cleared_cards) {
  }

  inline void operator ()(byte* card, byte expected_value, byte new_value) const {
    if (expected_value == CardTable::kCardDirty) {
      cleared_cards_->insert(card);
    }
  }

 private:
  std::set<byte*>* const cleared_cards_;
};

class ModUnionClearCardVisitor {
 public:
  explicit ModUnionClearCardVisitor(std::vector<byte*>* cleared_cards)
    : cleared_cards_(cleared_cards) {
  }

  void operator ()(byte* card, byte expected_card, byte new_card) const {
    if (expected_card == CardTable::kCardDirty) {
      cleared_cards_->push_back(card);
    }
  }
 private:
  std::vector<byte*>* cleared_cards_;
};

ModUnionTableBitmap::ModUnionTableBitmap(Heap* heap) : ModUnionTable(heap)  {
  // Prevent fragmentation of the heap which is caused by resizing of the vector.
  // TODO: Make a new vector which uses madvise (basically same as a mark stack).
  cleared_cards_.reserve(32);
  const Spaces& spaces = heap->GetSpaces();
  // Create one heap bitmap per image space.
  // TODO: C++0x auto
  for (Spaces::const_iterator it = spaces.begin(); it != spaces.end(); ++it) {
    ContinuousSpace* space = *it;
    if (space->IsImageSpace()) {
      // The mod-union table is only needed when we have an image space since it's purpose is to
      // cache image roots.
      UniquePtr<SpaceBitmap> bitmap(SpaceBitmap::Create("mod-union table bitmap", space->Begin(),
                                                        space->Size()));
      CHECK(bitmap.get() != NULL) << "Failed to create mod-union bitmap";
      bitmaps_.Put(space, bitmap.release());
    }
  }
}

ModUnionTableBitmap::~ModUnionTableBitmap() {
  STLDeleteValues(&bitmaps_);
}

void ModUnionTableBitmap::ClearCards(ContinuousSpace* space) {
  CardTable* card_table = heap_->GetCardTable();
  ModUnionClearCardVisitor visitor(&cleared_cards_);
  // Clear dirty cards in the this image space and update the corresponding mod-union bits.
  card_table->ModifyCardsAtomic(space->Begin(), space->End(), AgeCardVisitor(), visitor);
}

void ModUnionTableBitmap::Update() {
  CardTable* card_table = heap_->GetCardTable();
  while (!cleared_cards_.empty()) {
    byte* card = cleared_cards_.back();
    cleared_cards_.pop_back();

    uintptr_t start = reinterpret_cast<uintptr_t>(card_table->AddrFromCard(card));
    uintptr_t end = start + CardTable::kCardSize;
    ContinuousSpace* space = heap_->FindSpaceFromObject(reinterpret_cast<Object*>(start));
    SpaceBitmap* bitmap = space->GetLiveBitmap();

    // Clear the mod-union bitmap range corresponding to this card so that we don't have any
    // objects marked which do not reach the alloc space.
    bitmap->VisitRange(start, end, SpaceBitmap::ClearVisitor(bitmap));

    // At this point we need to update the mod-union bitmap to contain all the objects which reach
    // the alloc space.
    ModUnionVisitor visitor(heap_, bitmap);
    space->GetLiveBitmap()->VisitMarkedRange(start, end, visitor, VoidFunctor());
  }
}

class ModUnionScanImageRootVisitor {
 public:
  ModUnionScanImageRootVisitor(MarkSweep* const mark_sweep) : mark_sweep_(mark_sweep) {
  }

  void operator ()(const Object* root) const
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(root != NULL);
    mark_sweep_->ScanRoot(root);
  }

 private:
  MarkSweep* const mark_sweep_;
};

void ModUnionTableBitmap::MarkReferences(MarkSweep* mark_sweep) {
  // Some tests have no image space, and therefore no mod-union bitmap.
  ModUnionScanImageRootVisitor image_root_scanner(mark_sweep);
  for (BitmapMap::iterator it = bitmaps_.begin(); it != bitmaps_.end(); ++it) {
    const ContinuousSpace* space = it->first;
    uintptr_t begin = reinterpret_cast<uintptr_t>(space->Begin());
    uintptr_t end = reinterpret_cast<uintptr_t>(space->End());
    it->second->VisitMarkedRange(begin, end, image_root_scanner, VoidFunctor());
  }
}


ModUnionTableReferenceCache::ModUnionTableReferenceCache(Heap* heap) : ModUnionTable(heap) {

}

ModUnionTableReferenceCache::~ModUnionTableReferenceCache() {

}

void ModUnionTableReferenceCache::ClearCards(ContinuousSpace* space) {
  CardTable* card_table = GetHeap()->GetCardTable();
  ModUnionClearCardSetVisitor visitor(&cleared_cards_);
  // Clear dirty cards in the this space and update the corresponding mod-union bits.
  card_table->ModifyCardsAtomic(space->Begin(), space->End(), AgeCardVisitor(), visitor);
}

class AddToReferenceArrayVisitor {
 public:
  explicit AddToReferenceArrayVisitor(
      ModUnionTableReferenceCache* const mod_union_table,
        ModUnionTableReferenceCache::ReferenceArray* references)
    : mod_union_table_(mod_union_table),
      references_(references) {
  }

  // Extra parameters are required since we use this same visitor signature for checking objects.
  void operator ()(const Object* obj, const Object* ref, const MemberOffset& /* offset */,
                     bool /* is_static */) const {
    // Only add the reference if it is non null and fits our criteria.
    if (ref != NULL && mod_union_table_->AddReference(obj, ref)) {
      references_->push_back(ref);
    }
  }

 private:
  ModUnionTableReferenceCache* mod_union_table_;
  ModUnionTable::ReferenceArray* references_;
};

class ModUnionReferenceVisitor {
 public:
  explicit ModUnionReferenceVisitor(
        ModUnionTableReferenceCache* const mod_union_table,
        ModUnionTableReferenceCache::ReferenceArray* references)
    : mod_union_table_(mod_union_table),
      references_(references) {
  }

  void operator ()(const Object* obj) const
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_,
                            Locks::mutator_lock_) {
    DCHECK(obj != NULL);
    // We don't have an early exit since we use the visitor pattern, an early
    // exit should significantly speed this up.
    AddToReferenceArrayVisitor visitor(mod_union_table_, references_);
    MarkSweep::VisitObjectReferences(obj, visitor);
  }
 private:
  ModUnionTableReferenceCache* const mod_union_table_;
  ModUnionTable::ReferenceArray* references_;
};


class CheckReferenceVisitor {
 public:
  typedef std::set<const Object*> ReferenceSet;

  explicit CheckReferenceVisitor(
      ModUnionTableReferenceCache* const mod_union_table,
      const ReferenceSet& references)
    : mod_union_table_(mod_union_table),
      references_(references) {
  }

  // Extra parameters are required since we use this same visitor signature for checking objects.
  // TODO: Fixme when anotatalysis works with visitors.
  void operator ()(const Object* obj, const Object* ref, const MemberOffset& /* offset */,
                     bool /* is_static */) const
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
    Heap* heap = mod_union_table_->GetHeap();
    if (ref != NULL && mod_union_table_->AddReference(obj, ref) &&
        references_.find(ref) == references_.end()) {
      ContinuousSpace* from_space = heap->FindSpaceFromObject(obj);
      ContinuousSpace* to_space = heap->FindSpaceFromObject(ref);
      LOG(INFO) << "Object " << reinterpret_cast<const void*>(obj) << "(" << PrettyTypeOf(obj) << ")"
                << "References " << reinterpret_cast<const void*>(ref)
                << "(" << PrettyTypeOf(ref) << ") without being in mod-union table";
      LOG(INFO) << "FromSpace " << from_space->GetName() << " type " << from_space->GetGcRetentionPolicy();
      LOG(INFO) << "ToSpace " << to_space->GetName() << " type " << to_space->GetGcRetentionPolicy();
      mod_union_table_->GetHeap()->DumpSpaces();
      LOG(FATAL) << "FATAL ERROR";
    }
  }

 private:
  ModUnionTableReferenceCache* const mod_union_table_;
  const ReferenceSet& references_;
};

class ModUnionCheckReferences {
 public:
  typedef std::set<const Object*> ReferenceSet;

  explicit ModUnionCheckReferences (
      ModUnionTableReferenceCache* const mod_union_table,
      const ReferenceSet& references)
    : mod_union_table_(mod_union_table),
      references_(references) {
  }

  void operator ()(const Object* obj) const NO_THREAD_SAFETY_ANALYSIS {
    DCHECK(obj != NULL);
    if (kDebugLocking) {
      Locks::heap_bitmap_lock_->AssertSharedHeld(Thread::Current());
    }
    CheckReferenceVisitor visitor(mod_union_table_, references_);
    MarkSweep::VisitObjectReferences(obj, visitor);
  }

 private:
  ModUnionTableReferenceCache* const mod_union_table_;
  const ReferenceSet& references_;
};

void ModUnionTableReferenceCache::Verify() {
  // Start by checking that everything in the mod union table is marked.
  Heap* heap = GetHeap();
  for (ReferenceMap::const_iterator it = references_.begin(); it != references_.end(); ++it) {
    for (ReferenceArray::const_iterator it_ref = it->second.begin(); it_ref != it->second.end();
        ++it_ref ) {
      DCHECK(heap->GetLiveBitmap()->Test(*it_ref));
    }
  }

  // Check the references of each clean card which is also in the mod union table.
  for (ReferenceMap::const_iterator it = references_.begin(); it != references_.end(); ++it) {
    const byte* card = &*it->first;
    if (*card == CardTable::kCardClean) {
      std::set<const Object*> reference_set;
      for (ReferenceArray::const_iterator itr = it->second.begin(); itr != it->second.end();++itr) {
        reference_set.insert(*itr);
      }
      ModUnionCheckReferences visitor(this, reference_set);
      CardTable* card_table = heap->GetCardTable();
      uintptr_t start = reinterpret_cast<uintptr_t>(card_table->AddrFromCard(card));
      uintptr_t end = start + CardTable::kCardSize;
      SpaceBitmap* live_bitmap =
              heap->FindSpaceFromObject(reinterpret_cast<Object*>(start))->GetLiveBitmap();
      live_bitmap->VisitMarkedRange(start, end, visitor, VoidFunctor());
    }
  }
}

void ModUnionTableReferenceCache::Update() {
  Heap* heap = GetHeap();
  CardTable* card_table = heap->GetCardTable();

  ReferenceArray cards_references;
  ModUnionReferenceVisitor visitor(this, &cards_references);

  for (ClearedCards::iterator it = cleared_cards_.begin(); it != cleared_cards_.end(); ++it) {
    byte* card = *it;
    // Clear and re-compute alloc space references associated with this card.
    cards_references.clear();
    uintptr_t start = reinterpret_cast<uintptr_t>(card_table->AddrFromCard(card));
    uintptr_t end = start + CardTable::kCardSize;
    SpaceBitmap* live_bitmap =
        heap->FindSpaceFromObject(reinterpret_cast<Object*>(start))->GetLiveBitmap();
    live_bitmap->VisitMarkedRange(start, end, visitor, VoidFunctor());

    // Update the corresponding references for the card.
    // TODO: C++0x auto
    ReferenceMap::iterator found = references_.find(card);
    if (found == references_.end()) {
      if (cards_references.empty()) {
        // No reason to add empty array.
        continue;
      }
      references_.Put(card, cards_references);
    } else {
      found->second = cards_references;
    }
  }
  cleared_cards_.clear();
}

void ModUnionTableReferenceCache::MarkReferences(MarkSweep* mark_sweep) {
  // TODO: C++0x auto
  size_t count = 0;
  for (ReferenceMap::const_iterator it = references_.begin(); it != references_.end(); ++it) {
    for (ReferenceArray::const_iterator it_ref = it->second.begin(); it_ref != it->second.end(); ++it_ref ) {
      mark_sweep->MarkRoot(*it_ref);
      ++count;
    }
  }
  if (VLOG_IS_ON(heap)) {
    VLOG(gc) << "Marked " << count << " references in mod union table";
  }
}

ModUnionTableCardCache::ModUnionTableCardCache(Heap* heap) : ModUnionTable(heap) {

}

ModUnionTableCardCache::~ModUnionTableCardCache() {

}

void ModUnionTableCardCache::ClearCards(ContinuousSpace* space) {
  CardTable* card_table = GetHeap()->GetCardTable();
  ModUnionClearCardSetVisitor visitor(&cleared_cards_);
  // Clear dirty cards in the this space and update the corresponding mod-union bits.
  card_table->ModifyCardsAtomic(space->Begin(), space->End(), AgeCardVisitor(), visitor);
}

// Mark all references to the alloc space(s).
void ModUnionTableCardCache::MarkReferences(MarkSweep* mark_sweep) {
  CardTable* card_table = heap_->GetCardTable();
  ModUnionScanImageRootVisitor visitor(mark_sweep);
  for (ClearedCards::const_iterator it = cleared_cards_.begin(); it != cleared_cards_.end(); ++it) {
    byte* card = *it;
    uintptr_t start = reinterpret_cast<uintptr_t>(card_table->AddrFromCard(card));
    uintptr_t end = start + CardTable::kCardSize;
    SpaceBitmap* live_bitmap =
        heap_->FindSpaceFromObject(reinterpret_cast<Object*>(start))->GetLiveBitmap();
    live_bitmap->VisitMarkedRange(start, end, visitor, VoidFunctor());
  }
}

}  // namespace art
