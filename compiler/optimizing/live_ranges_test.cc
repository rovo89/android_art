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

#include "builder.h"
#include "code_generator.h"
#include "dex_file.h"
#include "dex_instruction.h"
#include "nodes.h"
#include "optimizing_unit_test.h"
#include "ssa_liveness_analysis.h"
#include "utils/arena_allocator.h"

#include "gtest/gtest.h"

namespace art {

static HGraph* BuildGraph(const uint16_t* data, ArenaAllocator* allocator) {
  HGraphBuilder builder(allocator);
  const DexFile::CodeItem* item = reinterpret_cast<const DexFile::CodeItem*>(data);
  HGraph* graph = builder.BuildGraph(*item);
  graph->BuildDominatorTree();
  graph->TransformToSSA();
  graph->FindNaturalLoops();
  return graph;
}

TEST(LiveRangesTest, CFG1) {
  /*
   * Test the following snippet:
   *  return 0;
   *
   * Which becomes the following graph (numbered by lifetime position):
   *       2: constant0
   *       4: goto
   *           |
   *       8: return
   *           |
   *       12: exit
   */
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::RETURN);

  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraph* graph = BuildGraph(data, &allocator);

  CodeGenerator* codegen = CodeGenerator::Create(&allocator, graph, InstructionSet::kX86);
  SsaLivenessAnalysis liveness(*graph, codegen);
  liveness.Analyze();

  LiveInterval* interval = liveness.GetInstructionFromSsaIndex(0)->GetLiveInterval();
  LiveRange* range = interval->GetFirstRange();
  ASSERT_EQ(2u, range->GetStart());
  // Last use is the return instruction.
  ASSERT_EQ(9u, range->GetEnd());
  HBasicBlock* block = graph->GetBlocks().Get(1);
  ASSERT_TRUE(block->GetLastInstruction()->AsReturn() != nullptr);
  ASSERT_EQ(8u, block->GetLastInstruction()->GetLifetimePosition());
  ASSERT_TRUE(range->GetNext() == nullptr);
}

TEST(LiveRangesTest, CFG2) {
  /*
   * Test the following snippet:
   *  var a = 0;
   *  if (0 == 0) {
   *  } else {
   *  }
   *  return a;
   *
   * Which becomes the following graph (numbered by lifetime position):
   *       2: constant0
   *       4: goto
   *           |
   *       8: equal
   *       10: if
   *       /       \
   *   14: goto   18: goto
   *       \       /
   *       22: return
   *         |
   *       26: exit
   */
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 3,
    Instruction::GOTO | 0x100,
    Instruction::RETURN | 0 << 8);

  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraph* graph = BuildGraph(data, &allocator);
  CodeGenerator* codegen = CodeGenerator::Create(&allocator, graph, InstructionSet::kX86);
  SsaLivenessAnalysis liveness(*graph, codegen);
  liveness.Analyze();

  LiveInterval* interval = liveness.GetInstructionFromSsaIndex(0)->GetLiveInterval();
  LiveRange* range = interval->GetFirstRange();
  ASSERT_EQ(2u, range->GetStart());
  // Last use is the return instruction.
  ASSERT_EQ(23u, range->GetEnd());
  HBasicBlock* block = graph->GetBlocks().Get(3);
  ASSERT_TRUE(block->GetLastInstruction()->AsReturn() != nullptr);
  ASSERT_EQ(22u, block->GetLastInstruction()->GetLifetimePosition());
  ASSERT_TRUE(range->GetNext() == nullptr);
}

