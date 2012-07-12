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
#include "object.h"
#include "offsets.h"

namespace art {

class CheckObjectVisitor;
class Class;
class Heap;
class MarkIfReachesAllocspaceVisitor;
class ModUnionClearCardVisitor;
class ModUnionVisitor;
class ModUnionTableBitmap;
class Object;

class MarkSweep {
 public:
  explicit MarkSweep(MarkStack* mark_stack);

  ~MarkSweep();

  // Initializes internal structures.
  void Init();

  // Marks the root set at the start of a garbage collection.
  void MarkRoots();

  // Marks the roots in the image space on dirty cards.
  void ScanDirtyImageRoots();

  // Verify that image roots point to only marked objects within the alloc space.
  void VerifyImageRoots();

  bool IsMarkStackEmpty() const {
    return mark_stack_->IsEmpty();
  }

  // Builds a mark stack and recursively mark until it empties.
  void RecursiveMark();

  // Builds a mark stack with objects on dirty cards and recursively mark
  // until it empties.
  void RecursiveMarkDirtyObjects();

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
    if (current_mark_bitmap_->HasAddress(object)) {
      return current_mark_bitmap_->Test(object);
    }
    return heap_->GetMarkBitmap()->Test(object);
  }

  static bool IsMarked(const Object* object, void* arg) {
    return reinterpret_cast<MarkSweep*>(arg)->IsMarked(object);
  }

  static void MarkObjectVisitor(const Object* root, void* arg);

  static void ReMarkObjectVisitor(const Object* root, void* arg);

  static void ScanImageRootVisitor(Object* root, void* arg);

  static void VerifyImageRootVisitor(Object* root, void* arg);

  static void ScanDirtyCardCallback(Object* obj, void* arg);

  // Marks an object.
  void MarkObject(const Object* obj);

  // Yuck.
  void MarkObject0(const Object* obj, bool check_finger);

  static void ScanBitmapCallback(Object* obj, void* finger, void* arg);

  static void CheckBitmapCallback(Object* obj, void* finger, void* arg);

  static void CheckBitmapNoFingerCallback(Object* obj, void* arg);

  static void SweepCallback(size_t num_ptrs, Object** ptrs, void* arg);

  void CheckReference(const Object* obj, const Object* ref, MemberOffset offset, bool is_static);

  // Blackens an object.
  void ScanObject(const Object* obj);

  void CheckObject(const Object* obj);

  template <typename Visitor>
  void VisitObjectReferences(const Object* obj, const Visitor& visitor) {
    DCHECK(obj != NULL);
    DCHECK(obj->GetClass() != NULL);
    if (obj->IsClass()) {
      VisitClassReferences(obj, visitor);
    } else if (obj->IsArrayInstance()) {
      VisitArrayReferences(obj, visitor);
    } else {
      VisitOtherReferences(obj, visitor);
    }
  }

  // Grays references in instance fields.
  void ScanInstanceFields(const Object* obj);

  template <typename Visitor>
  void VisitInstanceFieldsReferences(const Object* obj, const Visitor& visitor) {
    DCHECK(obj != NULL);
    Class* klass = obj->GetClass();
    DCHECK(klass != NULL);
    VisitFieldsReferences(obj, klass->GetReferenceInstanceOffsets(), false, visitor);
  }

  // Blackens a class object.
  void ScanClass(const Object* obj);

  template <typename Visitor>
  void VisitClassReferences(const Object* obj, const Visitor& visitor) {
    VisitInstanceFieldsReferences(obj, visitor);
    VisitStaticFieldsReferences(obj->AsClass(), visitor);
  }

  // Grays references in static fields.
  void ScanStaticFields(const Class* klass);

  template <typename Visitor>
  void VisitStaticFieldsReferences(const Class* klass, const Visitor& visitor) {
    DCHECK(klass != NULL);
    VisitFieldsReferences(klass, klass->GetReferenceStaticOffsets(), true, visitor);
  }

  // Used by ScanInstanceFields and ScanStaticFields
  void ScanFields(const Object* obj, uint32_t ref_offsets, bool is_static);

  template <typename Visitor>
  void VisitFieldsReferences(const Object* obj, uint32_t ref_offsets, bool is_static, const Visitor& visitor) {
    if (ref_offsets != CLASS_WALK_SUPER) {
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

  // Grays references in an array.
  void ScanArray(const Object* obj);

  template <typename Visitor>
  void VisitArrayReferences(const Object* obj, const Visitor& visitor) {
    visitor(obj, obj->GetClass(), Object::ClassOffset(), false);
    if (obj->IsObjectArray()) {
      const ObjectArray<Object>* array = obj->AsObjectArray<Object>();
      for (int32_t i = 0; i < array->GetLength(); ++i) {
        const Object* element = array->GetWithoutChecks(i);
        size_t width = sizeof(Object*);
        visitor(obj, element, MemberOffset(i * width + Array::DataOffset(width).Int32Value()), false);
      }
    }
  }

  void ScanOther(const Object* obj);

  template <typename Visitor>
  void VisitOtherReferences(const Object* obj, const Visitor& visitor) {
    return VisitInstanceFieldsReferences(obj, visitor);
  }

  // Blackens objects grayed during a garbage collection.
  void ScanGrayObjects();

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

  // Current space, we check this space first to avoid searching for the appropriate space for an object.
  SpaceBitmap* current_mark_bitmap_;

  MarkStack* mark_stack_;

  Heap* heap_;

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

  friend class CheckObjectVisitor;
  friend class InternTableEntryIsUnmarked;
  friend class MarkIfReachesAllocspaceVisitor;
  friend class ModUnionClearCardVisitor;
  friend class ModUnionVisitor;
  friend class ModUnionTableBitmap;

  DISALLOW_COPY_AND_ASSIGN(MarkSweep);
};

}  // namespace art

#endif  // ART_SRC_MARK_SWEEP_H_
