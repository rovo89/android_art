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

#include "base/stringprintf.h"
#include "builder.h"
#include "dex_instruction.h"
#include "nodes.h"
#include "pretty_printer.h"
#include "utils/arena_allocator.h"

#include "gtest/gtest.h"

namespace art {

class StringPrettyPrinter : public HPrettyPrinter {
 public:
  explicit StringPrettyPrinter(HGraph* graph)
      : HPrettyPrinter(graph), str_(""), current_block_(nullptr) { }

  virtual void PrintInt(int value) {
    str_ += StringPrintf("%d", value);
  }

  virtual void PrintString(const char* value) {
    str_ += value;
  }

  virtual void PrintNewLine() {
    str_ += '\n';
  }

  void Clear() { str_.clear(); }

  std::string str() const { return str_; }

  virtual void VisitBasicBlock(HBasicBlock* block) {
    current_block_ = block;
    HPrettyPrinter::VisitBasicBlock(block);
  }

  virtual void VisitGoto(HGoto* gota) {
    str_ += "  Goto ";
    PrintInt(current_block_->successors()->Get(0)->block_id());
    PrintNewLine();
  }

 private:
  std::string str_;
  HBasicBlock* current_block_;

  DISALLOW_COPY_AND_ASSIGN(StringPrettyPrinter);
};


static void TestCode(const uint16_t* data, int length, const char* expected) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraphBuilder builder(&allocator);
  HGraph* graph = builder.BuildGraph(data, data + length);
  ASSERT_NE(graph, nullptr);
  StringPrettyPrinter printer(graph);
  printer.VisitInsertionOrder();
  ASSERT_STREQ(expected, printer.str().c_str());
}

TEST(PrettyPrinterTest, ReturnVoid) {
  const uint16_t data[] = { Instruction::RETURN_VOID };

  const char* expected =
      "BasicBlock 0, succ: 1\n"
      "  Goto 1\n"
      "BasicBlock 1, pred: 0, succ: 2\n"
      "  ReturnVoid\n"
      "BasicBlock 2, pred: 1\n"
      "  Exit\n";

  TestCode(data, sizeof(data) / sizeof(uint16_t), expected);
}

TEST(PrettyPrinterTest, CFG1) {
  const char* expected =
    "BasicBlock 0, succ: 1\n"
    "  Goto 1\n"
    "BasicBlock 1, pred: 0, succ: 2\n"
    "  Goto 2\n"
    "BasicBlock 2, pred: 1, succ: 3\n"
    "  ReturnVoid\n"
    "BasicBlock 3, pred: 2\n"
    "  Exit\n";

  const uint16_t data[] = {
    Instruction::GOTO | 0x100,
    Instruction::RETURN_VOID
  };

  TestCode(data, sizeof(data) / sizeof(uint16_t), expected);
}

TEST(PrettyPrinterTest, CFG2) {
  const char* expected =
    "BasicBlock 0, succ: 1\n"
    "  Goto 1\n"
    "BasicBlock 1, pred: 0, succ: 2\n"
    "  Goto 2\n"
    "BasicBlock 2, pred: 1, succ: 3\n"
    "  Goto 3\n"
    "BasicBlock 3, pred: 2, succ: 4\n"
    "  ReturnVoid\n"
    "BasicBlock 4, pred: 3\n"
    "  Exit\n";

  const uint16_t data[] = {
    Instruction::GOTO | 0x100,
    Instruction::GOTO | 0x100,
    Instruction::RETURN_VOID
  };

  TestCode(data, sizeof(data) / sizeof(uint16_t), expected);
}

