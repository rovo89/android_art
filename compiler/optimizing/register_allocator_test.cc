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
#include "base/arena_allocator.h"
#include "builder.h"
#include "code_generator.h"
#include "code_generator_x86.h"
#include "dex_file.h"
#include "dex_instruction.h"
#include "driver/compiler_options.h"
#include "nodes.h"
#include "optimizing_unit_test.h"
#include "register_allocator.h"
#include "ssa_liveness_analysis.h"
#include "ssa_phi_elimination.h"

#include "gtest/gtest.h"

namespace art {

// Note: the register allocator tests rely on the fact that constants have live
// intervals and registers get allocated to them.

static bool Check(const uint16_t* data) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraph* graph = CreateGraph(&allocator);
  HGraphBuilder builder(graph);
  const DexFile::CodeItem* item = reinterpret_cast<const DexFile::CodeItem*>(data);
  builder.BuildGraph(*item);
  graph->TryBuildingSsa();
  std::unique_ptr<const X86InstructionSetFeatures> features_x86(
      X86InstructionSetFeatures::FromCppDefines());
  x86::CodeGeneratorX86 codegen(graph, *features_x86.get(), CompilerOptions());
  SsaLivenessAnalysis liveness(graph, &codegen);
  liveness.Analyze();
  RegisterAllocator register_allocator(&allocator, &codegen, liveness);
  register_allocator.AllocateRegisters();
  return register_allocator.Validate(false);
}

/**
 * Unit testing of RegisterAllocator::ValidateIntervals. Register allocator
 * tests are based on this validation method.
 */
TEST(RegisterAllocatorTest, ValidateIntervals) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraph* graph = CreateGraph(&allocator);
  std::unique_ptr<const X86InstructionSetFeatures> features_x86(
      X86InstructionSetFeatures::FromCppDefines());
  x86::CodeGeneratorX86 codegen(graph, *features_x86.get(), CompilerOptions());
  GrowableArray<LiveInterval*> intervals(&allocator, 0);

  // Test with two intervals of the same range.
  {
    static constexpr size_t ranges[][2] = {{0, 42}};
    intervals.Add(BuildInterval(ranges, arraysize(ranges), &allocator, 0));
    intervals.Add(BuildInterval(ranges, arraysize(ranges), &allocator, 1));
    ASSERT_TRUE(RegisterAllocator::ValidateIntervals(
        intervals, 0, 0, codegen, &allocator, true, false));

    intervals.Get(1)->SetRegister(0);
    ASSERT_FALSE(RegisterAllocator::ValidateIntervals(
        intervals, 0, 0, codegen, &allocator, true, false));
    intervals.Reset();
  }

  // Test with two non-intersecting intervals.
  {
    static constexpr size_t ranges1[][2] = {{0, 42}};
    intervals.Add(BuildInterval(ranges1, arraysize(ranges1), &allocator, 0));
    static constexpr size_t ranges2[][2] = {{42, 43}};
    intervals.Add(BuildInterval(ranges2, arraysize(ranges2), &allocator, 1));
    ASSERT_TRUE(RegisterAllocator::ValidateIntervals(
        intervals, 0, 0, codegen, &allocator, true, false));

    intervals.Get(1)->SetRegister(0);
    ASSERT_TRUE(RegisterAllocator::ValidateIntervals(
        intervals, 0, 0, codegen, &allocator, true, false));
    intervals.Reset();
  }

  // Test with two non-intersecting intervals, with one with a lifetime hole.
  {
    static constexpr size_t ranges1[][2] = {{0, 42}, {45, 48}};
    intervals.Add(BuildInterval(ranges1, arraysize(ranges1), &allocator, 0));
    static constexpr size_t ranges2[][2] = {{42, 43}};
    intervals.Add(BuildInterval(ranges2, arraysize(ranges2), &allocator, 1));
    ASSERT_TRUE(RegisterAllocator::ValidateIntervals(
        intervals, 0, 0, codegen, &allocator, true, false));

    intervals.Get(1)->SetRegister(0);
    ASSERT_TRUE(RegisterAllocator::ValidateIntervals(
        intervals, 0, 0, codegen, &allocator, true, false));
    intervals.Reset();
  }

  // Test with intersecting intervals.
  {
    static constexpr size_t ranges1[][2] = {{0, 42}, {44, 48}};
    intervals.Add(BuildInterval(ranges1, arraysize(ranges1), &allocator, 0));
    static constexpr size_t ranges2[][2] = {{42, 47}};
    intervals.Add(BuildInterval(ranges2, arraysize(ranges2), &allocator, 1));
    ASSERT_TRUE(RegisterAllocator::ValidateIntervals(
        intervals, 0, 0, codegen, &allocator, true, false));

    intervals.Get(1)->SetRegister(0);
    ASSERT_FALSE(RegisterAllocator::ValidateIntervals(
        intervals, 0, 0, codegen, &allocator, true, false));
    intervals.Reset();
  }

  // Test with siblings.
  {
    static constexpr size_t ranges1[][2] = {{0, 42}, {44, 48}};
    intervals.Add(BuildInterval(ranges1, arraysize(ranges1), &allocator, 0));
    intervals.Get(0)->SplitAt(43);
    static constexpr size_t ranges2[][2] = {{42, 47}};
    intervals.Add(BuildInterval(ranges2, arraysize(ranges2), &allocator, 1));
    ASSERT_TRUE(RegisterAllocator::ValidateIntervals(
        intervals, 0, 0, codegen, &allocator, true, false));

    intervals.Get(1)->SetRegister(0);
    // Sibling of the first interval has no register allocated to it.
    ASSERT_TRUE(RegisterAllocator::ValidateIntervals(
        intervals, 0, 0, codegen, &allocator, true, false));

    intervals.Get(0)->GetNextSibling()->SetRegister(0);
    ASSERT_FALSE(RegisterAllocator::ValidateIntervals(
        intervals, 0, 0, codegen, &allocator, true, false));
  }
}

