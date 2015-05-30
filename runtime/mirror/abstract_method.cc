/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "abstract_method.h"

#include "art_method-inl.h"

namespace art {
namespace mirror {

bool AbstractMethod::CreateFromArtMethod(ArtMethod* method) {
  auto* interface_method = method->GetInterfaceMethodIfProxy(sizeof(void*));
  SetArtMethod(method);
  SetFieldObject<false>(DeclaringClassOffset(), method->GetDeclaringClass());
  SetFieldObject<false>(
      DeclaringClassOfOverriddenMethodOffset(), interface_method->GetDeclaringClass());
  SetField32<false>(AccessFlagsOffset(), method->GetAccessFlags());
  SetField32<false>(DexMethodIndexOffset(), method->GetDexMethodIndex());
  return true;
}

ArtMethod* AbstractMethod::GetArtMethod() {
  return reinterpret_cast<ArtMethod*>(GetField64(ArtMethodOffset()));
}

void AbstractMethod::SetArtMethod(ArtMethod* method) {
  SetField64<false>(ArtMethodOffset(), reinterpret_cast<uint64_t>(method));
}

mirror::Class* AbstractMethod::GetDeclaringClass() {
  return GetFieldObject<mirror::Class>(DeclaringClassOffset());
}

}  // namespace mirror
}  // namespace art
