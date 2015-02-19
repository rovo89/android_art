/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include "base/arena_allocator.h"
#include "gtest/gtest.h"
#include "utils/arena_bit_vector.h"

namespace art {

TEST(ArenaAllocator, Test) {
  ArenaPool pool;
  ArenaAllocator arena(&pool);
  ArenaBitVector bv(&arena, 10, true);
  bv.SetBit(5);
  EXPECT_EQ(1U, bv.GetStorageSize());
  bv.SetBit(35);
  EXPECT_EQ(2U, bv.GetStorageSize());
}

}  // namespace art
