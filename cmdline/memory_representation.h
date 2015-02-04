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

#ifndef ART_CMDLINE_MEMORY_REPRESENTATION_H_
#define ART_CMDLINE_MEMORY_REPRESENTATION_H_

#include <string>
#include <assert.h>
#include <ostream>
#include "utils.h"

namespace art {

// An integral representation of bytes of memory.
// The underlying runtime size_t value is guaranteed to be a multiple of Divisor.
template <size_t Divisor = 1024>
struct Memory {
  static_assert(IsPowerOfTwo(Divisor), "Divisor must be a power of 2");

  static Memory<Divisor> FromBytes(size_t bytes) {
    assert(bytes % Divisor == 0);
    return Memory<Divisor>(bytes);
  }

  Memory() : Value(0u) {}
  Memory(size_t value) : Value(value) {  // NOLINT [runtime/explicit] [5]
    assert(value % Divisor == 0);
  }
  operator size_t() const { return Value; }

  size_t ToBytes() const {
    return Value;
  }

  static constexpr size_t kDivisor = Divisor;

  static const char* Name() {
    static std::string str;
    if (str.empty()) {
      str = "Memory<" + std::to_string(Divisor) + '>';
    }

    return str.c_str();
  }

  size_t Value;
};

template <size_t Divisor>
std::ostream& operator<<(std::ostream& stream, Memory<Divisor> memory) {
  return stream << memory.Value << '*' << Divisor;
}

using MemoryKiB = Memory<1024>;

}  // namespace art

#endif  // ART_CMDLINE_MEMORY_REPRESENTATION_H_
