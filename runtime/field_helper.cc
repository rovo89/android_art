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

#include "field_helper.h"

#include "class_linker-inl.h"
#include "dex_file.h"
#include "mirror/dex_cache.h"
#include "runtime.h"
#include "thread-inl.h"

namespace art {

mirror::Class* FieldHelper::GetType(bool resolve) {
  uint32_t field_index = field_->GetDexFieldIndex();
  if (UNLIKELY(field_->GetDeclaringClass()->IsProxyClass())) {
    return Runtime::Current()->GetClassLinker()->FindSystemClass(Thread::Current(),
                                                                 field_->GetTypeDescriptor());
  }
  const DexFile* dex_file = field_->GetDexFile();
  const DexFile::FieldId& field_id = dex_file->GetFieldId(field_index);
  mirror::Class* type = field_->GetDexCache()->GetResolvedType(field_id.type_idx_);
  if (resolve && (type == nullptr)) {
    type = Runtime::Current()->GetClassLinker()->ResolveType(field_id.type_idx_, field_.Get());
    CHECK(type != nullptr || Thread::Current()->IsExceptionPending());
  }
  return type;
}

const char* FieldHelper::GetDeclaringClassDescriptor() {
  return field_->GetDeclaringClass()->GetDescriptor(&declaring_class_descriptor_);
}

}  // namespace art