TEST(RegisterAllocatorTest, CFG1) {
  /*
   * Test the following snippet:
   *  return 0;
   *
   * Which becomes the following graph:
   *       constant0
   *       goto
   *        |
   *       return
   *        |
   *       exit
   */
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::RETURN);

  ASSERT_TRUE(Check(data));
}

TEST(RegisterAllocatorTest, Loop1) {
  /*
   * Test the following snippet:
   *  int a = 0;
   *  while (a == a) {
   *    a = 4;
   *  }
   *  return 5;
   *
   * Which becomes the following graph:
   *       constant0
   *       constant4
   *       constant5
   *       goto
   *        |
   *       goto
   *        |
   *       phi
   *       equal
   *       if +++++
   *        |       \ +
   *        |     goto
   *        |
   *       return
   *        |
   *       exit
   */

  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 4,
    Instruction::CONST_4 | 4 << 12 | 0,
    Instruction::GOTO | 0xFD00,
    Instruction::CONST_4 | 5 << 12 | 1 << 8,
    Instruction::RETURN | 1 << 8);

  ASSERT_TRUE(Check(data));
}

TEST(RegisterAllocatorTest, Loop2) {
  /*
   * Test the following snippet:
   *  int a = 0;
   *  while (a == 8) {
   *    a = 4 + 5;
   *  }
   *  return 6 + 7;
   *
   * Which becomes the following graph:
   *       constant0
   *       constant4
   *       constant5
   *       constant6
   *       constant7
   *       constant8
   *       goto
   *        |
   *       goto
   *        |
   *       phi
   *       equal
   *       if +++++
   *        |       \ +
   *        |      4 + 5
   *        |      goto
   *        |
   *       6 + 7
   *       return
   *        |
   *       exit
   */

  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::CONST_4 | 8 << 12 | 1 << 8,
    Instruction::IF_EQ | 1 << 8, 7,
    Instruction::CONST_4 | 4 << 12 | 0 << 8,
    Instruction::CONST_4 | 5 << 12 | 1 << 8,
    Instruction::ADD_INT, 1 << 8 | 0,
    Instruction::GOTO | 0xFA00,
    Instruction::CONST_4 | 6 << 12 | 1 << 8,
    Instruction::CONST_4 | 7 << 12 | 1 << 8,
    Instruction::ADD_INT, 1 << 8 | 0,
    Instruction::RETURN | 1 << 8);

  ASSERT_TRUE(Check(data));
}

static HGraph* BuildSSAGraph(const uint16_t* data, ArenaAllocator* allocator) {
  HGraph* graph = CreateGraph(allocator);
  HGraphBuilder builder(graph);
  const DexFile::CodeItem* item = reinterpret_cast<const DexFile::CodeItem*>(data);
  builder.BuildGraph(*item);
  graph->TryBuildingSsa();
  return graph;
}

