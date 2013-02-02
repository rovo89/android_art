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

#include "garbage_collector.h"

#include "base/mutex-inl.h"
#include "thread.h"
#include "thread_list.h"

namespace art {
  GarbageCollector::GarbageCollector(Heap* heap)
      : heap_(heap),
        duration_(0) {

  }

  bool GarbageCollector::HandleDirtyObjectsPhase() {
    DCHECK(IsConcurrent());
    return true;
  }

  void GarbageCollector::RegisterPause(uint64_t nano_length) {
    pause_times_.push_back(nano_length);
  }

  void GarbageCollector::Run() {
    Thread* self = Thread::Current();
    ThreadList* thread_list = Runtime::Current()->GetThreadList();

    uint64_t start_time = NanoTime();
    pause_times_.clear();
    duration_ = 0;

    InitializePhase();

    if (!IsConcurrent()) {
      // Pause is the entire length of the GC.
      uint64_t pause_start = NanoTime();
      thread_list->SuspendAll();
      MarkingPhase();
      ReclaimPhase();
      thread_list->ResumeAll();
      uint64_t pause_end = NanoTime();
      pause_times_.push_back(pause_end - pause_start);
    } else {
      {
        ReaderMutexLock mu(self, *Locks::mutator_lock_);
        MarkingPhase();
      }
      bool done = false;
      while (!done) {
        uint64_t pause_start = NanoTime();
        thread_list->SuspendAll();
        done = HandleDirtyObjectsPhase();
        thread_list->ResumeAll();
        uint64_t pause_end = NanoTime();
        pause_times_.push_back(pause_end - pause_start);
      }
      {
        ReaderMutexLock mu(self, *Locks::mutator_lock_);
        ReclaimPhase();
      }
    }

    uint64_t end_time = NanoTime();
    duration_ = end_time - start_time;

    FinishPhase();
  }

  GarbageCollector::~GarbageCollector() {

  }
}  // namespace art
