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

}  // namespace mirror
}  // namespace art
