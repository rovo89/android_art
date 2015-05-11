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

#include "base/arena_allocator.h"
#include "nodes.h"
#include "optimizing_unit_test.h"

#include "gtest/gtest.h"

namespace art {

/**
 * Test that removing instruction from the graph removes itself from user lists
 * and environment lists.
 */
TEST(Node, RemoveInstruction) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);

  HGraph* graph = CreateGraph(&allocator);
  HBasicBlock* entry = new (&allocator) HBasicBlock(graph);
  graph->AddBlock(entry);
  graph->SetEntryBlock(entry);
  HInstruction* parameter = new (&allocator) HParameterValue(0, Primitive::kPrimNot);
  entry->AddInstruction(parameter);
  entry->AddInstruction(new (&allocator) HGoto());

  HBasicBlock* first_block = new (&allocator) HBasicBlock(graph);
  graph->AddBlock(first_block);
  entry->AddSuccessor(first_block);
  HInstruction* null_check = new (&allocator) HNullCheck(parameter, 0);
  first_block->AddInstruction(null_check);
  first_block->AddInstruction(new (&allocator) HReturnVoid());

  HBasicBlock* exit_block = new (&allocator) HBasicBlock(graph);
  graph->AddBlock(exit_block);
  first_block->AddSuccessor(exit_block);
  exit_block->AddInstruction(new (&allocator) HExit());

  HEnvironment* environment = new (&allocator) HEnvironment(
      &allocator, 1, graph->GetDexFile(), graph->GetMethodIdx(), 0);
  null_check->SetRawEnvironment(environment);
  environment->SetRawEnvAt(0, parameter);
  parameter->AddEnvUseAt(null_check->GetEnvironment(), 0);

  ASSERT_TRUE(parameter->HasEnvironmentUses());
  ASSERT_TRUE(parameter->HasUses());

  first_block->RemoveInstruction(null_check);

  ASSERT_FALSE(parameter->HasEnvironmentUses());
  ASSERT_FALSE(parameter->HasUses());
}

/**
 * Test that inserting an instruction in the graph updates user lists.
 */
TEST(Node, InsertInstruction) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);

  HGraph* graph = CreateGraph(&allocator);
  HBasicBlock* entry = new (&allocator) HBasicBlock(graph);
  graph->AddBlock(entry);
  graph->SetEntryBlock(entry);
  HInstruction* parameter1 = new (&allocator) HParameterValue(0, Primitive::kPrimNot);
  HInstruction* parameter2 = new (&allocator) HParameterValue(0, Primitive::kPrimNot);
  entry->AddInstruction(parameter1);
  entry->AddInstruction(parameter2);
  entry->AddInstruction(new (&allocator) HExit());

  ASSERT_FALSE(parameter1->HasUses());

  HInstruction* to_insert = new (&allocator) HNullCheck(parameter1, 0);
  entry->InsertInstructionBefore(to_insert, parameter2);

  ASSERT_TRUE(parameter1->HasUses());
  ASSERT_TRUE(parameter1->GetUses().HasOnlyOneUse());
}

/**
 * Test that adding an instruction in the graph updates user lists.
 */
TEST(Node, AddInstruction) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);

  HGraph* graph = CreateGraph(&allocator);
  HBasicBlock* entry = new (&allocator) HBasicBlock(graph);
  graph->AddBlock(entry);
  graph->SetEntryBlock(entry);
  HInstruction* parameter = new (&allocator) HParameterValue(0, Primitive::kPrimNot);
  entry->AddInstruction(parameter);

  ASSERT_FALSE(parameter->HasUses());

  HInstruction* to_add = new (&allocator) HNullCheck(parameter, 0);
  entry->AddInstruction(to_add);

  ASSERT_TRUE(parameter->HasUses());
  ASSERT_TRUE(parameter->GetUses().HasOnlyOneUse());
}

TEST(Node, ParentEnvironment) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);

  HGraph* graph = CreateGraph(&allocator);
  HBasicBlock* entry = new (&allocator) HBasicBlock(graph);
  graph->AddBlock(entry);
  graph->SetEntryBlock(entry);
  HInstruction* parameter1 = new (&allocator) HParameterValue(0, Primitive::kPrimNot);
  HInstruction* with_environment = new (&allocator) HNullCheck(parameter1, 0);
  entry->AddInstruction(parameter1);
  entry->AddInstruction(with_environment);
  entry->AddInstruction(new (&allocator) HExit());

  ASSERT_TRUE(parameter1->HasUses());
  ASSERT_TRUE(parameter1->GetUses().HasOnlyOneUse());

  HEnvironment* environment = new (&allocator) HEnvironment(
      &allocator, 1, graph->GetDexFile(), graph->GetMethodIdx(), 0);
  GrowableArray<HInstruction*> array(&allocator, 1);
  array.Add(parameter1);

  environment->CopyFrom(array);
  with_environment->SetRawEnvironment(environment);

  ASSERT_TRUE(parameter1->HasEnvironmentUses());
  ASSERT_TRUE(parameter1->GetEnvUses().HasOnlyOneUse());

  HEnvironment* parent1 = new (&allocator) HEnvironment(
      &allocator, 1, graph->GetDexFile(), graph->GetMethodIdx(), 0);
  parent1->CopyFrom(array);

  ASSERT_EQ(parameter1->GetEnvUses().SizeSlow(), 2u);

  HEnvironment* parent2 = new (&allocator) HEnvironment(
      &allocator, 1, graph->GetDexFile(), graph->GetMethodIdx(), 0);
  parent2->CopyFrom(array);
  parent1->SetAndCopyParentChain(&allocator, parent2);

  // One use for parent2, and one other use for the new parent of parent1.
  ASSERT_EQ(parameter1->GetEnvUses().SizeSlow(), 4u);

  // We have copied the parent chain. So we now have two more uses.
  environment->SetAndCopyParentChain(&allocator, parent1);
  ASSERT_EQ(parameter1->GetEnvUses().SizeSlow(), 6u);
}

}  // namespace art
