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

#include "field.h"

#include "field-inl.h"
#include "gc/card_table-inl.h"
#include "object-inl.h"
#include "object_utils.h"
#include "runtime.h"
#include "utils.h"

namespace art {
namespace mirror {

// TODO: get global references for these
Class* Field::java_lang_reflect_Field_ = NULL;

void Field::SetClass(Class* java_lang_reflect_Field) {
  CHECK(java_lang_reflect_Field_ == NULL);
  CHECK(java_lang_reflect_Field != NULL);
  java_lang_reflect_Field_ = java_lang_reflect_Field;
}

void Field::ResetClass() {
  CHECK(java_lang_reflect_Field_ != NULL);
  java_lang_reflect_Field_ = NULL;
}

void Field::SetOffset(MemberOffset num_bytes) {
  DCHECK(GetDeclaringClass()->IsLoaded() || GetDeclaringClass()->IsErroneous());
#if 0 // TODO enable later in boot and under !NDEBUG
  FieldHelper fh(this);
  Primitive::Type type = fh.GetTypeAsPrimitiveType();
  if (type == Primitive::kPrimDouble || type == Primitive::kPrimLong) {
    DCHECK_ALIGNED(num_bytes.Uint32Value(), 8);
  }
#endif
  SetField32(OFFSET_OF_OBJECT_MEMBER(Field, offset_), num_bytes.Uint32Value(), false);
}

uint32_t Field::Get32(const Object* object) const {
  DCHECK(object != NULL) << PrettyField(this);
  DCHECK(!IsStatic() || (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  return object->GetField32(GetOffset(), IsVolatile());
}

void Field::Set32(Object* object, uint32_t new_value) const {
  DCHECK(object != NULL) << PrettyField(this);
  DCHECK(!IsStatic() || (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  object->SetField32(GetOffset(), new_value, IsVolatile());
}

uint64_t Field::Get64(const Object* object) const {
  DCHECK(object != NULL) << PrettyField(this);
  DCHECK(!IsStatic() || (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  return object->GetField64(GetOffset(), IsVolatile());
}

void Field::Set64(Object* object, uint64_t new_value) const {
  DCHECK(object != NULL) << PrettyField(this);
  DCHECK(!IsStatic() || (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  object->SetField64(GetOffset(), new_value, IsVolatile());
}

Object* Field::GetObj(const Object* object) const {
  DCHECK(object != NULL) << PrettyField(this);
  DCHECK(!IsStatic() || (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  return object->GetFieldObject<Object*>(GetOffset(), IsVolatile());
}

void Field::SetObj(Object* object, const Object* new_value) const {
  DCHECK(object != NULL) << PrettyField(this);
  DCHECK(!IsStatic() || (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  object->SetFieldObject(GetOffset(), new_value, IsVolatile());
}

bool Field::GetBoolean(const Object* object) const {
  DCHECK_EQ(Primitive::kPrimBoolean, FieldHelper(this).GetTypeAsPrimitiveType())
      << PrettyField(this);
  return Get32(object);
}

void Field::SetBoolean(Object* object, bool z) const {
  DCHECK_EQ(Primitive::kPrimBoolean, FieldHelper(this).GetTypeAsPrimitiveType())
      << PrettyField(this);
  Set32(object, z);
}

int8_t Field::GetByte(const Object* object) const {
  DCHECK_EQ(Primitive::kPrimByte, FieldHelper(this).GetTypeAsPrimitiveType())
      << PrettyField(this);
  return Get32(object);
}

void Field::SetByte(Object* object, int8_t b) const {
  DCHECK_EQ(Primitive::kPrimByte, FieldHelper(this).GetTypeAsPrimitiveType())
      << PrettyField(this);
  Set32(object, b);
}

uint16_t Field::GetChar(const Object* object) const {
  DCHECK_EQ(Primitive::kPrimChar, FieldHelper(this).GetTypeAsPrimitiveType())
      << PrettyField(this);
  return Get32(object);
}

void Field::SetChar(Object* object, uint16_t c) const {
  DCHECK_EQ(Primitive::kPrimChar, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  Set32(object, c);
}

int16_t Field::GetShort(const Object* object) const {
  DCHECK_EQ(Primitive::kPrimShort, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  return Get32(object);
}

void Field::SetShort(Object* object, int16_t s) const {
  DCHECK_EQ(Primitive::kPrimShort, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  Set32(object, s);
}

int32_t Field::GetInt(const Object* object) const {
#ifndef NDEBUG
  Primitive::Type type = FieldHelper(this).GetTypeAsPrimitiveType();
  CHECK(type == Primitive::kPrimInt || type == Primitive::kPrimFloat) << PrettyField(this);
#endif
  return Get32(object);
}

void Field::SetInt(Object* object, int32_t i) const {
#ifndef NDEBUG
  Primitive::Type type = FieldHelper(this).GetTypeAsPrimitiveType();
  CHECK(type == Primitive::kPrimInt || type == Primitive::kPrimFloat) << PrettyField(this);
#endif
  Set32(object, i);
}

int64_t Field::GetLong(const Object* object) const {
#ifndef NDEBUG
  Primitive::Type type = FieldHelper(this).GetTypeAsPrimitiveType();
  CHECK(type == Primitive::kPrimLong || type == Primitive::kPrimDouble) << PrettyField(this);
#endif
  return Get64(object);
}

void Field::SetLong(Object* object, int64_t j) const {
#ifndef NDEBUG
  Primitive::Type type = FieldHelper(this).GetTypeAsPrimitiveType();
  CHECK(type == Primitive::kPrimLong || type == Primitive::kPrimDouble) << PrettyField(this);
#endif
  Set64(object, j);
}

union Bits {
  jdouble d;
  jfloat f;
  jint i;
  jlong j;
};

float Field::GetFloat(const Object* object) const {
  DCHECK_EQ(Primitive::kPrimFloat, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  Bits bits;
  bits.i = Get32(object);
  return bits.f;
}

void Field::SetFloat(Object* object, float f) const {
  DCHECK_EQ(Primitive::kPrimFloat, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  Bits bits;
  bits.f = f;
  Set32(object, bits.i);
}

double Field::GetDouble(const Object* object) const {
  DCHECK_EQ(Primitive::kPrimDouble, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  Bits bits;
  bits.j = Get64(object);
  return bits.d;
}

void Field::SetDouble(Object* object, double d) const {
  DCHECK_EQ(Primitive::kPrimDouble, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  Bits bits;
  bits.d = d;
  Set64(object, bits.j);
}

Object* Field::GetObject(const Object* object) const {
  DCHECK_EQ(Primitive::kPrimNot, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  return GetObj(object);
}

void Field::SetObject(Object* object, const Object* l) const {
  DCHECK_EQ(Primitive::kPrimNot, FieldHelper(this).GetTypeAsPrimitiveType())
       << PrettyField(this);
  SetObj(object, l);
}

}  // namespace mirror
}  // namespace art
