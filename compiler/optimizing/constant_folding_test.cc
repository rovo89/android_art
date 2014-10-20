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

#include <functional>

#include "code_generator_x86.h"
#include "constant_folding.h"
#include "dead_code_elimination.h"
#include "graph_checker.h"
#include "optimizing_unit_test.h"
#include "pretty_printer.h"

#include "gtest/gtest.h"

namespace art {

static void TestCode(const uint16_t* data,
                     const std::string& expected_before,
                     const std::string& expected_after_cf,
                     const std::string& expected_after_dce,
                     std::function<void(HGraph*)> check_after_cf,
                     Primitive::Type return_type = Primitive::kPrimInt) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraph* graph = CreateCFG(&allocator, data, return_type);
  ASSERT_NE(graph, nullptr);

  graph->BuildDominatorTree();
  graph->TransformToSSA();

  StringPrettyPrinter printer_before(graph);
  printer_before.VisitInsertionOrder();
  std::string actual_before = printer_before.str();
  ASSERT_EQ(expected_before, actual_before);

  x86::CodeGeneratorX86 codegen(graph);
  HGraphVisualizer visualizer(nullptr, graph, codegen, "");
  HConstantFolding(graph, visualizer).Run();
  SSAChecker ssa_checker(&allocator, graph);
  ssa_checker.Run();
  ASSERT_TRUE(ssa_checker.IsValid());

  StringPrettyPrinter printer_after_cf(graph);
  printer_after_cf.VisitInsertionOrder();
  std::string actual_after_cf = printer_after_cf.str();
  ASSERT_EQ(expected_after_cf, actual_after_cf);

  check_after_cf(graph);

  HDeadCodeElimination(graph, visualizer).Run();
  ssa_checker.Run();
  ASSERT_TRUE(ssa_checker.IsValid());

  StringPrettyPrinter printer_after_dce(graph);
  printer_after_dce.VisitInsertionOrder();
  std::string actual_after_dce = printer_after_dce.str();
  ASSERT_EQ(expected_after_dce, actual_after_dce);
}


/**
 * Tiny three-register program exercising int constant folding on negation.
 *
 *                              16-bit
 *                              offset
 *                              ------
 *     v0 <- 1                  0.      const/4 v0, #+1
 *     v1 <- -v0                1.      neg-int v0, v1
 *     return v1                2.      return v1
 */
TEST(ConstantFolding, IntConstantFoldingNegation) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 << 8 | 1 << 12,
    Instruction::NEG_INT | 1 << 8 | 0 << 12,
    Instruction::RETURN | 1 << 8);

  std::string expected_before =
      "BasicBlock 0, succ: 1\n"
      "  2: IntConstant [5]\n"
      "  10: SuspendCheck\n"
      "  11: Goto 1\n"
      "BasicBlock 1, pred: 0, succ: 2\n"
      "  5: Neg(2) [8]\n"
      "  8: Return(5)\n"
      "BasicBlock 2, pred: 1\n"
      "  9: Exit\n";

  // Expected difference after constant folding.
  diff_t expected_cf_diff = {
    { "  2: IntConstant [5]\n", "  2: IntConstant\n" },
    { "  5: Neg(2) [8]\n",      "  12: IntConstant [8]\n" },
    { "  8: Return(5)\n",       "  8: Return(12)\n" }
  };
  std::string expected_after_cf = Patch(expected_before, expected_cf_diff);

  // Check the value of the computed constant.
  auto check_after_cf = [](HGraph* graph) {
    HInstruction* inst = graph->GetBlock(1)->GetFirstInstruction();
    ASSERT_TRUE(inst->IsIntConstant());
    ASSERT_EQ(inst->AsIntConstant()->GetValue(), -1);
  };

  // Expected difference after dead code elimination.
  diff_t expected_dce_diff = {
    { "  2: IntConstant\n", removed },
  };
  std::string expected_after_dce = Patch(expected_after_cf, expected_dce_diff);

  TestCode(data,
           expected_before,
           expected_after_cf,
           expected_after_dce,
           check_after_cf);
}

/**
 * Tiny three-register program exercising int constant folding on addition.
 *
 *                              16-bit
 *                              offset
 *                              ------
 *     v0 <- 1                  0.      const/4 v0, #+1
 *     v1 <- 2                  1.      const/4 v1, #+2
 *     v2 <- v0 + v1            2.      add-int v2, v0, v1
 *     return v2                4.      return v2
 */
