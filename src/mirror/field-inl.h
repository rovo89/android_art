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

#ifndef ART_SRC_MIRROR_FIELD_INL_H_
#define ART_SRC_MIRROR_FIELD_INL_H_

#include "field.h"

namespace art {
namespace mirror {

inline Class* Field::GetDeclaringClass() const {
  Class* result = GetFieldObject<Class*>(OFFSET_OF_OBJECT_MEMBER(Field, declaring_class_), false);
  DCHECK(result != NULL);
  DCHECK(result->IsLoaded() || result->IsErroneous());
  return result;
}

inline void Field::SetDeclaringClass(Class *new_declaring_class) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Field, declaring_class_), new_declaring_class, false);
}

inline uint32_t Field::GetAccessFlags() const {
  DCHECK(GetDeclaringClass()->IsLoaded() || GetDeclaringClass()->IsErroneous());
  return GetField32(OFFSET_OF_OBJECT_MEMBER(Field, access_flags_), false);
}

inline MemberOffset Field::GetOffset() const {
  DCHECK(GetDeclaringClass()->IsResolved() || GetDeclaringClass()->IsErroneous());
  return MemberOffset(GetField32(OFFSET_OF_OBJECT_MEMBER(Field, offset_), false));
}

inline MemberOffset Field::GetOffsetDuringLinking() const {
  DCHECK(GetDeclaringClass()->IsLoaded() || GetDeclaringClass()->IsErroneous());
  return MemberOffset(GetField32(OFFSET_OF_OBJECT_MEMBER(Field, offset_), false));
}

}  // namespace mirror
}  // namespace art

#endif  // ART_SRC_MIRROR_FIELD_INL_H_
