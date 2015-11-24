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

#include "assembler_mips64.h"

#include <inttypes.h>
#include <map>
#include <random>

#include "base/bit_utils.h"
#include "base/stl_util.h"
#include "utils/assembler_test.h"

#define __ GetAssembler()->

namespace art {

struct MIPS64CpuRegisterCompare {
  bool operator()(const mips64::GpuRegister& a, const mips64::GpuRegister& b) const {
    return a < b;
  }
};

class AssemblerMIPS64Test : public AssemblerTest<mips64::Mips64Assembler,
                                                 mips64::GpuRegister,
                                                 mips64::FpuRegister,
                                                 uint32_t> {
 public:
  typedef AssemblerTest<mips64::Mips64Assembler,
                        mips64::GpuRegister,
                        mips64::FpuRegister,
                        uint32_t> Base;

 protected:
  // Get the typically used name for this architecture, e.g., aarch64, x86-64, ...
  std::string GetArchitectureString() OVERRIDE {
    return "mips64";
  }

  std::string GetAssemblerCmdName() OVERRIDE {
    // We assemble and link for MIPS64R6. See GetAssemblerParameters() for details.
    return "gcc";
  }

  std::string GetAssemblerParameters() OVERRIDE {
    // We assemble and link for MIPS64R6. The reason is that object files produced for MIPS64R6
    // (and MIPS32R6) with the GNU assembler don't have correct final offsets in PC-relative
    // branches in the .text section and so they require a relocation pass (there's a relocation
    // section, .rela.text, that has the needed info to fix up the branches).
    return " -march=mips64r6 -Wa,--no-warn -Wl,-Ttext=0 -Wl,-e0 -nostdlib";
  }

  void Pad(std::vector<uint8_t>& data) OVERRIDE {
    // The GNU linker unconditionally pads the code segment with NOPs to a size that is a multiple
    // of 16 and there doesn't appear to be a way to suppress this padding. Our assembler doesn't
    // pad, so, in order for two assembler outputs to match, we need to match the padding as well.
    // NOP is encoded as four zero bytes on MIPS.
    size_t pad_size = RoundUp(data.size(), 16u) - data.size();
    data.insert(data.end(), pad_size, 0);
  }

  std::string GetDisassembleParameters() OVERRIDE {
    return " -D -bbinary -mmips:isa64r6";
  }

  void SetUpHelpers() OVERRIDE {
    if (registers_.size() == 0) {
      registers_.push_back(new mips64::GpuRegister(mips64::ZERO));
      registers_.push_back(new mips64::GpuRegister(mips64::AT));
      registers_.push_back(new mips64::GpuRegister(mips64::V0));
      registers_.push_back(new mips64::GpuRegister(mips64::V1));
      registers_.push_back(new mips64::GpuRegister(mips64::A0));
      registers_.push_back(new mips64::GpuRegister(mips64::A1));
      registers_.push_back(new mips64::GpuRegister(mips64::A2));
      registers_.push_back(new mips64::GpuRegister(mips64::A3));
      registers_.push_back(new mips64::GpuRegister(mips64::A4));
      registers_.push_back(new mips64::GpuRegister(mips64::A5));
      registers_.push_back(new mips64::GpuRegister(mips64::A6));
      registers_.push_back(new mips64::GpuRegister(mips64::A7));
      registers_.push_back(new mips64::GpuRegister(mips64::T0));
      registers_.push_back(new mips64::GpuRegister(mips64::T1));
      registers_.push_back(new mips64::GpuRegister(mips64::T2));
      registers_.push_back(new mips64::GpuRegister(mips64::T3));
      registers_.push_back(new mips64::GpuRegister(mips64::S0));
      registers_.push_back(new mips64::GpuRegister(mips64::S1));
      registers_.push_back(new mips64::GpuRegister(mips64::S2));
      registers_.push_back(new mips64::GpuRegister(mips64::S3));
      registers_.push_back(new mips64::GpuRegister(mips64::S4));
      registers_.push_back(new mips64::GpuRegister(mips64::S5));
      registers_.push_back(new mips64::GpuRegister(mips64::S6));
      registers_.push_back(new mips64::GpuRegister(mips64::S7));
      registers_.push_back(new mips64::GpuRegister(mips64::T8));
      registers_.push_back(new mips64::GpuRegister(mips64::T9));
      registers_.push_back(new mips64::GpuRegister(mips64::K0));
      registers_.push_back(new mips64::GpuRegister(mips64::K1));
      registers_.push_back(new mips64::GpuRegister(mips64::GP));
      registers_.push_back(new mips64::GpuRegister(mips64::SP));
      registers_.push_back(new mips64::GpuRegister(mips64::S8));
      registers_.push_back(new mips64::GpuRegister(mips64::RA));

      secondary_register_names_.emplace(mips64::GpuRegister(mips64::ZERO), "zero");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::AT), "at");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::V0), "v0");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::V1), "v1");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::A0), "a0");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::A1), "a1");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::A2), "a2");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::A3), "a3");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::A4), "a4");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::A5), "a5");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::A6), "a6");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::A7), "a7");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::T0), "t0");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::T1), "t1");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::T2), "t2");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::T3), "t3");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::S0), "s0");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::S1), "s1");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::S2), "s2");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::S3), "s3");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::S4), "s4");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::S5), "s5");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::S6), "s6");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::S7), "s7");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::T8), "t8");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::T9), "t9");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::K0), "k0");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::K1), "k1");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::GP), "gp");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::SP), "sp");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::S8), "s8");
      secondary_register_names_.emplace(mips64::GpuRegister(mips64::RA), "ra");

      fp_registers_.push_back(new mips64::FpuRegister(mips64::F0));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F1));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F2));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F3));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F4));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F5));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F6));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F7));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F8));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F9));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F10));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F11));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F12));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F13));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F14));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F15));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F16));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F17));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F18));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F19));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F20));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F21));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F22));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F23));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F24));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F25));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F26));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F27));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F28));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F29));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F30));
      fp_registers_.push_back(new mips64::FpuRegister(mips64::F31));
    }
  }

  void TearDown() OVERRIDE {
    AssemblerTest::TearDown();
    STLDeleteElements(&registers_);
    STLDeleteElements(&fp_registers_);
  }

  std::vector<mips64::GpuRegister*> GetRegisters() OVERRIDE {
    return registers_;
  }

  std::vector<mips64::FpuRegister*> GetFPRegisters() OVERRIDE {
    return fp_registers_;
  }

  uint32_t CreateImmediate(int64_t imm_value) OVERRIDE {
    return imm_value;
  }

  std::string GetSecondaryRegisterName(const mips64::GpuRegister& reg) OVERRIDE {
    CHECK(secondary_register_names_.find(reg) != secondary_register_names_.end());
    return secondary_register_names_[reg];
  }

  std::string RepeatInsn(size_t count, const std::string& insn) {
    std::string result;
    for (; count != 0u; --count) {
      result += insn;
    }
    return result;
  }

  void BranchCondOneRegHelper(void (mips64::Mips64Assembler::*f)(mips64::GpuRegister,
                                                                 mips64::Mips64Label*),
                              std::string instr_name) {
    mips64::Mips64Label label;
    (Base::GetAssembler()->*f)(mips64::A0, &label);
    constexpr size_t kAdduCount1 = 63;
    for (size_t i = 0; i != kAdduCount1; ++i) {
      __ Addu(mips64::ZERO, mips64::ZERO, mips64::ZERO);
    }
    __ Bind(&label);
    constexpr size_t kAdduCount2 = 64;
    for (size_t i = 0; i != kAdduCount2; ++i) {
      __ Addu(mips64::ZERO, mips64::ZERO, mips64::ZERO);
    }
    (Base::GetAssembler()->*f)(mips64::A1, &label);

    std::string expected =
        ".set noreorder\n" +
        instr_name + " $a0, 1f\n"
        "nop\n" +
        RepeatInsn(kAdduCount1, "addu $zero, $zero, $zero\n") +
        "1:\n" +
        RepeatInsn(kAdduCount2, "addu $zero, $zero, $zero\n") +
        instr_name + " $a1, 1b\n"
        "nop\n";
    DriverStr(expected, instr_name);
  }

  void BranchCondTwoRegsHelper(void (mips64::Mips64Assembler::*f)(mips64::GpuRegister,
                                                                  mips64::GpuRegister,
                                                                  mips64::Mips64Label*),
                               std::string instr_name) {
    mips64::Mips64Label label;
    (Base::GetAssembler()->*f)(mips64::A0, mips64::A1, &label);
    constexpr size_t kAdduCount1 = 63;
    for (size_t i = 0; i != kAdduCount1; ++i) {
      __ Addu(mips64::ZERO, mips64::ZERO, mips64::ZERO);
    }
    __ Bind(&label);
    constexpr size_t kAdduCount2 = 64;
    for (size_t i = 0; i != kAdduCount2; ++i) {
      __ Addu(mips64::ZERO, mips64::ZERO, mips64::ZERO);
    }
    (Base::GetAssembler()->*f)(mips64::A2, mips64::A3, &label);

    std::string expected =
        ".set noreorder\n" +
        instr_name + " $a0, $a1, 1f\n"
        "nop\n" +
        RepeatInsn(kAdduCount1, "addu $zero, $zero, $zero\n") +
        "1:\n" +
        RepeatInsn(kAdduCount2, "addu $zero, $zero, $zero\n") +
        instr_name + " $a2, $a3, 1b\n"
        "nop\n";
    DriverStr(expected, instr_name);
  }

 private:
  std::vector<mips64::GpuRegister*> registers_;
  std::map<mips64::GpuRegister, std::string, MIPS64CpuRegisterCompare> secondary_register_names_;

  std::vector<mips64::FpuRegister*> fp_registers_;
};


