/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef ART_RUNTIME_GC_COLLECTOR_MARK_SWEEP_H_
#define ART_RUNTIME_GC_COLLECTOR_MARK_SWEEP_H_

#include "atomic.h"
#include "barrier.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "garbage_collector.h"
#include "object_callbacks.h"
#include "offsets.h"
#include "UniquePtr.h"

namespace art {

namespace mirror {
  class Class;
  class Object;
  template<class T> class ObjectArray;
}  // namespace mirror

class StackVisitor;
class Thread;

namespace gc {

namespace accounting {
  template <typename T> class AtomicStack;
  class MarkIfReachesAllocspaceVisitor;
  class ModUnionClearCardVisitor;
  class ModUnionVisitor;
  class ModUnionTableBitmap;
  class MarkStackChunk;
  typedef AtomicStack<mirror::Object*> ObjectStack;
  class SpaceBitmap;
}  // namespace accounting

namespace space {
  class ContinuousSpace;
}  // namespace space

class Heap;

namespace collector {

class MarkSweep : public GarbageCollector {
 public:
  explicit MarkSweep(Heap* heap, bool is_concurrent, const std::string& name_prefix = "");

  ~MarkSweep() {}

  virtual void InitializePhase();
  virtual bool IsConcurrent() const;
  virtual bool HandleDirtyObjectsPhase() EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual void MarkingPhase() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual void ReclaimPhase() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual void FinishPhase() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual void MarkReachableObjects()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);
  virtual GcType GetGcType() const {
    return kGcTypeFull;
  }

  // Initializes internal structures.
  void Init();

  // Find the default mark bitmap.
  void FindDefaultMarkBitmap();

  // Marks the root set at the start of a garbage collection.
  void MarkRoots()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void MarkNonThreadRoots()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void MarkConcurrentRoots()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void MarkRootsCheckpoint(Thread* self)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Builds a mark stack and recursively mark until it empties.
  void RecursiveMark()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Make a space immune, immune spaces have all live objects marked - that is the mark and
  // live bitmaps are bound together.
  void ImmuneSpace(space::ContinuousSpace* space)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsImmuneSpace(const space::ContinuousSpace* space) const;
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Bind the live bits to the mark bits of bitmaps for spaces that are never collected, ie
  // the image. Mark that portion of the heap as immune.
  virtual void BindBitmaps() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Builds a mark stack with objects on dirty cards and recursively mark until it empties.
  void RecursiveMarkDirtyObjects(bool paused, byte minimum_age)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Remarks the root set after completing the concurrent mark.
  void ReMarkRoots()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void ProcessReferences(Thread* self)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Update and mark references from immune spaces.
  virtual void UpdateAndMarkModUnion()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Pre clean cards to reduce how much work is needed in the pause.
  void PreCleanCards()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Sweeps unmarked objects to complete the garbage collection.
  virtual void Sweep(bool swap_bitmaps) EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Sweeps unmarked objects to complete the garbage collection.
  void SweepLargeObjects(bool swap_bitmaps) EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Sweep only pointers within an array. WARNING: Trashes objects.
  void SweepArray(accounting::ObjectStack* allocation_stack_, bool swap_bitmaps)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Blackens an object.
  void ScanObject(mirror::Object* obj)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // TODO: enable thread safety analysis when in use by multiple worker threads.
  template <typename MarkVisitor>
  void ScanObjectVisit(mirror::Object* obj, const MarkVisitor& visitor)
      NO_THREAD_SAFETY_ANALYSIS;

  // Everything inside the immune range is assumed to be marked.
  void SetImmuneRange(mirror::Object* begin, mirror::Object* end);

  void SweepSystemWeaks()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_);

  static mirror::Object* VerifySystemWeakIsLiveCallback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  void VerifySystemWeaks()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_);

  // Verify that an object is live, either in a live bitmap or in the allocation stack.
  void VerifyIsLive(const mirror::Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  template <typename Visitor>
  static void VisitObjectReferences(mirror::Object* obj, const Visitor& visitor, bool visit_class)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_,
                            Locks::mutator_lock_);

