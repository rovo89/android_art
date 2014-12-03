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

#include "method_helper.h"

#include "class_linker.h"
#include "dex_file-inl.h"
#include "handle_scope-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/dex_cache.h"
#include "runtime.h"

namespace art {

template <template <class T> class HandleKind>
template <template <class T2> class HandleKind2>
bool MethodHelperT<HandleKind>::HasSameSignatureWithDifferentClassLoaders(Thread* self,
    MethodHelperT<HandleKind2>* other) {
  {
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> return_type(hs.NewHandle(GetMethod()->GetReturnType()));
    if (UNLIKELY(other->GetMethod()->GetReturnType() != return_type.Get())) {
      return false;
    }
  }
  const DexFile::TypeList* types = method_->GetParameterTypeList();
  const DexFile::TypeList* other_types = other->method_->GetParameterTypeList();
  if (types == nullptr) {
    return (other_types == nullptr) || (other_types->Size() == 0);
  } else if (UNLIKELY(other_types == nullptr)) {
    return types->Size() == 0;
  }
  uint32_t num_types = types->Size();
  if (UNLIKELY(num_types != other_types->Size())) {
    return false;
  }
  for (uint32_t i = 0; i < num_types; ++i) {
    mirror::Class* param_type =
        method_->GetClassFromTypeIndex(types->GetTypeItem(i).type_idx_, true);
    mirror::Class* other_param_type =
        other->method_->GetClassFromTypeIndex(other_types->GetTypeItem(i).type_idx_, true);
    if (UNLIKELY(param_type != other_param_type)) {
      return false;
    }
  }
  return true;
}

// Instantiate methods.
template
bool MethodHelperT<Handle>::HasSameSignatureWithDifferentClassLoaders<Handle>(Thread* self,
    MethodHelperT<Handle>* other);

template
bool MethodHelperT<Handle>::HasSameSignatureWithDifferentClassLoaders<MutableHandle>(Thread* self,
    MethodHelperT<MutableHandle>* other);

template
bool MethodHelperT<MutableHandle>::HasSameSignatureWithDifferentClassLoaders<Handle>(Thread* self,
    MethodHelperT<Handle>* other);

template
bool MethodHelperT<MutableHandle>::HasSameSignatureWithDifferentClassLoaders<MutableHandle>(
    Thread* self, MethodHelperT<MutableHandle>* other);

}  // namespace art
