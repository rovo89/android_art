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

#include "builder.h"
#include "code_generator_arm.h"
#include "code_generator_arm64.h"
#include "code_generator_x86.h"
#include "code_generator_x86_64.h"
#include "common_compiler_test.h"
#include "dex_file.h"
#include "dex_instruction.h"
#include "instruction_set.h"
#include "nodes.h"
#include "optimizing_unit_test.h"
#include "prepare_for_register_allocation.h"
#include "register_allocator.h"
#include "ssa_liveness_analysis.h"

#include "gtest/gtest.h"

namespace art {

class InternalCodeAllocator : public CodeAllocator {
 public:
  InternalCodeAllocator() { }

  virtual uint8_t* Allocate(size_t size) {
    size_ = size;
    memory_.reset(new uint8_t[size]);
    return memory_.get();
  }

  size_t GetSize() const { return size_; }
  uint8_t* GetMemory() const { return memory_.get(); }

 private:
  size_t size_;
  std::unique_ptr<uint8_t[]> memory_;

  DISALLOW_COPY_AND_ASSIGN(InternalCodeAllocator);
};

static void Run(const InternalCodeAllocator& allocator,
                const CodeGenerator& codegen,
                bool has_result,
                int32_t expected) {
  typedef int32_t (*fptr)();
  CommonCompilerTest::MakeExecutable(allocator.GetMemory(), allocator.GetSize());
  fptr f = reinterpret_cast<fptr>(allocator.GetMemory());
  if (codegen.GetInstructionSet() == kThumb2) {
    // For thumb we need the bottom bit set.
    f = reinterpret_cast<fptr>(reinterpret_cast<uintptr_t>(f) + 1);
  }
  int32_t result = f();
  if (has_result) {
    ASSERT_EQ(result, expected);
  }
}

static void RunCodeBaseline(HGraph* graph, bool has_result, int32_t expected) {
  InternalCodeAllocator allocator;

  x86::CodeGeneratorX86 codegenX86(graph);
  // We avoid doing a stack overflow check that requires the runtime being setup,
  // by making sure the compiler knows the methods we are running are leaf methods.
  codegenX86.CompileBaseline(&allocator, true);
  if (kRuntimeISA == kX86) {
    Run(allocator, codegenX86, has_result, expected);
  }

  arm::CodeGeneratorARM codegenARM(graph);
  codegenARM.CompileBaseline(&allocator, true);
  if (kRuntimeISA == kArm || kRuntimeISA == kThumb2) {
    Run(allocator, codegenARM, has_result, expected);
  }

  x86_64::CodeGeneratorX86_64 codegenX86_64(graph);
  codegenX86_64.CompileBaseline(&allocator, true);
  if (kRuntimeISA == kX86_64) {
    Run(allocator, codegenX86_64, has_result, expected);
  }

  arm64::CodeGeneratorARM64 codegenARM64(graph);
  codegenARM64.CompileBaseline(&allocator, true);
  if (kRuntimeISA == kArm64) {
    Run(allocator, codegenARM64, has_result, expected);
  }
}

static void RunCodeOptimized(CodeGenerator* codegen,
                             HGraph* graph,
                             std::function<void(HGraph*)> hook_before_codegen,
                             bool has_result,
                             int32_t expected) {
  SsaLivenessAnalysis liveness(*graph, codegen);
  liveness.Analyze();

  RegisterAllocator register_allocator(graph->GetArena(), codegen, liveness);
  register_allocator.AllocateRegisters();
  hook_before_codegen(graph);

  InternalCodeAllocator allocator;
  codegen->CompileOptimized(&allocator);
  Run(allocator, *codegen, has_result, expected);
}

static void RunCodeOptimized(HGraph* graph,
                             std::function<void(HGraph*)> hook_before_codegen,
                             bool has_result,
                             int32_t expected) {
  if (kRuntimeISA == kX86) {
    x86::CodeGeneratorX86 codegenX86(graph);
    RunCodeOptimized(&codegenX86, graph, hook_before_codegen, has_result, expected);
  } else if (kRuntimeISA == kArm || kRuntimeISA == kThumb2) {
    arm::CodeGeneratorARM codegenARM(graph);
    RunCodeOptimized(&codegenARM, graph, hook_before_codegen, has_result, expected);
  } else if (kRuntimeISA == kX86_64) {
    x86_64::CodeGeneratorX86_64 codegenX86_64(graph);
    RunCodeOptimized(&codegenX86_64, graph, hook_before_codegen, has_result, expected);
  }
}

static void TestCode(const uint16_t* data, bool has_result = false, int32_t expected = 0) {
  ArenaPool pool;
  ArenaAllocator arena(&pool);
  HGraphBuilder builder(&arena);
  const DexFile::CodeItem* item = reinterpret_cast<const DexFile::CodeItem*>(data);
  HGraph* graph = builder.BuildGraph(*item);
  ASSERT_NE(graph, nullptr);
  // Remove suspend checks, they cannot be executed in this context.
  RemoveSuspendChecks(graph);
  RunCodeBaseline(graph, has_result, expected);
}

TEST(CodegenTest, ReturnVoid) {
  const uint16_t data[] = ZERO_REGISTER_CODE_ITEM(Instruction::RETURN_VOID);
  TestCode(data);
}

TEST(CodegenTest, CFG1) {
  const uint16_t data[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO | 0x100,
    Instruction::RETURN_VOID);

  TestCode(data);
}

TEST(CodegenTest, CFG2) {
  const uint16_t data[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO | 0x100,
    Instruction::GOTO | 0x100,
    Instruction::RETURN_VOID);

  TestCode(data);
}

TEST(CodegenTest, CFG3) {
  const uint16_t data1[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO | 0x200,
    Instruction::RETURN_VOID,
    Instruction::GOTO | 0xFF00);

  TestCode(data1);

  const uint16_t data2[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO_16, 3,
    Instruction::RETURN_VOID,
    Instruction::GOTO_16, 0xFFFF);

  TestCode(data2);

  const uint16_t data3[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO_32, 4, 0,
    Instruction::RETURN_VOID,
    Instruction::GOTO_32, 0xFFFF, 0xFFFF);

  TestCode(data3);
}

TEST(CodegenTest, CFG4) {
  const uint16_t data[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::RETURN_VOID,
    Instruction::GOTO | 0x100,
    Instruction::GOTO | 0xFE00);

  TestCode(data);
}

TEST(CodegenTest, CFG5) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 3,
    Instruction::GOTO | 0x100,
    Instruction::RETURN_VOID);

  TestCode(data);
}