TEST(RegisterAllocatorTest, Loop3) {
  /*
   * Test the following snippet:
   *  int a = 0
   *  do {
   *    b = a;
   *    a++;
   *  } while (a != 5)
   *  return b;
   *
   * Which becomes the following graph:
   *       constant0
   *       constant1
   *       constant5
   *       goto
   *        |
   *       goto
   *        |++++++++++++
   *       phi          +
   *       a++          +
   *       equals       +
   *       if           +
   *        |++++++++++++
   *       return
   *        |
   *       exit
   */

  const uint16_t data[] = THREE_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::ADD_INT_LIT8 | 1 << 8, 1 << 8,
    Instruction::CONST_4 | 5 << 12 | 2 << 8,
    Instruction::IF_NE | 1 << 8 | 2 << 12, 3,
    Instruction::RETURN | 0 << 8,
    Instruction::MOVE | 1 << 12 | 0 << 8,
    Instruction::GOTO | 0xF900);

  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraph* graph = BuildSSAGraph(data, &allocator);
  std::unique_ptr<const X86InstructionSetFeatures> features_x86(
      X86InstructionSetFeatures::FromCppDefines());
  x86::CodeGeneratorX86 codegen(graph, *features_x86.get(), CompilerOptions());
  SsaLivenessAnalysis liveness(graph, &codegen);
  liveness.Analyze();
  RegisterAllocator register_allocator(&allocator, &codegen, liveness);
  register_allocator.AllocateRegisters();
  ASSERT_TRUE(register_allocator.Validate(false));

  HBasicBlock* loop_header = graph->GetBlocks().Get(2);
  HPhi* phi = loop_header->GetFirstPhi()->AsPhi();

  LiveInterval* phi_interval = phi->GetLiveInterval();
  LiveInterval* loop_update = phi->InputAt(1)->GetLiveInterval();
  ASSERT_TRUE(phi_interval->HasRegister());
  ASSERT_TRUE(loop_update->HasRegister());
  ASSERT_NE(phi_interval->GetRegister(), loop_update->GetRegister());

  HBasicBlock* return_block = graph->GetBlocks().Get(3);
  HReturn* ret = return_block->GetLastInstruction()->AsReturn();
  ASSERT_EQ(phi_interval->GetRegister(), ret->InputAt(0)->GetLiveInterval()->GetRegister());
}

TEST(RegisterAllocatorTest, FirstRegisterUse) {
  const uint16_t data[] = THREE_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::XOR_INT_LIT8 | 1 << 8, 1 << 8,
    Instruction::XOR_INT_LIT8 | 0 << 8, 1 << 8,
    Instruction::XOR_INT_LIT8 | 1 << 8, 1 << 8 | 1,
    Instruction::RETURN_VOID);

  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraph* graph = BuildSSAGraph(data, &allocator);
  std::unique_ptr<const X86InstructionSetFeatures> features_x86(
      X86InstructionSetFeatures::FromCppDefines());
  x86::CodeGeneratorX86 codegen(graph, *features_x86.get(), CompilerOptions());
  SsaLivenessAnalysis liveness(graph, &codegen);
  liveness.Analyze();

  HXor* first_xor = graph->GetBlocks().Get(1)->GetFirstInstruction()->AsXor();
  HXor* last_xor = graph->GetBlocks().Get(1)->GetLastInstruction()->GetPrevious()->AsXor();
  ASSERT_EQ(last_xor->InputAt(0), first_xor);
  LiveInterval* interval = first_xor->GetLiveInterval();
  ASSERT_EQ(interval->GetEnd(), last_xor->GetLifetimePosition());
  ASSERT_TRUE(interval->GetNextSibling() == nullptr);

  // We need a register for the output of the instruction.
  ASSERT_EQ(interval->FirstRegisterUse(), first_xor->GetLifetimePosition());

  // Split at the next instruction.
  interval = interval->SplitAt(first_xor->GetLifetimePosition() + 2);
  // The user of the split is the last add.
  ASSERT_EQ(interval->FirstRegisterUse(), last_xor->GetLifetimePosition());

  // Split before the last add.
  LiveInterval* new_interval = interval->SplitAt(last_xor->GetLifetimePosition() - 1);
  // Ensure the current interval has no register use...
  ASSERT_EQ(interval->FirstRegisterUse(), kNoLifetime);
  // And the new interval has it for the last add.
  ASSERT_EQ(new_interval->FirstRegisterUse(), last_xor->GetLifetimePosition());
}

