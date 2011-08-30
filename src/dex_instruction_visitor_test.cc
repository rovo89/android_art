// Copyright 2011 Google Inc. All Rights Reserved.

#include "dex_instruction_visitor.h"

#include <iostream>

#include "UniquePtr.h"
#include "gtest/gtest.h"

namespace art {

class TestVisitor : public DexInstructionVisitor<TestVisitor> {};

TEST(InstructionTest, Init) {
  UniquePtr<TestVisitor> visitor(new TestVisitor);
}

class CountVisitor : public DexInstructionVisitor<CountVisitor> {
 public:
  int count_;

  CountVisitor() : count_(0) {}

  void Do_Default(const Instruction* inst) {
    ++count_;
  }
};

TEST(InstructionTest, Count) {
  CountVisitor v0;
  const uint16_t c0[] = {};
  v0.Visit(c0, sizeof(c0));
  EXPECT_EQ(0, v0.count_);

  CountVisitor v1;
  const uint16_t c1[] = { 0 };
  v1.Visit(c1, sizeof(c1));
  EXPECT_EQ(1, v1.count_);

  CountVisitor v2;
  const uint16_t c2[] = { 0, 0 };
  v2.Visit(c2, sizeof(c2));
  EXPECT_EQ(2, v2.count_);

  CountVisitor v3;
  const uint16_t c3[] = { 0, 0, 0, };
  v3.Visit(c3, sizeof(c3));
  EXPECT_EQ(3, v3.count_);

  CountVisitor v4;
  const uint16_t c4[] = { 0, 0, 0, 0  };
  v4.Visit(c4, sizeof(c4));
  EXPECT_EQ(4, v4.count_);
}

}  // namespace art
