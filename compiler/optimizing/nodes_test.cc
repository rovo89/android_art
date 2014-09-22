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

#include "nodes.h"
#include "utils/arena_allocator.h"

#include "gtest/gtest.h"

namespace art {

/**
 * Test that removing instruction from the graph removes itself from user lists
 * and environment lists.
 */
TEST(Node, RemoveInstruction) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);

  HGraph* graph = new (&allocator) HGraph(&allocator);
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

  HEnvironment* environment = new (&allocator) HEnvironment(&allocator, 1);
  null_check->SetEnvironment(environment);
  environment->SetRawEnvAt(0, parameter);
  parameter->AddEnvUseAt(null_check->GetEnvironment(), 0);

  ASSERT_TRUE(parameter->HasEnvironmentUses());
  ASSERT_TRUE(parameter->HasUses());

  first_block->RemoveInstruction(null_check);

  ASSERT_FALSE(parameter->HasEnvironmentUses());
  ASSERT_FALSE(parameter->HasUses());
}

}  // namespace art