TEST(RegisterAllocatorTest, DeadPhi) {
  /* Test for a dead loop phi taking as back-edge input a phi that also has
   * this loop phi as input. Walking backwards in SsaDeadPhiElimination
   * does not solve the problem because the loop phi will be visited last.
   *
   * Test the following snippet:
   *  int a = 0
   *  do {
   *    if (true) {
   *      a = 2;
   *    }
   *  } while (true);
   */

  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::CONST_4 | 1 << 8 | 0,
    Instruction::IF_NE | 1 << 8 | 1 << 12, 3,
    Instruction::CONST_4 | 2 << 12 | 0 << 8,
    Instruction::GOTO | 0xFD00,
    Instruction::RETURN_VOID);

  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraph* graph = BuildSSAGraph(data, &allocator);
  SsaDeadPhiElimination(graph).Run();
  std::unique_ptr<const X86InstructionSetFeatures> features_x86(
      X86InstructionSetFeatures::FromCppDefines());
  x86::CodeGeneratorX86 codegen(graph, *features_x86.get(), CompilerOptions());
  SsaLivenessAnalysis liveness(graph, &codegen);
  liveness.Analyze();
  RegisterAllocator register_allocator(&allocator, &codegen, liveness);
  register_allocator.AllocateRegisters();
  ASSERT_TRUE(register_allocator.Validate(false));
}

/**
 * Test that the TryAllocateFreeReg method works in the presence of inactive intervals
 * that share the same register. It should split the interval it is currently
 * allocating for at the minimum lifetime position between the two inactive intervals.
 */
TEST(RegisterAllocatorTest, FreeUntil) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::RETURN);

  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraph* graph = BuildSSAGraph(data, &allocator);
  SsaDeadPhiElimination(graph).Run();
  std::unique_ptr<const X86InstructionSetFeatures> features_x86(
      X86InstructionSetFeatures::FromCppDefines());
  x86::CodeGeneratorX86 codegen(graph, *features_x86.get(), CompilerOptions());
  SsaLivenessAnalysis liveness(graph, &codegen);
  liveness.Analyze();
  RegisterAllocator register_allocator(&allocator, &codegen, liveness);

  // Add an artifical range to cover the temps that will be put in the unhandled list.
  LiveInterval* unhandled = graph->GetEntryBlock()->GetFirstInstruction()->GetLiveInterval();
  unhandled->AddLoopRange(0, 60);
  // For SSA value intervals, only an interval resulted from a split may intersect
  // with inactive intervals.
  unhandled = register_allocator.Split(unhandled, 5);

  // Add three temps holding the same register, and starting at different positions.
  // Put the one that should be picked in the middle of the inactive list to ensure
  // we do not depend on an order.
  LiveInterval* interval = LiveInterval::MakeFixedInterval(&allocator, 0, Primitive::kPrimInt);
  interval->AddRange(40, 50);
  register_allocator.inactive_.Add(interval);

  interval = LiveInterval::MakeFixedInterval(&allocator, 0, Primitive::kPrimInt);
  interval->AddRange(20, 30);
  register_allocator.inactive_.Add(interval);

  interval = LiveInterval::MakeFixedInterval(&allocator, 0, Primitive::kPrimInt);
  interval->AddRange(60, 70);
  register_allocator.inactive_.Add(interval);

  register_allocator.number_of_registers_ = 1;
  register_allocator.registers_array_ = allocator.AllocArray<size_t>(1);
  register_allocator.processing_core_registers_ = true;
  register_allocator.unhandled_ = &register_allocator.unhandled_core_intervals_;

  ASSERT_TRUE(register_allocator.TryAllocateFreeReg(unhandled));

  // Check that we have split the interval.
  ASSERT_EQ(1u, register_allocator.unhandled_->Size());
  // Check that we know need to find a new register where the next interval
  // that uses the register starts.
  ASSERT_EQ(20u, register_allocator.unhandled_->Get(0)->GetStart());
}

static HGraph* BuildIfElseWithPhi(ArenaAllocator* allocator,
                                  HPhi** phi,
                                  HInstruction** input1,
                                  HInstruction** input2) {
  HGraph* graph = CreateGraph(allocator);
  HBasicBlock* entry = new (allocator) HBasicBlock(graph);
  graph->AddBlock(entry);
  graph->SetEntryBlock(entry);
  HInstruction* parameter = new (allocator) HParameterValue(0, Primitive::kPrimNot);
  entry->AddInstruction(parameter);

  HBasicBlock* block = new (allocator) HBasicBlock(graph);
  graph->AddBlock(block);
  entry->AddSuccessor(block);

  HInstruction* test = new (allocator) HInstanceFieldGet(
      parameter, Primitive::kPrimBoolean, MemberOffset(22), false);
  block->AddInstruction(test);
  block->AddInstruction(new (allocator) HIf(test));
  HBasicBlock* then = new (allocator) HBasicBlock(graph);
  HBasicBlock* else_ = new (allocator) HBasicBlock(graph);
  HBasicBlock* join = new (allocator) HBasicBlock(graph);
  graph->AddBlock(then);
  graph->AddBlock(else_);
  graph->AddBlock(join);

  block->AddSuccessor(then);
  block->AddSuccessor(else_);
  then->AddSuccessor(join);
  else_->AddSuccessor(join);
  then->AddInstruction(new (allocator) HGoto());
  else_->AddInstruction(new (allocator) HGoto());

  *phi = new (allocator) HPhi(allocator, 0, 0, Primitive::kPrimInt);
  join->AddPhi(*phi);
  *input1 = new (allocator) HInstanceFieldGet(parameter, Primitive::kPrimInt,
                                              MemberOffset(42), false);
  *input2 = new (allocator) HInstanceFieldGet(parameter, Primitive::kPrimInt,
                                              MemberOffset(42), false);
  then->AddInstruction(*input1);
  else_->AddInstruction(*input2);
  join->AddInstruction(new (allocator) HExit());
  (*phi)->AddInput(*input1);
  (*phi)->AddInput(*input2);

  graph->BuildDominatorTree();
  graph->AnalyzeNaturalLoops();
  return graph;
}

