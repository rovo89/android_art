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

#include "constant_propagation.h"
#include "dead_code_elimination.h"
#include "pretty_printer.h"
#include "graph_checker.h"
#include "optimizing_unit_test.h"

#include "gtest/gtest.h"

namespace art {

static void TestCode(const uint16_t* data,
                     const std::string& expected_before,
                     const std::string& expected_after_cp,
                     const std::string& expected_after_dce) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraph* graph = CreateCFG(&allocator, data);
  ASSERT_NE(graph, nullptr);

  graph->BuildDominatorTree();
  graph->TransformToSSA();

  StringPrettyPrinter printer_before(graph);
  printer_before.VisitInsertionOrder();
  std::string actual_before = printer_before.str();
  ASSERT_EQ(expected_before, actual_before);

  ConstantPropagation(graph).Run();

  StringPrettyPrinter printer_after_cp(graph);
  printer_after_cp.VisitInsertionOrder();
  std::string actual_after_cp = printer_after_cp.str();
  ASSERT_EQ(expected_after_cp, actual_after_cp);

  DeadCodeElimination(graph).Run();

  StringPrettyPrinter printer_after_dce(graph);
  printer_after_dce.VisitInsertionOrder();
  std::string actual_after_dce = printer_after_dce.str();
  ASSERT_EQ(expected_after_dce, actual_after_dce);

