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
#include "dex_file.h"
#include "dex_instruction.h"
#include "nodes.h"
#include "optimizing_unit_test.h"
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
    PrintString("  ");
    PrintInt(gota->GetId());
    PrintString(": Goto ");
    PrintInt(current_block_->GetSuccessors().Get(0)->GetBlockId());
    PrintNewLine();
  }

 private:
  std::string str_;
  HBasicBlock* current_block_;

  DISALLOW_COPY_AND_ASSIGN(StringPrettyPrinter);
};

static void TestCode(const uint16_t* data, const char* expected) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraphBuilder builder(&allocator);
  const DexFile::CodeItem* item = reinterpret_cast<const DexFile::CodeItem*>(data);
  HGraph* graph = builder.BuildGraph(*item);
  ASSERT_NE(graph, nullptr);
  StringPrettyPrinter printer(graph);
  printer.VisitInsertionOrder();
  ASSERT_STREQ(expected, printer.str().c_str());
}

TEST(PrettyPrinterTest, ReturnVoid) {
  const uint16_t data[] = ZERO_REGISTER_CODE_ITEM(
      Instruction::RETURN_VOID);

  const char* expected =
      "BasicBlock 0, succ: 1\n"
      "  2: Goto 1\n"
      "BasicBlock 1, pred: 0, succ: 2\n"
      "  0: ReturnVoid\n"
      "BasicBlock 2, pred: 1\n"
      "  1: Exit\n";

  TestCode(data, expected);
}

TEST(PrettyPrinterTest, CFG1) {
  const char* expected =
    "BasicBlock 0, succ: 1\n"
    "  3: Goto 1\n"
    "BasicBlock 1, pred: 0, succ: 2\n"
    "  0: Goto 2\n"
    "BasicBlock 2, pred: 1, succ: 3\n"
    "  1: ReturnVoid\n"
    "BasicBlock 3, pred: 2\n"
    "  2: Exit\n";

  const uint16_t data[] =
    ZERO_REGISTER_CODE_ITEM(
      Instruction::GOTO | 0x100,
      Instruction::RETURN_VOID);

  TestCode(data, expected);
}

TEST(PrettyPrinterTest, CFG2) {
  const char* expected =
    "BasicBlock 0, succ: 1\n"
    "  4: Goto 1\n"
    "BasicBlock 1, pred: 0, succ: 2\n"
    "  0: Goto 2\n"
    "BasicBlock 2, pred: 1, succ: 3\n"
    "  1: Goto 3\n"
    "BasicBlock 3, pred: 2, succ: 4\n"
    "  2: ReturnVoid\n"
    "BasicBlock 4, pred: 3\n"
    "  3: Exit\n";

  const uint16_t data[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO | 0x100,
    Instruction::GOTO | 0x100,
    Instruction::RETURN_VOID);

  TestCode(data, expected);
}

TEST(PrettyPrinterTest, CFG3) {
  const char* expected =
    "BasicBlock 0, succ: 1\n"
    "  4: Goto 1\n"
    "BasicBlock 1, pred: 0, succ: 3\n"
    "  0: Goto 3\n"
    "BasicBlock 2, pred: 3, succ: 4\n"
    "  1: ReturnVoid\n"
    "BasicBlock 3, pred: 1, succ: 2\n"
    "  2: Goto 2\n"
    "BasicBlock 4, pred: 2\n"
    "  3: Exit\n";

  const uint16_t data1[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO | 0x200,
    Instruction::RETURN_VOID,
    Instruction::GOTO | 0xFF00);

  TestCode(data1, expected);

  const uint16_t data2[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO_16, 3,
    Instruction::RETURN_VOID,
    Instruction::GOTO_16, 0xFFFF);

  TestCode(data2, expected);

  const uint16_t data3[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO_32, 4, 0,
    Instruction::RETURN_VOID,
    Instruction::GOTO_32, 0xFFFF, 0xFFFF);

  TestCode(data3, expected);
}

TEST(PrettyPrinterTest, CFG4) {
  const char* expected =
    "BasicBlock 0, succ: 1\n"
    "  2: Goto 1\n"
    "BasicBlock 1, pred: 0, 1, succ: 1\n"
    "  0: Goto 1\n"
    "BasicBlock 2\n"
    "  1: Exit\n";

  const uint16_t data1[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::NOP,
    Instruction::GOTO | 0xFF00);

  TestCode(data1, expected);

  const uint16_t data2[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO_32, 0, 0);

  TestCode(data2, expected);
}

TEST(PrettyPrinterTest, CFG5) {
  const char* expected =
    "BasicBlock 0, succ: 1\n"
    "  3: Goto 1\n"
    "BasicBlock 1, pred: 0, 2, succ: 3\n"
    "  0: ReturnVoid\n"
    "BasicBlock 2, succ: 1\n"
    "  1: Goto 1\n"
    "BasicBlock 3, pred: 1\n"
    "  2: Exit\n";

  const uint16_t data[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::RETURN_VOID,
    Instruction::GOTO | 0x100,
    Instruction::GOTO | 0xFE00);

  TestCode(data, expected);
}

TEST(PrettyPrinterTest, CFG6) {
  const char* expected =
    "BasicBlock 0, succ: 1\n"
    "  0: Local [4, 3, 2]\n"
    "  1: IntConstant [2]\n"
    "  10: Goto 1\n"
    "BasicBlock 1, pred: 0, succ: 3, 2\n"
    "  2: StoreLocal(0, 1)\n"
    "  3: LoadLocal(0) [5]\n"
    "  4: LoadLocal(0) [5]\n"
    "  5: Equal(3, 4) [6]\n"
    "  6: If(5)\n"
    "BasicBlock 2, pred: 1, succ: 3\n"
    "  7: Goto 3\n"
    "BasicBlock 3, pred: 1, 2, succ: 4\n"
    "  8: ReturnVoid\n"
    "BasicBlock 4, pred: 3\n"
    "  9: Exit\n";

  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 3,
    Instruction::GOTO | 0x100,
    Instruction::RETURN_VOID);

  TestCode(data, expected);
}

TEST(PrettyPrinterTest, CFG7) {
  const char* expected =
    "BasicBlock 0, succ: 1\n"
    "  0: Local [4, 3, 2]\n"
    "  1: IntConstant [2]\n"
    "  10: Goto 1\n"
    "BasicBlock 1, pred: 0, succ: 3, 2\n"
    "  2: StoreLocal(0, 1)\n"
    "  3: LoadLocal(0) [5]\n"
    "  4: LoadLocal(0) [5]\n"
    "  5: Equal(3, 4) [6]\n"
    "  6: If(5)\n"
    "BasicBlock 2, pred: 1, 3, succ: 3\n"
    "  7: Goto 3\n"
    "BasicBlock 3, pred: 1, 2, succ: 2\n"
    "  8: Goto 2\n"
    "BasicBlock 4\n"
    "  9: Exit\n";

  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 3,
    Instruction::GOTO | 0x100,
    Instruction::GOTO | 0xFF00);

  TestCode(data, expected);
}

TEST(PrettyPrinterTest, IntConstant) {
  const char* expected =
    "BasicBlock 0, succ: 1\n"
    "  0: Local [2]\n"
    "  1: IntConstant [2]\n"
    "  5: Goto 1\n"
    "BasicBlock 1, pred: 0, succ: 2\n"
    "  2: StoreLocal(0, 1)\n"
    "  3: ReturnVoid\n"
    "BasicBlock 2, pred: 1\n"
    "  4: Exit\n";

  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::RETURN_VOID);

  TestCode(data, expected);
}
}  // namespace art
