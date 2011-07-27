// Copyright 2011 Google Inc. All Rights Reserved.
// Author: cshapiro@google.com (Carl Shapiro)

#include "heap.h"

#include <vector>

#include "mark_sweep.h"
#include "object.h"
#include "space.h"
#include "stl_util.h"

namespace art {

std::vector<Space*> Heap::spaces_;

size_t Heap::startup_size_ = 0;

size_t Heap::maximum_size_ = 0;

size_t Heap::num_bytes_allocated_ = 0;

size_t Heap::num_objects_allocated_ = 0;

bool Heap::is_gc_running_ = false;

HeapBitmap* Heap::mark_bitmap_ = NULL;

HeapBitmap* Heap::live_bitmap_ = NULL;

bool Heap::Init(size_t startup_size, size_t maximum_size) {
  Space* space = Space::Create(startup_size, maximum_size);
  if (space == NULL) {
    return false;
  }

  byte* base = space->GetBase();
  size_t num_bytes = space->Size();

  // Allocate the initial live bitmap.
  scoped_ptr<HeapBitmap> live_bitmap(HeapBitmap::Create(base, num_bytes));
  if (live_bitmap == NULL) {
    return false;
  }

  // Allocate the initial mark bitmap.
  scoped_ptr<HeapBitmap> mark_bitmap(HeapBitmap::Create(base, num_bytes));
  if (mark_bitmap == NULL) {
    return false;
  }

  spaces_.push_back(space);
  startup_size_ = startup_size;
  maximum_size_ = maximum_size;
  live_bitmap_ = live_bitmap.release();
  mark_bitmap_ = mark_bitmap.release();

  // TODO: allocate the card table

  return true;
}

void Heap::Destroy() {
  STLDeleteElements(&spaces_);
  delete mark_bitmap_;
  delete live_bitmap_;
}

Object* Heap::AllocObject(Class* klass, size_t num_bytes) {
  Object* obj = Allocate(num_bytes);
  if (obj != NULL) {
    obj->klass_ = klass;
  }
  return obj;
}

void Heap::RecordAllocation(Space* space, const Object* obj) {
  size_t size = space->AllocationSize(obj);
  DCHECK_NE(size, 0u);
  num_bytes_allocated_ += size;
  num_objects_allocated_ += 1;
  live_bitmap_->Set(obj);
}

void Heap::RecordFree(Space* space, const Object* obj) {
  size_t size = space->AllocationSize(obj);
  DCHECK_NE(size, 0u);
  if (size < num_bytes_allocated_) {
    num_bytes_allocated_ -= size;
  } else {
    num_bytes_allocated_ = 0;
  }
  live_bitmap_->Clear(obj);
  if (num_objects_allocated_ > 0) {
    num_objects_allocated_ -= 1;
  }
}

Object* Heap::Allocate(size_t size) {
  CHECK_EQ(spaces_.size(), 1u);
  Space* space = spaces_[0];
  Object* obj = Allocate(space, size);
  if (obj != NULL) {
    RecordAllocation(space, obj);
  }
  return obj;
}

Object* Heap::Allocate(Space* space, size_t size) {
  // Fail impossible allocations.  TODO: collect soft references.
  if (size > maximum_size_) {
    return NULL;
  }

  Object* ptr = space->AllocWithoutGrowth(size);
  if (ptr != NULL) {
    return ptr;
  }

  // The allocation failed.  If the GC is running, block until it
  // completes and retry.
  if (is_gc_running_) {
    // The GC is concurrently tracing the heap.  Release the heap
    // lock, wait for the GC to complete, and retrying allocating.
    WaitForConcurrentGcToComplete();
    ptr = space->AllocWithoutGrowth(size);
    if (ptr != NULL) {
      return ptr;
    }
  }

  // Another failure.  Our thread was starved or there may be too many
  // live objects.  Try a foreground GC.  This will have no effect if
  // the concurrent GC is already running.
  CollectGarbageInternal();
  ptr = space->AllocWithoutGrowth(size);
  if (ptr != NULL) {
    return ptr;
  }

  // Even that didn't work;  this is an exceptional state.
  // Try harder, growing the heap if necessary.
  ptr = space->AllocWithGrowth(size);
  if (ptr != NULL) {
    //size_t new_footprint = dvmHeapSourceGetIdealFootprint();
    size_t new_footprint = space->MaxAllowedFootprint();
    // TODO: may want to grow a little bit more so that the amount of
    //       free space is equal to the old free space + the
    //       utilization slop for the new allocation.
    LOG(INFO) << "Grow heap (frag case) to " << new_footprint / MB
              << "for " << size << "-byte allocation";
    return ptr;
  }

  // Most allocations should have succeeded by now, so the heap is
  // really full, really fragmented, or the requested size is really
  // big.  Do another GC, collecting SoftReferences this time.  The VM
  // spec requires that all SoftReferences have been collected and
  // cleared before throwing an OOME.

  // TODO: wait for the finalizers from the previous GC to finish
  LOG(INFO) << "Forcing collection of SoftReferences for "
            << size << "-byte allocation";
  CollectGarbageInternal();
  ptr = space->AllocWithGrowth(size);
  if (ptr != NULL) {
    return ptr;
  }

  LOG(ERROR) << "Out of memory on a " << size << " byte allocation";

  // TODO: tell the HeapSource to dump its state
  // TODO: dump stack traces for all threads

  return NULL;
}

void Heap::CollectGarbage() {
  CollectGarbageInternal();
}

void Heap::CollectGarbageInternal() {
  // TODO: check that heap lock is held

  // TODO: Suspend all threads
  {
    MarkSweep mark_sweep;

    mark_sweep.Init();

    mark_sweep.MarkRoots();

    // Push marked roots onto the mark stack

    // TODO: if concurrent
    //   unlock heap
    //   resume threads

    mark_sweep.RecursiveMark();

    // TODO: if concurrent
    //   lock heap
    //   suspend threads
    //   re-mark root set
    //   scan dirty objects

    mark_sweep.ProcessReferences(false);

    // TODO: swap bitmaps

    mark_sweep.Sweep();
  }

  GrowForUtilization();

  // TODO: Resume all threads
}

void Heap::WaitForConcurrentGcToComplete() {
}

// Given the current contents of the active heap, increase the allowed
// heap footprint to match the target utilization ratio.  This should
// only be called immediately after a full garbage collection.
void Heap::GrowForUtilization() {
  LOG(ERROR) << "Unimplemented";
}

}  // namespace art
