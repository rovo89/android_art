/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_RUNTIME_GC_COLLECTOR_SEMI_SPACE_H_
#define ART_RUNTIME_GC_COLLECTOR_SEMI_SPACE_H_

#include "atomic_integer.h"
#include "barrier.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "garbage_collector.h"
#include "offsets.h"
#include "root_visitor.h"
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
  class BumpPointerSpace;
  class ContinuousMemMapAllocSpace;
  class ContinuousSpace;
  class MallocSpace;
}  // namespace space

class Heap;

namespace collector {

class SemiSpace : public GarbageCollector {
 public:
  explicit SemiSpace(Heap* heap, const std::string& name_prefix = "");

  ~SemiSpace() {}

  virtual void InitializePhase();
  virtual bool IsConcurrent() const {
    return false;
  }
  virtual void MarkingPhase() EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual void ReclaimPhase() EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual void FinishPhase() EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual void MarkReachableObjects()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_);
  virtual GcType GetGcType() const {
    return kGcTypePartial;
  }

  // Sets which space we will be copying objects to.
  void SetToSpace(space::ContinuousMemMapAllocSpace* to_space);

  // Set the space where we copy objects from.
  void SetFromSpace(space::ContinuousMemMapAllocSpace* from_space);

  // Initializes internal structures.
  void Init();

  // Find the default mark bitmap.
  void FindDefaultMarkBitmap();

  // Returns the new address of the object.
  mirror::Object* MarkObject(mirror::Object* object)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  void ScanObject(mirror::Object* obj)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  // Marks the root set at the start of a garbage collection.
  void MarkRoots()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  // Make a space immune, immune spaces have all live objects marked - that is the mark and
  // live bitmaps are bound together.
  void ImmuneSpace(space::ContinuousSpace* space)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Bind the live bits to the mark bits of bitmaps for spaces that are never collected, ie
  // the image. Mark that portion of the heap as immune.
  virtual void BindBitmaps() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void BindLiveToMarkBitmap(space::ContinuousSpace* space)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  void UnBindBitmaps()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  void ProcessReferences(Thread* self)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Sweeps unmarked objects to complete the garbage collection.
  virtual void Sweep(bool swap_bitmaps) EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Sweeps unmarked objects to complete the garbage collection.
  void SweepLargeObjects(bool swap_bitmaps) EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Sweep only pointers within an array. WARNING: Trashes objects.
  void SweepArray(accounting::ObjectStack* allocation_stack_, bool swap_bitmaps)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // TODO: enable thread safety analysis when in use by multiple worker threads.
  template <typename MarkVisitor>
  void ScanObjectVisit(const mirror::Object* obj, const MarkVisitor& visitor)
      NO_THREAD_SAFETY_ANALYSIS;

  void SweepSystemWeaks()
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  template <typename Visitor>
  static void VisitObjectReferencesAndClass(mirror::Object* obj, const Visitor& visitor)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  static mirror::Object* MarkRootCallback(mirror::Object* root, void* arg)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  static mirror::Object* RecursiveMarkObjectCallback(mirror::Object* root, void* arg)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  virtual mirror::Object* MarkNonForwardedObject(mirror::Object* obj)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

 protected:
  // Returns null if the object is not marked, otherwise returns the forwarding address (same as
  // object for non movable things).
  mirror::Object* GetMarkedForwardAddress(mirror::Object* object) const;

