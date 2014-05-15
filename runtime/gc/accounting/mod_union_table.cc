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
#include "heap_bitmap.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/collector/mark_sweep.h"
#include "gc/collector/mark_sweep-inl.h"
#include "gc/heap.h"
#include "gc/space/space.h"
#include "mirror/art_field-inl.h"
#include "mirror/object-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object_array-inl.h"
#include "space_bitmap-inl.h"
#include "thread.h"
#include "UniquePtrCompat.h"

using ::art::mirror::Object;

namespace art {
namespace gc {
namespace accounting {

class ModUnionClearCardSetVisitor {
 public:
  explicit ModUnionClearCardSetVisitor(ModUnionTable::CardSet* const cleared_cards)
    : cleared_cards_(cleared_cards) {
  }

  inline void operator()(byte* card, byte expected_value, byte new_value) const {
    if (expected_value == CardTable::kCardDirty) {
      cleared_cards_->insert(card);
    }
  }

 private:
  ModUnionTable::CardSet* const cleared_cards_;
};

class ModUnionClearCardVisitor {
 public:
  explicit ModUnionClearCardVisitor(std::vector<byte*>* cleared_cards)
    : cleared_cards_(cleared_cards) {
  }

  void operator()(byte* card, byte expected_card, byte new_card) const {
    if (expected_card == CardTable::kCardDirty) {
      cleared_cards_->push_back(card);
    }
  }
 private:
  std::vector<byte*>* const cleared_cards_;
};

class ModUnionUpdateObjectReferencesVisitor {
 public:
  ModUnionUpdateObjectReferencesVisitor(MarkHeapReferenceCallback* callback, void* arg)
    : callback_(callback),
      arg_(arg) {
  }

  // Extra parameters are required since we use this same visitor signature for checking objects.
  void operator()(Object* obj, MemberOffset offset, bool /*is_static*/) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // Only add the reference if it is non null and fits our criteria.
    mirror::HeapReference<Object>* obj_ptr = obj->GetFieldObjectReferenceAddr(offset);
    if (obj_ptr->AsMirrorPtr() != nullptr) {
      callback_(obj_ptr, arg_);
    }
  }

 private:
  MarkHeapReferenceCallback* const callback_;
  void* arg_;
};

class ModUnionScanImageRootVisitor {
 public:
  ModUnionScanImageRootVisitor(MarkHeapReferenceCallback* callback, void* arg)
      : callback_(callback), arg_(arg) {}

  void operator()(Object* root) const
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(root != NULL);
    ModUnionUpdateObjectReferencesVisitor ref_visitor(callback_, arg_);
    root->VisitReferences<kMovingClasses>(ref_visitor, VoidFunctor());
  }

 private:
  MarkHeapReferenceCallback* const callback_;
  void* const arg_;
};

void ModUnionTableReferenceCache::ClearCards() {
  CardTable* card_table = GetHeap()->GetCardTable();
  ModUnionClearCardSetVisitor visitor(&cleared_cards_);
  // Clear dirty cards in the this space and update the corresponding mod-union bits.
  card_table->ModifyCardsAtomic(space_->Begin(), space_->End(), AgeCardVisitor(), visitor);
}

class AddToReferenceArrayVisitor {
 public:
  explicit AddToReferenceArrayVisitor(ModUnionTableReferenceCache* mod_union_table,
                                      std::vector<mirror::HeapReference<Object>*>* references)
    : mod_union_table_(mod_union_table), references_(references) {
  }

  // Extra parameters are required since we use this same visitor signature for checking objects.
  void operator()(Object* obj, MemberOffset offset, bool /*is_static*/) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::HeapReference<Object>* ref_ptr = obj->GetFieldObjectReferenceAddr(offset);
    mirror::Object* ref = ref_ptr->AsMirrorPtr();
    // Only add the reference if it is non null and fits our criteria.
    if (ref != nullptr && mod_union_table_->ShouldAddReference(ref)) {
      // Push the adddress of the reference.
      references_->push_back(ref_ptr);
    }
  }

 private:
  ModUnionTableReferenceCache* const mod_union_table_;
  std::vector<mirror::HeapReference<Object>*>* const references_;
};