TEST(ConstantFolding, IntConstantFoldingOnAddition1) {
  const uint16_t data[] = THREE_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 << 8 | 1 << 12,
    Instruction::CONST_4 | 1 << 8 | 2 << 12,
    Instruction::ADD_INT | 2 << 8, 0 | 1 << 8,
    Instruction::RETURN | 2 << 8);

  std::string expected_before =
    "BasicBlock 0, succ: 1\n"
    "  3: IntConstant [9]\n"
    "  5: IntConstant [9]\n"
    "  14: SuspendCheck\n"
    "  15: Goto 1\n"
    "BasicBlock 1, pred: 0, succ: 2\n"
    "  9: Add(3, 5) [12]\n"
    "  12: Return(9)\n"
    "BasicBlock 2, pred: 1\n"
    "  13: Exit\n";

  // Expected difference after constant folding.
  diff_t expected_cf_diff = {
    { "  3: IntConstant [9]\n", "  3: IntConstant\n" },
    { "  5: IntConstant [9]\n", "  5: IntConstant\n" },
    { "  9: Add(3, 5) [12]\n",  "  16: IntConstant [12]\n" },
    { "  12: Return(9)\n",      "  12: Return(16)\n" }
  };
  std::string expected_after_cf = Patch(expected_before, expected_cf_diff);

  // Check the value of the computed constant.
  auto check_after_cf = [](HGraph* graph) {
    HInstruction* inst = graph->GetBlock(1)->GetFirstInstruction();
    ASSERT_TRUE(inst->IsIntConstant());
    ASSERT_EQ(inst->AsIntConstant()->GetValue(), 3);
  };

  // Expected difference after dead code elimination.
  diff_t expected_dce_diff = {
    { "  3: IntConstant\n", removed },
    { "  5: IntConstant\n", removed }
  };
  std::string expected_after_dce = Patch(expected_after_cf, expected_dce_diff);

  TestCode(data,
           expected_before,
           expected_after_cf,
           expected_after_dce,
           check_after_cf);
}

/**
 * Small three-register program exercising int constant folding on addition.
 *
 *                              16-bit
 *                              offset
 *                              ------
 *     v0 <- 1                  0.      const/4 v0, #+1
 *     v1 <- 2                  1.      const/4 v1, #+2
 *     v0 <- v0 + v1            2.      add-int/2addr v0, v1
 *     v1 <- 3                  3.      const/4 v1, #+3
 *     v2 <- 4                  4.      const/4 v2, #+4
 *     v1 <- v1 + v2            5.      add-int/2addr v1, v2
 *     v2 <- v0 + v1            6.      add-int v2, v0, v1
 *     return v2                8.      return v2
 */
TEST(ConstantFolding, IntConstantFoldingOnAddition2) {
  const uint16_t data[] = THREE_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 << 8 | 1 << 12,
    Instruction::CONST_4 | 1 << 8 | 2 << 12,
    Instruction::ADD_INT_2ADDR | 0 << 8 | 1 << 12,
    Instruction::CONST_4 | 1 << 8 | 3 << 12,
    Instruction::CONST_4 | 2 << 8 | 4 << 12,
    Instruction::ADD_INT_2ADDR | 1 << 8 | 2 << 12,
    Instruction::ADD_INT | 2 << 8, 0 | 1 << 8,
    Instruction::RETURN | 2 << 8);

  std::string expected_before =
    "BasicBlock 0, succ: 1\n"
    "  3: IntConstant [9]\n"
    "  5: IntConstant [9]\n"
    "  11: IntConstant [17]\n"
    "  13: IntConstant [17]\n"
    "  26: SuspendCheck\n"
    "  27: Goto 1\n"
    "BasicBlock 1, pred: 0, succ: 2\n"
    "  9: Add(3, 5) [21]\n"
    "  17: Add(11, 13) [21]\n"
    "  21: Add(9, 17) [24]\n"
    "  24: Return(21)\n"
    "BasicBlock 2, pred: 1\n"
    "  25: Exit\n";

  // Expected difference after constant folding.
  diff_t expected_cf_diff = {
    { "  3: IntConstant [9]\n",   "  3: IntConstant\n" },
    { "  5: IntConstant [9]\n",   "  5: IntConstant\n" },
    { "  11: IntConstant [17]\n", "  11: IntConstant\n" },
    { "  13: IntConstant [17]\n", "  13: IntConstant\n" },
    { "  9: Add(3, 5) [21]\n",    "  28: IntConstant\n" },
    { "  17: Add(11, 13) [21]\n", "  29: IntConstant\n" },
    { "  21: Add(9, 17) [24]\n",  "  30: IntConstant [24]\n" },
    { "  24: Return(21)\n",       "  24: Return(30)\n" }
  };
  std::string expected_after_cf = Patch(expected_before, expected_cf_diff);

  // Check the values of the computed constants.
  auto check_after_cf = [](HGraph* graph) {
    HInstruction* inst1 = graph->GetBlock(1)->GetFirstInstruction();
    ASSERT_TRUE(inst1->IsIntConstant());
    ASSERT_EQ(inst1->AsIntConstant()->GetValue(), 3);
    HInstruction* inst2 = inst1->GetNext();
    ASSERT_TRUE(inst2->IsIntConstant());
    ASSERT_EQ(inst2->AsIntConstant()->GetValue(), 7);
    HInstruction* inst3 = inst2->GetNext();
    ASSERT_TRUE(inst3->IsIntConstant());
    ASSERT_EQ(inst3->AsIntConstant()->GetValue(), 10);
  };

  // Expected difference after dead code elimination.
  diff_t expected_dce_diff = {
    { "  3: IntConstant\n",  removed },
    { "  5: IntConstant\n",  removed },
    { "  11: IntConstant\n", removed },
    { "  13: IntConstant\n", removed },
    { "  28: IntConstant\n", removed },
    { "  29: IntConstant\n", removed }
  };
  std::string expected_after_dce = Patch(expected_after_cf, expected_dce_diff);

  TestCode(data,
           expected_before,
           expected_after_cf,
           expected_after_dce,
           check_after_cf);
}