TEST_F(AssemblerMIPS64Test, Toolchain) {
  EXPECT_TRUE(CheckTools());
}

///////////////////
// FP Operations //
///////////////////

TEST_F(AssemblerMIPS64Test, SqrtS) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::SqrtS, "sqrt.s ${reg1}, ${reg2}"), "sqrt.s");
}

TEST_F(AssemblerMIPS64Test, SqrtD) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::SqrtD, "sqrt.d ${reg1}, ${reg2}"), "sqrt.d");
}

TEST_F(AssemblerMIPS64Test, AbsS) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::AbsS, "abs.s ${reg1}, ${reg2}"), "abs.s");
}

TEST_F(AssemblerMIPS64Test, AbsD) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::AbsD, "abs.d ${reg1}, ${reg2}"), "abs.d");
}

TEST_F(AssemblerMIPS64Test, MovS) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::MovS, "mov.s ${reg1}, ${reg2}"), "mov.s");
}

TEST_F(AssemblerMIPS64Test, MovD) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::MovD, "mov.d ${reg1}, ${reg2}"), "mov.d");
}

TEST_F(AssemblerMIPS64Test, NegS) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::NegS, "neg.s ${reg1}, ${reg2}"), "neg.s");
}

TEST_F(AssemblerMIPS64Test, NegD) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::NegD, "neg.d ${reg1}, ${reg2}"), "neg.d");
}

