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

#ifndef ART_RUNTIME_GC_HEAP_H_
#define ART_RUNTIME_GC_HEAP_H_

#include <iosfwd>
#include <string>
#include <vector>

#include "atomic_integer.h"
#include "base/timing_logger.h"
#include "gc/accounting/atomic_stack.h"
#include "gc/accounting/card_table.h"
#include "gc/collector/gc_type.h"
#include "globals.h"
#include "gtest/gtest.h"
#include "locks.h"
#include "offsets.h"
#include "safe_map.h"
#include "thread_pool.h"

namespace art {

class ConditionVariable;
class Mutex;
class StackVisitor;
class Thread;
class TimingLogger;

namespace mirror {
  class Class;
  class Object;
}  // namespace mirror

namespace gc {
namespace accounting {
  class HeapBitmap;
  class ModUnionTable;
  class SpaceSetMap;
}  // namespace accounting

namespace collector {
  class GarbageCollector;
  class MarkSweep;
}  // namespace collector

namespace space {
  class AllocSpace;
  class DiscontinuousSpace;
  class DlMallocSpace;
  class ImageSpace;
  class LargeObjectSpace;
  class Space;
  class SpaceTest;
}  // namespace space

class AgeCardVisitor {
 public:
  byte operator()(byte card) const {
    if (card == accounting::CardTable::kCardDirty) {
      return card - 1;
    } else {
      return 0;
    }
  }
};

// What caused the GC?
enum GcCause {
  // GC triggered by a failed allocation. Thread doing allocation is blocked waiting for GC before
  // retrying allocation.
  kGcCauseForAlloc,
  // A background GC trying to ensure there is free memory ahead of allocations.
  kGcCauseBackground,
  // An explicit System.gc() call.
  kGcCauseExplicit,
};
std::ostream& operator<<(std::ostream& os, const GcCause& policy);

// How we want to sanity check the heap's correctness.
enum HeapVerificationMode {
  kHeapVerificationNotPermitted,  // Too early in runtime start-up for heap to be verified.
  kNoHeapVerification,  // Production default.
  kVerifyAllFast,  // Sanity check all heap accesses with quick(er) tests.
  kVerifyAll  // Sanity check all heap accesses.
};
const HeapVerificationMode kDesiredHeapVerification = kNoHeapVerification;

// This comes from ActivityManager and needs to be kept in sync.
enum ProcessState {
  PROCESS_STATE_PERSISTENT = 0,
  PROCESS_STATE_PERSISTENT_UI = 1,
  PROCESS_STATE_TOP = 2,
  PROCESS_STATE_IMPORTANT_FOREGROUND = 3,
  PROCESS_STATE_IMPORTANT_BACKGROUND = 4,
  PROCESS_STATE_BACKUP = 5,
  PROCESS_STATE_HEAVY_WEIGHT = 6,
  PROCESS_STATE_SERVICE = 7,
  PROCESS_STATE_RECEIVER = 8,
  PROCESS_STATE_HOME = 9,
  PROCESS_STATE_LAST_ACTIVITY = 10,
  PROCESS_STATE_CACHED_ACTIVITY = 11,
  PROCESS_STATE_CACHED_ACTIVITY_CLIENT = 12,
  PROCESS_STATE_CACHED_EMPTY = 13,
};

class Heap {
 public:
  static const size_t kDefaultInitialSize = 2 * MB;
  static const size_t kDefaultMaximumSize = 32 * MB;
  static const size_t kDefaultMaxFree = 2 * MB;
  static const size_t kDefaultMinFree = kDefaultMaxFree / 4;

  // Default target utilization.
  static const double kDefaultTargetUtilization;

  // Used so that we don't overflow the allocation time atomic integer.
  static const size_t kTimeAdjust = 1024;

