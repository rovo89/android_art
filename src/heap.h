// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_HEAP_H_
#define ART_SRC_HEAP_H_

#include <vector>

#include "globals.h"
#include "object_bitmap.h"
#include "offsets.h"

#define VERIFY_OBJECT_ENABLED 0

namespace art {

class Class;
class Mutex;
class Object;
class Space;
class HeapBitmap;

class Heap {
 public:
  static const size_t kInitialSize = 64 * MB;  // TODO: lower to 4

  static const size_t kMaximumSize = 64 * MB;  // TODO: lower to 16

  typedef void (RootVisitor)(const Object* root, void* arg);

  // Create a heap with the requested sizes. The optional boot image may
  // be NULL, otherwise it is an image filename created by ImageWriter.
  // image_file_names specifies application images to load.
  static void Init(size_t starting_size, size_t maximum_size,
                   const char* boot_image_file_name,
                   std::vector<const char*>& image_file_names);

  static void Destroy();

  // Allocates and initializes storage for an object instance.
  static Object* AllocObject(Class* klass, size_t num_bytes);

  // Check sanity of given reference. Requires the heap lock.
#if VERIFY_OBJECT_ENABLED
  static void VerifyObject(const Object *obj);
#else
  static void VerifyObject(const Object *obj) {}
#endif

  // Check sanity of all live references. Requires the heap lock.
  static void VerifyHeap();

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

  static void Lock();

  static void Unlock();

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

  static void SetReferenceOffsets(MemberOffset reference_referent_offset,
                                  MemberOffset reference_queue_offset,
                                  MemberOffset reference_queueNext_offset,
                                  MemberOffset reference_pendingNext_offset,
                                  MemberOffset finalizer_reference_zombie_offset) {
    CHECK_NE(reference_referent_offset.Uint32Value(), 0U);
    CHECK_NE(reference_queue_offset.Uint32Value(), 0U);
    CHECK_NE(reference_queueNext_offset.Uint32Value(), 0U);
    CHECK_NE(reference_pendingNext_offset.Uint32Value(), 0U);
    CHECK_NE(finalizer_reference_zombie_offset.Uint32Value(), 0U);
    reference_referent_offset_ = reference_referent_offset;
    reference_queue_offset_ = reference_queue_offset;
    reference_queueNext_offset_ = reference_queueNext_offset;
    reference_pendingNext_offset_ = reference_pendingNext_offset;
    finalizer_reference_zombie_offset_ = finalizer_reference_zombie_offset;
  }

  static MemberOffset GetReferenceReferentOffset() {
    DCHECK_NE(reference_referent_offset_.Uint32Value(), 0U);
    return reference_referent_offset_;
  }

  static MemberOffset GetReferenceQueueOffset() {
    DCHECK_NE(reference_queue_offset_.Uint32Value(), 0U);
    return reference_queue_offset_;
  }

  static MemberOffset GetReferenceQueueNextOffset() {
    DCHECK_NE(reference_queueNext_offset_.Uint32Value(), 0U);
    return reference_queueNext_offset_;
  }

  static MemberOffset GetReferencePendingNextOffset() {
    DCHECK_NE(reference_pendingNext_offset_.Uint32Value(), 0U);
    return reference_pendingNext_offset_;
  }

  static MemberOffset GetFinalizerReferenceZombieOffset() {
    DCHECK_NE(finalizer_reference_zombie_offset_.Uint32Value(), 0U);
    return finalizer_reference_zombie_offset_;
  }

  static void DisableObjectValidation() {
    // TODO: remove this hack necessary for image writing
    verify_object_disabled_ = true;
  }

  // Callers must hold the heap lock.
  static void RecordFreeLocked(Space* space, const Object* object);

  // Must be called if a field of an Object in the heap changes, and before any GC safe-point.
  // The call is not needed if NULL is stored in the field.
  static void WriteBarrier(const Object* object) {
    // TODO: re-enable card marking when concurrent collector is active.
  }

 private:
  // Allocates uninitialized storage.
  static Object* AllocateLocked(size_t num_bytes);
  static Object* AllocateLocked(Space* space, size_t num_bytes);

  static void RecordAllocationLocked(Space* space, const Object* object);
  static void RecordImageAllocations(Space* space);

  static void CollectGarbageInternal();

  static void GrowForUtilization();

  static void VerifyObjectLocked(const Object *obj);

  static void VerificationCallback(Object* obj, void *arg);

  static Mutex* lock_;

  static std::vector<Space*> spaces_;

  // Space loaded from an image
  // TODO: remove after intern_addr is removed
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
  static MemberOffset reference_referent_offset_;

  // offset of java.lang.ref.Reference.queue
  static MemberOffset reference_queue_offset_;

  // offset of java.lang.ref.Reference.queueNext
  static MemberOffset reference_queueNext_offset_;

  // offset of java.lang.ref.Reference.pendingNext
  static MemberOffset reference_pendingNext_offset_;

  // offset of java.lang.ref.FinalizerReference.zombie
  static MemberOffset finalizer_reference_zombie_offset_;

  static bool verify_object_disabled_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
};

}  // namespace art

#endif  // ART_SRC_HEAP_H_