TEST_F(AssemblerMIPS64Test, RoundLS) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::RoundLS, "round.l.s ${reg1}, ${reg2}"), "round.l.s");
}

TEST_F(AssemblerMIPS64Test, RoundLD) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::RoundLD, "round.l.d ${reg1}, ${reg2}"), "round.l.d");
}

TEST_F(AssemblerMIPS64Test, RoundWS) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::RoundWS, "round.w.s ${reg1}, ${reg2}"), "round.w.s");
}

TEST_F(AssemblerMIPS64Test, RoundWD) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::RoundWD, "round.w.d ${reg1}, ${reg2}"), "round.w.d");
}

TEST_F(AssemblerMIPS64Test, CeilLS) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::CeilLS, "ceil.l.s ${reg1}, ${reg2}"), "ceil.l.s");
}

TEST_F(AssemblerMIPS64Test, CeilLD) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::CeilLD, "ceil.l.d ${reg1}, ${reg2}"), "ceil.l.d");
}

TEST_F(AssemblerMIPS64Test, CeilWS) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::CeilWS, "ceil.w.s ${reg1}, ${reg2}"), "ceil.w.s");
}

TEST_F(AssemblerMIPS64Test, CeilWD) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::CeilWD, "ceil.w.d ${reg1}, ${reg2}"), "ceil.w.d");
}