TEST(RegisterAllocatorTest, PhiHint) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HPhi *phi;
  HInstruction *input1, *input2;

  {
    HGraph* graph = BuildIfElseWithPhi(&allocator, &phi, &input1, &input2);
    std::unique_ptr<const X86InstructionSetFeatures> features_x86(
        X86InstructionSetFeatures::FromCppDefines());
    x86::CodeGeneratorX86 codegen(graph, *features_x86.get(), CompilerOptions());
    SsaLivenessAnalysis liveness(graph, &codegen);
    liveness.Analyze();

    // Check that the register allocator is deterministic.
    RegisterAllocator register_allocator(&allocator, &codegen, liveness);
    register_allocator.AllocateRegisters();

    ASSERT_EQ(input1->GetLiveInterval()->GetRegister(), 0);
    ASSERT_EQ(input2->GetLiveInterval()->GetRegister(), 0);
    ASSERT_EQ(phi->GetLiveInterval()->GetRegister(), 0);
  }

  {
    HGraph* graph = BuildIfElseWithPhi(&allocator, &phi, &input1, &input2);
    std::unique_ptr<const X86InstructionSetFeatures> features_x86(
        X86InstructionSetFeatures::FromCppDefines());
    x86::CodeGeneratorX86 codegen(graph, *features_x86.get(), CompilerOptions());
    SsaLivenessAnalysis liveness(graph, &codegen);
    liveness.Analyze();

    // Set the phi to a specific register, and check that the inputs get allocated
    // the same register.
    phi->GetLocations()->UpdateOut(Location::RegisterLocation(2));
    RegisterAllocator register_allocator(&allocator, &codegen, liveness);
    register_allocator.AllocateRegisters();

    ASSERT_EQ(input1->GetLiveInterval()->GetRegister(), 2);
    ASSERT_EQ(input2->GetLiveInterval()->GetRegister(), 2);
    ASSERT_EQ(phi->GetLiveInterval()->GetRegister(), 2);
  }

  {
    HGraph* graph = BuildIfElseWithPhi(&allocator, &phi, &input1, &input2);
    std::unique_ptr<const X86InstructionSetFeatures> features_x86(
        X86InstructionSetFeatures::FromCppDefines());
    x86::CodeGeneratorX86 codegen(graph, *features_x86.get(), CompilerOptions());
    SsaLivenessAnalysis liveness(graph, &codegen);
    liveness.Analyze();

    // Set input1 to a specific register, and check that the phi and other input get allocated
    // the same register.
    input1->GetLocations()->UpdateOut(Location::RegisterLocation(2));
    RegisterAllocator register_allocator(&allocator, &codegen, liveness);
    register_allocator.AllocateRegisters();

    ASSERT_EQ(input1->GetLiveInterval()->GetRegister(), 2);
    ASSERT_EQ(input2->GetLiveInterval()->GetRegister(), 2);
    ASSERT_EQ(phi->GetLiveInterval()->GetRegister(), 2);
  }

  {
    HGraph* graph = BuildIfElseWithPhi(&allocator, &phi, &input1, &input2);
    std::unique_ptr<const X86InstructionSetFeatures> features_x86(
        X86InstructionSetFeatures::FromCppDefines());
    x86::CodeGeneratorX86 codegen(graph, *features_x86.get(), CompilerOptions());
    SsaLivenessAnalysis liveness(graph, &codegen);
    liveness.Analyze();

    // Set input2 to a specific register, and check that the phi and other input get allocated
    // the same register.
    input2->GetLocations()->UpdateOut(Location::RegisterLocation(2));
    RegisterAllocator register_allocator(&allocator, &codegen, liveness);
    register_allocator.AllocateRegisters();

    ASSERT_EQ(input1->GetLiveInterval()->GetRegister(), 2);
    ASSERT_EQ(input2->GetLiveInterval()->GetRegister(), 2);
    ASSERT_EQ(phi->GetLiveInterval()->GetRegister(), 2);
  }
}

