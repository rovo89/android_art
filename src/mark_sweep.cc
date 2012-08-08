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

#include "mark_sweep.h"

#include <climits>
#include <vector>

#include "class_loader.h"
#include "dex_cache.h"
#include "heap.h"
#include "indirect_reference_table.h"
#include "intern_table.h"
#include "logging.h"
#include "macros.h"
#include "mark_stack.h"
#include "monitor.h"
#include "object.h"
#include "runtime.h"
#include "scoped_heap_lock.h"
#include "space.h"
#include "timing_logger.h"
#include "thread.h"

namespace art {

MarkSweep::MarkSweep(MarkStack* mark_stack)
    : current_mark_bitmap_(NULL),
      mark_stack_(mark_stack),
      heap_(NULL),
      finger_(NULL),
      condemned_(NULL),
      soft_reference_list_(NULL),
      weak_reference_list_(NULL),
      finalizer_reference_list_(NULL),
      phantom_reference_list_(NULL),
      cleared_reference_list_(NULL),
      class_count_(0), array_count_(0), other_count_(0) {
  DCHECK(mark_stack_ != NULL);
}

void MarkSweep::Init() {
  heap_ = Runtime::Current()->GetHeap();
  mark_stack_->Reset();

  const Spaces& spaces = heap_->GetSpaces();
  // TODO: C++0x auto
  for (Spaces::const_iterator cur = spaces.begin(); cur != spaces.end(); ++cur) {
    if ((*cur)->GetGcRetentionPolicy() == GCRP_ALWAYS_COLLECT) {
      current_mark_bitmap_ = (*cur)->GetMarkBitmap();
      break;
    }
  }
  // TODO: if concurrent, enable card marking in compiler
  // TODO: check that the mark bitmap is entirely clear.
}

void MarkSweep::MarkObject0(const Object* obj, bool check_finger) {
  DCHECK(obj != NULL);

  SpaceBitmap* space_bitmap = NULL;
  // Try to take advantage of locality of references within a space, failing this find the space
  // the hard way.
  if (current_mark_bitmap_->HasAddress(obj)) {
    space_bitmap = current_mark_bitmap_;
  } else {
    space_bitmap = heap_->GetMarkBitmap()->GetSpaceBitmap(obj);
  }

  if (obj < condemned_) {
    DCHECK(IsMarked(obj));
    return;
  }
  bool is_marked = space_bitmap->Test(obj);
  // This object was not previously marked.
  if (!is_marked) {
    space_bitmap->Set(obj);
    if (check_finger && obj < finger_) {
      // The object must be pushed on to the mark stack.
      mark_stack_->Push(obj);
    }
  }
}

// Used to mark objects when recursing.  Recursion is done by moving
// the finger across the bitmaps in address order and marking child
// objects.  Any newly-marked objects whose addresses are lower than
// the finger won't be visited by the bitmap scan, so those objects
// need to be added to the mark stack.
void MarkSweep::MarkObject(const Object* obj) {
  if (obj != NULL) {
    MarkObject0(obj, true);
  }
}

void MarkSweep::MarkObjectVisitor(const Object* root, void* arg) {
  DCHECK(root != NULL);
  DCHECK(arg != NULL);
  MarkSweep* mark_sweep = reinterpret_cast<MarkSweep*>(arg);
  DCHECK(mark_sweep->finger_ == NULL);  // no point to check finger if it is NULL
  mark_sweep->MarkObject0(root, false);
}

void MarkSweep::ReMarkObjectVisitor(const Object* root, void* arg) {
  DCHECK(root != NULL);
  DCHECK(arg != NULL);
  MarkSweep* mark_sweep = reinterpret_cast<MarkSweep*>(arg);
  mark_sweep->MarkObject0(root, true);
}

// Marks all objects in the root set.
void MarkSweep::MarkRoots() {
  Runtime::Current()->VisitRoots(MarkObjectVisitor, this);
}

class CheckObjectVisitor {
 public:
  CheckObjectVisitor(MarkSweep* const mark_sweep)
      : mark_sweep_(mark_sweep) {

  }

  void operator ()(const Object* obj, const Object* ref, MemberOffset offset, bool is_static) const {
    mark_sweep_->CheckReference(obj, ref, offset, is_static);
  }

