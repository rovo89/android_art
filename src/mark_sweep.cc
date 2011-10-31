// Copyright 2011 Google Inc. All Rights Reserved.

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
#include "space.h"
#include "timing_logger.h"
#include "thread.h"

namespace art {

bool MarkSweep::Init() {
  mark_stack_ = MarkStack::Create();
  if (mark_stack_ == NULL) {
    return false;
  }

  mark_bitmap_ = Heap::GetMarkBits();
  live_bitmap_ = Heap::GetLiveBits();

  // TODO: if concurrent, clear the card table.

  // TODO: if concurrent, enable card marking in compiler

  // TODO: check that the mark bitmap is entirely clear.

  return true;
}

inline void MarkSweep::MarkObject0(const Object* obj, bool check_finger) {
  DCHECK(obj != NULL);
  if (obj < condemned_) {
    DCHECK(IsMarked(obj));
    return;
  }
  bool is_marked = mark_bitmap_->Test(obj);
  // This object was not previously marked.
  if (!is_marked) {
    mark_bitmap_->Set(obj);
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
inline void MarkSweep::MarkObject(const Object* obj) {
  if (obj != NULL) {
    MarkObject0(obj, true);
  }
}

void MarkSweep::MarkObjectVisitor(const Object* root, void* arg) {
  DCHECK(root != NULL);
  DCHECK(arg != NULL);
  MarkSweep* mark_sweep = reinterpret_cast<MarkSweep*>(arg);
  mark_sweep->MarkObject0(root, true);
}

// Marks all objects in the root set.
void MarkSweep::MarkRoots() {
  Runtime::Current()->VisitRoots(MarkObjectVisitor, this);
}

void MarkSweep::ScanBitmapCallback(Object* obj, void* finger, void* arg) {
  MarkSweep* mark_sweep = reinterpret_cast<MarkSweep*>(arg);
  mark_sweep->finger_ = reinterpret_cast<Object*>(finger);
  mark_sweep->ScanObject(obj);
}

// Populates the mark stack based on the set of marked objects and
// recursively marks until the mark stack is emptied.
void MarkSweep::RecursiveMark() {
  // RecursiveMark will build the lists of known instances of the Reference classes.
  // See DelayReferenceReferent for details.
  CHECK(soft_reference_list_ == NULL);
  CHECK(weak_reference_list_ == NULL);
  CHECK(finalizer_reference_list_ == NULL);
  CHECK(phantom_reference_list_ == NULL);
  CHECK(cleared_reference_list_ == NULL);

  TimingLogger timings("MarkSweep::RecursiveMark");
  void* arg = reinterpret_cast<void*>(this);
  const std::vector<Space*>& spaces = Heap::GetSpaces();
  for (size_t i = 0; i < spaces.size(); ++i) {
    if (!spaces[i]->IsImageSpace()) {
      uintptr_t base = reinterpret_cast<uintptr_t>(spaces[i]->GetBase());
      mark_bitmap_->ScanWalk(base, &MarkSweep::ScanBitmapCallback, arg);
    }
    timings.AddSplit(StringPrintf("ScanWalk space #%i (%s)", i, spaces[i]->GetName().c_str()));
  }
  finger_ = reinterpret_cast<Object*>(~0);
  ProcessMarkStack();
  timings.AddSplit("ProcessMarkStack");
  if (Heap::IsVerboseHeap()) {
    timings.Dump();
  }
}

void MarkSweep::ReMarkRoots() {
  UNIMPLEMENTED(FATAL);
}

void MarkSweep::SweepJniWeakGlobals() {
  JavaVMExt* vm = Runtime::Current()->GetJavaVM();
  MutexLock mu(vm->weak_globals_lock);
  IndirectReferenceTable* table = &vm->weak_globals;
  typedef IndirectReferenceTable::iterator It; // TODO: C++0x auto
  for (It it = table->begin(), end = table->end(); it != end; ++it) {
    const Object** entry = *it;
    if (!IsMarked(*entry)) {
      *entry = kClearedJniWeakGlobal;
    }
  }
}

void MarkSweep::SweepSystemWeaks() {
  Runtime::Current()->GetInternTable()->SweepInternTableWeaks(IsMarked, this);
  Runtime::Current()->GetMonitorList()->SweepMonitorList(IsMarked, this);
  SweepJniWeakGlobals();
}

void MarkSweep::SweepCallback(size_t num_ptrs, void** ptrs, void* arg) {
  // TODO: lock heap if concurrent
  size_t freed_objects = num_ptrs;
  size_t freed_bytes = 0;
  Space* space = static_cast<Space*>(arg);
  for (size_t i = 0; i < num_ptrs; ++i) {
    Object* obj = static_cast<Object*>(ptrs[i]);
    freed_bytes += space->AllocationSize(obj);
    Heap::GetLiveBits()->Clear(obj);
    space->Free(obj);
  }
  Heap::RecordFreeLocked(freed_objects, freed_bytes);
  // TODO: unlock heap if concurrent
}

void MarkSweep::Sweep() {
  SweepSystemWeaks();

  const std::vector<Space*>& spaces = Heap::GetSpaces();
  for (size_t i = 0; i < spaces.size(); ++i) {
    if (!spaces[i]->IsImageSpace()) {
      uintptr_t base = reinterpret_cast<uintptr_t>(spaces[i]->GetBase());
      uintptr_t limit = reinterpret_cast<uintptr_t>(spaces[i]->GetLimit());
      void* arg = static_cast<void*>(spaces[i]);
      HeapBitmap::SweepWalk(*live_bitmap_, *mark_bitmap_, base, limit,
                            &MarkSweep::SweepCallback, arg);
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
      const Object* element = array->Get(i);
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
  Object* pending = obj->GetFieldObject<Object*>(Heap::GetReferencePendingNextOffset(), false);
  Object* referent = Heap::GetReferenceReferent(obj);
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
    Heap::EnqueuePendingReference(obj, list);
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
inline void MarkSweep::ScanObject(const Object* obj) {
  DCHECK(obj != NULL);
  DCHECK(obj->GetClass() != NULL);
  DCHECK(IsMarked(obj));
  if (obj->IsClass()) {
    ScanClass(obj);
  } else if (obj->IsArrayInstance()) {
    ScanArray(obj);
  } else {
    ScanOther(obj);
  }
}

// Scan anything that's on the mark stack.  We can't use the bitmaps
// anymore, so use a finger that points past the end of them.
void MarkSweep::ProcessMarkStack() {
  while (!mark_stack_->IsEmpty()) {
    const Object* obj = mark_stack_->Pop();
    ScanObject(obj);
  }
}

void MarkSweep::ScanDirtyObjects() {
  ProcessMarkStack();
}

// Walks the reference list marking any references subject to the
// reference clearing policy.  References with a black referent are
// removed from the list.  References with white referents biased
// toward saving are blackened and also removed from the list.
void MarkSweep::PreserveSomeSoftReferences(Object** list) {
  DCHECK(list != NULL);
  Object* clear = NULL;
  size_t counter = 0;
  while (*list != NULL) {
    Object* ref = Heap::DequeuePendingReference(list);
    Object* referent = Heap::GetReferenceReferent(ref);
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
      Heap::EnqueuePendingReference(ref, &clear);
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
    Object* ref = Heap::DequeuePendingReference(list);
    Object* referent = Heap::GetReferenceReferent(ref);
    if (referent != NULL && !IsMarked(referent)) {
      // Referent is white, clear it.
      Heap::ClearReferenceReferent(ref);
      if (Heap::IsEnqueuable(ref)) {
        Heap::EnqueueReference(ref, &cleared_reference_list_);
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
  MemberOffset zombie_offset = Heap::GetFinalizerReferenceZombieOffset();
  bool has_enqueued = false;
  while (*list != NULL) {
    Object* ref = Heap::DequeuePendingReference(list);
    Object* referent = Heap::GetReferenceReferent(ref);
    if (referent != NULL && !IsMarked(referent)) {
      MarkObject(referent);
      // If the referent is non-null the reference must queuable.
      DCHECK(Heap::IsEnqueuable(ref));
      ref->SetFieldObject(zombie_offset, referent, false);
      Heap::ClearReferenceReferent(ref);
      Heap::EnqueueReference(ref, &cleared_reference_list_);
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
  if (clear_soft) {
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
  if (Heap::IsVerboseHeap()) {
    LOG(INFO) << "MarkSweep scanned classes=" << class_count_ << " arrays=" << array_count_ << " other=" << other_count_;
  }
#endif
  delete mark_stack_;
  mark_bitmap_->Clear();
}

}  // namespace art