static HGraph* BuildFieldReturn(ArenaAllocator* allocator,
                                HInstruction** field,
                                HInstruction** ret) {
  HGraph* graph = CreateGraph(allocator);
  HBasicBlock* entry = new (allocator) HBasicBlock(graph);
  graph->AddBlock(entry);
  graph->SetEntryBlock(entry);
  HInstruction* parameter = new (allocator) HParameterValue(0, Primitive::kPrimNot);
  entry->AddInstruction(parameter);

  HBasicBlock* block = new (allocator) HBasicBlock(graph);
  graph->AddBlock(block);
  entry->AddSuccessor(block);

  *field = new (allocator) HInstanceFieldGet(parameter, Primitive::kPrimInt,
                                             MemberOffset(42), false);
  block->AddInstruction(*field);
  *ret = new (allocator) HReturn(*field);
  block->AddInstruction(*ret);

  HBasicBlock* exit = new (allocator) HBasicBlock(graph);
  graph->AddBlock(exit);
  block->AddSuccessor(exit);
  exit->AddInstruction(new (allocator) HExit());

  graph->BuildDominatorTree();
  return graph;
}

TEST(RegisterAllocatorTest, ExpectedInRegisterHint) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HInstruction *field, *ret;

  {
    HGraph* graph = BuildFieldReturn(&allocator, &field, &ret);
    std::unique_ptr<const X86InstructionSetFeatures> features_x86(
        X86InstructionSetFeatures::FromCppDefines());
    x86::CodeGeneratorX86 codegen(graph, *features_x86.get(), CompilerOptions());
    SsaLivenessAnalysis liveness(graph, &codegen);
    liveness.Analyze();

    RegisterAllocator register_allocator(&allocator, &codegen, liveness);
    register_allocator.AllocateRegisters();

    // Sanity check that in normal conditions, the register should be hinted to 0 (EAX).
    ASSERT_EQ(field->GetLiveInterval()->GetRegister(), 0);
  }

  {
    HGraph* graph = BuildFieldReturn(&allocator, &field, &ret);
    std::unique_ptr<const X86InstructionSetFeatures> features_x86(
        X86InstructionSetFeatures::FromCppDefines());
    x86::CodeGeneratorX86 codegen(graph, *features_x86.get(), CompilerOptions());
    SsaLivenessAnalysis liveness(graph, &codegen);
    liveness.Analyze();

    // Check that the field gets put in the register expected by its use.
    // Don't use SetInAt because we are overriding an already allocated location.
    ret->GetLocations()->inputs_.Put(0, Location::RegisterLocation(2));

    RegisterAllocator register_allocator(&allocator, &codegen, liveness);
    register_allocator.AllocateRegisters();

    ASSERT_EQ(field->GetLiveInterval()->GetRegister(), 2);
  }
}

static HGraph* BuildTwoSubs(ArenaAllocator* allocator,
                            HInstruction** first_sub,
                            HInstruction** second_sub) {
  HGraph* graph = CreateGraph(allocator);
  HBasicBlock* entry = new (allocator) HBasicBlock(graph);
  graph->AddBlock(entry);
  graph->SetEntryBlock(entry);
  HInstruction* parameter = new (allocator) HParameterValue(0, Primitive::kPrimInt);
  entry->AddInstruction(parameter);

  HInstruction* constant1 = graph->GetIntConstant(1);
  HInstruction* constant2 = graph->GetIntConstant(2);

  HBasicBlock* block = new (allocator) HBasicBlock(graph);
  graph->AddBlock(block);
  entry->AddSuccessor(block);

  *first_sub = new (allocator) HSub(Primitive::kPrimInt, parameter, constant1);
  block->AddInstruction(*first_sub);
  *second_sub = new (allocator) HSub(Primitive::kPrimInt, *first_sub, constant2);
  block->AddInstruction(*second_sub);

  block->AddInstruction(new (allocator) HExit());

  graph->BuildDominatorTree();
  return graph;
}

