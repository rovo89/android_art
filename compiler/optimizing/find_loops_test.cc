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
#include "dex_file.h"
#include "dex_instruction.h"
#include "nodes.h"
#include "optimizing_unit_test.h"
#include "ssa_liveness_analysis.h"
#include "utils/arena_allocator.h"
#include "pretty_printer.h"

#include "gtest/gtest.h"

namespace art {

static HGraph* TestCode(const uint16_t* data, ArenaPool* pool) {
  ArenaAllocator allocator(pool);
  HGraphBuilder builder(&allocator);
  const DexFile::CodeItem* item = reinterpret_cast<const DexFile::CodeItem*>(data);
  HGraph* graph = builder.BuildGraph(*item);
  graph->BuildDominatorTree();
  graph->FindNaturalLoops();
  return graph;
}

TEST(FindLoopsTest, CFG1) {
  // Constant is not used.
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::RETURN_VOID);

  ArenaPool arena;
  HGraph* graph = TestCode(data, &arena);
  for (size_t i = 0, e = graph->GetBlocks().Size(); i < e; ++i) {
    ASSERT_EQ(graph->GetBlocks().Get(i)->GetLoopInformation(), nullptr);
  }
}

TEST(FindLoopsTest, CFG2) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::RETURN);

  ArenaPool arena;
  HGraph* graph = TestCode(data, &arena);
  for (size_t i = 0, e = graph->GetBlocks().Size(); i < e; ++i) {
    ASSERT_EQ(graph->GetBlocks().Get(i)->GetLoopInformation(), nullptr);
  }
}

TEST(FindLoopsTest, CFG3) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 3 << 12 | 0,
    Instruction::CONST_4 | 4 << 12 | 1 << 8,
    Instruction::ADD_INT_2ADDR | 1 << 12,
    Instruction::GOTO | 0x100,
    Instruction::RETURN);

  ArenaPool arena;
  HGraph* graph = TestCode(data, &arena);
  for (size_t i = 0, e = graph->GetBlocks().Size(); i < e; ++i) {
    ASSERT_EQ(graph->GetBlocks().Get(i)->GetLoopInformation(), nullptr);
  }
}

TEST(FindLoopsTest, CFG4) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 4,
    Instruction::CONST_4 | 4 << 12 | 0,
    Instruction::GOTO | 0x200,
    Instruction::CONST_4 | 5 << 12 | 0,
    Instruction::RETURN | 0 << 8);

  ArenaPool arena;
  HGraph* graph = TestCode(data, &arena);
  for (size_t i = 0, e = graph->GetBlocks().Size(); i < e; ++i) {
    ASSERT_EQ(graph->GetBlocks().Get(i)->GetLoopInformation(), nullptr);
  }
}

TEST(FindLoopsTest, CFG5) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 3,
    Instruction::CONST_4 | 4 << 12 | 0,
    Instruction::RETURN | 0 << 8);

  ArenaPool arena;
  HGraph* graph = TestCode(data, &arena);
  for (size_t i = 0, e = graph->GetBlocks().Size(); i < e; ++i) {
    ASSERT_EQ(graph->GetBlocks().Get(i)->GetLoopInformation(), nullptr);
  }
}

static void TestBlock(HGraph* graph,
                      int block_id,
                      bool is_loop_header,
                      int parent_loop_header_id,
                      const int* blocks_in_loop = nullptr,
                      size_t number_of_blocks = 0) {
  HBasicBlock* block = graph->GetBlocks().Get(block_id);
  ASSERT_EQ(block->IsLoopHeader(), is_loop_header);
  if (parent_loop_header_id == -1) {
    ASSERT_EQ(block->GetLoopInformation(), nullptr);
  } else {
    ASSERT_EQ(block->GetLoopInformation()->GetHeader()->GetBlockId(), parent_loop_header_id);
  }

  if (blocks_in_loop != nullptr) {
    HLoopInformation* info = block->GetLoopInformation();
    const BitVector& blocks = info->GetBlocks();
    ASSERT_EQ(blocks.NumSetBits(), number_of_blocks);
    for (size_t i = 0; i < number_of_blocks; ++i) {
      ASSERT_TRUE(blocks.IsBitSet(blocks_in_loop[i]));
    }
  } else {
    ASSERT_FALSE(block->IsLoopHeader());
  }
}

