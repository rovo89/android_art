// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_HEAP_H_
#define ART_SRC_HEAP_H_

#include <vector>

#include "globals.h"
#include "object_bitmap.h"
#include "thread.h"

namespace art {

class Class;
class Object;
class Space;
class HeapBitmap;

class Heap {
 public:
  static const size_t kInitialSize = 16 * MB;

  static const size_t kMaximumSize = 64 * MB;

  typedef void (RootVistor)(const Object* root, void* arg);

  // Create a heap with the requested sizes. optional boot image may
  // be NULL, otherwise it is an image filename created by ImageWriter.
  static bool Init(size_t starting_size, size_t maximum_size, const char* boot_image_file_name);

  static void Destroy();

  // Allocates and initializes storage for an object instance.
  static Object* AllocObject(Class* klass, size_t num_bytes);

  // Check sanity of given reference. Requires the heap lock.
  static void VerifyObject(const Object *obj);

  // A weaker test than VerifyObject that doesn't require the heap lock,
  // and doesn't abort on error, allowing the caller to report more
  // meaningful diagnostics.
  static bool IsHeapAddress(const Object* obj);

  // Initiates an explicit garbage collection.
  static void CollectGarbage();

  // Implements java.lang.Runtime.maxMemory.
  static int64_t GetMaxMemory();
  // Implements java.lang.Runtime.totalMemory.
  static int64_t GetTotalMemory();
  // Implements java.lang.Runtime.freeMemory.
  static int64_t GetFreeMemory();

  // Blocks the caller until the garbage collector becomes idle.
  static void WaitForConcurrentGcToComplete();

  static Mutex* GetLock() {
    return lock_;
  }

  static const std::vector<Space*>& GetSpaces() {
    return spaces_;
  }

  static Space* GetBootSpace() {
    return boot_space_;
  }

  static HeapBitmap* GetLiveBits() {
    return live_bitmap_;
  }

  static HeapBitmap* GetMarkBits() {
    return mark_bitmap_;
  }

  static void SetReferenceOffsets(size_t reference_referent_offset,
                                  size_t reference_queue_offset,
                                  size_t reference_queueNext_offset,
                                  size_t reference_pendingNext_offset,
                                  size_t finalizer_reference_zombie_offset) {
    CHECK_NE(reference_referent_offset, 0U);
    CHECK_NE(reference_queue_offset, 0U);
    CHECK_NE(reference_queueNext_offset, 0U);
    CHECK_NE(reference_pendingNext_offset, 0U);
    CHECK_NE(finalizer_reference_zombie_offset, 0U);
    reference_referent_offset_ = reference_referent_offset;
    reference_queue_offset_ = reference_queue_offset;
    reference_queueNext_offset_ = reference_queueNext_offset;
    reference_pendingNext_offset_ = reference_pendingNext_offset;
    finalizer_reference_zombie_offset_ = finalizer_reference_zombie_offset;
  }

  static size_t GetReferenceReferentOffset() {
    DCHECK_NE(reference_referent_offset_, 0U);
    return reference_referent_offset_;
  }

  static size_t GetReferenceQueueOffset() {
    DCHECK_NE(reference_queue_offset_, 0U);
    return reference_queue_offset_;
  }

  static size_t GetReferenceQueueNextOffset() {
    DCHECK_NE(reference_queueNext_offset_, 0U);
    return reference_queueNext_offset_;
  }

  static size_t GetReferencePendingNextOffset() {
    DCHECK_NE(reference_pendingNext_offset_, 0U);
    return reference_pendingNext_offset_;
  }

  static size_t GetFinalizerReferenceZombieOffset() {
    DCHECK_NE(finalizer_reference_zombie_offset_, 0U);
    return finalizer_reference_zombie_offset_;
  }

 private:
  // Allocates uninitialized storage.
  static Object* Allocate(size_t num_bytes);
  static Object* Allocate(Space* space, size_t num_bytes);

  static void RecordAllocation(Space* space, const Object* object);
  static void RecordFree(Space* space, const Object* object);
  static void RecordImageAllocations(Space* space);

  static void CollectGarbageInternal();

  static void GrowForUtilization();

  static Mutex* lock_;

  static std::vector<Space*> spaces_;

  // Space loaded from an image
  static Space* boot_space_;

  // default Space for allocations
  static Space* alloc_space_;

  static HeapBitmap* mark_bitmap_;

  static HeapBitmap* live_bitmap_;

  // The maximum size of the heap in bytes.
  static size_t maximum_size_;

  // True while the garbage collector is running.
  static bool is_gc_running_;

  // Number of bytes allocated.  Adjusted after each allocation and
  // free.
  static size_t num_bytes_allocated_;

  // Number of objects allocated.  Adjusted after each allocation and
  // free.
  static size_t num_objects_allocated_;

  // offset of java.lang.ref.Reference.referent
  static size_t reference_referent_offset_;

  // offset of java.lang.ref.Reference.queue
  static size_t reference_queue_offset_;

  // offset of java.lang.ref.Reference.queueNext
  static size_t reference_queueNext_offset_;

  // offset of java.lang.ref.Reference.pendingNext
  static size_t reference_pendingNext_offset_;

  // offset of java.lang.ref.FinalizerReference.zombie
  static size_t finalizer_reference_zombie_offset_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
};

class HeapLock {
 public:
  HeapLock(Heap* heap) : lock_(heap->GetLock()) {
    lock_->Lock();
  }
  ~HeapLock() {
    lock_->Unlock();
  }
 private:
  Mutex* lock_;
  DISALLOW_COPY_AND_ASSIGN(HeapLock);
};

}  // namespace art

#endif  // ART_SRC_HEAP_H_
