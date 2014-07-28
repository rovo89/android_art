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
#include <fstream>
#include <sys/types.h>
#include <map>

#include "gtest/gtest.h"
#include "utils/arm/assembler_thumb2.h"
#include "base/hex_dump.h"
#include "common_runtime_test.h"

namespace art {
namespace arm {

// Include results file (generated manually)
#include "assembler_thumb_test_expected.cc.inc"

#ifndef HAVE_ANDROID_OS
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

static const char* TOOL_PREFIX = "arm-linux-androideabi-";

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

std::string GetAndroidToolsDir() {
  std::string root;
  const char* android_build_top = getenv("ANDROID_BUILD_TOP");
  if (android_build_top != nullptr) {
    root += android_build_top;
  } else {
    // Not set by build server, so default to current directory
    char* cwd = getcwd(nullptr, 0);
    setenv("ANDROID_BUILD_TOP", cwd, 1);
    root += cwd;
    free(cwd);
  }

  // Look for "prebuilts"
  std::string toolsdir = root;
  struct stat st;
  while (toolsdir != "") {
    std::string prebuilts = toolsdir + "/prebuilts";
    if (stat(prebuilts.c_str(), &st) == 0) {
       // Found prebuilts.
       toolsdir += "/prebuilts/gcc/linux-x86/arm";
       break;
    }
    // Not present, move up one dir.
    size_t slash = toolsdir.rfind('/');
    if (slash == std::string::npos) {
      toolsdir = "";
    } else {
      toolsdir = toolsdir.substr(0, slash-1);
    }
  }
  bool statok = stat(toolsdir.c_str(), &st) == 0;
  if (!statok) {
    return "";      // Use path.
  }

  DIR* dir = opendir(toolsdir.c_str());
  if (dir == nullptr) {
    return "";      // Use path.
  }

  struct dirent* entry;
  std::string founddir;
  double maxversion  = 0;

  // Find the latest version of the arm-eabi tools (biggest version number).
  // Suffix on toolsdir will be something like "arm-eabi-4.8"
  while ((entry = readdir(dir)) != nullptr) {
    std::string subdir = toolsdir + std::string("/") + std::string(entry->d_name);
    size_t eabi = subdir.find(TOOL_PREFIX);
    if (eabi != std::string::npos) {
      std::string suffix = subdir.substr(eabi + strlen(TOOL_PREFIX));
      double version = strtod(suffix.c_str(), nullptr);
      if (version > maxversion) {
        maxversion = version;
        founddir = subdir;
      }
    }
  }
  closedir(dir);
  bool found = founddir != "";
  if (!found) {
    return "";      // Use path.
  }

  return founddir + "/bin/";
}

void dump(std::vector<uint8_t>& code, const char* testname) {
  // This will only work on the host.  There is no as, objcopy or objdump on the
  // device.
#ifndef HAVE_ANDROID_OS
  static bool results_ok = false;
  static std::string toolsdir;

  if (!results_ok) {
    setup_results();
    toolsdir = GetAndroidToolsDir();
    SetAndroidData();
    results_ok = true;
  }

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
  snprintf(cmd, sizeof(cmd), "%s%sas %s -o %s.o", toolsdir.c_str(), TOOL_PREFIX, filename, filename);
  system(cmd);

  // Remove the $d symbols to prevent the disassembler dumping the instructions
  // as .word
  snprintf(cmd, sizeof(cmd), "%s%sobjcopy -N '$d' %s.o %s.oo", toolsdir.c_str(), TOOL_PREFIX,
    filename, filename);
  system(cmd);

  // Disassemble.

  snprintf(cmd, sizeof(cmd), "%s%sobjdump -d %s.oo | grep '^  *[0-9a-f][0-9a-f]*:'",
    toolsdir.c_str(), TOOL_PREFIX, filename);
  if (kPrintResults) {
    // Print the results only, don't check. This is used to generate new output for inserting
    // into the .inc file.
    system(cmd);
  } else {
    // Check the results match the appropriate results in the .inc file.
    FILE *fp = popen(cmd, "r");
    ASSERT_TRUE(fp != nullptr);

    std::map<std::string, const char**>::iterator results = test_results.find(testname);
    ASSERT_NE(results, test_results.end());

    uint32_t lineindex = 0;

    while (!feof(fp)) {
      char testline[256];
      char *s = fgets(testline, sizeof(testline), fp);
      if (s == nullptr) {
        break;
      }
      if (CompareIgnoringSpace(results->second[lineindex], testline) != 0) {
        LOG(FATAL) << "Output is not as expected at line: " << lineindex
          << results->second[lineindex] << "/" << testline;
      }
      ++lineindex;
    }
    // Check that we are at the end.
    ASSERT_TRUE(results->second[lineindex] == nullptr);
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

TEST(Thumb2AssemblerTest, SimpleMov) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  __ mov(R0, ShifterOperand(R1));
  __ mov(R8, ShifterOperand(R9));

  __ mov(R0, ShifterOperand(1));
  __ mov(R8, ShifterOperand(9));

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "SimpleMov");
  delete assembler;
}

TEST(Thumb2AssemblerTest, SimpleMov32) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));
  assembler->Force32Bit();

  __ mov(R0, ShifterOperand(R1));
  __ mov(R8, ShifterOperand(R9));

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "SimpleMov32");
  delete assembler;
}

