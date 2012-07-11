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

#include <iosfwd>
#include <string>
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
class HeapBitmap;
class ImageSpace;
class MarkStack;
class ModUnionTable;
class ModUnionTableBitmap;
class Object;
class Space;
class SpaceTest;
class Thread;

class LOCKABLE Heap {
 public:
  static const size_t kInitialSize = 2 * MB;

  static const size_t kMaximumSize = 32 * MB;

  typedef void (RootVisitor)(const Object* root, void* arg);
  typedef bool (IsMarkedTester)(const Object* object, void* arg);

  // Create a heap with the requested sizes. The possible empty
  // image_file_names names specify Spaces to load based on
  // ImageWriter output.
  explicit Heap(size_t starting_size, size_t growth_limit, size_t capacity,
                const std::string& image_file_name);

  ~Heap();

  // Allocates and initializes storage for an object instance.
  Object* AllocObject(Class* klass, size_t num_bytes);

  // Check sanity of given reference. Requires the heap lock.
#if VERIFY_OBJECT_ENABLED
  void VerifyObject(const Object* o);
#else
  void VerifyObject(const Object*) {}
#endif

  // Check sanity of all live references. Requires the heap lock.
  void VerifyHeap();

  // A weaker test than IsLiveObject or VerifyObject that doesn't require the heap lock,
  // and doesn't abort on error, allowing the caller to report more
  // meaningful diagnostics.
  bool IsHeapAddress(const Object* obj);

  // Returns true if 'obj' is a live heap object, false otherwise (including for invalid addresses).
  // Requires the heap lock to be held.
  bool IsLiveObjectLocked(const Object* obj);

  // Initiates an explicit garbage collection.
  void CollectGarbage(bool clear_soft_references);

  // Does a concurrent GC, should only be called by the GC daemon thread
  // through runtime.
  void ConcurrentGC();

  // Implements java.lang.Runtime.maxMemory.
  int64_t GetMaxMemory();
  // Implements java.lang.Runtime.totalMemory.
  int64_t GetTotalMemory();
  // Implements java.lang.Runtime.freeMemory.
  int64_t GetFreeMemory();

  // Implements VMDebug.countInstancesOfClass.
  int64_t CountInstances(Class* c, bool count_assignable);

  // Removes the growth limit on the alloc space so it may grow to its maximum capacity. Used to
  // implement dalvik.system.VMRuntime.clearGrowthLimit.
  void ClearGrowthLimit();

  // Target ideal heap utilization ratio, implements
  // dalvik.system.VMRuntime.getTargetHeapUtilization.
  float GetTargetHeapUtilization() {
    return target_utilization_;
  }
  // Set target ideal heap utilization ratio, implements
  // dalvik.system.VMRuntime.setTargetHeapUtilization.
  void SetTargetHeapUtilization(float target) {
    DCHECK_GT(target, 0.0f);  // asserted in Java code
    DCHECK_LT(target, 1.0f);
    target_utilization_ = target;
  }

  // For the alloc space, sets the maximum number of bytes that the heap is allowed to allocate
  // from the system. Doesn't allow the space to exceed its growth limit.
  void SetIdealFootprint(size_t max_allowed_footprint);

  // Blocks the caller until the garbage collector becomes idle and returns
  // true if we waited for the GC to complete.
  bool WaitForConcurrentGcToComplete();

  pid_t GetLockOwner(); // For SignalCatcher.
  void AssertLockHeld() {
    lock_->AssertHeld();
  }
  void AssertLockNotHeld() {
    lock_->AssertNotHeld();
  }

  const std::vector<Space*>& GetSpaces() {
    return spaces_;
  }

  HeapBitmap* GetLiveBits() {
    return live_bitmap_;
  }

  HeapBitmap* GetMarkBits() {
    return mark_bitmap_;
  }

  void SetReferenceOffsets(MemberOffset reference_referent_offset,
                           MemberOffset reference_queue_offset,
                           MemberOffset reference_queueNext_offset,
                           MemberOffset reference_pendingNext_offset,
                           MemberOffset finalizer_reference_zombie_offset);

  Object* GetReferenceReferent(Object* reference);
  void ClearReferenceReferent(Object* reference);

  // Returns true if the reference object has not yet been enqueued.
  bool IsEnqueuable(const Object* ref);
  void EnqueueReference(Object* ref, Object** list);
  void EnqueuePendingReference(Object* ref, Object** list);
  Object* DequeuePendingReference(Object** list);

  MemberOffset GetReferencePendingNextOffset() {
    DCHECK_NE(reference_pendingNext_offset_.Uint32Value(), 0U);
    return reference_pendingNext_offset_;
  }

  MemberOffset GetFinalizerReferenceZombieOffset() {
    DCHECK_NE(finalizer_reference_zombie_offset_.Uint32Value(), 0U);
    return finalizer_reference_zombie_offset_;
  }

  void EnableObjectValidation() {
#if VERIFY_OBJECT_ENABLED
    VerifyHeap();
#endif
    verify_objects_ = true;
  }

  void DisableObjectValidation() {
    verify_objects_ = false;
  }

  // Callers must hold the heap lock.
  void RecordFreeLocked(size_t freed_objects, size_t freed_bytes);