TEST_F(AssemblerMIPS64Test, FloorLS) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::FloorLS, "floor.l.s ${reg1}, ${reg2}"), "floor.l.s");
}

TEST_F(AssemblerMIPS64Test, FloorLD) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::FloorLD, "floor.l.d ${reg1}, ${reg2}"), "floor.l.d");
}

TEST_F(AssemblerMIPS64Test, FloorWS) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::FloorWS, "floor.w.s ${reg1}, ${reg2}"), "floor.w.s");
}

TEST_F(AssemblerMIPS64Test, FloorWD) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::FloorWD, "floor.w.d ${reg1}, ${reg2}"), "floor.w.d");
}

TEST_F(AssemblerMIPS64Test, SelS) {
  DriverStr(RepeatFFF(&mips64::Mips64Assembler::SelS, "sel.s ${reg1}, ${reg2}, ${reg3}"), "sel.s");
}

TEST_F(AssemblerMIPS64Test, SelD) {
  DriverStr(RepeatFFF(&mips64::Mips64Assembler::SelD, "sel.d ${reg1}, ${reg2}, ${reg3}"), "sel.d");
}

TEST_F(AssemblerMIPS64Test, RintS) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::RintS, "rint.s ${reg1}, ${reg2}"), "rint.s");
}

TEST_F(AssemblerMIPS64Test, RintD) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::RintD, "rint.d ${reg1}, ${reg2}"), "rint.d");
}

TEST_F(AssemblerMIPS64Test, ClassS) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::ClassS, "class.s ${reg1}, ${reg2}"), "class.s");
}

TEST_F(AssemblerMIPS64Test, ClassD) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::ClassD, "class.d ${reg1}, ${reg2}"), "class.d");
}

TEST_F(AssemblerMIPS64Test, MinS) {
  DriverStr(RepeatFFF(&mips64::Mips64Assembler::MinS, "min.s ${reg1}, ${reg2}, ${reg3}"), "min.s");
}

TEST_F(AssemblerMIPS64Test, MinD) {
  DriverStr(RepeatFFF(&mips64::Mips64Assembler::MinD, "min.d ${reg1}, ${reg2}, ${reg3}"), "min.d");
}

TEST_F(AssemblerMIPS64Test, MaxS) {
  DriverStr(RepeatFFF(&mips64::Mips64Assembler::MaxS, "max.s ${reg1}, ${reg2}, ${reg3}"), "max.s");
}

TEST_F(AssemblerMIPS64Test, MaxD) {
  DriverStr(RepeatFFF(&mips64::Mips64Assembler::MaxD, "max.d ${reg1}, ${reg2}, ${reg3}"), "max.d");
}

TEST_F(AssemblerMIPS64Test, CvtDL) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::Cvtdl, "cvt.d.l ${reg1}, ${reg2}"), "cvt.d.l");
}

TEST_F(AssemblerMIPS64Test, CvtDS) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::Cvtds, "cvt.d.s ${reg1}, ${reg2}"), "cvt.d.s");
}

TEST_F(AssemblerMIPS64Test, CvtDW) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::Cvtdw, "cvt.d.w ${reg1}, ${reg2}"), "cvt.d.w");
}

TEST_F(AssemblerMIPS64Test, CvtSL) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::Cvtsl, "cvt.s.l ${reg1}, ${reg2}"), "cvt.s.l");
}

TEST_F(AssemblerMIPS64Test, CvtSD) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::Cvtsd, "cvt.s.d ${reg1}, ${reg2}"), "cvt.s.d");
}

TEST_F(AssemblerMIPS64Test, CvtSW) {
  DriverStr(RepeatFF(&mips64::Mips64Assembler::Cvtsw, "cvt.s.w ${reg1}, ${reg2}"), "cvt.s.w");
}

////////////////
// CALL / JMP //
////////////////

TEST_F(AssemblerMIPS64Test, Jalr) {
  DriverStr(".set noreorder\n" +
            RepeatRRNoDupes(&mips64::Mips64Assembler::Jalr, "jalr ${reg1}, ${reg2}"), "jalr");
}

