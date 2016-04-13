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

#include <dirent.h>
#include <errno.h>
#include <fstream>
#include <map>
#include <string.h>
#include <sys/types.h>

#include "gtest/gtest.h"
#include "utils/arm/assembler_thumb2.h"
#include "base/hex_dump.h"
#include "common_runtime_test.h"

namespace art {
namespace arm {

// Include results file (generated manually)
#include "assembler_thumb_test_expected.cc.inc"

#ifndef __ANDROID__
// This controls whether the results are printed to the
// screen or compared against the expected output.
// To generate new expected output, set this to true and
// copy the output into the .cc.inc file in the form
// of the other results.
//
// When this is false, the results are not printed to the
// output, but are compared against the expected results
// in the .cc.inc file.
static constexpr bool kPrintResults = false;
#endif

void SetAndroidData() {
  const char* data = getenv("ANDROID_DATA");
  if (data == nullptr) {
    setenv("ANDROID_DATA", "/tmp", 1);
  }
}

int CompareIgnoringSpace(const char* s1, const char* s2) {
  while (*s1 != '\0') {
    while (isspace(*s1)) ++s1;
    while (isspace(*s2)) ++s2;
    if (*s1 == '\0' || *s1 != *s2) {
      break;
    }
    ++s1;
    ++s2;
  }
  return *s1 - *s2;
}

void InitResults() {
  if (test_results.empty()) {
    setup_results();
  }
}

std::string GetToolsDir() {
#ifndef __ANDROID__
  // This will only work on the host.  There is no as, objcopy or objdump on the device.
  static std::string toolsdir;

  if (toolsdir.empty()) {
    setup_results();
    toolsdir = CommonRuntimeTest::GetAndroidTargetToolsDir(kThumb2);
    SetAndroidData();
  }

  return toolsdir;
#else
  return std::string();
#endif
}

void DumpAndCheck(std::vector<uint8_t>& code, const char* testname, const char* const* results) {
#ifndef __ANDROID__
  static std::string toolsdir = GetToolsDir();

  ScratchFile file;

  const char* filename = file.GetFilename().c_str();

  std::ofstream out(filename);
  if (out) {
    out << ".section \".text\"\n";
    out << ".syntax unified\n";
    out << ".arch armv7-a\n";
    out << ".thumb\n";
    out << ".thumb_func\n";
    out << ".type " << testname << ", #function\n";
    out << ".global " << testname << "\n";
    out << testname << ":\n";
    out << ".fnstart\n";

    for (uint32_t i = 0 ; i < code.size(); ++i) {
      out << ".byte " << (static_cast<int>(code[i]) & 0xff) << "\n";
    }
    out << ".fnend\n";
    out << ".size " << testname << ", .-" << testname << "\n";
  }
  out.close();

  char cmd[1024];

  // Assemble the .S
  snprintf(cmd, sizeof(cmd), "%sas %s -o %s.o", toolsdir.c_str(), filename, filename);
  int cmd_result = system(cmd);
  ASSERT_EQ(cmd_result, 0) << strerror(errno);

  // Remove the $d symbols to prevent the disassembler dumping the instructions
  // as .word
  snprintf(cmd, sizeof(cmd), "%sobjcopy -N '$d' %s.o %s.oo", toolsdir.c_str(), filename, filename);
  int cmd_result2 = system(cmd);
  ASSERT_EQ(cmd_result2, 0) << strerror(errno);

  // Disassemble.

  snprintf(cmd, sizeof(cmd), "%sobjdump -d %s.oo | grep '^  *[0-9a-f][0-9a-f]*:'",
    toolsdir.c_str(), filename);
  if (kPrintResults) {
    // Print the results only, don't check. This is used to generate new output for inserting
    // into the .inc file, so let's add the appropriate prefix/suffix needed in the C++ code.
    strcat(cmd, " | sed '-es/^/  \"/' | sed '-es/$/\\\\n\",/'");
    int cmd_result3 = system(cmd);
    ASSERT_EQ(cmd_result3, 0) << strerror(errno);
  } else {
    // Check the results match the appropriate results in the .inc file.
    FILE *fp = popen(cmd, "r");
    ASSERT_TRUE(fp != nullptr);

    uint32_t lineindex = 0;

    while (!feof(fp)) {
      char testline[256];
      char *s = fgets(testline, sizeof(testline), fp);
      if (s == nullptr) {
        break;
      }
      if (CompareIgnoringSpace(results[lineindex], testline) != 0) {
        LOG(FATAL) << "Output is not as expected at line: " << lineindex
          << results[lineindex] << "/" << testline;
      }
      ++lineindex;
    }
    // Check that we are at the end.
    ASSERT_TRUE(results[lineindex] == nullptr);
    fclose(fp);
  }

  char buf[FILENAME_MAX];
  snprintf(buf, sizeof(buf), "%s.o", filename);
  unlink(buf);

  snprintf(buf, sizeof(buf), "%s.oo", filename);
  unlink(buf);
#endif
}

#define __ assembler->

void EmitAndCheck(arm::Thumb2Assembler* assembler, const char* testname,
                  const char* const* results) {
  __ FinalizeCode();
  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);

  DumpAndCheck(managed_code, testname, results);
}

void EmitAndCheck(arm::Thumb2Assembler* assembler, const char* testname) {
  InitResults();
  std::map<std::string, const char* const*>::iterator results = test_results.find(testname);
  ASSERT_NE(results, test_results.end());

  EmitAndCheck(assembler, testname, results->second);
}

#undef __

class Thumb2AssemblerTest : public ::testing::Test {
 public:
  Thumb2AssemblerTest() : pool(), arena(&pool), assembler(&arena) { }