TEST(Thumb2AssemblerTest, SimpleMovAdd) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  __ mov(R0, ShifterOperand(R1));
  __ add(R0, R1, ShifterOperand(R2));
  __ add(R0, R1, ShifterOperand());

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "SimpleMovAdd");
  delete assembler;
}

TEST(Thumb2AssemblerTest, DataProcessingRegister) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  __ mov(R0, ShifterOperand(R1));
  __ mvn(R0, ShifterOperand(R1));

  // 32 bit variants.
  __ add(R0, R1, ShifterOperand(R2));
  __ sub(R0, R1, ShifterOperand(R2));
  __ and_(R0, R1, ShifterOperand(R2));
  __ orr(R0, R1, ShifterOperand(R2));
  __ eor(R0, R1, ShifterOperand(R2));
  __ bic(R0, R1, ShifterOperand(R2));
  __ adc(R0, R1, ShifterOperand(R2));
  __ sbc(R0, R1, ShifterOperand(R2));
  __ rsb(R0, R1, ShifterOperand(R2));

  // 16 bit variants.
  __ add(R0, R1, ShifterOperand());
  __ sub(R0, R1, ShifterOperand());
  __ and_(R0, R1, ShifterOperand());
  __ orr(R0, R1, ShifterOperand());
  __ eor(R0, R1, ShifterOperand());
  __ bic(R0, R1, ShifterOperand());
  __ adc(R0, R1, ShifterOperand());
  __ sbc(R0, R1, ShifterOperand());
  __ rsb(R0, R1, ShifterOperand());

  __ tst(R0, ShifterOperand(R1));
  __ teq(R0, ShifterOperand(R1));
  __ cmp(R0, ShifterOperand(R1));
  __ cmn(R0, ShifterOperand(R1));

  __ movs(R0, ShifterOperand(R1));
  __ mvns(R0, ShifterOperand(R1));

  // 32 bit variants.
  __ add(R12, R1, ShifterOperand(R0));

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "DataProcessingRegister");
  delete assembler;
}

TEST(Thumb2AssemblerTest, DataProcessingImmediate) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  __ mov(R0, ShifterOperand(0x55));
  __ mvn(R0, ShifterOperand(0x55));
  __ add(R0, R1, ShifterOperand(0x55));
  __ sub(R0, R1, ShifterOperand(0x55));
  __ and_(R0, R1, ShifterOperand(0x55));
  __ orr(R0, R1, ShifterOperand(0x55));
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

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "DataProcessingImmediate");
  delete assembler;
}

TEST(Thumb2AssemblerTest, DataProcessingModifiedImmediate) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  __ mov(R0, ShifterOperand(0x550055));
  __ mvn(R0, ShifterOperand(0x550055));
  __ add(R0, R1, ShifterOperand(0x550055));
  __ sub(R0, R1, ShifterOperand(0x550055));
  __ and_(R0, R1, ShifterOperand(0x550055));
  __ orr(R0, R1, ShifterOperand(0x550055));
  __ eor(R0, R1, ShifterOperand(0x550055));
  __ bic(R0, R1, ShifterOperand(0x550055));
  __ adc(R0, R1, ShifterOperand(0x550055));
  __ sbc(R0, R1, ShifterOperand(0x550055));
  __ rsb(R0, R1, ShifterOperand(0x550055));

  __ tst(R0, ShifterOperand(0x550055));
  __ teq(R0, ShifterOperand(0x550055));
  __ cmp(R0, ShifterOperand(0x550055));
  __ cmn(R0, ShifterOperand(0x550055));

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "DataProcessingModifiedImmediate");
  delete assembler;
}


