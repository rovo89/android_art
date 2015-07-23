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

#include "out.h"

#include <algorithm>
#include <gtest/gtest.h>

namespace art {

struct OutTest : public testing::Test {
  // Multiplies values less than 10 by two, stores the result and returns 0.
  // Returns -1 if the original value was not multiplied by two.
  static int multiply_small_values_by_two(size_t args, out<int> result) {
    if (args < 10) {
      *result = args * 2;
      return 0;
    } else {
      return -1;
    }
  }
};

extern "C" int multiply_small_values_by_two_legacy(size_t args, int* result) {
  if (args < 10) {
    *result = args * 2;
    return 0;
  } else {
    return -1;
  }
}

TEST_F(OutTest, TraditionalCall) {
  // For calling traditional C++ functions.
  int res;
  EXPECT_EQ(multiply_small_values_by_two(1, outof(res)), 0);
  EXPECT_EQ(2, res);
}

TEST_F(OutTest, LegacyCall) {
  // For calling legacy, e.g. C-style functions.
  int res2;
  EXPECT_EQ(0, multiply_small_values_by_two_legacy(1, outof(res2)));
  EXPECT_EQ(2, res2);
}

TEST_F(OutTest, CallFromIterator) {
  // For calling a function with a parameter originating as an iterator.
  std::vector<int> list = {1, 2, 3};  // NOLINT [whitespace/labels] [4]
  std::vector<int>::iterator it = list.begin();

  EXPECT_EQ(0, multiply_small_values_by_two(2, outof_iterator(it)));
  EXPECT_EQ(4, list[0]);
}

TEST_F(OutTest, CallFromPointer) {
  // For calling a function with a parameter originating as a C-pointer.
  std::vector<int> list = {1, 2, 3};  // NOLINT [whitespace/labels] [4]

  int* list_ptr = &list[2];  // 3

  EXPECT_EQ(0, multiply_small_values_by_two(2, outof_ptr(list_ptr)));
  EXPECT_EQ(4, list[2]);
}

TEST_F(OutTest, OutAsIterator) {
  // For using the out<T> parameter as an iterator inside of the callee.
  std::vector<int> list;
  int x = 100;
  out<int> out_from_x = outof(x);

  for (const int& val : out_from_x) {
    list.push_back(val);
  }

  ASSERT_EQ(1u, list.size());
  EXPECT_EQ(100, list[0]);

  // A more typical use-case would be to use std algorithms
  EXPECT_NE(out_from_x.end(),
            std::find(out_from_x.begin(),
                      out_from_x.end(),
                      100));  // Search for '100' in out.
}

}  // namespace art
