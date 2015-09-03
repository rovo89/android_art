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
#include "dex_instruction.h"
#include "nodes.h"
#include "optimizing_unit_test.h"

#include "gtest/gtest.h"

namespace art {

/**
 * Check that the HGraphBuilder adds suspend checks to backward branches.
 */

static void TestCode(const uint16_t* data) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraph* graph = CreateGraph(&allocator);
  HGraphBuilder builder(graph);
  const DexFile::CodeItem* item = reinterpret_cast<const DexFile::CodeItem*>(data);
  bool graph_built = builder.BuildGraph(*item);
  ASSERT_TRUE(graph_built);

  HBasicBlock* first_block = graph->GetEntryBlock()->GetSuccessors().Get(0);
  HInstruction* first_instruction = first_block->GetFirstInstruction();
  // Account for some tests having a store local as first instruction.
  ASSERT_TRUE(first_instruction->IsSuspendCheck()
              || first_instruction->GetNext()->IsSuspendCheck());
}

TEST(CodegenTest, CFG1) {
  const uint16_t data[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::NOP,
    Instruction::GOTO | 0xFF00);

  TestCode(data);
}

TEST(CodegenTest, CFG2) {
  const uint16_t data[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO_32, 0, 0);

  TestCode(data);
}

TEST(CodegenTest, CFG3) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 0xFFFF,
    Instruction::RETURN_VOID);

  TestCode(data);
}

TEST(CodegenTest, CFG4) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_NE, 0xFFFF,
    Instruction::RETURN_VOID);

  TestCode(data);
}

TEST(CodegenTest, CFG5) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQZ, 0xFFFF,
    Instruction::RETURN_VOID);

  TestCode(data);
}

TEST(CodegenTest, CFG6) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_NEZ, 0xFFFF,
    Instruction::RETURN_VOID);

  TestCode(data);
}
}  // namespace art
