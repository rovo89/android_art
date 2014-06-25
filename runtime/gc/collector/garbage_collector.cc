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

Iteration::Iteration()
    : duration_ns_(0), timings_("GC iteration timing logger", true, VLOG_IS_ON(heap)) {
  Reset(kGcCauseBackground, false);  // Reset to some place holder values.
}

void Iteration::Reset(GcCause gc_cause, bool clear_soft_references) {
  timings_.Reset();
  pause_times_.clear();
  duration_ns_ = 0;
  clear_soft_references_ = clear_soft_references;
  gc_cause_ = gc_cause;
  freed_ = ObjectBytePair();
  freed_los_ = ObjectBytePair();
}

uint64_t Iteration::GetEstimatedThroughput() const {
  // Add 1ms to prevent possible division by 0.
  return (static_cast<uint64_t>(freed_.bytes) * 1000) / (NsToMs(GetDurationNs()) + 1);
}

GarbageCollector::GarbageCollector(Heap* heap, const std::string& name)
    : heap_(heap),
      name_(name),
      pause_histogram_((name_ + " paused").c_str(), kPauseBucketSize, kPauseBucketCount),
      cumulative_timings_(name) {
  ResetCumulativeStatistics();
}

void GarbageCollector::RegisterPause(uint64_t nano_length) {
  GetCurrentIteration()->pause_times_.push_back(nano_length);
}

void GarbageCollector::ResetCumulativeStatistics() {
  cumulative_timings_.Reset();
  pause_histogram_.Reset();
  total_time_ns_ = 0;
  total_freed_objects_ = 0;
  total_freed_bytes_ = 0;
}

void GarbageCollector::Run(GcCause gc_cause, bool clear_soft_references) {
  ATRACE_BEGIN(StringPrintf("%s %s GC", PrettyCause(gc_cause), GetName()).c_str());
  Thread* self = Thread::Current();
  uint64_t start_time = NanoTime();
  Iteration* current_iteration = GetCurrentIteration();
  current_iteration->Reset(gc_cause, clear_soft_references);
  RunPhases();  // Run all the GC phases.
  // Add the current timings to the cumulative timings.
  cumulative_timings_.AddLogger(*GetTimings());
  // Update cumulative statistics with how many bytes the GC iteration freed.
  total_freed_objects_ += current_iteration->GetFreedObjects() +
      current_iteration->GetFreedLargeObjects();
  total_freed_bytes_ += current_iteration->GetFreedBytes() +
      current_iteration->GetFreedLargeObjectBytes();
  uint64_t end_time = NanoTime();
  current_iteration->SetDurationNs(end_time - start_time);
  if (Locks::mutator_lock_->IsExclusiveHeld(self)) {
    // The entire GC was paused, clear the fake pauses which might be in the pause times and add
    // the whole GC duration.
    current_iteration->pause_times_.clear();
    RegisterPause(current_iteration->GetDurationNs());
  }
  total_time_ns_ += current_iteration->GetDurationNs();
  for (uint64_t pause_time : current_iteration->GetPauseTimes()) {
    pause_histogram_.AddValue(pause_time / 1000);
  }
  ATRACE_END();
}

void GarbageCollector::SwapBitmaps() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
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

// Returns the current GC iteration and assocated info.
Iteration* GarbageCollector::GetCurrentIteration() {
  return heap_->GetCurrentGcIteration();
}
const Iteration* GarbageCollector::GetCurrentIteration() const {
  return heap_->GetCurrentGcIteration();
}

void GarbageCollector::RecordFree(const ObjectBytePair& freed) {
  GetCurrentIteration()->freed_.Add(freed);
  heap_->RecordFree(freed.objects, freed.bytes);
}
void GarbageCollector::RecordFreeLOS(const ObjectBytePair& freed) {
  GetCurrentIteration()->freed_los_.Add(freed);
  heap_->RecordFree(freed.objects, freed.bytes);
}

}  // namespace collector
}  // namespace gc
}  // namespace art