class ModUnionReferenceVisitor {
 public:
  explicit ModUnionReferenceVisitor(ModUnionTableReferenceCache* const mod_union_table,
                                    std::vector<mirror::HeapReference<Object>*>* references)
    : mod_union_table_(mod_union_table),
      references_(references) {
  }

  void operator()(Object* obj) const
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
    // We don't have an early exit since we use the visitor pattern, an early
    // exit should significantly speed this up.
    AddToReferenceArrayVisitor visitor(mod_union_table_, references_);
    obj->VisitReferences<kMovingClasses>(visitor, VoidFunctor());
  }
 private:
  ModUnionTableReferenceCache* const mod_union_table_;
  std::vector<mirror::HeapReference<Object>*>* const references_;
};

class CheckReferenceVisitor {
 public:
  explicit CheckReferenceVisitor(ModUnionTableReferenceCache* mod_union_table,
                                 const std::set<const Object*>& references)
    : mod_union_table_(mod_union_table),
      references_(references) {
  }

  // Extra parameters are required since we use this same visitor signature for checking objects.
  void operator()(Object* obj, MemberOffset offset, bool /*is_static*/) const
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
    mirror::Object* ref = obj->GetFieldObject<mirror::Object>(offset);
    if (ref != nullptr && mod_union_table_->ShouldAddReference(ref) &&
        references_.find(ref) == references_.end()) {
      Heap* heap = mod_union_table_->GetHeap();
      space::ContinuousSpace* from_space = heap->FindContinuousSpaceFromObject(obj, false);
      space::ContinuousSpace* to_space = heap->FindContinuousSpaceFromObject(ref, false);
      LOG(INFO) << "Object " << reinterpret_cast<const void*>(obj) << "(" << PrettyTypeOf(obj)
          << ")" << "References " << reinterpret_cast<const void*>(ref) << "(" << PrettyTypeOf(ref)
          << ") without being in mod-union table";
      LOG(INFO) << "FromSpace " << from_space->GetName() << " type "
          << from_space->GetGcRetentionPolicy();
      LOG(INFO) << "ToSpace " << to_space->GetName() << " type "
          << to_space->GetGcRetentionPolicy();
      heap->DumpSpaces();
      LOG(FATAL) << "FATAL ERROR";
    }
  }

 private:
  ModUnionTableReferenceCache* const mod_union_table_;
  const std::set<const Object*>& references_;
};

class ModUnionCheckReferences {
 public:
  explicit ModUnionCheckReferences(ModUnionTableReferenceCache* mod_union_table,
                                   const std::set<const Object*>& references)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      : mod_union_table_(mod_union_table), references_(references) {
  }

  void operator()(Object* obj) const NO_THREAD_SAFETY_ANALYSIS {
    Locks::heap_bitmap_lock_->AssertSharedHeld(Thread::Current());
    CheckReferenceVisitor visitor(mod_union_table_, references_);
    obj->VisitReferences<kMovingClasses>(visitor, VoidFunctor());
  }

 private:
  ModUnionTableReferenceCache* const mod_union_table_;
  const std::set<const Object*>& references_;
};

void ModUnionTableReferenceCache::Verify() {
  // Start by checking that everything in the mod union table is marked.
  for (const auto& ref_pair : references_) {
    for (mirror::HeapReference<Object>* ref : ref_pair.second) {
      CHECK(heap_->IsLiveObjectLocked(ref->AsMirrorPtr()));
    }
  }

  // Check the references of each clean card which is also in the mod union table.
  CardTable* card_table = heap_->GetCardTable();
  ContinuousSpaceBitmap* live_bitmap = space_->GetLiveBitmap();
  for (const auto& ref_pair : references_) {
    const byte* card = ref_pair.first;
    if (*card == CardTable::kCardClean) {
      std::set<const Object*> reference_set;
      for (mirror::HeapReference<Object>* obj_ptr : ref_pair.second) {
        reference_set.insert(obj_ptr->AsMirrorPtr());
      }
      ModUnionCheckReferences visitor(this, reference_set);
      uintptr_t start = reinterpret_cast<uintptr_t>(card_table->AddrFromCard(card));
      live_bitmap->VisitMarkedRange(start, start + CardTable::kCardSize, visitor);
    }
  }
}