/**
 * Tiny three-register program exercising int constant folding on subtraction.
 *
 *                              16-bit
 *                              offset
 *                              ------
 *     v0 <- 3                  0.      const/4 v0, #+3
 *     v1 <- 2                  1.      const/4 v1, #+2
 *     v2 <- v0 - v1            2.      sub-int v2, v0, v1
 *     return v2                4.      return v2
 */
TEST(ConstantFolding, IntConstantFoldingOnSubtraction) {
  const uint16_t data[] = THREE_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 << 8 | 3 << 12,
    Instruction::CONST_4 | 1 << 8 | 2 << 12,
    Instruction::SUB_INT | 2 << 8, 0 | 1 << 8,
    Instruction::RETURN | 2 << 8);

  std::string expected_before =
    "BasicBlock 0, succ: 1\n"
    "  3: IntConstant [9]\n"
    "  5: IntConstant [9]\n"
    "  14: SuspendCheck\n"
    "  15: Goto 1\n"
    "BasicBlock 1, pred: 0, succ: 2\n"
    "  9: Sub(3, 5) [12]\n"
    "  12: Return(9)\n"
    "BasicBlock 2, pred: 1\n"
    "  13: Exit\n";

  // Expected difference after constant folding.
  diff_t expected_cf_diff = {
    { "  3: IntConstant [9]\n", "  3: IntConstant\n" },
    { "  5: IntConstant [9]\n", "  5: IntConstant\n" },
    { "  9: Sub(3, 5) [12]\n",  "  16: IntConstant [12]\n" },
    { "  12: Return(9)\n",      "  12: Return(16)\n" }
  };
  std::string expected_after_cf = Patch(expected_before, expected_cf_diff);

  // Check the value of the computed constant.
  auto check_after_cf = [](HGraph* graph) {
    HInstruction* inst = graph->GetBlock(1)->GetFirstInstruction();
    ASSERT_TRUE(inst->IsIntConstant());
    ASSERT_EQ(inst->AsIntConstant()->GetValue(), 1);
  };

  // Expected difference after dead code elimination.
  diff_t expected_dce_diff = {
    { "  3: IntConstant\n", removed },
    { "  5: IntConstant\n", removed }
  };
  std::string expected_after_dce = Patch(expected_after_cf, expected_dce_diff);

  TestCode(data,
           expected_before,
           expected_after_cf,
           expected_after_dce,
           check_after_cf);
}

#define SIX_REGISTERS_CODE_ITEM(...)                                     \
    { 6, 0, 0, 0, 0, 0, NUM_INSTRUCTIONS(__VA_ARGS__), 0, __VA_ARGS__ }

