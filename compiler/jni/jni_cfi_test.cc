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
#include "jni/quick/calling_convention.h"
#include "utils/assembler.h"

#include "jni/jni_cfi_test_expected.inc"

namespace art {

// Run the tests only on host.
#ifndef HAVE_ANDROID_OS

class JNICFITest : public CFITest {
 public:
  // Enable this flag to generate the expected outputs.
  static constexpr bool kGenerateExpected = false;

  void TestImpl(InstructionSet isa, const char* isa_str,
                const std::vector<uint8_t>& expected_asm,
                const std::vector<uint8_t>& expected_cfi) {
    // Description of simple method.
    const bool is_static = true;
    const bool is_synchronized = false;
    const char* shorty = "IIFII";
    std::unique_ptr<JniCallingConvention> jni_conv(
        JniCallingConvention::Create(is_static, is_synchronized, shorty, isa));
    std::unique_ptr<ManagedRuntimeCallingConvention> mr_conv(
        ManagedRuntimeCallingConvention::Create(is_static, is_synchronized, shorty, isa));
    const int frame_size(jni_conv->FrameSize());
    const std::vector<ManagedRegister>& callee_save_regs = jni_conv->CalleeSaveRegisters();

    // Assemble the method.
    std::unique_ptr<Assembler> jni_asm(Assembler::Create(isa));
    jni_asm->BuildFrame(frame_size, mr_conv->MethodRegister(),
                        callee_save_regs, mr_conv->EntrySpills());
    jni_asm->IncreaseFrameSize(32);
    jni_asm->DecreaseFrameSize(32);
    jni_asm->RemoveFrame(frame_size, callee_save_regs);
    jni_asm->EmitSlowPaths();
    std::vector<uint8_t> actual_asm(jni_asm->CodeSize());
    MemoryRegion code(&actual_asm[0], actual_asm.size());
    jni_asm->FinalizeInstructions(code);
    ASSERT_EQ(jni_asm->cfi().GetCurrentCFAOffset(), frame_size);
    const std::vector<uint8_t>& actual_cfi = *(jni_asm->cfi().data());

    if (kGenerateExpected) {
      GenerateExpected(stdout, isa, isa_str, actual_asm, actual_cfi);
    } else {
      EXPECT_EQ(expected_asm, actual_asm);
      EXPECT_EQ(expected_cfi, actual_cfi);
    }
  }
};

#define TEST_ISA(isa) \
  TEST_F(JNICFITest, isa) { \
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