  SSAChecker ssa_checker(&allocator, graph);
  ssa_checker.VisitInsertionOrder();
  ASSERT_TRUE(ssa_checker.IsValid());
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
TEST(ConstantPropagation, IntConstantFoldingOnAddition1) {
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

  // Expected difference after constant propagation.
  diff_t expected_cp_diff = {
    { "  3: IntConstant [9]\n", "  3: IntConstant\n" },
    { "  5: IntConstant [9]\n", "  5: IntConstant\n" },
    { "  9: Add(3, 5) [12]\n",  "  16: IntConstant [12]\n" },
    { "  12: Return(9)\n",      "  12: Return(16)\n" }
  };
  std::string expected_after_cp = Patch(expected_before, expected_cp_diff);

  // Expected difference after dead code elimination.
  diff_t expected_dce_diff = {
    { "  3: IntConstant\n", removed },
    { "  5: IntConstant\n", removed }
  };
  std::string expected_after_dce = Patch(expected_after_cp, expected_dce_diff);

  TestCode(data, expected_before, expected_after_cp, expected_after_dce);
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
TEST(ConstantPropagation, IntConstantFoldingOnAddition2) {
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

  // Expected difference after constant propagation.
  diff_t expected_cp_diff = {
    { "  3: IntConstant [9]\n",   "  3: IntConstant\n" },
    { "  5: IntConstant [9]\n",   "  5: IntConstant\n" },
    { "  11: IntConstant [17]\n", "  11: IntConstant\n" },
    { "  13: IntConstant [17]\n", "  13: IntConstant\n" },
    { "  9: Add(3, 5) [21]\n",    "  28: IntConstant\n" },
    { "  17: Add(11, 13) [21]\n", "  29: IntConstant\n" },
    { "  21: Add(9, 17) [24]\n",  "  30: IntConstant [24]\n" },
    { "  24: Return(21)\n",       "  24: Return(30)\n" }
  };
  std::string expected_after_cp = Patch(expected_before, expected_cp_diff);

  // Expected difference after dead code elimination.
  diff_t expected_dce_diff = {
    { "  3: IntConstant\n",  removed },
    { "  5: IntConstant\n",  removed },
    { "  11: IntConstant\n", removed },
    { "  13: IntConstant\n", removed },
    { "  28: IntConstant\n", removed },
    { "  29: IntConstant\n", removed }
  };
  std::string expected_after_dce = Patch(expected_after_cp, expected_dce_diff);

  TestCode(data, expected_before, expected_after_cp, expected_after_dce);
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
TEST(ConstantPropagation, IntConstantFoldingOnSubtraction) {
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

  // Expected difference after constant propagation.
  diff_t expected_cp_diff = {
    { "  3: IntConstant [9]\n", "  3: IntConstant\n" },
    { "  5: IntConstant [9]\n", "  5: IntConstant\n" },
    { "  9: Sub(3, 5) [12]\n",  "  16: IntConstant [12]\n" },
    { "  12: Return(9)\n",      "  12: Return(16)\n" }
  };
  std::string expected_after_cp = Patch(expected_before, expected_cp_diff);

  // Expected difference after dead code elimination.
  diff_t expected_dce_diff = {
    { "  3: IntConstant\n", removed },
    { "  5: IntConstant\n", removed }
  };
  std::string expected_after_dce = Patch(expected_after_cp, expected_dce_diff);

  TestCode(data, expected_before, expected_after_cp, expected_after_dce);
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
TEST(ConstantPropagation, LongConstantFoldingOnAddition) {
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

  // Expected difference after constant propagation.
  diff_t expected_cp_diff = {
    { "  6: LongConstant [12]\n", "  6: LongConstant\n" },
    { "  8: LongConstant [12]\n", "  8: LongConstant\n" },
    { "  12: Add(6, 8) [15]\n",   "  19: LongConstant [15]\n" },
    { "  15: Return(12)\n",       "  15: Return(19)\n" }
  };
  std::string expected_after_cp = Patch(expected_before, expected_cp_diff);

  // Expected difference after dead code elimination.
  diff_t expected_dce_diff = {
    { "  6: LongConstant\n", removed },
    { "  8: LongConstant\n", removed }
  };
  std::string expected_after_dce = Patch(expected_after_cp, expected_dce_diff);

  TestCode(data, expected_before, expected_after_cp, expected_after_dce);
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
TEST(ConstantPropagation, LongConstantFoldingOnSubtraction) {
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

  // Expected difference after constant propagation.
  diff_t expected_cp_diff = {
    { "  6: LongConstant [12]\n", "  6: LongConstant\n" },
    { "  8: LongConstant [12]\n", "  8: LongConstant\n" },
    { "  12: Sub(6, 8) [15]\n",   "  19: LongConstant [15]\n" },
    { "  15: Return(12)\n",       "  15: Return(19)\n" }
  };
  std::string expected_after_cp = Patch(expected_before, expected_cp_diff);

  // Expected difference after dead code elimination.
  diff_t expected_dce_diff = {
    { "  6: LongConstant\n", removed },
    { "  8: LongConstant\n", removed }
  };
  std::string expected_after_dce = Patch(expected_after_cp, expected_dce_diff);

  TestCode(data, expected_before, expected_after_cp, expected_after_dce);
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
TEST(ConstantPropagation, IntConstantFoldingAndJumps) {
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
    "  3: IntConstant [9]\n"
    "  5: IntConstant [9]\n"
    "  13: IntConstant [14]\n"
    "  18: IntConstant [19]\n"
    "  24: IntConstant [25]\n"
    "  30: SuspendCheck\n"
    "  31: Goto 1\n"
    "BasicBlock 1, pred: 0, succ: 3\n"
    "  9: Add(3, 5) [19]\n"
    "  11: Goto 3\n"
    "BasicBlock 2, pred: 3, succ: 4\n"
    "  14: Add(19, 13) [25]\n"
    "  16: Goto 4\n"
    "BasicBlock 3, pred: 1, succ: 2\n"
    "  19: Add(9, 18) [14]\n"
    "  21: SuspendCheck\n"
    "  22: Goto 2\n"
    "BasicBlock 4, pred: 2, succ: 5\n"
    "  25: Add(14, 24) [28]\n"
    "  28: Return(25)\n"
    "BasicBlock 5, pred: 4\n"
    "  29: Exit\n";

  // Expected difference after constant propagation.
  diff_t expected_cp_diff = {
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
  std::string expected_after_cp = Patch(expected_before, expected_cp_diff);

  // Expected difference after dead code elimination.
  diff_t expected_dce_diff = {
    { "  3: IntConstant\n",     removed },
    { "  13: IntConstant\n",    removed },
    { "  18: IntConstant\n",    removed },
    { "  24: IntConstant\n",    removed },
    { "  34: IntConstant\n",    removed },
  };
  std::string expected_after_dce = Patch(expected_after_cp, expected_dce_diff);

  TestCode(data, expected_before, expected_after_cp, expected_after_dce);
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
TEST(ConstantPropagation, ConstantCondition) {
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

  // Expected difference after constant propagation.
  diff_t expected_cp_diff = {
    { "  3: IntConstant [15, 22, 8]\n",      "  3: IntConstant [15, 22]\n" },
    { "  5: IntConstant [22, 8]\n",          "  5: IntConstant [22]\n" },
    { "  8: GreaterThanOrEqual(3, 5) [9]\n", "  23: IntConstant [9]\n" },
    { "  9: If(8)\n",                        "  9: If(23)\n" }
  };
  std::string expected_after_cp = Patch(expected_before, expected_cp_diff);

  // Expected difference after dead code elimination.
  diff_t expected_dce_diff = {
    { "  3: IntConstant [15, 22]\n", "  3: IntConstant [22]\n" },
    { "  22: Phi(3, 5) [15]\n",      "  22: Phi(3, 5)\n" },
    { "  15: Add(22, 3)\n",          removed }
  };
  std::string expected_after_dce = Patch(expected_after_cp, expected_dce_diff);

  TestCode(data, expected_before, expected_after_cp, expected_after_dce);
}

}  // namespace art
