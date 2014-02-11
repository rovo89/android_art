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

#include "reference_queue.h"

#include "accounting/card_table-inl.h"
#include "heap.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"

namespace art {
namespace gc {

ReferenceQueue::ReferenceQueue(Heap* heap)
    : lock_("reference queue lock"),
      heap_(heap),
      list_(nullptr) {
}

void ReferenceQueue::AtomicEnqueueIfNotEnqueued(Thread* self, mirror::Object* ref) {
  DCHECK(ref != NULL);
  MutexLock mu(self, lock_);
  if (!heap_->IsEnqueued(ref)) {
    EnqueuePendingReference(ref);
  }
}

void ReferenceQueue::EnqueueReference(mirror::Object* ref) {
  CHECK(heap_->IsEnqueuable(ref));
  EnqueuePendingReference(ref);
}

void ReferenceQueue::EnqueuePendingReference(mirror::Object* ref) {
  DCHECK(ref != NULL);
  MemberOffset pending_next_offset = heap_->GetReferencePendingNextOffset();
  DCHECK_NE(pending_next_offset.Uint32Value(), 0U);
  if (IsEmpty()) {
    // 1 element cyclic queue, ie: Reference ref = ..; ref.pendingNext = ref;
    ref->SetFieldObject(pending_next_offset, ref, false);
    list_ = ref;
  } else {
    mirror::Object* head = list_->GetFieldObject<mirror::Object>(pending_next_offset, false);
    ref->SetFieldObject(pending_next_offset, head, false);
    list_->SetFieldObject(pending_next_offset, ref, false);
  }
}

mirror::Object* ReferenceQueue::DequeuePendingReference() {
  DCHECK(!IsEmpty());
  MemberOffset pending_next_offset = heap_->GetReferencePendingNextOffset();
  mirror::Object* head = list_->GetFieldObject<mirror::Object>(pending_next_offset, false);
  DCHECK(head != nullptr);
  mirror::Object* ref;
  // Note: the following code is thread-safe because it is only called from ProcessReferences which
  // is single threaded.
  if (list_ == head) {
    ref = list_;
    list_ = nullptr;
  } else {
    mirror::Object* next = head->GetFieldObject<mirror::Object>(pending_next_offset, false);
    list_->SetFieldObject(pending_next_offset, next, false);
    ref = head;
  }
  ref->SetFieldObject(pending_next_offset, nullptr, false);
  return ref;
}

void ReferenceQueue::Dump(std::ostream& os) const {
  mirror::Object* cur = list_;
  os << "Reference starting at list_=" << list_ << "\n";
  while (cur != nullptr) {
    mirror::Object* pending_next =
        cur->GetFieldObject<mirror::Object>(heap_->GetReferencePendingNextOffset(), false);
    os << "PendingNext=" << pending_next;
    if (cur->GetClass()->IsFinalizerReferenceClass()) {
      os << " Zombie=" <<
          cur->GetFieldObject<mirror::Object>(heap_->GetFinalizerReferenceZombieOffset(), false);
    }
    os << "\n";
    cur = pending_next;
  }
}

void ReferenceQueue::ClearWhiteReferences(ReferenceQueue& cleared_references,
                                          IsMarkedCallback* preserve_callback,
                                          void* arg) {
  while (!IsEmpty()) {
    mirror::Object* ref = DequeuePendingReference();
    mirror::Object* referent = heap_->GetReferenceReferent(ref);
    if (referent != nullptr) {
      mirror::Object* forward_address = preserve_callback(referent, arg);
      if (forward_address == nullptr) {
        // Referent is white, clear it.
        heap_->ClearReferenceReferent(ref);
        if (heap_->IsEnqueuable(ref)) {
          cleared_references.EnqueuePendingReference(ref);
        }
      } else if (referent != forward_address) {
        // Object moved, need to updated the referent.
        heap_->SetReferenceReferent(ref, forward_address);
      }
    }
  }
}

void ReferenceQueue::EnqueueFinalizerReferences(ReferenceQueue& cleared_references,
                                                IsMarkedCallback is_marked_callback,
                                                MarkObjectCallback recursive_mark_callback,
                                                void* arg) {
  while (!IsEmpty()) {
    mirror::Object* ref = DequeuePendingReference();
    mirror::Object* referent = heap_->GetReferenceReferent(ref);
    if (referent != nullptr) {
      mirror::Object* forward_address = is_marked_callback(referent, arg);
      // If the referent isn't marked, mark it and update the
      if (forward_address == nullptr) {
        forward_address = recursive_mark_callback(referent, arg);
        // If the referent is non-null the reference must queuable.
        DCHECK(heap_->IsEnqueuable(ref));
        // Move the updated referent to the zombie field.
        ref->SetFieldObject(heap_->GetFinalizerReferenceZombieOffset(), forward_address, false);
        heap_->ClearReferenceReferent(ref);
        cleared_references.EnqueueReference(ref);
      } else if (referent != forward_address) {
        heap_->SetReferenceReferent(ref, forward_address);
      }
    }
  }
}

void ReferenceQueue::PreserveSomeSoftReferences(IsMarkedCallback preserve_callback, void* arg) {
  ReferenceQueue cleared(heap_);
  while (!IsEmpty()) {
    mirror::Object* ref = DequeuePendingReference();
    mirror::Object* referent = heap_->GetReferenceReferent(ref);
    if (referent != nullptr) {
      mirror::Object* forward_address = preserve_callback(referent, arg);
      if (forward_address == nullptr) {
        // Either the reference isn't marked or we don't wish to preserve it.
        cleared.EnqueuePendingReference(ref);
      } else if (forward_address != referent) {
        heap_->SetReferenceReferent(ref, forward_address);
      }
    }
  }
  list_ = cleared.GetList();
}

}  // namespace gc
}  // namespace art

