/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "remembered_set.h"

#include <memory>

#include "base/stl_util.h"
#include "card_table-inl.h"
#include "heap_bitmap.h"
#include "gc/collector/mark_sweep.h"
#include "gc/collector/mark_sweep-inl.h"
#include "gc/collector/semi_space.h"
#include "gc/heap.h"
#include "gc/space/space.h"
#include "mirror/art_field-inl.h"
#include "mirror/object-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object_array-inl.h"
#include "space_bitmap-inl.h"
#include "thread.h"

namespace art {
namespace gc {
namespace accounting {

class RememberedSetCardVisitor {
 public:
  explicit RememberedSetCardVisitor(RememberedSet::CardSet* const dirty_cards)
      : dirty_cards_(dirty_cards) {}

  void operator()(byte* card, byte expected_value, byte new_value) const {
    if (expected_value == CardTable::kCardDirty) {
      dirty_cards_->insert(card);
    }
  }

 private:
  RememberedSet::CardSet* const dirty_cards_;
};

void RememberedSet::ClearCards() {
  CardTable* card_table = GetHeap()->GetCardTable();
  RememberedSetCardVisitor card_visitor(&dirty_cards_);
  // Clear dirty cards in the space and insert them into the dirty card set.
  card_table->ModifyCardsAtomic(space_->Begin(), space_->End(), AgeCardVisitor(), card_visitor);
}

class RememberedSetReferenceVisitor {
 public:
  RememberedSetReferenceVisitor(MarkHeapReferenceCallback* callback,
                                DelayReferenceReferentCallback* ref_callback,
                                space::ContinuousSpace* target_space,
                                bool* const contains_reference_to_target_space, void* arg)
      : callback_(callback), ref_callback_(ref_callback), target_space_(target_space), arg_(arg),
        contains_reference_to_target_space_(contains_reference_to_target_space) {}

  void operator()(mirror::Object* obj, MemberOffset offset, bool /* is_static */) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(obj != nullptr);
    mirror::HeapReference<mirror::Object>* ref_ptr = obj->GetFieldObjectReferenceAddr(offset);
    if (target_space_->HasAddress(ref_ptr->AsMirrorPtr())) {
      *contains_reference_to_target_space_ = true;
      callback_(ref_ptr, arg_);
      DCHECK(!target_space_->HasAddress(ref_ptr->AsMirrorPtr()));
    }
  }

  void operator()(mirror::Class* klass, mirror::Reference* ref) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    if (target_space_->HasAddress(ref->GetReferent())) {
      *contains_reference_to_target_space_ = true;
      ref_callback_(klass, ref, arg_);
    }
  }

 private:
  MarkHeapReferenceCallback* const callback_;
  DelayReferenceReferentCallback* const ref_callback_;
  space::ContinuousSpace* const target_space_;
  void* const arg_;
  bool* const contains_reference_to_target_space_;
};

class RememberedSetObjectVisitor {
 public:
  RememberedSetObjectVisitor(MarkHeapReferenceCallback* callback,
                             DelayReferenceReferentCallback* ref_callback,
                             space::ContinuousSpace* target_space,
                             bool* const contains_reference_to_target_space, void* arg)
      : callback_(callback), ref_callback_(ref_callback), target_space_(target_space), arg_(arg),
        contains_reference_to_target_space_(contains_reference_to_target_space) {}

  void operator()(mirror::Object* obj) const EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    RememberedSetReferenceVisitor visitor(callback_, ref_callback_, target_space_,
                                          contains_reference_to_target_space_, arg_);
    obj->VisitReferences<kMovingClasses>(visitor, visitor);
  }

 private:
  MarkHeapReferenceCallback* const callback_;
  DelayReferenceReferentCallback* const ref_callback_;
  space::ContinuousSpace* const target_space_;
  void* const arg_;
  bool* const contains_reference_to_target_space_;
};

void RememberedSet::UpdateAndMarkReferences(MarkHeapReferenceCallback* callback,
                                            DelayReferenceReferentCallback* ref_callback,
                                            space::ContinuousSpace* target_space, void* arg) {
  CardTable* card_table = heap_->GetCardTable();
  bool contains_reference_to_target_space = false;
  RememberedSetObjectVisitor obj_visitor(callback, ref_callback, target_space,
                                         &contains_reference_to_target_space, arg);
  ContinuousSpaceBitmap* bitmap = space_->GetLiveBitmap();
  CardSet remove_card_set;
  for (byte* const card_addr : dirty_cards_) {
    contains_reference_to_target_space = false;
    uintptr_t start = reinterpret_cast<uintptr_t>(card_table->AddrFromCard(card_addr));
    DCHECK(space_->HasAddress(reinterpret_cast<mirror::Object*>(start)));
    bitmap->VisitMarkedRange(start, start + CardTable::kCardSize, obj_visitor);
    if (!contains_reference_to_target_space) {
      // It was in the dirty card set, but it didn't actually contain
      // a reference to the target space. So, remove it from the dirty
      // card set so we won't have to scan it again (unless it gets
      // dirty again.)
      remove_card_set.insert(card_addr);
    }
  }

  // Remove the cards that didn't contain a reference to the target
  // space from the dirty card set.
  for (byte* const card_addr : remove_card_set) {
    DCHECK(dirty_cards_.find(card_addr) != dirty_cards_.end());
    dirty_cards_.erase(card_addr);
  }
}

void RememberedSet::Dump(std::ostream& os) {
  CardTable* card_table = heap_->GetCardTable();
  os << "RememberedSet dirty cards: [";
  for (const byte* card_addr : dirty_cards_) {
    auto start = reinterpret_cast<uintptr_t>(card_table->AddrFromCard(card_addr));
    auto end = start + CardTable::kCardSize;
    os << reinterpret_cast<void*>(start) << "-" << reinterpret_cast<void*>(end) << "\n";
  }
  os << "]";
}

void RememberedSet::AssertAllDirtyCardsAreWithinSpace() const {
  CardTable* card_table = heap_->GetCardTable();
  for (const byte* card_addr : dirty_cards_) {
    auto start = reinterpret_cast<byte*>(card_table->AddrFromCard(card_addr));
    auto end = start + CardTable::kCardSize;
    DCHECK_LE(space_->Begin(), start);
    DCHECK_LE(end, space_->Limit());
  }
}

}  // namespace accounting
}  // namespace gc
}  // namespace art
