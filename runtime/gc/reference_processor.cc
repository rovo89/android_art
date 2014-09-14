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

#include "reference_processor.h"

#include "mirror/object-inl.h"
#include "mirror/reference.h"
#include "mirror/reference-inl.h"
#include "reference_processor-inl.h"
#include "reflection.h"
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change.h"
#include "well_known_classes.h"

namespace art {
namespace gc {

ReferenceProcessor::ReferenceProcessor()
    : process_references_args_(nullptr, nullptr, nullptr),
      preserving_references_(false),
      condition_("reference processor condition", *Locks::reference_processor_lock_) ,
      soft_reference_queue_(Locks::reference_queue_soft_references_lock_),
      weak_reference_queue_(Locks::reference_queue_weak_references_lock_),
      finalizer_reference_queue_(Locks::reference_queue_finalizer_references_lock_),
      phantom_reference_queue_(Locks::reference_queue_phantom_references_lock_),
      cleared_references_(Locks::reference_queue_cleared_references_lock_) {
}

void ReferenceProcessor::EnableSlowPath() {
  mirror::Reference::GetJavaLangRefReference()->SetSlowPath(true);
}

void ReferenceProcessor::DisableSlowPath(Thread* self) {
  mirror::Reference::GetJavaLangRefReference()->SetSlowPath(false);
  condition_.Broadcast(self);
}

mirror::Object* ReferenceProcessor::GetReferent(Thread* self, mirror::Reference* reference) {
  mirror::Object* const referent = reference->GetReferent();
  // If the referent is null then it is already cleared, we can just return null since there is no
  // scenario where it becomes non-null during the reference processing phase.
  if (UNLIKELY(!SlowPathEnabled()) || referent == nullptr) {
    return referent;
  }
  MutexLock mu(self, *Locks::reference_processor_lock_);
  while (SlowPathEnabled()) {
    mirror::HeapReference<mirror::Object>* const referent_addr =
        reference->GetReferentReferenceAddr();
    // If the referent became cleared, return it. Don't need barrier since thread roots can't get
    // updated until after we leave the function due to holding the mutator lock.
    if (referent_addr->AsMirrorPtr() == nullptr) {
      return nullptr;
    }
    // Try to see if the referent is already marked by using the is_marked_callback. We can return
    // it to the mutator as long as the GC is not preserving references.
    IsHeapReferenceMarkedCallback* const is_marked_callback =
        process_references_args_.is_marked_callback_;
    if (LIKELY(is_marked_callback != nullptr)) {
      // If it's null it means not marked, but it could become marked if the referent is reachable
      // by finalizer referents. So we can not return in this case and must block. Otherwise, we
      // can return it to the mutator as long as the GC is not preserving references, in which
      // case only black nodes can be safely returned. If the GC is preserving references, the
      // mutator could take a white field from a grey or white node and move it somewhere else
      // in the heap causing corruption since this field would get swept.
      if (is_marked_callback(referent_addr, process_references_args_.arg_)) {
        if (!preserving_references_ ||
           (LIKELY(!reference->IsFinalizerReferenceInstance()) && !reference->IsEnqueued())) {
          return referent_addr->AsMirrorPtr();
        }
      }
    }
    condition_.WaitHoldingLocks(self);
  }
  return reference->GetReferent();
}

bool ReferenceProcessor::PreserveSoftReferenceCallback(mirror::HeapReference<mirror::Object>* obj,
                                                       void* arg) {
  auto* const args = reinterpret_cast<ProcessReferencesArgs*>(arg);
  // TODO: Add smarter logic for preserving soft references.
  mirror::Object* new_obj = args->mark_callback_(obj->AsMirrorPtr(), args->arg_);
  DCHECK(new_obj != nullptr);
  obj->Assign(new_obj);
  return true;
}

void ReferenceProcessor::StartPreservingReferences(Thread* self) {
  MutexLock mu(self, *Locks::reference_processor_lock_);
  preserving_references_ = true;
}

void ReferenceProcessor::StopPreservingReferences(Thread* self) {
  MutexLock mu(self, *Locks::reference_processor_lock_);
  preserving_references_ = false;
  // We are done preserving references, some people who are blocked may see a marked referent.
  condition_.Broadcast(self);
}

// Process reference class instances and schedule finalizations.
void ReferenceProcessor::ProcessReferences(bool concurrent, TimingLogger* timings,
                                           bool clear_soft_references,
                                           IsHeapReferenceMarkedCallback* is_marked_callback,
                                           MarkObjectCallback* mark_object_callback,
                                           ProcessMarkStackCallback* process_mark_stack_callback,
                                           void* arg) {
  TimingLogger::ScopedTiming t(concurrent ? __FUNCTION__ : "(Paused)ProcessReferences", timings);
  Thread* self = Thread::Current();
  {
    MutexLock mu(self, *Locks::reference_processor_lock_);
    process_references_args_.is_marked_callback_ = is_marked_callback;
    process_references_args_.mark_callback_ = mark_object_callback;
    process_references_args_.arg_ = arg;
    CHECK_EQ(SlowPathEnabled(), concurrent) << "Slow path must be enabled iff concurrent";
  }
  // Unless required to clear soft references with white references, preserve some white referents.
  if (!clear_soft_references) {
    TimingLogger::ScopedTiming split(concurrent ? "ForwardSoftReferences" :
        "(Paused)ForwardSoftReferences", timings);
    if (concurrent) {
      StartPreservingReferences(self);
    }
    soft_reference_queue_.ForwardSoftReferences(&PreserveSoftReferenceCallback,
                                                &process_references_args_);
    process_mark_stack_callback(arg);
    if (concurrent) {
      StopPreservingReferences(self);
    }
  }
  // Clear all remaining soft and weak references with white referents.
  soft_reference_queue_.ClearWhiteReferences(&cleared_references_, is_marked_callback, arg);
  weak_reference_queue_.ClearWhiteReferences(&cleared_references_, is_marked_callback, arg);
  {
    TimingLogger::ScopedTiming t(concurrent ? "EnqueueFinalizerReferences" :
        "(Paused)EnqueueFinalizerReferences", timings);
    if (concurrent) {
      StartPreservingReferences(self);
    }
    // Preserve all white objects with finalize methods and schedule them for finalization.
    finalizer_reference_queue_.EnqueueFinalizerReferences(&cleared_references_, is_marked_callback,
                                                          mark_object_callback, arg);
    process_mark_stack_callback(arg);
    if (concurrent) {
      StopPreservingReferences(self);
    }
  }
  // Clear all finalizer referent reachable soft and weak references with white referents.
  soft_reference_queue_.ClearWhiteReferences(&cleared_references_, is_marked_callback, arg);
  weak_reference_queue_.ClearWhiteReferences(&cleared_references_, is_marked_callback, arg);
  // Clear all phantom references with white referents.
  phantom_reference_queue_.ClearWhiteReferences(&cleared_references_, is_marked_callback, arg);
  // At this point all reference queues other than the cleared references should be empty.
  DCHECK(soft_reference_queue_.IsEmpty());
  DCHECK(weak_reference_queue_.IsEmpty());
  DCHECK(finalizer_reference_queue_.IsEmpty());
  DCHECK(phantom_reference_queue_.IsEmpty());
  {
    MutexLock mu(self, *Locks::reference_processor_lock_);
    // Need to always do this since the next GC may be concurrent. Doing this for only concurrent
    // could result in a stale is_marked_callback_ being called before the reference processing
    // starts since there is a small window of time where slow_path_enabled_ is enabled but the
    // callback isn't yet set.
    process_references_args_.is_marked_callback_ = nullptr;
    if (concurrent) {
      // Done processing, disable the slow path and broadcast to the waiters.
      DisableSlowPath(self);
    }
  }
}

// Process the "referent" field in a java.lang.ref.Reference.  If the referent has not yet been
// marked, put it on the appropriate list in the heap for later processing.
void ReferenceProcessor::DelayReferenceReferent(mirror::Class* klass, mirror::Reference* ref,
                                                IsHeapReferenceMarkedCallback* is_marked_callback,
                                                void* arg) {
  // klass can be the class of the old object if the visitor already updated the class of ref.
  DCHECK(klass != nullptr);
  DCHECK(klass->IsTypeOfReferenceClass());
  mirror::HeapReference<mirror::Object>* referent = ref->GetReferentReferenceAddr();
  if (referent->AsMirrorPtr() != nullptr && !is_marked_callback(referent, arg)) {
    Thread* self = Thread::Current();
    // TODO: Remove these locks, and use atomic stacks for storing references?
    // We need to check that the references haven't already been enqueued since we can end up
    // scanning the same reference multiple times due to dirty cards.
    if (klass->IsSoftReferenceClass()) {
      soft_reference_queue_.AtomicEnqueueIfNotEnqueued(self, ref);
    } else if (klass->IsWeakReferenceClass()) {
      weak_reference_queue_.AtomicEnqueueIfNotEnqueued(self, ref);
    } else if (klass->IsFinalizerReferenceClass()) {
      finalizer_reference_queue_.AtomicEnqueueIfNotEnqueued(self, ref);
    } else if (klass->IsPhantomReferenceClass()) {
      phantom_reference_queue_.AtomicEnqueueIfNotEnqueued(self, ref);
    } else {
      LOG(FATAL) << "Invalid reference type " << PrettyClass(klass) << " " << std::hex
                 << klass->GetAccessFlags();
    }
  }
}

void ReferenceProcessor::UpdateRoots(IsMarkedCallback* callback, void* arg) {
  cleared_references_.UpdateRoots(callback, arg);
}

void ReferenceProcessor::EnqueueClearedReferences(Thread* self) {
  Locks::mutator_lock_->AssertNotHeld(self);
  if (!cleared_references_.IsEmpty()) {
    // When a runtime isn't started there are no reference queues to care about so ignore.
    if (LIKELY(Runtime::Current()->IsStarted())) {
      ScopedObjectAccess soa(self);
      ScopedLocalRef<jobject> arg(self->GetJniEnv(),
                                  soa.AddLocalReference<jobject>(cleared_references_.GetList()));
      jvalue args[1];
      args[0].l = arg.get();
      InvokeWithJValues(soa, nullptr, WellKnownClasses::java_lang_ref_ReferenceQueue_add, args);
    }
    cleared_references_.Clear();
  }
}

bool ReferenceProcessor::MakeCircularListIfUnenqueued(mirror::FinalizerReference* reference) {
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::reference_processor_lock_);
  // Wait untul we are done processing reference.
  while (SlowPathEnabled()) {
    condition_.Wait(self);
  }
  // At this point, since the sentinel of the reference is live, it is guaranteed to not be
  // enqueued if we just finished processing references. Otherwise, we may be doing the main GC
  // phase. Since we are holding the reference processor lock, it guarantees that reference
  // processing can't begin. The GC could have just enqueued the reference one one of the internal
  // GC queues, but since we hold the lock finalizer_reference_queue_ lock it also prevents this
  // race.
  MutexLock mu2(self, *Locks::reference_queue_finalizer_references_lock_);
  if (!reference->IsEnqueued()) {
    CHECK(reference->IsFinalizerReferenceInstance());
    if (Runtime::Current()->IsActiveTransaction()) {
      reference->SetPendingNext<true>(reference);
    } else {
      reference->SetPendingNext<false>(reference);
    }
    return true;
  }
  return false;
}

}  // namespace gc
}  // namespace art