TEST(PrettyPrinterTest, CFG3) {
  const char* expected =
    "BasicBlock 0, succ: 1\n"
    "  Goto 1\n"
    "BasicBlock 1, pred: 0, succ: 3\n"
    "  Goto 3\n"
    "BasicBlock 2, pred: 3, succ: 4\n"
    "  ReturnVoid\n"
    "BasicBlock 3, pred: 1, succ: 2\n"
    "  Goto 2\n"
    "BasicBlock 4, pred: 2\n"
    "  Exit\n";

  const uint16_t data1[] = {
    Instruction::GOTO | 0x200,
    Instruction::RETURN_VOID,
    Instruction::GOTO | 0xFF00
  };

  TestCode(data1, sizeof(data1) / sizeof(uint16_t), expected);

  const uint16_t data2[] = {
    Instruction::GOTO_16, 3,
    Instruction::RETURN_VOID,
    Instruction::GOTO_16, 0xFFFF
  };

  TestCode(data2, sizeof(data2) / sizeof(uint16_t), expected);

  const uint16_t data3[] = {
    Instruction::GOTO_32, 4, 0,
    Instruction::RETURN_VOID,
    Instruction::GOTO_32, 0xFFFF, 0xFFFF
  };

  TestCode(data3, sizeof(data3) / sizeof(uint16_t), expected);
}

TEST(PrettyPrinterTest, CFG4) {
  const char* expected =
    "BasicBlock 0, succ: 1\n"
    "  Goto 1\n"
    "BasicBlock 1, pred: 0, 1, succ: 1\n"
    "  Goto 1\n"
    "BasicBlock 2\n"
    "  Exit\n";

  const uint16_t data1[] = {
    Instruction::NOP,
    Instruction::GOTO | 0xFF00
  };

  TestCode(data1, sizeof(data1) / sizeof(uint16_t), expected);

  const uint16_t data2[] = {
    Instruction::GOTO_32, 0, 0
  };

  TestCode(data2, sizeof(data2) / sizeof(uint16_t), expected);
}

TEST(PrettyPrinterTest, CFG5) {
  const char* expected =
    "BasicBlock 0, succ: 1\n"
    "  Goto 1\n"
    "BasicBlock 1, pred: 0, 2, succ: 3\n"
    "  ReturnVoid\n"
    "BasicBlock 2, succ: 1\n"
    "  Goto 1\n"
    "BasicBlock 3, pred: 1\n"
    "  Exit\n";

  const uint16_t data[] = {
    Instruction::RETURN_VOID,
    Instruction::GOTO | 0x100,
    Instruction::GOTO | 0xFE00
  };

  TestCode(data, sizeof(data) / sizeof(uint16_t), expected);
}

TEST(OptimizerTest, CFG6) {
  const char* expected =
    "BasicBlock 0, succ: 1\n"
    "  Goto 1\n"
    "BasicBlock 1, pred: 0, succ: 3, 2\n"
    "  If\n"
    "BasicBlock 2, pred: 1, succ: 3\n"
    "  Goto 3\n"
    "BasicBlock 3, pred: 1, 2, succ: 4\n"
    "  ReturnVoid\n"
    "BasicBlock 4, pred: 3\n"
    "  Exit\n";

  const uint16_t data[] = {
    Instruction::IF_EQ, 3,
    Instruction::GOTO | 0x100,
    Instruction::RETURN_VOID
  };

  TestCode(data, sizeof(data) / sizeof(uint16_t), expected);
}

TEST(OptimizerTest, CFG7) {
  const char* expected =
    "BasicBlock 0, succ: 1\n"
    "  Goto 1\n"
    "BasicBlock 1, pred: 0, succ: 3, 2\n"
    "  If\n"
    "BasicBlock 2, pred: 1, 3, succ: 3\n"
    "  Goto 3\n"
    "BasicBlock 3, pred: 1, 2, succ: 2\n"
    "  Goto 2\n"
    "BasicBlock 4\n"
    "  Exit\n";

  const uint16_t data[] = {
    Instruction::IF_EQ, 3,
    Instruction::GOTO | 0x100,
    Instruction::GOTO | 0xFF00
  };

  TestCode(data, sizeof(data) / sizeof(uint16_t), expected);
}
}  // namespace art
