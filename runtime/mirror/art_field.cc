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

#include "art_field.h"

#include "art_field-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "object-inl.h"
#include "object_utils.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "utils.h"
#include "well_known_classes.h"

namespace art {
namespace mirror {

// TODO: get global references for these
Class* ArtField::java_lang_reflect_ArtField_ = NULL;

ArtField* ArtField::FromReflectedField(const ScopedObjectAccess& soa, jobject jlr_field) {
  mirror::ArtField* f = soa.DecodeField(WellKnownClasses::java_lang_reflect_Field_artField);
  mirror::ArtField* field = f->GetObject(soa.Decode<mirror::Object*>(jlr_field))->AsArtField();
  DCHECK(field != nullptr);
  return field;
}

void ArtField::SetClass(Class* java_lang_reflect_ArtField) {
  CHECK(java_lang_reflect_ArtField_ == NULL);
  CHECK(java_lang_reflect_ArtField != NULL);
  java_lang_reflect_ArtField_ = java_lang_reflect_ArtField;
}

void ArtField::ResetClass() {
  CHECK(java_lang_reflect_ArtField_ != NULL);
  java_lang_reflect_ArtField_ = NULL;
}

void ArtField::SetOffset(MemberOffset num_bytes) {
  DCHECK(GetDeclaringClass()->IsLoaded() || GetDeclaringClass()->IsErroneous());
  if (kIsDebugBuild && Runtime::Current()->IsCompiler() &&
      !Runtime::Current()->UseCompileTimeClassPath()) {
    Primitive::Type type = FieldHelper(this).GetTypeAsPrimitiveType();
    if (type == Primitive::kPrimDouble || type == Primitive::kPrimLong) {
      DCHECK_ALIGNED(num_bytes.Uint32Value(), 8);
    }
  }
  // Not called within a transaction.
  SetField32<false>(OFFSET_OF_OBJECT_MEMBER(ArtField, offset_), num_bytes.Uint32Value(), false);
}

void ArtField::VisitRoots(RootCallback* callback, void* arg) {
  if (java_lang_reflect_ArtField_ != nullptr) {
    callback(reinterpret_cast<mirror::Object**>(&java_lang_reflect_ArtField_), arg, 0,
             kRootStickyClass);
  }
}

}  // namespace mirror
}  // namespace art
