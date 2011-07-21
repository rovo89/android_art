// Copyright 2011 Google Inc. All Rights Reserved.
// Author: cshapiro@google.com (Carl Shapiro)

#include "mark_sweep.h"

#include "logging.h"
#include "macros.h"
#include "mark_stack.h"
#include "object.h"
#include "thread.h"

#define CLZ(x) __builtin_clz(x)

namespace art {

size_t MarkSweep::reference_referent_offset_ = 0;  // TODO
size_t MarkSweep::reference_queue_offset_ = 0;  // TODO
size_t MarkSweep::reference_queueNext_offset_ = 0;  // TODO
size_t MarkSweep::reference_pendingNext_offset_ = 0;  // TODO
size_t MarkSweep::finalizer_reference_zombie_offset_ = 0;  // TODO

void MarkSweep::MarkObject0(const Object* obj, bool check_finger) {
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
void MarkSweep::MarkObject(const Object* obj) {
  if (obj != NULL) {
    MarkObject0(obj, true);
  }
}

// Marks all objects in the root set.
void MarkSweep::MarkRoots() {
  LOG(FATAL) << "Unimplemented";
}

void MarkSweep::ReMarkRoots()
{
  LOG(FATAL) << "Unimplemented";
}

// Scans instance fields.
void MarkSweep::ScanInstanceFields(const Object* obj) {
  DCHECK(obj != NULL);
  DCHECK(obj->GetClass() != NULL);
  uint32_t ref_offsets = obj->GetClass()->GetReferenceOffsets();
  if (ref_offsets != CLASS_WALK_SUPER) {
    // Found a reference offset bitmap.  Mark the specified offsets.
    while (ref_offsets != 0) {
      size_t right_shift = CLZ(ref_offsets);
      size_t byte_offset = CLASS_OFFSET_FROM_CLZ(right_shift);
      const Object* ref = obj->GetFieldObject(byte_offset);
      MarkObject(ref);
      ref_offsets &= ~(CLASS_HIGH_BIT >> right_shift);
    }
  } else {
    // There is no reference offset bitmap for this class.  Walk up
    // the class inheritance hierarchy and find reference offsets the
    // hard way.
    for (Class *klass = obj->GetClass();
         klass != NULL;
         klass = klass->GetSuperClass()) {
      for (size_t i = 0; i < klass->NumReferenceInstanceFields(); ++i) {
        size_t field_offset = klass->GetInstanceField(i)->GetOffset();
        const Object* ref = obj->GetFieldObject(field_offset);
        MarkObject(ref);
      }
    }
  }
}

// Scans the static fields of a class object.
void MarkSweep::ScanStaticFields(const Class* klass) {
  DCHECK(klass != NULL);
  for (size_t i = 0; i < klass->NumStaticFields(); ++i) {
    // char ch = clazz->sfields[i].signature[0];
    const StaticField* static_field = klass->GetStaticField(i);
    char ch = static_field->GetType();
    if (ch == '[' || ch == 'L') {
      // Object *obj = clazz->sfields[i].value.l;
      // markObject(obj, ctx);
      const Object* obj = static_field->GetObject();
      MarkObject(obj);
    }
  }
}

void MarkSweep::ScanInterfaces(const Class* klass) {
  DCHECK(klass != NULL);
  for (size_t i = 0; i < klass->NumInterfaces(); ++i) {
    MarkObject(klass->GetInterface(i));
  }
}

// Scans the header, static field references, and interface pointers
// of a class object.
void MarkSweep::ScanClass(const Object* obj) {
  DCHECK(obj != NULL);
  DCHECK(obj->IsClass());
  const Class* klass = obj->AsClass();
  MarkObject(klass->GetClass());
  if (klass->IsArray()) {
    MarkObject(klass->GetComponentType());
  }
  if (klass->IsLoaded()) {
    MarkObject(klass->GetSuperClass());
  }
  MarkObject(klass->GetClassLoader());
  ScanInstanceFields(obj);
  ScanStaticFields(klass);
  // TODO: scan methods
  // TODO: scan instance fields
  if (klass->IsLoaded()) {
    ScanInterfaces(klass);
  }
}

// Scans the header of all array objects.  If the array object is
// specialized to a reference type, scans the array data as well.
void MarkSweep::ScanArray(const Object *obj) {
  DCHECK(obj != NULL);
  DCHECK(obj->GetClass() != NULL);
  MarkObject(obj->GetClass());
  if (obj->IsObjectArray()) {
    const ObjectArray* array = obj->AsObjectArray();
    for (size_t i = 0; i < array->GetLength(); ++i) {
      const Object* element = array->Get(i);
      MarkObject(element);
    }
  }
}

void MarkSweep::EnqueuePendingReference(Object* ref, Object** list) {
  DCHECK(ref != NULL);
  DCHECK(list != NULL);
  size_t offset = reference_pendingNext_offset_;
  if (*list == NULL) {
    ref->SetFieldObject(offset, ref);
    *list = ref;
  } else {
    Object *head = (*list)->GetFieldObject(offset);
    ref->SetFieldObject(offset, head);
    (*list)->SetFieldObject(offset, ref);
  }
}

Object* MarkSweep::DequeuePendingReference(Object** list) {
  DCHECK(list != NULL);
  DCHECK(*list != NULL);
  size_t offset = reference_pendingNext_offset_;
  Object* head = (*list)->GetFieldObject(offset);
  Object* ref;
  if (*list == head) {
    ref = *list;
    *list = NULL;
  } else {
    Object *next = head->GetFieldObject(offset);
    (*list)->SetFieldObject(offset, next);
    ref = head;
  }
  ref->SetFieldObject(offset, NULL);
  return ref;
}

// Process the "referent" field in a java.lang.ref.Reference.  If the
// referent has not yet been marked, put it on the appropriate list in
// the gcHeap for later processing.
void MarkSweep::DelayReferenceReferent(Object* obj) {
  DCHECK(obj != NULL);
  DCHECK(obj->GetClass() != NULL);
  //DCHECK(IS_CLASS_FLAG_SET(obj->clazz, CLASS_ISREFERENCE));
  Object* pending = obj->GetFieldObject(reference_pendingNext_offset_);
  Object* referent = obj->GetFieldObject(reference_referent_offset_);
  if (pending == NULL && referent != NULL && !IsMarked(referent)) {
    Object **list = NULL;
    if (obj->IsSoftReference()) {
      list = &soft_reference_list_;
    } else if (obj->IsWeakReference()) {
      list = &weak_reference_list_;
    } else if (obj->IsFinalizerReference()) {
      list = &finalizer_reference_list_;
    } else if (obj->IsPhantomReference()) {
      list = &phantom_reference_list_;
    }
    DCHECK(list != NULL);
    EnqueuePendingReference(obj, list);
  }
}

// Scans the header and field references of a data object.  If the
// scanned object is a reference subclass, it is scheduled for later
// processing
void MarkSweep::ScanDataObject(const Object *obj) {
  DCHECK(obj != NULL);
  DCHECK(obj->GetClass() != NULL);
  MarkObject(obj->GetClass());
  ScanInstanceFields(obj);
  if (obj->IsReference()) {
    DelayReferenceReferent(const_cast<Object*>(obj));
  }
}

// Scans an object reference.  Determines the type of the reference
// and dispatches to a specialized scanning routine.
void MarkSweep::ScanObject(const Object* obj) {
  DCHECK(obj != NULL);
  DCHECK(obj->GetClass() != NULL);
  DCHECK(IsMarked(obj));
  if (obj->IsClass()) {
    ScanClass(obj);
  } else if (obj->IsArray()) {
    ScanArray(obj);
  } else {
    ScanDataObject(obj);
  }
}

// Scan anything that's on the mark stack.  We can't use the bitmaps
// anymore, so use a finger that points past the end of them.
void MarkSweep::ProcessMarkStack() {
  while (!mark_stack_->IsEmpty()) {
    const Object *obj = mark_stack_->Pop();
    ScanObject(obj);
  }
}

void MarkSweep::ScanDirtyObjects() {
  ProcessMarkStack();
}

void MarkSweep::ClearReference(Object* ref) {
  DCHECK(ref != NULL);
  ref->SetFieldObject(reference_referent_offset_, NULL);
}

bool MarkSweep::IsEnqueuable(const Object* ref) {
  DCHECK(ref != NULL);
  const Object* queue = ref->GetFieldObject(reference_queue_offset_);
  const Object* queue_next = ref->GetFieldObject(reference_queueNext_offset_);
  return (queue != NULL) && (queue_next == NULL);
}

void MarkSweep::EnqueueReference(Object* ref) {
  DCHECK(ref != NULL);
  CHECK(ref->GetFieldObject(reference_queue_offset_) != NULL);
  CHECK(ref->GetFieldObject(reference_queueNext_offset_) == NULL);
  EnqueuePendingReference(ref, &cleared_reference_list_);
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
    Object* ref = DequeuePendingReference(list);
    Object* referent = ref->GetFieldObject(reference_referent_offset_);
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
      EnqueuePendingReference(ref, &clear);
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
  size_t offset = reference_referent_offset_;
  while (*list != NULL) {
    Object *ref = DequeuePendingReference(list);
    Object *referent = ref->GetFieldObject(offset);
    if (referent != NULL && !IsMarked(referent)) {
      // Referent is white, clear it.
      ClearReference(ref);
      if (IsEnqueuable(ref)) {
        EnqueueReference(ref);
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
  size_t referent_offset = reference_referent_offset_;
  size_t zombie_offset = finalizer_reference_zombie_offset_;
  bool has_enqueued = false;
  while (*list != NULL) {
    Object* ref = DequeuePendingReference(list);
    Object* referent = ref->GetFieldObject(referent_offset);
    if (referent != NULL && !IsMarked(referent)) {
      MarkObject(referent);
      // If the referent is non-null the reference must queuable.
      DCHECK(IsEnqueuable(ref));
      ref->SetFieldObject(zombie_offset, referent);
      ClearReference(ref);
      EnqueueReference(ref);
      has_enqueued = true;
    }
  }
  if (has_enqueued) {
    ProcessMarkStack();
  }
  DCHECK(*list == NULL);
}

/*
 * Process reference class instances and schedule finalizations.
 */
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

// Pushes a list of cleared references out to the managed heap.
void MarkSweep::EnqueueClearedReferences(Object** cleared) {
  DCHECK(cleared != NULL);
  if (*cleared != NULL) {
    Thread* self = Thread::Current();
    DCHECK(self != NULL);
    // TODO: Method *meth = gDvm.methJavaLangRefReferenceQueueAdd;
    // DCHECK(meth != NULL);
    // JValue unused;
    // Object *reference = *cleared;
    // TODO: dvmCallMethod(self, meth, NULL, &unused, reference);
    LOG(FATAL) << "Unimplemented";
    *cleared = NULL;
  }
}

MarkSweep::~MarkSweep() {
  mark_bitmap_->Clear();
}

}  // namespace art