TEST(CodegenTest, IntConstant) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::RETURN_VOID);

  TestCode(data);
}

TEST(CodegenTest, Return1) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::RETURN | 0);

  TestCode(data, true, 0);
}

TEST(CodegenTest, Return2) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::CONST_4 | 0 | 1 << 8,
    Instruction::RETURN | 1 << 8);

  TestCode(data, true, 0);
}

TEST(CodegenTest, Return3) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::CONST_4 | 1 << 8 | 1 << 12,
    Instruction::RETURN | 1 << 8);

  TestCode(data, true, 1);
}

TEST(CodegenTest, ReturnIf1) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::CONST_4 | 1 << 8 | 1 << 12,
    Instruction::IF_EQ, 3,
    Instruction::RETURN | 0 << 8,
    Instruction::RETURN | 1 << 8);

  TestCode(data, true, 1);
}

TEST(CodegenTest, ReturnIf2) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::CONST_4 | 1 << 8 | 1 << 12,
    Instruction::IF_EQ | 0 << 4 | 1 << 8, 3,
    Instruction::RETURN | 0 << 8,
    Instruction::RETURN | 1 << 8);

  TestCode(data, true, 0);
}

// Exercise bit-wise (one's complement) not-int instruction.
#define NOT_INT_TEST(TEST_NAME, INPUT, EXPECTED_OUTPUT) \
TEST(CodegenTest, TEST_NAME) {                          \
  const int32_t input = INPUT;                          \
  const uint16_t input_lo = input & 0x0000FFFF;         \
  const uint16_t input_hi = input >> 16;                \
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(      \
      Instruction::CONST | 0 << 8, input_lo, input_hi,  \
      Instruction::NOT_INT | 1 << 8 | 0 << 12 ,         \
      Instruction::RETURN | 1 << 8);                    \
                                                        \
  TestCode(data, true, EXPECTED_OUTPUT);                \
}

NOT_INT_TEST(ReturnNotIntMinus2, -2, 1)
NOT_INT_TEST(ReturnNotIntMinus1, -1, 0)
NOT_INT_TEST(ReturnNotInt0, 0, -1)
NOT_INT_TEST(ReturnNotInt1, 1, -2)
NOT_INT_TEST(ReturnNotIntINT_MIN, -2147483648, 2147483647)  // (2^31) - 1
NOT_INT_TEST(ReturnNotIntINT_MINPlus1, -2147483647, 2147483646)  // (2^31) - 2
NOT_INT_TEST(ReturnNotIntINT_MAXMinus1, 2147483646, -2147483647)  // -(2^31) - 1
NOT_INT_TEST(ReturnNotIntINT_MAX, 2147483647, -2147483648)  // -(2^31)

#undef NOT_INT_TEST

TEST(CodegenTest, ReturnAdd1) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 3 << 12 | 0,
    Instruction::CONST_4 | 4 << 12 | 1 << 8,
    Instruction::ADD_INT, 1 << 8 | 0,
    Instruction::RETURN);

  TestCode(data, true, 7);
}

