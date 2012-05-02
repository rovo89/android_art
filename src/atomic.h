/*
 * Copyright (C) 2008 The Android Open Source Project
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

#ifndef ART_SRC_ATOMIC_H_
#define ART_SRC_ATOMIC_H_

#include <stdint.h>

#include "cutils/atomic.h"
#include "cutils/atomic-inline.h"
#include "macros.h"

namespace art {

// NOTE: Two "quasiatomic" operations on the exact same memory address
// are guaranteed to operate atomically with respect to each other,
// but no guarantees are made about quasiatomic operations mixed with
// non-quasiatomic operations on the same address, nor about
// quasiatomic operations that are performed on partially-overlapping
// memory.
//
// Only the "Sync" functions provide a memory barrier.
class QuasiAtomic {
 public:
  static void Startup();

  static void Shutdown();

  // Swaps the 64-bit value at "addr" with "value".  Returns the previous
  // value. No memory barriers.
  static int64_t Swap64(int64_t value, volatile int64_t* addr);

  // Swaps the 64-bit value at "addr" with "value".  Returns the previous
  // value. Provides memory barriers.
  static int64_t Swap64Sync(int64_t value, volatile int64_t* addr);

  // Reads the 64-bit value at "addr".
  static int64_t Read64(volatile const int64_t* addr);

  // If the value at "addr" is equal to "old_value", replace it with "new_value"
  // and return 0. Otherwise, don't swap, and return nonzero.
  static int Cas64(int64_t old_value, int64_t new_value, volatile int64_t* addr);

 private:
  DISALLOW_COPY_AND_ASSIGN(QuasiAtomic);
};

}  // namespace art

#endif  // ART_SRC_ATOMIC_H_
