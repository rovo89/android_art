// Copyright 2011 Google Inc. All Rights Reserved.

#include "heap.h"

#include <vector>

#include "UniquePtr.h"
#include "image.h"
#include "mark_sweep.h"
#include "object.h"
#include "space.h"
#include "stl_util.h"
#include "thread_list.h"

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

MemberOffset Heap::reference_referent_offset_ = MemberOffset(0);
MemberOffset Heap::reference_queue_offset_ = MemberOffset(0);
MemberOffset Heap::reference_queueNext_offset_ = MemberOffset(0);
MemberOffset Heap::reference_pendingNext_offset_ = MemberOffset(0);
MemberOffset Heap::finalizer_reference_zombie_offset_ = MemberOffset(0);

Mutex* Heap::lock_ = NULL;

class ScopedHeapLock {
 public:
  ScopedHeapLock() {
    Heap::Lock();
  }

  ~ScopedHeapLock() {
    Heap::Unlock();
  }
};

void Heap::Init(size_t initial_size, size_t maximum_size,
                const char* boot_image_file_name,
                std::vector<const char*>& image_file_names) {
  Space* boot_space;
  byte* requested_base;
  if (boot_image_file_name == NULL) {
    boot_space = NULL;
    requested_base = NULL;
  } else {
    boot_space = Space::Create(boot_image_file_name);
    if (boot_space == NULL) {
      LOG(FATAL) << "Failed to create space from " << boot_image_file_name;
    }
    spaces_.push_back(boot_space);
    requested_base = boot_space->GetBase() + RoundUp(boot_space->Size(), kPageSize);
  }

  std::vector<Space*> image_spaces;
  for (size_t i = 0; i < image_file_names.size(); i++) {
    Space* space = Space::Create(image_file_names[i]);
    if (space == NULL) {
      LOG(FATAL) << "Failed to create space from " << image_file_names[i];
    }
    image_spaces.push_back(space);
    spaces_.push_back(space);
    requested_base = space->GetBase() + RoundUp(space->Size(), kPageSize);
  }

  Space* space = Space::Create(initial_size, maximum_size, requested_base);
  if (space == NULL) {
    LOG(FATAL) << "Failed to create alloc space";
  }

  if (boot_space == NULL) {
    boot_space = space;
  }
  byte* base = std::min(boot_space->GetBase(), space->GetBase());
  byte* limit = std::max(boot_space->GetLimit(), space->GetLimit());
  DCHECK_LT(base, limit);
  size_t num_bytes = limit - base;

  // Allocate the initial live bitmap.
  UniquePtr<HeapBitmap> live_bitmap(HeapBitmap::Create(base, num_bytes));
  if (live_bitmap.get() == NULL) {
    LOG(FATAL) << "Failed to create live bitmap";
  }

  // Allocate the initial mark bitmap.
  UniquePtr<HeapBitmap> mark_bitmap(HeapBitmap::Create(base, num_bytes));
  if (mark_bitmap.get() == NULL) {
    LOG(FATAL) << "Failed to create mark bitmap";
  }

  alloc_space_ = space;
  spaces_.push_back(space);
  maximum_size_ = maximum_size;
  live_bitmap_ = live_bitmap.release();
  mark_bitmap_ = mark_bitmap.release();

  num_bytes_allocated_ = 0;
  num_objects_allocated_ = 0;

  // TODO: allocate the card table

  // Make objects in boot_space live (after live_bitmap_ is set)
  if (boot_image_file_name != NULL) {
    boot_space_ = boot_space;
    RecordImageAllocations(boot_space);
  }
  for (size_t i = 0; i < image_spaces.size(); i++) {
    RecordImageAllocations(image_spaces[i]);
  }

  // It's still to early to take a lock because there are no threads yet,
  // but we can create the heap lock now. We don't create it earlier to
  // make it clear that you can't use locks during heap initialization.
  lock_ = new Mutex("Heap lock");
}

void Heap::Destroy() {
  ScopedHeapLock lock;
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
  ScopedHeapLock lock;
  DCHECK(klass == NULL
         || klass->GetDescriptor() == NULL
         || (klass->IsClassClass() && num_bytes >= sizeof(Class))
         || (klass->IsVariableSize() || klass->GetObjectSize() == num_bytes));
  DCHECK(num_bytes >= sizeof(Object));
  Object* obj = AllocateLocked(num_bytes);
  if (obj != NULL) {
    obj->SetClass(klass);
  }
  return obj;
}

bool Heap::IsHeapAddress(const Object* obj) {
  // Note: we deliberately don't take the lock here, and mustn't test anything that would
  // require taking the lock.
  if (!IsAligned(obj, kObjectAlignment)) {
    return false;
  }
  // TODO
  return true;
}

bool Heap::verify_object_disabled_;

#if VERIFY_OBJECT_ENABLED
void Heap::VerifyObject(const Object* obj) {
  ScopedHeapLock lock;
  Heap::VerifyObjectLocked(obj);
}
#endif

