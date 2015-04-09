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

#include "assembler_thumb2.h"

#include "base/stl_util.h"
#include "utils/assembler_test.h"

namespace art {

class AssemblerThumb2Test : public AssemblerTest<arm::Thumb2Assembler,
                                                 arm::Register, arm::SRegister,
                                                 uint32_t> {
 protected:
  std::string GetArchitectureString() OVERRIDE {
    return "arm";
  }

  std::string GetAssemblerParameters() OVERRIDE {
    return " -march=armv7-a -mcpu=cortex-a15 -mfpu=neon -mthumb";
  }

  const char* GetAssemblyHeader() OVERRIDE {
    return kThumb2AssemblyHeader;
  }

  std::string GetDisassembleParameters() OVERRIDE {
    return " -D -bbinary -marm --disassembler-options=force-thumb --no-show-raw-insn";
  }

  void SetUpHelpers() OVERRIDE {
    if (registers_.size() == 0) {
      registers_.insert(end(registers_),
                        {  // NOLINT(whitespace/braces)
                          new arm::Register(arm::R0),
                          new arm::Register(arm::R1),
                          new arm::Register(arm::R2),
                          new arm::Register(arm::R3),
                          new arm::Register(arm::R4),
                          new arm::Register(arm::R5),
                          new arm::Register(arm::R6),
                          new arm::Register(arm::R7),
                          new arm::Register(arm::R8),
                          new arm::Register(arm::R9),
                          new arm::Register(arm::R10),
                          new arm::Register(arm::R11),
                          new arm::Register(arm::R12),
                          new arm::Register(arm::R13),
                          new arm::Register(arm::R14),
                          new arm::Register(arm::R15)
                        });
    }
  }

  void TearDown() OVERRIDE {
    AssemblerTest::TearDown();
    STLDeleteElements(&registers_);
  }

  std::vector<arm::Register*> GetRegisters() OVERRIDE {
    return registers_;
  }

  uint32_t CreateImmediate(int64_t imm_value) OVERRIDE {
    return imm_value;
  }

 private:
  std::vector<arm::Register*> registers_;

  static constexpr const char* kThumb2AssemblyHeader = ".syntax unified\n.thumb\n";
};


TEST_F(AssemblerThumb2Test, Toolchain) {
  EXPECT_TRUE(CheckTools());
}


TEST_F(AssemblerThumb2Test, Sbfx) {
  GetAssembler()->sbfx(arm::R0, arm::R1, 0, 1);
  GetAssembler()->sbfx(arm::R0, arm::R1, 0, 8);
  GetAssembler()->sbfx(arm::R0, arm::R1, 0, 16);
  GetAssembler()->sbfx(arm::R0, arm::R1, 0, 32);

  GetAssembler()->sbfx(arm::R0, arm::R1, 8, 1);
  GetAssembler()->sbfx(arm::R0, arm::R1, 8, 8);
  GetAssembler()->sbfx(arm::R0, arm::R1, 8, 16);
  GetAssembler()->sbfx(arm::R0, arm::R1, 8, 24);

  GetAssembler()->sbfx(arm::R0, arm::R1, 16, 1);
  GetAssembler()->sbfx(arm::R0, arm::R1, 16, 8);
  GetAssembler()->sbfx(arm::R0, arm::R1, 16, 16);

  GetAssembler()->sbfx(arm::R0, arm::R1, 31, 1);

  const char* expected =
      "sbfx r0, r1, #0, #1\n"
      "sbfx r0, r1, #0, #8\n"
      "sbfx r0, r1, #0, #16\n"
      "sbfx r0, r1, #0, #32\n"

      "sbfx r0, r1, #8, #1\n"
      "sbfx r0, r1, #8, #8\n"
      "sbfx r0, r1, #8, #16\n"
      "sbfx r0, r1, #8, #24\n"

      "sbfx r0, r1, #16, #1\n"
      "sbfx r0, r1, #16, #8\n"
      "sbfx r0, r1, #16, #16\n"

      "sbfx r0, r1, #31, #1\n";
  DriverStr(expected, "sbfx");
}

TEST_F(AssemblerThumb2Test, Ubfx) {
  GetAssembler()->ubfx(arm::R0, arm::R1, 0, 1);
  GetAssembler()->ubfx(arm::R0, arm::R1, 0, 8);
  GetAssembler()->ubfx(arm::R0, arm::R1, 0, 16);
  GetAssembler()->ubfx(arm::R0, arm::R1, 0, 32);

  GetAssembler()->ubfx(arm::R0, arm::R1, 8, 1);
  GetAssembler()->ubfx(arm::R0, arm::R1, 8, 8);
  GetAssembler()->ubfx(arm::R0, arm::R1, 8, 16);
  GetAssembler()->ubfx(arm::R0, arm::R1, 8, 24);

  GetAssembler()->ubfx(arm::R0, arm::R1, 16, 1);
  GetAssembler()->ubfx(arm::R0, arm::R1, 16, 8);
  GetAssembler()->ubfx(arm::R0, arm::R1, 16, 16);

  GetAssembler()->ubfx(arm::R0, arm::R1, 31, 1);

  const char* expected =
      "ubfx r0, r1, #0, #1\n"
      "ubfx r0, r1, #0, #8\n"
      "ubfx r0, r1, #0, #16\n"
      "ubfx r0, r1, #0, #32\n"

      "ubfx r0, r1, #8, #1\n"
      "ubfx r0, r1, #8, #8\n"
      "ubfx r0, r1, #8, #16\n"
      "ubfx r0, r1, #8, #24\n"

      "ubfx r0, r1, #16, #1\n"
      "ubfx r0, r1, #16, #8\n"
      "ubfx r0, r1, #16, #16\n"

      "ubfx r0, r1, #31, #1\n";
  DriverStr(expected, "ubfx");
}

TEST_F(AssemblerThumb2Test, Vmstat) {
  GetAssembler()->vmstat();

  const char* expected = "vmrs APSR_nzcv, FPSCR\n";

  DriverStr(expected, "vmrs");
}

TEST_F(AssemblerThumb2Test, ldrexd) {
  GetAssembler()->ldrexd(arm::R0, arm::R1, arm::R0);
  GetAssembler()->ldrexd(arm::R0, arm::R1, arm::R1);
  GetAssembler()->ldrexd(arm::R0, arm::R1, arm::R2);
  GetAssembler()->ldrexd(arm::R5, arm::R3, arm::R7);

  const char* expected =
      "ldrexd r0, r1, [r0]\n"
      "ldrexd r0, r1, [r1]\n"
      "ldrexd r0, r1, [r2]\n"
      "ldrexd r5, r3, [r7]\n";
  DriverStr(expected, "ldrexd");
}

TEST_F(AssemblerThumb2Test, strexd) {
  GetAssembler()->strexd(arm::R9, arm::R0, arm::R1, arm::R0);
  GetAssembler()->strexd(arm::R9, arm::R0, arm::R1, arm::R1);
  GetAssembler()->strexd(arm::R9, arm::R0, arm::R1, arm::R2);
  GetAssembler()->strexd(arm::R9, arm::R5, arm::R3, arm::R7);

  const char* expected =
      "strexd r9, r0, r1, [r0]\n"
      "strexd r9, r0, r1, [r1]\n"
      "strexd r9, r0, r1, [r2]\n"
      "strexd r9, r5, r3, [r7]\n";
  DriverStr(expected, "strexd");
}

TEST_F(AssemblerThumb2Test, LdrdStrd) {
  GetAssembler()->ldrd(arm::R0, arm::Address(arm::R2, 8));
  GetAssembler()->ldrd(arm::R0, arm::Address(arm::R12));
  GetAssembler()->strd(arm::R0, arm::Address(arm::R2, 8));

  const char* expected =
      "ldrd r0, r1, [r2, #8]\n"
      "ldrd r0, r1, [r12]\n"
      "strd r0, r1, [r2, #8]\n";
  DriverStr(expected, "ldrdstrd");
}

TEST_F(AssemblerThumb2Test, eor) {
#define __ GetAssembler()->
  __ eor(arm::R1, arm::R1, arm::ShifterOperand(arm::R0));
  __ eor(arm::R1, arm::R0, arm::ShifterOperand(arm::R1));
  __ eor(arm::R1, arm::R8, arm::ShifterOperand(arm::R0));
  __ eor(arm::R8, arm::R1, arm::ShifterOperand(arm::R0));
  __ eor(arm::R1, arm::R0, arm::ShifterOperand(arm::R8));

  const char* expected =
      "eors r1, r0\n"
      "eor r1, r0, r1\n"
      "eor r1, r8, r0\n"
      "eor r8, r1, r0\n"
      "eor r1, r0, r8\n";
  DriverStr(expected, "abs");
}

TEST_F(AssemblerThumb2Test, sub) {
  __ subs(arm::R1, arm::R0, arm::ShifterOperand(42));
  __ sub(arm::R1, arm::R0, arm::ShifterOperand(42));

  const char* expected =
      "subs r1, r0, #42\n"
      "subw r1, r0, #42\n";
  DriverStr(expected, "sub");
}

TEST_F(AssemblerThumb2Test, add) {
  __ adds(arm::R1, arm::R0, arm::ShifterOperand(42));
  __ add(arm::R1, arm::R0, arm::ShifterOperand(42));

  const char* expected =
      "adds r1, r0, #42\n"
      "addw r1, r0, #42\n";
  DriverStr(expected, "add");
}

TEST_F(AssemblerThumb2Test, StoreWordToThumbOffset) {
  arm::StoreOperandType type = arm::kStoreWord;
  int32_t offset = 4092;
  ASSERT_TRUE(arm::Address::CanHoldStoreOffsetThumb(type, offset));

  __ StoreToOffset(type, arm::R0, arm::SP, offset);
  __ StoreToOffset(type, arm::IP, arm::SP, offset);
  __ StoreToOffset(type, arm::IP, arm::R5, offset);

  const char* expected =
      "str r0, [sp, #4092]\n"
      "str ip, [sp, #4092]\n"
      "str ip, [r5, #4092]\n";
  DriverStr(expected, "StoreWordToThumbOffset");
}

TEST_F(AssemblerThumb2Test, StoreWordToNonThumbOffset) {
  arm::StoreOperandType type = arm::kStoreWord;
  int32_t offset = 4096;
  ASSERT_FALSE(arm::Address::CanHoldStoreOffsetThumb(type, offset));

  __ StoreToOffset(type, arm::R0, arm::SP, offset);
  __ StoreToOffset(type, arm::IP, arm::SP, offset);
  __ StoreToOffset(type, arm::IP, arm::R5, offset);

  const char* expected =
      "mov ip, #4096\n"       // LoadImmediate(ip, 4096)
      "add ip, ip, sp\n"
      "str r0, [ip, #0]\n"

      "str r5, [sp, #-4]!\n"  // Push(r5)
      "movw r5, #4100\n"      // LoadImmediate(r5, 4096 + kRegisterSize)
      "add r5, r5, sp\n"
      "str ip, [r5, #0]\n"
      "ldr r5, [sp], #4\n"    // Pop(r5)

      "str r6, [sp, #-4]!\n"  // Push(r6)
      "mov r6, #4096\n"       // LoadImmediate(r6, 4096)
      "add r6, r6, r5\n"
      "str ip, [r6, #0]\n"
      "ldr r6, [sp], #4\n";   // Pop(r6)
  DriverStr(expected, "StoreWordToNonThumbOffset");
}

TEST_F(AssemblerThumb2Test, StoreWordPairToThumbOffset) {
  arm::StoreOperandType type = arm::kStoreWordPair;
  int32_t offset = 1020;
  ASSERT_TRUE(arm::Address::CanHoldStoreOffsetThumb(type, offset));

  __ StoreToOffset(type, arm::R0, arm::SP, offset);
  // We cannot use IP (i.e. R12) as first source register, as it would
  // force us to use SP (i.e. R13) as second source register, which
  // would have an "unpredictable" effect according to the ARMv7
  // specification (the T1 encoding describes the result as
  // UNPREDICTABLE when of the source registers is R13).
  //
  // So we use (R11, IP) (e.g. (R11, R12)) as source registers in the
  // following instructions.
  __ StoreToOffset(type, arm::R11, arm::SP, offset);
  __ StoreToOffset(type, arm::R11, arm::R5, offset);

  const char* expected =
      "strd r0, r1, [sp, #1020]\n"
      "strd r11, ip, [sp, #1020]\n"
      "strd r11, ip, [r5, #1020]\n";
  DriverStr(expected, "StoreWordPairToThumbOffset");
}

TEST_F(AssemblerThumb2Test, StoreWordPairToNonThumbOffset) {
  arm::StoreOperandType type = arm::kStoreWordPair;
  int32_t offset = 1024;
  ASSERT_FALSE(arm::Address::CanHoldStoreOffsetThumb(type, offset));

  __ StoreToOffset(type, arm::R0, arm::SP, offset);
  // Same comment as in AssemblerThumb2Test.StoreWordPairToThumbOffset
  // regarding the use of (R11, IP) (e.g. (R11, R12)) as source
  // registers in the following instructions.
  __ StoreToOffset(type, arm::R11, arm::SP, offset);
  __ StoreToOffset(type, arm::R11, arm::R5, offset);

  const char* expected =
      "mov ip, #1024\n"           // LoadImmediate(ip, 1024)
      "add ip, ip, sp\n"
      "strd r0, r1, [ip, #0]\n"

      "str r5, [sp, #-4]!\n"      // Push(r5)
      "movw r5, #1028\n"          // LoadImmediate(r5, 1024 + kRegisterSize)
      "add r5, r5, sp\n"
      "strd r11, ip, [r5, #0]\n"
      "ldr r5, [sp], #4\n"        // Pop(r5)

      "str r6, [sp, #-4]!\n"      // Push(r6)
      "mov r6, #1024\n"           // LoadImmediate(r6, 1024)
      "add r6, r6, r5\n"
      "strd r11, ip, [r6, #0]\n"
      "ldr r6, [sp], #4\n";       // Pop(r6)
  DriverStr(expected, "StoreWordPairToNonThumbOffset");
}

}  // namespace art