TEST(Thumb2AssemblerTest, DataProcessingModifiedImmediates) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  __ mov(R0, ShifterOperand(0x550055));
  __ mov(R0, ShifterOperand(0x55005500));
  __ mov(R0, ShifterOperand(0x55555555));
  __ mov(R0, ShifterOperand(0xd5000000));       // rotated to first position
  __ mov(R0, ShifterOperand(0x6a000000));       // rotated to second position
  __ mov(R0, ShifterOperand(0x350));            // rotated to 2nd last position
  __ mov(R0, ShifterOperand(0x1a8));            // rotated to last position

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "DataProcessingModifiedImmediates");
  delete assembler;
}

TEST(Thumb2AssemblerTest, DataProcessingShiftedRegister) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  __ mov(R3, ShifterOperand(R4, LSL, 4));
  __ mov(R3, ShifterOperand(R4, LSR, 5));
  __ mov(R3, ShifterOperand(R4, ASR, 6));
  __ mov(R3, ShifterOperand(R4, ROR, 7));
  __ mov(R3, ShifterOperand(R4, ROR));

  // 32 bit variants.
  __ mov(R8, ShifterOperand(R4, LSL, 4));
  __ mov(R8, ShifterOperand(R4, LSR, 5));
  __ mov(R8, ShifterOperand(R4, ASR, 6));
  __ mov(R8, ShifterOperand(R4, ROR, 7));
  __ mov(R8, ShifterOperand(R4, RRX));

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "DataProcessingShiftedRegister");
  delete assembler;
}


TEST(Thumb2AssemblerTest, BasicLoad) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

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

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "BasicLoad");
  delete assembler;
}


TEST(Thumb2AssemblerTest, BasicStore) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  __ str(R3, Address(R4, 24));
  __ strb(R3, Address(R4, 24));
  __ strh(R3, Address(R4, 24));

  __ str(R3, Address(SP, 24));

  // 32 bit variants.
  __ str(R8, Address(R4, 24));
  __ strb(R8, Address(R4, 24));
  __ strh(R8, Address(R4, 24));

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "BasicStore");
  delete assembler;
}

TEST(Thumb2AssemblerTest, ComplexLoad) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

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

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "ComplexLoad");
  delete assembler;
}


TEST(Thumb2AssemblerTest, ComplexStore) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

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

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "ComplexStore");
  delete assembler;
}

TEST(Thumb2AssemblerTest, NegativeLoadStore) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

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

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "NegativeLoadStore");
  delete assembler;
}

TEST(Thumb2AssemblerTest, SimpleLoadStoreDual) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  __ strd(R2, Address(R0, 24, Address::Mode::Offset));
  __ ldrd(R2, Address(R0, 24, Address::Mode::Offset));

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "SimpleLoadStoreDual");
  delete assembler;
}

TEST(Thumb2AssemblerTest, ComplexLoadStoreDual) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

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

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "ComplexLoadStoreDual");
  delete assembler;
}

TEST(Thumb2AssemblerTest, NegativeLoadStoreDual) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

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

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "NegativeLoadStoreDual");
  delete assembler;
}

TEST(Thumb2AssemblerTest, SimpleBranch) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

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

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "SimpleBranch");
  delete assembler;
}

TEST(Thumb2AssemblerTest, LongBranch) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));
  assembler->Force32Bit();
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

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "LongBranch");
  delete assembler;
}

TEST(Thumb2AssemblerTest, LoadMultiple) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  // 16 bit.
  __ ldm(DB_W, R4, (1 << R0 | 1 << R3));

  // 32 bit.
  __ ldm(DB_W, R4, (1 << LR | 1 << R11));
  __ ldm(DB, R4, (1 << LR | 1 << R11));

  // Single reg is converted to ldr
  __ ldm(DB_W, R4, (1 << R5));

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "LoadMultiple");
  delete assembler;
}

TEST(Thumb2AssemblerTest, StoreMultiple) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  // 16 bit.
  __ stm(IA_W, R4, (1 << R0 | 1 << R3));

  // 32 bit.
  __ stm(IA_W, R4, (1 << LR | 1 << R11));
  __ stm(IA, R4, (1 << LR | 1 << R11));

  // Single reg is converted to str
  __ stm(IA_W, R4, (1 << R5));
  __ stm(IA, R4, (1 << R5));

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "StoreMultiple");
  delete assembler;
}

