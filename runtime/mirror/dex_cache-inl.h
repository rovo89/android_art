/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_RUNTIME_MIRROR_DEX_CACHE_INL_H_
#define ART_RUNTIME_MIRROR_DEX_CACHE_INL_H_

#include "dex_cache.h"

#include "art_field-inl.h"
#include "base/logging.h"
#include "mirror/class.h"
#include "runtime.h"

namespace art {
namespace mirror {

inline uint32_t DexCache::ClassSize() {
  uint32_t vtable_entries = Object::kVTableLength + 5;
  return Class::ComputeClassSize(true, vtable_entries, 0, 0, 0, 0, 0);
}

inline ArtMethod* DexCache::GetResolvedMethod(uint32_t method_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ArtMethod* method = GetResolvedMethods()->Get(method_idx);
  // Hide resolution trampoline methods from the caller
  if (method != nullptr && method->IsRuntimeMethod()) {
    DCHECK_EQ(method, Runtime::Current()->GetResolutionMethod());
    return nullptr;
  }
  return method;
}

inline void DexCache::SetResolvedType(uint32_t type_idx, Class* resolved) {
  // TODO default transaction support.
  DCHECK(resolved == nullptr || !resolved->IsErroneous());
  GetResolvedTypes()->Set(type_idx, resolved);
}

inline ArtField* DexCache::GetResolvedField(uint32_t idx, size_t ptr_size) {
  ArtField* field = nullptr;
  if (ptr_size == 8u) {
    field = reinterpret_cast<ArtField*>(
        static_cast<uintptr_t>(GetResolvedFields()->AsLongArray()->GetWithoutChecks(idx)));
  } else {
    DCHECK_EQ(ptr_size, 4u);
    field = reinterpret_cast<ArtField*>(
      static_cast<uintptr_t>(GetResolvedFields()->AsIntArray()->GetWithoutChecks(idx)));
  }
  if (field == nullptr || field->GetDeclaringClass()->IsErroneous()) {
    return nullptr;
  }
  return field;
}

inline void DexCache::SetResolvedField(uint32_t idx, ArtField* field, size_t ptr_size) {
  if (ptr_size == 8u) {
    GetResolvedFields()->AsLongArray()->Set(
        idx, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(field)));
  } else {
    DCHECK_EQ(ptr_size, 4u);
    CHECK_LE(reinterpret_cast<uintptr_t>(field), 0xFFFFFFFF);
    GetResolvedFields()->AsIntArray()->Set(
        idx, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(field)));
  }
}

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_DEX_CACHE_INL_H_