  // Create a heap with the requested sizes. The possible empty
  // image_file_names names specify Spaces to load based on
  // ImageWriter output.
  explicit Heap(size_t initial_size, size_t growth_limit, size_t min_free,
                size_t max_free, double target_utilization, size_t capacity,
                const std::string& original_image_file_name, bool concurrent_gc,
                size_t num_gc_threads);

  ~Heap();

  // Allocates and initializes storage for an object instance.
  mirror::Object* AllocObject(Thread* self, mirror::Class* klass, size_t num_bytes)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void RegisterNativeAllocation(int bytes)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void RegisterNativeFree(int bytes) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // The given reference is believed to be to an object in the Java heap, check the soundness of it.
  void VerifyObjectImpl(const mirror::Object* o);
  void VerifyObject(const mirror::Object* o) {
    if (o != NULL && this != NULL && verify_object_mode_ > kNoHeapVerification) {
      VerifyObjectImpl(o);
    }
  }

  // Check sanity of all live references.
  void VerifyHeap() LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
  bool VerifyHeapReferences()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);
  bool VerifyMissingCardMarks()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // A weaker test than IsLiveObject or VerifyObject that doesn't require the heap lock,
  // and doesn't abort on error, allowing the caller to report more
  // meaningful diagnostics.
  bool IsHeapAddress(const mirror::Object* obj);

