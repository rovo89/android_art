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

#ifndef ART_RUNTIME_ATOMIC_INTEGER_H_
#define ART_RUNTIME_ATOMIC_INTEGER_H_

#include <stdint.h>

namespace art {

class AtomicInteger {
 public:
  AtomicInteger() : value_(0) { }

  explicit AtomicInteger(int32_t value) : value_(value) { }

  AtomicInteger& operator=(int32_t desired) {
    Store(desired);
    return *this;
  }

  int32_t Load() const {
    return value_;
  }

  operator int32_t() const {
    return Load();
  }

  int32_t FetchAndAdd(const int32_t value) {
    return __sync_fetch_and_add(&value_, value);  // Return old_value.
  }

  int32_t FetchAndSub(const int32_t value) {
    return __sync_fetch_and_sub(&value_, value);  // Return old value.
  }

  int32_t operator++() {  // Prefix operator.
    return __sync_add_and_fetch(&value_, 1);  // Return new value.
  }

  int32_t operator++(int32_t) {  // Postfix operator.
    return __sync_fetch_and_add(&value_, 1);  // Return old value.
  }

  int32_t operator--() {  // Prefix operator.
    return __sync_sub_and_fetch(&value_, 1);  // Return new value.
  }

  int32_t operator--(int32_t) {  // Postfix operator.
    return __sync_fetch_and_sub(&value_, 1);  // Return old value.
  }

  bool CompareAndSwap(int32_t expected_value, int32_t desired_value) {
    return __sync_bool_compare_and_swap(&value_, expected_value, desired_value);
  }

  volatile int32_t* Address() {
    return &value_;
  }

 private:
  // Unsafe = operator for non atomic operations on the integer.
  void Store(int32_t desired) {
    value_ = desired;
  }

  volatile int32_t value_;
};

}  // namespace art

#endif  // ART_RUNTIME_ATOMIC_INTEGER_H_
