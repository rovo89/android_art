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
#include "mirror/reference-inl.h"
#include "reflection.h"
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change.h"
#include "well_known_classes.h"

namespace art {
namespace gc {

ReferenceProcessor::ReferenceProcessor()
    : process_references_args_(nullptr, nullptr, nullptr), slow_path_enabled_(false),
      preserving_references_(false), lock_("reference processor lock", kReferenceProcessorLock),
      condition_("reference processor condition", lock_) {
}

void ReferenceProcessor::EnableSlowPath() {
  Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());
  slow_path_enabled_ = true;
}

void ReferenceProcessor::DisableSlowPath(Thread* self) {
  slow_path_enabled_ = false;
  condition_.Broadcast(self);
}

mirror::Object* ReferenceProcessor::GetReferent(Thread* self, mirror::Reference* reference) {
  mirror::Object* const referent = reference->GetReferent();
  if (LIKELY(!slow_path_enabled_)) {
    return referent;
  }
  // Another fast path, the referent is cleared, we can just return null since there is no scenario
  // where it becomes non-null.
  if (referent == nullptr) {
    return nullptr;
  }
  MutexLock mu(self, lock_);
  while (slow_path_enabled_) {
    mirror::Object* const referent = reference->GetReferent();
    // If the referent became cleared, return it.
    if (referent == nullptr) {
      return nullptr;
    }
    // Try to see if the referent is already marked by using the is_marked_callback. We can return
    // it to the mutator as long as the GC is not preserving references. If the GC is
    // preserving references, the mutator could take a white field and move it somewhere else
    // in the heap causing corruption since this field would get swept.
    IsMarkedCallback* const is_marked_callback = process_references_args_.is_marked_callback_;
    if (!preserving_references_ && is_marked_callback != nullptr) {
      mirror::Object* const obj = is_marked_callback(referent, process_references_args_.arg_);
      // If it's null it means not marked, but it could become marked if the referent is reachable
      // by finalizer referents. So we can not return in this case and must block.
      if (obj != nullptr) {
        return obj;
      }
    }
    condition_.WaitHoldingLocks(self);
  }
  return reference->GetReferent();
}

mirror::Object* ReferenceProcessor::PreserveSoftReferenceCallback(mirror::Object* obj, void* arg) {
  auto* const args = reinterpret_cast<ProcessReferencesArgs*>(arg);
  // TODO: Not preserve all soft references.
  return args->mark_callback_(obj, args->arg_);
}

void ReferenceProcessor::StartPreservingReferences(Thread* self) {
  MutexLock mu(self, lock_);
  preserving_references_ = true;
}

void ReferenceProcessor::StopPreservingReferences(Thread* self) {
  MutexLock mu(self, lock_);
  preserving_references_ = false;
  // We are done preserving references, some people who are blocked may see a marked referent.
  condition_.Broadcast(self);
}

// Process reference class instances and schedule finalizations.
void ReferenceProcessor::ProcessReferences(bool concurrent, TimingLogger* timings,
                                           bool clear_soft_references,
                                           IsMarkedCallback* is_marked_callback,
                                           MarkObjectCallback* mark_object_callback,
                                           ProcessMarkStackCallback* process_mark_stack_callback,
                                           void* arg) {
  Thread* self = Thread::Current();
  {
    MutexLock mu(self, lock_);
    process_references_args_.is_marked_callback_ = is_marked_callback;
    process_references_args_.mark_callback_ = mark_object_callback;
    process_references_args_.arg_ = arg;
    CHECK_EQ(slow_path_enabled_, concurrent) << "Slow path must be enabled iff concurrent";
  }
  timings->StartSplit(concurrent ? "ProcessReferences" : "(Paused)ProcessReferences");
  // Unless required to clear soft references with white references, preserve some white referents.
  if (!clear_soft_references) {
    TimingLogger::ScopedSplit split(concurrent ? "PreserveSomeSoftReferences" :
        "(Paused)PreserveSomeSoftReferences", timings);
    if (concurrent) {
      StartPreservingReferences(self);
    }
    // References with a marked referent are removed from the list.
    soft_reference_queue_.PreserveSomeSoftReferences(&PreserveSoftReferenceCallback,
                                                     &process_references_args_);
    process_mark_stack_callback(arg);
    if (concurrent) {
      StopPreservingReferences(self);
    }
  }
  // Clear all remaining soft and weak references with white referents.
  soft_reference_queue_.ClearWhiteReferences(cleared_references_, is_marked_callback, arg);
  weak_reference_queue_.ClearWhiteReferences(cleared_references_, is_marked_callback, arg);
  {
    TimingLogger::ScopedSplit split(concurrent ? "EnqueueFinalizerReferences" :
        "(Paused)EnqueueFinalizerReferences", timings);
    if (concurrent) {
      StartPreservingReferences(self);
    }
    // Preserve all white objects with finalize methods and schedule them for finalization.
    finalizer_reference_queue_.EnqueueFinalizerReferences(cleared_references_, is_marked_callback,
                                                          mark_object_callback, arg);
    process_mark_stack_callback(arg);
    if (concurrent) {
      StopPreservingReferences(self);
    }
  }
  // Clear all finalizer referent reachable soft and weak references with white referents.
  soft_reference_queue_.ClearWhiteReferences(cleared_references_, is_marked_callback, arg);
  weak_reference_queue_.ClearWhiteReferences(cleared_references_, is_marked_callback, arg);
  // Clear all phantom references with white referents.
  phantom_reference_queue_.ClearWhiteReferences(cleared_references_, is_marked_callback, arg);
  // At this point all reference queues other than the cleared references should be empty.
  DCHECK(soft_reference_queue_.IsEmpty());
  DCHECK(weak_reference_queue_.IsEmpty());
  DCHECK(finalizer_reference_queue_.IsEmpty());
  DCHECK(phantom_reference_queue_.IsEmpty());
  {
    MutexLock mu(self, lock_);
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
  timings->EndSplit();
}

// Process the "referent" field in a java.lang.ref.Reference.  If the referent has not yet been
// marked, put it on the appropriate list in the heap for later processing.
void ReferenceProcessor::DelayReferenceReferent(mirror::Class* klass, mirror::Reference* ref,
                                                IsMarkedCallback is_marked_callback, void* arg) {
  // klass can be the class of the old object if the visitor already updated the class of ref.
  DCHECK(klass->IsReferenceClass());
  mirror::Object* referent = ref->GetReferent<kWithoutReadBarrier>();
  if (referent != nullptr) {
    mirror::Object* forward_address = is_marked_callback(referent, arg);
    // Null means that the object is not currently marked.
    if (forward_address == nullptr) {
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
    } else if (referent != forward_address) {
      // Referent is already marked and we need to update it.
      ref->SetReferent<false>(forward_address);
    }
  }
}

void ReferenceProcessor::EnqueueClearedReferences() {
  Thread* self = Thread::Current();
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

}  // namespace gc
}  // namespace art