  // Returns true if 'obj' is a live heap object, false otherwise (including for invalid addresses).
  // Requires the heap lock to be held.
  bool IsLiveObjectLocked(const mirror::Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Initiates an explicit garbage collection.
  void CollectGarbage(bool clear_soft_references) LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Does a concurrent GC, should only be called by the GC daemon thread
  // through runtime.
  void ConcurrentGC(Thread* self) LOCKS_EXCLUDED(Locks::runtime_shutdown_lock_);

  // Implements VMDebug.countInstancesOfClass and JDWP VM_InstanceCount.
  // The boolean decides whether to use IsAssignableFrom or == when comparing classes.
  void CountInstances(const std::vector<mirror::Class*>& classes, bool use_is_assignable_from,
                      uint64_t* counts)
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  // Implements JDWP RT_Instances.
  void GetInstances(mirror::Class* c, int32_t max_count, std::vector<mirror::Object*>& instances)
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  // Implements JDWP OR_ReferringObjects.
  void GetReferringObjects(mirror::Object* o, int32_t max_count, std::vector<mirror::Object*>& referring_objects)
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Removes the growth limit on the alloc space so it may grow to its maximum capacity. Used to
  // implement dalvik.system.VMRuntime.clearGrowthLimit.
  void ClearGrowthLimit();

  // Target ideal heap utilization ratio, implements
  // dalvik.system.VMRuntime.getTargetHeapUtilization.
  double GetTargetHeapUtilization() const {
    return target_utilization_;
  }

  // Data structure memory usage tracking.
  void RegisterGCAllocation(size_t bytes);
  void RegisterGCDeAllocation(size_t bytes);

  // Set target ideal heap utilization ratio, implements
  // dalvik.system.VMRuntime.setTargetHeapUtilization.
  void SetTargetHeapUtilization(float target);

  // For the alloc space, sets the maximum number of bytes that the heap is allowed to allocate
  // from the system. Doesn't allow the space to exceed its growth limit.
  void SetIdealFootprint(size_t max_allowed_footprint);

  // Blocks the caller until the garbage collector becomes idle and returns
  // true if we waited for the GC to complete.
  collector::GcType WaitForConcurrentGcToComplete(Thread* self) LOCKS_EXCLUDED(gc_complete_lock_);

  const std::vector<space::ContinuousSpace*>& GetContinuousSpaces() const {
    return continuous_spaces_;
  }

  const std::vector<space::DiscontinuousSpace*>& GetDiscontinuousSpaces() const {
    return discontinuous_spaces_;
  }

  void SetReferenceOffsets(MemberOffset reference_referent_offset,
                           MemberOffset reference_queue_offset,
                           MemberOffset reference_queueNext_offset,
                           MemberOffset reference_pendingNext_offset,
                           MemberOffset finalizer_reference_zombie_offset);

  mirror::Object* GetReferenceReferent(mirror::Object* reference);
  void ClearReferenceReferent(mirror::Object* reference) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Returns true if the reference object has not yet been enqueued.
  bool IsEnqueuable(const mirror::Object* ref);
  void EnqueueReference(mirror::Object* ref, mirror::Object** list) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void EnqueuePendingReference(mirror::Object* ref, mirror::Object** list)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::Object* DequeuePendingReference(mirror::Object** list) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  MemberOffset GetReferencePendingNextOffset() {
    DCHECK_NE(reference_pendingNext_offset_.Uint32Value(), 0U);
    return reference_pendingNext_offset_;
  }

  MemberOffset GetFinalizerReferenceZombieOffset() {
    DCHECK_NE(finalizer_reference_zombie_offset_.Uint32Value(), 0U);
    return finalizer_reference_zombie_offset_;
  }

  // Enable verification of object references when the runtime is sufficiently initialized.
  void EnableObjectValidation() {
    verify_object_mode_ = kDesiredHeapVerification;
    if (verify_object_mode_ > kNoHeapVerification) {
      VerifyHeap();
    }
  }

  // Disable object reference verification for image writing.
  void DisableObjectValidation() {
    verify_object_mode_ = kHeapVerificationNotPermitted;
  }

  // Other checks may be performed if we know the heap should be in a sane state.
  bool IsObjectValidationEnabled() const {
    return kDesiredHeapVerification > kNoHeapVerification &&
        verify_object_mode_ > kHeapVerificationNotPermitted;
  }

  void RecordFree(size_t freed_objects, size_t freed_bytes);

  // Must be called if a field of an Object in the heap changes, and before any GC safe-point.
  // The call is not needed if NULL is stored in the field.
  void WriteBarrierField(const mirror::Object* dst, MemberOffset /*offset*/, const mirror::Object* /*new_value*/) {
    card_table_->MarkCard(dst);
  }

  // Write barrier for array operations that update many field positions
  void WriteBarrierArray(const mirror::Object* dst, int /*start_offset*/,
                         size_t /*length TODO: element_count or byte_count?*/) {
    card_table_->MarkCard(dst);
  }

  accounting::CardTable* GetCardTable() const {
    return card_table_.get();
  }

  void AddFinalizerReference(Thread* self, mirror::Object* object);

  // Returns the number of bytes currently allocated.
  size_t GetBytesAllocated() const {
    return num_bytes_allocated_;
  }

  // Returns the number of objects currently allocated.
  size_t GetObjectsAllocated() const;

  // Returns the total number of objects allocated since the heap was created.
  size_t GetObjectsAllocatedEver() const;

  // Returns the total number of bytes allocated since the heap was created.
  size_t GetBytesAllocatedEver() const;

  // Returns the total number of objects freed since the heap was created.
  size_t GetObjectsFreedEver() const {
    return total_objects_freed_ever_;
  }

  // Returns the total number of bytes freed since the heap was created.
  size_t GetBytesFreedEver() const {
    return total_bytes_freed_ever_;
  }

  // Implements java.lang.Runtime.maxMemory, returning the maximum amount of memory a program can
  // consume. For a regular VM this would relate to the -Xmx option and would return -1 if no Xmx
  // were specified. Android apps start with a growth limit (small heap size) which is
  // cleared/extended for large apps.
  int64_t GetMaxMemory() const {
    return growth_limit_;
  }

  // Implements java.lang.Runtime.totalMemory, returning the amount of memory consumed by an
  // application.
  int64_t GetTotalMemory() const {
    // TODO: we use the footprint limit here which is conservative wrt number of pages really used.
    //       We could implement a more accurate count across all spaces.
    return max_allowed_footprint_;
  }

  // Implements java.lang.Runtime.freeMemory.
  int64_t GetFreeMemory() const {
    return GetTotalMemory() - num_bytes_allocated_;
  }

  // Get the space that corresponds to an object's address. Current implementation searches all
  // spaces in turn. If fail_ok is false then failing to find a space will cause an abort.
  // TODO: consider using faster data structure like binary tree.
  space::ContinuousSpace* FindContinuousSpaceFromObject(const mirror::Object*, bool fail_ok) const;
  space::DiscontinuousSpace* FindDiscontinuousSpaceFromObject(const mirror::Object*,
                                                              bool fail_ok) const;
  space::Space* FindSpaceFromObject(const mirror::Object*, bool fail_ok) const;

  void DumpForSigQuit(std::ostream& os);

  size_t Trim();

  accounting::HeapBitmap* GetLiveBitmap() SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    return live_bitmap_.get();
  }