TEST(FindLoopsTest, Loop1) {
  // Simple loop with one preheader and one back edge.
  // var a = 0;
  // while (a == a) {
  // }
  // return;
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 3,
    Instruction::GOTO | 0xFE00,
    Instruction::RETURN_VOID);

  ArenaPool arena;
  HGraph* graph = TestCode(data, &arena);

  TestBlock(graph, 0, false, -1);            // entry block
  TestBlock(graph, 1, false, -1);            // pre header
  const int blocks2[] = {2, 3};
  TestBlock(graph, 2, true, 2, blocks2, 2);  // loop header
  TestBlock(graph, 3, false, 2);             // block in loop
  TestBlock(graph, 4, false, -1);            // return block
  TestBlock(graph, 5, false, -1);            // exit block
}

TEST(FindLoopsTest, Loop2) {
  // Make sure we support a preheader of a loop not being the first predecessor
  // in the predecessor list of the header.
  // var a = 0;
  // while (a == a) {
  // }
  // return a;
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::GOTO | 0x400,
    Instruction::IF_EQ, 4,
    Instruction::GOTO | 0xFE00,
    Instruction::GOTO | 0xFD00,
    Instruction::RETURN | 0 << 8);

  ArenaPool arena;
  HGraph* graph = TestCode(data, &arena);

  TestBlock(graph, 0, false, -1);            // entry block
  TestBlock(graph, 1, false, -1);            // goto block
  const int blocks2[] = {2, 3};
  TestBlock(graph, 2, true, 2, blocks2, 2);  // loop header
  TestBlock(graph, 3, false, 2);             // block in loop
  TestBlock(graph, 4, false, -1);            // pre header
  TestBlock(graph, 5, false, -1);            // return block
  TestBlock(graph, 6, false, -1);            // exit block
}

TEST(FindLoopsTest, Loop3) {
  // Make sure we create a preheader of a loop when a header originally has two
  // incoming blocks and one back edge.
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 3,
    Instruction::GOTO | 0x100,
    Instruction::IF_EQ, 3,
    Instruction::GOTO | 0xFE00,
    Instruction::RETURN | 0 << 8);

  ArenaPool arena;
  HGraph* graph = TestCode(data, &arena);

  TestBlock(graph, 0, false, -1);            // entry block
  TestBlock(graph, 1, false, -1);            // goto block
  TestBlock(graph, 2, false, -1);
  const int blocks2[] = {3, 4};
  TestBlock(graph, 3, true, 3, blocks2, 2);  // loop header
  TestBlock(graph, 4, false, 3);             // block in loop
  TestBlock(graph, 5, false, -1);            // pre header
  TestBlock(graph, 6, false, -1);            // return block
  TestBlock(graph, 7, false, -1);            // exit block
  TestBlock(graph, 8, false, -1);            // synthesized pre header
}

TEST(FindLoopsTest, Loop4) {
  // Test loop with originally two back edges.
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 6,
    Instruction::IF_EQ, 3,
    Instruction::GOTO | 0xFC00,
    Instruction::GOTO | 0xFB00,
    Instruction::RETURN | 0 << 8);

  ArenaPool arena;
  HGraph* graph = TestCode(data, &arena);

  TestBlock(graph, 0, false, -1);            // entry block
  TestBlock(graph, 1, false, -1);            // pre header
  const int blocks2[] = {2, 3, 4, 5, 8};
  TestBlock(graph, 2, true, 2, blocks2, 5);  // loop header
  TestBlock(graph, 3, false, 2);             // block in loop
  TestBlock(graph, 4, false, 2);             // original back edge
  TestBlock(graph, 5, false, 2);             // original back edge
  TestBlock(graph, 6, false, -1);            // return block
  TestBlock(graph, 7, false, -1);            // exit block
  TestBlock(graph, 8, false, 2);             // synthesized back edge
}


TEST(FindLoopsTest, Loop5) {
  // Test loop with two exit edges.
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 6,
    Instruction::IF_EQ, 3,
    Instruction::GOTO | 0x0200,
    Instruction::GOTO | 0xFB00,
    Instruction::RETURN | 0 << 8);

  ArenaPool arena;
  HGraph* graph = TestCode(data, &arena);

  TestBlock(graph, 0, false, -1);            // entry block
  TestBlock(graph, 1, false, -1);            // pre header
  const int blocks2[] = {2, 3, 5};
  TestBlock(graph, 2, true, 2, blocks2, 3);  // loop header
  TestBlock(graph, 3, false, 2);             // block in loop
  TestBlock(graph, 4, false, -1);            // loop exit
  TestBlock(graph, 5, false, 2);             // back edge
  TestBlock(graph, 6, false, -1);            // return block
  TestBlock(graph, 7, false, -1);            // exit block
  TestBlock(graph, 8, false, -1);            // synthesized block at the loop exit
}