TEST_F(AssemblerMIPS64Test, Jialc) {
  mips64::Mips64Label label1, label2;
  __ Jialc(&label1, mips64::T9);
  constexpr size_t kAdduCount1 = 63;
  for (size_t i = 0; i != kAdduCount1; ++i) {
    __ Addu(mips64::ZERO, mips64::ZERO, mips64::ZERO);
  }
  __ Bind(&label1);
  __ Jialc(&label2, mips64::T9);
  constexpr size_t kAdduCount2 = 64;
  for (size_t i = 0; i != kAdduCount2; ++i) {
    __ Addu(mips64::ZERO, mips64::ZERO, mips64::ZERO);
  }
  __ Bind(&label2);
  __ Jialc(&label1, mips64::T9);

  std::string expected =
      ".set noreorder\n"
      "lapc $t9, 1f\n"
      "jialc $t9, 0\n" +
      RepeatInsn(kAdduCount1, "addu $zero, $zero, $zero\n") +
      "1:\n"
      "lapc $t9, 2f\n"
      "jialc $t9, 0\n" +
      RepeatInsn(kAdduCount2, "addu $zero, $zero, $zero\n") +
      "2:\n"
      "lapc $t9, 1b\n"
      "jialc $t9, 0\n";
  DriverStr(expected, "Jialc");
}

TEST_F(AssemblerMIPS64Test, LongJialc) {
  mips64::Mips64Label label1, label2;
  __ Jialc(&label1, mips64::T9);
  constexpr uint32_t kAdduCount1 = (1u << 18) + 1;
  for (uint32_t i = 0; i != kAdduCount1; ++i) {
    __ Addu(mips64::ZERO, mips64::ZERO, mips64::ZERO);
  }
  __ Bind(&label1);
  __ Jialc(&label2, mips64::T9);
  constexpr uint32_t kAdduCount2 = (1u << 18) + 1;
  for (uint32_t i = 0; i != kAdduCount2; ++i) {
    __ Addu(mips64::ZERO, mips64::ZERO, mips64::ZERO);
  }
  __ Bind(&label2);
  __ Jialc(&label1, mips64::T9);

  uint32_t offset_forward1 = 3 + kAdduCount1;  // 3: account for auipc, daddiu and jic.
  offset_forward1 <<= 2;
  offset_forward1 += (offset_forward1 & 0x8000) << 1;  // Account for sign extension in daddiu.

  uint32_t offset_forward2 = 3 + kAdduCount2;  // 3: account for auipc, daddiu and jic.
  offset_forward2 <<= 2;
  offset_forward2 += (offset_forward2 & 0x8000) << 1;  // Account for sign extension in daddiu.

  uint32_t offset_back = -(3 + kAdduCount2);  // 3: account for auipc, daddiu and jic.
  offset_back <<= 2;
  offset_back += (offset_back & 0x8000) << 1;  // Account for sign extension in daddiu.

  std::ostringstream oss;
  oss <<
      ".set noreorder\n"
      "auipc $t9, 0x" << std::hex << High16Bits(offset_forward1) << "\n"
      "daddiu $t9, 0x" << std::hex << Low16Bits(offset_forward1) << "\n"
      "jialc $t9, 0\n" <<
      RepeatInsn(kAdduCount1, "addu $zero, $zero, $zero\n") <<
      "1:\n"
      "auipc $t9, 0x" << std::hex << High16Bits(offset_forward2) << "\n"
      "daddiu $t9, 0x" << std::hex << Low16Bits(offset_forward2) << "\n"
      "jialc $t9, 0\n" <<
      RepeatInsn(kAdduCount2, "addu $zero, $zero, $zero\n") <<
      "2:\n"
      "auipc $t9, 0x" << std::hex << High16Bits(offset_back) << "\n"
      "daddiu $t9, 0x" << std::hex << Low16Bits(offset_back) << "\n"
      "jialc $t9, 0\n";
  std::string expected = oss.str();
  DriverStr(expected, "LongJialc");
}