TEST(CodegenTest, ReturnAdd2) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 3 << 12 | 0,
    Instruction::CONST_4 | 4 << 12 | 1 << 8,
    Instruction::ADD_INT_2ADDR | 1 << 12,
    Instruction::RETURN);

  TestCode(data, true, 7);
}

TEST(CodegenTest, ReturnAdd3) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 4 << 12 | 0 << 8,
    Instruction::ADD_INT_LIT8, 3 << 8 | 0,
    Instruction::RETURN);

  TestCode(data, true, 7);
}

TEST(CodegenTest, ReturnAdd4) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 4 << 12 | 0 << 8,
    Instruction::ADD_INT_LIT16, 3,
    Instruction::RETURN);

  TestCode(data, true, 7);
}

TEST(CodegenTest, NonMaterializedCondition) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);

  HGraph* graph = new (&allocator) HGraph(&allocator);
  HBasicBlock* entry = new (&allocator) HBasicBlock(graph);
  graph->AddBlock(entry);
  graph->SetEntryBlock(entry);
  entry->AddInstruction(new (&allocator) HGoto());

  HBasicBlock* first_block = new (&allocator) HBasicBlock(graph);
  graph->AddBlock(first_block);
  entry->AddSuccessor(first_block);
  HIntConstant* constant0 = new (&allocator) HIntConstant(0);
  entry->AddInstruction(constant0);
  HIntConstant* constant1 = new (&allocator) HIntConstant(1);
  entry->AddInstruction(constant1);
  HEqual* equal = new (&allocator) HEqual(constant0, constant0);
  first_block->AddInstruction(equal);
  first_block->AddInstruction(new (&allocator) HIf(equal));

  HBasicBlock* then = new (&allocator) HBasicBlock(graph);
  HBasicBlock* else_ = new (&allocator) HBasicBlock(graph);
  HBasicBlock* exit = new (&allocator) HBasicBlock(graph);

  graph->AddBlock(then);
  graph->AddBlock(else_);
  graph->AddBlock(exit);
  first_block->AddSuccessor(then);
  first_block->AddSuccessor(else_);
  then->AddSuccessor(exit);
  else_->AddSuccessor(exit);

  exit->AddInstruction(new (&allocator) HExit());
  then->AddInstruction(new (&allocator) HReturn(constant0));
  else_->AddInstruction(new (&allocator) HReturn(constant1));

  ASSERT_TRUE(equal->NeedsMaterialization());
  graph->BuildDominatorTree();
  PrepareForRegisterAllocation(graph).Run();
  ASSERT_FALSE(equal->NeedsMaterialization());

  auto hook_before_codegen = [](HGraph* graph) {
    HBasicBlock* block = graph->GetEntryBlock()->GetSuccessors().Get(0);
    HParallelMove* move = new (graph->GetArena()) HParallelMove(graph->GetArena());
    block->InsertInstructionBefore(move, block->GetLastInstruction());
  };

  RunCodeOptimized(graph, hook_before_codegen, true, 0);
}

#define MUL_TEST(TYPE, TEST_NAME)                     \
  TEST(CodegenTest, Return ## TEST_NAME) {            \
    const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(  \
      Instruction::CONST_4 | 3 << 12 | 0,             \
      Instruction::CONST_4 | 4 << 12 | 1 << 8,        \
      Instruction::MUL_ ## TYPE, 1 << 8 | 0,          \
      Instruction::RETURN);                           \
                                                      \
    TestCode(data, true, 12);                         \
  }                                                   \
                                                      \
  TEST(CodegenTest, Return ## TEST_NAME ## 2addr) {   \
    const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(  \
      Instruction::CONST_4 | 3 << 12 | 0,             \
      Instruction::CONST_4 | 4 << 12 | 1 << 8,        \
      Instruction::MUL_ ## TYPE ## _2ADDR | 1 << 12,  \
      Instruction::RETURN);                           \
                                                      \
    TestCode(data, true, 12);                         \
  }

#if !defined(__aarch64__)
MUL_TEST(INT, MulInt);
MUL_TEST(LONG, MulLong);
#endif

#if defined(__aarch64__)
TEST(CodegenTest, DISABLED_ReturnMulIntLit8) {
#else
TEST(CodegenTest, ReturnMulIntLit8) {
#endif
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 4 << 12 | 0 << 8,
    Instruction::MUL_INT_LIT8, 3 << 8 | 0,
    Instruction::RETURN);

  TestCode(data, true, 12);
}

#if defined(__aarch64__)
TEST(CodegenTest, DISABLED_ReturnMulIntLit16) {
#else
TEST(CodegenTest, ReturnMulIntLit16) {
#endif
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 4 << 12 | 0 << 8,
    Instruction::MUL_INT_LIT16, 3,
    Instruction::RETURN);

  TestCode(data, true, 12);
}


}  // namespace art
