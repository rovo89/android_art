// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_MARK_SWEEP_H_
#define ART_SRC_MARK_SWEEP_H_

#include "macros.h"
#include "mark_stack.h"
#include "object_bitmap.h"

namespace art {

class Class;
class Object;

class MarkSweep {
 public:
  MarkSweep() :
      finger_(NULL),
      condemned_(NULL),
      soft_reference_list_(NULL),
      weak_reference_list_(NULL),
      finalizer_reference_list_(NULL),
      phantom_reference_list_(NULL),
      cleared_reference_list_(NULL) {
  }

  ~MarkSweep();

  // Initializes internal structures.
  bool Init();

  // Marks the root set at the start of a garbage collection.
  void MarkRoots();

  // Builds a mark stack and recursively mark until it empties.
  void RecursiveMark();

  // Remarks the root set after completing the concurrent mark.
  void ReMarkRoots();

  void ProcessReferences(bool clear_soft_references) {
    ProcessReferences(&soft_reference_list_, clear_soft_references,
                      &weak_reference_list_,
                      &finalizer_reference_list_,
                      &phantom_reference_list_);
  }

  // Sweeps unmarked objects to complete the garbage collection.
  void Sweep();

 private:
  // Returns true if the object has its bit set in the mark bitmap.
  bool IsMarked(const Object* object) const {
    return mark_bitmap_->Test(object);
  }

  static void MarkObjectVisitor(const Object* root, void* arg);

  // Marks an object.
  void MarkObject(const Object* obj);

  // Yuck.
  void MarkObject0(const Object* obj, bool check_finger);

  static void ScanBitmapCallback(Object* obj, void* finger, void* arg);

  static void SweepCallback(size_t num_ptrs, void** ptrs, void* arg);

  // Blackens an object.
  void ScanObject(const Object* obj);

  // Grays references in instance fields.
  void ScanInstanceFields(const Object* obj);

  // Blackens a class object.
  void ScanClass(const Object* obj);

  // Grays references in static fields.
  void ScanStaticFields(const Class* klass);

  // Used by ScanInstanceFields and ScanStaticFields
  void ScanFields(const Object* obj, uint32_t ref_offsets, bool is_static);

  // Grays interface class objects.
  void ScanInterfaces(const Class* klass);

  // Grays references in an array.
  void ScanArray(const Object* obj);

  void ScanOther(const Object* obj);

  // Blackens objects grayed during a garbage collection.
  void ScanDirtyObjects();

  // Schedules an unmarked object for reference processing.
  void DelayReferenceReferent(Object* reference);

  // Recursively blackens objects on the mark stack.
  void ProcessMarkStack();

  // Adds a reference to the tail of a circular queue of references.
  static void EnqueuePendingReference(Object* ref, Object** list);

  // Removes the reference at the head of a circular queue of
  // references.
  static Object* DequeuePendingReference(Object** list);

  // Sets the referent field of a reference object to null.
  static void ClearReference(Object* reference);

  // Returns true if the reference object has not yet been enqueued.
  static bool IsEnqueuable(const Object* ref);

  void EnqueueReference(Object* ref);

  void EnqueueFinalizerReferences(Object** ref);

  void PreserveSomeSoftReferences(Object** ref);

  void EnqueueClearedReferences(Object** cleared_references);

  void ClearWhiteReferences(Object** list);

  void ProcessReferences(Object** soft_references, bool clear_soft_references,
                         Object** weak_references,
                         Object** finalizer_references,
                         Object** phantom_references);

  void SweepSystemWeaks();

  MarkStack* mark_stack_;

  HeapBitmap* mark_bitmap_;

  HeapBitmap* live_bitmap_;

  Object* finger_;

  Object* condemned_;

  Object* soft_reference_list_;

  Object* weak_reference_list_;

  Object* finalizer_reference_list_;

  Object* phantom_reference_list_;

  Object* cleared_reference_list_;

  DISALLOW_COPY_AND_ASSIGN(MarkSweep);
};

}  // namespace art

#endif  // ART_SRC_MARK_SWEEP_H_
