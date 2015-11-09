/*
 * Copyright (C) 2015 The Android Open Source Project
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
#include "builder.h"
#include "gtest/gtest.h"
#include "licm.h"
#include "nodes.h"
#include "optimizing_unit_test.h"
#include "side_effects_analysis.h"

namespace art {

/**
 * Fixture class for the LICM tests.
 */
class LICMTest : public testing::Test {
 public:
  LICMTest() : pool_(), allocator_(&pool_) {
    graph_ = CreateGraph(&allocator_);
  }

  ~LICMTest() { }

  // Builds a singly-nested loop structure in CFG. Tests can further populate
  // the basic blocks with instructions to set up interesting scenarios.
  void BuildLoop() {
    entry_ = new (&allocator_) HBasicBlock(graph_);
    loop_preheader_ = new (&allocator_) HBasicBlock(graph_);
    loop_header_ = new (&allocator_) HBasicBlock(graph_);
    loop_body_ = new (&allocator_) HBasicBlock(graph_);
    return_ = new (&allocator_) HBasicBlock(graph_);
    exit_ = new (&allocator_) HBasicBlock(graph_);

    graph_->AddBlock(entry_);
    graph_->AddBlock(loop_preheader_);
    graph_->AddBlock(loop_header_);
    graph_->AddBlock(loop_body_);
    graph_->AddBlock(return_);
    graph_->AddBlock(exit_);

    graph_->SetEntryBlock(entry_);
    graph_->SetExitBlock(exit_);

    // Set up loop flow in CFG.
    entry_->AddSuccessor(loop_preheader_);
    loop_preheader_->AddSuccessor(loop_header_);
    loop_header_->AddSuccessor(loop_body_);
    loop_header_->AddSuccessor(return_);
    loop_body_->AddSuccessor(loop_header_);
    return_->AddSuccessor(exit_);

    // Provide boiler-plate instructions.
    parameter_ = new (&allocator_) HParameterValue(graph_->GetDexFile(), 0, 0, Primitive::kPrimNot);
    entry_->AddInstruction(parameter_);
    constant_ = graph_->GetIntConstant(42);
    loop_preheader_->AddInstruction(new (&allocator_) HGoto());
    loop_header_->AddInstruction(new (&allocator_) HIf(parameter_));
    loop_body_->AddInstruction(new (&allocator_) HGoto());
    exit_->AddInstruction(new (&allocator_) HExit());
  }

  // Performs LICM optimizations (after proper set up).
  void PerformLICM() {
    ASSERT_TRUE(graph_->TryBuildingSsa());
    SideEffectsAnalysis side_effects(graph_);
    side_effects.Run();
    LICM licm(graph_, side_effects);
    licm.Run();
  }

  // General building fields.
  ArenaPool pool_;
  ArenaAllocator allocator_;
  HGraph* graph_;

  // Specific basic blocks.
  HBasicBlock* entry_;
  HBasicBlock* loop_preheader_;
  HBasicBlock* loop_header_;
  HBasicBlock* loop_body_;
  HBasicBlock* return_;
  HBasicBlock* exit_;

  HInstruction* parameter_;  // "this"
  HInstruction* constant_;
};

//
// The actual LICM tests.
//

TEST_F(LICMTest, FieldHoisting) {
  BuildLoop();

  // Populate the loop with instructions: set/get field with different types.
  NullHandle<mirror::DexCache> dex_cache;
  HInstruction* get_field = new (&allocator_) HInstanceFieldGet(parameter_,
                                                                Primitive::kPrimLong,
                                                                MemberOffset(10),
                                                                false,
                                                                kUnknownFieldIndex,
                                                                kUnknownClassDefIndex,
                                                                graph_->GetDexFile(),
                                                                dex_cache,
                                                                0);
  loop_body_->InsertInstructionBefore(get_field, loop_body_->GetLastInstruction());
  HInstruction* set_field = new (&allocator_) HInstanceFieldSet(
      parameter_, constant_, Primitive::kPrimInt, MemberOffset(20),
      false, kUnknownFieldIndex, kUnknownClassDefIndex, graph_->GetDexFile(), dex_cache, 0);
  loop_body_->InsertInstructionBefore(set_field, loop_body_->GetLastInstruction());

  EXPECT_EQ(get_field->GetBlock(), loop_body_);
  EXPECT_EQ(set_field->GetBlock(), loop_body_);
  PerformLICM();
  EXPECT_EQ(get_field->GetBlock(), loop_preheader_);
  EXPECT_EQ(set_field->GetBlock(), loop_body_);
}

