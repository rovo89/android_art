/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_COMPILER_DEX_REG_STORAGE_EQ_H_
#define ART_COMPILER_DEX_REG_STORAGE_EQ_H_

#include "reg_storage.h"

namespace art {

// Define == and != operators for RegStorage. These are based on exact equality of the reg storage,
// that is, 32b and 64b views of the same physical register won't match. This is often not the
// intended behavior, so be careful when including this header.

inline bool operator==(const RegStorage& lhs, const RegStorage& rhs) {
  return lhs.ExactlyEquals(rhs);
}

inline bool operator!=(const RegStorage& lhs, const RegStorage& rhs) {
  return lhs.NotExactlyEquals(rhs);
}

}  // namespace art

#endif  // ART_COMPILER_DEX_REG_STORAGE_EQ_H_