TEST(Thumb2AssemblerTest, MovWMovT) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  __ movw(R4, 0);         // 16 bit.
  __ movw(R4, 0x34);      // 16 bit.
  __ movw(R9, 0x34);      // 32 bit due to high register.
  __ movw(R3, 0x1234);    // 32 bit due to large value.
  __ movw(R9, 0xffff);    // 32 bit due to large value and high register.

  // Always 32 bit.
  __ movt(R0, 0);
  __ movt(R0, 0x1234);
  __ movt(R1, 0xffff);

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "MovWMovT");
  delete assembler;
}

TEST(Thumb2AssemblerTest, SpecialAddSub) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  __ add(R2, SP, ShifterOperand(0x50));   // 16 bit.
  __ add(SP, SP, ShifterOperand(0x50));   // 16 bit.
  __ add(R8, SP, ShifterOperand(0x50));   // 32 bit.

  __ add(R2, SP, ShifterOperand(0xf00));  // 32 bit due to imm size.
  __ add(SP, SP, ShifterOperand(0xf00));  // 32 bit due to imm size.

  __ sub(SP, SP, ShifterOperand(0x50));     // 16 bit
  __ sub(R0, SP, ShifterOperand(0x50));     // 32 bit
  __ sub(R8, SP, ShifterOperand(0x50));     // 32 bit.

  __ sub(SP, SP, ShifterOperand(0xf00));   // 32 bit due to imm size

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "SpecialAddSub");
  delete assembler;
}

TEST(Thumb2AssemblerTest, StoreToOffset) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  __ StoreToOffset(kStoreWord, R2, R4, 12);     // Simple
  __ StoreToOffset(kStoreWord, R2, R4, 0x2000);     // Offset too big.
  __ StoreToOffset(kStoreWord, R0, R12, 12);
  __ StoreToOffset(kStoreHalfword, R0, R12, 12);
  __ StoreToOffset(kStoreByte, R2, R12, 12);

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "StoreToOffset");
  delete assembler;
}


TEST(Thumb2AssemblerTest, IfThen) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

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

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "IfThen");
  delete assembler;
}

TEST(Thumb2AssemblerTest, CbzCbnz) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

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

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "CbzCbnz");
  delete assembler;
}

TEST(Thumb2AssemblerTest, Multiply) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

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

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "Multiply");
  delete assembler;
}

TEST(Thumb2AssemblerTest, Divide) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  __ sdiv(R0, R1, R2);
  __ sdiv(R8, R9, R10);

  __ udiv(R0, R1, R2);
  __ udiv(R8, R9, R10);

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "Divide");
  delete assembler;
}

TEST(Thumb2AssemblerTest, VMov) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  __ vmovs(S1, 1.0);
  __ vmovd(D1, 1.0);

  __ vmovs(S1, S2);
  __ vmovd(D1, D2);

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "VMov");
  delete assembler;
}


TEST(Thumb2AssemblerTest, BasicFloatingPoint) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

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

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "BasicFloatingPoint");
  delete assembler;
}

TEST(Thumb2AssemblerTest, FloatingPointConversions) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

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

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "FloatingPointConversions");
  delete assembler;
}

TEST(Thumb2AssemblerTest, FloatingPointComparisons) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  __ vcmps(S0, S1);
  __ vcmpd(D0, D1);

  __ vcmpsz(S2);
  __ vcmpdz(D2);

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "FloatingPointComparisons");
  delete assembler;
}

TEST(Thumb2AssemblerTest, Calls) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  __ blx(LR);
  __ bx(LR);

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "Calls");
  delete assembler;
}

TEST(Thumb2AssemblerTest, Breakpoint) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  __ bkpt(0);

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "Breakpoint");
  delete assembler;
}

TEST(Thumb2AssemblerTest, StrR1) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  __ str(R1, Address(SP, 68));
  __ str(R1, Address(SP, 1068));

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "StrR1");
  delete assembler;
}

TEST(Thumb2AssemblerTest, VPushPop) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  __ vpushs(S2, 4);
  __ vpushd(D2, 4);

  __ vpops(S2, 4);
  __ vpopd(D2, 4);

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "VPushPop");
  delete assembler;
}

