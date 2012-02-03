/*
 * Copyright (C) 2008 The Android Open Source Project
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

#ifndef ART_SRC_HEAP_H_
#define ART_SRC_HEAP_H_

#include <vector>

#include "card_table.h"
#include "globals.h"
#include "gtest/gtest.h"
#include "heap_bitmap.h"
#include "mutex.h"
#include "offsets.h"

#define VERIFY_OBJECT_ENABLED 0

namespace art {

class AllocSpace;
class Class;
class Object;
class Space;
class Thread;
class HeapBitmap;
class SpaceTest;

class Heap {
 public:
  static const size_t kInitialSize = 2 * MB;

  static const size_t kMaximumSize = 32 * MB;

  typedef void (RootVisitor)(const Object* root, void* arg);
  typedef bool (IsMarkedTester)(const Object* object, void* arg);

  // Create a heap with the requested sizes. The possible empty
  // image_file_names names specify Spaces to load based on
  // ImageWriter output.
  static void Init(size_t starting_size, size_t growth_limit, size_t capacity,
                   const std::vector<std::string>& image_file_names);

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

  // A weaker test than IsLiveObject or VerifyObject that doesn't require the heap lock,
  // and doesn't abort on error, allowing the caller to report more
  // meaningful diagnostics.
  static bool IsHeapAddress(const Object* obj);
  // Returns true if 'obj' is a live heap object, false otherwise (including for invalid addresses).
  // Requires the heap lock to be held.
  static bool IsLiveObjectLocked(const Object* obj);

  // Initiates an explicit garbage collection.
  static void CollectGarbage(bool clear_soft_references);

  // Implements java.lang.Runtime.maxMemory.
  static int64_t GetMaxMemory();
  // Implements java.lang.Runtime.totalMemory.
  static int64_t GetTotalMemory();
  // Implements java.lang.Runtime.freeMemory.
  static int64_t GetFreeMemory();

  // Implements VMDebug.countInstancesOfClass.
  static int64_t CountInstances(Class* c, bool count_assignable);

  // Removes the growth limit on the alloc space so it may grow to its maximum capacity. Used to
  // implement dalvik.system.VMRuntime.clearGrowthLimit.
  static void ClearGrowthLimit();

  // Target ideal heap utilization ratio, implements
  // dalvik.system.VMRuntime.getTargetHeapUtilization.
  static float GetTargetHeapUtilization() {
    return target_utilization_;
  }
  // Set target ideal heap utilization ratio, implements
  // dalvik.system.VMRuntime.setTargetHeapUtilization.
  static void SetTargetHeapUtilization(float target) {
    DCHECK_GT(target, 0.0f);  // asserted in Java code
    DCHECK_LT(target, 1.0f);
    target_utilization_ = target;
  }

  // For the alloc space, sets the maximum number of bytes that the heap is allowed to allocate
  // from the system. Doesn't allow the space to exceed its growth limit.
  static void SetIdealFootprint(size_t max_allowed_footprint);

  // Blocks the caller until the garbage collector becomes idle.
  static void WaitForConcurrentGcToComplete();

  static pid_t GetLockOwner(); // For SignalCatcher.
  static void Lock();
  static void Unlock();
  static void AssertLockHeld() {
    lock_->AssertHeld();
  }
  static void AssertLockNotHeld() {
    lock_->AssertNotHeld();
  }

  static const std::vector<Space*>& GetSpaces() {
    return spaces_;
  }

  static HeapBitmap* GetLiveBits() {
    return live_bitmap_;
  }

  static HeapBitmap* GetMarkBits() {
    return mark_bitmap_;
  }

  static void SetWellKnownClasses(Class* java_lang_ref_FinalizerReference,
      Class* java_lang_ref_ReferenceQueue);

  static void SetReferenceOffsets(MemberOffset reference_referent_offset,
                                  MemberOffset reference_queue_offset,
                                  MemberOffset reference_queueNext_offset,
                                  MemberOffset reference_pendingNext_offset,
                                  MemberOffset finalizer_reference_zombie_offset);

  static Object* GetReferenceReferent(Object* reference);
  static void ClearReferenceReferent(Object* reference);

  // Returns true if the reference object has not yet been enqueued.
  static bool IsEnqueuable(const Object* ref);
  static void EnqueueReference(Object* ref, Object** list);
  static void EnqueuePendingReference(Object* ref, Object** list);
  static Object* DequeuePendingReference(Object** list);

  static MemberOffset GetReferencePendingNextOffset() {
    DCHECK_NE(reference_pendingNext_offset_.Uint32Value(), 0U);
    return reference_pendingNext_offset_;
  }

  static MemberOffset GetFinalizerReferenceZombieOffset() {
    DCHECK_NE(finalizer_reference_zombie_offset_.Uint32Value(), 0U);
    return finalizer_reference_zombie_offset_;
  }

  static void EnableObjectValidation() {
#if VERIFY_OBJECT_ENABLED
    Heap::VerifyHeap();
#endif
    verify_objects_ = true;
  }

  static void DisableObjectValidation() {
    verify_objects_ = false;
  }

  // Callers must hold the heap lock.
  static void RecordFreeLocked(size_t freed_objects, size_t freed_bytes);

  // Must be called if a field of an Object in the heap changes, and before any GC safe-point.
  // The call is not needed if NULL is stored in the field.
  static void WriteBarrierField(const Object* dest, MemberOffset offset, const Object* new_val) {
    if (!card_marking_disabled_) {
      card_table_->MarkCard(dest);
    }
  }

  // Write barrier for array operations that update many field positions
  static void WriteBarrierArray(const Object* dest, int pos, size_t len) {
    if (UNLIKELY(!card_marking_disabled_)) {
      card_table_->MarkCard(dest);
    }
  }

  static CardTable* GetCardTable() {
    return card_table_;
  }

  static void DisableCardMarking() {
    // TODO: we shouldn't need to disable card marking, this is here to help the image_writer
    card_marking_disabled_ = true;
  }

  static void AddFinalizerReference(Thread* self, Object* object);

  static size_t GetBytesAllocated() { return num_bytes_allocated_; }
  static size_t GetObjectsAllocated() { return num_objects_allocated_; }

  static AllocSpace* GetAllocSpace() {
    return alloc_space_;
  }

 private:
  // Allocates uninitialized storage.
  static Object* AllocateLocked(size_t num_bytes);
  static Object* AllocateLocked(AllocSpace* space, size_t num_bytes);

  // Pushes a list of cleared references out to the managed heap.
  static void EnqueueClearedReferences(Object** cleared_references);

  static void RequestHeapTrim();

  static void RecordAllocationLocked(AllocSpace* space, const Object* object);
  static void RecordImageAllocations(Space* space);

  static void CollectGarbageInternal(bool clear_soft_references);

  // Given the current contents of the alloc space, increase the allowed heap footprint to match
  // the target utilization ratio.  This should only be called immediately after a full garbage
  // collection.
  static void GrowForUtilization();

  static void AddSpace(Space* space) {
    spaces_.push_back(space);
  }

  static void VerifyObjectLocked(const Object *obj);

  static void VerificationCallback(Object* obj, void* arg);

  static Mutex* lock_;

  static std::vector<Space*> spaces_;

  // default Space for allocations
  static AllocSpace* alloc_space_;

  static HeapBitmap* mark_bitmap_;

  static HeapBitmap* live_bitmap_;

  static CardTable* card_table_;

  // Used by the image writer to disable card marking on copied objects
  // TODO: remove
  static bool card_marking_disabled_;

  // True while the garbage collector is running.
  static bool is_gc_running_;

  // Number of bytes allocated.  Adjusted after each allocation and
  // free.
  static size_t num_bytes_allocated_;

  // Number of objects allocated.  Adjusted after each allocation and
  // free.
  static size_t num_objects_allocated_;

  static Class* java_lang_ref_FinalizerReference_;
  static Class* java_lang_ref_ReferenceQueue_;

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

  // Target ideal heap utilization ratio
  static float target_utilization_;

  static bool verify_objects_;

  FRIEND_TEST(SpaceTest, AllocAndFree);
  FRIEND_TEST(SpaceTest, AllocAndFreeList);
  friend class SpaceTest;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
};

class ScopedHeapLock {
 public:
  ScopedHeapLock() {
    Heap::Lock();
  }

  ~ScopedHeapLock() {
    Heap::Unlock();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(ScopedHeapLock);
};

}  // namespace art

#endif  // ART_SRC_HEAP_H_