 private:
  MarkSweep* const mark_sweep_;
};

void MarkSweep::CheckObject(const Object* obj) {
  DCHECK(obj != NULL);
  CheckObjectVisitor visitor(this);
  VisitObjectReferences(obj, visitor);
}

void MarkSweep::VerifyImageRootVisitor(Object* root, void* arg) {
  DCHECK(root != NULL);
  DCHECK(arg != NULL);
  MarkSweep* mark_sweep = reinterpret_cast<MarkSweep*>(arg);
  DCHECK(mark_sweep->heap_->GetMarkBitmap()->Test(root));
  mark_sweep->CheckObject(root);
}

void MarkSweep::CopyMarkBits() {
  const std::vector<Space*>& spaces = heap_->GetSpaces();
  for (size_t i = 0; i < spaces.size(); ++i) {
    Space* space = spaces[i];
    if (space->GetGcRetentionPolicy() == GCRP_FULL_COLLECT) {
      SpaceBitmap* live_bitmap = space->GetLiveBitmap();
      SpaceBitmap* mark_bitmap = space->GetMarkBitmap();
      DCHECK_EQ(live_bitmap->Size(), mark_bitmap->Size());
      std::copy(live_bitmap->Begin(), live_bitmap->Begin() + live_bitmap->Size() / kWordSize, mark_bitmap->Begin());
    }
  }
}

class ScanImageRootVisitor {
 public:
  ScanImageRootVisitor(MarkSweep* const mark_sweep) : mark_sweep_(mark_sweep) {

  }

  void operator ()(const Object* root) const {
    DCHECK(root != NULL);
    mark_sweep_->ScanObject(root);
  }

 private:
  MarkSweep* const mark_sweep_;
};

// Marks all objects that are in images and have been touched by the mutator
void MarkSweep::ScanDirtyImageRoots() {
  const std::vector<Space*>& spaces = heap_->GetSpaces();
  CardTable* card_table = heap_->GetCardTable();
  ScanImageRootVisitor image_root_visitor(this);
  for (size_t i = 0; i < spaces.size(); ++i) {
    Space* space = spaces[i];
    if (space->IsImageSpace()) {
      card_table->Scan(space->GetLiveBitmap(), space->Begin(), space->End(), image_root_visitor);
    }
  }
}

void MarkSweep::ScanBitmapCallback(Object* obj, void* finger, void* arg) {
  MarkSweep* mark_sweep = reinterpret_cast<MarkSweep*>(arg);
  mark_sweep->finger_ = reinterpret_cast<Object*>(finger);
  mark_sweep->ScanObject(obj);
}

void MarkSweep::ScanDirtyCardCallback(Object* obj, void* arg) {
  MarkSweep* mark_sweep = reinterpret_cast<MarkSweep*>(arg);
  mark_sweep->ScanObject(obj);
}

void MarkSweep::ScanGrayObjects() {
  const std::vector<Space*>& spaces = heap_->GetSpaces();
  CardTable* card_table = heap_->GetCardTable();
  ScanImageRootVisitor image_root_visitor(this);
  for (size_t i = 0; i < spaces.size(); ++i) {
    byte* begin = spaces[i]->Begin();
    byte* end = spaces[i]->End();
    // Image spaces are handled properly since live == marked for them.
    card_table->Scan(spaces[i]->GetMarkBitmap(), begin, end, image_root_visitor);
  }
}

class CheckBitmapVisitor {
 public:
  CheckBitmapVisitor(MarkSweep* mark_sweep) : mark_sweep_(mark_sweep) {

  }

  void operator ()(const Object* obj) const {
    DCHECK(obj != NULL);
    mark_sweep_->CheckObject(obj);
  }