TEST(Thumb2AssemblerTest, Max16BitBranch) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  Label l1;
  __ b(&l1);
  for (int i = 0 ; i < (1 << 11) ; i += 2) {
    __ mov(R3, ShifterOperand(i & 0xff));
  }
  __ Bind(&l1);
  __ mov(R1, ShifterOperand(R2));

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "Max16BitBranch");
  delete assembler;
}

TEST(Thumb2AssemblerTest, Branch32) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  Label l1;
  __ b(&l1);
  for (int i = 0 ; i < (1 << 11) + 2 ; i += 2) {
    __ mov(R3, ShifterOperand(i & 0xff));
  }
  __ Bind(&l1);
  __ mov(R1, ShifterOperand(R2));

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "Branch32");
  delete assembler;
}

TEST(Thumb2AssemblerTest, CompareAndBranchMax) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  Label l1;
  __ cbz(R4, &l1);
  for (int i = 0 ; i < (1 << 7) ; i += 2) {
    __ mov(R3, ShifterOperand(i & 0xff));
  }
  __ Bind(&l1);
  __ mov(R1, ShifterOperand(R2));

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "CompareAndBranchMax");
  delete assembler;
}

TEST(Thumb2AssemblerTest, CompareAndBranchRelocation16) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  Label l1;
  __ cbz(R4, &l1);
  for (int i = 0 ; i < (1 << 7) + 2 ; i += 2) {
    __ mov(R3, ShifterOperand(i & 0xff));
  }
  __ Bind(&l1);
  __ mov(R1, ShifterOperand(R2));

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "CompareAndBranchRelocation16");
  delete assembler;
}

TEST(Thumb2AssemblerTest, CompareAndBranchRelocation32) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  Label l1;
  __ cbz(R4, &l1);
  for (int i = 0 ; i < (1 << 11) + 2 ; i += 2) {
    __ mov(R3, ShifterOperand(i & 0xff));
  }
  __ Bind(&l1);
  __ mov(R1, ShifterOperand(R2));

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "CompareAndBranchRelocation32");
  delete assembler;
}

TEST(Thumb2AssemblerTest, MixedBranch32) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

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

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "MixedBranch32");
  delete assembler;
}

TEST(Thumb2AssemblerTest, Shifts) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  // 16 bit
  __ Lsl(R0, R1, 5);
  __ Lsr(R0, R1, 5);
  __ Asr(R0, R1, 5);

  __ Lsl(R0, R0, R1);
  __ Lsr(R0, R0, R1);
  __ Asr(R0, R0, R1);

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
  __ Lsl(R8, R1, 5, true);
  __ Lsr(R0, R8, 5, true);
  __ Asr(R8, R1, 5, true);
  __ Ror(R0, R8, 5, true);

  // 32 bit due to different Rd and Rn.
  __ Lsl(R0, R1, R2, true);
  __ Lsr(R0, R1, R2, true);
  __ Asr(R0, R1, R2, true);
  __ Ror(R0, R1, R2, true);

  // 32 bit due to use of high registers.
  __ Lsl(R8, R1, R2, true);
  __ Lsr(R0, R8, R2, true);
  __ Asr(R0, R1, R8, true);

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "Shifts");
  delete assembler;
}

TEST(Thumb2AssemblerTest, LoadStoreRegOffset) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

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

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "LoadStoreRegOffset");
  delete assembler;
}

TEST(Thumb2AssemblerTest, LoadStoreLiteral) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

  __ ldr(R0, Address(4));
  __ str(R0, Address(4));

  __ ldr(R0, Address(-8));
  __ str(R0, Address(-8));

  // Limits.
  __ ldr(R0, Address(0x3ff));       // 10 bits (16 bit).
  __ ldr(R0, Address(0x7ff));       // 11 bits (32 bit).
  __ str(R0, Address(0x3ff));       // 32 bit (no 16 bit str(literal)).
  __ str(R0, Address(0x7ff));       // 11 bits (32 bit).

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "LoadStoreLiteral");
  delete assembler;
}

TEST(Thumb2AssemblerTest, LoadStoreLimits) {
  arm::Thumb2Assembler* assembler = static_cast<arm::Thumb2Assembler*>(Assembler::Create(kThumb2));

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

  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  dump(managed_code, "LoadStoreLimits");
  delete assembler;
}

#undef __
}  // namespace arm
}  // namespace art
