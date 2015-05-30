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

#include "base/bit_utils.h"
#include "base/logging.h"
#include "globals.h"
#include "mirror/array-inl.h"
#include "primitive.h"

namespace art {

inline DexCacheArraysLayout::DexCacheArraysLayout(size_t pointer_size, const DexFile* dex_file)
    : /* types_offset_ is always 0u */
      pointer_size_(pointer_size),
      methods_offset_(types_offset_ + TypesSize(dex_file->NumTypeIds())),
      strings_offset_(methods_offset_ + MethodsSize(dex_file->NumMethodIds())),
      fields_offset_(strings_offset_ + StringsSize(dex_file->NumStringIds())),
      size_(fields_offset_ + FieldsSize(dex_file->NumFieldIds())) {
  DCHECK(ValidPointerSize(pointer_size)) << pointer_size;
}

inline size_t DexCacheArraysLayout::TypeOffset(uint32_t type_idx) const {
  return types_offset_ + ElementOffset(sizeof(mirror::HeapReference<mirror::Class>), type_idx);
}

inline size_t DexCacheArraysLayout::TypesSize(size_t num_elements) const {
  return ArraySize(sizeof(mirror::HeapReference<mirror::Class>), num_elements);
}

inline size_t DexCacheArraysLayout::MethodOffset(uint32_t method_idx) const {
  return methods_offset_ + ElementOffset(pointer_size_, method_idx);
}

inline size_t DexCacheArraysLayout::MethodsSize(size_t num_elements) const {
  return ArraySize(pointer_size_, num_elements);
}

inline size_t DexCacheArraysLayout::StringOffset(uint32_t string_idx) const {
  return strings_offset_ + ElementOffset(sizeof(mirror::HeapReference<mirror::String>), string_idx);
}

inline size_t DexCacheArraysLayout::StringsSize(size_t num_elements) const {
  return ArraySize(sizeof(mirror::HeapReference<mirror::String>), num_elements);
}

inline size_t DexCacheArraysLayout::FieldOffset(uint32_t field_idx) const {
  return fields_offset_ + ElementOffset(pointer_size_, field_idx);
}

inline size_t DexCacheArraysLayout::FieldsSize(size_t num_elements) const {
  return ArraySize(pointer_size_, num_elements);
}

inline size_t DexCacheArraysLayout::ElementOffset(size_t element_size, uint32_t idx) {
  return mirror::Array::DataOffset(element_size).Uint32Value() + element_size * idx;
}

inline size_t DexCacheArraysLayout::ArraySize(size_t element_size, uint32_t num_elements) {
  size_t array_size = mirror::ComputeArraySize(num_elements, ComponentSizeShiftWidth(element_size));
  DCHECK_NE(array_size, 0u);  // No overflow expected for dex cache arrays.
  return RoundUp(array_size, kObjectAlignment);
}

}  // namespace art

#endif  // ART_COMPILER_UTILS_DEX_CACHE_ARRAYS_LAYOUT_INL_H_