TEST(LiveRangesTest, CFG3) {
  /*
   * Test the following snippet:
   *  var a = 0;
   *  if (0 == 0) {
   *  } else {
   *    a = 4;
   *  }
   *  return a;
   *
   * Which becomes the following graph (numbered by lifetime position):
   *       2: constant0
   *       4: constant4
   *       6: goto
   *           |
   *       10: equal
   *       12: if
   *       /       \
   *   16: goto   20: goto
   *       \       /
   *       22: phi
   *       24: return
   *         |
   *       38: exit
   */
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 3,
    Instruction::CONST_4 | 4 << 12 | 0,
    Instruction::RETURN | 0 << 8);

  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraph* graph = BuildGraph(data, &allocator);
  CodeGenerator* codegen = CodeGenerator::Create(&allocator, graph, InstructionSet::kX86);
  SsaLivenessAnalysis liveness(*graph, codegen);
  liveness.Analyze();

  // Test for the 4 constant.
  LiveInterval* interval = liveness.GetInstructionFromSsaIndex(1)->GetLiveInterval();
  LiveRange* range = interval->GetFirstRange();
  ASSERT_EQ(4u, range->GetStart());
  // Last use is the phi at the return block so instruction is live until
  // the end of the then block.
  ASSERT_EQ(18u, range->GetEnd());
  ASSERT_TRUE(range->GetNext() == nullptr);

  // Test for the 0 constant.
  interval = liveness.GetInstructionFromSsaIndex(0)->GetLiveInterval();
  // The then branch is a hole for this constant, therefore its interval has 2 ranges.
  // First range starts from the definition and ends at the if block.
  range = interval->GetFirstRange();
  ASSERT_EQ(2u, range->GetStart());
  // 14 is the end of the if block.
  ASSERT_EQ(14u, range->GetEnd());
  // Second range is the else block.
  range = range->GetNext();
  ASSERT_EQ(18u, range->GetStart());
  // Last use is the phi at the return block.
  ASSERT_EQ(22u, range->GetEnd());
  ASSERT_TRUE(range->GetNext() == nullptr);

  // Test for the phi.
  interval = liveness.GetInstructionFromSsaIndex(2)->GetLiveInterval();
  range = interval->GetFirstRange();
  ASSERT_EQ(22u, liveness.GetInstructionFromSsaIndex(2)->GetLifetimePosition());
  ASSERT_EQ(22u, range->GetStart());
  ASSERT_EQ(25u, range->GetEnd());
  ASSERT_TRUE(range->GetNext() == nullptr);
}

TEST(LiveRangesTest, Loop) {
  /*
   * Test the following snippet:
   *  var a = 0;
   *  while (a == a) {
   *    a = 4;
   *  }
   *  return 5;
   *
   * Which becomes the following graph (numbered by lifetime position):
   *       2: constant0
   *       4: constant4
   *       6: constant5
   *       8: goto
   *           |
   *       12: goto
   *           |
   *       14: phi
   *       16: equal
   *       18: if +++++
   *        |       \ +
   *        |     22: goto
   *        |
   *       26: return
   *         |
   *       30: exit
   */

  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 4,
    Instruction::CONST_4 | 4 << 12 | 0,
    Instruction::GOTO | 0xFD00,
    Instruction::CONST_4 | 5 << 12 | 1 << 8,
    Instruction::RETURN | 1 << 8);

  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraph* graph = BuildGraph(data, &allocator);
  CodeGenerator* codegen = CodeGenerator::Create(&allocator, graph, InstructionSet::kX86);
  SsaLivenessAnalysis liveness(*graph, codegen);
  liveness.Analyze();

  // Test for the 0 constant.
  LiveInterval* interval = liveness.GetInstructionFromSsaIndex(0)->GetLiveInterval();
  LiveRange* range = interval->GetFirstRange();
  ASSERT_EQ(2u, range->GetStart());
  // Last use is the loop phi so instruction is live until
  // the end of the pre loop header.
  ASSERT_EQ(14u, range->GetEnd());
  ASSERT_TRUE(range->GetNext() == nullptr);

  // Test for the 4 constant.
  interval = liveness.GetInstructionFromSsaIndex(1)->GetLiveInterval();
  range = interval->GetFirstRange();
  // The instruction is live until the end of the loop.
  ASSERT_EQ(4u, range->GetStart());
  ASSERT_EQ(24u, range->GetEnd());
  ASSERT_TRUE(range->GetNext() == nullptr);

  // Test for the 5 constant.
  interval = liveness.GetInstructionFromSsaIndex(2)->GetLiveInterval();
  range = interval->GetFirstRange();
  // The instruction is live until the return instruction after the loop.
  ASSERT_EQ(6u, range->GetStart());
  ASSERT_EQ(27u, range->GetEnd());
  ASSERT_TRUE(range->GetNext() == nullptr);

  // Test for the phi.
  interval = liveness.GetInstructionFromSsaIndex(3)->GetLiveInterval();
  range = interval->GetFirstRange();
  // Instruction is consumed by the if.
  ASSERT_EQ(14u, range->GetStart());
  ASSERT_EQ(17u, range->GetEnd());
  ASSERT_TRUE(range->GetNext() == nullptr);
}

}  // namespace art