TEST_F(AssemblerMIPS64Test, Bc) {
  mips64::Mips64Label label1, label2;
  __ Bc(&label1);
  constexpr size_t kAdduCount1 = 63;
  for (size_t i = 0; i != kAdduCount1; ++i) {
    __ Addu(mips64::ZERO, mips64::ZERO, mips64::ZERO);
  }
  __ Bind(&label1);
  __ Bc(&label2);
  constexpr size_t kAdduCount2 = 64;
  for (size_t i = 0; i != kAdduCount2; ++i) {
    __ Addu(mips64::ZERO, mips64::ZERO, mips64::ZERO);
  }
  __ Bind(&label2);
  __ Bc(&label1);

  std::string expected =
      ".set noreorder\n"
      "bc 1f\n" +
      RepeatInsn(kAdduCount1, "addu $zero, $zero, $zero\n") +
      "1:\n"
      "bc 2f\n" +
      RepeatInsn(kAdduCount2, "addu $zero, $zero, $zero\n") +
      "2:\n"
      "bc 1b\n";
  DriverStr(expected, "Bc");
}

TEST_F(AssemblerMIPS64Test, Beqzc) {
  BranchCondOneRegHelper(&mips64::Mips64Assembler::Beqzc, "Beqzc");
}

TEST_F(AssemblerMIPS64Test, Bnezc) {
  BranchCondOneRegHelper(&mips64::Mips64Assembler::Bnezc, "Bnezc");
}

TEST_F(AssemblerMIPS64Test, Bltzc) {
  BranchCondOneRegHelper(&mips64::Mips64Assembler::Bltzc, "Bltzc");
}

TEST_F(AssemblerMIPS64Test, Bgezc) {
  BranchCondOneRegHelper(&mips64::Mips64Assembler::Bgezc, "Bgezc");
}

TEST_F(AssemblerMIPS64Test, Blezc) {
  BranchCondOneRegHelper(&mips64::Mips64Assembler::Blezc, "Blezc");
}

TEST_F(AssemblerMIPS64Test, Bgtzc) {
  BranchCondOneRegHelper(&mips64::Mips64Assembler::Bgtzc, "Bgtzc");
}

TEST_F(AssemblerMIPS64Test, Beqc) {
  BranchCondTwoRegsHelper(&mips64::Mips64Assembler::Beqc, "Beqc");
}

TEST_F(AssemblerMIPS64Test, Bnec) {
  BranchCondTwoRegsHelper(&mips64::Mips64Assembler::Bnec, "Bnec");
}

TEST_F(AssemblerMIPS64Test, Bltc) {
  BranchCondTwoRegsHelper(&mips64::Mips64Assembler::Bltc, "Bltc");
}

TEST_F(AssemblerMIPS64Test, Bgec) {
  BranchCondTwoRegsHelper(&mips64::Mips64Assembler::Bgec, "Bgec");
}

TEST_F(AssemblerMIPS64Test, Bltuc) {
  BranchCondTwoRegsHelper(&mips64::Mips64Assembler::Bltuc, "Bltuc");
}

TEST_F(AssemblerMIPS64Test, Bgeuc) {
  BranchCondTwoRegsHelper(&mips64::Mips64Assembler::Bgeuc, "Bgeuc");
}

TEST_F(AssemblerMIPS64Test, LongBeqc) {
  mips64::Mips64Label label;
  __ Beqc(mips64::A0, mips64::A1, &label);
  constexpr uint32_t kAdduCount1 = (1u << 15) + 1;
  for (uint32_t i = 0; i != kAdduCount1; ++i) {
    __ Addu(mips64::ZERO, mips64::ZERO, mips64::ZERO);
  }
  __ Bind(&label);
  constexpr uint32_t kAdduCount2 = (1u << 15) + 1;
  for (uint32_t i = 0; i != kAdduCount2; ++i) {
    __ Addu(mips64::ZERO, mips64::ZERO, mips64::ZERO);
  }
  __ Beqc(mips64::A2, mips64::A3, &label);

  uint32_t offset_forward = 2 + kAdduCount1;  // 2: account for auipc and jic.
  offset_forward <<= 2;
  offset_forward += (offset_forward & 0x8000) << 1;  // Account for sign extension in jic.

  uint32_t offset_back = -(kAdduCount2 + 1);  // 1: account for bnec.
  offset_back <<= 2;
  offset_back += (offset_back & 0x8000) << 1;  // Account for sign extension in jic.

  std::ostringstream oss;
  oss <<
      ".set noreorder\n"
      "bnec $a0, $a1, 1f\n"
      "auipc $at, 0x" << std::hex << High16Bits(offset_forward) << "\n"
      "jic $at, 0x" << std::hex << Low16Bits(offset_forward) << "\n"
      "1:\n" <<
      RepeatInsn(kAdduCount1, "addu $zero, $zero, $zero\n") <<
      "2:\n" <<
      RepeatInsn(kAdduCount2, "addu $zero, $zero, $zero\n") <<
      "bnec $a2, $a3, 3f\n"
      "auipc $at, 0x" << std::hex << High16Bits(offset_back) << "\n"
      "jic $at, 0x" << std::hex << Low16Bits(offset_back) << "\n"
      "3:\n";
  std::string expected = oss.str();
  DriverStr(expected, "LongBeqc");
}

