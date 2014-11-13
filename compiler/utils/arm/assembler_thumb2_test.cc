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
                                                arm::Register,
                                                uint32_t> {
 protected:
  std::string GetArchitectureString() OVERRIDE {
    return "arm";
  }

  std::string GetAssemblerParameters() OVERRIDE {
    return " -mthumb";
  }

  std::string GetDisassembleParameters() OVERRIDE {
    return " -D -bbinary -marm --no-show-raw-insn";
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

}  // namespace art
