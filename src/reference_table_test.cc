// Copyright 2011 Google Inc. All Rights Reserved.

#include "common_test.h"

#include "reference_table.h"

#include "gtest/gtest.h"

namespace art {

class ReferenceTableTest : public CommonTest {
};

TEST_F(ReferenceTableTest, Basics) {
  Object* o1 = String::AllocFromModifiedUtf8(0, "hello");
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