  static mirror::Object* MarkedForwardingAddressCallback(mirror::Object* object, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Marks or unmarks a large object based on whether or not set is true. If set is true, then we
  // mark, otherwise we unmark.
  bool MarkLargeObject(const mirror::Object* obj)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Expand mark stack to 2x its current size.
  void ResizeMarkStack(size_t new_size);

  // Returns true if we should sweep the space.
  virtual bool ShouldSweepSpace(space::MallocSpace* space) const;

  // Returns how many threads we should use for the current GC phase based on if we are paused,
  // whether or not we care about pauses.
  size_t GetThreadCount(bool paused) const;

  // Returns true if an object is inside of the immune region (assumed to be marked).
  bool IsImmune(const mirror::Object* obj) const ALWAYS_INLINE {
    return obj >= immune_begin_ && obj < immune_end_;
  }

  bool IsImmuneSpace(const space::ContinuousSpace* space) const;

  static void VerifyRootCallback(const mirror::Object* root, void* arg, size_t vreg,
                                 const StackVisitor *visitor);

  void VerifyRoot(const mirror::Object* root, size_t vreg, const StackVisitor* visitor)
      NO_THREAD_SAFETY_ANALYSIS;

  template <typename Visitor>
  static void VisitInstanceFieldsReferences(const mirror::Class* klass, const mirror::Object* obj,
                                            const Visitor& visitor)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  // Visit the header, static field references, and interface pointers of a class object.
  template <typename Visitor>
  static void VisitClassReferences(const mirror::Class* klass, const mirror::Object* obj,
                                   const Visitor& visitor)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  template <typename Visitor>
  static void VisitStaticFieldsReferences(const mirror::Class* klass, const Visitor& visitor)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  template <typename Visitor>
  static void VisitFieldsReferences(const mirror::Object* obj, uint32_t ref_offsets, bool is_static,
                                    const Visitor& visitor)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  // Visit all of the references in an object array.
  template <typename Visitor>
  static void VisitObjectArrayReferences(const mirror::ObjectArray<mirror::Object>* array,
                                         const Visitor& visitor)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  // Visits the header and field references of a data object.
  template <typename Visitor>
  static void VisitOtherReferences(const mirror::Class* klass, const mirror::Object* obj,
                                   const Visitor& visitor)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
    return VisitInstanceFieldsReferences(klass, obj, visitor);
  }

  // Push an object onto the mark stack.
  inline void MarkStackPush(mirror::Object* obj);

  void UpdateAndMarkModUnion()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Schedules an unmarked object for reference processing.
  void DelayReferenceReferent(mirror::Class* klass, mirror::Object* reference)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_);

  // Recursively blackens objects on the mark stack.
  void ProcessMarkStack(bool paused)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_);

  void EnqueueFinalizerReferences(mirror::Object** ref)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_);

  void PreserveSomeSoftReferences(mirror::Object** ref)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_);

  void ClearWhiteReferences(mirror::Object** list)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_);

  void ProcessReferences(mirror::Object** soft_references, bool clear_soft_references,
                         mirror::Object** weak_references,
                         mirror::Object** finalizer_references,
                         mirror::Object** phantom_references)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_);

  inline mirror::Object* GetForwardingAddressInFromSpace(mirror::Object* obj) const;

  // Current space, we check this space first to avoid searching for the appropriate space for an
  // object.
  accounting::ObjectStack* mark_stack_;

  // Immune range, every object inside the immune range is assumed to be marked.
  mirror::Object* immune_begin_;
  mirror::Object* immune_end_;

  // If true, the large object space is immune.
  bool is_large_object_space_immune_;

  // Destination and source spaces (can be any type of ContinuousMemMapAllocSpace which either has
  // a live bitmap or doesn't).
  space::ContinuousMemMapAllocSpace* to_space_;
  accounting::SpaceBitmap* to_space_live_bitmap_;  // Cached live bitmap as an optimization.
  space::ContinuousMemMapAllocSpace* from_space_;

  Thread* self_;

  // Used for kEnableSimplePromo. The end/top of the bump pointer
  // space at the end of the last collection.
  byte* last_gc_to_space_end_;

  // Used for kEnableSimplePromo. During a collection, keeps track of
  // how many bytes of objects have been copied so far from the bump
  // pointer space to the non-moving space.
  uint64_t bytes_promoted_;

  // When true, collect the whole heap. When false, collect only the
  // bump pointer spaces.
  bool whole_heap_collection_;

  // A counter used to enable whole_heap_collection_ once per
  // interval.
  int whole_heap_collection_interval_counter_;

  // The default interval of the whole heap collection. If N, the
  // whole heap collection occurs every N collections.
  static constexpr int kDefaultWholeHeapCollectionInterval = 5;

 private:
  DISALLOW_COPY_AND_ASSIGN(SemiSpace);
};

}  // namespace collector
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_COLLECTOR_SEMI_SPACE_H_
