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
#include "base/stringprintf.h"
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

  std::string RepeatInsn(size_t count, const std::string& insn) {
    std::string result;
    for (; count != 0u; --count) {
      result += insn;
    }
    return result;
  }

 private:
  std::vector<arm::Register*> registers_;

  static constexpr const char* kThumb2AssemblyHeader = ".syntax unified\n.thumb\n";
};

TEST_F(AssemblerThumb2Test, Toolchain) {
  EXPECT_TRUE(CheckTools());
}

#define __ GetAssembler()->

TEST_F(AssemblerThumb2Test, Sbfx) {
  __ sbfx(arm::R0, arm::R1, 0, 1);
  __ sbfx(arm::R0, arm::R1, 0, 8);
  __ sbfx(arm::R0, arm::R1, 0, 16);
  __ sbfx(arm::R0, arm::R1, 0, 32);

  __ sbfx(arm::R0, arm::R1, 8, 1);
  __ sbfx(arm::R0, arm::R1, 8, 8);
  __ sbfx(arm::R0, arm::R1, 8, 16);
  __ sbfx(arm::R0, arm::R1, 8, 24);

  __ sbfx(arm::R0, arm::R1, 16, 1);
  __ sbfx(arm::R0, arm::R1, 16, 8);
  __ sbfx(arm::R0, arm::R1, 16, 16);

  __ sbfx(arm::R0, arm::R1, 31, 1);

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
  __ ubfx(arm::R0, arm::R1, 0, 1);
  __ ubfx(arm::R0, arm::R1, 0, 8);
  __ ubfx(arm::R0, arm::R1, 0, 16);
  __ ubfx(arm::R0, arm::R1, 0, 32);

  __ ubfx(arm::R0, arm::R1, 8, 1);
  __ ubfx(arm::R0, arm::R1, 8, 8);
  __ ubfx(arm::R0, arm::R1, 8, 16);
  __ ubfx(arm::R0, arm::R1, 8, 24);

  __ ubfx(arm::R0, arm::R1, 16, 1);
  __ ubfx(arm::R0, arm::R1, 16, 8);
  __ ubfx(arm::R0, arm::R1, 16, 16);

  __ ubfx(arm::R0, arm::R1, 31, 1);

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
  __ vmstat();

  const char* expected = "vmrs APSR_nzcv, FPSCR\n";

  DriverStr(expected, "vmrs");
}

TEST_F(AssemblerThumb2Test, ldrexd) {
  __ ldrexd(arm::R0, arm::R1, arm::R0);
  __ ldrexd(arm::R0, arm::R1, arm::R1);
  __ ldrexd(arm::R0, arm::R1, arm::R2);
  __ ldrexd(arm::R5, arm::R3, arm::R7);

  const char* expected =
      "ldrexd r0, r1, [r0]\n"
      "ldrexd r0, r1, [r1]\n"
      "ldrexd r0, r1, [r2]\n"
      "ldrexd r5, r3, [r7]\n";
  DriverStr(expected, "ldrexd");
}

TEST_F(AssemblerThumb2Test, strexd) {
  __ strexd(arm::R9, arm::R0, arm::R1, arm::R0);
  __ strexd(arm::R9, arm::R0, arm::R1, arm::R1);
  __ strexd(arm::R9, arm::R0, arm::R1, arm::R2);
  __ strexd(arm::R9, arm::R5, arm::R3, arm::R7);

  const char* expected =
      "strexd r9, r0, r1, [r0]\n"
      "strexd r9, r0, r1, [r1]\n"
      "strexd r9, r0, r1, [r2]\n"
      "strexd r9, r5, r3, [r7]\n";
  DriverStr(expected, "strexd");
}

TEST_F(AssemblerThumb2Test, LdrdStrd) {
  __ ldrd(arm::R0, arm::Address(arm::R2, 8));
  __ ldrd(arm::R0, arm::Address(arm::R12));
  __ strd(arm::R0, arm::Address(arm::R2, 8));

  const char* expected =
      "ldrd r0, r1, [r2, #8]\n"
      "ldrd r0, r1, [r12]\n"
      "strd r0, r1, [r2, #8]\n";
  DriverStr(expected, "ldrdstrd");
}

TEST_F(AssemblerThumb2Test, eor) {
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
  __ subs(arm::R1, arm::R0, arm::ShifterOperand(arm::R2, arm::ASR, 31));
  __ sub(arm::R1, arm::R0, arm::ShifterOperand(arm::R2, arm::ASR, 31));

  const char* expected =
      "subs r1, r0, #42\n"
      "sub.w r1, r0, #42\n"
      "subs r1, r0, r2, asr #31\n"
      "sub r1, r0, r2, asr #31\n";
  DriverStr(expected, "sub");
}

TEST_F(AssemblerThumb2Test, add) {
  __ adds(arm::R1, arm::R0, arm::ShifterOperand(42));
  __ add(arm::R1, arm::R0, arm::ShifterOperand(42));
  __ adds(arm::R1, arm::R0, arm::ShifterOperand(arm::R2, arm::ASR, 31));
  __ add(arm::R1, arm::R0, arm::ShifterOperand(arm::R2, arm::ASR, 31));

  const char* expected =
      "adds r1, r0, #42\n"
      "add.w r1, r0, #42\n"
      "adds r1, r0, r2, asr #31\n"
      "add r1, r0, r2, asr #31\n";
  DriverStr(expected, "add");
}

TEST_F(AssemblerThumb2Test, umull) {
  __ umull(arm::R0, arm::R1, arm::R2, arm::R3);

  const char* expected =
      "umull r0, r1, r2, r3\n";
  DriverStr(expected, "umull");
}

