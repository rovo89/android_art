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

#include <memory>
#include <vector>

#include "arch/instruction_set.h"
#include "cfi_test.h"
#include "gtest/gtest.h"
#include "optimizing/code_generator.h"
#include "optimizing/optimizing_unit_test.h"
#include "utils/assembler.h"

#include "optimizing/optimizing_cfi_test_expected.inc"

namespace art {

// Run the tests only on host.
#ifndef HAVE_ANDROID_OS

class OptimizingCFITest : public CFITest {
 public:
  // Enable this flag to generate the expected outputs.
  static constexpr bool kGenerateExpected = false;

  void TestImpl(InstructionSet isa, const char* isa_str,
                const std::vector<uint8_t>& expected_asm,
                const std::vector<uint8_t>& expected_cfi) {
    // Setup simple context.
    ArenaPool pool;
    ArenaAllocator allocator(&pool);
    CompilerOptions opts;
    std::unique_ptr<const InstructionSetFeatures> isa_features;
    std::string error;
    isa_features.reset(InstructionSetFeatures::FromVariant(isa, "default", &error));
    HGraph* graph = CreateGraph(&allocator);
    // Generate simple frame with some spills.
    std::unique_ptr<CodeGenerator> code_gen(
        CodeGenerator::Create(graph, isa, *isa_features.get(), opts));
    const int frame_size = 64;
    int core_reg = 0;
    int fp_reg = 0;
    for (int i = 0; i < 2; i++) {  // Two registers of each kind.
      for (; core_reg < 32; core_reg++) {
        if (code_gen->IsCoreCalleeSaveRegister(core_reg)) {
          auto location = Location::RegisterLocation(core_reg);
          code_gen->AddAllocatedRegister(location);
          core_reg++;
          break;
        }
      }
      for (; fp_reg < 32; fp_reg++) {
        if (code_gen->IsFloatingPointCalleeSaveRegister(fp_reg)) {
          auto location = Location::FpuRegisterLocation(fp_reg);
          code_gen->AddAllocatedRegister(location);
          fp_reg++;
          break;
        }
      }
    }
    code_gen->ComputeSpillMask();
    code_gen->SetFrameSize(frame_size);
    code_gen->GenerateFrameEntry();
    code_gen->GenerateFrameExit();
    // Get the outputs.
    InternalCodeAllocator code_allocator;
    code_gen->Finalize(&code_allocator);
    const std::vector<uint8_t>& actual_asm = code_allocator.GetMemory();
    Assembler* opt_asm = code_gen->GetAssembler();
    const std::vector<uint8_t>& actual_cfi = *(opt_asm->cfi().data());

    if (kGenerateExpected) {
      GenerateExpected(stdout, isa, isa_str, actual_asm, actual_cfi);
    } else {
      EXPECT_EQ(expected_asm, actual_asm);
      EXPECT_EQ(expected_cfi, actual_cfi);
    }
  }

 private:
  class InternalCodeAllocator : public CodeAllocator {
   public:
    InternalCodeAllocator() {}

    virtual uint8_t* Allocate(size_t size) {
      memory_.resize(size);
      return memory_.data();
    }

    const std::vector<uint8_t>& GetMemory() { return memory_; }

   private:
    std::vector<uint8_t> memory_;

    DISALLOW_COPY_AND_ASSIGN(InternalCodeAllocator);
  };
};

#define TEST_ISA(isa) \
  TEST_F(OptimizingCFITest, isa) { \
    std::vector<uint8_t> expected_asm(expected_asm_##isa, \
        expected_asm_##isa + arraysize(expected_asm_##isa)); \
    std::vector<uint8_t> expected_cfi(expected_cfi_##isa, \
        expected_cfi_##isa + arraysize(expected_cfi_##isa)); \
    TestImpl(isa, #isa, expected_asm, expected_cfi); \
  }

TEST_ISA(kThumb2)
TEST_ISA(kArm64)
TEST_ISA(kX86)
TEST_ISA(kX86_64)

#endif  // HAVE_ANDROID_OS

}  // namespace art