TEST(FindLoopsTest, InnerLoop) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 6,
    Instruction::IF_EQ, 3,
    Instruction::GOTO | 0xFE00,  // inner loop
    Instruction::GOTO | 0xFB00,
    Instruction::RETURN | 0 << 8);


  ArenaPool arena;
  HGraph* graph = TestCode(data, &arena);

  TestBlock(graph, 0, false, -1);            // entry block
  TestBlock(graph, 1, false, -1);            // pre header of outer loop
  const int blocks2[] = {2, 3, 4, 5, 8};
  TestBlock(graph, 2, true, 2, blocks2, 5);  // outer loop header
  const int blocks3[] = {3, 4};
  TestBlock(graph, 3, true, 3, blocks3, 2);  // inner loop header
  TestBlock(graph, 4, false, 3);             // back edge on inner loop
  TestBlock(graph, 5, false, 2);             // back edge on outer loop
  TestBlock(graph, 6, false, -1);            // return block
  TestBlock(graph, 7, false, -1);            // exit block
  TestBlock(graph, 8, false, 2);             // synthesized block as pre header of inner loop

  ASSERT_TRUE(graph->GetBlocks().Get(3)->GetLoopInformation()->IsIn(
                    *graph->GetBlocks().Get(2)->GetLoopInformation()));
  ASSERT_FALSE(graph->GetBlocks().Get(2)->GetLoopInformation()->IsIn(
                    *graph->GetBlocks().Get(3)->GetLoopInformation()));
}

TEST(FindLoopsTest, TwoLoops) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 3,
    Instruction::GOTO | 0xFE00,  // first loop
    Instruction::IF_EQ, 3,
    Instruction::GOTO | 0xFE00,  // second loop
    Instruction::RETURN | 0 << 8);


  ArenaPool arena;
  HGraph* graph = TestCode(data, &arena);

  TestBlock(graph, 0, false, -1);            // entry block
  TestBlock(graph, 1, false, -1);            // pre header of first loop
  const int blocks2[] = {2, 3};
  TestBlock(graph, 2, true, 2, blocks2, 2);  // first loop header
  TestBlock(graph, 3, false, 2);             // back edge of first loop
  const int blocks4[] = {4, 5};
  TestBlock(graph, 4, true, 4, blocks4, 2);  // second loop header
  TestBlock(graph, 5, false, 4);             // back edge of second loop
  TestBlock(graph, 6, false, -1);            // return block
  TestBlock(graph, 7, false, -1);            // exit block

  ASSERT_FALSE(graph->GetBlocks().Get(4)->GetLoopInformation()->IsIn(
                    *graph->GetBlocks().Get(2)->GetLoopInformation()));
  ASSERT_FALSE(graph->GetBlocks().Get(2)->GetLoopInformation()->IsIn(
                    *graph->GetBlocks().Get(4)->GetLoopInformation()));
}

TEST(FindLoopsTest, NonNaturalLoop) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 3,
    Instruction::GOTO | 0x0100,
    Instruction::IF_EQ, 3,
    Instruction::GOTO | 0xFD00,
    Instruction::RETURN | 0 << 8);

  ArenaPool arena;
  HGraph* graph = TestCode(data, &arena);
  ASSERT_TRUE(graph->GetBlocks().Get(3)->IsLoopHeader());
  HLoopInformation* info = graph->GetBlocks().Get(3)->GetLoopInformation();
  ASSERT_FALSE(info->GetHeader()->Dominates(info->GetBackEdges().Get(0)));
}

TEST(FindLoopsTest, DoWhileLoop) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::GOTO | 0x0100,
    Instruction::IF_EQ, 0xFFFF,
    Instruction::RETURN | 0 << 8);

  ArenaPool arena;
  HGraph* graph = TestCode(data, &arena);

  TestBlock(graph, 0, false, -1);            // entry block
  TestBlock(graph, 1, false, -1);            // pre header of first loop
  const int blocks2[] = {2, 3, 6};
  TestBlock(graph, 2, true, 2, blocks2, 3);  // loop header
  TestBlock(graph, 3, false, 2);             // back edge of first loop
  TestBlock(graph, 4, false, -1);            // return block
  TestBlock(graph, 5, false, -1);            // exit block
  TestBlock(graph, 6, false, 2);             // synthesized block to avoid a critical edge
}

}  // namespace art
