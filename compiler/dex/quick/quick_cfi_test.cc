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

#include <vector>
#include <memory>

#include "arch/instruction_set.h"
#include "arch/instruction_set_features.h"
#include "cfi_test.h"
#include "dex/compiler_ir.h"
#include "dex/mir_graph.h"
#include "dex/pass_manager.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "dex/quick/quick_compiler.h"
#include "dex/quick/mir_to_lir.h"
#include "dex/verification_results.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "gtest/gtest.h"

#include "dex/quick/quick_cfi_test_expected.inc"

namespace art {

// Run the tests only on host.
#ifndef HAVE_ANDROID_OS

class QuickCFITest : public CFITest {
 public:
  // Enable this flag to generate the expected outputs.
  static constexpr bool kGenerateExpected = false;

  void TestImpl(InstructionSet isa, const char* isa_str,
                const std::vector<uint8_t>& expected_asm,
                const std::vector<uint8_t>& expected_cfi) {
    // Setup simple compiler context.
    ArenaPool pool;
    ArenaAllocator arena(&pool);
    CompilerOptions compiler_options(
      CompilerOptions::kDefaultCompilerFilter,
      CompilerOptions::kDefaultHugeMethodThreshold,
      CompilerOptions::kDefaultLargeMethodThreshold,
      CompilerOptions::kDefaultSmallMethodThreshold,
      CompilerOptions::kDefaultTinyMethodThreshold,
      CompilerOptions::kDefaultNumDexMethodsThreshold,
      CompilerOptions::kDefaultInlineDepthLimit,
      CompilerOptions::kDefaultInlineMaxCodeUnits,
      false,
      CompilerOptions::kDefaultTopKProfileThreshold,
      false,
      true,  // generate_debug_info.
      false,
      false,
      false,
      false,
      nullptr,
      new PassManagerOptions(),
      nullptr,
      false);
    VerificationResults verification_results(&compiler_options);
    DexFileToMethodInlinerMap method_inliner_map;
    std::unique_ptr<const InstructionSetFeatures> isa_features;
    std::string error;
    isa_features.reset(InstructionSetFeatures::FromVariant(isa, "default", &error));
    CompilerDriver driver(&compiler_options, &verification_results, &method_inliner_map,
                          Compiler::kQuick, isa, isa_features.get(),
                          false, nullptr, nullptr, nullptr, 0, false, false, "", 0, -1, "");
    ClassLinker* linker = nullptr;
    CompilationUnit cu(&pool, isa, &driver, linker);
    DexFile::CodeItem code_item { 0, 0, 0, 0, 0, 0, { 0 } };  // NOLINT
    cu.mir_graph.reset(new MIRGraph(&cu, &arena));
    cu.mir_graph->current_code_item_ = &code_item;

    // Generate empty method with some spills.
    std::unique_ptr<Mir2Lir> m2l(QuickCompiler::GetCodeGenerator(&cu, nullptr));
    m2l->frame_size_ = 64u;
    m2l->CompilerInitializeRegAlloc();
    for (const auto& info : m2l->reg_pool_->core_regs_) {
      if (m2l->num_core_spills_ < 2 && !info->IsTemp() && !info->InUse()) {
        m2l->core_spill_mask_ |= 1 << info->GetReg().GetRegNum();
        m2l->num_core_spills_++;
      }
    }
    for (const auto& info : m2l->reg_pool_->sp_regs_) {
      if (m2l->num_fp_spills_ < 2 && !info->IsTemp() && !info->InUse()) {
        m2l->fp_spill_mask_ |= 1 << info->GetReg().GetRegNum();
        m2l->num_fp_spills_++;
      }
    }
    m2l->AdjustSpillMask();
    m2l->GenEntrySequence(nullptr, m2l->GetCompilationUnit()->target64 ?
        m2l->LocCReturnWide() : m2l->LocCReturnRef());
    m2l->GenExitSequence();
    m2l->HandleSlowPaths();
    m2l->AssembleLIR();
    std::vector<uint8_t> actual_asm(m2l->code_buffer_.begin(), m2l->code_buffer_.end());
    auto const& cfi_data = m2l->cfi().Patch(actual_asm.size());
    std::vector<uint8_t> actual_cfi(cfi_data->begin(), cfi_data->end());
    EXPECT_EQ(m2l->cfi().GetCurrentPC(), static_cast<int>(actual_asm.size()));

    if (kGenerateExpected) {
      GenerateExpected(stdout, isa, isa_str, actual_asm, actual_cfi);
    } else {
      EXPECT_EQ(expected_asm, actual_asm);
      EXPECT_EQ(expected_cfi, actual_cfi);
    }
  }
};

#define TEST_ISA(isa) \
  TEST_F(QuickCFITest, isa) { \
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
TEST_ISA(kMips)
TEST_ISA(kMips64)

#endif  // HAVE_ANDROID_OS

}  // namespace art