  accounting::HeapBitmap* GetMarkBitmap() SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    return mark_bitmap_.get();
  }

  accounting::ObjectStack* GetLiveStack() SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    return live_stack_.get();
  }

  void PreZygoteFork() LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);

  // Mark and empty stack.
  void FlushAllocStack()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Mark all the objects in the allocation stack in the specified bitmap.
  void MarkAllocStack(accounting::SpaceBitmap* bitmap, accounting::SpaceSetMap* large_objects,
                      accounting::ObjectStack* stack)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Unmark all the objects in the allocation stack in the specified bitmap.
  void UnMarkAllocStack(accounting::SpaceBitmap* bitmap, accounting::SpaceSetMap* large_objects,
                        accounting::ObjectStack* stack)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Update and mark mod union table based on gc type.
  void UpdateAndMarkModUnion(collector::MarkSweep* mark_sweep, base::NewTimingLogger& timings,
                             collector::GcType gc_type)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Update process state to let the heap know which type of GC to do.
  void UpdateProcessState(ProcessState process_state);

  // DEPRECATED: Should remove in "near" future when support for multiple image spaces is added.
  // Assumes there is only one image space.
  space::ImageSpace* GetImageSpace() const;

  space::DlMallocSpace* GetAllocSpace() const {
    return alloc_space_;
  }

  space::LargeObjectSpace* GetLargeObjectsSpace() const {
    return large_object_space_;
  }

  void DumpSpaces();

  // GC performance measuring
  void DumpGcPerformanceInfo(std::ostream& os);

  // Thread pool.
  void CreateThreadPool();
  void DeleteThreadPool();
  ThreadPool* GetThreadPool() {
    return thread_pool_.get();
  }

 private:
  // Allocates uninitialized storage. Passing in a null space tries to place the object in the
  // large object space.
  mirror::Object* Allocate(Thread* self, space::AllocSpace* space, size_t num_bytes)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Try to allocate a number of bytes, this function never does any GCs.
  mirror::Object* TryToAllocate(Thread* self, space::AllocSpace* space, size_t alloc_size, bool grow)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Pushes a list of cleared references out to the managed heap.
  void EnqueueClearedReferences(mirror::Object** cleared_references);

  void RequestHeapTrim() LOCKS_EXCLUDED(Locks::runtime_shutdown_lock_);
  void RequestConcurrentGC(Thread* self) LOCKS_EXCLUDED(Locks::runtime_shutdown_lock_);
  bool IsGCRequestPending() const;

  void RecordAllocation(size_t size, mirror::Object* object)
      LOCKS_EXCLUDED(GlobalSynchronization::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Sometimes CollectGarbageInternal decides to run a different Gc than you requested. Returns
  // which type of Gc was actually ran.
  collector::GcType CollectGarbageInternal(collector::GcType gc_plan, GcCause gc_cause,
                                           bool clear_soft_references)
      LOCKS_EXCLUDED(gc_complete_lock_,
                     Locks::heap_bitmap_lock_,
                     Locks::thread_suspend_count_lock_);

  void PreGcVerification(collector::GarbageCollector* gc);
  void PreSweepingGcVerification(collector::GarbageCollector* gc)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void PostGcVerification(collector::GarbageCollector* gc);

  // Update the watermark for the native allocated bytes based on the current number of native
  // bytes allocated and the target utilization ratio.
  void UpdateMaxNativeFootprint();

  // Given the current contents of the alloc space, increase the allowed heap footprint to match
  // the target utilization ratio.  This should only be called immediately after a full garbage
  // collection.
  void GrowForUtilization(collector::GcType gc_type, uint64_t gc_duration);

  size_t GetPercentFree();

  void AddContinuousSpace(space::ContinuousSpace* space) LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
  void AddDiscontinuousSpace(space::DiscontinuousSpace* space)
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);

  // No thread saftey analysis since we call this everywhere and it is impossible to find a proper
  // lock ordering for it.
  void VerifyObjectBody(const mirror::Object *obj) NO_THREAD_SAFETY_ANALYSIS;

  static void VerificationCallback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(GlobalSychronization::heap_bitmap_lock_);

  // Swap the allocation stack with the live stack.
  void SwapStacks();

  // Clear cards and update the mod union table.
  void ProcessCards(base::NewTimingLogger& timings);

  // All-known continuous spaces, where objects lie within fixed bounds.
  std::vector<space::ContinuousSpace*> continuous_spaces_;

  // All-known discontinuous spaces, where objects may be placed throughout virtual memory.
  std::vector<space::DiscontinuousSpace*> discontinuous_spaces_;

  // The allocation space we are currently allocating into.
  space::DlMallocSpace* alloc_space_;

  // The large object space we are currently allocating into.
  space::LargeObjectSpace* large_object_space_;

  // The card table, dirtied by the write barrier.
  UniquePtr<accounting::CardTable> card_table_;

  // The mod-union table remembers all of the references from the image space to the alloc /
  // zygote spaces to allow the card table to be cleared.
  UniquePtr<accounting::ModUnionTable> image_mod_union_table_;

  // This table holds all of the references from the zygote space to the alloc space.
  UniquePtr<accounting::ModUnionTable> zygote_mod_union_table_;

  // What kind of concurrency behavior is the runtime after? True for concurrent mark sweep GC,
  // false for stop-the-world mark sweep.
  const bool concurrent_gc_;

  // How many GC threads we may use for garbage collection.
  const bool num_gc_threads_;

  // If we have a zygote space.
  bool have_zygote_space_;

  // Guards access to the state of GC, associated conditional variable is used to signal when a GC
  // completes.
  Mutex* gc_complete_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  UniquePtr<ConditionVariable> gc_complete_cond_ GUARDED_BY(gc_complete_lock_);

  // Mutex held when adding references to reference queues.
  // TODO: move to a UniquePtr, currently annotalysis is confused that UniquePtr isn't lockable.
  Mutex* reference_queue_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;

  // True while the garbage collector is running.
  volatile bool is_gc_running_ GUARDED_BY(gc_complete_lock_);

  // Last Gc type we ran. Used by WaitForConcurrentGc to know which Gc was waited on.
  volatile collector::GcType last_gc_type_ GUARDED_BY(gc_complete_lock_);
  collector::GcType next_gc_type_;

  // Maximum size that the heap can reach.
  const size_t capacity_;
  // The size the heap is limited to. This is initially smaller than capacity, but for largeHeap
  // programs it is "cleared" making it the same as capacity.
  size_t growth_limit_;
  // When the number of bytes allocated exceeds the footprint TryAllocate returns NULL indicating
  // a GC should be triggered.
  size_t max_allowed_footprint_;
  // The watermark at which a concurrent GC is requested by registerNativeAllocation.
  size_t native_footprint_gc_watermark_;
  // The watermark at which a GC is performed inside of registerNativeAllocation.
  size_t native_footprint_limit_;

  // When num_bytes_allocated_ exceeds this amount then a concurrent GC should be requested so that
  // it completes ahead of an allocation failing.
  size_t concurrent_start_bytes_;

  // Since the heap was created, how many bytes have been freed.
  size_t total_bytes_freed_ever_;

  // Since the heap was created, how many objects have been freed.
  size_t total_objects_freed_ever_;

  // Primitive objects larger than this size are put in the large object space.
  const size_t large_object_threshold_;

  // Number of bytes allocated.  Adjusted after each allocation and free.
  AtomicInteger num_bytes_allocated_;

  // Bytes which are allocated and managed by native code but still need to be accounted for.
  AtomicInteger native_bytes_allocated_;

  // Current process state, updated by activity manager.
  ProcessState process_state_;

  // Data structure GC overhead.
  AtomicInteger gc_memory_overhead_;

  // Heap verification flags.
  const bool verify_missing_card_marks_;
  const bool verify_system_weaks_;
  const bool verify_pre_gc_heap_;
  const bool verify_post_gc_heap_;
  const bool verify_mod_union_table_;

  // Parallel GC data structures.
  UniquePtr<ThreadPool> thread_pool_;

  // Sticky mark bits GC has some overhead, so if we have less a few megabytes of AllocSpace then
  // it's probably better to just do a partial GC.
  const size_t min_alloc_space_size_for_sticky_gc_;

  // Minimum remaining size for sticky GC. Since sticky GC doesn't free up as much memory as a
  // normal GC, it is important to not use it when we are almost out of memory.
  const size_t min_remaining_space_for_sticky_gc_;

  // The last time a heap trim occurred.
  uint64_t last_trim_time_ms_;

  // The nanosecond time at which the last GC ended.
  uint64_t last_gc_time_ns_;

  // How many bytes were allocated at the end of the last GC.
  uint64_t last_gc_size_;

  // Estimated allocation rate (bytes / second). Computed between the time of the last GC cycle
  // and the start of the current one.
  uint64_t allocation_rate_;

  // For a GC cycle, a bitmap that is set corresponding to the
  UniquePtr<accounting::HeapBitmap> live_bitmap_ GUARDED_BY(Locks::heap_bitmap_lock_);
  UniquePtr<accounting::HeapBitmap> mark_bitmap_ GUARDED_BY(Locks::heap_bitmap_lock_);

  // Mark stack that we reuse to avoid re-allocating the mark stack.
  UniquePtr<accounting::ObjectStack> mark_stack_;

  // Allocation stack, new allocations go here so that we can do sticky mark bits. This enables us
  // to use the live bitmap as the old mark bitmap.
  const size_t max_allocation_stack_size_;
  bool is_allocation_stack_sorted_;
  UniquePtr<accounting::ObjectStack> allocation_stack_;

  // Second allocation stack so that we can process allocation with the heap unlocked.
  UniquePtr<accounting::ObjectStack> live_stack_;

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

  // Minimum free guarantees that you always have at least min_free_ free bytes after growing for
  // utilization, regardless of target utilization ratio.
  size_t min_free_;

  // The ideal maximum free size, when we grow the heap for utilization.
  size_t max_free_;

  // Target ideal heap utilization ratio
  double target_utilization_;

  // Total time which mutators are paused or waiting for GC to complete.
  uint64_t total_wait_time_;

  // Total number of objects allocated in microseconds.
  const bool measure_allocation_time_;
  AtomicInteger total_allocation_time_;

  // The current state of heap verification, may be enabled or disabled.
  HeapVerificationMode verify_object_mode_;

  std::vector<collector::MarkSweep*> mark_sweep_collectors_;

  friend class collector::MarkSweep;
  friend class VerifyReferenceCardVisitor;
  friend class VerifyReferenceVisitor;
  friend class VerifyObjectVisitor;
  friend class ScopedHeapLock;
  friend class space::SpaceTest;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
};

}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_HEAP_H_