TEST(RegisterAllocatorTest, SameAsFirstInputHint) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HInstruction *first_sub, *second_sub;

  {
    HGraph* graph = BuildTwoSubs(&allocator, &first_sub, &second_sub);
    std::unique_ptr<const X86InstructionSetFeatures> features_x86(
        X86InstructionSetFeatures::FromCppDefines());
    x86::CodeGeneratorX86 codegen(graph, *features_x86.get(), CompilerOptions());
    SsaLivenessAnalysis liveness(graph, &codegen);
    liveness.Analyze();

    RegisterAllocator register_allocator(&allocator, &codegen, liveness);
    register_allocator.AllocateRegisters();

    // Sanity check that in normal conditions, the registers are the same.
    ASSERT_EQ(first_sub->GetLiveInterval()->GetRegister(), 1);
    ASSERT_EQ(second_sub->GetLiveInterval()->GetRegister(), 1);
  }

  {
    HGraph* graph = BuildTwoSubs(&allocator, &first_sub, &second_sub);
    std::unique_ptr<const X86InstructionSetFeatures> features_x86(
        X86InstructionSetFeatures::FromCppDefines());
    x86::CodeGeneratorX86 codegen(graph, *features_x86.get(), CompilerOptions());
    SsaLivenessAnalysis liveness(graph, &codegen);
    liveness.Analyze();

    // check that both adds get the same register.
    // Don't use UpdateOutput because output is already allocated.
    first_sub->InputAt(0)->GetLocations()->output_ = Location::RegisterLocation(2);
    ASSERT_EQ(first_sub->GetLocations()->Out().GetPolicy(), Location::kSameAsFirstInput);
    ASSERT_EQ(second_sub->GetLocations()->Out().GetPolicy(), Location::kSameAsFirstInput);

    RegisterAllocator register_allocator(&allocator, &codegen, liveness);
    register_allocator.AllocateRegisters();

    ASSERT_EQ(first_sub->GetLiveInterval()->GetRegister(), 2);
    ASSERT_EQ(second_sub->GetLiveInterval()->GetRegister(), 2);
  }
}

static HGraph* BuildDiv(ArenaAllocator* allocator,
                        HInstruction** div) {
  HGraph* graph = CreateGraph(allocator);
  HBasicBlock* entry = new (allocator) HBasicBlock(graph);
  graph->AddBlock(entry);
  graph->SetEntryBlock(entry);
  HInstruction* first = new (allocator) HParameterValue(0, Primitive::kPrimInt);
  HInstruction* second = new (allocator) HParameterValue(0, Primitive::kPrimInt);
  entry->AddInstruction(first);
  entry->AddInstruction(second);

  HBasicBlock* block = new (allocator) HBasicBlock(graph);
  graph->AddBlock(block);
  entry->AddSuccessor(block);

  *div = new (allocator) HDiv(Primitive::kPrimInt, first, second, 0);  // don't care about dex_pc.
  block->AddInstruction(*div);

  block->AddInstruction(new (allocator) HExit());

  graph->BuildDominatorTree();
  return graph;
}

TEST(RegisterAllocatorTest, ExpectedExactInRegisterAndSameOutputHint) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HInstruction *div;

  {
    HGraph* graph = BuildDiv(&allocator, &div);
    std::unique_ptr<const X86InstructionSetFeatures> features_x86(
        X86InstructionSetFeatures::FromCppDefines());
    x86::CodeGeneratorX86 codegen(graph, *features_x86.get(), CompilerOptions());
    SsaLivenessAnalysis liveness(graph, &codegen);
    liveness.Analyze();

    RegisterAllocator register_allocator(&allocator, &codegen, liveness);
    register_allocator.AllocateRegisters();

    // div on x86 requires its first input in eax and the output be the same as the first input.
    ASSERT_EQ(div->GetLiveInterval()->GetRegister(), 0);
  }
}