TEST_F(LICMTest, NoFieldHoisting) {
  BuildLoop();

  // Populate the loop with instructions: set/get field with same types.
  NullHandle<mirror::DexCache> dex_cache;
  HInstruction* get_field = new (&allocator_) HInstanceFieldGet(parameter_,
                                                                Primitive::kPrimLong,
                                                                MemberOffset(10),
                                                                false,
                                                                kUnknownFieldIndex,
                                                                kUnknownClassDefIndex,
                                                                graph_->GetDexFile(),
                                                                dex_cache,
                                                                0);
  loop_body_->InsertInstructionBefore(get_field, loop_body_->GetLastInstruction());
  HInstruction* set_field = new (&allocator_) HInstanceFieldSet(parameter_,
                                                                get_field,
                                                                Primitive::kPrimLong,
                                                                MemberOffset(10),
                                                                false,
                                                                kUnknownFieldIndex,
                                                                kUnknownClassDefIndex,
                                                                graph_->GetDexFile(),
                                                                dex_cache,
                                                                0);
  loop_body_->InsertInstructionBefore(set_field, loop_body_->GetLastInstruction());

  EXPECT_EQ(get_field->GetBlock(), loop_body_);
  EXPECT_EQ(set_field->GetBlock(), loop_body_);
  PerformLICM();
  EXPECT_EQ(get_field->GetBlock(), loop_body_);
  EXPECT_EQ(set_field->GetBlock(), loop_body_);
}

TEST_F(LICMTest, ArrayHoisting) {
  BuildLoop();

  // Populate the loop with instructions: set/get array with different types.
  HInstruction* get_array = new (&allocator_) HArrayGet(
      parameter_, constant_, Primitive::kPrimLong, 0);
  loop_body_->InsertInstructionBefore(get_array, loop_body_->GetLastInstruction());
  HInstruction* set_array = new (&allocator_) HArraySet(
      parameter_, constant_, constant_, Primitive::kPrimInt, 0);
  loop_body_->InsertInstructionBefore(set_array, loop_body_->GetLastInstruction());

  EXPECT_EQ(get_array->GetBlock(), loop_body_);
  EXPECT_EQ(set_array->GetBlock(), loop_body_);
  PerformLICM();
  EXPECT_EQ(get_array->GetBlock(), loop_preheader_);
  EXPECT_EQ(set_array->GetBlock(), loop_body_);
}

TEST_F(LICMTest, NoArrayHoisting) {
  BuildLoop();

  // Populate the loop with instructions: set/get array with same types.
  HInstruction* get_array = new (&allocator_) HArrayGet(
      parameter_, constant_, Primitive::kPrimLong, 0);
  loop_body_->InsertInstructionBefore(get_array, loop_body_->GetLastInstruction());
  HInstruction* set_array = new (&allocator_) HArraySet(
      parameter_, get_array, constant_, Primitive::kPrimLong, 0);
  loop_body_->InsertInstructionBefore(set_array, loop_body_->GetLastInstruction());

  EXPECT_EQ(get_array->GetBlock(), loop_body_);
  EXPECT_EQ(set_array->GetBlock(), loop_body_);
  PerformLICM();
  EXPECT_EQ(get_array->GetBlock(), loop_body_);
  EXPECT_EQ(set_array->GetBlock(), loop_body_);
}

}  // namespace art