//////////
// MISC //
//////////

TEST_F(AssemblerMIPS64Test, Bitswap) {
  DriverStr(RepeatRR(&mips64::Mips64Assembler::Bitswap, "bitswap ${reg1}, ${reg2}"), "bitswap");
}

TEST_F(AssemblerMIPS64Test, Dbitswap) {
  DriverStr(RepeatRR(&mips64::Mips64Assembler::Dbitswap, "dbitswap ${reg1}, ${reg2}"), "dbitswap");
}

TEST_F(AssemblerMIPS64Test, Seb) {
  DriverStr(RepeatRR(&mips64::Mips64Assembler::Seb, "seb ${reg1}, ${reg2}"), "seb");
}

TEST_F(AssemblerMIPS64Test, Seh) {
  DriverStr(RepeatRR(&mips64::Mips64Assembler::Seh, "seh ${reg1}, ${reg2}"), "seh");
}

TEST_F(AssemblerMIPS64Test, Dsbh) {
  DriverStr(RepeatRR(&mips64::Mips64Assembler::Dsbh, "dsbh ${reg1}, ${reg2}"), "dsbh");
}

TEST_F(AssemblerMIPS64Test, Dshd) {
  DriverStr(RepeatRR(&mips64::Mips64Assembler::Dshd, "dshd ${reg1}, ${reg2}"), "dshd");
}

TEST_F(AssemblerMIPS64Test, Wsbh) {
  DriverStr(RepeatRR(&mips64::Mips64Assembler::Wsbh, "wsbh ${reg1}, ${reg2}"), "wsbh");
}

TEST_F(AssemblerMIPS64Test, Sll) {
  DriverStr(RepeatRRIb(&mips64::Mips64Assembler::Sll, 5, "sll ${reg1}, ${reg2}, {imm}"), "sll");
}

TEST_F(AssemblerMIPS64Test, Srl) {
  DriverStr(RepeatRRIb(&mips64::Mips64Assembler::Srl, 5, "srl ${reg1}, ${reg2}, {imm}"), "srl");
}

TEST_F(AssemblerMIPS64Test, Rotr) {
  DriverStr(RepeatRRIb(&mips64::Mips64Assembler::Rotr, 5, "rotr ${reg1}, ${reg2}, {imm}"), "rotr");
}

TEST_F(AssemblerMIPS64Test, Sra) {
  DriverStr(RepeatRRIb(&mips64::Mips64Assembler::Sra, 5, "sra ${reg1}, ${reg2}, {imm}"), "sra");
}

TEST_F(AssemblerMIPS64Test, Sllv) {
  DriverStr(RepeatRRR(&mips64::Mips64Assembler::Sllv, "sllv ${reg1}, ${reg2}, ${reg3}"), "sllv");
}

TEST_F(AssemblerMIPS64Test, Srlv) {
  DriverStr(RepeatRRR(&mips64::Mips64Assembler::Srlv, "srlv ${reg1}, ${reg2}, ${reg3}"), "srlv");
}

TEST_F(AssemblerMIPS64Test, Rotrv) {
  DriverStr(RepeatRRR(&mips64::Mips64Assembler::Rotrv, "rotrv ${reg1}, ${reg2}, ${reg3}"), "rotrv");
}

TEST_F(AssemblerMIPS64Test, Srav) {
  DriverStr(RepeatRRR(&mips64::Mips64Assembler::Srav, "srav ${reg1}, ${reg2}, ${reg3}"), "srav");
}

