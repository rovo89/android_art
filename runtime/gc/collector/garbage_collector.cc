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

#define ATRACE_TAG ATRACE_TAG_DALVIK

#include <stdio.h>
#include <cutils/trace.h>

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

void GarbageCollector::PausePhase() {
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
  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  Thread* self = Thread::Current();
  uint64_t start_time = NanoTime();
  pause_times_.clear();
  duration_ns_ = 0;
  clear_soft_references_ = clear_soft_references;
  gc_cause_ = gc_cause;

  // Reset stats.
  freed_bytes_ = 0;
  freed_large_object_bytes_ = 0;
  freed_objects_ = 0;
  freed_large_objects_ = 0;

  CollectorType collector_type = GetCollectorType();
  switch (collector_type) {
    case kCollectorTypeMS:      // Fall through.
    case kCollectorTypeSS:      // Fall through.
    case kCollectorTypeGSS: {
      InitializePhase();
      // Pause is the entire length of the GC.
      uint64_t pause_start = NanoTime();
      ATRACE_BEGIN("Application threads suspended");
      // Mutator lock may be already exclusively held when we do garbage collections for changing
      // the current collector / allocator during process state updates.
      if (Locks::mutator_lock_->IsExclusiveHeld(self)) {
        // PreGcRosAllocVerification() is called in Heap::TransitionCollector().
        RevokeAllThreadLocalBuffers();
        MarkingPhase();
        PausePhase();
        ReclaimPhase();
        // PostGcRosAllocVerification() is called in Heap::TransitionCollector().
      } else {
        ATRACE_BEGIN("Suspending mutator threads");
        thread_list->SuspendAll();
        ATRACE_END();
        GetHeap()->PreGcRosAllocVerification(&timings_);
        RevokeAllThreadLocalBuffers();
        MarkingPhase();
        PausePhase();
        ReclaimPhase();
        GetHeap()->PostGcRosAllocVerification(&timings_);
        ATRACE_BEGIN("Resuming mutator threads");
        thread_list->ResumeAll();
        ATRACE_END();
      }
      ATRACE_END();
      RegisterPause(NanoTime() - pause_start);
      FinishPhase();
      break;
    }
    case kCollectorTypeCMS: {
      InitializePhase();
      CHECK(!Locks::mutator_lock_->IsExclusiveHeld(self));
      {
        ReaderMutexLock mu(self, *Locks::mutator_lock_);
        MarkingPhase();
      }
      uint64_t pause_start = NanoTime();
      ATRACE_BEGIN("Suspending mutator threads");
      thread_list->SuspendAll();
      ATRACE_END();
      ATRACE_BEGIN("All mutator threads suspended");
      GetHeap()->PreGcRosAllocVerification(&timings_);
      PausePhase();
      RevokeAllThreadLocalBuffers();
      GetHeap()->PostGcRosAllocVerification(&timings_);
      ATRACE_END();
      uint64_t pause_end = NanoTime();
      ATRACE_BEGIN("Resuming mutator threads");
      thread_list->ResumeAll();
      ATRACE_END();
      RegisterPause(pause_end - pause_start);
      {
        ReaderMutexLock mu(self, *Locks::mutator_lock_);
        ReclaimPhase();
      }
      FinishPhase();
      break;
    }
    case kCollectorTypeCC: {
      // To be implemented.
      break;
    }
    default: {
      LOG(FATAL) << "Unreachable collector type=" << static_cast<size_t>(collector_type);
      break;
    }
  }
  // Add the current timings to the cumulative timings.
  cumulative_timings_.AddLogger(timings_);
  // Update cumulative statistics with how many bytes the GC iteration freed.
  total_freed_objects_ += GetFreedObjects() + GetFreedLargeObjects();
  total_freed_bytes_ += GetFreedBytes() + GetFreedLargeObjectBytes();
  uint64_t end_time = NanoTime();
  duration_ns_ = end_time - start_time;
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
      accounting::SpaceBitmap* live_bitmap = space->GetLiveBitmap();
      accounting::SpaceBitmap* mark_bitmap = space->GetMarkBitmap();
      if (live_bitmap != nullptr && live_bitmap != mark_bitmap) {
        heap_->GetLiveBitmap()->ReplaceBitmap(live_bitmap, mark_bitmap);
        heap_->GetMarkBitmap()->ReplaceBitmap(mark_bitmap, live_bitmap);
        CHECK(space->IsContinuousMemMapAllocSpace());
        space->AsContinuousMemMapAllocSpace()->SwapBitmaps();
      }
    }
  }
  for (const auto& disc_space : GetHeap()->GetDiscontinuousSpaces()) {
    space::LargeObjectSpace* space = down_cast<space::LargeObjectSpace*>(disc_space);
    accounting::ObjectSet* live_set = space->GetLiveObjects();
    accounting::ObjectSet* mark_set = space->GetMarkObjects();
    heap_->GetLiveBitmap()->ReplaceObjectSet(live_set, mark_set);
    heap_->GetMarkBitmap()->ReplaceObjectSet(mark_set, live_set);
    down_cast<space::LargeObjectSpace*>(space)->SwapBitmaps();
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

}  // namespace collector
}  // namespace gc
}  // namespace art