TEST_F(AssemblerThumb2Test, smull) {
  __ smull(arm::R0, arm::R1, arm::R2, arm::R3);

  const char* expected =
      "smull r0, r1, r2, r3\n";
  DriverStr(expected, "smull");
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
      "add.w ip, sp, #4096\n"   // AddConstant(ip, sp, 4096)
      "str r0, [ip, #0]\n"

      "str r5, [sp, #-4]!\n"    // Push(r5)
      "add.w r5, sp, #4096\n"   // AddConstant(r5, 4100 & ~0xfff)
      "str ip, [r5, #4]\n"      // StoreToOffset(type, ip, r5, 4100 & 0xfff)
      "ldr r5, [sp], #4\n"      // Pop(r5)

      "str r6, [sp, #-4]!\n"    // Push(r6)
      "add.w r6, r5, #4096\n"   // AddConstant(r6, r5, 4096 & ~0xfff)
      "str ip, [r6, #0]\n"      // StoreToOffset(type, ip, r6, 4096 & 0xfff)
      "ldr r6, [sp], #4\n";     // Pop(r6)
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
      "add.w ip, sp, #1024\n"     // AddConstant(ip, sp, 1024)
      "strd r0, r1, [ip, #0]\n"

      "str r5, [sp, #-4]!\n"      // Push(r5)
      "add.w r5, sp, #1024\n"     // AddConstant(r5, sp, (1024 + kRegisterSize) & ~0x3fc)
      "strd r11, ip, [r5, #4]\n"  // StoreToOffset(type, r11, sp, (1024 + kRegisterSize) & 0x3fc)
      "ldr r5, [sp], #4\n"        // Pop(r5)

      "str r6, [sp, #-4]!\n"      // Push(r6)
      "add.w r6, r5, #1024\n"     // AddConstant(r6, r5, 1024 & ~0x3fc)
      "strd r11, ip, [r6, #0]\n"  // StoreToOffset(type, r11, r6, 1024 & 0x3fc)
      "ldr r6, [sp], #4\n";       // Pop(r6)
  DriverStr(expected, "StoreWordPairToNonThumbOffset");
}