 private:
  MarkSweep* mark_sweep_;
};

void MarkSweep::VerifyImageRoots() {
  // Verify roots ensures that all the references inside the image space point
  // objects which are either in the image space or marked objects in the alloc
  // space
#ifndef NDEBUG
  CheckBitmapVisitor visitor(this);
  const Spaces& spaces = heap_->GetSpaces();
  for (Spaces::const_iterator it = spaces.begin(); it != spaces.end(); ++it) {
    const Space* space = *it;
    if (space->IsImageSpace()) {
      uintptr_t begin = reinterpret_cast<uintptr_t>(space->Begin());
      uintptr_t end = reinterpret_cast<uintptr_t>(space->End());
      SpaceBitmap* live_bitmap = space->GetLiveBitmap();
      DCHECK(live_bitmap != NULL);
      live_bitmap->VisitMarkedRange(begin, end, visitor);
    }
  }
#endif
}

// Populates the mark stack based on the set of marked objects and
// recursively marks until the mark stack is emptied.
void MarkSweep::RecursiveMark(bool partial) {
  // RecursiveMark will build the lists of known instances of the Reference classes.
  // See DelayReferenceReferent for details.
  CHECK(soft_reference_list_ == NULL);
  CHECK(weak_reference_list_ == NULL);
  CHECK(finalizer_reference_list_ == NULL);
  CHECK(phantom_reference_list_ == NULL);
  CHECK(cleared_reference_list_ == NULL);

  void* arg = reinterpret_cast<void*>(this);
  const Spaces& spaces = heap_->GetSpaces();

  for (size_t i = 0; i < spaces.size(); ++i) {
    Space* space = spaces[i];
    if (space->GetGcRetentionPolicy() == GCRP_ALWAYS_COLLECT ||
        (!partial && space->GetGcRetentionPolicy() == GCRP_FULL_COLLECT)
        ) {
      uintptr_t begin = reinterpret_cast<uintptr_t>(space->Begin());
      uintptr_t end = reinterpret_cast<uintptr_t>(space->End());

      current_mark_bitmap_ = space->GetMarkBitmap();
      current_mark_bitmap_->ScanWalk(begin, end, &ScanBitmapCallback, arg);
    }
  }
  finger_ = reinterpret_cast<Object*>(~0);
  // TODO: tune the frequency of emptying the mark stack
  ProcessMarkStack();
}

void MarkSweep::RecursiveMarkDirtyObjects() {
  ScanGrayObjects();
  ProcessMarkStack();
}

void MarkSweep::ReMarkRoots() {
  Runtime::Current()->VisitRoots(ReMarkObjectVisitor, this);
}

void MarkSweep::SweepJniWeakGlobals(HeapBitmap* bitmap) {
  JavaVMExt* vm = Runtime::Current()->GetJavaVM();
  MutexLock mu(vm->weak_globals_lock);
  IndirectReferenceTable* table = &vm->weak_globals;
  typedef IndirectReferenceTable::iterator It;  // TODO: C++0x auto
  for (It it = table->begin(), end = table->end(); it != end; ++it) {
    const Object** entry = *it;
    if (!bitmap->Test(*entry)) {
      *entry = kClearedJniWeakGlobal;
    }
  }
}

void MarkSweep::SweepSystemWeaks(bool swap_bitmaps) {
  Runtime* runtime = Runtime::Current();
  runtime->GetInternTable()->SweepInternTableWeaks(swap_bitmaps ? IsLiveCallback : IsMarkedCallback,
                                                   this);
  runtime->GetMonitorList()->SweepMonitorList(swap_bitmaps ? IsLiveCallback : IsMarkedCallback,
                                              this);
  SweepJniWeakGlobals(swap_bitmaps ? GetHeap()->GetLiveBitmap() : GetHeap()->GetMarkBitmap());
}

struct SweepCallbackContext {
  Heap* heap;
  AllocSpace* space;
};

void MarkSweep::SweepCallback(size_t num_ptrs, Object** ptrs, void* arg) {
  ScopedHeapLock lock;

  size_t freed_objects = num_ptrs;
  size_t freed_bytes = 0;
  SweepCallbackContext* context = static_cast<SweepCallbackContext*>(arg);
  Heap* heap = context->heap;
  AllocSpace* space = context->space;
  // Use a bulk free, that merges consecutive objects before freeing or free per object?
  // Documentation suggests better free performance with merging, but this may be at the expensive
  // of allocation.
  // TODO: investigate performance
  static const bool kUseFreeList = true;
  if (kUseFreeList) {
    for (size_t i = 0; i < num_ptrs; ++i) {
      Object* obj = static_cast<Object*>(ptrs[i]);
      freed_bytes += space->AllocationSize(obj);
    }
    // AllocSpace::FreeList clears the value in ptrs, so perform after clearing the live bit
    space->FreeList(num_ptrs, ptrs);
  } else {
    for (size_t i = 0; i < num_ptrs; ++i) {
      Object* obj = static_cast<Object*>(ptrs[i]);
      freed_bytes += space->AllocationSize(obj);
      space->Free(obj);
    }
  }
  heap->RecordFreeLocked(freed_objects, freed_bytes);
}

void MarkSweep::ZygoteSweepCallback(size_t num_ptrs, Object** ptrs, void* arg) {
  ScopedHeapLock lock;
  SweepCallbackContext* context = static_cast<SweepCallbackContext*>(arg);
  Heap* heap = context->heap;
  // We don't free any actual memory to avoid dirtying the shared zygote pages.
  for (size_t i = 0; i < num_ptrs; ++i) {
    Object* obj = static_cast<Object*>(ptrs[i]);
    heap->GetLiveBitmap()->Clear(obj);
    heap->GetCardTable()->MarkCard(obj);
  }
}

void MarkSweep::Sweep(bool partial) {
  // If we don't swap bitmaps then we can not do this concurrently.
  SweepSystemWeaks(true);

  DCHECK(mark_stack_->IsEmpty());

  const Spaces& spaces = heap_->GetSpaces();
  SweepCallbackContext scc;
  scc.heap = heap_;
  for (size_t i = 0; i < spaces.size(); ++i) {
    Space* space = spaces[i];
    if (
        space->GetGcRetentionPolicy() == GCRP_ALWAYS_COLLECT ||
        (!partial && space->GetGcRetentionPolicy() == GCRP_FULL_COLLECT)
        ) {
      uintptr_t begin = reinterpret_cast<uintptr_t>(space->Begin());
      uintptr_t end = reinterpret_cast<uintptr_t>(space->End());
      scc.space = space->AsAllocSpace();
      SpaceBitmap* live_bitmap = space->GetLiveBitmap();
      SpaceBitmap* mark_bitmap = space->GetMarkBitmap();
      if (space->GetGcRetentionPolicy() == GCRP_ALWAYS_COLLECT) {
        // Bitmaps are pre-swapped for optimization which enables sweeping with the heap unlocked.
        SpaceBitmap::SweepWalk(
            *mark_bitmap, *live_bitmap, begin, end, &SweepCallback, reinterpret_cast<void*>(&scc));
      } else {
        // Zygote sweep takes care of dirtying cards and clearing live bits, does not free actual memory.
        SpaceBitmap::SweepWalk(
            *live_bitmap, *mark_bitmap, begin, end, &ZygoteSweepCallback, reinterpret_cast<void*>(&scc));
      }
    }
  }
}

// Scans instance fields.
inline void MarkSweep::ScanInstanceFields(const Object* obj) {
  DCHECK(obj != NULL);
  Class* klass = obj->GetClass();
  DCHECK(klass != NULL);
  ScanFields(obj, klass->GetReferenceInstanceOffsets(), false);
}

// Scans static storage on a Class.
inline void MarkSweep::ScanStaticFields(const Class* klass) {
  DCHECK(klass != NULL);
  ScanFields(klass, klass->GetReferenceStaticOffsets(), true);
}

inline void MarkSweep::ScanFields(const Object* obj, uint32_t ref_offsets, bool is_static) {
  if (ref_offsets != CLASS_WALK_SUPER) {
    // Found a reference offset bitmap.  Mark the specified offsets.
    while (ref_offsets != 0) {
      size_t right_shift = CLZ(ref_offsets);
      MemberOffset byte_offset = CLASS_OFFSET_FROM_CLZ(right_shift);
      const Object* ref = obj->GetFieldObject<const Object*>(byte_offset, false);
      MarkObject(ref);
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
        MarkObject(ref);
      }
    }
  }
}

