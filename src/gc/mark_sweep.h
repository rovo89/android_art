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

#ifndef ART_SRC_MARK_SWEEP_H_
#define ART_SRC_MARK_SWEEP_H_

#include "atomic_stack.h"
#include "garbage_collector.h"
#include "macros.h"
#include "heap_bitmap.h"
#include "object.h"
#include "offsets.h"

namespace art {

class Barrier;
class CheckObjectVisitor;
class Class;
class Heap;
class MarkIfReachesAllocspaceVisitor;
class ModUnionClearCardVisitor;
class ModUnionVisitor;
class ModUnionTableBitmap;
class Object;
class TimingLogger;
class MarkStackChunk;

class MarkSweep : public GarbageCollector {
 public:
  explicit MarkSweep(Heap* heap, bool is_concurrent);

  ~MarkSweep();

  virtual std::string GetName() const;
  virtual void InitializePhase();
  virtual bool IsConcurrent() const;
  virtual bool HandleDirtyObjectsPhase() EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual void MarkingPhase() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual void ReclaimPhase() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual void FinishPhase();
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
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  void MarkConcurrentRoots();
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  void MarkRootsCheckpoint();
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Verify that image roots point to only marked objects within the alloc space.
  void VerifyImageRoots() EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Builds a mark stack and recursively mark until it empties.
  void RecursiveMark()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Make a space immune, immune spaces are assumed to have all live objects marked.
  void ImmuneSpace(ContinuousSpace* space)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);;

  // Bind the live bits to the mark bits of bitmaps based on the gc type.
  virtual void BindBitmaps()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void BindLiveToMarkBitmap(ContinuousSpace* space)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  void UnBindBitmaps()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Builds a mark stack with objects on dirty cards and recursively mark until it empties.
  void RecursiveMarkDirtyObjects(byte minimum_age = CardTable::kCardDirty)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Remarks the root set after completing the concurrent mark.
  void ReMarkRoots()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void ProcessReferences(Thread* self)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Sweeps unmarked objects to complete the garbage collection.
  virtual void Sweep(TimingLogger& timings, bool swap_bitmaps)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Sweeps unmarked objects to complete the garbage collection.
  void SweepLargeObjects(bool swap_bitmaps)
      EXCLUSIVE_LOCKS_REQUIRED(GlobalSynchronization::heap_bitmap_lock_);

