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
#include "safe_map.h"

#define VERIFY_OBJECT_ENABLED 0

// Fast verification means we do not verify the classes of objects.
#define VERIFY_OBJECT_FAST 1

namespace art {

class AllocSpace;
class Class;
class HeapBitmap;
class ImageSpace;
class MarkStack;
class ModUnionTable;
class Object;
class Space;
class SpaceTest;
class Thread;
class TimingLogger;

typedef std::vector<Space*> Spaces;

enum GcType {
  // Full GC
  GC_FULL,
  // Sticky mark bits "generational" GC.
  GC_STICKY,
  // Partial GC, over only the alloc space
  GC_PARTIAL,
};

class LOCKABLE Heap {
 public:
  static const size_t kInitialSize = 2 * MB;

  static const size_t kMaximumSize = 32 * MB;

  // After how many GCs we force to do a partial GC instead of sticky mark bits GC.
  static const size_t kPartialGCFrequency = 10;

  // Sticky mark bits GC has some overhead, so if we have less a few megabytes of AllocSpace then
  // it's probably better to just do a partial GC.
  static const size_t kMinAllocSpaceSizeForStickyGC = 6 * MB;

  // Minimum remaining size fo sticky GC. Since sticky GC doesn't free up as much memory as a
  // normal GC, it is important to not use it when we are almost out of memory.
  static const size_t kMinRemainingSpaceForStickyGC = 1 * MB;

  typedef void (RootVisitor)(const Object* root, void* arg);
  typedef bool (IsMarkedTester)(const Object* object, void* arg);

  // Create a heap with the requested sizes. The possible empty
  // image_file_names names specify Spaces to load based on
  // ImageWriter output.
  explicit Heap(size_t starting_size, size_t growth_limit, size_t capacity,
                const std::string& image_file_name, bool concurrent_gc);

  ~Heap();

  // Allocates and initializes storage for an object instance.
  Object* AllocObject(Class* klass, size_t num_bytes)
      LOCKS_EXCLUDED(statistics_lock_)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

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
  bool IsLiveObjectLocked(const Object* obj)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::heap_bitmap_lock_);

  // Initiates an explicit garbage collection.
  void CollectGarbage(bool clear_soft_references)
      LOCKS_EXCLUDED(GlobalSynchronization::mutator_lock_);

  // Does a concurrent GC, should only be called by the GC daemon thread
  // through runtime.
  void ConcurrentGC();

  // Implements java.lang.Runtime.maxMemory.
  int64_t GetMaxMemory();
  // Implements java.lang.Runtime.totalMemory.
  int64_t GetTotalMemory();
  // Implements java.lang.Runtime.freeMemory.
  int64_t GetFreeMemory() LOCKS_EXCLUDED(statistics_lock_);

