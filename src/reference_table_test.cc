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

#include "common_test.h"

#include "reference_table.h"

namespace art {

class ReferenceTableTest : public CommonTest {
};

TEST_F(ReferenceTableTest, Basics) {
  Object* o1 = String::AllocFromModifiedUtf8("hello");
  Object* o2 = ShortArray::Alloc(0);

  // TODO: rewrite Dump to take a std::ostream& so we can test it better.

  ReferenceTable rt("test", 0, 4);
  rt.Dump();
  EXPECT_EQ(0U, rt.Size());
  rt.Remove(NULL);
  EXPECT_EQ(0U, rt.Size());
  rt.Remove(o1);
  EXPECT_EQ(0U, rt.Size());
  rt.Add(o1);
  EXPECT_EQ(1U, rt.Size());
  rt.Add(o2);
  EXPECT_EQ(2U, rt.Size());
  rt.Dump();
  rt.Remove(o1);
  EXPECT_EQ(1U, rt.Size());
  rt.Remove(o2);
  EXPECT_EQ(0U, rt.Size());
}

}  // namespace art
