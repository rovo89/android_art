// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/dex_instruction_visitor.h"
#include "src/scoped_ptr.h"

#include <iostream>
#include "gtest/gtest.h"

namespace art {

class TestVisitor : public DexInstructionVisitor<TestVisitor> {};

TEST(Instruction, Init) {
  scoped_ptr<TestVisitor> visitor(new TestVisitor);
}

class CountVisitor : public DexInstructionVisitor<CountVisitor> {
 public:
  int count_;

  CountVisitor() : count_(0) {}

  void Do_Default(Instruction* inst) {
    ++count_;
  }
};

TEST(Instruction, Count) {
  CountVisitor v0;
  uint16_t c0[] = {};
  v0.Visit(c0, sizeof(c0));
  EXPECT_EQ(0, v0.count_);

  CountVisitor v1;
  uint16_t c1[] = { 0 };
  v1.Visit(c1, sizeof(c1));
  EXPECT_EQ(1, v1.count_);

  CountVisitor v2;
  uint16_t c2[] = { 0, 0 };
  v2.Visit(c2, sizeof(c2));
  EXPECT_EQ(2, v2.count_);

  CountVisitor v3;
  uint16_t c3[] = { 0, 0, 0, };
  v3.Visit(c3, sizeof(c3));
  EXPECT_EQ(3, v3.count_);

  CountVisitor v4;
  uint16_t c4[] = { 0, 0, 0, 0  };
  v4.Visit(c4, sizeof(c4));
  EXPECT_EQ(4, v4.count_);
}

}  // namespace art
