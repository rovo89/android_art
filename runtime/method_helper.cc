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

#include "method_helper-inl.h"

#include "class_linker.h"
#include "dex_file-inl.h"
#include "handle_scope-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/dex_cache.h"
#include "runtime.h"

namespace art {

mirror::String* MethodHelper::GetNameAsString(Thread* self) {
  const DexFile* dex_file = method_->GetDexFile();
  mirror::ArtMethod* method = method_->GetInterfaceMethodIfProxy();
  uint32_t dex_method_idx = method->GetDexMethodIndex();
  const DexFile::MethodId& method_id = dex_file->GetMethodId(dex_method_idx);
  StackHandleScope<1> hs(self);
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(method->GetDexCache()));
  return Runtime::Current()->GetClassLinker()->ResolveString(*dex_file, method_id.name_idx_,
                                                             dex_cache);
}

bool MethodHelper::HasSameSignatureWithDifferentClassLoaders(MethodHelper* other) {
  if (UNLIKELY(GetReturnType() != other->GetReturnType())) {
    return false;
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
    mirror::Class* param_type = GetClassFromTypeIdx(types->GetTypeItem(i).type_idx_);
    mirror::Class* other_param_type =
        other->GetClassFromTypeIdx(other_types->GetTypeItem(i).type_idx_);
    if (UNLIKELY(param_type != other_param_type)) {
      return false;
    }
  }
  return true;
}

uint32_t MethodHelper::FindDexMethodIndexInOtherDexFile(const DexFile& other_dexfile)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtMethod* method = GetMethod();
  const DexFile* dexfile = method->GetDexFile();
  if (dexfile == &other_dexfile) {
    return method->GetDexMethodIndex();
  }
  const DexFile::MethodId& mid = dexfile->GetMethodId(method->GetDexMethodIndex());
  const char* mid_declaring_class_descriptor = dexfile->StringByTypeIdx(mid.class_idx_);
  const DexFile::StringId* other_descriptor =
      other_dexfile.FindStringId(mid_declaring_class_descriptor);
  if (other_descriptor != nullptr) {
    const DexFile::TypeId* other_type_id =
        other_dexfile.FindTypeId(other_dexfile.GetIndexForStringId(*other_descriptor));
    if (other_type_id != nullptr) {
      const char* mid_name = dexfile->GetMethodName(mid);
      const DexFile::StringId* other_name = other_dexfile.FindStringId(mid_name);
      if (other_name != nullptr) {
        uint16_t other_return_type_idx;
        std::vector<uint16_t> other_param_type_idxs;
        bool success = other_dexfile.CreateTypeList(
            dexfile->GetMethodSignature(mid).ToString(), &other_return_type_idx,
            &other_param_type_idxs);
        if (success) {
          const DexFile::ProtoId* other_sig =
              other_dexfile.FindProtoId(other_return_type_idx, other_param_type_idxs);
          if (other_sig != nullptr) {
            const  DexFile::MethodId* other_mid = other_dexfile.FindMethodId(
                *other_type_id, *other_name, *other_sig);
            if (other_mid != nullptr) {
              return other_dexfile.GetIndexForMethodId(*other_mid);
            }
          }
        }
      }
    }
  }
  return DexFile::kDexNoIndex;
}

uint32_t MethodHelper::FindDexMethodIndexInOtherDexFile(const DexFile& other_dexfile,
                                                        uint32_t name_and_signature_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtMethod* method = GetMethod();
  const DexFile* dexfile = method->GetDexFile();
  const uint32_t dex_method_idx = method->GetDexMethodIndex();
  const DexFile::MethodId& mid = dexfile->GetMethodId(dex_method_idx);
  const DexFile::MethodId& name_and_sig_mid = other_dexfile.GetMethodId(name_and_signature_idx);
  DCHECK_STREQ(dexfile->GetMethodName(mid), other_dexfile.GetMethodName(name_and_sig_mid));
  DCHECK_EQ(dexfile->GetMethodSignature(mid), other_dexfile.GetMethodSignature(name_and_sig_mid));
  if (dexfile == &other_dexfile) {
    return dex_method_idx;
  }
  const char* mid_declaring_class_descriptor = dexfile->StringByTypeIdx(mid.class_idx_);
  const DexFile::StringId* other_descriptor =
      other_dexfile.FindStringId(mid_declaring_class_descriptor);
  if (other_descriptor != nullptr) {
    const DexFile::TypeId* other_type_id =
        other_dexfile.FindTypeId(other_dexfile.GetIndexForStringId(*other_descriptor));
    if (other_type_id != nullptr) {
      const DexFile::MethodId* other_mid = other_dexfile.FindMethodId(
          *other_type_id, other_dexfile.GetStringId(name_and_sig_mid.name_idx_),
          other_dexfile.GetProtoId(name_and_sig_mid.proto_idx_));
      if (other_mid != nullptr) {
        return other_dexfile.GetIndexForMethodId(*other_mid);
      }
    }
  }
  return DexFile::kDexNoIndex;
}

}  // namespace art
