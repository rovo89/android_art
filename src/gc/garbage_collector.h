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

#ifndef ART_SRC_GC_GARBAGE_COLLECTOR_H_
#define ART_SRC_GC_GARBAGE_COLLECTOR_H_

#include "locks.h"

#include <stdint.h>
#include <vector>

namespace art {

class Heap;

class GarbageCollector {
 public:
  // Returns true iff the garbage collector is concurrent.
  virtual bool IsConcurrent() const = 0;

  GarbageCollector(Heap* heap);

  virtual ~GarbageCollector();

  // Run the garbage collector.
  void Run();

  Heap* GetHeap() {
    return heap_;
  }

  // Returns how long the mutators were paused in nanoseconds.
  const std::vector<uint64_t>& GetPauseTimes() const {
    return pause_times_;
  }

  // Returns how long the GC took to complete in nanoseconds.
  uint64_t GetDuration() const {
    return duration_;
  }


  virtual std::string GetName() const = 0;

  void RegisterPause(uint64_t nano_length);

 protected:
  // The initial phase. Done without mutators paused.
  virtual void InitializePhase() = 0;

  // Mark all reachable objects, done concurrently.
  virtual void MarkingPhase() = 0;

  // Only called for concurrent GCs. Gets called repeatedly until it succeeds.
  virtual bool HandleDirtyObjectsPhase() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Called with mutators running.
  virtual void ReclaimPhase() = 0;

  // Called after the GC is finished. Done without mutators paused.
  virtual void FinishPhase() = 0;

  Heap* heap_;
  std::vector<uint64_t> pause_times_;
  uint64_t duration_;
};

}  // namespace art

#endif  // ART_SRC_GC_GARBAGE_COLLECTOR_H_