  static mirror::Object* MarkObjectCallback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  static void MarkRootCallback(mirror::Object** root, void* arg, uint32_t thread_id,
                               RootType root_type)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  static void ProcessMarkStackPausedCallback(void* arg)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  static void MarkRootParallelCallback(mirror::Object** root, void* arg, uint32_t thread_id,
                                       RootType root_type)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Marks an object.
  void MarkObject(const mirror::Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  void MarkRoot(const mirror::Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  Barrier& GetBarrier() {
    return *gc_barrier_;
  }

 protected:
  // Returns true if the object has its bit set in the mark bitmap.
  bool IsMarked(const mirror::Object* object) const;

  static mirror::Object* IsMarkedCallback(mirror::Object* object, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  static void VerifyImageRootVisitor(mirror::Object* root, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_,
                            Locks::mutator_lock_);

  void MarkObjectNonNull(const mirror::Object* obj)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
        EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Unmarks an object by clearing the bit inside of the corresponding bitmap, or if it is in a
  // space set, removing the object from the set.
  void UnMarkObjectNonNull(const mirror::Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Mark the vm thread roots.
  virtual void MarkThreadRoots(Thread* self)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Marks an object atomically, safe to use from multiple threads.
  void MarkObjectNonNullParallel(const mirror::Object* obj);

  // Marks or unmarks a large object based on whether or not set is true. If set is true, then we
  // mark, otherwise we unmark.
  bool MarkLargeObject(const mirror::Object* obj, bool set)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Returns true if we need to add obj to a mark stack.
  bool MarkObjectParallel(const mirror::Object* obj) NO_THREAD_SAFETY_ANALYSIS;

  // Verify the roots of the heap and print out information related to any invalid roots.
  // Called in MarkObject, so may we may not hold the mutator lock.
  void VerifyRoots()
      NO_THREAD_SAFETY_ANALYSIS;

  // Expand mark stack to 2x its current size.
  void ExpandMarkStack() EXCLUSIVE_LOCKS_REQUIRED(mark_stack_lock_);
  void ResizeMarkStack(size_t new_size) EXCLUSIVE_LOCKS_REQUIRED(mark_stack_lock_);

  // Returns how many threads we should use for the current GC phase based on if we are paused,
  // whether or not we care about pauses.
  size_t GetThreadCount(bool paused) const;

  // Returns true if an object is inside of the immune region (assumed to be marked).
  bool IsImmune(const mirror::Object* obj) const ALWAYS_INLINE {
    return obj >= immune_begin_ && obj < immune_end_;
  }

  static void VerifyRootCallback(const mirror::Object* root, void* arg, size_t vreg,
                                 const StackVisitor *visitor);

  void VerifyRoot(const mirror::Object* root, size_t vreg, const StackVisitor* visitor)
      NO_THREAD_SAFETY_ANALYSIS;

  template <typename Visitor>
  static void VisitInstanceFieldsReferences(mirror::Class* klass, mirror::Object* obj,
                                            const Visitor& visitor)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  // Visit the header, static field references, and interface pointers of a class object.
  template <typename Visitor>
  static void VisitClassReferences(mirror::Class* klass, mirror::Object* obj,
                                   const Visitor& visitor)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  template <typename Visitor>
  static void VisitStaticFieldsReferences(mirror::Class* klass, const Visitor& visitor)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  template <typename Visitor>
  static void VisitFieldsReferences(mirror::Object* obj, uint32_t ref_offsets, bool is_static,
                                    const Visitor& visitor)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  // Visit all of the references in an object array.
  template <typename Visitor>
  static void VisitObjectArrayReferences(mirror::ObjectArray<mirror::Object>* array,
                                         const Visitor& visitor)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  // Visits the header and field references of a data object.
  template <typename Visitor>
  static void VisitOtherReferences(mirror::Class* klass, mirror::Object* obj,
                                   const Visitor& visitor)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
    return VisitInstanceFieldsReferences(klass, obj, visitor);
  }

  // Blackens objects grayed during a garbage collection.
  void ScanGrayObjects(bool paused, byte minimum_age)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Schedules an unmarked object for reference processing.
  void DelayReferenceReferent(mirror::Class* klass, mirror::Object* reference)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  // Recursively blackens objects on the mark stack.
  void ProcessMarkStack(bool paused)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void ProcessMarkStackParallel(size_t thread_count)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void EnqueueFinalizerReferences(mirror::Object** ref)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void PreserveSomeSoftReferences(mirror::Object** ref)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void ClearWhiteReferences(mirror::Object** list)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  // Used to get around thread safety annotations. The call is from MarkingPhase and is guarded by
  // IsExclusiveHeld.
  void RevokeAllThreadLocalAllocationStacks(Thread* self) NO_THREAD_SAFETY_ANALYSIS;

  // Whether or not we count how many of each type of object were scanned.
  static const bool kCountScannedTypes = false;

  // Current space, we check this space first to avoid searching for the appropriate space for an
  // object.
  accounting::SpaceBitmap* current_mark_bitmap_;

  accounting::ObjectStack* mark_stack_;

  // Immune range, every object inside the immune range is assumed to be marked.
  mirror::Object* immune_begin_;
  mirror::Object* immune_end_;

  // Parallel finger.
  AtomicInteger atomic_finger_;
  // Number of classes scanned, if kCountScannedTypes.
  AtomicInteger class_count_;
  // Number of arrays scanned, if kCountScannedTypes.
  AtomicInteger array_count_;
  // Number of non-class/arrays scanned, if kCountScannedTypes.
  AtomicInteger other_count_;
  AtomicInteger large_object_test_;
  AtomicInteger large_object_mark_;
  AtomicInteger classes_marked_;
  AtomicInteger overhead_time_;
  AtomicInteger work_chunks_created_;
  AtomicInteger work_chunks_deleted_;
  AtomicInteger reference_count_;

  // Verification.
  size_t live_stack_freeze_size_;

  UniquePtr<Barrier> gc_barrier_;
  Mutex large_object_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  Mutex mark_stack_lock_ ACQUIRED_AFTER(Locks::classlinker_classes_lock_);

  const bool is_concurrent_;

 private:
  friend class AddIfReachesAllocSpaceVisitor;  // Used by mod-union table.
  friend class CardScanTask;
  friend class CheckBitmapVisitor;
  friend class CheckReferenceVisitor;
  friend class art::gc::Heap;
  friend class InternTableEntryIsUnmarked;
  friend class MarkIfReachesAllocspaceVisitor;
  friend class ModUnionCheckReferences;
  friend class ModUnionClearCardVisitor;
  friend class ModUnionReferenceVisitor;
  friend class ModUnionVisitor;
  friend class ModUnionTableBitmap;
  friend class ModUnionTableReferenceCache;
  friend class ModUnionScanImageRootVisitor;
  friend class ScanBitmapVisitor;
  friend class ScanImageRootVisitor;
  template<bool kUseFinger> friend class MarkStackTask;
  friend class FifoMarkStackChunk;

  DISALLOW_COPY_AND_ASSIGN(MarkSweep);
};

}  // namespace collector
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_COLLECTOR_MARK_SWEEP_H_
