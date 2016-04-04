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

#include "arch/x86/instruction_set_features_x86.h"
#include "code_generator_x86.h"
#include "dead_code_elimination.h"
#include "driver/compiler_options.h"
#include "graph_checker.h"
#include "optimizing_unit_test.h"
#include "pretty_printer.h"

#include "gtest/gtest.h"

namespace art {

class DeadCodeEliminationTest : public CommonCompilerTest {};

static void TestCode(const uint16_t* data,
                     const std::string& expected_before,
                     const std::string& expected_after) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraph* graph = CreateCFG(&allocator, data);
  ASSERT_NE(graph, nullptr);

  StringPrettyPrinter printer_before(graph);
  printer_before.VisitInsertionOrder();
  std::string actual_before = printer_before.str();
  ASSERT_EQ(actual_before, expected_before);

  std::unique_ptr<const X86InstructionSetFeatures> features_x86(
      X86InstructionSetFeatures::FromCppDefines());
  x86::CodeGeneratorX86 codegenX86(graph, *features_x86.get(), CompilerOptions());
  HDeadCodeElimination(graph).Run();
  GraphChecker graph_checker(graph);
  graph_checker.Run();
  ASSERT_TRUE(graph_checker.IsValid());

  StringPrettyPrinter printer_after(graph);
  printer_after.VisitInsertionOrder();
  std::string actual_after = printer_after.str();
  ASSERT_EQ(actual_after, expected_after);
}

/**
 * Small three-register program.
 *
 *                              16-bit
 *                              offset
 *                              ------
 *     v1 <- 1                  0.      const/4 v1, #+1
 *     v0 <- 0                  1.      const/4 v0, #+0
 *     if v1 >= 0 goto L1       2.      if-gez v1, +3
 *     v0 <- v1                 4.      move v0, v1
 * L1: v2 <- v0 + v1            5.      add-int v2, v0, v1
 *     return-void              7.      return
 */
TEST_F(DeadCodeEliminationTest, AdditionAndConditionalJump) {
  const uint16_t data[] = THREE_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 1 << 8 | 1 << 12,
    Instruction::CONST_4 | 0 << 8 | 0 << 12,
    Instruction::IF_GEZ | 1 << 8, 3,
    Instruction::MOVE | 0 << 8 | 1 << 12,
    Instruction::ADD_INT | 2 << 8, 0 | 1 << 8,
    Instruction::RETURN_VOID);

  std::string expected_before =
      "BasicBlock 0, succ: 1\n"
      "  6: IntConstant [18, 22, 11]\n"
      "  8: IntConstant [22, 11]\n"
      "  4: SuspendCheck\n"
      "  5: Goto 1\n"
      "BasicBlock 1, pred: 0, succ: 5, 2\n"
      "  11: GreaterThanOrEqual(6, 8) [12]\n"
      "  12: If(11)\n"
      "BasicBlock 2, pred: 1, succ: 3\n"
      "  15: Goto 3\n"
      "BasicBlock 3, pred: 5, 2, succ: 4\n"
      "  22: Phi(8, 6) [18]\n"
      "  18: Add(22, 6)\n"
      "  20: ReturnVoid\n"
      "BasicBlock 4, pred: 3\n"
      "  21: Exit\n"
      "BasicBlock 5, pred: 1, succ: 3\n"
      "  0: Goto 3\n";

  // Expected difference after dead code elimination.
  diff_t expected_diff = {
    { "  6: IntConstant [18, 22, 11]\n", "  6: IntConstant [22, 11]\n" },
    { "  22: Phi(8, 6) [18]\n",          "  22: Phi(8, 6)\n" },
    { "  18: Add(22, 6)\n",              removed }
  };
  std::string expected_after = Patch(expected_before, expected_diff);

  TestCode(data, expected_before, expected_after);
}

/**
 * Three-register program with jumps leading to the creation of many
 * blocks.
 *
 * The intent of this test is to ensure that all dead instructions are
 * actually pruned at compile-time, thanks to the (backward)
 * post-order traversal of the the dominator tree.
 *
 *                              16-bit
 *                              offset
 *                              ------
 *     v0 <- 0                   0.     const/4 v0, #+0
 *     v1 <- 1                   1.     const/4 v1, #+1
 *     v2 <- v0 + v1             2.     add-int v2, v0, v1
 *     goto L2                   4.     goto +4
 * L1: v1 <- v0 + 3              5.     add-int/lit16 v1, v0, #+3
 *     goto L3                   7.     goto +4
 * L2: v0 <- v2 + 2              8.     add-int/lit16 v0, v2, #+2
 *     goto L1                  10.     goto +(-5)
 * L3: v2 <- v1 + 4             11.     add-int/lit16 v2, v1, #+4
 *     return                   13.     return-void
 */
TEST_F(DeadCodeEliminationTest, AdditionsAndInconditionalJumps) {
  const uint16_t data[] = THREE_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 << 8 | 0 << 12,
    Instruction::CONST_4 | 1 << 8 | 1 << 12,
    Instruction::ADD_INT | 2 << 8, 0 | 1 << 8,
    Instruction::GOTO | 4 << 8,
    Instruction::ADD_INT_LIT16 | 1 << 8 | 0 << 12, 3,
    Instruction::GOTO | 4 << 8,
    Instruction::ADD_INT_LIT16 | 0 << 8 | 2 << 12, 2,
    static_cast<uint16_t>(Instruction::GOTO | 0xFFFFFFFB << 8),
    Instruction::ADD_INT_LIT16 | 2 << 8 | 1 << 12, 4,
    Instruction::RETURN_VOID);

  std::string expected_before =
      "BasicBlock 0, succ: 1\n"
      "  5: IntConstant [11]\n"
      "  7: IntConstant [11]\n"
      "  15: IntConstant [16]\n"
      "  20: IntConstant [21]\n"
      "  25: IntConstant [26]\n"
      "  3: SuspendCheck\n"
      "  4: Goto 1\n"
      "BasicBlock 1, pred: 0, succ: 3\n"
      "  11: Add(5, 7) [21]\n"
      "  13: Goto 3\n"
      "BasicBlock 2, pred: 3, succ: 4\n"
      "  16: Add(21, 15) [26]\n"
      "  18: Goto 4\n"
      "BasicBlock 3, pred: 1, succ: 2\n"
      "  21: Add(11, 20) [16]\n"
      "  23: Goto 2\n"
      "BasicBlock 4, pred: 2, succ: 5\n"
      "  26: Add(16, 25)\n"
      "  28: ReturnVoid\n"
      "BasicBlock 5, pred: 4\n"
      "  29: Exit\n";

  std::string expected_after =
      "BasicBlock 0, succ: 1\n"
      "  3: SuspendCheck\n"
      "  4: Goto 1\n"
      "BasicBlock 1, pred: 0, succ: 5\n"
      "  28: ReturnVoid\n"
      "BasicBlock 5, pred: 1\n"
      "  29: Exit\n";

  TestCode(data, expected_before, expected_after);
}

}  // namespace art
