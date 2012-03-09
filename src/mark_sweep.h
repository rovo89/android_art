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

#include "macros.h"
#include "mark_stack.h"
#include "heap_bitmap.h"
#include "offsets.h"

namespace art {

class Class;
class Heap;
class Object;

class MarkSweep {
 public:
  MarkSweep() :
      mark_stack_(NULL),
      heap_(NULL),
      mark_bitmap_(NULL),
      live_bitmap_(NULL),
      finger_(NULL),
      condemned_(NULL),
      soft_reference_list_(NULL),
      weak_reference_list_(NULL),
      finalizer_reference_list_(NULL),
      phantom_reference_list_(NULL),
      cleared_reference_list_(NULL),
      class_count_(0), array_count_(0), other_count_(0) {
  }

  ~MarkSweep();

  // Initializes internal structures.
  void Init();

  // Marks the root set at the start of a garbage collection.
  void MarkRoots();

  // Marks the roots in the image space on dirty cards
  void ScanDirtyImageRoots();

  bool IsMarkStackEmpty() const {
    return mark_stack_->IsEmpty();
  }

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

  Object* GetClearedReferences() {
    return cleared_reference_list_;
  }

 private:
  // Returns true if the object has its bit set in the mark bitmap.
  bool IsMarked(const Object* object) const {
    return mark_bitmap_->Test(object);
  }

  static bool IsMarked(const Object* object, void* arg) {
    return reinterpret_cast<MarkSweep*>(arg)->IsMarked(object);
  }

  static void MarkObjectVisitor(const Object* root, void* arg);

  static void ScanImageRootVisitor(Object* root, void* arg);

  // Marks an object.
  void MarkObject(const Object* obj);

  // Yuck.
  void MarkObject0(const Object* obj, bool check_finger);

  static void ScanBitmapCallback(Object* obj, void* finger, void* arg);

  static void CheckBitmapCallback(Object* obj, void* finger, void* arg);

  static void SweepCallback(size_t num_ptrs, Object** ptrs, void* arg);

  void CheckReference(const Object* obj, const Object* ref, MemberOffset offset, bool is_static);

  // Blackens an object.
  void ScanObject(const Object* obj);

  void CheckObject(const Object* obj);

  // Grays references in instance fields.
  void ScanInstanceFields(const Object* obj);

  void CheckInstanceFields(const Object* obj);

  // Blackens a class object.
  void ScanClass(const Object* obj);

  void CheckClass(const Object* obj);

  // Grays references in static fields.
  void ScanStaticFields(const Class* klass);

  void CheckStaticFields(const Class* klass);

  // Used by ScanInstanceFields and ScanStaticFields
  void ScanFields(const Object* obj, uint32_t ref_offsets, bool is_static);

  void CheckFields(const Object* obj, uint32_t ref_offsets, bool is_static);

  // Grays references in an array.
  void ScanArray(const Object* obj);

  void CheckArray(const Object* obj);

  void ScanOther(const Object* obj);

  void CheckOther(const Object* obj);

  // Blackens objects grayed during a garbage collection.
  void ScanDirtyObjects();

  // Schedules an unmarked object for reference processing.
  void DelayReferenceReferent(Object* reference);

  // Recursively blackens objects on the mark stack.
  void ProcessMarkStack();

  void EnqueueFinalizerReferences(Object** ref);

  void PreserveSomeSoftReferences(Object** ref);

  void ClearWhiteReferences(Object** list);

  void ProcessReferences(Object** soft_references, bool clear_soft_references,
                         Object** weak_references,
                         Object** finalizer_references,
                         Object** phantom_references);

  void SweepSystemWeaks();
  void SweepJniWeakGlobals();

  MarkStack* mark_stack_;

  Heap* heap_;
  HeapBitmap* mark_bitmap_;
  HeapBitmap* live_bitmap_;

  Object* finger_;

  Object* condemned_;

  Object* soft_reference_list_;

  Object* weak_reference_list_;

  Object* finalizer_reference_list_;

  Object* phantom_reference_list_;

  Object* cleared_reference_list_;

  size_t class_count_;
  size_t array_count_;
  size_t other_count_;

  friend class InternTableEntryIsUnmarked;

  DISALLOW_COPY_AND_ASSIGN(MarkSweep);
};

}  // namespace art

#endif  // ART_SRC_MARK_SWEEP_H_