void MarkSweep::CheckReference(const Object* obj, const Object* ref, MemberOffset offset, bool is_static) {
  const Spaces& spaces = heap_->GetSpaces();
  // TODO: C++0x auto
  for (Spaces::const_iterator cur = spaces.begin(); cur != spaces.end(); ++cur) {
    if ((*cur)->IsAllocSpace() && (*cur)->Contains(ref)) {
      DCHECK(IsMarked(obj));

      bool is_marked = IsMarked(ref);
      if (!is_marked) {
        LOG(INFO) << **cur;
        LOG(WARNING) << (is_static ? "Static ref'" : "Instance ref'") << PrettyTypeOf(ref)
                     << "' (" << reinterpret_cast<const void*>(ref) << ") in '" << PrettyTypeOf(obj)
                     << "' (" << reinterpret_cast<const void*>(obj) << ") at offset "
                     << reinterpret_cast<void*>(offset.Int32Value()) << " wasn't marked";

        const Class* klass = is_static ? obj->AsClass() : obj->GetClass();
        DCHECK(klass != NULL);
        const ObjectArray<Field>* fields = is_static ? klass->GetSFields() : klass->GetIFields();
        DCHECK(fields != NULL);
        bool found = false;
        for (int32_t i = 0; i < fields->GetLength(); ++i) {
          const Field* cur = fields->Get(i);
          if (cur->GetOffset().Int32Value() == offset.Int32Value()) {
            LOG(WARNING) << "Field referencing the alloc space was " << PrettyField(cur);
            found = true;
            break;
          }
        }
        if (!found) {
          LOG(WARNING) << "Could not find field in object alloc space with offset " << offset.Int32Value();
        }

        bool obj_marked = heap_->GetCardTable()->IsDirty(obj);
        if (!obj_marked) {
          LOG(WARNING) << "Object '" << PrettyTypeOf(obj) << "' "
                       << "(" << reinterpret_cast<const void*>(obj) << ") contains references to "
                       << "the alloc space, but wasn't card marked";
        }
      }
    }
    break;
  }
}

