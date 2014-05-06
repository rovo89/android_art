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

#include <stdio.h>

#include "garbage_collector.h"

#include "base/histogram-inl.h"
#include "base/logging.h"
#include "base/mutex-inl.h"
#include "gc/accounting/heap_bitmap.h"
#include "gc/space/large_object_space.h"
#include "gc/space/space-inl.h"
#include "thread-inl.h"
#include "thread_list.h"

namespace art {
namespace gc {
namespace collector {

GarbageCollector::GarbageCollector(Heap* heap, const std::string& name)
    : heap_(heap),
      name_(name),
      gc_cause_(kGcCauseForAlloc),
      clear_soft_references_(false),
      duration_ns_(0),
      timings_(name_.c_str(), true, VLOG_IS_ON(heap)),
      pause_histogram_((name_ + " paused").c_str(), kPauseBucketSize, kPauseBucketCount),
      cumulative_timings_(name) {
  ResetCumulativeStatistics();
}

void GarbageCollector::RegisterPause(uint64_t nano_length) {
  pause_times_.push_back(nano_length);
}

void GarbageCollector::ResetCumulativeStatistics() {
  cumulative_timings_.Reset();
  pause_histogram_.Reset();
  total_time_ns_ = 0;
  total_freed_objects_ = 0;
  total_freed_bytes_ = 0;
}

void GarbageCollector::Run(GcCause gc_cause, bool clear_soft_references) {
  Thread* self = Thread::Current();
  uint64_t start_time = NanoTime();
  timings_.Reset();
  pause_times_.clear();
  duration_ns_ = 0;
  clear_soft_references_ = clear_soft_references;
  gc_cause_ = gc_cause;
  // Reset stats.
  freed_bytes_ = 0;
  freed_large_object_bytes_ = 0;
  freed_objects_ = 0;
  freed_large_objects_ = 0;
  RunPhases();  // Run all the GC phases.
  // Add the current timings to the cumulative timings.
  cumulative_timings_.AddLogger(timings_);
  // Update cumulative statistics with how many bytes the GC iteration freed.
  total_freed_objects_ += GetFreedObjects() + GetFreedLargeObjects();
  total_freed_bytes_ += GetFreedBytes() + GetFreedLargeObjectBytes();
  uint64_t end_time = NanoTime();
  duration_ns_ = end_time - start_time;
  if (Locks::mutator_lock_->IsExclusiveHeld(self)) {
    // The entire GC was paused, clear the fake pauses which might be in the pause times and add
    // the whole GC duration.
    pause_times_.clear();
    RegisterPause(duration_ns_);
  }
  total_time_ns_ += GetDurationNs();
  for (uint64_t pause_time : pause_times_) {
    pause_histogram_.AddValue(pause_time / 1000);
  }
}

void GarbageCollector::SwapBitmaps() {
  // Swap the live and mark bitmaps for each alloc space. This is needed since sweep re-swaps
  // these bitmaps. The bitmap swapping is an optimization so that we do not need to clear the live
  // bits of dead objects in the live bitmap.
  const GcType gc_type = GetGcType();
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    // We never allocate into zygote spaces.
    if (space->GetGcRetentionPolicy() == space::kGcRetentionPolicyAlwaysCollect ||
        (gc_type == kGcTypeFull &&
         space->GetGcRetentionPolicy() == space::kGcRetentionPolicyFullCollect)) {
      accounting::ContinuousSpaceBitmap* live_bitmap = space->GetLiveBitmap();
      accounting::ContinuousSpaceBitmap* mark_bitmap = space->GetMarkBitmap();
      if (live_bitmap != nullptr && live_bitmap != mark_bitmap) {
        heap_->GetLiveBitmap()->ReplaceBitmap(live_bitmap, mark_bitmap);
        heap_->GetMarkBitmap()->ReplaceBitmap(mark_bitmap, live_bitmap);
        CHECK(space->IsContinuousMemMapAllocSpace());
        space->AsContinuousMemMapAllocSpace()->SwapBitmaps();
      }
    }
  }
  for (const auto& disc_space : GetHeap()->GetDiscontinuousSpaces()) {
    space::LargeObjectSpace* space = disc_space->AsLargeObjectSpace();
    accounting::LargeObjectBitmap* live_set = space->GetLiveBitmap();
    accounting::LargeObjectBitmap* mark_set = space->GetMarkBitmap();
    heap_->GetLiveBitmap()->ReplaceLargeObjectBitmap(live_set, mark_set);
    heap_->GetMarkBitmap()->ReplaceLargeObjectBitmap(mark_set, live_set);
    space->SwapBitmaps();
  }
}

uint64_t GarbageCollector::GetEstimatedMeanThroughput() const {
  // Add 1ms to prevent possible division by 0.
  return (total_freed_bytes_ * 1000) / (NsToMs(GetCumulativeTimings().GetTotalNs()) + 1);
}

uint64_t GarbageCollector::GetEstimatedLastIterationThroughput() const {
  // Add 1ms to prevent possible division by 0.
  return (static_cast<uint64_t>(freed_bytes_) * 1000) / (NsToMs(GetDurationNs()) + 1);
}

void GarbageCollector::RecordFree(uint64_t freed_objects, int64_t freed_bytes) {
  freed_objects_ += freed_objects;
  freed_bytes_ += freed_bytes;
  GetHeap()->RecordFree(freed_objects, freed_bytes);
}

void GarbageCollector::RecordFreeLargeObjects(uint64_t freed_objects, int64_t freed_bytes) {
  freed_large_objects_ += freed_objects;
  freed_large_object_bytes_ += freed_bytes;
  GetHeap()->RecordFree(freed_objects, freed_bytes);
}

void GarbageCollector::ResetMeasurements() {
  cumulative_timings_.Reset();
  pause_histogram_.Reset();
  total_time_ns_ = 0;
  total_freed_objects_ = 0;
  total_freed_bytes_ = 0;
}

GarbageCollector::ScopedPause::ScopedPause(GarbageCollector* collector)
    : start_time_(NanoTime()), collector_(collector) {
  Runtime::Current()->GetThreadList()->SuspendAll();
}

GarbageCollector::ScopedPause::~ScopedPause() {
  collector_->RegisterPause(NanoTime() - start_time_);
  Runtime::Current()->GetThreadList()->ResumeAll();
}

}  // namespace collector
}  // namespace gc
}  // namespace art
