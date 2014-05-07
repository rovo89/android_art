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

#ifndef ART_RUNTIME_GC_COLLECTOR_GARBAGE_COLLECTOR_H_
#define ART_RUNTIME_GC_COLLECTOR_GARBAGE_COLLECTOR_H_

#include "base/histogram.h"
#include "base/mutex.h"
#include "base/timing_logger.h"
#include "gc/collector_type.h"
#include "gc/gc_cause.h"
#include "gc_type.h"
#include <stdint.h>
#include <vector>

namespace art {
namespace gc {

class Heap;

namespace collector {

class GarbageCollector {
 public:
  class SCOPED_LOCKABLE ScopedPause {
   public:
    explicit ScopedPause(GarbageCollector* collector) EXCLUSIVE_LOCK_FUNCTION(Locks::mutator_lock_);
    ~ScopedPause() UNLOCK_FUNCTION();

   private:
    const uint64_t start_time_;
    GarbageCollector* const collector_;
  };

  GarbageCollector(Heap* heap, const std::string& name);
  virtual ~GarbageCollector() { }

  const char* GetName() const {
    return name_.c_str();
  }

  virtual GcType GetGcType() const = 0;

  virtual CollectorType GetCollectorType() const = 0;

  // Run the garbage collector.
  void Run(GcCause gc_cause, bool clear_soft_references);

  Heap* GetHeap() const {
    return heap_;
  }

  // Returns how long the mutators were paused in nanoseconds.
  const std::vector<uint64_t>& GetPauseTimes() const {
    return pause_times_;
  }

  // Returns how long the GC took to complete in nanoseconds.
  uint64_t GetDurationNs() const {
    return duration_ns_;
  }

  void RegisterPause(uint64_t nano_length);

  TimingLogger& GetTimings() {
    return timings_;
  }
  const CumulativeLogger& GetCumulativeTimings() const {
    return cumulative_timings_;
  }

  void ResetCumulativeStatistics();

  // Swap the live and mark bitmaps of spaces that are active for the collector. For partial GC,
  // this is the allocation space, for full GC then we swap the zygote bitmaps too.
  void SwapBitmaps() EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  int64_t GetFreedBytes() const {
    return freed_bytes_;
  }

  int64_t GetFreedLargeObjectBytes() const {
    return freed_large_object_bytes_;
  }

  uint64_t GetFreedObjects() const {
    return freed_objects_;
  }

  uint64_t GetFreedLargeObjects() const {
    return freed_large_objects_;
  }

  uint64_t GetTotalPausedTimeNs() const {
    return pause_histogram_.Sum();
  }

  int64_t GetTotalFreedBytes() const {
    return total_freed_bytes_;
  }

  uint64_t GetTotalFreedObjects() const {
    return total_freed_objects_;
  }

  const Histogram<uint64_t>& GetPauseHistogram() const {
    return pause_histogram_;
  }

  // Reset the cumulative timings and pause histogram.
  void ResetMeasurements();

  // Returns the estimated throughput in bytes / second.
  uint64_t GetEstimatedMeanThroughput() const;

  // Returns the estimated throughput of the last GC iteration.
  uint64_t GetEstimatedLastIterationThroughput() const;

  // Returns how many GC iterations have been run.
  size_t GetIterations() const {
    return GetCumulativeTimings().GetIterations();
  }

 protected:
  // Run all of the GC phases.
  virtual void RunPhases() = 0;

  // Revoke all the thread-local buffers.
  virtual void RevokeAllThreadLocalBuffers() = 0;

  // Record that you have freed some objects or large objects, calls Heap::RecordFree.
  // TODO: These are not thread safe, add a lock if we get parallel sweeping.
  void RecordFree(uint64_t freed_objects, int64_t freed_bytes);
  void RecordFreeLargeObjects(uint64_t freed_objects, int64_t freed_bytes);

  static constexpr size_t kPauseBucketSize = 500;
  static constexpr size_t kPauseBucketCount = 32;

  Heap* const heap_;

  std::string name_;

  GcCause gc_cause_;
  bool clear_soft_references_;

  uint64_t duration_ns_;
  TimingLogger timings_;

  // Cumulative statistics.
  Histogram<uint64_t> pause_histogram_;
  uint64_t total_time_ns_;
  uint64_t total_freed_objects_;
  int64_t total_freed_bytes_;

  // Single GC statitstics, freed bytes are signed since the GC can free negative bytes if it
  // promotes objects to a space which has a larger allocation size.
  int64_t freed_bytes_;
  int64_t freed_large_object_bytes_;
  uint64_t freed_objects_;
  uint64_t freed_large_objects_;

  CumulativeLogger cumulative_timings_;

  std::vector<uint64_t> pause_times_;
};

}  // namespace collector
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_COLLECTOR_GARBAGE_COLLECTOR_H_