/**
 * Tiny three-register-pair program exercising long constant folding
 * on addition.
 *
 *                              16-bit
 *                              offset
 *                              ------
 *     (v0, v1) <- 1            0.      const-wide/16 v0, #+1
 *     (v2, v3) <- 2            2.      const-wide/16 v2, #+2
 *     (v4, v5) <-
 *       (v0, v1) + (v1, v2)    4.      add-long v4, v0, v2
 *     return (v4, v5)          6.      return-wide v4
 */
TEST(ConstantFolding, LongConstantFoldingOnAddition) {
  const uint16_t data[] = SIX_REGISTERS_CODE_ITEM(
    Instruction::CONST_WIDE_16 | 0 << 8, 1,
    Instruction::CONST_WIDE_16 | 2 << 8, 2,
    Instruction::ADD_LONG | 4 << 8, 0 | 2 << 8,
    Instruction::RETURN_WIDE | 4 << 8);

  std::string expected_before =
    "BasicBlock 0, succ: 1\n"
    "  6: LongConstant [12]\n"
    "  8: LongConstant [12]\n"
    "  17: SuspendCheck\n"
    "  18: Goto 1\n"
    "BasicBlock 1, pred: 0, succ: 2\n"
    "  12: Add(6, 8) [15]\n"
    "  15: Return(12)\n"
    "BasicBlock 2, pred: 1\n"
    "  16: Exit\n";

  // Expected difference after constant folding.
  diff_t expected_cf_diff = {
    { "  6: LongConstant [12]\n", "  6: LongConstant\n" },
    { "  8: LongConstant [12]\n", "  8: LongConstant\n" },
    { "  12: Add(6, 8) [15]\n",   "  19: LongConstant [15]\n" },
    { "  15: Return(12)\n",       "  15: Return(19)\n" }
  };
  std::string expected_after_cf = Patch(expected_before, expected_cf_diff);

  // Check the value of the computed constant.
  auto check_after_cf = [](HGraph* graph) {
    HInstruction* inst = graph->GetBlock(1)->GetFirstInstruction();
    ASSERT_TRUE(inst->IsLongConstant());
    ASSERT_EQ(inst->AsLongConstant()->GetValue(), 3);
  };

  // Expected difference after dead code elimination.
  diff_t expected_dce_diff = {
    { "  6: LongConstant\n", removed },
    { "  8: LongConstant\n", removed }
  };
  std::string expected_after_dce = Patch(expected_after_cf, expected_dce_diff);

  TestCode(data,
           expected_before,
           expected_after_cf,
           expected_after_dce,
           check_after_cf,
           Primitive::kPrimLong);
}

/**
 * Tiny three-register-pair program exercising long constant folding
 * on subtraction.
 *
 *                              16-bit
 *                              offset
 *                              ------
 *     (v0, v1) <- 3            0.      const-wide/16 v0, #+3
 *     (v2, v3) <- 2            2.      const-wide/16 v2, #+2
 *     (v4, v5) <-
 *       (v0, v1) - (v1, v2)    4.      sub-long v4, v0, v2
 *     return (v4, v5)          6.      return-wide v4
 */
TEST(ConstantFolding, LongConstantFoldingOnSubtraction) {
  const uint16_t data[] = SIX_REGISTERS_CODE_ITEM(
    Instruction::CONST_WIDE_16 | 0 << 8, 3,
    Instruction::CONST_WIDE_16 | 2 << 8, 2,
    Instruction::SUB_LONG | 4 << 8, 0 | 2 << 8,
    Instruction::RETURN_WIDE | 4 << 8);

  std::string expected_before =
    "BasicBlock 0, succ: 1\n"
    "  6: LongConstant [12]\n"
    "  8: LongConstant [12]\n"
    "  17: SuspendCheck\n"
    "  18: Goto 1\n"
    "BasicBlock 1, pred: 0, succ: 2\n"
    "  12: Sub(6, 8) [15]\n"
    "  15: Return(12)\n"
    "BasicBlock 2, pred: 1\n"
    "  16: Exit\n";

  // Expected difference after constant folding.
  diff_t expected_cf_diff = {
    { "  6: LongConstant [12]\n", "  6: LongConstant\n" },
    { "  8: LongConstant [12]\n", "  8: LongConstant\n" },
    { "  12: Sub(6, 8) [15]\n",   "  19: LongConstant [15]\n" },
    { "  15: Return(12)\n",       "  15: Return(19)\n" }
  };
  std::string expected_after_cf = Patch(expected_before, expected_cf_diff);

  // Check the value of the computed constant.
  auto check_after_cf = [](HGraph* graph) {
    HInstruction* inst = graph->GetBlock(1)->GetFirstInstruction();
    ASSERT_TRUE(inst->IsLongConstant());
    ASSERT_EQ(inst->AsLongConstant()->GetValue(), 1);
  };

  // Expected difference after dead code elimination.
  diff_t expected_dce_diff = {
    { "  6: LongConstant\n", removed },
    { "  8: LongConstant\n", removed }
  };
  std::string expected_after_dce = Patch(expected_after_cf, expected_dce_diff);

  TestCode(data,
           expected_before,
           expected_after_cf,
           expected_after_dce,
           check_after_cf,
           Primitive::kPrimLong);
}