TEST_F(AssemblerMIPS64Test, Dsll) {
  DriverStr(RepeatRRIb(&mips64::Mips64Assembler::Dsll, 5, "dsll ${reg1}, ${reg2}, {imm}"), "dsll");
}

TEST_F(AssemblerMIPS64Test, Dsrl) {
  DriverStr(RepeatRRIb(&mips64::Mips64Assembler::Dsrl, 5, "dsrl ${reg1}, ${reg2}, {imm}"), "dsrl");
}

TEST_F(AssemblerMIPS64Test, Drotr) {
  DriverStr(RepeatRRIb(&mips64::Mips64Assembler::Drotr, 5, "drotr ${reg1}, ${reg2}, {imm}"),
            "drotr");
}

TEST_F(AssemblerMIPS64Test, Dsra) {
  DriverStr(RepeatRRIb(&mips64::Mips64Assembler::Dsra, 5, "dsra ${reg1}, ${reg2}, {imm}"), "dsra");
}

TEST_F(AssemblerMIPS64Test, Dsll32) {
  DriverStr(RepeatRRIb(&mips64::Mips64Assembler::Dsll32, 5, "dsll32 ${reg1}, ${reg2}, {imm}"),
            "dsll32");
}

TEST_F(AssemblerMIPS64Test, Dsrl32) {
  DriverStr(RepeatRRIb(&mips64::Mips64Assembler::Dsrl32, 5, "dsrl32 ${reg1}, ${reg2}, {imm}"),
            "dsrl32");
}

TEST_F(AssemblerMIPS64Test, Drotr32) {
  DriverStr(RepeatRRIb(&mips64::Mips64Assembler::Drotr32, 5, "drotr32 ${reg1}, ${reg2}, {imm}"),
            "drotr32");
}

TEST_F(AssemblerMIPS64Test, Dsra32) {
  DriverStr(RepeatRRIb(&mips64::Mips64Assembler::Dsra32, 5, "dsra32 ${reg1}, ${reg2}, {imm}"),
            "dsra32");
}

TEST_F(AssemblerMIPS64Test, Sc) {
  DriverStr(RepeatRRIb(&mips64::Mips64Assembler::Sc, -9, "sc ${reg1}, {imm}(${reg2})"), "sc");
}

TEST_F(AssemblerMIPS64Test, Scd) {
  DriverStr(RepeatRRIb(&mips64::Mips64Assembler::Scd, -9, "scd ${reg1}, {imm}(${reg2})"), "scd");
}

TEST_F(AssemblerMIPS64Test, Ll) {
  DriverStr(RepeatRRIb(&mips64::Mips64Assembler::Ll, -9, "ll ${reg1}, {imm}(${reg2})"), "ll");
}

TEST_F(AssemblerMIPS64Test, Lld) {
  DriverStr(RepeatRRIb(&mips64::Mips64Assembler::Lld, -9, "lld ${reg1}, {imm}(${reg2})"), "lld");
}

TEST_F(AssemblerMIPS64Test, Seleqz) {
  DriverStr(RepeatRRR(&mips64::Mips64Assembler::Seleqz, "seleqz ${reg1}, ${reg2}, ${reg3}"),
            "seleqz");
}

TEST_F(AssemblerMIPS64Test, Selnez) {
  DriverStr(RepeatRRR(&mips64::Mips64Assembler::Selnez, "selnez ${reg1}, ${reg2}, ${reg3}"),
            "selnez");
}

TEST_F(AssemblerMIPS64Test, Clz) {
  DriverStr(RepeatRR(&mips64::Mips64Assembler::Clz, "clz ${reg1}, ${reg2}"), "clz");
}

TEST_F(AssemblerMIPS64Test, Clo) {
  DriverStr(RepeatRR(&mips64::Mips64Assembler::Clo, "clo ${reg1}, ${reg2}"), "clo");
}

TEST_F(AssemblerMIPS64Test, Dclz) {
  DriverStr(RepeatRR(&mips64::Mips64Assembler::Dclz, "dclz ${reg1}, ${reg2}"), "dclz");
}

TEST_F(AssemblerMIPS64Test, Dclo) {
  DriverStr(RepeatRR(&mips64::Mips64Assembler::Dclo, "dclo ${reg1}, ${reg2}"), "dclo");
}

}  // namespace art
