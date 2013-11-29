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

#ifndef ART_COMPILER_LEB128_ENCODER_H_
#define ART_COMPILER_LEB128_ENCODER_H_

#include "base/macros.h"
#include "leb128.h"

namespace art {

// An encoder with an API similar to vector<uint32_t> where the data is captured in ULEB128 format.
class Leb128EncodingVector {
 public:
  Leb128EncodingVector() {
  }

  void Reserve(uint32_t size) {
    data_.reserve(size);
  }

  void PushBackUnsigned(uint32_t value) {
    uint8_t out = value & 0x7f;
    value >>= 7;
    while (value != 0) {
      data_.push_back(out | 0x80);
      out = value & 0x7f;
      value >>= 7;
    }
    data_.push_back(out);
  }

  template<typename It>
  void InsertBackUnsigned(It cur, It end) {
    for (; cur != end; ++cur) {
      PushBackUnsigned(*cur);
    }
  }

  void PushBackSigned(int32_t value) {
    uint32_t extra_bits = static_cast<uint32_t>(value ^ (value >> 31)) >> 6;
    uint8_t out = value & 0x7f;
    while (extra_bits != 0u) {
      data_.push_back(out | 0x80);
      value >>= 7;
      out = value & 0x7f;
      extra_bits >>= 7;
    }
    data_.push_back(out);
  }

  template<typename It>
  void InsertBackSigned(It cur, It end) {
    for (; cur != end; ++cur) {
      PushBackSigned(*cur);
    }
  }

  const std::vector<uint8_t>& GetData() const {
    return data_;
  }

 private:
  std::vector<uint8_t> data_;

  DISALLOW_COPY_AND_ASSIGN(Leb128EncodingVector);
};

}  // namespace art

#endif  // ART_COMPILER_LEB128_ENCODER_H_