// Scans the header, static field references, and interface pointers
// of a class object.
inline void MarkSweep::ScanClass(const Object* obj) {
#ifndef NDEBUG
  ++class_count_;
#endif
  ScanInstanceFields(obj);
  ScanStaticFields(obj->AsClass());
}

// Scans the header of all array objects.  If the array object is
// specialized to a reference type, scans the array data as well.
inline void MarkSweep::ScanArray(const Object* obj) {
#ifndef NDEBUG
  ++array_count_;
#endif
  MarkObject(obj->GetClass());
  if (obj->IsObjectArray()) {
    const ObjectArray<Object>* array = obj->AsObjectArray<Object>();
    for (int32_t i = 0; i < array->GetLength(); ++i) {
      const Object* element = array->GetWithoutChecks(i);
      MarkObject(element);
    }
  }
}

// Process the "referent" field in a java.lang.ref.Reference.  If the
// referent has not yet been marked, put it on the appropriate list in
// the gcHeap for later processing.
void MarkSweep::DelayReferenceReferent(Object* obj) {
  DCHECK(obj != NULL);
  Class* klass = obj->GetClass();
  DCHECK(klass != NULL);
  DCHECK(klass->IsReferenceClass());
  Object* pending = obj->GetFieldObject<Object*>(heap_->GetReferencePendingNextOffset(), false);
  Object* referent = heap_->GetReferenceReferent(obj);
  if (pending == NULL && referent != NULL && !IsMarked(referent)) {
    Object** list = NULL;
    if (klass->IsSoftReferenceClass()) {
      list = &soft_reference_list_;
    } else if (klass->IsWeakReferenceClass()) {
      list = &weak_reference_list_;
    } else if (klass->IsFinalizerReferenceClass()) {
      list = &finalizer_reference_list_;
    } else if (klass->IsPhantomReferenceClass()) {
      list = &phantom_reference_list_;
    }
    DCHECK(list != NULL) << PrettyClass(klass) << " " << std::hex << klass->GetAccessFlags();
    heap_->EnqueuePendingReference(obj, list);
  }
}

// Scans the header and field references of a data object.  If the
// scanned object is a reference subclass, it is scheduled for later
// processing.
inline void MarkSweep::ScanOther(const Object* obj) {
#ifndef NDEBUG
  ++other_count_;
#endif
  ScanInstanceFields(obj);
  if (obj->GetClass()->IsReferenceClass()) {
    DelayReferenceReferent(const_cast<Object*>(obj));
  }
}

// Scans an object reference.  Determines the type of the reference
// and dispatches to a specialized scanning routine.
void MarkSweep::ScanObject(const Object* obj) {
  DCHECK(obj != NULL);
  DCHECK(obj->GetClass() != NULL);
  DCHECK(heap_->GetMarkBitmap()->Test(obj));
  if (obj->IsClass()) {
    ScanClass(obj);
  } else if (obj->IsArrayInstance()) {
    ScanArray(obj);
  } else {
    ScanOther(obj);
  }
}

// Scan anything that's on the mark stack.
void MarkSweep::ProcessMarkStack() {
  while (!mark_stack_->IsEmpty()) {
    const Object* obj = mark_stack_->Pop();
    DCHECK(obj != NULL);
    ScanObject(obj);
  }
}

