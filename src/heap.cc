// Copyright 2011 Google Inc. All Rights Reserved.

#include "heap.h"

#include <vector>

#include "image.h"
#include "mark_sweep.h"
#include "object.h"
#include "space.h"
#include "scoped_ptr.h"
#include "stl_util.h"

namespace art {

std::vector<Space*> Heap::spaces_;

Space* Heap::boot_space_ = NULL;

Space* Heap::alloc_space_ = NULL;

size_t Heap::maximum_size_ = 0;

size_t Heap::num_bytes_allocated_ = 0;

size_t Heap::num_objects_allocated_ = 0;

bool Heap::is_gc_running_ = false;

HeapBitmap* Heap::mark_bitmap_ = NULL;

HeapBitmap* Heap::live_bitmap_ = NULL;

bool Heap::Init(size_t initial_size, size_t maximum_size, const char* boot_image_file_name) {
  Space* boot_space;
  byte* requested_base;
  if (boot_image_file_name == NULL) {
    boot_space = NULL;
    requested_base = NULL;
  } else {
    boot_space = Space::Create(boot_image_file_name);
    if (boot_space == NULL) {
      return false;
    }
    spaces_.push_back(boot_space);
    requested_base = boot_space->GetBase() + RoundUp(boot_space->Size(), kPageSize);
  }

  Space* space = Space::Create(initial_size, maximum_size, requested_base);
  if (space == NULL) {
    return false;
  }

  if (boot_space == NULL) {
    boot_space = space;
  }
  byte* base = std::min(boot_space->GetBase(), space->GetBase());
  byte* limit = std::max(boot_space->GetLimit(), space->GetLimit());
  DCHECK_LT(base, limit);
  size_t num_bytes = limit - base;

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

  alloc_space_ = space;
  spaces_.push_back(space);
  maximum_size_ = maximum_size;
  live_bitmap_ = live_bitmap.release();
  mark_bitmap_ = mark_bitmap.release();

  // TODO: allocate the card table

  // Make objects in boot_space live (after live_bitmap_ is set)
  if (boot_image_file_name != NULL) {
    boot_space_ = boot_space;
    RecordImageAllocations(boot_space);
  }

  return true;
}

void Heap::Destroy() {
  STLDeleteElements(&spaces_);
  if (mark_bitmap_ != NULL) {
    delete mark_bitmap_;
    mark_bitmap_ = NULL;
  }
  if (live_bitmap_ != NULL) {
    delete live_bitmap_;
  }
  live_bitmap_ = NULL;
}

Object* Heap::AllocObject(Class* klass, size_t num_bytes) {
  DCHECK(klass == NULL
         || klass->descriptor_ == NULL
         || (klass->IsClassClass() && num_bytes >= sizeof(Class))
         || (klass->object_size_ == (klass->IsArray() ? 0 : num_bytes)));
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

void Heap::RecordImageAllocations(Space* space) {
  CHECK(space != NULL);
  CHECK(live_bitmap_ != NULL);
  byte* current = space->GetBase() + RoundUp(sizeof(ImageHeader), 8);
  while (current < space->GetLimit()) {
    DCHECK(IsAligned(current, 8));
    const Object* obj = reinterpret_cast<const Object*>(current);
    live_bitmap_->Set(obj);
    current += RoundUp(obj->SizeOf(), 8);
  }
}

Object* Heap::Allocate(size_t size) {
  DCHECK(alloc_space_ != NULL);
  Space* space = alloc_space_;
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
  UNIMPLEMENTED(ERROR);
}

}  // namespace art