  // Sweep only pointers within an array. WARNING: Trashes objects.
  void SweepArray(TimingLogger& logger, ObjectStack* allocation_stack_, bool swap_bitmaps)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Swap bitmaps (if we are a full Gc then we swap the zygote bitmap too).
  virtual void SwapBitmaps() EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);
  void SwapLargeObjects() EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  Object* GetClearedReferences() {
    return cleared_reference_list_;
  }

  // Proxy for external access to ScanObject.
  void ScanRoot(const Object* obj)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Blackens an object.
  void ScanObject(const Object* obj)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  template <typename MarkVisitor>
  void ScanObjectVisit(const Object* obj, const MarkVisitor& visitor)
      NO_THREAD_SAFETY_ANALYSIS {
    DCHECK(obj != NULL);
    if (kIsDebugBuild && !IsMarked(obj)) {
      heap_->DumpSpaces();
      LOG(FATAL) << "Scanning unmarked object " << obj;
    }
    Class* klass = obj->GetClass();
    DCHECK(klass != NULL);
    if (klass == java_lang_Class_) {
      DCHECK_EQ(klass->GetClass(), java_lang_Class_);
      if (kCountScannedTypes) {
        ++class_count_;
      }
      VisitClassReferences(klass, obj, visitor);
    } else if (klass->IsArrayClass()) {
      if (kCountScannedTypes) {
        ++array_count_;
      }
      visitor(obj, klass, Object::ClassOffset(), false);
      if (klass->IsObjectArrayClass()) {
        VisitObjectArrayReferences(obj->AsObjectArray<Object>(), visitor);
      }
    } else {
      if (kCountScannedTypes) {
        ++other_count_;
      }
      VisitOtherReferences(klass, obj, visitor);
      if (UNLIKELY(klass->IsReferenceClass())) {
        DelayReferenceReferent(const_cast<Object*>(obj));
      }
    }
  }

  void SetFinger(Object* new_finger) {
    finger_ = new_finger;
  }

  void DisableFinger() {
    SetFinger(reinterpret_cast<Object*>(~static_cast<uintptr_t>(0)));
  }

  size_t GetFreedBytes() const {
    return freed_bytes_;
  }

  size_t GetFreedObjects() const {
    return freed_objects_;
  }

  uint64_t GetTotalTime() const {
    return total_time_;
  }

  uint64_t GetTotalPausedTime() const {
    return total_paused_time_;
  }

  uint64_t GetTotalFreedObjects() const {
    return total_freed_objects_;
  }

  uint64_t GetTotalFreedBytes() const {
    return total_freed_bytes_;
  }

  // Everything inside the immune range is assumed to be marked.
  void SetImmuneRange(Object* begin, Object* end);

  void SweepSystemWeaks()
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Only sweep the weaks which are inside of an allocation stack.
  void SweepSystemWeaksArray(ObjectStack* allocations)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  static bool VerifyIsLiveCallback(const Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  void VerifySystemWeaks()
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Verify that an object is live, either in a live bitmap or in the allocation stack.
  void VerifyIsLive(const Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  template <typename Visitor>
  static void VisitObjectReferences(const Object* obj, const Visitor& visitor)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_,
                            Locks::mutator_lock_) {
    DCHECK(obj != NULL);
    DCHECK(obj->GetClass() != NULL);

    Class* klass = obj->GetClass();
    DCHECK(klass != NULL);
    if (klass == Class::GetJavaLangClass()) {
      DCHECK_EQ(klass->GetClass(), Class::GetJavaLangClass());
      VisitClassReferences(klass, obj, visitor);
    } else {
      if (klass->IsArrayClass()) {
        visitor(obj, klass, Object::ClassOffset(), false);
        if (klass->IsObjectArrayClass()) {
          VisitObjectArrayReferences(obj->AsObjectArray<Object>(), visitor);
        }
      } else {
        VisitOtherReferences(klass, obj, visitor);
      }
    }
  }

  static void MarkObjectCallback(const Object* root, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  static void MarkRootParallelCallback(const Object* root, void* arg);

  // Marks an object.
  void MarkObject(const Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  void MarkRoot(const Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  Barrier& GetBarrier();
  const TimingLogger& GetTimings() const;
  const CumulativeLogger& GetCumulativeTimings() const;
  void ResetCumulativeStatistics();

 protected:
  // Returns true if the object has its bit set in the mark bitmap.
  bool IsMarked(const Object* object) const;

  static bool IsMarkedCallback(const Object* object, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  static bool IsMarkedArrayCallback(const Object* object, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  static void ReMarkObjectVisitor(const Object* root, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  static void VerifyImageRootVisitor(Object* root, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_,
                            Locks::mutator_lock_);

  void MarkObjectNonNull(const Object* obj, bool check_finger)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  void MarkObjectNonNullParallel(const Object* obj, bool check_finger);

  bool MarkLargeObject(const Object* obj)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Returns true if we need to add obj to a mark stack.
  bool MarkObjectParallel(const Object* obj) NO_THREAD_SAFETY_ANALYSIS;

  static void SweepCallback(size_t num_ptrs, Object** ptrs, void* arg)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Special sweep for zygote that just marks objects / dirties cards.
  static void ZygoteSweepCallback(size_t num_ptrs, Object** ptrs, void* arg)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  void CheckReference(const Object* obj, const Object* ref, MemberOffset offset, bool is_static)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  void CheckObject(const Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  // Verify the roots of the heap and print out information related to any invalid roots.
  // Called in MarkObject, so may we may not hold the mutator lock.
  void VerifyRoots()
      NO_THREAD_SAFETY_ANALYSIS;

  // Expand mark stack to 2x its current size. Thread safe.
  void ExpandMarkStack();

  static void VerifyRootCallback(const Object* root, void* arg, size_t vreg,
                                 const StackVisitor *visitor);

  void VerifyRoot(const Object* root, size_t vreg, const StackVisitor* visitor)
      NO_THREAD_SAFETY_ANALYSIS;

  template <typename Visitor>
  static void VisitInstanceFieldsReferences(const Class* klass, const Object* obj,
                                            const Visitor& visitor)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
    DCHECK(obj != NULL);
    DCHECK(klass != NULL);
    VisitFieldsReferences(obj, klass->GetReferenceInstanceOffsets(), false, visitor);
  }

  // Visit the header, static field references, and interface pointers of a class object.
  template <typename Visitor>
  static void VisitClassReferences(const Class* klass, const Object* obj,
                                   const Visitor& visitor)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
    VisitInstanceFieldsReferences(klass, obj, visitor);
    VisitStaticFieldsReferences(obj->AsClass(), visitor);
  }

  template <typename Visitor>
  static void VisitStaticFieldsReferences(const Class* klass, const Visitor& visitor)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
    DCHECK(klass != NULL);
    VisitFieldsReferences(klass, klass->GetReferenceStaticOffsets(), true, visitor);
  }

  template <typename Visitor>
  static void VisitFieldsReferences(const Object* obj, uint32_t ref_offsets, bool is_static,
                             const Visitor& visitor)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
    if (LIKELY(ref_offsets != CLASS_WALK_SUPER)) {
      // Found a reference offset bitmap.  Mark the specified offsets.
      while (ref_offsets != 0) {
        size_t right_shift = CLZ(ref_offsets);
        MemberOffset field_offset = CLASS_OFFSET_FROM_CLZ(right_shift);
        const Object* ref = obj->GetFieldObject<const Object*>(field_offset, false);
        visitor(obj, ref, field_offset, is_static);
        ref_offsets &= ~(CLASS_HIGH_BIT >> right_shift);
      }
    } else {
      // There is no reference offset bitmap.  In the non-static case,
      // walk up the class inheritance hierarchy and find reference
      // offsets the hard way. In the static case, just consider this
      // class.
      for (const Class* klass = is_static ? obj->AsClass() : obj->GetClass();
           klass != NULL;
           klass = is_static ? NULL : klass->GetSuperClass()) {
        size_t num_reference_fields = (is_static
                                       ? klass->NumReferenceStaticFields()
                                       : klass->NumReferenceInstanceFields());
        for (size_t i = 0; i < num_reference_fields; ++i) {
          Field* field = (is_static
                          ? klass->GetStaticField(i)
                          : klass->GetInstanceField(i));
          MemberOffset field_offset = field->GetOffset();
          const Object* ref = obj->GetFieldObject<const Object*>(field_offset, false);
          visitor(obj, ref, field_offset, is_static);
        }
      }
    }
  }

  // Visit all of the references in an object array.
  template <typename Visitor>
  static void VisitObjectArrayReferences(const ObjectArray<Object>* array,
                                         const Visitor& visitor)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
    const int32_t length = array->GetLength();
    for (int32_t i = 0; i < length; ++i) {
      const Object* element = array->GetWithoutChecks(i);
      const size_t width = sizeof(Object*);
      MemberOffset offset = MemberOffset(i * width + Array::DataOffset(width).Int32Value());
      visitor(array, element, offset, false);
    }
  }

  // Visits the header and field references of a data object.
  template <typename Visitor>
  static void VisitOtherReferences(const Class* klass, const Object* obj,
                                   const Visitor& visitor)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
    return VisitInstanceFieldsReferences(klass, obj, visitor);
  }

  // Blackens objects grayed during a garbage collection.
  void ScanGrayObjects(byte minimum_age)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Schedules an unmarked object for reference processing.
  void DelayReferenceReferent(Object* reference)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  // Recursively blackens objects on the mark stack.
  void ProcessMarkStack()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void ProcessMarkStackParallel()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void EnqueueFinalizerReferences(Object** ref)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void PreserveSomeSoftReferences(Object** ref)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void ClearWhiteReferences(Object** list)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  void ProcessReferences(Object** soft_references, bool clear_soft_references,
                         Object** weak_references,
                         Object** finalizer_references,
                         Object** phantom_references)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void SweepJniWeakGlobals(Heap::IsMarkedTester is_marked, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Whether or not we count how many of each type of object were scanned.
  static const bool kCountScannedTypes = false;

  // Current space, we check this space first to avoid searching for the appropriate space for an object.
  SpaceBitmap* current_mark_bitmap_;

  // Cache java.lang.Class for optimization.
  Class* java_lang_Class_;

  ObjectStack* mark_stack_;

  Object* finger_;

  // Immune range, every object inside the immune range is assumed to be marked.
  Object* immune_begin_;
  Object* immune_end_;

  Object* soft_reference_list_;
  Object* weak_reference_list_;
  Object* finalizer_reference_list_;
  Object* phantom_reference_list_;
  Object* cleared_reference_list_;

  AtomicInteger freed_bytes_;
  AtomicInteger freed_objects_;
  AtomicInteger class_count_;
  AtomicInteger array_count_;
  AtomicInteger other_count_;
  AtomicInteger large_object_test_;
  AtomicInteger large_object_mark_;
  AtomicInteger classes_marked_;
  AtomicInteger overhead_time_;
  AtomicInteger work_chunks_created_;
  AtomicInteger work_chunks_deleted_;
  AtomicInteger reference_count_;

  // Cumulative statistics.
  uint64_t total_time_;
  uint64_t total_paused_time_;
  uint64_t total_freed_objects_;
  uint64_t total_freed_bytes_;

  UniquePtr<Barrier> gc_barrier_;
  Mutex large_object_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  Mutex mark_stack_expand_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  TimingLogger timings_;
  CumulativeLogger cumulative_timings_;

  bool is_concurrent_;
  bool clear_soft_references_;

  friend class AddIfReachesAllocSpaceVisitor; // Used by mod-union table.
  friend class CheckBitmapVisitor;
  friend class CheckObjectVisitor;
  friend class CheckReferenceVisitor;
  friend class Heap;
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
  friend class MarkStackChunk;
  friend class FifoMarkStackChunk;

  DISALLOW_COPY_AND_ASSIGN(MarkSweep);
};

}  // namespace art

#endif  // ART_SRC_MARK_SWEEP_H_
