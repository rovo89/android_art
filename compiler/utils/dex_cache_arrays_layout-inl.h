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

#ifndef ART_COMPILER_UTILS_DEX_CACHE_ARRAYS_LAYOUT_INL_H_
#define ART_COMPILER_UTILS_DEX_CACHE_ARRAYS_LAYOUT_INL_H_

#include "dex_cache_arrays_layout.h"

#include "base/logging.h"
#include "globals.h"
#include "mirror/array-inl.h"
#include "primitive.h"
#include "utils.h"

namespace mirror {
class ArtField;
class ArtMethod;
class Class;
class String;
}  // namespace mirror

namespace art {

inline DexCacheArraysLayout::DexCacheArraysLayout(const DexFile* dex_file)
    : /* types_offset_ is always 0u */
      methods_offset_(types_offset_ + ArraySize<mirror::Class>(dex_file->NumTypeIds())),
      strings_offset_(methods_offset_ + ArraySize<mirror::ArtMethod>(dex_file->NumMethodIds())),
      fields_offset_(strings_offset_ + ArraySize<mirror::String>(dex_file->NumStringIds())),
      size_(fields_offset_ + ArraySize<mirror::ArtField>(dex_file->NumFieldIds())) {
}

inline size_t DexCacheArraysLayout::TypeOffset(uint32_t type_idx) const {
  return types_offset_ + ElementOffset<mirror::Class>(type_idx);
}

inline size_t DexCacheArraysLayout::MethodOffset(uint32_t method_idx) const {
  return methods_offset_ + ElementOffset<mirror::ArtMethod>(method_idx);
}

inline size_t DexCacheArraysLayout::StringOffset(uint32_t string_idx) const {
  return strings_offset_ + ElementOffset<mirror::String>(string_idx);
}

inline size_t DexCacheArraysLayout::FieldOffset(uint32_t field_idx) const {
  return fields_offset_ + ElementOffset<mirror::ArtField>(field_idx);
}

template <typename MirrorType>
inline size_t DexCacheArraysLayout::ElementOffset(uint32_t idx) {
  return mirror::Array::DataOffset(sizeof(mirror::HeapReference<MirrorType>)).Uint32Value() +
      sizeof(mirror::HeapReference<MirrorType>) * idx;
}

template <typename MirrorType>
inline size_t DexCacheArraysLayout::ArraySize(uint32_t num_elements) {
  size_t array_size = mirror::ComputeArraySize(
      num_elements, ComponentSizeShiftWidth<sizeof(mirror::HeapReference<MirrorType>)>());
  DCHECK_NE(array_size, 0u);  // No overflow expected for dex cache arrays.
  return RoundUp(array_size, kObjectAlignment);
}

}  // namespace art

#endif  // ART_COMPILER_UTILS_DEX_CACHE_ARRAYS_LAYOUT_INL_H_