  // Must be called if a field of an Object in the heap changes, and before any GC safe-point.
  // The call is not needed if NULL is stored in the field.
  void WriteBarrierField(const Object* dst, MemberOffset /*offset*/, const Object* /*new_value*/) {
    if (!card_marking_disabled_) {
      card_table_->MarkCard(dst);
    }
  }

  // Write barrier for array operations that update many field positions
  void WriteBarrierArray(const Object* dst, int /*start_offset*/, size_t /*length TODO: element_count or byte_count?*/) {
    if (UNLIKELY(!card_marking_disabled_)) {
      card_table_->MarkCard(dst);
    }
  }

  CardTable* GetCardTable() {
    return card_table_;
  }

  void DisableCardMarking() {
    // TODO: we shouldn't need to disable card marking, this is here to help the image_writer
    card_marking_disabled_ = true;
  }

  void AddFinalizerReference(Thread* self, Object* object);

  size_t GetBytesAllocated() { return num_bytes_allocated_; }
  size_t GetObjectsAllocated() { return num_objects_allocated_; }

  ImageSpace* GetImageSpace() {
    CHECK(image_space_ != NULL);
    return image_space_;
  }

  AllocSpace* GetAllocSpace() {
    CHECK(alloc_space_ != NULL);
    return alloc_space_;
  }

  size_t GetConcurrentStartSize() const { return concurrent_start_size_; }

  void SetConcurrentStartSize(size_t size) {
    concurrent_start_size_ = size;
  }

  size_t GetConcurrentMinFree() const { return concurrent_min_free_; }

  void SetConcurrentMinFree(size_t size) {
    concurrent_min_free_ = size;
  }

  void DumpForSigQuit(std::ostream& os);

  void Trim();

 private:
  // Allocates uninitialized storage.
  Object* AllocateLocked(size_t num_bytes);
  Object* AllocateLocked(AllocSpace* space, size_t num_bytes);

  void Lock() EXCLUSIVE_LOCK_FUNCTION();
  void Unlock() UNLOCK_FUNCTION();

  // Pushes a list of cleared references out to the managed heap.
  void EnqueueClearedReferences(Object** cleared_references);

  void RequestHeapTrim();
  void RequestConcurrentGC();

  void RecordAllocationLocked(AllocSpace* space, const Object* object);

  // TODO: can we teach GCC to understand the weird locking in here?
  void CollectGarbageInternal(bool concurrent, bool clear_soft_references) NO_THREAD_SAFETY_ANALYSIS;

  // Given the current contents of the alloc space, increase the allowed heap footprint to match
  // the target utilization ratio.  This should only be called immediately after a full garbage
  // collection.
  void GrowForUtilization();

  size_t GetPercentFree();

  void AddSpace(Space* space);

  void VerifyObjectLocked(const Object *obj);

  void VerifyHeapLocked();

  static void VerificationCallback(Object* obj, void* arg);

  Mutex* lock_;
  ConditionVariable* condition_;

  std::vector<Space*> spaces_;

  ImageSpace* image_space_;

  // default Space for allocations
  AllocSpace* alloc_space_;

  HeapBitmap* mark_bitmap_;

  HeapBitmap* live_bitmap_;

  // TODO: Reduce memory usage, this bitmap currently takes 1 bit per 8 bytes
  // of image space.
  ModUnionTable* mod_union_table_;

  CardTable* card_table_;

  // Used by the image writer to disable card marking on copied objects
  // TODO: remove
  bool card_marking_disabled_;

  // True while the garbage collector is running.
  volatile bool is_gc_running_;

  // Bytes until concurrent GC
  size_t concurrent_start_bytes_;
  size_t concurrent_start_size_;
  size_t concurrent_min_free_;

  // True while the garbage collector is trying to signal the GC daemon thread.
  // This flag is needed to prevent recursion from occurring when the JNI calls
  // allocate memory and request another GC.
  bool try_running_gc_;

  // Used to ensure that we don't ever recursively request GC.
  bool requesting_gc_;

  // Mark stack that we reuse to avoid re-allocating the mark stack
  MarkStack* mark_stack_;

  // Number of bytes allocated.  Adjusted after each allocation and free.
  size_t num_bytes_allocated_;

  // Number of objects allocated.  Adjusted after each allocation and free.
  size_t num_objects_allocated_;

  // Last trim time
  uint64_t last_trim_time_;

  // offset of java.lang.ref.Reference.referent
  MemberOffset reference_referent_offset_;

  // offset of java.lang.ref.Reference.queue
  MemberOffset reference_queue_offset_;

  // offset of java.lang.ref.Reference.queueNext
  MemberOffset reference_queueNext_offset_;

  // offset of java.lang.ref.Reference.pendingNext
  MemberOffset reference_pendingNext_offset_;

  // offset of java.lang.ref.FinalizerReference.zombie
  MemberOffset finalizer_reference_zombie_offset_;

  // Target ideal heap utilization ratio
  float target_utilization_;

  bool verify_objects_;

  friend class ScopedHeapLock;
  FRIEND_TEST(SpaceTest, AllocAndFree);
  FRIEND_TEST(SpaceTest, AllocAndFreeList);
  friend class SpaceTest;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
};

}  // namespace art

#endif  // ART_SRC_HEAP_H_
