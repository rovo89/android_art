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
#include "base/iteration_range.h"

namespace art {

template<typename T>
class LengthPrefixedArray {
 public:
  explicit LengthPrefixedArray(uint64_t length) : length_(length) {}

  T& At(size_t index, size_t element_size = sizeof(T)) {
    DCHECK_LT(index, length_);
    return *reinterpret_cast<T*>(&data_[0] + index * element_size);
  }

  StrideIterator<T> Begin(size_t element_size = sizeof(T)) {
    return StrideIterator<T>(reinterpret_cast<T*>(&data_[0]), element_size);
  }

  StrideIterator<T> End(size_t element_size = sizeof(T)) {
    return StrideIterator<T>(reinterpret_cast<T*>(&data_[0] + element_size * length_),
                             element_size);
  }

  static size_t OffsetOfElement(size_t index, size_t element_size = sizeof(T)) {
    return offsetof(LengthPrefixedArray<T>, data_) + index * element_size;
  }

  static size_t ComputeSize(size_t num_elements, size_t element_size = sizeof(T)) {
    return sizeof(LengthPrefixedArray<T>) + num_elements * element_size;
  }

  uint64_t Length() const {
    return length_;
  }

 private:
  uint64_t length_;  // 64 bits for padding reasons.
  uint8_t data_[0];
};

// Returns empty iteration range if the array is null.
template<typename T>
IterationRange<StrideIterator<T>> MakeIterationRangeFromLengthPrefixedArray(
    LengthPrefixedArray<T>* arr, size_t element_size) {
  return arr != nullptr ?
      MakeIterationRange(arr->Begin(element_size), arr->End(element_size)) :
      MakeEmptyIterationRange(StrideIterator<T>(nullptr, 0));
}

}  // namespace art

#endif  // ART_RUNTIME_LENGTH_PREFIXED_ARRAY_H_