/**
 * Three-register program with jumps leading to the creation of many
 * blocks.
 *
 * The intent of this test is to ensure that all constant expressions
 * are actually evaluated at compile-time, thanks to the reverse
 * (forward) post-order traversal of the the dominator tree.
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
 *     return v2                13.     return v2
 */
TEST(ConstantFolding, IntConstantFoldingAndJumps) {
  const uint16_t data[] = THREE_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 << 8 | 0 << 12,
    Instruction::CONST_4 | 1 << 8 | 1 << 12,
    Instruction::ADD_INT | 2 << 8, 0 | 1 << 8,
    Instruction::GOTO | 4 << 8,
    Instruction::ADD_INT_LIT16 | 1 << 8 | 0 << 12, 3,
    Instruction::GOTO | 4 << 8,
    Instruction::ADD_INT_LIT16 | 0 << 8 | 2 << 12, 2,
    static_cast<uint16_t>(Instruction::GOTO | -5 << 8),
    Instruction::ADD_INT_LIT16 | 2 << 8 | 1 << 12, 4,
    Instruction::RETURN | 2 << 8);

  std::string expected_before =
    "BasicBlock 0, succ: 1\n"
    "  3: IntConstant [9]\n"            // v0 <- 0
    "  5: IntConstant [9]\n"            // v1 <- 1
    "  13: IntConstant [14]\n"          // const 3
    "  18: IntConstant [19]\n"          // const 2
    "  24: IntConstant [25]\n"          // const 4
    "  30: SuspendCheck\n"
    "  31: Goto 1\n"
    "BasicBlock 1, pred: 0, succ: 3\n"
    "  9: Add(3, 5) [19]\n"             // v2 <- v0 + v1 = 0 + 1 = 1
    "  11: Goto 3\n"                    // goto L2
    "BasicBlock 2, pred: 3, succ: 4\n"  // L1:
    "  14: Add(19, 13) [25]\n"          // v1 <- v0 + 3 = 3 + 3 = 6
    "  16: Goto 4\n"                    // goto L3
    "BasicBlock 3, pred: 1, succ: 2\n"  // L2:
    "  19: Add(9, 18) [14]\n"           // v0 <- v2 + 2 = 1 + 2 = 3
    "  21: SuspendCheck\n"
    "  22: Goto 2\n"                    // goto L1
    "BasicBlock 4, pred: 2, succ: 5\n"  // L3:
    "  25: Add(14, 24) [28]\n"          // v2 <- v1 + 4 = 6 + 4 = 10
    "  28: Return(25)\n"                // return v2
    "BasicBlock 5, pred: 4\n"
    "  29: Exit\n";

  // Expected difference after constant folding.
  diff_t expected_cf_diff = {
    { "  3: IntConstant [9]\n",   "  3: IntConstant\n" },
    { "  5: IntConstant [9]\n",   "  5: IntConstant []\n" },
    { "  13: IntConstant [14]\n", "  13: IntConstant\n" },
    { "  18: IntConstant [19]\n", "  18: IntConstant\n" },
    { "  24: IntConstant [25]\n", "  24: IntConstant\n" },
    { "  9: Add(3, 5) [19]\n",    "  32: IntConstant []\n" },
    { "  14: Add(19, 13) [25]\n", "  34: IntConstant\n" },
    { "  19: Add(9, 18) [14]\n",  "  33: IntConstant []\n" },
    { "  25: Add(14, 24) [28]\n", "  35: IntConstant [28]\n" },
    { "  28: Return(25)\n",       "  28: Return(35)\n"}
  };
  std::string expected_after_cf = Patch(expected_before, expected_cf_diff);

  // Check the values of the computed constants.
  auto check_after_cf = [](HGraph* graph) {
    HInstruction* inst1 = graph->GetBlock(1)->GetFirstInstruction();
    ASSERT_TRUE(inst1->IsIntConstant());
    ASSERT_EQ(inst1->AsIntConstant()->GetValue(), 1);
    HInstruction* inst2 = graph->GetBlock(2)->GetFirstInstruction();
    ASSERT_TRUE(inst2->IsIntConstant());
    ASSERT_EQ(inst2->AsIntConstant()->GetValue(), 6);
    HInstruction* inst3 = graph->GetBlock(3)->GetFirstInstruction();
    ASSERT_TRUE(inst3->IsIntConstant());
    ASSERT_EQ(inst3->AsIntConstant()->GetValue(), 3);
    HInstruction* inst4 = graph->GetBlock(4)->GetFirstInstruction();
    ASSERT_TRUE(inst4->IsIntConstant());
    ASSERT_EQ(inst4->AsIntConstant()->GetValue(), 10);
  };

  // Expected difference after dead code elimination.
  diff_t expected_dce_diff = {
    { "  3: IntConstant\n",     removed },
    { "  13: IntConstant\n",    removed },
    { "  18: IntConstant\n",    removed },
    { "  24: IntConstant\n",    removed },
    { "  34: IntConstant\n",    removed },
  };
  std::string expected_after_dce = Patch(expected_after_cf, expected_dce_diff);

  TestCode(data,
           expected_before,
           expected_after_cf,
           expected_after_dce,
           check_after_cf);
}


