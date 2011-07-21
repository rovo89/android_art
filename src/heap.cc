// Copyright 2011 Google Inc. All Rights Reserved.
// Author: cshapiro@google.com (Carl Shapiro)

#include "heap.h"
#include "object.h"
#include "space.h"

namespace art {

Space* Heap::space_ = NULL;

size_t Heap::startup_size_ = 0;

size_t Heap::maximum_size_ = 0;

bool Heap::is_gc_running_ = false;

HeapBitmap* Heap::mark_bitmap_ = NULL;

HeapBitmap* Heap::live_bitmap_ = NULL;

bool Heap::Init(size_t startup_size, size_t maximum_size) {
  space_ = Space::Create(startup_size, maximum_size);
  if (space_ == NULL) {
    return false;
  }

  byte* base = space_->GetBase();
  size_t num_bytes = space_->Size();

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

  startup_size_ = startup_size;
  maximum_size_ = maximum_size;
  live_bitmap_ = live_bitmap.release();
  mark_bitmap_ = mark_bitmap.release();

  // TODO: allocate the card table

  return true;
}

void Heap::Destroy() {
  delete space_;
  delete mark_bitmap_;
  delete live_bitmap_;
}

Object* Heap::Allocate(size_t size) {
  // Fail impossible allocations.  TODO: collect soft references.
  if (size > maximum_size_) {
    return NULL;
  }

  Object* ptr = space_->AllocWithoutGrowth(size);
  if (ptr != NULL) {
    return ptr;
  }

  // The allocation failed.  If the GC is running, block until it
  // completes and retry.
  if (is_gc_running_) {
    // The GC is concurrently tracing the heap.  Release the heap
    // lock, wait for the GC to complete, and retrying allocating.
    WaitForConcurrentGcToComplete();
    ptr = space_->AllocWithoutGrowth(size);
    if (ptr != NULL) {
      return ptr;
    }
  }

  // Another failure.  Our thread was starved or there may be too many
  // live objects.  Try a foreground GC.  This will have no effect if
  // the concurrent GC is already running.
  CollectGarbage();
  ptr = space_->AllocWithoutGrowth(size);
  if (ptr != NULL) {
    return ptr;
  }

  // Even that didn't work;  this is an exceptional state.
  // Try harder, growing the heap if necessary.
  ptr = space_->AllocWithGrowth(size);
  if (ptr != NULL) {
    //size_t new_footprint = dvmHeapSourceGetIdealFootprint();
    size_t new_footprint = space_->MaxAllowedFootprint();
    //TODO: may want to grow a little bit more so that the amount of free
    //      space is equal to the old free space + the utilization slop for
    //      the new allocation.
    // LOGI_HEAP("Grow heap (frag case) to "
    //           "%zu.%03zuMB for %zu-byte allocation",
    //           FRACTIONAL_MB(newHeapSize), size);
    LOG(INFO) << "Grow heap (frag case) to " << new_footprint
              << "for " << size << "-byte allocation";
    return ptr;
  }

  // Most allocations should have succeeded by now, so the heap is
  // really full, really fragmented, or the requested size is really
  // big.  Do another GC, collecting SoftReferences this time.  The VM
  // spec requires that all SoftReferences have been collected and
  // cleared before throwing an OOME.

  //TODO: wait for the finalizers from the previous GC to finish
  //collect_soft_refs:
  LOG(INFO) << "Forcing collection of SoftReferences for "
            << size << "-byte allocation";
  //gcForMalloc(true);
  CollectGarbage();
  ptr = space_->AllocWithGrowth(size);
  if (ptr != NULL) {
    return ptr;
  }
  //TODO: maybe wait for finalizers and try one last time

  //LOGE_HEAP("Out of memory on a %zd-byte allocation.", size);
  LOG(ERROR) << "Out of memory on a " << size << " byte allocation";

  //TODO: tell the HeapSource to dump its state
  //dvmDumpThread(dvmThreadSelf(), false);

  // TODO: stack trace
  return NULL;
}

String* Heap::AllocStringFromModifiedUtf8(Class* java_lang_String,
                                          Class* char_array,
                                          const char* data) {
  String* string = AllocString(java_lang_String);
  uint32_t count = strlen(data);  // TODO
  CharArray* array = AllocCharArray(char_array, count);
  string->array_ = array;
  string->count_ = count;
  return string;
}

void Heap::CollectGarbage() {
}

void Heap::CollectGarbageInternal() {
}

void Heap::WaitForConcurrentGcToComplete() {
}

// Given the current contents of the active heap, increase the allowed
// heap footprint to match the target utilization ratio.  This should
// only be called immediately after a full garbage collection.
void Heap::GrowForUtilization() {
  LOG(FATAL) << "Unimplemented";
}

}  // namespace art
