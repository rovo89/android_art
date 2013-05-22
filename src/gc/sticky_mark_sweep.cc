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

#include "heap.h"
#include "large_object_space.h"
#include "space.h"
#include "sticky_mark_sweep.h"
#include "thread.h"

namespace art {

StickyMarkSweep::StickyMarkSweep(Heap* heap, bool is_concurrent)
    : PartialMarkSweep(heap, is_concurrent) {
  cumulative_timings_.SetName(GetName());
}

StickyMarkSweep::~StickyMarkSweep() {

}

void StickyMarkSweep::BindBitmaps() {
  PartialMarkSweep::BindBitmaps();

  Spaces& spaces = GetHeap()->GetSpaces();
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  // For sticky GC, we want to bind the bitmaps of both the zygote space and the alloc space.
  // This lets us start with the mark bitmap of the previous garbage collection as the current
  // mark bitmap of the alloc space. After the sticky GC finishes, we then unbind the bitmaps,
  // making it so that the live bitmap of the alloc space is contains the newly marked objects
  // from the sticky GC.
  for (Spaces::iterator it = spaces.begin(); it != spaces.end(); ++it) {
    if ((*it)->GetGcRetentionPolicy() == kGcRetentionPolicyAlwaysCollect) {
      BindLiveToMarkBitmap(*it);
    }
  }

  GetHeap()->GetLargeObjectsSpace()->CopyLiveToMarked();
}

void StickyMarkSweep::MarkReachableObjects() {
  DisableFinger();
  RecursiveMarkDirtyObjects(CardTable::kCardDirty - 1);
}

void StickyMarkSweep::Sweep(TimingLogger& timings, bool swap_bitmaps) {
  ObjectStack* live_stack = GetHeap()->GetLiveStack();
  SweepArray(timings_, live_stack, false);
  timings_.AddSplit("SweepArray");
}

}  // namespace art
