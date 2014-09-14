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
#include "mirror/reference-inl.h"

namespace art {
namespace gc {

ReferenceQueue::ReferenceQueue(Mutex* lock) : lock_(lock), list_(nullptr) {
}

void ReferenceQueue::AtomicEnqueueIfNotEnqueued(Thread* self, mirror::Reference* ref) {
  DCHECK(ref != NULL);
  MutexLock mu(self, *lock_);
  if (!ref->IsEnqueued()) {
    EnqueuePendingReference(ref);
  }
}

void ReferenceQueue::EnqueueReference(mirror::Reference* ref) {
  CHECK(ref->IsEnqueuable());
  EnqueuePendingReference(ref);
}

void ReferenceQueue::EnqueuePendingReference(mirror::Reference* ref) {
  DCHECK(ref != NULL);
  if (IsEmpty()) {
    // 1 element cyclic queue, ie: Reference ref = ..; ref.pendingNext = ref;
    list_ = ref;
  } else {
    mirror::Reference* head = list_->GetPendingNext();
    if (Runtime::Current()->IsActiveTransaction()) {
      ref->SetPendingNext<true>(head);
    } else {
      ref->SetPendingNext<false>(head);
    }
  }
  if (Runtime::Current()->IsActiveTransaction()) {
    list_->SetPendingNext<true>(ref);
  } else {
    list_->SetPendingNext<false>(ref);
  }
}

mirror::Reference* ReferenceQueue::DequeuePendingReference() {
  DCHECK(!IsEmpty());
  mirror::Reference* head = list_->GetPendingNext();
  DCHECK(head != nullptr);
  mirror::Reference* ref;
  // Note: the following code is thread-safe because it is only called from ProcessReferences which
  // is single threaded.
  if (list_ == head) {
    ref = list_;
    list_ = nullptr;
  } else {
    mirror::Reference* next = head->GetPendingNext();
    if (Runtime::Current()->IsActiveTransaction()) {
      list_->SetPendingNext<true>(next);
    } else {
      list_->SetPendingNext<false>(next);
    }
    ref = head;
  }
  if (Runtime::Current()->IsActiveTransaction()) {
    ref->SetPendingNext<true>(nullptr);
  } else {
    ref->SetPendingNext<false>(nullptr);
  }
  return ref;
}

void ReferenceQueue::Dump(std::ostream& os) const {
  mirror::Reference* cur = list_;
  os << "Reference starting at list_=" << list_ << "\n";
  while (cur != nullptr) {
    mirror::Reference* pending_next = cur->GetPendingNext();
    os << "PendingNext=" << pending_next;
    if (cur->IsFinalizerReferenceInstance()) {
      os << " Zombie=" << cur->AsFinalizerReference()->GetZombie();
    }
    os << "\n";
    cur = pending_next;
  }
}

void ReferenceQueue::ClearWhiteReferences(ReferenceQueue* cleared_references,
                                          IsHeapReferenceMarkedCallback* preserve_callback,
                                          void* arg) {
  while (!IsEmpty()) {
    mirror::Reference* ref = DequeuePendingReference();
    mirror::HeapReference<mirror::Object>* referent_addr = ref->GetReferentReferenceAddr();
    if (referent_addr->AsMirrorPtr() != nullptr && !preserve_callback(referent_addr, arg)) {
      // Referent is white, clear it.
      if (Runtime::Current()->IsActiveTransaction()) {
        ref->ClearReferent<true>();
      } else {
        ref->ClearReferent<false>();
      }
      if (ref->IsEnqueuable()) {
        cleared_references->EnqueuePendingReference(ref);
      }
    }
  }
}

void ReferenceQueue::EnqueueFinalizerReferences(ReferenceQueue* cleared_references,
                                                IsHeapReferenceMarkedCallback* is_marked_callback,
                                                MarkObjectCallback* mark_object_callback,
                                                void* arg) {
  while (!IsEmpty()) {
    mirror::FinalizerReference* ref = DequeuePendingReference()->AsFinalizerReference();
    mirror::HeapReference<mirror::Object>* referent_addr = ref->GetReferentReferenceAddr();
    if (referent_addr->AsMirrorPtr() != nullptr && !is_marked_callback(referent_addr, arg)) {
      mirror::Object* forward_address = mark_object_callback(referent_addr->AsMirrorPtr(), arg);
      // If the referent is non-null the reference must queuable.
      DCHECK(ref->IsEnqueuable());
      // Move the updated referent to the zombie field.
      if (Runtime::Current()->IsActiveTransaction()) {
        ref->SetZombie<true>(forward_address);
        ref->ClearReferent<true>();
      } else {
        ref->SetZombie<false>(forward_address);
        ref->ClearReferent<false>();
      }
      cleared_references->EnqueueReference(ref);
    }
  }
}

void ReferenceQueue::ForwardSoftReferences(IsHeapReferenceMarkedCallback* preserve_callback,
                                           void* arg) {
  if (UNLIKELY(IsEmpty())) {
    return;
  }
  mirror::Reference* const head = list_;
  mirror::Reference* ref = head;
  do {
    mirror::HeapReference<mirror::Object>* referent_addr = ref->GetReferentReferenceAddr();
    if (referent_addr->AsMirrorPtr() != nullptr) {
      UNUSED(preserve_callback(referent_addr, arg));
    }
    ref = ref->GetPendingNext();
  } while (LIKELY(ref != head));
}

void ReferenceQueue::UpdateRoots(IsMarkedCallback* callback, void* arg) {
  if (list_ != nullptr) {
    list_ = down_cast<mirror::Reference*>(callback(list_, arg));
  }
}

}  // namespace gc
}  // namespace art