/**
 * Three-register program with a constant (static) condition.
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
TEST(ConstantFolding, ConstantCondition) {
  const uint16_t data[] = THREE_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 1 << 8 | 1 << 12,
    Instruction::CONST_4 | 0 << 8 | 0 << 12,
    Instruction::IF_GEZ | 1 << 8, 3,
    Instruction::MOVE | 0 << 8 | 1 << 12,
    Instruction::ADD_INT | 2 << 8, 0 | 1 << 8,
    Instruction::RETURN_VOID);

  std::string expected_before =
    "BasicBlock 0, succ: 1\n"
    "  3: IntConstant [15, 22, 8]\n"
    "  5: IntConstant [22, 8]\n"
    "  19: SuspendCheck\n"
    "  20: Goto 1\n"
    "BasicBlock 1, pred: 0, succ: 5, 2\n"
    "  8: GreaterThanOrEqual(3, 5) [9]\n"
    "  9: If(8)\n"
    "BasicBlock 2, pred: 1, succ: 3\n"
    "  12: Goto 3\n"
    "BasicBlock 3, pred: 2, 5, succ: 4\n"
    "  22: Phi(3, 5) [15]\n"
    "  15: Add(22, 3)\n"
    "  17: ReturnVoid\n"
    "BasicBlock 4, pred: 3\n"
    "  18: Exit\n"
    "BasicBlock 5, pred: 1, succ: 3\n"
    "  21: Goto 3\n";

  // Expected difference after constant folding.
  diff_t expected_cf_diff = {
    { "  3: IntConstant [15, 22, 8]\n",      "  3: IntConstant [15, 22]\n" },
    { "  5: IntConstant [22, 8]\n",          "  5: IntConstant [22]\n" },
    { "  8: GreaterThanOrEqual(3, 5) [9]\n", "  23: IntConstant [9]\n" },
    { "  9: If(8)\n",                        "  9: If(23)\n" }
  };
  std::string expected_after_cf = Patch(expected_before, expected_cf_diff);

  // Check the values of the computed constants.
  auto check_after_cf = [](HGraph* graph) {
    HInstruction* inst = graph->GetBlock(1)->GetFirstInstruction();
    ASSERT_TRUE(inst->IsIntConstant());
    ASSERT_EQ(inst->AsIntConstant()->GetValue(), 1);
  };

  // Expected difference after dead code elimination.
  diff_t expected_dce_diff = {
    { "  3: IntConstant [15, 22]\n", "  3: IntConstant [22]\n" },
    { "  22: Phi(3, 5) [15]\n",      "  22: Phi(3, 5)\n" },
    { "  15: Add(22, 3)\n",          removed }
  };
  std::string expected_after_dce = Patch(expected_after_cf, expected_dce_diff);

  TestCode(data,
           expected_before,
           expected_after_cf,
           expected_after_dce,
           check_after_cf);
}

}  // namespace art
