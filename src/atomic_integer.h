/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_ATOMIC_INTEGER_H_
#define ART_SRC_ATOMIC_INTEGER_H_

#include "atomic.h"

namespace art {

class AtomicInteger {
 public:
  AtomicInteger(int32_t value) : value_(value) { }

  operator int32_t () const {
    return get();
  }

  int32_t get() const {
    return value_;
  }

  int32_t operator += (const int32_t value) {
    return android_atomic_add(value, &value_);
  }

  int32_t operator -= (const int32_t value) {
    return android_atomic_add(-value, &value_);
  }

  int32_t operator |= (const int32_t value) {
    return android_atomic_or(value, &value_);
  }

  int32_t operator &= (const int32_t value) {
    return android_atomic_and(-value, &value_);
  }

  int32_t operator ++ () {
    return android_atomic_inc(&value_);
  }

  int32_t operator -- () {
    return android_atomic_dec(&value_);
  }
 private:
  int32_t value_;
};

}

#endif  // ART_SRC_ATOMIC_INTEGER_H_
