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
#include "jni.h"
#include "locks.h"
#include "offsets.h"
#include "reference_queue.h"
#include "root_visitor.h"
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
  class SemiSpace;
}  // namespace collector

namespace space {
  class AllocSpace;
  class BumpPointerSpace;
  class DiscontinuousSpace;
  class DlMallocSpace;
  class ImageSpace;
  class LargeObjectSpace;
  class MallocSpace;
  class RosAllocSpace;
  class Space;
  class SpaceTest;
  class ContinuousMemMapAllocSpace;
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

// Different types of allocators.
enum AllocatorType {
  kAllocatorTypeBumpPointer,
  kAllocatorTypeFreeList,  // ROSAlloc / dlmalloc
  kAllocatorTypeLOS,  // Large object space.
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
static constexpr HeapVerificationMode kDesiredHeapVerification = kNoHeapVerification;

// If true, use rosalloc/RosAllocSpace instead of dlmalloc/DlMallocSpace
static constexpr bool kUseRosAlloc = true;

class Heap {
 public:
  // If true, measure the total allocation time.
  static constexpr bool kMeasureAllocationTime = false;
  // Primitive arrays larger than this size are put in the large object space.
  static constexpr size_t kLargeObjectThreshold = 3 * kPageSize;

  static constexpr size_t kDefaultInitialSize = 2 * MB;
  static constexpr size_t kDefaultMaximumSize = 32 * MB;
  static constexpr size_t kDefaultMaxFree = 2 * MB;
  static constexpr size_t kDefaultMinFree = kDefaultMaxFree / 4;
  static constexpr size_t kDefaultLongPauseLogThreshold = MsToNs(5);
  static constexpr size_t kDefaultLongGCLogThreshold = MsToNs(100);

  // Default target utilization.
  static constexpr double kDefaultTargetUtilization = 0.5;

  // Used so that we don't overflow the allocation time atomic integer.
  static constexpr size_t kTimeAdjust = 1024;

  // Create a heap with the requested sizes. The possible empty
  // image_file_names names specify Spaces to load based on
  // ImageWriter output.
  explicit Heap(size_t initial_size, size_t growth_limit, size_t min_free,
                size_t max_free, double target_utilization, size_t capacity,
                const std::string& original_image_file_name, bool concurrent_gc,
                size_t parallel_gc_threads, size_t conc_gc_threads, bool low_memory_mode,
                size_t long_pause_threshold, size_t long_gc_threshold, bool ignore_max_footprint);

  ~Heap();

  // Allocates and initializes storage for an object instance.
  template <const bool kInstrumented>
  inline mirror::Object* AllocObject(Thread* self, mirror::Class* klass, size_t num_bytes)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return AllocObjectWithAllocator<kInstrumented>(self, klass, num_bytes, GetCurrentAllocator());
  }
  template <const bool kInstrumented>
  inline mirror::Object* AllocNonMovableObject(Thread* self, mirror::Class* klass,
                                               size_t num_bytes)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return AllocObjectWithAllocator<kInstrumented>(self, klass, num_bytes,
                                                   GetCurrentNonMovingAllocator());
  }
  template <bool kInstrumented, typename PreFenceVisitor = VoidFunctor>
  ALWAYS_INLINE mirror::Object* AllocObjectWithAllocator(
      Thread* self, mirror::Class* klass, size_t byte_count, AllocatorType allocator,
      const PreFenceVisitor& pre_fence_visitor = VoidFunctor())
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  AllocatorType GetCurrentAllocator() const {
    return current_allocator_;
  }

  AllocatorType GetCurrentNonMovingAllocator() const {
    return current_non_moving_allocator_;
  }

  // Visit all of the live objects in the heap.
  void VisitObjects(ObjectVisitorCallback callback, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  void SwapSemiSpaces() EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);

  void DebugCheckPreconditionsForAllocObject(mirror::Class* c, size_t byte_count)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void ThrowOutOfMemoryError(size_t byte_count, bool large_object_allocation);

  void RegisterNativeAllocation(JNIEnv* env, int bytes);
  void RegisterNativeFree(JNIEnv* env, int bytes);

  // Change the allocator, updates entrypoints.
  void ChangeAllocator(AllocatorType allocator);

  // The given reference is believed to be to an object in the Java heap, check the soundness of it.
  void VerifyObjectImpl(const mirror::Object* o);
  void VerifyObject(const mirror::Object* o) {
    if (o != nullptr && this != nullptr && verify_object_mode_ > kNoHeapVerification) {
      VerifyObjectImpl(o);
    }
  }

