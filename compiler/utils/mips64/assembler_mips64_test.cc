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

  std::string GetAssemblerParameters() OVERRIDE {
    return " --no-warn -march=mips64r6";
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

//////////
// MISC //
//////////

TEST_F(AssemblerMIPS64Test, Bitswap) {
  DriverStr(RepeatRR(&mips64::Mips64Assembler::Bitswap, "bitswap ${reg1}, ${reg2}"), "bitswap");
}

TEST_F(AssemblerMIPS64Test, Dbitswap) {
  DriverStr(RepeatRR(&mips64::Mips64Assembler::Dbitswap, "dbitswap ${reg1}, ${reg2}"), "dbitswap");
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

TEST_F(AssemblerMIPS64Test, Rotr) {
  DriverStr(RepeatRRIb(&mips64::Mips64Assembler::Rotr, 5, "rotr ${reg1}, ${reg2}, {imm}"), "rotr");
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
