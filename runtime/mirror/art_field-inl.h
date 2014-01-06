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

#ifndef ART_RUNTIME_MIRROR_ART_FIELD_INL_H_
#define ART_RUNTIME_MIRROR_ART_FIELD_INL_H_

#include "art_field.h"

#include "base/logging.h"
#include "gc/accounting/card_table-inl.h"
#include "jvalue.h"
#include "object-inl.h"
#include "object_utils.h"
#include "primitive.h"

namespace art {
namespace mirror {

inline Class* ArtField::GetDeclaringClass() {
  Class* result = GetFieldObject<Class>(OFFSET_OF_OBJECT_MEMBER(ArtField, declaring_class_), false);
  DCHECK(result != NULL);
  DCHECK(result->IsLoaded() || result->IsErroneous());
  return result;
}

inline void ArtField::SetDeclaringClass(Class *new_declaring_class) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(ArtField, declaring_class_), new_declaring_class, false);
}

inline uint32_t ArtField::GetAccessFlags() {
  DCHECK(GetDeclaringClass()->IsLoaded() || GetDeclaringClass()->IsErroneous());
  return GetField32(OFFSET_OF_OBJECT_MEMBER(ArtField, access_flags_), false);
}

inline MemberOffset ArtField::GetOffset() {
  DCHECK(GetDeclaringClass()->IsResolved() || GetDeclaringClass()->IsErroneous());
  return MemberOffset(GetField32(OFFSET_OF_OBJECT_MEMBER(ArtField, offset_), false));
}

inline MemberOffset ArtField::GetOffsetDuringLinking() {
  DCHECK(GetDeclaringClass()->IsLoaded() || GetDeclaringClass()->IsErroneous());
  return MemberOffset(GetField32(OFFSET_OF_OBJECT_MEMBER(ArtField, offset_), false));
}

inline uint32_t ArtField::Get32(Object* object) {
  DCHECK(object != NULL) << PrettyField(this);
  DCHECK(!IsStatic() || (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  return object->GetField32(GetOffset(), IsVolatile());
}

inline void ArtField::Set32(Object* object, uint32_t new_value) {
  DCHECK(object != NULL) << PrettyField(this);
  DCHECK(!IsStatic() || (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  object->SetField32(GetOffset(), new_value, IsVolatile());
}

inline uint64_t ArtField::Get64(Object* object) {
  DCHECK(object != NULL) << PrettyField(this);
  DCHECK(!IsStatic() || (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  return object->GetField64(GetOffset(), IsVolatile());
}

inline void ArtField::Set64(Object* object, uint64_t new_value) {
  DCHECK(object != NULL) << PrettyField(this);
  DCHECK(!IsStatic() || (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  object->SetField64(GetOffset(), new_value, IsVolatile());
}

inline Object* ArtField::GetObj(Object* object) {
  DCHECK(object != NULL) << PrettyField(this);
  DCHECK(!IsStatic() || (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  return object->GetFieldObject<Object>(GetOffset(), IsVolatile());
}

inline void ArtField::SetObj(Object* object, Object* new_value) {
  DCHECK(object != NULL) << PrettyField(this);
  DCHECK(!IsStatic() || (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  object->SetFieldObject(GetOffset(), new_value, IsVolatile());
}

inline bool ArtField::GetBoolean(Object* object) {
  DCHECK_EQ(Primitive::kPrimBoolean, FieldHelper(this).GetTypeAsPrimitiveType())
      << PrettyField(this);
  return Get32(object);
}

inline void ArtField::SetBoolean(Object* object, bool z) {
  DCHECK_EQ(Primitive::kPrimBoolean, FieldHelper(this).GetTypeAsPrimitiveType())
      << PrettyField(this);
  Set32(object, z);
}

inline int8_t ArtField::GetByte(Object* object) {
  DCHECK_EQ(Primitive::kPrimByte, FieldHelper(this).GetTypeAsPrimitiveType())
      << PrettyField(this);
  return Get32(object);
}

inline void ArtField::SetByte(Object* object, int8_t b) {
  DCHECK_EQ(Primitive::kPrimByte, FieldHelper(this).GetTypeAsPrimitiveType())
      << PrettyField(this);
  Set32(object, b);
}

inline uint16_t ArtField::GetChar(Object* object) {
  DCHECK_EQ(Primitive::kPrimChar, FieldHelper(this).GetTypeAsPrimitiveType())
      << PrettyField(this);
  return Get32(object);
}

inline void ArtField::SetChar(Object* object, uint16_t c) {
  DCHECK_EQ(Primitive::kPrimChar, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  Set32(object, c);
}

inline int16_t ArtField::GetShort(Object* object) {
  DCHECK_EQ(Primitive::kPrimShort, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  return Get32(object);
}

inline void ArtField::SetShort(Object* object, int16_t s) {
  DCHECK_EQ(Primitive::kPrimShort, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  Set32(object, s);
}

inline int32_t ArtField::GetInt(Object* object) {
#ifndef NDEBUG
  Primitive::Type type = FieldHelper(this).GetTypeAsPrimitiveType();
  CHECK(type == Primitive::kPrimInt || type == Primitive::kPrimFloat) << PrettyField(this);
#endif
  return Get32(object);
}

inline void ArtField::SetInt(Object* object, int32_t i) {
#ifndef NDEBUG
  Primitive::Type type = FieldHelper(this).GetTypeAsPrimitiveType();
  CHECK(type == Primitive::kPrimInt || type == Primitive::kPrimFloat) << PrettyField(this);
#endif
  Set32(object, i);
}

inline int64_t ArtField::GetLong(Object* object) {
#ifndef NDEBUG
  Primitive::Type type = FieldHelper(this).GetTypeAsPrimitiveType();
  CHECK(type == Primitive::kPrimLong || type == Primitive::kPrimDouble) << PrettyField(this);
#endif
  return Get64(object);
}

inline void ArtField::SetLong(Object* object, int64_t j) {
#ifndef NDEBUG
  Primitive::Type type = FieldHelper(this).GetTypeAsPrimitiveType();
  CHECK(type == Primitive::kPrimLong || type == Primitive::kPrimDouble) << PrettyField(this);
#endif
  Set64(object, j);
}

inline float ArtField::GetFloat(Object* object) {
  DCHECK_EQ(Primitive::kPrimFloat, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  JValue bits;
  bits.SetI(Get32(object));
  return bits.GetF();
}

inline void ArtField::SetFloat(Object* object, float f) {
  DCHECK_EQ(Primitive::kPrimFloat, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  JValue bits;
  bits.SetF(f);
  Set32(object, bits.GetI());
}

inline double ArtField::GetDouble(Object* object) {
  DCHECK_EQ(Primitive::kPrimDouble, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  JValue bits;
  bits.SetJ(Get64(object));
  return bits.GetD();
}

inline void ArtField::SetDouble(Object* object, double d) {
  DCHECK_EQ(Primitive::kPrimDouble, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  JValue bits;
  bits.SetD(d);
  Set64(object, bits.GetJ());
}

inline Object* ArtField::GetObject(Object* object) {
  DCHECK_EQ(Primitive::kPrimNot, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  return GetObj(object);
}

inline void ArtField::SetObject(Object* object, Object* l) {
  DCHECK_EQ(Primitive::kPrimNot, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  SetObj(object, l);
}

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_ART_FIELD_INL_H_