// Walks the reference list marking any references subject to the
// reference clearing policy.  References with a black referent are
// removed from the list.  References with white referents biased
// toward saving are blackened and also removed from the list.
void MarkSweep::PreserveSomeSoftReferences(Object** list) {
  DCHECK(list != NULL);
  Object* clear = NULL;
  size_t counter = 0;

  DCHECK(mark_stack_->IsEmpty());

  while (*list != NULL) {
    Object* ref = heap_->DequeuePendingReference(list);
    Object* referent = heap_->GetReferenceReferent(ref);
    if (referent == NULL) {
      // Referent was cleared by the user during marking.
      continue;
    }
    bool is_marked = IsMarked(referent);
    if (!is_marked && ((++counter) & 1)) {
      // Referent is white and biased toward saving, mark it.
      MarkObject(referent);
      is_marked = true;
    }
    if (!is_marked) {
      // Referent is white, queue it for clearing.
      heap_->EnqueuePendingReference(ref, &clear);
    }
  }
  *list = clear;
  // Restart the mark with the newly black references added to the
  // root set.
  ProcessMarkStack();
}

// Unlink the reference list clearing references objects with white
// referents.  Cleared references registered to a reference queue are
// scheduled for appending by the heap worker thread.
void MarkSweep::ClearWhiteReferences(Object** list) {
  DCHECK(list != NULL);
  while (*list != NULL) {
    Object* ref = heap_->DequeuePendingReference(list);
    Object* referent = heap_->GetReferenceReferent(ref);
    if (referent != NULL && !IsMarked(referent)) {
      // Referent is white, clear it.
      heap_->ClearReferenceReferent(ref);
      if (heap_->IsEnqueuable(ref)) {
        heap_->EnqueueReference(ref, &cleared_reference_list_);
      }
    }
  }
  DCHECK(*list == NULL);
}

// Enqueues finalizer references with white referents.  White
// referents are blackened, moved to the zombie field, and the
// referent field is cleared.
void MarkSweep::EnqueueFinalizerReferences(Object** list) {
  DCHECK(list != NULL);
  MemberOffset zombie_offset = heap_->GetFinalizerReferenceZombieOffset();
  bool has_enqueued = false;
  while (*list != NULL) {
    Object* ref = heap_->DequeuePendingReference(list);
    Object* referent = heap_->GetReferenceReferent(ref);
    if (referent != NULL && !IsMarked(referent)) {
      MarkObject(referent);
      // If the referent is non-null the reference must queuable.
      DCHECK(heap_->IsEnqueuable(ref));
      ref->SetFieldObject(zombie_offset, referent, false);
      heap_->ClearReferenceReferent(ref);
      heap_->EnqueueReference(ref, &cleared_reference_list_);
      has_enqueued = true;
    }
  }
  if (has_enqueued) {
    ProcessMarkStack();
  }
  DCHECK(*list == NULL);
}

// Process reference class instances and schedule finalizations.
void MarkSweep::ProcessReferences(Object** soft_references, bool clear_soft,
                                  Object** weak_references,
                                  Object** finalizer_references,
                                  Object** phantom_references) {
  DCHECK(soft_references != NULL);
  DCHECK(weak_references != NULL);
  DCHECK(finalizer_references != NULL);
  DCHECK(phantom_references != NULL);

  // Unless we are in the zygote or required to clear soft references
  // with white references, preserve some white referents.
  if (!clear_soft && !Runtime::Current()->IsZygote()) {
    PreserveSomeSoftReferences(soft_references);
  }

  // Clear all remaining soft and weak references with white
  // referents.
  ClearWhiteReferences(soft_references);
  ClearWhiteReferences(weak_references);

  // Preserve all white objects with finalize methods and schedule
  // them for finalization.
  EnqueueFinalizerReferences(finalizer_references);

  // Clear all f-reachable soft and weak references with white
  // referents.
  ClearWhiteReferences(soft_references);
  ClearWhiteReferences(weak_references);

  // Clear all phantom references with white referents.
  ClearWhiteReferences(phantom_references);

  // At this point all reference lists should be empty.
  DCHECK(*soft_references == NULL);
  DCHECK(*weak_references == NULL);
  DCHECK(*finalizer_references == NULL);
  DCHECK(*phantom_references == NULL);
}

MarkSweep::~MarkSweep() {
#ifndef NDEBUG
  VLOG(heap) << "MarkSweep scanned classes=" << class_count_ << " arrays=" << array_count_ << " other=" << other_count_;
#endif
  // Clear all of the alloc spaces' mark bitmaps.
  const Spaces& spaces = heap_->GetSpaces();
  // TODO: C++0x auto
  for (Spaces::const_iterator cur = spaces.begin(); cur != spaces.end(); ++cur) {
    if ((*cur)->GetGcRetentionPolicy() != GCRP_NEVER_COLLECT) {
      (*cur)->GetMarkBitmap()->Clear();
    }
  }
  mark_stack_->Reset();
}

}  // namespace art
