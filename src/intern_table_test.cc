// Copyright 2011 Google Inc. All Rights Reserved.

#include "intern_table.h"

#include "common_test.h"
#include "object.h"

#include "gtest/gtest.h"

namespace art {

class InternTableTest : public CommonTest {};

TEST_F(InternTableTest, Intern) {
  InternTable intern_table;
  String* foo_1 = intern_table.Intern(3, "foo");
  String* foo_2 = intern_table.Intern(3, "foo");
  String* foo_3 = String::AllocFromAscii("foo");
  String* bar = intern_table.Intern(3, "bar");
  EXPECT_TRUE(foo_1->Equals("foo"));
  EXPECT_TRUE(foo_2->Equals("foo"));
  EXPECT_TRUE(foo_3->Equals("foo"));
  EXPECT_TRUE(foo_1 != NULL);
  EXPECT_TRUE(foo_2 != NULL);
  EXPECT_EQ(foo_1, foo_2);
  EXPECT_NE(foo_1, bar);
  EXPECT_NE(foo_2, bar);
  EXPECT_NE(foo_3, bar);
}

}  // namespace art