// Test a bug in the register allocator, where allocating a blocked
// register would lead to spilling an inactive interval at the wrong
// position.
TEST(RegisterAllocatorTest, SpillInactive) {
  ArenaPool pool;

  // Create a synthesized graph to please the register_allocator and
  // ssa_liveness_analysis code.
  ArenaAllocator allocator(&pool);
  HGraph* graph = CreateGraph(&allocator);
  HBasicBlock* entry = new (&allocator) HBasicBlock(graph);
  graph->AddBlock(entry);
  graph->SetEntryBlock(entry);
  HInstruction* one = new (&allocator) HParameterValue(0, Primitive::kPrimInt);
  HInstruction* two = new (&allocator) HParameterValue(0, Primitive::kPrimInt);
  HInstruction* three = new (&allocator) HParameterValue(0, Primitive::kPrimInt);
  HInstruction* four = new (&allocator) HParameterValue(0, Primitive::kPrimInt);
  entry->AddInstruction(one);
  entry->AddInstruction(two);
  entry->AddInstruction(three);
  entry->AddInstruction(four);

  HBasicBlock* block = new (&allocator) HBasicBlock(graph);
  graph->AddBlock(block);
  entry->AddSuccessor(block);
  block->AddInstruction(new (&allocator) HExit());

  // We create a synthesized user requesting a register, to avoid just spilling the
  // intervals.
  HPhi* user = new (&allocator) HPhi(&allocator, 0, 1, Primitive::kPrimInt);
  user->AddInput(one);
  user->SetBlock(block);
  LocationSummary* locations = new (&allocator) LocationSummary(user, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  static constexpr size_t phi_ranges[][2] = {{20, 30}};
  BuildInterval(phi_ranges, arraysize(phi_ranges), &allocator, -1, user);

  // Create an interval with lifetime holes.
  static constexpr size_t ranges1[][2] = {{0, 2}, {4, 6}, {8, 10}};
  LiveInterval* first = BuildInterval(ranges1, arraysize(ranges1), &allocator, -1, one);
  first->first_use_ = new(&allocator) UsePosition(user, 0, false, 8, first->first_use_);
  first->first_use_ = new(&allocator) UsePosition(user, 0, false, 7, first->first_use_);
  first->first_use_ = new(&allocator) UsePosition(user, 0, false, 6, first->first_use_);

  locations = new (&allocator) LocationSummary(first->GetDefinedBy(), LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());
  first = first->SplitAt(1);

  // Create an interval that conflicts with the next interval, to force the next
  // interval to call `AllocateBlockedReg`.
  static constexpr size_t ranges2[][2] = {{2, 4}};
  LiveInterval* second = BuildInterval(ranges2, arraysize(ranges2), &allocator, -1, two);
  locations = new (&allocator) LocationSummary(second->GetDefinedBy(), LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());

  // Create an interval that will lead to splitting the first interval. The bug occured
  // by splitting at a wrong position, in this case at the next intersection between
  // this interval and the first interval. We would have then put the interval with ranges
  // "[0, 2(, [4, 6(" in the list of handled intervals, even though we haven't processed intervals
  // before lifetime position 6 yet.
  static constexpr size_t ranges3[][2] = {{2, 4}, {8, 10}};
  LiveInterval* third = BuildInterval(ranges3, arraysize(ranges3), &allocator, -1, three);
  third->first_use_ = new(&allocator) UsePosition(user, 0, false, 8, third->first_use_);
  third->first_use_ = new(&allocator) UsePosition(user, 0, false, 4, third->first_use_);
  third->first_use_ = new(&allocator) UsePosition(user, 0, false, 3, third->first_use_);
  locations = new (&allocator) LocationSummary(third->GetDefinedBy(), LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());
  third = third->SplitAt(3);

  // Because the first part of the split interval was considered handled, this interval
  // was free to allocate the same register, even though it conflicts with it.
  static constexpr size_t ranges4[][2] = {{4, 6}};
  LiveInterval* fourth = BuildInterval(ranges4, arraysize(ranges4), &allocator, -1, four);
  locations = new (&allocator) LocationSummary(fourth->GetDefinedBy(), LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());

  std::unique_ptr<const X86InstructionSetFeatures> features_x86(
      X86InstructionSetFeatures::FromCppDefines());
  x86::CodeGeneratorX86 codegen(graph, *features_x86.get(), CompilerOptions());
  SsaLivenessAnalysis liveness(graph, &codegen);
  // Populate the instructions in the liveness object, to please the register allocator.
  for (size_t i = 0; i < 32; ++i) {
    liveness.instructions_from_lifetime_position_.Add(user);
  }

  RegisterAllocator register_allocator(&allocator, &codegen, liveness);
  register_allocator.unhandled_core_intervals_.Add(fourth);
  register_allocator.unhandled_core_intervals_.Add(third);
  register_allocator.unhandled_core_intervals_.Add(second);
  register_allocator.unhandled_core_intervals_.Add(first);

  // Set just one register available to make all intervals compete for the same.
  register_allocator.number_of_registers_ = 1;
  register_allocator.registers_array_ = allocator.AllocArray<size_t>(1);
  register_allocator.processing_core_registers_ = true;
  register_allocator.unhandled_ = &register_allocator.unhandled_core_intervals_;
  register_allocator.LinearScan();

  // Test that there is no conflicts between intervals.
  GrowableArray<LiveInterval*> intervals(&allocator, 0);
  intervals.Add(first);
  intervals.Add(second);
  intervals.Add(third);
  intervals.Add(fourth);
  ASSERT_TRUE(RegisterAllocator::ValidateIntervals(
      intervals, 0, 0, codegen, &allocator, true, false));
}

}  // namespace art