TEST_F(AssemblerThumb2Test, DistantBackBranch) {
  Label start, end;
  __ Bind(&start);
  constexpr size_t kLdrR0R0Count1 = 256;
  for (size_t i = 0; i != kLdrR0R0Count1; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ b(&end, arm::EQ);
  __ b(&start, arm::LT);
  constexpr size_t kLdrR0R0Count2 = 256;
  for (size_t i = 0; i != kLdrR0R0Count2; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ Bind(&end);

  std::string expected =
      "0:\n" +
      RepeatInsn(kLdrR0R0Count1, "ldr r0, [r0]\n") +
      "beq 1f\n"
      "blt 0b\n" +
      RepeatInsn(kLdrR0R0Count2, "ldr r0, [r0]\n") +
      "1:\n";
  DriverStr(expected, "DistantBackBranch");
}

TEST_F(AssemblerThumb2Test, TwoCbzMaxOffset) {
  Label label0, label1, label2;
  __ cbz(arm::R0, &label1);
  constexpr size_t kLdrR0R0Count1 = 63;
  for (size_t i = 0; i != kLdrR0R0Count1; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ Bind(&label0);
  __ cbz(arm::R0, &label2);
  __ Bind(&label1);
  constexpr size_t kLdrR0R0Count2 = 64;
  for (size_t i = 0; i != kLdrR0R0Count2; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ Bind(&label2);

  std::string expected =
      "cbz r0, 1f\n" +            // cbz r0, label1
      RepeatInsn(kLdrR0R0Count1, "ldr r0, [r0]\n") +
      "0:\n"
      "cbz r0, 2f\n"              // cbz r0, label2
      "1:\n" +
      RepeatInsn(kLdrR0R0Count2, "ldr r0, [r0]\n") +
      "2:\n";
  DriverStr(expected, "TwoCbzMaxOffset");

  EXPECT_EQ(static_cast<uint32_t>(label0.Position()) + 0u,
            __ GetAdjustedPosition(label0.Position()));
  EXPECT_EQ(static_cast<uint32_t>(label1.Position()) + 0u,
            __ GetAdjustedPosition(label1.Position()));
  EXPECT_EQ(static_cast<uint32_t>(label2.Position()) + 0u,
            __ GetAdjustedPosition(label2.Position()));
}

TEST_F(AssemblerThumb2Test, TwoCbzBeyondMaxOffset) {
  Label label0, label1, label2;
  __ cbz(arm::R0, &label1);
  constexpr size_t kLdrR0R0Count1 = 63;
  for (size_t i = 0; i != kLdrR0R0Count1; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ Bind(&label0);
  __ cbz(arm::R0, &label2);
  __ Bind(&label1);
  constexpr size_t kLdrR0R0Count2 = 65;
  for (size_t i = 0; i != kLdrR0R0Count2; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ Bind(&label2);

  std::string expected =
      "cmp r0, #0\n"              // cbz r0, label1
      "beq.n 1f\n" +
      RepeatInsn(kLdrR0R0Count1, "ldr r0, [r0]\n") +
      "0:\n"
      "cmp r0, #0\n"              // cbz r0, label2
      "beq.n 2f\n"
      "1:\n" +
      RepeatInsn(kLdrR0R0Count2, "ldr r0, [r0]\n") +
      "2:\n";
  DriverStr(expected, "TwoCbzBeyondMaxOffset");

  EXPECT_EQ(static_cast<uint32_t>(label0.Position()) + 2u,
            __ GetAdjustedPosition(label0.Position()));
  EXPECT_EQ(static_cast<uint32_t>(label1.Position()) + 4u,
            __ GetAdjustedPosition(label1.Position()));
  EXPECT_EQ(static_cast<uint32_t>(label2.Position()) + 4u,
            __ GetAdjustedPosition(label2.Position()));
}

TEST_F(AssemblerThumb2Test, TwoCbzSecondAtMaxB16Offset) {
  Label label0, label1, label2;
  __ cbz(arm::R0, &label1);
  constexpr size_t kLdrR0R0Count1 = 62;
  for (size_t i = 0; i != kLdrR0R0Count1; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ Bind(&label0);
  __ cbz(arm::R0, &label2);
  __ Bind(&label1);
  constexpr size_t kLdrR0R0Count2 = 128;
  for (size_t i = 0; i != kLdrR0R0Count2; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ Bind(&label2);

  std::string expected =
      "cbz r0, 1f\n" +            // cbz r0, label1
      RepeatInsn(kLdrR0R0Count1, "ldr r0, [r0]\n") +
      "0:\n"
      "cmp r0, #0\n"              // cbz r0, label2
      "beq.n 2f\n"
      "1:\n" +
      RepeatInsn(kLdrR0R0Count2, "ldr r0, [r0]\n") +
      "2:\n";
  DriverStr(expected, "TwoCbzSecondAtMaxB16Offset");

  EXPECT_EQ(static_cast<uint32_t>(label0.Position()) + 0u,
            __ GetAdjustedPosition(label0.Position()));
  EXPECT_EQ(static_cast<uint32_t>(label1.Position()) + 2u,
            __ GetAdjustedPosition(label1.Position()));
  EXPECT_EQ(static_cast<uint32_t>(label2.Position()) + 2u,
            __ GetAdjustedPosition(label2.Position()));
}

TEST_F(AssemblerThumb2Test, TwoCbzSecondBeyondMaxB16Offset) {
  Label label0, label1, label2;
  __ cbz(arm::R0, &label1);
  constexpr size_t kLdrR0R0Count1 = 62;
  for (size_t i = 0; i != kLdrR0R0Count1; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ Bind(&label0);
  __ cbz(arm::R0, &label2);
  __ Bind(&label1);
  constexpr size_t kLdrR0R0Count2 = 129;
  for (size_t i = 0; i != kLdrR0R0Count2; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ Bind(&label2);

  std::string expected =
      "cmp r0, #0\n"              // cbz r0, label1
      "beq.n 1f\n" +
      RepeatInsn(kLdrR0R0Count1, "ldr r0, [r0]\n") +
      "0:\n"
      "cmp r0, #0\n"              // cbz r0, label2
      "beq.w 2f\n"
      "1:\n" +
      RepeatInsn(kLdrR0R0Count2, "ldr r0, [r0]\n") +
      "2:\n";
  DriverStr(expected, "TwoCbzSecondBeyondMaxB16Offset");

  EXPECT_EQ(static_cast<uint32_t>(label0.Position()) + 2u,
            __ GetAdjustedPosition(label0.Position()));
  EXPECT_EQ(static_cast<uint32_t>(label1.Position()) + 6u,
            __ GetAdjustedPosition(label1.Position()));
  EXPECT_EQ(static_cast<uint32_t>(label2.Position()) + 6u,
            __ GetAdjustedPosition(label2.Position()));
}

TEST_F(AssemblerThumb2Test, TwoCbzFirstAtMaxB16Offset) {
  Label label0, label1, label2;
  __ cbz(arm::R0, &label1);
  constexpr size_t kLdrR0R0Count1 = 127;
  for (size_t i = 0; i != kLdrR0R0Count1; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ Bind(&label0);
  __ cbz(arm::R0, &label2);
  __ Bind(&label1);
  constexpr size_t kLdrR0R0Count2 = 64;
  for (size_t i = 0; i != kLdrR0R0Count2; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ Bind(&label2);

  std::string expected =
      "cmp r0, #0\n"              // cbz r0, label1
      "beq.n 1f\n" +
      RepeatInsn(kLdrR0R0Count1, "ldr r0, [r0]\n") +
      "0:\n"
      "cbz r0, 2f\n"              // cbz r0, label2
      "1:\n" +
      RepeatInsn(kLdrR0R0Count2, "ldr r0, [r0]\n") +
      "2:\n";
  DriverStr(expected, "TwoCbzFirstAtMaxB16Offset");

  EXPECT_EQ(static_cast<uint32_t>(label0.Position()) + 2u,
            __ GetAdjustedPosition(label0.Position()));
  EXPECT_EQ(static_cast<uint32_t>(label1.Position()) + 2u,
            __ GetAdjustedPosition(label1.Position()));
  EXPECT_EQ(static_cast<uint32_t>(label2.Position()) + 2u,
            __ GetAdjustedPosition(label2.Position()));
}

TEST_F(AssemblerThumb2Test, TwoCbzFirstBeyondMaxB16Offset) {
  Label label0, label1, label2;
  __ cbz(arm::R0, &label1);
  constexpr size_t kLdrR0R0Count1 = 127;
  for (size_t i = 0; i != kLdrR0R0Count1; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ Bind(&label0);
  __ cbz(arm::R0, &label2);
  __ Bind(&label1);
  constexpr size_t kLdrR0R0Count2 = 65;
  for (size_t i = 0; i != kLdrR0R0Count2; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ Bind(&label2);

  std::string expected =
      "cmp r0, #0\n"              // cbz r0, label1
      "beq.w 1f\n" +
      RepeatInsn(kLdrR0R0Count1, "ldr r0, [r0]\n") +
      "0:\n"
      "cmp r0, #0\n"              // cbz r0, label2
      "beq.n 2f\n"
      "1:\n" +
      RepeatInsn(kLdrR0R0Count2, "ldr r0, [r0]\n") +
      "2:\n";
  DriverStr(expected, "TwoCbzFirstBeyondMaxB16Offset");

  EXPECT_EQ(static_cast<uint32_t>(label0.Position()) + 4u,
            __ GetAdjustedPosition(label0.Position()));
  EXPECT_EQ(static_cast<uint32_t>(label1.Position()) + 6u,
            __ GetAdjustedPosition(label1.Position()));
  EXPECT_EQ(static_cast<uint32_t>(label2.Position()) + 6u,
            __ GetAdjustedPosition(label2.Position()));
}

TEST_F(AssemblerThumb2Test, LoadLiteralMax1KiB) {
  arm::Literal* literal = __ NewLiteral<int32_t>(0x12345678);
  __ LoadLiteral(arm::R0, literal);
  Label label;
  __ Bind(&label);
  constexpr size_t kLdrR0R0Count = 511;
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  std::string expected =
      "1:\n"
      "ldr.n r0, [pc, #((2f - 1b - 2) & ~2)]\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".align 2, 0\n"
      "2:\n"
      ".word 0x12345678\n";
  DriverStr(expected, "LoadLiteralMax1KiB");

  EXPECT_EQ(static_cast<uint32_t>(label.Position()) + 0u,
            __ GetAdjustedPosition(label.Position()));
}

TEST_F(AssemblerThumb2Test, LoadLiteralBeyondMax1KiB) {
  arm::Literal* literal = __ NewLiteral<int32_t>(0x12345678);
  __ LoadLiteral(arm::R0, literal);
  Label label;
  __ Bind(&label);
  constexpr size_t kLdrR0R0Count = 512;
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  std::string expected =
      "1:\n"
      "ldr.w r0, [pc, #((2f - 1b - 2) & ~2)]\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".align 2, 0\n"
      "2:\n"
      ".word 0x12345678\n";
  DriverStr(expected, "LoadLiteralBeyondMax1KiB");

  EXPECT_EQ(static_cast<uint32_t>(label.Position()) + 2u,
            __ GetAdjustedPosition(label.Position()));
}

TEST_F(AssemblerThumb2Test, LoadLiteralMax4KiB) {
  arm::Literal* literal = __ NewLiteral<int32_t>(0x12345678);
  __ LoadLiteral(arm::R1, literal);
  Label label;
  __ Bind(&label);
  constexpr size_t kLdrR0R0Count = 2046;
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  std::string expected =
      "1:\n"
      "ldr.w r1, [pc, #((2f - 1b - 2) & ~2)]\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".align 2, 0\n"
      "2:\n"
      ".word 0x12345678\n";
  DriverStr(expected, "LoadLiteralMax4KiB");

  EXPECT_EQ(static_cast<uint32_t>(label.Position()) + 2u,
            __ GetAdjustedPosition(label.Position()));
}

TEST_F(AssemblerThumb2Test, LoadLiteralBeyondMax4KiB) {
  arm::Literal* literal = __ NewLiteral<int32_t>(0x12345678);
  __ LoadLiteral(arm::R1, literal);
  Label label;
  __ Bind(&label);
  constexpr size_t kLdrR0R0Count = 2047;
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  std::string expected =
      "movw r1, #4096\n"  // "as" does not consider (2f - 1f - 4) a constant expression for movw.
      "1:\n"
      "add r1, pc\n"
      "ldr r1, [r1, #0]\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".align 2, 0\n"
      "2:\n"
      ".word 0x12345678\n";
  DriverStr(expected, "LoadLiteralBeyondMax4KiB");

  EXPECT_EQ(static_cast<uint32_t>(label.Position()) + 6u,
            __ GetAdjustedPosition(label.Position()));
}

TEST_F(AssemblerThumb2Test, LoadLiteralMax64KiB) {
  arm::Literal* literal = __ NewLiteral<int32_t>(0x12345678);
  __ LoadLiteral(arm::R1, literal);
  Label label;
  __ Bind(&label);
  constexpr size_t kLdrR0R0Count = (1u << 15) - 2u;
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  std::string expected =
      "movw r1, #0xfffc\n"  // "as" does not consider (2f - 1f - 4) a constant expression for movw.
      "1:\n"
      "add r1, pc\n"
      "ldr r1, [r1, #0]\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".align 2, 0\n"
      "2:\n"
      ".word 0x12345678\n";
  DriverStr(expected, "LoadLiteralMax64KiB");

  EXPECT_EQ(static_cast<uint32_t>(label.Position()) + 6u,
            __ GetAdjustedPosition(label.Position()));
}

TEST_F(AssemblerThumb2Test, LoadLiteralBeyondMax64KiB) {
  arm::Literal* literal = __ NewLiteral<int32_t>(0x12345678);
  __ LoadLiteral(arm::R1, literal);
  Label label;
  __ Bind(&label);
  constexpr size_t kLdrR0R0Count = (1u << 15) - 1u;
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  std::string expected =
      "mov.w r1, #((2f - 1f - 4) & ~0xfff)\n"
      "1:\n"
      "add r1, pc\n"
      "ldr r1, [r1, #((2f - 1b - 4) & 0xfff)]\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".align 2, 0\n"
      "2:\n"
      ".word 0x12345678\n";
  DriverStr(expected, "LoadLiteralBeyondMax64KiB");

  EXPECT_EQ(static_cast<uint32_t>(label.Position()) + 8u,
            __ GetAdjustedPosition(label.Position()));
}

TEST_F(AssemblerThumb2Test, LoadLiteralMax1MiB) {
  arm::Literal* literal = __ NewLiteral<int32_t>(0x12345678);
  __ LoadLiteral(arm::R1, literal);
  Label label;
  __ Bind(&label);
  constexpr size_t kLdrR0R0Count = (1u << 19) - 3u;
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  std::string expected =
      "mov.w r1, #((2f - 1f - 4) & ~0xfff)\n"
      "1:\n"
      "add r1, pc\n"
      "ldr r1, [r1, #((2f - 1b - 4) & 0xfff)]\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".align 2, 0\n"
      "2:\n"
      ".word 0x12345678\n";
  DriverStr(expected, "LoadLiteralMax1MiB");

  EXPECT_EQ(static_cast<uint32_t>(label.Position()) + 8u,
            __ GetAdjustedPosition(label.Position()));
}

TEST_F(AssemblerThumb2Test, LoadLiteralBeyondMax1MiB) {
  arm::Literal* literal = __ NewLiteral<int32_t>(0x12345678);
  __ LoadLiteral(arm::R1, literal);
  Label label;
  __ Bind(&label);
  constexpr size_t kLdrR0R0Count = (1u << 19) - 2u;
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  std::string expected =
      // "as" does not consider ((2f - 1f - 4) & 0xffff) a constant expression for movw.
      "movw r1, #(0x100000 & 0xffff)\n"
      // "as" does not consider ((2f - 1f - 4) >> 16) a constant expression for movt.
      "movt r1, #(0x100000 >> 16)\n"
      "1:\n"
      "add r1, pc\n"
      "ldr.w r1, [r1, #0]\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".align 2, 0\n"
      "2:\n"
      ".word 0x12345678\n";
  DriverStr(expected, "LoadLiteralBeyondMax1MiB");

  EXPECT_EQ(static_cast<uint32_t>(label.Position()) + 12u,
            __ GetAdjustedPosition(label.Position()));
}

TEST_F(AssemblerThumb2Test, LoadLiteralFar) {
  arm::Literal* literal = __ NewLiteral<int32_t>(0x12345678);
  __ LoadLiteral(arm::R1, literal);
  Label label;
  __ Bind(&label);
  constexpr size_t kLdrR0R0Count = (1u << 19) - 2u + 0x1234;
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  std::string expected =
      // "as" does not consider ((2f - 1f - 4) & 0xffff) a constant expression for movw.
      "movw r1, #((0x100000 + 2 * 0x1234) & 0xffff)\n"
      // "as" does not consider ((2f - 1f - 4) >> 16) a constant expression for movt.
      "movt r1, #((0x100000 + 2 * 0x1234) >> 16)\n"
      "1:\n"
      "add r1, pc\n"
      "ldr.w r1, [r1, #0]\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".align 2, 0\n"
      "2:\n"
      ".word 0x12345678\n";
  DriverStr(expected, "LoadLiteralFar");

  EXPECT_EQ(static_cast<uint32_t>(label.Position()) + 12u,
            __ GetAdjustedPosition(label.Position()));
}

TEST_F(AssemblerThumb2Test, LoadLiteralWideMax1KiB) {
  arm::Literal* literal = __ NewLiteral<int64_t>(INT64_C(0x1234567887654321));
  __ LoadLiteral(arm::R1, arm::R3, literal);
  Label label;
  __ Bind(&label);
  constexpr size_t kLdrR0R0Count = 510;
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  std::string expected =
      "1:\n"
      "ldrd r1, r3, [pc, #((2f - 1b - 2) & ~2)]\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".align 2, 0\n"
      "2:\n"
      ".word 0x87654321\n"
      ".word 0x12345678\n";
  DriverStr(expected, "LoadLiteralWideMax1KiB");

  EXPECT_EQ(static_cast<uint32_t>(label.Position()) + 0u,
            __ GetAdjustedPosition(label.Position()));
}

TEST_F(AssemblerThumb2Test, LoadLiteralWideBeyondMax1KiB) {
  arm::Literal* literal = __ NewLiteral<int64_t>(INT64_C(0x1234567887654321));
  __ LoadLiteral(arm::R1, arm::R3, literal);
  Label label;
  __ Bind(&label);
  constexpr size_t kLdrR0R0Count = 511;
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  std::string expected =
      "mov.w ip, #((2f - 1f - 4) & ~0x3ff)\n"
      "1:\n"
      "add ip, pc\n"
      "ldrd r1, r3, [ip, #((2f - 1b - 4) & 0x3ff)]\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".align 2, 0\n"
      "2:\n"
      ".word 0x87654321\n"
      ".word 0x12345678\n";
  DriverStr(expected, "LoadLiteralWideBeyondMax1KiB");

  EXPECT_EQ(static_cast<uint32_t>(label.Position()) + 6u,
            __ GetAdjustedPosition(label.Position()));
}

TEST_F(AssemblerThumb2Test, LoadLiteralSingleMax256KiB) {
  // The literal size must match but the type doesn't, so use an int32_t rather than float.
  arm::Literal* literal = __ NewLiteral<int32_t>(0x12345678);
  __ LoadLiteral(arm::S3, literal);
  Label label;
  __ Bind(&label);
  constexpr size_t kLdrR0R0Count = (1 << 17) - 3u;
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  std::string expected =
      "mov.w ip, #((2f - 1f - 4) & ~0x3ff)\n"
      "1:\n"
      "add ip, pc\n"
      "vldr s3, [ip, #((2f - 1b - 4) & 0x3ff)]\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".align 2, 0\n"
      "2:\n"
      ".word 0x12345678\n";
  DriverStr(expected, "LoadLiteralSingleMax256KiB");

  EXPECT_EQ(static_cast<uint32_t>(label.Position()) + 6u,
            __ GetAdjustedPosition(label.Position()));
}

TEST_F(AssemblerThumb2Test, LoadLiteralDoubleBeyondMax256KiB) {
  // The literal size must match but the type doesn't, so use an int64_t rather than double.
  arm::Literal* literal = __ NewLiteral<int64_t>(INT64_C(0x1234567887654321));
  __ LoadLiteral(arm::D3, literal);
  Label label;
  __ Bind(&label);
  constexpr size_t kLdrR0R0Count = (1 << 17) - 2u;
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  std::string expected =
      // "as" does not consider ((2f - 1f - 4) & 0xffff) a constant expression for movw.
      "movw ip, #(0x40000 & 0xffff)\n"
      // "as" does not consider ((2f - 1f - 4) >> 16) a constant expression for movt.
      "movt ip, #(0x40000 >> 16)\n"
      "1:\n"
      "add ip, pc\n"
      "vldr d3, [ip, #0]\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".align 2, 0\n"
      "2:\n"
      ".word 0x87654321\n"
      ".word 0x12345678\n";
  DriverStr(expected, "LoadLiteralDoubleBeyondMax256KiB");

  EXPECT_EQ(static_cast<uint32_t>(label.Position()) + 10u,
            __ GetAdjustedPosition(label.Position()));
}

TEST_F(AssemblerThumb2Test, LoadLiteralDoubleFar) {
  // The literal size must match but the type doesn't, so use an int64_t rather than double.
  arm::Literal* literal = __ NewLiteral<int64_t>(INT64_C(0x1234567887654321));
  __ LoadLiteral(arm::D3, literal);
  Label label;
  __ Bind(&label);
  constexpr size_t kLdrR0R0Count = (1 << 17) - 2u + 0x1234;
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  std::string expected =
      // "as" does not consider ((2f - 1f - 4) & 0xffff) a constant expression for movw.
      "movw ip, #((0x40000 + 2 * 0x1234) & 0xffff)\n"
      // "as" does not consider ((2f - 1f - 4) >> 16) a constant expression for movt.
      "movt ip, #((0x40000 + 2 * 0x1234) >> 16)\n"
      "1:\n"
      "add ip, pc\n"
      "vldr d3, [ip, #0]\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".align 2, 0\n"
      "2:\n"
      ".word 0x87654321\n"
      ".word 0x12345678\n";
  DriverStr(expected, "LoadLiteralDoubleFar");

  EXPECT_EQ(static_cast<uint32_t>(label.Position()) + 10u,
            __ GetAdjustedPosition(label.Position()));
}

TEST_F(AssemblerThumb2Test, LoadLiteralBeyondMax1KiBDueToAlignmentOnSecondPass) {
  // First part: as TwoCbzBeyondMaxOffset but add one 16-bit instruction to the end,
  // so that the size is not Aligned<4>(.). On the first pass, the assembler resizes
  // the second CBZ because it's out of range, then it will resize the first CBZ
  // which has been pushed out of range. Thus, after the first pass, the code size
  // will appear Aligned<4>(.) but the final size will not be.
  Label label0, label1, label2;
  __ cbz(arm::R0, &label1);
  constexpr size_t kLdrR0R0Count1 = 63;
  for (size_t i = 0; i != kLdrR0R0Count1; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ Bind(&label0);
  __ cbz(arm::R0, &label2);
  __ Bind(&label1);
  constexpr size_t kLdrR0R0Count2 = 65;
  for (size_t i = 0; i != kLdrR0R0Count2; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ Bind(&label2);
  __ ldr(arm::R0, arm::Address(arm::R0));

  std::string expected_part1 =
      "cmp r0, #0\n"              // cbz r0, label1
      "beq.n 1f\n" +
      RepeatInsn(kLdrR0R0Count1, "ldr r0, [r0]\n") +
      "0:\n"
      "cmp r0, #0\n"              // cbz r0, label2
      "beq.n 2f\n"
      "1:\n" +
      RepeatInsn(kLdrR0R0Count2, "ldr r0, [r0]\n") +
      "2:\n"                      // Here the offset is Aligned<4>(.).
      "ldr r0, [r0]\n";           // Make the first part

  // Second part: as LoadLiteralMax1KiB with the caveat that the offset of the load
  // literal will not be Aligned<4>(.) but it will appear to be when we process the
  // instruction during the first pass, so the literal will need a padding and it
  // will push the literal out of range, so we shall end up with "ldr.w".
  arm::Literal* literal = __ NewLiteral<int32_t>(0x12345678);
  __ LoadLiteral(arm::R0, literal);
  Label label;
  __ Bind(&label);
  constexpr size_t kLdrR0R0Count = 511;
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  std::string expected =
      expected_part1 +
      "1:\n"
      "ldr.w r0, [pc, #((2f - 1b - 2) & ~2)]\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".align 2, 0\n"
      "2:\n"
      ".word 0x12345678\n";
  DriverStr(expected, "LoadLiteralMax1KiB");

  EXPECT_EQ(static_cast<uint32_t>(label.Position()) + 6u,
            __ GetAdjustedPosition(label.Position()));
}

TEST_F(AssemblerThumb2Test, BindTrackedLabel) {
  Label non_tracked, tracked, branch_target;

  // A few dummy loads on entry.
  constexpr size_t kLdrR0R0Count = 5;
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  // A branch that will need to be fixed up.
  __ cbz(arm::R0, &branch_target);

  // Some more dummy loads.
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  // Now insert tracked and untracked label.
  __ Bind(&non_tracked);
  __ BindTrackedLabel(&tracked);

  // A lot of dummy loads, to ensure the branch needs resizing.
  constexpr size_t kLdrR0R0CountLong = 60;
  for (size_t i = 0; i != kLdrR0R0CountLong; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  // Bind the branch target.
  __ Bind(&branch_target);

  // One more load.
  __ ldr(arm::R0, arm::Address(arm::R0));

  std::string expected =
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      "cmp r0, #0\n"                                                       // cbz r0, 1f
      "beq.n 1f\n" +
      RepeatInsn(kLdrR0R0Count + kLdrR0R0CountLong, "ldr r0, [r0]\n") +
      "1:\n"
      "ldr r0, [r0]\n";
  DriverStr(expected, "BindTrackedLabel");

  // Expectation is that the tracked label should have moved.
  EXPECT_LT(non_tracked.Position(), tracked.Position());
}

TEST_F(AssemblerThumb2Test, JumpTable) {
  // The jump table. Use three labels.
  Label label1, label2, label3;
  std::vector<Label*> labels({ &label1, &label2, &label3 });

  // A few dummy loads on entry, interspersed with 2 labels.
  constexpr size_t kLdrR0R0Count = 5;
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ BindTrackedLabel(&label1);
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ BindTrackedLabel(&label2);
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  // Create the jump table, emit the base load.
  arm::JumpTable* jump_table = __ CreateJumpTable(std::move(labels), arm::R1);

  // Dummy computation, stand-in for the address. We're only testing the jump table here, not how
  // it's being used.
  __ ldr(arm::R0, arm::Address(arm::R0));

  // Emit the jump
  __ EmitJumpTableDispatch(jump_table, arm::R1);

  // Some more dummy instructions.
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ BindTrackedLabel(&label3);
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {          // Note: odd so there's no alignment
    __ ldr(arm::R0, arm::Address(arm::R0));              //       necessary, as gcc as emits nops,
  }                                                      //       whereas we emit 0 != nop.

  static_assert((kLdrR0R0Count + 3) * 2 < 1 * KB, "Too much offset");

  std::string expected =
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".L1:\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".L2:\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      "adr r1, .Ljump_table\n"
      "ldr r0, [r0]\n"
      ".Lbase:\n"
      "add pc, r1\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".L3:\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".align 2\n"
      ".Ljump_table:\n"
      ".4byte (.L1 - .Lbase - 4)\n"
      ".4byte (.L2 - .Lbase - 4)\n"
      ".4byte (.L3 - .Lbase - 4)\n";
  DriverStr(expected, "JumpTable");
}

// Test for >1K fixup.
TEST_F(AssemblerThumb2Test, JumpTable4K) {
  // The jump table. Use three labels.
  Label label1, label2, label3;
  std::vector<Label*> labels({ &label1, &label2, &label3 });

  // A few dummy loads on entry, interspersed with 2 labels.
  constexpr size_t kLdrR0R0Count = 5;
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ BindTrackedLabel(&label1);
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ BindTrackedLabel(&label2);
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  // Create the jump table, emit the base load.
  arm::JumpTable* jump_table = __ CreateJumpTable(std::move(labels), arm::R1);

  // Dummy computation, stand-in for the address. We're only testing the jump table here, not how
  // it's being used.
  __ ldr(arm::R0, arm::Address(arm::R0));

  // Emit the jump
  __ EmitJumpTableDispatch(jump_table, arm::R1);

  // Some more dummy instructions.
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ BindTrackedLabel(&label3);
  constexpr size_t kLdrR0R0Count2 = 600;               // Note: even so there's no alignment
  for (size_t i = 0; i != kLdrR0R0Count2; ++i) {       //       necessary, as gcc as emits nops,
    __ ldr(arm::R0, arm::Address(arm::R0));            //       whereas we emit 0 != nop.
  }

  static_assert((kLdrR0R0Count + kLdrR0R0Count2 + 3) * 2 > 1 * KB, "Not enough offset");
  static_assert((kLdrR0R0Count + kLdrR0R0Count2 + 3) * 2 < 4 * KB, "Too much offset");

  std::string expected =
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".L1:\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".L2:\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      "adr r1, .Ljump_table\n"
      "ldr r0, [r0]\n"
      ".Lbase:\n"
      "add pc, r1\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".L3:\n" +
      RepeatInsn(kLdrR0R0Count2, "ldr r0, [r0]\n") +
      ".align 2\n"
      ".Ljump_table:\n"
      ".4byte (.L1 - .Lbase - 4)\n"
      ".4byte (.L2 - .Lbase - 4)\n"
      ".4byte (.L3 - .Lbase - 4)\n";
  DriverStr(expected, "JumpTable4K");
}

// Test for >4K fixup.
TEST_F(AssemblerThumb2Test, JumpTable64K) {
  // The jump table. Use three labels.
  Label label1, label2, label3;
  std::vector<Label*> labels({ &label1, &label2, &label3 });

  // A few dummy loads on entry, interspersed with 2 labels.
  constexpr size_t kLdrR0R0Count = 5;
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ BindTrackedLabel(&label1);
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ BindTrackedLabel(&label2);
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  // Create the jump table, emit the base load.
  arm::JumpTable* jump_table = __ CreateJumpTable(std::move(labels), arm::R1);

  // Dummy computation, stand-in for the address. We're only testing the jump table here, not how
  // it's being used.
  __ ldr(arm::R0, arm::Address(arm::R0));

  // Emit the jump
  __ EmitJumpTableDispatch(jump_table, arm::R1);

  // Some more dummy instructions.
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ BindTrackedLabel(&label3);
  constexpr size_t kLdrR0R0Count2 = 2601;              // Note: odd so there's no alignment
  for (size_t i = 0; i != kLdrR0R0Count2; ++i) {       //       necessary, as gcc as emits nops,
    __ ldr(arm::R0, arm::Address(arm::R0));            //       whereas we emit 0 != nop.
  }

  static_assert((kLdrR0R0Count + kLdrR0R0Count2 + 3) * 2 > 4 * KB, "Not enough offset");
  static_assert((kLdrR0R0Count + kLdrR0R0Count2 + 3) * 2 < 64 * KB, "Too much offset");

  std::string expected =
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".L1:\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".L2:\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      // ~ adr r1, .Ljump_table, gcc as can't seem to fix up a large offset itself.
      // (Note: have to use constants, as labels aren't accepted.
      "movw r1, #(((3 + " + StringPrintf("%zu", kLdrR0R0Count + kLdrR0R0Count2) +
          ") * 2 - 4) & 0xFFFF)\n"
      "add r1, pc\n"
      "ldr r0, [r0]\n"
      ".Lbase:\n"
      "add pc, r1\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".L3:\n" +
      RepeatInsn(kLdrR0R0Count2, "ldr r0, [r0]\n") +
      ".align 2\n"
      ".Ljump_table:\n"
      ".4byte (.L1 - .Lbase - 4)\n"
      ".4byte (.L2 - .Lbase - 4)\n"
      ".4byte (.L3 - .Lbase - 4)\n";
  DriverStr(expected, "JumpTable64K");
}

// Test for >64K fixup.
TEST_F(AssemblerThumb2Test, JumpTableFar) {
  // The jump table. Use three labels.
  Label label1, label2, label3;
  std::vector<Label*> labels({ &label1, &label2, &label3 });

  // A few dummy loads on entry, interspersed with 2 labels.
  constexpr size_t kLdrR0R0Count = 5;
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ BindTrackedLabel(&label1);
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ BindTrackedLabel(&label2);
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }

  // Create the jump table, emit the base load.
  arm::JumpTable* jump_table = __ CreateJumpTable(std::move(labels), arm::R1);

  // Dummy computation, stand-in for the address. We're only testing the jump table here, not how
  // it's being used.
  __ ldr(arm::R0, arm::Address(arm::R0));

  // Emit the jump
  __ EmitJumpTableDispatch(jump_table, arm::R1);

  // Some more dummy instructions.
  for (size_t i = 0; i != kLdrR0R0Count; ++i) {
    __ ldr(arm::R0, arm::Address(arm::R0));
  }
  __ BindTrackedLabel(&label3);
  constexpr size_t kLdrR0R0Count2 = 70001;             // Note: odd so there's no alignment
  for (size_t i = 0; i != kLdrR0R0Count2; ++i) {       //       necessary, as gcc as emits nops,
    __ ldr(arm::R0, arm::Address(arm::R0));            //       whereas we emit 0 != nop.
  }

  static_assert((kLdrR0R0Count + kLdrR0R0Count2 + 3) * 2 > 64 * KB, "Not enough offset");

  std::string expected =
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".L1:\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".L2:\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      // ~ adr r1, .Ljump_table, gcc as can't seem to fix up a large offset itself.
      // (Note: have to use constants, as labels aren't accepted.
      "movw r1, #(((3 + " + StringPrintf("%zu", kLdrR0R0Count + kLdrR0R0Count2) +
          ") * 2 - 4) & 0xFFFF)\n"
      "movt r1, #(((3 + " + StringPrintf("%zu", kLdrR0R0Count + kLdrR0R0Count2) +
          ") * 2 - 4) >> 16)\n"
      ".Lhelp:"
      "add r1, pc\n"
      "ldr r0, [r0]\n"
      ".Lbase:\n"
      "add pc, r1\n" +
      RepeatInsn(kLdrR0R0Count, "ldr r0, [r0]\n") +
      ".L3:\n" +
      RepeatInsn(kLdrR0R0Count2, "ldr r0, [r0]\n") +
      ".align 2\n"
      ".Ljump_table:\n"
      ".4byte (.L1 - .Lbase - 4)\n"
      ".4byte (.L2 - .Lbase - 4)\n"
      ".4byte (.L3 - .Lbase - 4)\n";
  DriverStr(expected, "JumpTableFar");
}

TEST_F(AssemblerThumb2Test, Clz) {
  __ clz(arm::R0, arm::R1);

  const char* expected = "clz r0, r1\n";

  DriverStr(expected, "clz");
}

TEST_F(AssemblerThumb2Test, rbit) {
  __ rbit(arm::R1, arm::R0);

  const char* expected = "rbit r1, r0\n";

  DriverStr(expected, "rbit");
}

TEST_F(AssemblerThumb2Test, rev) {
  __ rev(arm::R1, arm::R0);

  const char* expected = "rev r1, r0\n";

  DriverStr(expected, "rev");
}

TEST_F(AssemblerThumb2Test, rev16) {
  __ rev16(arm::R1, arm::R0);

  const char* expected = "rev16 r1, r0\n";

  DriverStr(expected, "rev16");
}

TEST_F(AssemblerThumb2Test, revsh) {
  __ revsh(arm::R1, arm::R0);

  const char* expected = "revsh r1, r0\n";

  DriverStr(expected, "revsh");
}

}  // namespace art