  ArenaPool pool;
  ArenaAllocator arena;
  arm::Thumb2Assembler assembler;
};

#define __ assembler.

TEST_F(Thumb2AssemblerTest, SimpleMov) {
  __ movs(R0, ShifterOperand(R1));
  __ mov(R0, ShifterOperand(R1));
  __ mov(R8, ShifterOperand(R9));

  __ mov(R0, ShifterOperand(1));
  __ mov(R8, ShifterOperand(9));

  EmitAndCheck(&assembler, "SimpleMov");
}

TEST_F(Thumb2AssemblerTest, SimpleMov32) {
  __ Force32Bit();

  __ mov(R0, ShifterOperand(R1));
  __ mov(R8, ShifterOperand(R9));

  EmitAndCheck(&assembler, "SimpleMov32");
}

TEST_F(Thumb2AssemblerTest, SimpleMovAdd) {
  __ mov(R0, ShifterOperand(R1));
  __ adds(R0, R1, ShifterOperand(R2));
  __ add(R0, R1, ShifterOperand(0));

  EmitAndCheck(&assembler, "SimpleMovAdd");
}

TEST_F(Thumb2AssemblerTest, DataProcessingRegister) {
  // 32 bit variants using low registers.
  __ mvn(R0, ShifterOperand(R1), AL, kCcKeep);
  __ add(R0, R1, ShifterOperand(R2), AL, kCcKeep);
  __ sub(R0, R1, ShifterOperand(R2), AL, kCcKeep);
  __ and_(R0, R1, ShifterOperand(R2), AL, kCcKeep);
  __ orr(R0, R1, ShifterOperand(R2), AL, kCcKeep);
  __ orn(R0, R1, ShifterOperand(R2), AL, kCcKeep);
  __ eor(R0, R1, ShifterOperand(R2), AL, kCcKeep);
  __ bic(R0, R1, ShifterOperand(R2), AL, kCcKeep);
  __ adc(R0, R1, ShifterOperand(R2), AL, kCcKeep);
  __ sbc(R0, R1, ShifterOperand(R2), AL, kCcKeep);
  __ rsb(R0, R1, ShifterOperand(R2), AL, kCcKeep);
  __ teq(R0, ShifterOperand(R1));

  // 16 bit variants using low registers.
  __ movs(R0, ShifterOperand(R1));
  __ mov(R0, ShifterOperand(R1), AL, kCcKeep);
  __ mvns(R0, ShifterOperand(R1));
  __ add(R0, R0, ShifterOperand(R1), AL, kCcKeep);
  __ adds(R0, R1, ShifterOperand(R2));
  __ subs(R0, R1, ShifterOperand(R2));
  __ adcs(R0, R0, ShifterOperand(R1));
  __ sbcs(R0, R0, ShifterOperand(R1));
  __ ands(R0, R0, ShifterOperand(R1));
  __ orrs(R0, R0, ShifterOperand(R1));
  __ eors(R0, R0, ShifterOperand(R1));
  __ bics(R0, R0, ShifterOperand(R1));
  __ tst(R0, ShifterOperand(R1));
  __ cmp(R0, ShifterOperand(R1));
  __ cmn(R0, ShifterOperand(R1));

  // 16-bit variants using high registers.
  __ mov(R1, ShifterOperand(R8), AL, kCcKeep);
  __ mov(R9, ShifterOperand(R0), AL, kCcKeep);
  __ mov(R8, ShifterOperand(R9), AL, kCcKeep);
  __ add(R1, R1, ShifterOperand(R8), AL, kCcKeep);
  __ add(R9, R9, ShifterOperand(R0), AL, kCcKeep);
  __ add(R8, R8, ShifterOperand(R9), AL, kCcKeep);
  __ cmp(R0, ShifterOperand(R9));
  __ cmp(R8, ShifterOperand(R1));
  __ cmp(R9, ShifterOperand(R8));

  // The 16-bit RSBS Rd, Rn, #0, also known as NEGS Rd, Rn is specified using
  // an immediate (0) but emitted without any, so we test it here.
  __ rsbs(R0, R1, ShifterOperand(0));
  __ rsbs(R0, R0, ShifterOperand(0));  // Check Rd == Rn code path.

  // 32 bit variants using high registers that would be 16-bit if using low registers.
  __ movs(R0, ShifterOperand(R8));
  __ mvns(R0, ShifterOperand(R8));
  __ add(R0, R1, ShifterOperand(R8), AL, kCcKeep);
  __ adds(R0, R1, ShifterOperand(R8));
  __ subs(R0, R1, ShifterOperand(R8));
  __ adcs(R0, R0, ShifterOperand(R8));
  __ sbcs(R0, R0, ShifterOperand(R8));
  __ ands(R0, R0, ShifterOperand(R8));
  __ orrs(R0, R0, ShifterOperand(R8));
  __ eors(R0, R0, ShifterOperand(R8));
  __ bics(R0, R0, ShifterOperand(R8));
  __ tst(R0, ShifterOperand(R8));
  __ cmn(R0, ShifterOperand(R8));
  __ rsbs(R0, R8, ShifterOperand(0));  // Check that this is not emitted as 16-bit.
  __ rsbs(R8, R8, ShifterOperand(0));  // Check that this is not emitted as 16-bit (Rd == Rn).

  // 32-bit variants of instructions that would be 16-bit outside IT block.
  __ it(arm::EQ);
  __ mvns(R0, ShifterOperand(R1), arm::EQ);
  __ it(arm::EQ);
  __ adds(R0, R1, ShifterOperand(R2), arm::EQ);
  __ it(arm::EQ);
  __ subs(R0, R1, ShifterOperand(R2), arm::EQ);
  __ it(arm::EQ);
  __ adcs(R0, R0, ShifterOperand(R1), arm::EQ);
  __ it(arm::EQ);
  __ sbcs(R0, R0, ShifterOperand(R1), arm::EQ);
  __ it(arm::EQ);
  __ ands(R0, R0, ShifterOperand(R1), arm::EQ);
  __ it(arm::EQ);
  __ orrs(R0, R0, ShifterOperand(R1), arm::EQ);
  __ it(arm::EQ);
  __ eors(R0, R0, ShifterOperand(R1), arm::EQ);
  __ it(arm::EQ);
  __ bics(R0, R0, ShifterOperand(R1), arm::EQ);

  // 16-bit variants of instructions that would be 32-bit outside IT block.
  __ it(arm::EQ);
  __ mvn(R0, ShifterOperand(R1), arm::EQ, kCcKeep);
  __ it(arm::EQ);
  __ add(R0, R1, ShifterOperand(R2), arm::EQ, kCcKeep);
  __ it(arm::EQ);
  __ sub(R0, R1, ShifterOperand(R2), arm::EQ, kCcKeep);
  __ it(arm::EQ);
  __ adc(R0, R0, ShifterOperand(R1), arm::EQ, kCcKeep);
  __ it(arm::EQ);
  __ sbc(R0, R0, ShifterOperand(R1), arm::EQ, kCcKeep);
  __ it(arm::EQ);
  __ and_(R0, R0, ShifterOperand(R1), arm::EQ, kCcKeep);
  __ it(arm::EQ);
  __ orr(R0, R0, ShifterOperand(R1), arm::EQ, kCcKeep);
  __ it(arm::EQ);
  __ eor(R0, R0, ShifterOperand(R1), arm::EQ, kCcKeep);
  __ it(arm::EQ);
  __ bic(R0, R0, ShifterOperand(R1), arm::EQ, kCcKeep);

  // 16 bit variants selected for the default kCcDontCare.
  __ mov(R0, ShifterOperand(R1));
  __ mvn(R0, ShifterOperand(R1));
  __ add(R0, R0, ShifterOperand(R1));
  __ add(R0, R1, ShifterOperand(R2));
  __ sub(R0, R1, ShifterOperand(R2));
  __ adc(R0, R0, ShifterOperand(R1));
  __ sbc(R0, R0, ShifterOperand(R1));
  __ and_(R0, R0, ShifterOperand(R1));
  __ orr(R0, R0, ShifterOperand(R1));
  __ eor(R0, R0, ShifterOperand(R1));
  __ bic(R0, R0, ShifterOperand(R1));
  __ mov(R1, ShifterOperand(R8));
  __ mov(R9, ShifterOperand(R0));
  __ mov(R8, ShifterOperand(R9));
  __ add(R1, R1, ShifterOperand(R8));
  __ add(R9, R9, ShifterOperand(R0));
  __ add(R8, R8, ShifterOperand(R9));
  __ rsb(R0, R1, ShifterOperand(0));
  __ rsb(R0, R0, ShifterOperand(0));

  // And an arbitrary 32-bit instruction using IP.
  __ add(R12, R1, ShifterOperand(R0), AL, kCcKeep);

  EmitAndCheck(&assembler, "DataProcessingRegister");
}

TEST_F(Thumb2AssemblerTest, DataProcessingImmediate) {
  __ mov(R0, ShifterOperand(0x55));
  __ mvn(R0, ShifterOperand(0x55));
  __ add(R0, R1, ShifterOperand(0x55));
  __ sub(R0, R1, ShifterOperand(0x55));
  __ and_(R0, R1, ShifterOperand(0x55));
  __ orr(R0, R1, ShifterOperand(0x55));
  __ orn(R0, R1, ShifterOperand(0x55));
  __ eor(R0, R1, ShifterOperand(0x55));
  __ bic(R0, R1, ShifterOperand(0x55));
  __ adc(R0, R1, ShifterOperand(0x55));
  __ sbc(R0, R1, ShifterOperand(0x55));
  __ rsb(R0, R1, ShifterOperand(0x55));

  __ tst(R0, ShifterOperand(0x55));
  __ teq(R0, ShifterOperand(0x55));
  __ cmp(R0, ShifterOperand(0x55));
  __ cmn(R0, ShifterOperand(0x55));

  __ add(R0, R1, ShifterOperand(5));
  __ sub(R0, R1, ShifterOperand(5));

  __ movs(R0, ShifterOperand(0x55));
  __ mvns(R0, ShifterOperand(0x55));

  __ adds(R0, R1, ShifterOperand(5));
  __ subs(R0, R1, ShifterOperand(5));

  EmitAndCheck(&assembler, "DataProcessingImmediate");
}

TEST_F(Thumb2AssemblerTest, DataProcessingModifiedImmediate) {
  __ mov(R0, ShifterOperand(0x550055));
  __ mvn(R0, ShifterOperand(0x550055));
  __ add(R0, R1, ShifterOperand(0x550055));
  __ sub(R0, R1, ShifterOperand(0x550055));
  __ and_(R0, R1, ShifterOperand(0x550055));
  __ orr(R0, R1, ShifterOperand(0x550055));
  __ orn(R0, R1, ShifterOperand(0x550055));
  __ eor(R0, R1, ShifterOperand(0x550055));
  __ bic(R0, R1, ShifterOperand(0x550055));
  __ adc(R0, R1, ShifterOperand(0x550055));
  __ sbc(R0, R1, ShifterOperand(0x550055));
  __ rsb(R0, R1, ShifterOperand(0x550055));

  __ tst(R0, ShifterOperand(0x550055));
  __ teq(R0, ShifterOperand(0x550055));
  __ cmp(R0, ShifterOperand(0x550055));
  __ cmn(R0, ShifterOperand(0x550055));

  EmitAndCheck(&assembler, "DataProcessingModifiedImmediate");
}


TEST_F(Thumb2AssemblerTest, DataProcessingModifiedImmediates) {
  __ mov(R0, ShifterOperand(0x550055));
  __ mov(R0, ShifterOperand(0x55005500));
  __ mov(R0, ShifterOperand(0x55555555));
  __ mov(R0, ShifterOperand(0xd5000000));       // rotated to first position
  __ mov(R0, ShifterOperand(0x6a000000));       // rotated to second position
  __ mov(R0, ShifterOperand(0x350));            // rotated to 2nd last position
  __ mov(R0, ShifterOperand(0x1a8));            // rotated to last position

  EmitAndCheck(&assembler, "DataProcessingModifiedImmediates");
}

TEST_F(Thumb2AssemblerTest, DataProcessingShiftedRegister) {
  // 16-bit variants.
  __ movs(R3, ShifterOperand(R4, LSL, 4));
  __ movs(R3, ShifterOperand(R4, LSR, 5));
  __ movs(R3, ShifterOperand(R4, ASR, 6));

  // 32-bit ROR because ROR immediate doesn't have the same 16-bit version as other shifts.
  __ movs(R3, ShifterOperand(R4, ROR, 7));

  // 32-bit RRX because RRX has no 16-bit version.
  __ movs(R3, ShifterOperand(R4, RRX));

  // 32 bit variants (not setting condition codes).
  __ mov(R3, ShifterOperand(R4, LSL, 4), AL, kCcKeep);
  __ mov(R3, ShifterOperand(R4, LSR, 5), AL, kCcKeep);
  __ mov(R3, ShifterOperand(R4, ASR, 6), AL, kCcKeep);
  __ mov(R3, ShifterOperand(R4, ROR, 7), AL, kCcKeep);
  __ mov(R3, ShifterOperand(R4, RRX), AL, kCcKeep);

  // 32 bit variants (high registers).
  __ movs(R8, ShifterOperand(R4, LSL, 4));
  __ movs(R8, ShifterOperand(R4, LSR, 5));
  __ movs(R8, ShifterOperand(R4, ASR, 6));
  __ movs(R8, ShifterOperand(R4, ROR, 7));
  __ movs(R8, ShifterOperand(R4, RRX));

  EmitAndCheck(&assembler, "DataProcessingShiftedRegister");
}

TEST_F(Thumb2AssemblerTest, ShiftImmediate) {
  // Note: This test produces the same results as DataProcessingShiftedRegister
  // but it does so using shift functions instead of mov().

  // 16-bit variants.
  __ Lsl(R3, R4, 4);
  __ Lsr(R3, R4, 5);
  __ Asr(R3, R4, 6);

  // 32-bit ROR because ROR immediate doesn't have the same 16-bit version as other shifts.
  __ Ror(R3, R4, 7);

  // 32-bit RRX because RRX has no 16-bit version.
  __ Rrx(R3, R4);

  // 32 bit variants (not setting condition codes).
  __ Lsl(R3, R4, 4, AL, kCcKeep);
  __ Lsr(R3, R4, 5, AL, kCcKeep);
  __ Asr(R3, R4, 6, AL, kCcKeep);
  __ Ror(R3, R4, 7, AL, kCcKeep);
  __ Rrx(R3, R4, AL, kCcKeep);

  // 32 bit variants (high registers).
  __ Lsls(R8, R4, 4);
  __ Lsrs(R8, R4, 5);
  __ Asrs(R8, R4, 6);
  __ Rors(R8, R4, 7);
  __ Rrxs(R8, R4);

  EmitAndCheck(&assembler, "ShiftImmediate");
}

TEST_F(Thumb2AssemblerTest, BasicLoad) {
  __ ldr(R3, Address(R4, 24));
  __ ldrb(R3, Address(R4, 24));
  __ ldrh(R3, Address(R4, 24));
  __ ldrsb(R3, Address(R4, 24));
  __ ldrsh(R3, Address(R4, 24));

  __ ldr(R3, Address(SP, 24));

  // 32 bit variants
  __ ldr(R8, Address(R4, 24));
  __ ldrb(R8, Address(R4, 24));
  __ ldrh(R8, Address(R4, 24));
  __ ldrsb(R8, Address(R4, 24));
  __ ldrsh(R8, Address(R4, 24));

  EmitAndCheck(&assembler, "BasicLoad");
}


TEST_F(Thumb2AssemblerTest, BasicStore) {
  __ str(R3, Address(R4, 24));
  __ strb(R3, Address(R4, 24));
  __ strh(R3, Address(R4, 24));

  __ str(R3, Address(SP, 24));

  // 32 bit variants.
  __ str(R8, Address(R4, 24));
  __ strb(R8, Address(R4, 24));
  __ strh(R8, Address(R4, 24));

  EmitAndCheck(&assembler, "BasicStore");
}

TEST_F(Thumb2AssemblerTest, ComplexLoad) {
  __ ldr(R3, Address(R4, 24, Address::Mode::Offset));
  __ ldr(R3, Address(R4, 24, Address::Mode::PreIndex));
  __ ldr(R3, Address(R4, 24, Address::Mode::PostIndex));
  __ ldr(R3, Address(R4, 24, Address::Mode::NegOffset));
  __ ldr(R3, Address(R4, 24, Address::Mode::NegPreIndex));
  __ ldr(R3, Address(R4, 24, Address::Mode::NegPostIndex));

  __ ldrb(R3, Address(R4, 24, Address::Mode::Offset));
  __ ldrb(R3, Address(R4, 24, Address::Mode::PreIndex));
  __ ldrb(R3, Address(R4, 24, Address::Mode::PostIndex));
  __ ldrb(R3, Address(R4, 24, Address::Mode::NegOffset));
  __ ldrb(R3, Address(R4, 24, Address::Mode::NegPreIndex));
  __ ldrb(R3, Address(R4, 24, Address::Mode::NegPostIndex));

  __ ldrh(R3, Address(R4, 24, Address::Mode::Offset));
  __ ldrh(R3, Address(R4, 24, Address::Mode::PreIndex));
  __ ldrh(R3, Address(R4, 24, Address::Mode::PostIndex));
  __ ldrh(R3, Address(R4, 24, Address::Mode::NegOffset));
  __ ldrh(R3, Address(R4, 24, Address::Mode::NegPreIndex));
  __ ldrh(R3, Address(R4, 24, Address::Mode::NegPostIndex));

  __ ldrsb(R3, Address(R4, 24, Address::Mode::Offset));
  __ ldrsb(R3, Address(R4, 24, Address::Mode::PreIndex));
  __ ldrsb(R3, Address(R4, 24, Address::Mode::PostIndex));
  __ ldrsb(R3, Address(R4, 24, Address::Mode::NegOffset));
  __ ldrsb(R3, Address(R4, 24, Address::Mode::NegPreIndex));
  __ ldrsb(R3, Address(R4, 24, Address::Mode::NegPostIndex));

  __ ldrsh(R3, Address(R4, 24, Address::Mode::Offset));
  __ ldrsh(R3, Address(R4, 24, Address::Mode::PreIndex));
  __ ldrsh(R3, Address(R4, 24, Address::Mode::PostIndex));
  __ ldrsh(R3, Address(R4, 24, Address::Mode::NegOffset));
  __ ldrsh(R3, Address(R4, 24, Address::Mode::NegPreIndex));
  __ ldrsh(R3, Address(R4, 24, Address::Mode::NegPostIndex));

  EmitAndCheck(&assembler, "ComplexLoad");
}


TEST_F(Thumb2AssemblerTest, ComplexStore) {
  __ str(R3, Address(R4, 24, Address::Mode::Offset));
  __ str(R3, Address(R4, 24, Address::Mode::PreIndex));
  __ str(R3, Address(R4, 24, Address::Mode::PostIndex));
  __ str(R3, Address(R4, 24, Address::Mode::NegOffset));
  __ str(R3, Address(R4, 24, Address::Mode::NegPreIndex));
  __ str(R3, Address(R4, 24, Address::Mode::NegPostIndex));

  __ strb(R3, Address(R4, 24, Address::Mode::Offset));
  __ strb(R3, Address(R4, 24, Address::Mode::PreIndex));
  __ strb(R3, Address(R4, 24, Address::Mode::PostIndex));
  __ strb(R3, Address(R4, 24, Address::Mode::NegOffset));
  __ strb(R3, Address(R4, 24, Address::Mode::NegPreIndex));
  __ strb(R3, Address(R4, 24, Address::Mode::NegPostIndex));

  __ strh(R3, Address(R4, 24, Address::Mode::Offset));
  __ strh(R3, Address(R4, 24, Address::Mode::PreIndex));
  __ strh(R3, Address(R4, 24, Address::Mode::PostIndex));
  __ strh(R3, Address(R4, 24, Address::Mode::NegOffset));
  __ strh(R3, Address(R4, 24, Address::Mode::NegPreIndex));
  __ strh(R3, Address(R4, 24, Address::Mode::NegPostIndex));

  EmitAndCheck(&assembler, "ComplexStore");
}

TEST_F(Thumb2AssemblerTest, NegativeLoadStore) {
  __ ldr(R3, Address(R4, -24, Address::Mode::Offset));
  __ ldr(R3, Address(R4, -24, Address::Mode::PreIndex));
  __ ldr(R3, Address(R4, -24, Address::Mode::PostIndex));
  __ ldr(R3, Address(R4, -24, Address::Mode::NegOffset));
  __ ldr(R3, Address(R4, -24, Address::Mode::NegPreIndex));
  __ ldr(R3, Address(R4, -24, Address::Mode::NegPostIndex));

  __ ldrb(R3, Address(R4, -24, Address::Mode::Offset));
  __ ldrb(R3, Address(R4, -24, Address::Mode::PreIndex));
  __ ldrb(R3, Address(R4, -24, Address::Mode::PostIndex));
  __ ldrb(R3, Address(R4, -24, Address::Mode::NegOffset));
  __ ldrb(R3, Address(R4, -24, Address::Mode::NegPreIndex));
  __ ldrb(R3, Address(R4, -24, Address::Mode::NegPostIndex));

  __ ldrh(R3, Address(R4, -24, Address::Mode::Offset));
  __ ldrh(R3, Address(R4, -24, Address::Mode::PreIndex));
  __ ldrh(R3, Address(R4, -24, Address::Mode::PostIndex));
  __ ldrh(R3, Address(R4, -24, Address::Mode::NegOffset));
  __ ldrh(R3, Address(R4, -24, Address::Mode::NegPreIndex));
  __ ldrh(R3, Address(R4, -24, Address::Mode::NegPostIndex));

  __ ldrsb(R3, Address(R4, -24, Address::Mode::Offset));
  __ ldrsb(R3, Address(R4, -24, Address::Mode::PreIndex));
  __ ldrsb(R3, Address(R4, -24, Address::Mode::PostIndex));
  __ ldrsb(R3, Address(R4, -24, Address::Mode::NegOffset));
  __ ldrsb(R3, Address(R4, -24, Address::Mode::NegPreIndex));
  __ ldrsb(R3, Address(R4, -24, Address::Mode::NegPostIndex));

  __ ldrsh(R3, Address(R4, -24, Address::Mode::Offset));
  __ ldrsh(R3, Address(R4, -24, Address::Mode::PreIndex));
  __ ldrsh(R3, Address(R4, -24, Address::Mode::PostIndex));
  __ ldrsh(R3, Address(R4, -24, Address::Mode::NegOffset));
  __ ldrsh(R3, Address(R4, -24, Address::Mode::NegPreIndex));
  __ ldrsh(R3, Address(R4, -24, Address::Mode::NegPostIndex));

  __ str(R3, Address(R4, -24, Address::Mode::Offset));
  __ str(R3, Address(R4, -24, Address::Mode::PreIndex));
  __ str(R3, Address(R4, -24, Address::Mode::PostIndex));
  __ str(R3, Address(R4, -24, Address::Mode::NegOffset));
  __ str(R3, Address(R4, -24, Address::Mode::NegPreIndex));
  __ str(R3, Address(R4, -24, Address::Mode::NegPostIndex));

  __ strb(R3, Address(R4, -24, Address::Mode::Offset));
  __ strb(R3, Address(R4, -24, Address::Mode::PreIndex));
  __ strb(R3, Address(R4, -24, Address::Mode::PostIndex));
  __ strb(R3, Address(R4, -24, Address::Mode::NegOffset));
  __ strb(R3, Address(R4, -24, Address::Mode::NegPreIndex));
  __ strb(R3, Address(R4, -24, Address::Mode::NegPostIndex));

  __ strh(R3, Address(R4, -24, Address::Mode::Offset));
  __ strh(R3, Address(R4, -24, Address::Mode::PreIndex));
  __ strh(R3, Address(R4, -24, Address::Mode::PostIndex));
  __ strh(R3, Address(R4, -24, Address::Mode::NegOffset));
  __ strh(R3, Address(R4, -24, Address::Mode::NegPreIndex));
  __ strh(R3, Address(R4, -24, Address::Mode::NegPostIndex));

  EmitAndCheck(&assembler, "NegativeLoadStore");
}

TEST_F(Thumb2AssemblerTest, SimpleLoadStoreDual) {
  __ strd(R2, Address(R0, 24, Address::Mode::Offset));
  __ ldrd(R2, Address(R0, 24, Address::Mode::Offset));

  EmitAndCheck(&assembler, "SimpleLoadStoreDual");
}

TEST_F(Thumb2AssemblerTest, ComplexLoadStoreDual) {
  __ strd(R2, Address(R0, 24, Address::Mode::Offset));
  __ strd(R2, Address(R0, 24, Address::Mode::PreIndex));
  __ strd(R2, Address(R0, 24, Address::Mode::PostIndex));
  __ strd(R2, Address(R0, 24, Address::Mode::NegOffset));
  __ strd(R2, Address(R0, 24, Address::Mode::NegPreIndex));
  __ strd(R2, Address(R0, 24, Address::Mode::NegPostIndex));

  __ ldrd(R2, Address(R0, 24, Address::Mode::Offset));
  __ ldrd(R2, Address(R0, 24, Address::Mode::PreIndex));
  __ ldrd(R2, Address(R0, 24, Address::Mode::PostIndex));
  __ ldrd(R2, Address(R0, 24, Address::Mode::NegOffset));
  __ ldrd(R2, Address(R0, 24, Address::Mode::NegPreIndex));
  __ ldrd(R2, Address(R0, 24, Address::Mode::NegPostIndex));

  EmitAndCheck(&assembler, "ComplexLoadStoreDual");
}

TEST_F(Thumb2AssemblerTest, NegativeLoadStoreDual) {
  __ strd(R2, Address(R0, -24, Address::Mode::Offset));
  __ strd(R2, Address(R0, -24, Address::Mode::PreIndex));
  __ strd(R2, Address(R0, -24, Address::Mode::PostIndex));
  __ strd(R2, Address(R0, -24, Address::Mode::NegOffset));
  __ strd(R2, Address(R0, -24, Address::Mode::NegPreIndex));
  __ strd(R2, Address(R0, -24, Address::Mode::NegPostIndex));

  __ ldrd(R2, Address(R0, -24, Address::Mode::Offset));
  __ ldrd(R2, Address(R0, -24, Address::Mode::PreIndex));
  __ ldrd(R2, Address(R0, -24, Address::Mode::PostIndex));
  __ ldrd(R2, Address(R0, -24, Address::Mode::NegOffset));
  __ ldrd(R2, Address(R0, -24, Address::Mode::NegPreIndex));
  __ ldrd(R2, Address(R0, -24, Address::Mode::NegPostIndex));

  EmitAndCheck(&assembler, "NegativeLoadStoreDual");
}

TEST_F(Thumb2AssemblerTest, SimpleBranch) {
  Label l1;
  __ mov(R0, ShifterOperand(2));
  __ Bind(&l1);
  __ mov(R1, ShifterOperand(1));
  __ b(&l1);
  Label l2;
  __ b(&l2);
  __ mov(R1, ShifterOperand(2));
  __ Bind(&l2);
  __ mov(R0, ShifterOperand(3));

  Label l3;
  __ mov(R0, ShifterOperand(2));
  __ Bind(&l3);
  __ mov(R1, ShifterOperand(1));
  __ b(&l3, EQ);

  Label l4;
  __ b(&l4, EQ);
  __ mov(R1, ShifterOperand(2));
  __ Bind(&l4);
  __ mov(R0, ShifterOperand(3));

  // 2 linked labels.
  Label l5;
  __ b(&l5);
  __ mov(R1, ShifterOperand(4));
  __ b(&l5);
  __ mov(R1, ShifterOperand(5));
  __ Bind(&l5);
  __ mov(R0, ShifterOperand(6));

  EmitAndCheck(&assembler, "SimpleBranch");
}

TEST_F(Thumb2AssemblerTest, LongBranch) {
  __ Force32Bit();
  // 32 bit branches.
  Label l1;
  __ mov(R0, ShifterOperand(2));
  __ Bind(&l1);
  __ mov(R1, ShifterOperand(1));
  __ b(&l1);

  Label l2;
  __ b(&l2);
  __ mov(R1, ShifterOperand(2));
  __ Bind(&l2);
  __ mov(R0, ShifterOperand(3));

  Label l3;
  __ mov(R0, ShifterOperand(2));
  __ Bind(&l3);
  __ mov(R1, ShifterOperand(1));
  __ b(&l3, EQ);

  Label l4;
  __ b(&l4, EQ);
  __ mov(R1, ShifterOperand(2));
  __ Bind(&l4);
  __ mov(R0, ShifterOperand(3));

  // 2 linked labels.
  Label l5;
  __ b(&l5);
  __ mov(R1, ShifterOperand(4));
  __ b(&l5);
  __ mov(R1, ShifterOperand(5));
  __ Bind(&l5);
  __ mov(R0, ShifterOperand(6));

  EmitAndCheck(&assembler, "LongBranch");
}

TEST_F(Thumb2AssemblerTest, LoadMultiple) {
  // 16 bit.
  __ ldm(DB_W, R4, (1 << R0 | 1 << R3));

  // 32 bit.
  __ ldm(DB_W, R4, (1 << LR | 1 << R11));
  __ ldm(DB, R4, (1 << LR | 1 << R11));

  // Single reg is converted to ldr
  __ ldm(DB_W, R4, (1 << R5));

  EmitAndCheck(&assembler, "LoadMultiple");
}

TEST_F(Thumb2AssemblerTest, StoreMultiple) {
  // 16 bit.
  __ stm(IA_W, R4, (1 << R0 | 1 << R3));

  // 32 bit.
  __ stm(IA_W, R4, (1 << LR | 1 << R11));
  __ stm(IA, R4, (1 << LR | 1 << R11));

  // Single reg is converted to str
  __ stm(IA_W, R4, (1 << R5));
  __ stm(IA, R4, (1 << R5));

  EmitAndCheck(&assembler, "StoreMultiple");
}

TEST_F(Thumb2AssemblerTest, MovWMovT) {
  // Always 32 bit.
  __ movw(R4, 0);
  __ movw(R4, 0x34);
  __ movw(R9, 0x34);
  __ movw(R3, 0x1234);
  __ movw(R9, 0xffff);

  // Always 32 bit.
  __ movt(R0, 0);
  __ movt(R0, 0x1234);
  __ movt(R1, 0xffff);

  EmitAndCheck(&assembler, "MovWMovT");
}

TEST_F(Thumb2AssemblerTest, SpecialAddSub) {
  __ add(R2, SP, ShifterOperand(0x50));   // 16 bit.
  __ add(SP, SP, ShifterOperand(0x50));   // 16 bit.
  __ add(R8, SP, ShifterOperand(0x50));   // 32 bit.

  __ add(R2, SP, ShifterOperand(0xf00));  // 32 bit due to imm size.
  __ add(SP, SP, ShifterOperand(0xf00));  // 32 bit due to imm size.
  __ add(SP, SP, ShifterOperand(0xffc));  // 32 bit due to imm size; encoding T4.

  __ sub(SP, SP, ShifterOperand(0x50));   // 16 bit
  __ sub(R0, SP, ShifterOperand(0x50));   // 32 bit
  __ sub(R8, SP, ShifterOperand(0x50));   // 32 bit.

  __ sub(SP, SP, ShifterOperand(0xf00));  // 32 bit due to imm size
  __ sub(SP, SP, ShifterOperand(0xffc));  // 32 bit due to imm size; encoding T4.

  EmitAndCheck(&assembler, "SpecialAddSub");
}

TEST_F(Thumb2AssemblerTest, LoadFromOffset) {
  __ LoadFromOffset(kLoadWord, R2, R4, 12);
  __ LoadFromOffset(kLoadWord, R2, R4, 0xfff);
  __ LoadFromOffset(kLoadWord, R2, R4, 0x1000);
  __ LoadFromOffset(kLoadWord, R2, R4, 0x1000a4);
  __ LoadFromOffset(kLoadWord, R2, R4, 0x101000);
  __ LoadFromOffset(kLoadWord, R4, R4, 0x101000);
  __ LoadFromOffset(kLoadUnsignedHalfword, R2, R4, 12);
  __ LoadFromOffset(kLoadUnsignedHalfword, R2, R4, 0xfff);
  __ LoadFromOffset(kLoadUnsignedHalfword, R2, R4, 0x1000);
  __ LoadFromOffset(kLoadUnsignedHalfword, R2, R4, 0x1000a4);
  __ LoadFromOffset(kLoadUnsignedHalfword, R2, R4, 0x101000);
  __ LoadFromOffset(kLoadUnsignedHalfword, R4, R4, 0x101000);
  __ LoadFromOffset(kLoadWordPair, R2, R4, 12);
  __ LoadFromOffset(kLoadWordPair, R2, R4, 0x3fc);
  __ LoadFromOffset(kLoadWordPair, R2, R4, 0x400);
  __ LoadFromOffset(kLoadWordPair, R2, R4, 0x400a4);
  __ LoadFromOffset(kLoadWordPair, R2, R4, 0x40400);
  __ LoadFromOffset(kLoadWordPair, R4, R4, 0x40400);

  __ LoadFromOffset(kLoadWord, R0, R12, 12);  // 32-bit because of R12.
  __ LoadFromOffset(kLoadWord, R2, R4, 0xa4 - 0x100000);

  __ LoadFromOffset(kLoadSignedByte, R2, R4, 12);
  __ LoadFromOffset(kLoadUnsignedByte, R2, R4, 12);
  __ LoadFromOffset(kLoadSignedHalfword, R2, R4, 12);

  EmitAndCheck(&assembler, "LoadFromOffset");
}

TEST_F(Thumb2AssemblerTest, StoreToOffset) {
  __ StoreToOffset(kStoreWord, R2, R4, 12);
  __ StoreToOffset(kStoreWord, R2, R4, 0xfff);
  __ StoreToOffset(kStoreWord, R2, R4, 0x1000);
  __ StoreToOffset(kStoreWord, R2, R4, 0x1000a4);
  __ StoreToOffset(kStoreWord, R2, R4, 0x101000);
  __ StoreToOffset(kStoreWord, R4, R4, 0x101000);
  __ StoreToOffset(kStoreHalfword, R2, R4, 12);
  __ StoreToOffset(kStoreHalfword, R2, R4, 0xfff);
  __ StoreToOffset(kStoreHalfword, R2, R4, 0x1000);
  __ StoreToOffset(kStoreHalfword, R2, R4, 0x1000a4);
  __ StoreToOffset(kStoreHalfword, R2, R4, 0x101000);
  __ StoreToOffset(kStoreHalfword, R4, R4, 0x101000);
  __ StoreToOffset(kStoreWordPair, R2, R4, 12);
  __ StoreToOffset(kStoreWordPair, R2, R4, 0x3fc);
  __ StoreToOffset(kStoreWordPair, R2, R4, 0x400);
  __ StoreToOffset(kStoreWordPair, R2, R4, 0x400a4);
  __ StoreToOffset(kStoreWordPair, R2, R4, 0x40400);
  __ StoreToOffset(kStoreWordPair, R4, R4, 0x40400);

  __ StoreToOffset(kStoreWord, R0, R12, 12);  // 32-bit because of R12.
  __ StoreToOffset(kStoreWord, R2, R4, 0xa4 - 0x100000);

  __ StoreToOffset(kStoreByte, R2, R4, 12);

  EmitAndCheck(&assembler, "StoreToOffset");
}

TEST_F(Thumb2AssemblerTest, IfThen) {
  __ it(EQ);
  __ mov(R1, ShifterOperand(1), EQ);

  __ it(EQ, kItThen);
  __ mov(R1, ShifterOperand(1), EQ);
  __ mov(R2, ShifterOperand(2), EQ);

  __ it(EQ, kItElse);
  __ mov(R1, ShifterOperand(1), EQ);
  __ mov(R2, ShifterOperand(2), NE);

  __ it(EQ, kItThen, kItElse);
  __ mov(R1, ShifterOperand(1), EQ);
  __ mov(R2, ShifterOperand(2), EQ);
  __ mov(R3, ShifterOperand(3), NE);

  __ it(EQ, kItElse, kItElse);
  __ mov(R1, ShifterOperand(1), EQ);
  __ mov(R2, ShifterOperand(2), NE);
  __ mov(R3, ShifterOperand(3), NE);

  __ it(EQ, kItThen, kItThen, kItElse);
  __ mov(R1, ShifterOperand(1), EQ);
  __ mov(R2, ShifterOperand(2), EQ);
  __ mov(R3, ShifterOperand(3), EQ);
  __ mov(R4, ShifterOperand(4), NE);

  EmitAndCheck(&assembler, "IfThen");
}

TEST_F(Thumb2AssemblerTest, CbzCbnz) {
  Label l1;
  __ cbz(R2, &l1);
  __ mov(R1, ShifterOperand(3));
  __ mov(R2, ShifterOperand(3));
  __ Bind(&l1);
  __ mov(R2, ShifterOperand(4));

  Label l2;
  __ cbnz(R2, &l2);
  __ mov(R8, ShifterOperand(3));
  __ mov(R2, ShifterOperand(3));
  __ Bind(&l2);
  __ mov(R2, ShifterOperand(4));

  EmitAndCheck(&assembler, "CbzCbnz");
}

TEST_F(Thumb2AssemblerTest, Multiply) {
  __ mul(R0, R1, R0);
  __ mul(R0, R1, R2);
  __ mul(R8, R9, R8);
  __ mul(R8, R9, R10);

  __ mla(R0, R1, R2, R3);
  __ mla(R8, R9, R8, R9);

  __ mls(R0, R1, R2, R3);
  __ mls(R8, R9, R8, R9);

  __ umull(R0, R1, R2, R3);
  __ umull(R8, R9, R10, R11);

  EmitAndCheck(&assembler, "Multiply");
}

TEST_F(Thumb2AssemblerTest, Divide) {
  __ sdiv(R0, R1, R2);
  __ sdiv(R8, R9, R10);

  __ udiv(R0, R1, R2);
  __ udiv(R8, R9, R10);

  EmitAndCheck(&assembler, "Divide");
}

TEST_F(Thumb2AssemblerTest, VMov) {
  __ vmovs(S1, 1.0);
  __ vmovd(D1, 1.0);

  __ vmovs(S1, S2);
  __ vmovd(D1, D2);

  EmitAndCheck(&assembler, "VMov");
}


TEST_F(Thumb2AssemblerTest, BasicFloatingPoint) {
  __ vadds(S0, S1, S2);
  __ vsubs(S0, S1, S2);
  __ vmuls(S0, S1, S2);
  __ vmlas(S0, S1, S2);
  __ vmlss(S0, S1, S2);
  __ vdivs(S0, S1, S2);
  __ vabss(S0, S1);
  __ vnegs(S0, S1);
  __ vsqrts(S0, S1);

  __ vaddd(D0, D1, D2);
  __ vsubd(D0, D1, D2);
  __ vmuld(D0, D1, D2);
  __ vmlad(D0, D1, D2);
  __ vmlsd(D0, D1, D2);
  __ vdivd(D0, D1, D2);
  __ vabsd(D0, D1);
  __ vnegd(D0, D1);
  __ vsqrtd(D0, D1);

  EmitAndCheck(&assembler, "BasicFloatingPoint");
}

TEST_F(Thumb2AssemblerTest, FloatingPointConversions) {
  __ vcvtsd(S2, D2);
  __ vcvtds(D2, S2);

  __ vcvtis(S1, S2);
  __ vcvtsi(S1, S2);

  __ vcvtid(S1, D2);
  __ vcvtdi(D1, S2);

  __ vcvtus(S1, S2);
  __ vcvtsu(S1, S2);

  __ vcvtud(S1, D2);
  __ vcvtdu(D1, S2);

  EmitAndCheck(&assembler, "FloatingPointConversions");
}

TEST_F(Thumb2AssemblerTest, FloatingPointComparisons) {
  __ vcmps(S0, S1);
  __ vcmpd(D0, D1);

  __ vcmpsz(S2);
  __ vcmpdz(D2);

  EmitAndCheck(&assembler, "FloatingPointComparisons");
}

TEST_F(Thumb2AssemblerTest, Calls) {
  __ blx(LR);
  __ bx(LR);

  EmitAndCheck(&assembler, "Calls");
}

TEST_F(Thumb2AssemblerTest, Breakpoint) {
  __ bkpt(0);

  EmitAndCheck(&assembler, "Breakpoint");
}

TEST_F(Thumb2AssemblerTest, StrR1) {
  __ str(R1, Address(SP, 68));
  __ str(R1, Address(SP, 1068));

  EmitAndCheck(&assembler, "StrR1");
}

TEST_F(Thumb2AssemblerTest, VPushPop) {
  __ vpushs(S2, 4);
  __ vpushd(D2, 4);

  __ vpops(S2, 4);
  __ vpopd(D2, 4);

  EmitAndCheck(&assembler, "VPushPop");
}

TEST_F(Thumb2AssemblerTest, Max16BitBranch) {
  Label l1;
  __ b(&l1);
  for (int i = 0 ; i < (1 << 11) ; i += 2) {
    __ mov(R3, ShifterOperand(i & 0xff));
  }
  __ Bind(&l1);
  __ mov(R1, ShifterOperand(R2));

  EmitAndCheck(&assembler, "Max16BitBranch");
}

TEST_F(Thumb2AssemblerTest, Branch32) {
  Label l1;
  __ b(&l1);
  for (int i = 0 ; i < (1 << 11) + 2 ; i += 2) {
    __ mov(R3, ShifterOperand(i & 0xff));
  }
  __ Bind(&l1);
  __ mov(R1, ShifterOperand(R2));

  EmitAndCheck(&assembler, "Branch32");
}

TEST_F(Thumb2AssemblerTest, CompareAndBranchMax) {
  Label l1;
  __ cbz(R4, &l1);
  for (int i = 0 ; i < (1 << 7) ; i += 2) {
    __ mov(R3, ShifterOperand(i & 0xff));
  }
  __ Bind(&l1);
  __ mov(R1, ShifterOperand(R2));

  EmitAndCheck(&assembler, "CompareAndBranchMax");
}

TEST_F(Thumb2AssemblerTest, CompareAndBranchRelocation16) {
  Label l1;
  __ cbz(R4, &l1);
  for (int i = 0 ; i < (1 << 7) + 2 ; i += 2) {
    __ mov(R3, ShifterOperand(i & 0xff));
  }
  __ Bind(&l1);
  __ mov(R1, ShifterOperand(R2));

  EmitAndCheck(&assembler, "CompareAndBranchRelocation16");
}

TEST_F(Thumb2AssemblerTest, CompareAndBranchRelocation32) {
  Label l1;
  __ cbz(R4, &l1);
  for (int i = 0 ; i < (1 << 11) + 2 ; i += 2) {
    __ mov(R3, ShifterOperand(i & 0xff));
  }
  __ Bind(&l1);
  __ mov(R1, ShifterOperand(R2));

  EmitAndCheck(&assembler, "CompareAndBranchRelocation32");
}

TEST_F(Thumb2AssemblerTest, MixedBranch32) {
  Label l1;
  Label l2;
  __ b(&l1);      // Forwards.
  __ Bind(&l2);

  // Space to force relocation.
  for (int i = 0 ; i < (1 << 11) + 2 ; i += 2) {
    __ mov(R3, ShifterOperand(i & 0xff));
  }
  __ b(&l2);      // Backwards.
  __ Bind(&l1);
  __ mov(R1, ShifterOperand(R2));

  EmitAndCheck(&assembler, "MixedBranch32");
}

TEST_F(Thumb2AssemblerTest, Shifts) {
  // 16 bit selected for CcDontCare.
  __ Lsl(R0, R1, 5);
  __ Lsr(R0, R1, 5);
  __ Asr(R0, R1, 5);

  __ Lsl(R0, R0, R1);
  __ Lsr(R0, R0, R1);
  __ Asr(R0, R0, R1);
  __ Ror(R0, R0, R1);

  // 16 bit with kCcSet.
  __ Lsls(R0, R1, 5);
  __ Lsrs(R0, R1, 5);
  __ Asrs(R0, R1, 5);

  __ Lsls(R0, R0, R1);
  __ Lsrs(R0, R0, R1);
  __ Asrs(R0, R0, R1);
  __ Rors(R0, R0, R1);

  // 32-bit with kCcKeep.
  __ Lsl(R0, R1, 5, AL, kCcKeep);
  __ Lsr(R0, R1, 5, AL, kCcKeep);
  __ Asr(R0, R1, 5, AL, kCcKeep);

  __ Lsl(R0, R0, R1, AL, kCcKeep);
  __ Lsr(R0, R0, R1, AL, kCcKeep);
  __ Asr(R0, R0, R1, AL, kCcKeep);
  __ Ror(R0, R0, R1, AL, kCcKeep);

  // 32-bit because ROR immediate doesn't have a 16-bit version like the other shifts.
  __ Ror(R0, R1, 5);
  __ Rors(R0, R1, 5);
  __ Ror(R0, R1, 5, AL, kCcKeep);

  // 32 bit due to high registers.
  __ Lsl(R8, R1, 5);
  __ Lsr(R0, R8, 5);
  __ Asr(R8, R1, 5);
  __ Ror(R0, R8, 5);

  // 32 bit due to different Rd and Rn.
  __ Lsl(R0, R1, R2);
  __ Lsr(R0, R1, R2);
  __ Asr(R0, R1, R2);
  __ Ror(R0, R1, R2);

  // 32 bit due to use of high registers.
  __ Lsl(R8, R1, R2);
  __ Lsr(R0, R8, R2);
  __ Asr(R0, R1, R8);

  // S bit (all 32 bit)

  // 32 bit due to high registers.
  __ Lsls(R8, R1, 5);
  __ Lsrs(R0, R8, 5);
  __ Asrs(R8, R1, 5);
  __ Rors(R0, R8, 5);

  // 32 bit due to different Rd and Rn.
  __ Lsls(R0, R1, R2);
  __ Lsrs(R0, R1, R2);
  __ Asrs(R0, R1, R2);
  __ Rors(R0, R1, R2);

  // 32 bit due to use of high registers.
  __ Lsls(R8, R1, R2);
  __ Lsrs(R0, R8, R2);
  __ Asrs(R0, R1, R8);

  EmitAndCheck(&assembler, "Shifts");
}

TEST_F(Thumb2AssemblerTest, LoadStoreRegOffset) {
  // 16 bit.
  __ ldr(R0, Address(R1, R2));
  __ str(R0, Address(R1, R2));

  // 32 bit due to shift.
  __ ldr(R0, Address(R1, R2, LSL, 1));
  __ str(R0, Address(R1, R2, LSL, 1));

  __ ldr(R0, Address(R1, R2, LSL, 3));
  __ str(R0, Address(R1, R2, LSL, 3));

  // 32 bit due to high register use.
  __ ldr(R8, Address(R1, R2));
  __ str(R8, Address(R1, R2));

  __ ldr(R1, Address(R8, R2));
  __ str(R2, Address(R8, R2));

  __ ldr(R0, Address(R1, R8));
  __ str(R0, Address(R1, R8));

  EmitAndCheck(&assembler, "LoadStoreRegOffset");
}

TEST_F(Thumb2AssemblerTest, LoadStoreLiteral) {
  __ ldr(R0, Address(4));
  __ str(R0, Address(4));

  __ ldr(R0, Address(-8));
  __ str(R0, Address(-8));

  // Limits.
  __ ldr(R0, Address(0x3ff));       // 10 bits (16 bit).
  __ ldr(R0, Address(0x7ff));       // 11 bits (32 bit).
  __ str(R0, Address(0x3ff));       // 32 bit (no 16 bit str(literal)).
  __ str(R0, Address(0x7ff));       // 11 bits (32 bit).

  EmitAndCheck(&assembler, "LoadStoreLiteral");
}

TEST_F(Thumb2AssemblerTest, LoadStoreLimits) {
  __ ldr(R0, Address(R4, 124));     // 16 bit.
  __ ldr(R0, Address(R4, 128));     // 32 bit.

  __ ldrb(R0, Address(R4, 31));     // 16 bit.
  __ ldrb(R0, Address(R4, 32));     // 32 bit.

  __ ldrh(R0, Address(R4, 62));     // 16 bit.
  __ ldrh(R0, Address(R4, 64));     // 32 bit.

  __ ldrsb(R0, Address(R4, 31));     // 32 bit.
  __ ldrsb(R0, Address(R4, 32));     // 32 bit.

  __ ldrsh(R0, Address(R4, 62));     // 32 bit.
  __ ldrsh(R0, Address(R4, 64));     // 32 bit.

  __ str(R0, Address(R4, 124));     // 16 bit.
  __ str(R0, Address(R4, 128));     // 32 bit.

  __ strb(R0, Address(R4, 31));     // 16 bit.
  __ strb(R0, Address(R4, 32));     // 32 bit.

  __ strh(R0, Address(R4, 62));     // 16 bit.
  __ strh(R0, Address(R4, 64));     // 32 bit.

  EmitAndCheck(&assembler, "LoadStoreLimits");
}

TEST_F(Thumb2AssemblerTest, CompareAndBranch) {
  Label label;
  __ CompareAndBranchIfZero(arm::R0, &label);
  __ CompareAndBranchIfZero(arm::R11, &label);
  __ CompareAndBranchIfNonZero(arm::R0, &label);
  __ CompareAndBranchIfNonZero(arm::R11, &label);
  __ Bind(&label);

  EmitAndCheck(&assembler, "CompareAndBranch");
}

TEST_F(Thumb2AssemblerTest, AddConstant) {
  // Low registers, Rd != Rn.
  __ AddConstant(R0, R1, 0);                          // MOV.
  __ AddConstant(R0, R1, 1);                          // 16-bit ADDS, encoding T1.
  __ AddConstant(R0, R1, 7);                          // 16-bit ADDS, encoding T1.
  __ AddConstant(R0, R1, 8);                          // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R1, 255);                        // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R1, 256);                        // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R1, 257);                        // 32-bit ADD, encoding T4.
  __ AddConstant(R0, R1, 0xfff);                      // 32-bit ADD, encoding T4.
  __ AddConstant(R0, R1, 0x1000);                     // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R1, 0x1001);                     // MVN+SUB.
  __ AddConstant(R0, R1, 0x1002);                     // MOVW+ADD.
  __ AddConstant(R0, R1, 0xffff);                     // MOVW+ADD.
  __ AddConstant(R0, R1, 0x10000);                    // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R1, 0x10001);                    // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R1, 0x10002);                    // MVN+SUB.
  __ AddConstant(R0, R1, 0x10003);                    // MOVW+MOVT+ADD.
  __ AddConstant(R0, R1, -1);                         // 16-bit SUBS.
  __ AddConstant(R0, R1, -7);                         // 16-bit SUBS.
  __ AddConstant(R0, R1, -8);                         // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R1, -255);                       // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R1, -256);                       // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R1, -257);                       // 32-bit SUB, encoding T4.
  __ AddConstant(R0, R1, -0xfff);                     // 32-bit SUB, encoding T4.
  __ AddConstant(R0, R1, -0x1000);                    // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R1, -0x1001);                    // MVN+ADD.
  __ AddConstant(R0, R1, -0x1002);                    // MOVW+SUB.
  __ AddConstant(R0, R1, -0xffff);                    // MOVW+SUB.
  __ AddConstant(R0, R1, -0x10000);                   // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R1, -0x10001);                   // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R1, -0x10002);                   // MVN+ADD.
  __ AddConstant(R0, R1, -0x10003);                   // MOVW+MOVT+ADD.

  // Low registers, Rd == Rn.
  __ AddConstant(R0, R0, 0);                          // Nothing.
  __ AddConstant(R1, R1, 1);                          // 16-bit ADDS, encoding T2,
  __ AddConstant(R0, R0, 7);                          // 16-bit ADDS, encoding T2.
  __ AddConstant(R1, R1, 8);                          // 16-bit ADDS, encoding T2.
  __ AddConstant(R0, R0, 255);                        // 16-bit ADDS, encoding T2.
  __ AddConstant(R1, R1, 256);                        // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R0, 257);                        // 32-bit ADD, encoding T4.
  __ AddConstant(R1, R1, 0xfff);                      // 32-bit ADD, encoding T4.
  __ AddConstant(R0, R0, 0x1000);                     // 32-bit ADD, encoding T3.
  __ AddConstant(R1, R1, 0x1001);                     // MVN+SUB.
  __ AddConstant(R0, R0, 0x1002);                     // MOVW+ADD.
  __ AddConstant(R1, R1, 0xffff);                     // MOVW+ADD.
  __ AddConstant(R0, R0, 0x10000);                    // 32-bit ADD, encoding T3.
  __ AddConstant(R1, R1, 0x10001);                    // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R0, 0x10002);                    // MVN+SUB.
  __ AddConstant(R1, R1, 0x10003);                    // MOVW+MOVT+ADD.
  __ AddConstant(R0, R0, -1);                         // 16-bit SUBS, encoding T2.
  __ AddConstant(R1, R1, -7);                         // 16-bit SUBS, encoding T2.
  __ AddConstant(R0, R0, -8);                         // 16-bit SUBS, encoding T2.
  __ AddConstant(R1, R1, -255);                       // 16-bit SUBS, encoding T2.
  __ AddConstant(R0, R0, -256);                       // 32-bit SUB, encoding T3.
  __ AddConstant(R1, R1, -257);                       // 32-bit SUB, encoding T4.
  __ AddConstant(R0, R0, -0xfff);                     // 32-bit SUB, encoding T4.
  __ AddConstant(R1, R1, -0x1000);                    // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R0, -0x1001);                    // MVN+ADD.
  __ AddConstant(R1, R1, -0x1002);                    // MOVW+SUB.
  __ AddConstant(R0, R0, -0xffff);                    // MOVW+SUB.
  __ AddConstant(R1, R1, -0x10000);                   // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R0, -0x10001);                   // 32-bit SUB, encoding T3.
  __ AddConstant(R1, R1, -0x10002);                   // MVN+ADD.
  __ AddConstant(R0, R0, -0x10003);                   // MOVW+MOVT+ADD.

  // High registers.
  __ AddConstant(R8, R8, 0);                          // Nothing.
  __ AddConstant(R8, R1, 1);                          // 32-bit ADD, encoding T3,
  __ AddConstant(R0, R8, 7);                          // 32-bit ADD, encoding T3.
  __ AddConstant(R8, R8, 8);                          // 32-bit ADD, encoding T3.
  __ AddConstant(R8, R1, 255);                        // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R8, 256);                        // 32-bit ADD, encoding T3.
  __ AddConstant(R8, R8, 257);                        // 32-bit ADD, encoding T4.
  __ AddConstant(R8, R1, 0xfff);                      // 32-bit ADD, encoding T4.
  __ AddConstant(R0, R8, 0x1000);                     // 32-bit ADD, encoding T3.
  __ AddConstant(R8, R8, 0x1001);                     // MVN+SUB.
  __ AddConstant(R0, R1, 0x1002);                     // MOVW+ADD.
  __ AddConstant(R0, R8, 0xffff);                     // MOVW+ADD.
  __ AddConstant(R8, R8, 0x10000);                    // 32-bit ADD, encoding T3.
  __ AddConstant(R8, R1, 0x10001);                    // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R8, 0x10002);                    // MVN+SUB.
  __ AddConstant(R0, R8, 0x10003);                    // MOVW+MOVT+ADD.
  __ AddConstant(R8, R8, -1);                         // 32-bit ADD, encoding T3.
  __ AddConstant(R8, R1, -7);                         // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R8, -8);                         // 32-bit SUB, encoding T3.
  __ AddConstant(R8, R8, -255);                       // 32-bit SUB, encoding T3.
  __ AddConstant(R8, R1, -256);                       // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R8, -257);                       // 32-bit SUB, encoding T4.
  __ AddConstant(R8, R8, -0xfff);                     // 32-bit SUB, encoding T4.
  __ AddConstant(R8, R1, -0x1000);                    // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R8, -0x1001);                    // MVN+ADD.
  __ AddConstant(R0, R1, -0x1002);                    // MOVW+SUB.
  __ AddConstant(R8, R1, -0xffff);                    // MOVW+SUB.
  __ AddConstant(R0, R8, -0x10000);                   // 32-bit SUB, encoding T3.
  __ AddConstant(R8, R8, -0x10001);                   // 32-bit SUB, encoding T3.
  __ AddConstant(R8, R1, -0x10002);                   // MVN+SUB.
  __ AddConstant(R0, R8, -0x10003);                   // MOVW+MOVT+ADD.

  // Low registers, Rd != Rn, kCcKeep.
  __ AddConstant(R0, R1, 0, AL, kCcKeep);             // MOV.
  __ AddConstant(R0, R1, 1, AL, kCcKeep);             // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R1, 7, AL, kCcKeep);             // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R1, 8, AL, kCcKeep);             // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R1, 255, AL, kCcKeep);           // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R1, 256, AL, kCcKeep);           // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R1, 257, AL, kCcKeep);           // 32-bit ADD, encoding T4.
  __ AddConstant(R0, R1, 0xfff, AL, kCcKeep);         // 32-bit ADD, encoding T4.
  __ AddConstant(R0, R1, 0x1000, AL, kCcKeep);        // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R1, 0x1001, AL, kCcKeep);        // MVN+SUB.
  __ AddConstant(R0, R1, 0x1002, AL, kCcKeep);        // MOVW+ADD.
  __ AddConstant(R0, R1, 0xffff, AL, kCcKeep);        // MOVW+ADD.
  __ AddConstant(R0, R1, 0x10000, AL, kCcKeep);       // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R1, 0x10001, AL, kCcKeep);       // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R1, 0x10002, AL, kCcKeep);       // MVN+SUB.
  __ AddConstant(R0, R1, 0x10003, AL, kCcKeep);       // MOVW+MOVT+ADD.
  __ AddConstant(R0, R1, -1, AL, kCcKeep);            // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R1, -7, AL, kCcKeep);            // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R1, -8, AL, kCcKeep);            // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R1, -255, AL, kCcKeep);          // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R1, -256, AL, kCcKeep);          // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R1, -257, AL, kCcKeep);          // 32-bit SUB, encoding T4.
  __ AddConstant(R0, R1, -0xfff, AL, kCcKeep);        // 32-bit SUB, encoding T4.
  __ AddConstant(R0, R1, -0x1000, AL, kCcKeep);       // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R1, -0x1001, AL, kCcKeep);       // MVN+ADD.
  __ AddConstant(R0, R1, -0x1002, AL, kCcKeep);       // MOVW+SUB.
  __ AddConstant(R0, R1, -0xffff, AL, kCcKeep);       // MOVW+SUB.
  __ AddConstant(R0, R1, -0x10000, AL, kCcKeep);      // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R1, -0x10001, AL, kCcKeep);      // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R1, -0x10002, AL, kCcKeep);      // MVN+ADD.
  __ AddConstant(R0, R1, -0x10003, AL, kCcKeep);      // MOVW+MOVT+ADD.

  // Low registers, Rd == Rn, kCcKeep.
  __ AddConstant(R0, R0, 0, AL, kCcKeep);             // Nothing.
  __ AddConstant(R1, R1, 1, AL, kCcKeep);             // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R0, 7, AL, kCcKeep);             // 32-bit ADD, encoding T3.
  __ AddConstant(R1, R1, 8, AL, kCcKeep);             // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R0, 255, AL, kCcKeep);           // 32-bit ADD, encoding T3.
  __ AddConstant(R1, R1, 256, AL, kCcKeep);           // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R0, 257, AL, kCcKeep);           // 32-bit ADD, encoding T4.
  __ AddConstant(R1, R1, 0xfff, AL, kCcKeep);         // 32-bit ADD, encoding T4.
  __ AddConstant(R0, R0, 0x1000, AL, kCcKeep);        // 32-bit ADD, encoding T3.
  __ AddConstant(R1, R1, 0x1001, AL, kCcKeep);        // MVN+SUB.
  __ AddConstant(R0, R0, 0x1002, AL, kCcKeep);        // MOVW+ADD.
  __ AddConstant(R1, R1, 0xffff, AL, kCcKeep);        // MOVW+ADD.
  __ AddConstant(R0, R0, 0x10000, AL, kCcKeep);       // 32-bit ADD, encoding T3.
  __ AddConstant(R1, R1, 0x10001, AL, kCcKeep);       // 32-bit ADD, encoding T3.
  __ AddConstant(R0, R0, 0x10002, AL, kCcKeep);       // MVN+SUB.
  __ AddConstant(R1, R1, 0x10003, AL, kCcKeep);       // MOVW+MOVT+ADD.
  __ AddConstant(R0, R0, -1, AL, kCcKeep);            // 32-bit ADD, encoding T3.
  __ AddConstant(R1, R1, -7, AL, kCcKeep);            // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R0, -8, AL, kCcKeep);            // 32-bit SUB, encoding T3.
  __ AddConstant(R1, R1, -255, AL, kCcKeep);          // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R0, -256, AL, kCcKeep);          // 32-bit SUB, encoding T3.
  __ AddConstant(R1, R1, -257, AL, kCcKeep);          // 32-bit SUB, encoding T4.
  __ AddConstant(R0, R0, -0xfff, AL, kCcKeep);        // 32-bit SUB, encoding T4.
  __ AddConstant(R1, R1, -0x1000, AL, kCcKeep);       // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R0, -0x1001, AL, kCcKeep);       // MVN+ADD.
  __ AddConstant(R1, R1, -0x1002, AL, kCcKeep);       // MOVW+SUB.
  __ AddConstant(R0, R0, -0xffff, AL, kCcKeep);       // MOVW+SUB.
  __ AddConstant(R1, R1, -0x10000, AL, kCcKeep);      // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R0, -0x10001, AL, kCcKeep);      // 32-bit SUB, encoding T3.
  __ AddConstant(R1, R1, -0x10002, AL, kCcKeep);      // MVN+ADD.
  __ AddConstant(R0, R0, -0x10003, AL, kCcKeep);      // MOVW+MOVT+ADD.

  // Low registers, Rd != Rn, kCcSet.
  __ AddConstant(R0, R1, 0, AL, kCcSet);              // 16-bit ADDS.
  __ AddConstant(R0, R1, 1, AL, kCcSet);              // 16-bit ADDS.
  __ AddConstant(R0, R1, 7, AL, kCcSet);              // 16-bit ADDS.
  __ AddConstant(R0, R1, 8, AL, kCcSet);              // 32-bit ADDS, encoding T3.
  __ AddConstant(R0, R1, 255, AL, kCcSet);            // 32-bit ADDS, encoding T3.
  __ AddConstant(R0, R1, 256, AL, kCcSet);            // 32-bit ADDS, encoding T3.
  __ AddConstant(R0, R1, 257, AL, kCcSet);            // MVN+SUBS.
  __ AddConstant(R0, R1, 0xfff, AL, kCcSet);          // MOVW+ADDS.
  __ AddConstant(R0, R1, 0x1000, AL, kCcSet);         // 32-bit ADDS, encoding T3.
  __ AddConstant(R0, R1, 0x1001, AL, kCcSet);         // MVN+SUBS.
  __ AddConstant(R0, R1, 0x1002, AL, kCcSet);         // MOVW+ADDS.
  __ AddConstant(R0, R1, 0xffff, AL, kCcSet);         // MOVW+ADDS.
  __ AddConstant(R0, R1, 0x10000, AL, kCcSet);        // 32-bit ADDS, encoding T3.
  __ AddConstant(R0, R1, 0x10001, AL, kCcSet);        // 32-bit ADDS, encoding T3.
  __ AddConstant(R0, R1, 0x10002, AL, kCcSet);        // MVN+SUBS.
  __ AddConstant(R0, R1, 0x10003, AL, kCcSet);        // MOVW+MOVT+ADDS.
  __ AddConstant(R0, R1, -1, AL, kCcSet);             // 16-bit SUBS.
  __ AddConstant(R0, R1, -7, AL, kCcSet);             // 16-bit SUBS.
  __ AddConstant(R0, R1, -8, AL, kCcSet);             // 32-bit SUBS, encoding T3.
  __ AddConstant(R0, R1, -255, AL, kCcSet);           // 32-bit SUBS, encoding T3.
  __ AddConstant(R0, R1, -256, AL, kCcSet);           // 32-bit SUBS, encoding T3.
  __ AddConstant(R0, R1, -257, AL, kCcSet);           // MVN+ADDS.
  __ AddConstant(R0, R1, -0xfff, AL, kCcSet);         // MOVW+SUBS.
  __ AddConstant(R0, R1, -0x1000, AL, kCcSet);        // 32-bit SUBS, encoding T3.
  __ AddConstant(R0, R1, -0x1001, AL, kCcSet);        // MVN+ADDS.
  __ AddConstant(R0, R1, -0x1002, AL, kCcSet);        // MOVW+SUBS.
  __ AddConstant(R0, R1, -0xffff, AL, kCcSet);        // MOVW+SUBS.
  __ AddConstant(R0, R1, -0x10000, AL, kCcSet);       // 32-bit SUBS, encoding T3.
  __ AddConstant(R0, R1, -0x10001, AL, kCcSet);       // 32-bit SUBS, encoding T3.
  __ AddConstant(R0, R1, -0x10002, AL, kCcSet);       // MVN+ADDS.
  __ AddConstant(R0, R1, -0x10003, AL, kCcSet);       // MOVW+MOVT+ADDS.

  // Low registers, Rd == Rn, kCcSet.
  __ AddConstant(R0, R0, 0, AL, kCcSet);              // 16-bit ADDS, encoding T2.
  __ AddConstant(R1, R1, 1, AL, kCcSet);              // 16-bit ADDS, encoding T2.
  __ AddConstant(R0, R0, 7, AL, kCcSet);              // 16-bit ADDS, encoding T2.
  __ AddConstant(R1, R1, 8, AL, kCcSet);              // 16-bit ADDS, encoding T2.
  __ AddConstant(R0, R0, 255, AL, kCcSet);            // 16-bit ADDS, encoding T2.
  __ AddConstant(R1, R1, 256, AL, kCcSet);            // 32-bit ADDS, encoding T3.
  __ AddConstant(R0, R0, 257, AL, kCcSet);            // MVN+SUBS.
  __ AddConstant(R1, R1, 0xfff, AL, kCcSet);          // MOVW+ADDS.
  __ AddConstant(R0, R0, 0x1000, AL, kCcSet);         // 32-bit ADDS, encoding T3.
  __ AddConstant(R1, R1, 0x1001, AL, kCcSet);         // MVN+SUBS.
  __ AddConstant(R0, R0, 0x1002, AL, kCcSet);         // MOVW+ADDS.
  __ AddConstant(R1, R1, 0xffff, AL, kCcSet);         // MOVW+ADDS.
  __ AddConstant(R0, R0, 0x10000, AL, kCcSet);        // 32-bit ADDS, encoding T3.
  __ AddConstant(R1, R1, 0x10001, AL, kCcSet);        // 32-bit ADDS, encoding T3.
  __ AddConstant(R0, R0, 0x10002, AL, kCcSet);        // MVN+SUBS.
  __ AddConstant(R1, R1, 0x10003, AL, kCcSet);        // MOVW+MOVT+ADDS.
  __ AddConstant(R0, R0, -1, AL, kCcSet);             // 16-bit SUBS, encoding T2.
  __ AddConstant(R1, R1, -7, AL, kCcSet);             // 16-bit SUBS, encoding T2.
  __ AddConstant(R0, R0, -8, AL, kCcSet);             // 16-bit SUBS, encoding T2.
  __ AddConstant(R1, R1, -255, AL, kCcSet);           // 16-bit SUBS, encoding T2.
  __ AddConstant(R0, R0, -256, AL, kCcSet);           // 32-bit SUB, encoding T3.
  __ AddConstant(R1, R1, -257, AL, kCcSet);           // MNV+ADDS.
  __ AddConstant(R0, R0, -0xfff, AL, kCcSet);         // MOVW+SUBS.
  __ AddConstant(R1, R1, -0x1000, AL, kCcSet);        // 32-bit SUB, encoding T3.
  __ AddConstant(R0, R0, -0x1001, AL, kCcSet);        // MVN+ADDS.
  __ AddConstant(R1, R1, -0x1002, AL, kCcSet);        // MOVW+SUBS.
  __ AddConstant(R0, R0, -0xffff, AL, kCcSet);        // MOVW+SUBS.
  __ AddConstant(R1, R1, -0x10000, AL, kCcSet);       // 32-bit SUBS, encoding T3.
  __ AddConstant(R0, R0, -0x10001, AL, kCcSet);       // 32-bit SUBS, encoding T3.
  __ AddConstant(R1, R1, -0x10002, AL, kCcSet);       // MVN+ADDS.
  __ AddConstant(R0, R0, -0x10003, AL, kCcSet);       // MOVW+MOVT+ADDS.

  __ it(EQ);
  __ AddConstant(R0, R1, 1, EQ, kCcSet);              // 32-bit ADDS, encoding T3.
  __ it(NE);
  __ AddConstant(R0, R1, 1, NE, kCcKeep);             // 16-bit ADDS, encoding T1.
  __ it(GE);
  __ AddConstant(R0, R0, 1, GE, kCcSet);              // 32-bit ADDS, encoding T3.
  __ it(LE);
  __ AddConstant(R0, R0, 1, LE, kCcKeep);             // 16-bit ADDS, encoding T2.

  EmitAndCheck(&assembler, "AddConstant");
}

TEST_F(Thumb2AssemblerTest, CmpConstant) {
  __ CmpConstant(R0, 0);                              // 16-bit CMP.
  __ CmpConstant(R1, 1);                              // 16-bit CMP.
  __ CmpConstant(R0, 7);                              // 16-bit CMP.
  __ CmpConstant(R1, 8);                              // 16-bit CMP.
  __ CmpConstant(R0, 255);                            // 16-bit CMP.
  __ CmpConstant(R1, 256);                            // 32-bit CMP.
  __ CmpConstant(R0, 257);                            // MNV+CMN.
  __ CmpConstant(R1, 0xfff);                          // MOVW+CMP.
  __ CmpConstant(R0, 0x1000);                         // 32-bit CMP.
  __ CmpConstant(R1, 0x1001);                         // MNV+CMN.
  __ CmpConstant(R0, 0x1002);                         // MOVW+CMP.
  __ CmpConstant(R1, 0xffff);                         // MOVW+CMP.
  __ CmpConstant(R0, 0x10000);                        // 32-bit CMP.
  __ CmpConstant(R1, 0x10001);                        // 32-bit CMP.
  __ CmpConstant(R0, 0x10002);                        // MVN+CMN.
  __ CmpConstant(R1, 0x10003);                        // MOVW+MOVT+CMP.
  __ CmpConstant(R0, -1);                             // 32-bit CMP.
  __ CmpConstant(R1, -7);                             // CMN.
  __ CmpConstant(R0, -8);                             // CMN.
  __ CmpConstant(R1, -255);                           // CMN.
  __ CmpConstant(R0, -256);                           // CMN.
  __ CmpConstant(R1, -257);                           // MNV+CMP.
  __ CmpConstant(R0, -0xfff);                         // MOVW+CMN.
  __ CmpConstant(R1, -0x1000);                        // CMN.
  __ CmpConstant(R0, -0x1001);                        // MNV+CMP.
  __ CmpConstant(R1, -0x1002);                        // MOVW+CMN.
  __ CmpConstant(R0, -0xffff);                        // MOVW+CMN.
  __ CmpConstant(R1, -0x10000);                       // CMN.
  __ CmpConstant(R0, -0x10001);                       // CMN.
  __ CmpConstant(R1, -0x10002);                       // MVN+CMP.
  __ CmpConstant(R0, -0x10003);                       // MOVW+MOVT+CMP.

  __ CmpConstant(R8, 0);                              // 32-bit CMP.
  __ CmpConstant(R9, 1);                              // 32-bit CMP.
  __ CmpConstant(R8, 7);                              // 32-bit CMP.
  __ CmpConstant(R9, 8);                              // 32-bit CMP.
  __ CmpConstant(R8, 255);                            // 32-bit CMP.
  __ CmpConstant(R9, 256);                            // 32-bit CMP.
  __ CmpConstant(R8, 257);                            // MNV+CMN
  __ CmpConstant(R9, 0xfff);                          // MOVW+CMP.
  __ CmpConstant(R8, 0x1000);                         // 32-bit CMP.
  __ CmpConstant(R9, 0x1001);                         // MVN+CMN.
  __ CmpConstant(R8, 0x1002);                         // MOVW+CMP.
  __ CmpConstant(R9, 0xffff);                         // MOVW+CMP.
  __ CmpConstant(R8, 0x10000);                        // 32-bit CMP.
  __ CmpConstant(R9, 0x10001);                        // 32-bit CMP.
  __ CmpConstant(R8, 0x10002);                        // MVN+CMN.
  __ CmpConstant(R9, 0x10003);                        // MOVW+MOVT+CMP.
  __ CmpConstant(R8, -1);                             // 32-bit CMP
  __ CmpConstant(R9, -7);                             // CMN.
  __ CmpConstant(R8, -8);                             // CMN.
  __ CmpConstant(R9, -255);                           // CMN.
  __ CmpConstant(R8, -256);                           // CMN.
  __ CmpConstant(R9, -257);                           // MNV+CMP.
  __ CmpConstant(R8, -0xfff);                         // MOVW+CMN.
  __ CmpConstant(R9, -0x1000);                        // CMN.
  __ CmpConstant(R8, -0x1001);                        // MVN+CMP.
  __ CmpConstant(R9, -0x1002);                        // MOVW+CMN.
  __ CmpConstant(R8, -0xffff);                        // MOVW+CMN.
  __ CmpConstant(R9, -0x10000);                       // CMN.
  __ CmpConstant(R8, -0x10001);                       // CMN.
  __ CmpConstant(R9, -0x10002);                       // MVN+CMP.
  __ CmpConstant(R8, -0x10003);                       // MOVW+MOVT+CMP.

  EmitAndCheck(&assembler, "CmpConstant");
}

#undef __
}  // namespace arm
}  // namespace art
