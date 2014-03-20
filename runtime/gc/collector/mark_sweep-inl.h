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

#ifndef ART_RUNTIME_GC_COLLECTOR_MARK_SWEEP_INL_H_
#define ART_RUNTIME_GC_COLLECTOR_MARK_SWEEP_INL_H_

#include "gc/collector/mark_sweep.h"

#include "gc/heap.h"
#include "mirror/art_field.h"
#include "mirror/class.h"
#include "mirror/object_array.h"

namespace art {
namespace gc {
namespace collector {

template <typename MarkVisitor>
inline void MarkSweep::ScanObjectVisit(mirror::Object* obj, const MarkVisitor& visitor) {
  if (kIsDebugBuild && !IsMarked(obj)) {
    heap_->DumpSpaces();
    LOG(FATAL) << "Scanning unmarked object " << obj;
  }
  // The GetClass verifies the object, don't need to reverify after.
  mirror::Class* klass = obj->GetClass();
  // IsArrayClass verifies klass.
  if (UNLIKELY(klass->IsArrayClass())) {
    if (kCountScannedTypes) {
      ++array_count_;
    }
    if (klass->IsObjectArrayClass<kVerifyNone>()) {
      VisitObjectArrayReferences(obj->AsObjectArray<mirror::Object, kVerifyNone>(), visitor);
    }
  } else if (UNLIKELY(klass == mirror::Class::GetJavaLangClass())) {
    if (kCountScannedTypes) {
      ++class_count_;
    }
    VisitClassReferences<false>(klass, obj, visitor);
  } else {
    if (kCountScannedTypes) {
      ++other_count_;
    }
    VisitInstanceFieldsReferences<false>(klass, obj, visitor);
    if (UNLIKELY(klass->IsReferenceClass<kVerifyNone>())) {
      DelayReferenceReferent(klass, obj);
    }
  }
}

template <bool kVisitClass, typename Visitor>
inline void MarkSweep::VisitObjectReferences(mirror::Object* obj, const Visitor& visitor)
    SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_,
                          Locks::mutator_lock_) {
  mirror::Class* klass = obj->GetClass();
  if (klass->IsArrayClass()) {
    if (kVisitClass) {
      visitor(obj, klass, mirror::Object::ClassOffset(), false);
    }
    if (klass->IsObjectArrayClass<kVerifyNone>()) {
      VisitObjectArrayReferences(obj->AsObjectArray<mirror::Object, kVerifyNone>(), visitor);
    }
  } else if (klass == mirror::Class::GetJavaLangClass()) {
    DCHECK_EQ(klass->GetClass<kVerifyNone>(), mirror::Class::GetJavaLangClass());
    VisitClassReferences<kVisitClass>(klass, obj, visitor);
  } else {
    VisitInstanceFieldsReferences<kVisitClass>(klass, obj, visitor);
  }
}

template <bool kVisitClass, typename Visitor>
inline void MarkSweep::VisitInstanceFieldsReferences(mirror::Class* klass,
                                                     mirror::Object* obj, const Visitor& visitor)
    SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
  VisitFieldsReferences<kVisitClass>(obj, klass->GetReferenceInstanceOffsets<kVerifyNone>(), false,
                                     visitor);
}

template <bool kVisitClass, typename Visitor>
inline void MarkSweep::VisitClassReferences(mirror::Class* klass, mirror::Object* obj,
                                            const Visitor& visitor)
    SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
  VisitInstanceFieldsReferences<kVisitClass>(klass, obj, visitor);
  VisitStaticFieldsReferences<kVisitClass>(obj->AsClass<kVerifyNone>(), visitor);
}

template <bool kVisitClass, typename Visitor>
inline void MarkSweep::VisitStaticFieldsReferences(mirror::Class* klass, const Visitor& visitor)
    SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
  VisitFieldsReferences<kVisitClass>(klass, klass->GetReferenceStaticOffsets<kVerifyNone>(), true,
                                     visitor);
}

template <bool kVisitClass, typename Visitor>
inline void MarkSweep::VisitFieldsReferences(mirror::Object* obj, uint32_t ref_offsets,
                                             bool is_static, const Visitor& visitor) {
  if (LIKELY(ref_offsets != CLASS_WALK_SUPER)) {
    if (!kVisitClass) {
      // Currently the class bit is always set in the word. Since we count leading zeros to find
      // the offset and the class bit is at offset 0, it means that the highest bit is the class
      // bit. We can quickly clear this using xor.
      ref_offsets ^= kWordHighBitMask;
      DCHECK_EQ(mirror::Object::ClassOffset().Uint32Value(), 0U);
    }
    // Found a reference offset bitmap.  Mark the specified offsets.
    while (ref_offsets != 0) {
      size_t right_shift = CLZ(ref_offsets);
      MemberOffset field_offset = CLASS_OFFSET_FROM_CLZ(right_shift);
      mirror::Object* ref = obj->GetFieldObject<mirror::Object, kVerifyReads>(field_offset, false);
      visitor(obj, ref, field_offset, is_static);
      ref_offsets &= ~(CLASS_HIGH_BIT >> right_shift);
    }
  } else {
    // There is no reference offset bitmap.  In the non-static case,
    // walk up the class inheritance hierarchy and find reference
    // offsets the hard way. In the static case, just consider this
    // class.
    for (mirror::Class* klass = is_static ? obj->AsClass<kVerifyNone>() : obj->GetClass<kVerifyNone>();
         klass != nullptr;
         klass = is_static ? nullptr : klass->GetSuperClass()) {
      size_t num_reference_fields = (is_static
                                     ? klass->NumReferenceStaticFields()
                                     : klass->NumReferenceInstanceFields());
      for (size_t i = 0; i < num_reference_fields; ++i) {
        mirror::ArtField* field = (is_static ? klass->GetStaticField(i)
                                             : klass->GetInstanceField(i));
        MemberOffset field_offset = field->GetOffset();
        mirror::Object* ref = obj->GetFieldObject<mirror::Object, kVerifyReads>(field_offset, false);
        visitor(obj, ref, field_offset, is_static);
      }
    }
  }
}

template <typename Visitor>
inline void MarkSweep::VisitObjectArrayReferences(mirror::ObjectArray<mirror::Object>* array,
                                                  const Visitor& visitor) {
  const size_t length = static_cast<size_t>(array->GetLength());
  for (size_t i = 0; i < length; ++i) {
    mirror::Object* element = array->GetWithoutChecks(static_cast<int32_t>(i));
    const size_t width = sizeof(mirror::HeapReference<mirror::Object>);
    MemberOffset offset(i * width + mirror::Array::DataOffset(width).Int32Value());
    visitor(array, element, offset, false);
  }
}

}  // namespace collector
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_COLLECTOR_MARK_SWEEP_INL_H_