void Heap::VerifyObjectLocked(const Object* obj) {
  lock_->AssertHeld();
  if (obj != NULL && !verify_object_disabled_) {
    if (!IsAligned(obj, kObjectAlignment)) {
      LOG(FATAL) << "Object isn't aligned: " << obj;
    } else if (!live_bitmap_->Test(obj)) {
      // TODO: we don't hold a lock here as it is assumed the live bit map
      // isn't changing if the mutator is running.
      LOG(FATAL) << "Object is dead: " << obj;
    }
    // Ignore early dawn of the universe verifications
    if (num_objects_allocated_ > 10) {
      const byte* raw_addr = reinterpret_cast<const byte*>(obj) +
          Object::ClassOffset().Int32Value();
      const Class* c = *reinterpret_cast<Class* const *>(raw_addr);
      if (c == NULL) {
        LOG(FATAL) << "Null class" << " in object: " << obj;
      } else if (!IsAligned(c, kObjectAlignment)) {
        LOG(FATAL) << "Class isn't aligned: " << c << " in object: " << obj;
      } else if (!live_bitmap_->Test(c)) {
        LOG(FATAL) << "Class of object is dead: " << c << " in object: " << obj;
      }
      // Check obj.getClass().getClass() == obj.getClass().getClass().getClass()
      // NB we don't use the accessors here as they have internal sanity checks
      // that we don't want to run
      raw_addr = reinterpret_cast<const byte*>(c) +
          Object::ClassOffset().Int32Value();
      const Class* c_c = *reinterpret_cast<Class* const *>(raw_addr);
      raw_addr = reinterpret_cast<const byte*>(c_c) +
          Object::ClassOffset().Int32Value();
      const Class* c_c_c = *reinterpret_cast<Class* const *>(raw_addr);
      CHECK_EQ(c_c, c_c_c);
    }
  }
}

void Heap::VerificationCallback(Object* obj, void *arg) {
  DCHECK(obj != NULL);
  Heap::VerifyObjectLocked(obj);
}

void Heap::VerifyHeap() {
  ScopedHeapLock lock;
  live_bitmap_->Walk(Heap::VerificationCallback, NULL);
}

void Heap::RecordAllocationLocked(Space* space, const Object* obj) {
#ifndef NDEBUG
  if (Runtime::Current()->IsStarted()) {
    lock_->AssertHeld();
  }
#endif
  size_t size = space->AllocationSize(obj);
  DCHECK_NE(size, 0u);
  num_bytes_allocated_ += size;
  num_objects_allocated_ += 1;
  live_bitmap_->Set(obj);
}

void Heap::RecordFreeLocked(Space* space, const Object* obj) {
  lock_->AssertHeld();
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
  DCHECK(!Runtime::Current()->IsStarted());
  CHECK(space != NULL);
  CHECK(live_bitmap_ != NULL);
  byte* current = space->GetBase() + RoundUp(sizeof(ImageHeader), kObjectAlignment);
  while (current < space->GetLimit()) {
    DCHECK(IsAligned(current, kObjectAlignment));
    const Object* obj = reinterpret_cast<const Object*>(current);
    live_bitmap_->Set(obj);
    current += RoundUp(obj->SizeOf(), kObjectAlignment);
  }
}

Object* Heap::AllocateLocked(size_t size) {
  lock_->AssertHeld();
  DCHECK(alloc_space_ != NULL);
  Space* space = alloc_space_;
  Object* obj = AllocateLocked(space, size);
  if (obj != NULL) {
    RecordAllocationLocked(space, obj);
  }
  return obj;
}

Object* Heap::AllocateLocked(Space* space, size_t size) {
  lock_->AssertHeld();

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

int64_t Heap::GetMaxMemory() {
  UNIMPLEMENTED(WARNING);
  return 0;
}

int64_t Heap::GetTotalMemory() {
  UNIMPLEMENTED(WARNING);
  return 0;
}

int64_t Heap::GetFreeMemory() {
  UNIMPLEMENTED(WARNING);
  return 0;
}

void Heap::CollectGarbage() {
  ScopedHeapLock lock;
  CollectGarbageInternal();
}

void Heap::CollectGarbageInternal() {
  lock_->AssertHeld();

  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  thread_list->SuspendAll();
  {
    MarkSweep mark_sweep;

    mark_sweep.Init();

    mark_sweep.MarkRoots();

    // Push marked roots onto the mark stack

    // TODO: if concurrent
    //   unlock heap
    //   thread_list->ResumeAll();

    mark_sweep.RecursiveMark();

    // TODO: if concurrent
    //   lock heap
    //   thread_list->SuspendAll();
    //   re-mark root set
    //   scan dirty objects

    mark_sweep.ProcessReferences(false);

    // TODO: swap bitmaps

    mark_sweep.Sweep();
  }

  GrowForUtilization();
  thread_list->ResumeAll();
}

void Heap::WaitForConcurrentGcToComplete() {
  lock_->AssertHeld();
}

// Given the current contents of the active heap, increase the allowed
// heap footprint to match the target utilization ratio.  This should
// only be called immediately after a full garbage collection.
void Heap::GrowForUtilization() {
  lock_->AssertHeld();
  UNIMPLEMENTED(ERROR);
}

void Heap::Lock() {
  // TODO: grab the lock, but put ourselves into Thread::kVmWait if it looks like
  // we're going to have to wait on the mutex.
  lock_->Lock();
}

void Heap::Unlock() {
  lock_->Unlock();
}

}  // namespace art
