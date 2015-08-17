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

#ifndef ART_RUNTIME_LENGTH_PREFIXED_ARRAY_H_
#define ART_RUNTIME_LENGTH_PREFIXED_ARRAY_H_

#include <stddef.h>  // for offsetof()

#include "linear_alloc.h"
#include "stride_iterator.h"
#include "base/bit_utils.h"
#include "base/casts.h"
#include "base/iteration_range.h"

namespace art {

template<typename T>
class LengthPrefixedArray {
 public:
  explicit LengthPrefixedArray(size_t length)
      : length_(dchecked_integral_cast<uint32_t>(length)) {}

  T& At(size_t index, size_t element_size = sizeof(T), size_t alignment = alignof(T)) {
    DCHECK_LT(index, length_);
    return AtUnchecked(index, element_size, alignment);
  }

  StrideIterator<T> Begin(size_t element_size = sizeof(T), size_t alignment = alignof(T)) {
    return StrideIterator<T>(&AtUnchecked(0, element_size, alignment), element_size);
  }

  StrideIterator<T> End(size_t element_size = sizeof(T), size_t alignment = alignof(T)) {
    return StrideIterator<T>(&AtUnchecked(length_, element_size, alignment), element_size);
  }

  static size_t OffsetOfElement(size_t index,
                                size_t element_size = sizeof(T),
                                size_t alignment = alignof(T)) {
    DCHECK_ALIGNED_PARAM(element_size, alignment);
    return RoundUp(offsetof(LengthPrefixedArray<T>, data), alignment) + index * element_size;
  }

  static size_t ComputeSize(size_t num_elements,
                            size_t element_size = sizeof(T),
                            size_t alignment = alignof(T)) {
    size_t result = OffsetOfElement(num_elements, element_size, alignment);
    DCHECK_ALIGNED_PARAM(result, alignment);
    return result;
  }

  uint64_t Length() const {
    return length_;
  }

  // Update the length but does not reallocate storage.
  void SetLength(size_t length) {
    length_ = dchecked_integral_cast<uint32_t>(length);
  }

 private:
  T& AtUnchecked(size_t index, size_t element_size, size_t alignment) {
    return *reinterpret_cast<T*>(
        reinterpret_cast<uintptr_t>(this) + OffsetOfElement(index, element_size, alignment));
  }

  uint32_t length_;
  uint8_t data[0];
};

// Returns empty iteration range if the array is null.
template<typename T>
IterationRange<StrideIterator<T>> MakeIterationRangeFromLengthPrefixedArray(
    LengthPrefixedArray<T>* arr, size_t element_size = sizeof(T), size_t alignment = alignof(T)) {
  return arr != nullptr ?
      MakeIterationRange(arr->Begin(element_size, alignment), arr->End(element_size, alignment)) :
      MakeEmptyIterationRange(StrideIterator<T>(nullptr, 0));
}

}  // namespace art

#endif  // ART_RUNTIME_LENGTH_PREFIXED_ARRAY_H_
