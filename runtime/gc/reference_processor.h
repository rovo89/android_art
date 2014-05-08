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

#ifndef ART_RUNTIME_GC_REFERENCE_PROCESSOR_H_
#define ART_RUNTIME_GC_REFERENCE_PROCESSOR_H_

#include "base/mutex.h"
#include "globals.h"
#include "jni.h"
#include "object_callbacks.h"
#include "reference_queue.h"

namespace art {

class TimingLogger;

namespace mirror {
class Object;
class Reference;
}  // namespace mirror

namespace gc {

class Heap;

// Used to process java.lang.References concurrently or paused.
class ReferenceProcessor {
 public:
  explicit ReferenceProcessor();
  static mirror::Object* PreserveSoftReferenceCallback(mirror::Object* obj, void* arg);
  void ProcessReferences(bool concurrent, TimingLogger* timings, bool clear_soft_references,
                         IsMarkedCallback* is_marked_callback,
                         MarkObjectCallback* mark_object_callback,
                         ProcessMarkStackCallback* process_mark_stack_callback, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      LOCKS_EXCLUDED(lock_);
  // Only allow setting this with mutators suspended so that we can avoid using a lock in the
  // GetReferent fast path as an optimization.
  void EnableSlowPath() EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
  // Decode the referent, may block if references are being processed.
  mirror::Object* GetReferent(Thread* self, mirror::Reference* reference)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) LOCKS_EXCLUDED(lock_);
  void EnqueueClearedReferences() LOCKS_EXCLUDED(Locks::mutator_lock_);
  void DelayReferenceReferent(mirror::Class* klass, mirror::Reference* ref,
                              IsMarkedCallback is_marked_callback, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
  class ProcessReferencesArgs {
   public:
    ProcessReferencesArgs(IsMarkedCallback* is_marked_callback,
                          MarkObjectCallback* mark_callback, void* arg)
        : is_marked_callback_(is_marked_callback), mark_callback_(mark_callback), arg_(arg) {
    }

    // The is marked callback is null when the args aren't set up.
    IsMarkedCallback* is_marked_callback_;
    MarkObjectCallback* mark_callback_;
    void* arg_;
  };
  // Called by ProcessReferences.
  void DisableSlowPath(Thread* self) EXCLUSIVE_LOCKS_REQUIRED(lock_);
  // If we are preserving references it means that some dead objects may become live, we use start
  // and stop preserving to block mutators using GetReferrent from getting access to these
  // referents.
  void StartPreservingReferences(Thread* self) LOCKS_EXCLUDED(lock_);
  void StopPreservingReferences(Thread* self) LOCKS_EXCLUDED(lock_);
  // Process args, used by the GetReferent to return referents which are already marked.
  ProcessReferencesArgs process_references_args_ GUARDED_BY(lock_);
  // Boolean for whether or not we need to go slow path in GetReferent.
  volatile bool slow_path_enabled_;
  // Boolean for whether or not we are preserving references (either soft references or finalizers).
  // If this is true, then we cannot return a referent (see comment in GetReferent).
  bool preserving_references_ GUARDED_BY(lock_);
  // Lock that guards the reference processing.
  Mutex lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  // Condition that people wait on if they attempt to get the referent of a reference while
  // processing is in progress.
  ConditionVariable condition_ GUARDED_BY(lock_);
  // Reference queues used by the GC.
  ReferenceQueue soft_reference_queue_;
  ReferenceQueue weak_reference_queue_;
  ReferenceQueue finalizer_reference_queue_;
  ReferenceQueue phantom_reference_queue_;
  ReferenceQueue cleared_references_;
};

}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_REFERENCE_PROCESSOR_H_