  // Implements VMDebug.countInstancesOfClass.
  int64_t CountInstances(Class* c, bool count_assignable)
      LOCKS_EXCLUDED(GlobalSynchronization::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

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

  const Spaces& GetSpaces() {
    return spaces_;
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

  void RecordFree(size_t freed_objects, size_t freed_bytes) LOCKS_EXCLUDED(statistics_lock_);

  // Must be called if a field of an Object in the heap changes, and before any GC safe-point.
  // The call is not needed if NULL is stored in the field.
  void WriteBarrierField(const Object* dst, MemberOffset /*offset*/, const Object* /*new_value*/) {
    if (!card_marking_disabled_) {
      card_table_->MarkCard(dst);
    }
  }

  // Write barrier for array operations that update many field positions
  void WriteBarrierArray(const Object* dst, int /*start_offset*/,
                         size_t /*length TODO: element_count or byte_count?*/) {
    if (UNLIKELY(!card_marking_disabled_)) {
      card_table_->MarkCard(dst);
    }
  }

  CardTable* GetCardTable() {
    return card_table_.get();
  }

  void DisableCardMarking() {
    // TODO: we shouldn't need to disable card marking, this is here to help the image_writer
    card_marking_disabled_ = true;
  }

  void AddFinalizerReference(Thread* self, Object* object);

  size_t GetBytesAllocated() const LOCKS_EXCLUDED(statistics_lock_);
  size_t GetObjectsAllocated() const LOCKS_EXCLUDED(statistics_lock_);
  size_t GetConcurrentStartSize() const LOCKS_EXCLUDED(statistics_lock_);
  size_t GetConcurrentMinFree() const LOCKS_EXCLUDED(statistics_lock_);

  // Functions for getting the bitmap which corresponds to an object's address.
  // This is probably slow, TODO: use better data structure like binary tree .
  Space* FindSpaceFromObject(const Object*) const;

  void DumpForSigQuit(std::ostream& os) LOCKS_EXCLUDED(statistics_lock_);

  void Trim(AllocSpace* alloc_space);

  HeapBitmap* GetLiveBitmap() SHARED_LOCKS_REQUIRED(GlobalSynchronization::heap_bitmap_lock_) {
    return live_bitmap_.get();
  }

  HeapBitmap* GetMarkBitmap() SHARED_LOCKS_REQUIRED(GlobalSynchronization::heap_bitmap_lock_) {
    return mark_bitmap_.get();
  }

  void PreZygoteFork();

  // Mark and empty stack.
  void FlushAllocStack()
      EXCLUSIVE_LOCKS_REQUIRED(GlobalSynchronization::heap_bitmap_lock_);

  // Mark all the objects in the allocation stack as live.
  void MarkStackAsLive(MarkStack* alloc_stack);

  // Un-mark all the objects in the allocation stack.
  void UnMarkStack(MarkStack* alloc_stack);

  // DEPRECATED: Should remove in "near" future when support for multiple image spaces is added.
  // Assumes there is only one image space.
  ImageSpace* GetImageSpace();
  AllocSpace* GetAllocSpace();
  void DumpSpaces();

 private:
  // Allocates uninitialized storage.
  Object* Allocate(size_t num_bytes)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);
  Object* Allocate(AllocSpace* space, size_t num_bytes)
      LOCKS_EXCLUDED(GlobalSynchronization::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

  // Pushes a list of cleared references out to the managed heap.
  void EnqueueClearedReferences(Object** cleared_references);

  void RequestHeapTrim();
  void RequestConcurrentGC();

  void RecordAllocation(AllocSpace* space, const Object* object)
      LOCKS_EXCLUDED(statistics_lock_, GlobalSynchronization::heap_bitmap_lock_);

  void CollectGarbageInternal(GcType gc_plan, bool clear_soft_references)
      LOCKS_EXCLUDED(gc_complete_lock_,
                     GlobalSynchronization::heap_bitmap_lock_,
                     GlobalSynchronization::mutator_lock_,
                     GlobalSynchronization::thread_suspend_count_lock_);
  void CollectGarbageMarkSweepPlan(GcType gc_plan, bool clear_soft_references)
      LOCKS_EXCLUDED(GlobalSynchronization::heap_bitmap_lock_,
                     GlobalSynchronization::mutator_lock_);
  void CollectGarbageConcurrentMarkSweepPlan(GcType gc_plan, bool clear_soft_references)
      LOCKS_EXCLUDED(GlobalSynchronization::heap_bitmap_lock_,
                     GlobalSynchronization::mutator_lock_);

  // Given the current contents of the alloc space, increase the allowed heap footprint to match
  // the target utilization ratio.  This should only be called immediately after a full garbage
  // collection.
  void GrowForUtilization();

  size_t GetPercentFree();

  void AddSpace(Space* space) LOCKS_EXCLUDED(GlobalSynchronization::heap_bitmap_lock_);

  // No thread saftey analysis since we call this everywhere and it is impossible to find a proper
  // lock ordering for it.
  void VerifyObjectBody(const Object *obj)
      NO_THREAD_SAFETY_ANALYSIS;

  static void VerificationCallback(Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(GlobalSychronization::heap_bitmap_lock_);

  Spaces spaces_;

  // The alloc space which we are currently allocating into.
  AllocSpace* alloc_space_;

  // The mod-union table remembers all of the references from the image space to the alloc /
  // zygote spaces.
  UniquePtr<ModUnionTable> mod_union_table_;

  // This table holds all of the references from the zygote space to the alloc space.
  UniquePtr<ModUnionTable> zygote_mod_union_table_;

  UniquePtr<CardTable> card_table_;

  // True for concurrent mark sweep GC, false for mark sweep.
  const bool concurrent_gc_;

  // If we have a zygote space.
  bool have_zygote_space_;

  // Used by the image writer to disable card marking on copied objects
  // TODO: remove
  bool card_marking_disabled_;

  // Guards access to the state of GC, associated conditional variable is used to signal when a GC
  // completes.
  Mutex* gc_complete_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  UniquePtr<ConditionVariable> gc_complete_cond_ GUARDED_BY(gc_complete_lock_);

  // True while the garbage collector is running.
  volatile bool is_gc_running_ GUARDED_BY(gc_complete_lock_);;

  // Bytes until concurrent GC starts.
  volatile size_t concurrent_start_bytes_;
  size_t concurrent_start_size_;
  size_t concurrent_min_free_;
  size_t sticky_gc_count_;

  // Number of bytes allocated.  Adjusted after each allocation and free.
  volatile size_t num_bytes_allocated_;

  // Number of objects allocated.  Adjusted after each allocation and free.
  volatile size_t num_objects_allocated_;

  // Last trim time
  uint64_t last_trim_time_;

  UniquePtr<HeapBitmap> live_bitmap_ GUARDED_BY(GlobalSynchronization::heap_bitmap_lock_);
  UniquePtr<HeapBitmap> mark_bitmap_ GUARDED_BY(GlobalSynchronization::heap_bitmap_lock_);

  // True while the garbage collector is trying to signal the GC daemon thread.
  // This flag is needed to prevent recursion from occurring when the JNI calls
  // allocate memory and request another GC.
  bool try_running_gc_;

  // Used to ensure that we don't ever recursively request GC.
  volatile bool requesting_gc_;

  // Mark stack that we reuse to avoid re-allocating the mark stack
  UniquePtr<MarkStack> mark_stack_;

  // Allocation stack, new allocations go here so that we can do sticky mark bits. This enables us
  // to use the live bitmap as the old mark bitmap.
  UniquePtr<MarkStack> allocation_stack_;

  // Second allocation stack so that we can process allocation with the heap unlocked.
  UniquePtr<MarkStack> live_stack_;

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
  FRIEND_TEST(SpaceTest, ZygoteSpace);
  friend class SpaceTest;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
};

}  // namespace art

#endif  // ART_SRC_HEAP_H_
