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
      "subw r1, r0, #42\n"
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
      "addw r1, r0, #42\n"
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

}  // namespace art