  // Check sanity of all live references.
  void VerifyHeap() LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);
  bool VerifyHeapReferences()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool VerifyMissingCardMarks()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // A weaker test than IsLiveObject or VerifyObject that doesn't require the heap lock,
  // and doesn't abort on error, allowing the caller to report more
  // meaningful diagnostics.
  bool IsValidObjectAddress(const mirror::Object* obj) const;

  // Returns true if the address passed in is a heap address, doesn't need to be aligned.
  bool IsHeapAddress(const mirror::Object* obj) const;

  // Returns true if 'obj' is a live heap object, false otherwise (including for invalid addresses).
  // Requires the heap lock to be held.
  bool IsLiveObjectLocked(const mirror::Object* obj, bool search_allocation_stack = true,
                          bool search_live_stack = true, bool sorted = false)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Returns true if there is any chance that the object (obj) will move.
  bool IsMovableObject(const mirror::Object* obj) const;

  // Returns true if an object is in the temp space, if this happens its usually indicative of
  // compaction related errors.
  bool IsInTempSpace(const mirror::Object* obj) const;

  // Enables us to prevent GC until objects are released.
  void IncrementDisableGC(Thread* self);
  void DecrementDisableGC(Thread* self);

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

  // Blocks the caller until the garbage collector becomes idle and returns the type of GC we
  // waited for.
  collector::GcType WaitForGcToComplete(Thread* self) LOCKS_EXCLUDED(gc_complete_lock_);

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
  MemberOffset GetReferenceReferentOffset() const {
    return reference_referent_offset_;
  }
  MemberOffset GetReferenceQueueOffset() const {
    return reference_queue_offset_;
  }
  MemberOffset GetReferenceQueueNextOffset() const {
    return reference_queueNext_offset_;
  }
  MemberOffset GetReferencePendingNextOffset() const {
    return reference_pendingNext_offset_;
  }
  MemberOffset GetFinalizerReferenceZombieOffset() const {
    return finalizer_reference_zombie_offset_;
  }
  static mirror::Object* PreserveSoftReferenceCallback(mirror::Object* obj, void* arg);
  void ProcessReferences(TimingLogger& timings, bool clear_soft, RootVisitor* is_marked_callback,
                         RootVisitor* recursive_mark_object_callback, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

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

  // Returns true if low memory mode is enabled.
  bool IsLowMemoryMode() const {
    return low_memory_mode_;
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

  void WriteBarrierEveryFieldOf(const mirror::Object* obj) {
    card_table_->MarkCard(obj);
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
  size_t GetObjectsAllocated() const LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);

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
  int64_t GetTotalMemory() const;

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

  // Trim the managed and native heaps by releasing unused memory back to the OS.
  void Trim();

  void RevokeThreadLocalBuffers(Thread* thread);
  void RevokeAllThreadLocalBuffers();

  accounting::HeapBitmap* GetLiveBitmap() SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    return live_bitmap_.get();
  }

  accounting::HeapBitmap* GetMarkBitmap() SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    return mark_bitmap_.get();
  }

  accounting::ObjectStack* GetLiveStack() SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    return live_stack_.get();
  }

  void PreZygoteFork() NO_THREAD_SAFETY_ANALYSIS;

  // Mark and empty stack.
  void FlushAllocStack()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Mark all the objects in the allocation stack in the specified bitmap.
  void MarkAllocStack(accounting::SpaceBitmap* bitmap, accounting::SpaceSetMap* large_objects,
                      accounting::ObjectStack* stack)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Mark the specified allocation stack as live.
  void MarkAllocStackAsLive(accounting::ObjectStack* stack)
        EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Gets called when we get notified by ActivityThread that the process state has changed.
  void ListenForProcessStateChange();

  // DEPRECATED: Should remove in "near" future when support for multiple image spaces is added.
  // Assumes there is only one image space.
  space::ImageSpace* GetImageSpace() const;

  space::MallocSpace* GetNonMovingSpace() const {
    return non_moving_space_;
  }

  space::LargeObjectSpace* GetLargeObjectsSpace() const {
    return large_object_space_;
  }

  void DumpSpaces(std::ostream& stream = LOG(INFO));

  // GC performance measuring
  void DumpGcPerformanceInfo(std::ostream& os);

  // Returns true if we currently care about pause times.
  bool CareAboutPauseTimes() const {
    return care_about_pause_times_;
  }

  // Thread pool.
  void CreateThreadPool();
  void DeleteThreadPool();
  ThreadPool* GetThreadPool() {
    return thread_pool_.get();
  }
  size_t GetParallelGCThreadCount() const {
    return parallel_gc_threads_;
  }
  size_t GetConcGCThreadCount() const {
    return conc_gc_threads_;
  }
  accounting::ModUnionTable* FindModUnionTableFromSpace(space::Space* space);
  void AddModUnionTable(accounting::ModUnionTable* mod_union_table);

  bool IsCompilingBoot() const;
  bool HasImageSpace() const;

 private:
  void Compact(space::ContinuousMemMapAllocSpace* target_space,
               space::ContinuousMemMapAllocSpace* source_space);

  static bool AllocatorHasAllocationStack(AllocatorType allocator_type) {
    return allocator_type != kAllocatorTypeBumpPointer;
  }
  static bool AllocatorHasConcurrentGC(AllocatorType allocator_type) {
    return allocator_type != kAllocatorTypeBumpPointer;
  }
  bool ShouldAllocLargeObject(mirror::Class* c, size_t byte_count) const;
  ALWAYS_INLINE void CheckConcurrentGC(Thread* self, size_t new_num_bytes_allocated,
                                       mirror::Object* obj);

  // Handles Allocate()'s slow allocation path with GC involved after
  // an initial allocation attempt failed.
  mirror::Object* AllocateInternalWithGc(Thread* self, AllocatorType allocator, size_t num_bytes,
                                         size_t* bytes_allocated)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Allocate into a specific space.
  mirror::Object* AllocateInto(Thread* self, space::AllocSpace* space, mirror::Class* c,
                               size_t bytes)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Try to allocate a number of bytes, this function never does any GCs. Needs to be inlined so
  // that the switch statement is constant optimized in the entrypoints.
  template <const bool kInstrumented>
  ALWAYS_INLINE mirror::Object* TryToAllocate(Thread* self, AllocatorType allocator_type,
                                              size_t alloc_size, bool grow,
                                              size_t* bytes_allocated)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void ThrowOutOfMemoryError(Thread* self, size_t byte_count, bool large_object_allocation)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool IsOutOfMemoryOnAllocation(size_t alloc_size, bool grow);

  // Pushes a list of cleared references out to the managed heap.
  void SetReferenceReferent(mirror::Object* reference, mirror::Object* referent)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::Object* GetReferenceReferent(mirror::Object* reference)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void ClearReferenceReferent(mirror::Object* reference)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    SetReferenceReferent(reference, nullptr);
  }
  void EnqueueClearedReferences();
  // Returns true if the reference object has not yet been enqueued.
  bool IsEnqueuable(const mirror::Object* ref) const;
  bool IsEnqueued(mirror::Object* ref) const;
  void DelayReferenceReferent(mirror::Class* klass, mirror::Object* obj, RootVisitor mark_visitor,
                              void* arg) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Run the finalizers.
  void RunFinalization(JNIEnv* env);

  // Blocks the caller until the garbage collector becomes idle and returns the type of GC we
  // waited for.
  collector::GcType WaitForGcToCompleteLocked(Thread* self)
      EXCLUSIVE_LOCKS_REQUIRED(gc_complete_lock_);

  void RequestHeapTrim() LOCKS_EXCLUDED(Locks::runtime_shutdown_lock_);
  void RequestConcurrentGC(Thread* self) LOCKS_EXCLUDED(Locks::runtime_shutdown_lock_);
  bool IsGCRequestPending() const;

  size_t RecordAllocationInstrumented(size_t size, mirror::Object* object)
      LOCKS_EXCLUDED(GlobalSynchronization::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  size_t RecordAllocationUninstrumented(size_t size, mirror::Object* object)
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
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
  void PostGcVerification(collector::GarbageCollector* gc)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Update the watermark for the native allocated bytes based on the current number of native
  // bytes allocated and the target utilization ratio.
  void UpdateMaxNativeFootprint();

  // Given the current contents of the alloc space, increase the allowed heap footprint to match
  // the target utilization ratio.  This should only be called immediately after a full garbage
  // collection.
  void GrowForUtilization(collector::GcType gc_type, uint64_t gc_duration);

  size_t GetPercentFree();

  void AddSpace(space::Space* space) LOCKS_EXCLUDED(Locks::heap_bitmap_lock_);

  // No thread saftey analysis since we call this everywhere and it is impossible to find a proper
  // lock ordering for it.
  void VerifyObjectBody(const mirror::Object *obj) NO_THREAD_SAFETY_ANALYSIS;

  static void VerificationCallback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(GlobalSychronization::heap_bitmap_lock_);

  // Swap the allocation stack with the live stack.
  void SwapStacks();

  // Clear cards and update the mod union table.
  void ProcessCards(TimingLogger& timings);

  // All-known continuous spaces, where objects lie within fixed bounds.
  std::vector<space::ContinuousSpace*> continuous_spaces_;

  // All-known discontinuous spaces, where objects may be placed throughout virtual memory.
  std::vector<space::DiscontinuousSpace*> discontinuous_spaces_;

  // All-known alloc spaces, where objects may be or have been allocated.
  std::vector<space::AllocSpace*> alloc_spaces_;

  // A space where non-movable objects are allocated, when compaction is enabled it contains
  // Classes, ArtMethods, ArtFields, and non moving objects.
  space::MallocSpace* non_moving_space_;

  // The large object space we are currently allocating into.
  space::LargeObjectSpace* large_object_space_;

  // The card table, dirtied by the write barrier.
  UniquePtr<accounting::CardTable> card_table_;

  // A mod-union table remembers all of the references from the it's space to other spaces.
  SafeMap<space::Space*, accounting::ModUnionTable*> mod_union_tables_;

  // What kind of concurrency behavior is the runtime after? True for concurrent mark sweep GC,
  // false for stop-the-world mark sweep.
  bool concurrent_gc_;

  // How many GC threads we may use for paused parts of garbage collection.
  const size_t parallel_gc_threads_;

  // How many GC threads we may use for unpaused parts of garbage collection.
  const size_t conc_gc_threads_;

  // Boolean for if we are in low memory mode.
  const bool low_memory_mode_;

  // If we get a pause longer than long pause log threshold, then we print out the GC after it
  // finishes.
  const size_t long_pause_log_threshold_;

  // If we get a GC longer than long GC log threshold, then we print out the GC after it finishes.
  const size_t long_gc_log_threshold_;

  // If we ignore the max footprint it lets the heap grow until it hits the heap capacity, this is
  // useful for benchmarking since it reduces time spent in GC to a low %.
  const bool ignore_max_footprint_;

  // If we have a zygote space.
  bool have_zygote_space_;

  // Number of pinned primitive arrays in the movable space.
  // Block all GC until this hits zero, or we hit the timeout!
  size_t number_gc_blockers_;
  static constexpr size_t KGCBlockTimeout = 30000;

  // Guards access to the state of GC, associated conditional variable is used to signal when a GC
  // completes.
  Mutex* gc_complete_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  UniquePtr<ConditionVariable> gc_complete_cond_ GUARDED_BY(gc_complete_lock_);

  // Reference queues.
  ReferenceQueue soft_reference_queue_;
  ReferenceQueue weak_reference_queue_;
  ReferenceQueue finalizer_reference_queue_;
  ReferenceQueue phantom_reference_queue_;
  ReferenceQueue cleared_references_;

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

  // Whether or not we need to run finalizers in the next native allocation.
  bool native_need_to_run_finalization_;

  // Activity manager members.
  jclass activity_thread_class_;
  jclass application_thread_class_;
  jobject activity_thread_;
  jobject application_thread_;
  jfieldID last_process_state_id_;

  // Process states which care about pause times.
  std::set<int> process_state_cares_about_pause_time_;

  // Whether or not we currently care about pause times.
  bool care_about_pause_times_;

  // When num_bytes_allocated_ exceeds this amount then a concurrent GC should be requested so that
  // it completes ahead of an allocation failing.
  size_t concurrent_start_bytes_;

  // Since the heap was created, how many bytes have been freed.
  size_t total_bytes_freed_ever_;

  // Since the heap was created, how many objects have been freed.
  size_t total_objects_freed_ever_;

  // Number of bytes allocated.  Adjusted after each allocation and free.
  AtomicInteger num_bytes_allocated_;

  // Bytes which are allocated and managed by native code but still need to be accounted for.
  AtomicInteger native_bytes_allocated_;

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
  UniquePtr<accounting::ObjectStack> allocation_stack_;

  // Second allocation stack so that we can process allocation with the heap unlocked.
  UniquePtr<accounting::ObjectStack> live_stack_;

  // Allocator type.
  AllocatorType current_allocator_;
  const AllocatorType current_non_moving_allocator_;

  // Which GCs we run in order when we an allocation fails.
  std::vector<collector::GcType> gc_plan_;

  // Bump pointer spaces.
  space::BumpPointerSpace* bump_pointer_space_;
  // Temp space is the space which the semispace collector copies to.
  space::BumpPointerSpace* temp_space_;

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
  AtomicInteger total_allocation_time_;

  // The current state of heap verification, may be enabled or disabled.
  HeapVerificationMode verify_object_mode_;

  // GC disable count, error on GC if > 0.
  size_t gc_disable_count_ GUARDED_BY(gc_complete_lock_);

  std::vector<collector::GarbageCollector*> garbage_collectors_;
  collector::SemiSpace* semi_space_collector_;

  const bool running_on_valgrind_;

  friend class collector::MarkSweep;
  friend class collector::SemiSpace;
  friend class ReferenceQueue;
  friend class VerifyReferenceCardVisitor;
  friend class VerifyReferenceVisitor;
  friend class VerifyObjectVisitor;
  friend class ScopedHeapLock;
  friend class space::SpaceTest;

  class AllocationTimer {
   private:
    Heap* heap_;
    mirror::Object** allocated_obj_ptr_;
    uint64_t allocation_start_time_;
   public:
    AllocationTimer(Heap* heap, mirror::Object** allocated_obj_ptr);
    ~AllocationTimer();
  };

  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
};

}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_HEAP_H_