void ModUnionTableReferenceCache::Dump(std::ostream& os) {
  CardTable* card_table = heap_->GetCardTable();
  os << "ModUnionTable cleared cards: [";
  for (byte* card_addr : cleared_cards_) {
    uintptr_t start = reinterpret_cast<uintptr_t>(card_table->AddrFromCard(card_addr));
    uintptr_t end = start + CardTable::kCardSize;
    os << reinterpret_cast<void*>(start) << "-" << reinterpret_cast<void*>(end) << ",";
  }
  os << "]\nModUnionTable references: [";
  for (const auto& ref_pair : references_) {
    const byte* card_addr = ref_pair.first;
    uintptr_t start = reinterpret_cast<uintptr_t>(card_table->AddrFromCard(card_addr));
    uintptr_t end = start + CardTable::kCardSize;
    os << reinterpret_cast<void*>(start) << "-" << reinterpret_cast<void*>(end) << "->{";
    for (mirror::HeapReference<Object>* ref : ref_pair.second) {
      os << reinterpret_cast<const void*>(ref->AsMirrorPtr()) << ",";
    }
    os << "},";
  }
}

void ModUnionTableReferenceCache::UpdateAndMarkReferences(MarkHeapReferenceCallback* callback,
                                                          void* arg) {
  CardTable* card_table = heap_->GetCardTable();

  std::vector<mirror::HeapReference<Object>*> cards_references;
  ModUnionReferenceVisitor add_visitor(this, &cards_references);

  for (const auto& card : cleared_cards_) {
    // Clear and re-compute alloc space references associated with this card.
    cards_references.clear();
    uintptr_t start = reinterpret_cast<uintptr_t>(card_table->AddrFromCard(card));
    uintptr_t end = start + CardTable::kCardSize;
    auto* space = heap_->FindContinuousSpaceFromObject(reinterpret_cast<Object*>(start), false);
    DCHECK(space != nullptr);
    ContinuousSpaceBitmap* live_bitmap = space->GetLiveBitmap();
    live_bitmap->VisitMarkedRange(start, end, add_visitor);

    // Update the corresponding references for the card.
    auto found = references_.find(card);
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
  size_t count = 0;
  for (const auto& ref : references_) {
    for (mirror::HeapReference<Object>* obj_ptr : ref.second) {
      callback(obj_ptr, arg);
    }
    count += ref.second.size();
  }
  if (VLOG_IS_ON(heap)) {
    VLOG(gc) << "Marked " << count << " references in mod union table";
  }
}

void ModUnionTableCardCache::ClearCards() {
  CardTable* card_table = GetHeap()->GetCardTable();
  ModUnionClearCardSetVisitor visitor(&cleared_cards_);
  // Clear dirty cards in the this space and update the corresponding mod-union bits.
  card_table->ModifyCardsAtomic(space_->Begin(), space_->End(), AgeCardVisitor(), visitor);
}

// Mark all references to the alloc space(s).
void ModUnionTableCardCache::UpdateAndMarkReferences(MarkHeapReferenceCallback* callback,
                                                     void* arg) {
  CardTable* card_table = heap_->GetCardTable();
  ModUnionScanImageRootVisitor scan_visitor(callback, arg);
  ContinuousSpaceBitmap* bitmap = space_->GetLiveBitmap();
  for (const byte* card_addr : cleared_cards_) {
    uintptr_t start = reinterpret_cast<uintptr_t>(card_table->AddrFromCard(card_addr));
    DCHECK(space_->HasAddress(reinterpret_cast<Object*>(start)));
    bitmap->VisitMarkedRange(start, start + CardTable::kCardSize, scan_visitor);
  }
}

void ModUnionTableCardCache::Dump(std::ostream& os) {
  CardTable* card_table = heap_->GetCardTable();
  os << "ModUnionTable dirty cards: [";
  for (const byte* card_addr : cleared_cards_) {
    auto start = reinterpret_cast<uintptr_t>(card_table->AddrFromCard(card_addr));
    auto end = start + CardTable::kCardSize;
    os << reinterpret_cast<void*>(start) << "-" << reinterpret_cast<void*>(end) << "\n";
  }
  os << "]";
}

}  // namespace accounting
}  // namespace gc
}  // namespace art
