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

#include "assembler_mips.h"

#include <map>

#include "base/stl_util.h"
#include "utils/assembler_test.h"

#define __ GetAssembler()->

namespace art {

struct MIPSCpuRegisterCompare {
  bool operator()(const mips::Register& a, const mips::Register& b) const {
    return a < b;
  }
};

class AssemblerMIPSTest : public AssemblerTest<mips::MipsAssembler,
                                               mips::Register,
                                               mips::FRegister,
                                               uint32_t> {
 public:
  typedef AssemblerTest<mips::MipsAssembler, mips::Register, mips::FRegister, uint32_t> Base;

 protected:
  // Get the typically used name for this architecture, e.g., aarch64, x86-64, ...
  std::string GetArchitectureString() OVERRIDE {
    return "mips";
  }

  std::string GetAssemblerParameters() OVERRIDE {
    return " --no-warn -32 -march=mips32r2";
  }

  std::string GetDisassembleParameters() OVERRIDE {
    return " -D -bbinary -mmips:isa32r2";
  }

  void SetUpHelpers() OVERRIDE {
    if (registers_.size() == 0) {
      registers_.push_back(new mips::Register(mips::ZERO));
      registers_.push_back(new mips::Register(mips::AT));
      registers_.push_back(new mips::Register(mips::V0));
      registers_.push_back(new mips::Register(mips::V1));
      registers_.push_back(new mips::Register(mips::A0));
      registers_.push_back(new mips::Register(mips::A1));
      registers_.push_back(new mips::Register(mips::A2));
      registers_.push_back(new mips::Register(mips::A3));
      registers_.push_back(new mips::Register(mips::T0));
      registers_.push_back(new mips::Register(mips::T1));
      registers_.push_back(new mips::Register(mips::T2));
      registers_.push_back(new mips::Register(mips::T3));
      registers_.push_back(new mips::Register(mips::T4));
      registers_.push_back(new mips::Register(mips::T5));
      registers_.push_back(new mips::Register(mips::T6));
      registers_.push_back(new mips::Register(mips::T7));
      registers_.push_back(new mips::Register(mips::S0));
      registers_.push_back(new mips::Register(mips::S1));
      registers_.push_back(new mips::Register(mips::S2));
      registers_.push_back(new mips::Register(mips::S3));
      registers_.push_back(new mips::Register(mips::S4));
      registers_.push_back(new mips::Register(mips::S5));
      registers_.push_back(new mips::Register(mips::S6));
      registers_.push_back(new mips::Register(mips::S7));
      registers_.push_back(new mips::Register(mips::T8));
      registers_.push_back(new mips::Register(mips::T9));
      registers_.push_back(new mips::Register(mips::K0));
      registers_.push_back(new mips::Register(mips::K1));
      registers_.push_back(new mips::Register(mips::GP));
      registers_.push_back(new mips::Register(mips::SP));
      registers_.push_back(new mips::Register(mips::FP));
      registers_.push_back(new mips::Register(mips::RA));

      secondary_register_names_.emplace(mips::Register(mips::ZERO), "zero");
      secondary_register_names_.emplace(mips::Register(mips::AT), "at");
      secondary_register_names_.emplace(mips::Register(mips::V0), "v0");
      secondary_register_names_.emplace(mips::Register(mips::V1), "v1");
      secondary_register_names_.emplace(mips::Register(mips::A0), "a0");
      secondary_register_names_.emplace(mips::Register(mips::A1), "a1");
      secondary_register_names_.emplace(mips::Register(mips::A2), "a2");
      secondary_register_names_.emplace(mips::Register(mips::A3), "a3");
      secondary_register_names_.emplace(mips::Register(mips::T0), "t0");
      secondary_register_names_.emplace(mips::Register(mips::T1), "t1");
      secondary_register_names_.emplace(mips::Register(mips::T2), "t2");
      secondary_register_names_.emplace(mips::Register(mips::T3), "t3");
      secondary_register_names_.emplace(mips::Register(mips::T4), "t4");
      secondary_register_names_.emplace(mips::Register(mips::T5), "t5");
      secondary_register_names_.emplace(mips::Register(mips::T6), "t6");
      secondary_register_names_.emplace(mips::Register(mips::T7), "t7");
      secondary_register_names_.emplace(mips::Register(mips::S0), "s0");
      secondary_register_names_.emplace(mips::Register(mips::S1), "s1");
      secondary_register_names_.emplace(mips::Register(mips::S2), "s2");
      secondary_register_names_.emplace(mips::Register(mips::S3), "s3");
      secondary_register_names_.emplace(mips::Register(mips::S4), "s4");
      secondary_register_names_.emplace(mips::Register(mips::S5), "s5");
      secondary_register_names_.emplace(mips::Register(mips::S6), "s6");
      secondary_register_names_.emplace(mips::Register(mips::S7), "s7");
      secondary_register_names_.emplace(mips::Register(mips::T8), "t8");
      secondary_register_names_.emplace(mips::Register(mips::T9), "t9");
      secondary_register_names_.emplace(mips::Register(mips::K0), "k0");
      secondary_register_names_.emplace(mips::Register(mips::K1), "k1");
      secondary_register_names_.emplace(mips::Register(mips::GP), "gp");
      secondary_register_names_.emplace(mips::Register(mips::SP), "sp");
      secondary_register_names_.emplace(mips::Register(mips::FP), "fp");
      secondary_register_names_.emplace(mips::Register(mips::RA), "ra");

      fp_registers_.push_back(new mips::FRegister(mips::F0));
      fp_registers_.push_back(new mips::FRegister(mips::F1));
      fp_registers_.push_back(new mips::FRegister(mips::F2));
      fp_registers_.push_back(new mips::FRegister(mips::F3));
      fp_registers_.push_back(new mips::FRegister(mips::F4));
      fp_registers_.push_back(new mips::FRegister(mips::F5));
      fp_registers_.push_back(new mips::FRegister(mips::F6));
      fp_registers_.push_back(new mips::FRegister(mips::F7));
      fp_registers_.push_back(new mips::FRegister(mips::F8));
      fp_registers_.push_back(new mips::FRegister(mips::F9));
      fp_registers_.push_back(new mips::FRegister(mips::F10));
      fp_registers_.push_back(new mips::FRegister(mips::F11));
      fp_registers_.push_back(new mips::FRegister(mips::F12));
      fp_registers_.push_back(new mips::FRegister(mips::F13));
      fp_registers_.push_back(new mips::FRegister(mips::F14));
      fp_registers_.push_back(new mips::FRegister(mips::F15));
      fp_registers_.push_back(new mips::FRegister(mips::F16));
      fp_registers_.push_back(new mips::FRegister(mips::F17));
      fp_registers_.push_back(new mips::FRegister(mips::F18));
      fp_registers_.push_back(new mips::FRegister(mips::F19));
      fp_registers_.push_back(new mips::FRegister(mips::F20));
      fp_registers_.push_back(new mips::FRegister(mips::F21));
      fp_registers_.push_back(new mips::FRegister(mips::F22));
      fp_registers_.push_back(new mips::FRegister(mips::F23));
      fp_registers_.push_back(new mips::FRegister(mips::F24));
      fp_registers_.push_back(new mips::FRegister(mips::F25));
      fp_registers_.push_back(new mips::FRegister(mips::F26));
      fp_registers_.push_back(new mips::FRegister(mips::F27));
      fp_registers_.push_back(new mips::FRegister(mips::F28));
      fp_registers_.push_back(new mips::FRegister(mips::F29));
      fp_registers_.push_back(new mips::FRegister(mips::F30));
      fp_registers_.push_back(new mips::FRegister(mips::F31));
    }
  }

  void TearDown() OVERRIDE {
    AssemblerTest::TearDown();
    STLDeleteElements(&registers_);
    STLDeleteElements(&fp_registers_);
  }

  std::vector<mips::Register*> GetRegisters() OVERRIDE {
    return registers_;
  }

  std::vector<mips::FRegister*> GetFPRegisters() OVERRIDE {
    return fp_registers_;
  }

  uint32_t CreateImmediate(int64_t imm_value) OVERRIDE {
    return imm_value;
  }

  std::string GetSecondaryRegisterName(const mips::Register& reg) OVERRIDE {
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

  void BranchCondOneRegHelper(void (mips::MipsAssembler::*f)(mips::Register,
                                                             mips::MipsLabel*),
                              std::string instr_name) {
    mips::MipsLabel label;
    (Base::GetAssembler()->*f)(mips::A0, &label);
    constexpr size_t kAdduCount1 = 63;
    for (size_t i = 0; i != kAdduCount1; ++i) {
      __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
    }
    __ Bind(&label);
    constexpr size_t kAdduCount2 = 64;
    for (size_t i = 0; i != kAdduCount2; ++i) {
      __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
    }
    (Base::GetAssembler()->*f)(mips::A1, &label);

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

  void BranchCondTwoRegsHelper(void (mips::MipsAssembler::*f)(mips::Register,
                                                              mips::Register,
                                                              mips::MipsLabel*),
                               std::string instr_name) {
    mips::MipsLabel label;
    (Base::GetAssembler()->*f)(mips::A0, mips::A1, &label);
    constexpr size_t kAdduCount1 = 63;
    for (size_t i = 0; i != kAdduCount1; ++i) {
      __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
    }
    __ Bind(&label);
    constexpr size_t kAdduCount2 = 64;
    for (size_t i = 0; i != kAdduCount2; ++i) {
      __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
    }
    (Base::GetAssembler()->*f)(mips::A2, mips::A3, &label);

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
  std::vector<mips::Register*> registers_;
  std::map<mips::Register, std::string, MIPSCpuRegisterCompare> secondary_register_names_;

  std::vector<mips::FRegister*> fp_registers_;
};


TEST_F(AssemblerMIPSTest, Toolchain) {
  EXPECT_TRUE(CheckTools());
}

TEST_F(AssemblerMIPSTest, Addu) {
  DriverStr(RepeatRRR(&mips::MipsAssembler::Addu, "addu ${reg1}, ${reg2}, ${reg3}"), "Addu");
}

TEST_F(AssemblerMIPSTest, Addiu) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Addiu, -16, "addiu ${reg1}, ${reg2}, {imm}"), "Addiu");
}

TEST_F(AssemblerMIPSTest, Subu) {
  DriverStr(RepeatRRR(&mips::MipsAssembler::Subu, "subu ${reg1}, ${reg2}, ${reg3}"), "Subu");
}

TEST_F(AssemblerMIPSTest, MultR2) {
  DriverStr(RepeatRR(&mips::MipsAssembler::MultR2, "mult ${reg1}, ${reg2}"), "MultR2");
}

TEST_F(AssemblerMIPSTest, MultuR2) {
  DriverStr(RepeatRR(&mips::MipsAssembler::MultuR2, "multu ${reg1}, ${reg2}"), "MultuR2");
}

TEST_F(AssemblerMIPSTest, DivR2Basic) {
  DriverStr(RepeatRR(&mips::MipsAssembler::DivR2, "div $zero, ${reg1}, ${reg2}"), "DivR2Basic");
}

TEST_F(AssemblerMIPSTest, DivuR2Basic) {
  DriverStr(RepeatRR(&mips::MipsAssembler::DivuR2, "divu $zero, ${reg1}, ${reg2}"), "DivuR2Basic");
}

TEST_F(AssemblerMIPSTest, MulR2) {
  DriverStr(RepeatRRR(&mips::MipsAssembler::MulR2, "mul ${reg1}, ${reg2}, ${reg3}"), "MulR2");
}

TEST_F(AssemblerMIPSTest, DivR2) {
  DriverStr(RepeatRRR(&mips::MipsAssembler::DivR2, "div $zero, ${reg2}, ${reg3}\nmflo ${reg1}"),
            "DivR2");
}

TEST_F(AssemblerMIPSTest, ModR2) {
  DriverStr(RepeatRRR(&mips::MipsAssembler::ModR2, "div $zero, ${reg2}, ${reg3}\nmfhi ${reg1}"),
            "ModR2");
}

TEST_F(AssemblerMIPSTest, DivuR2) {
  DriverStr(RepeatRRR(&mips::MipsAssembler::DivuR2, "divu $zero, ${reg2}, ${reg3}\nmflo ${reg1}"),
            "DivuR2");
}

TEST_F(AssemblerMIPSTest, ModuR2) {
  DriverStr(RepeatRRR(&mips::MipsAssembler::ModuR2, "divu $zero, ${reg2}, ${reg3}\nmfhi ${reg1}"),
            "ModuR2");
}

TEST_F(AssemblerMIPSTest, And) {
  DriverStr(RepeatRRR(&mips::MipsAssembler::And, "and ${reg1}, ${reg2}, ${reg3}"), "And");
}

TEST_F(AssemblerMIPSTest, Andi) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Andi, 16, "andi ${reg1}, ${reg2}, {imm}"), "Andi");
}

TEST_F(AssemblerMIPSTest, Or) {
  DriverStr(RepeatRRR(&mips::MipsAssembler::Or, "or ${reg1}, ${reg2}, ${reg3}"), "Or");
}

TEST_F(AssemblerMIPSTest, Ori) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Ori, 16, "ori ${reg1}, ${reg2}, {imm}"), "Ori");
}

TEST_F(AssemblerMIPSTest, Xor) {
  DriverStr(RepeatRRR(&mips::MipsAssembler::Xor, "xor ${reg1}, ${reg2}, ${reg3}"), "Xor");
}

TEST_F(AssemblerMIPSTest, Xori) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Xori, 16, "xori ${reg1}, ${reg2}, {imm}"), "Xori");
}

TEST_F(AssemblerMIPSTest, Nor) {
  DriverStr(RepeatRRR(&mips::MipsAssembler::Nor, "nor ${reg1}, ${reg2}, ${reg3}"), "Nor");
}

//////////
// MISC //
//////////

TEST_F(AssemblerMIPSTest, Movz) {
  DriverStr(RepeatRRR(&mips::MipsAssembler::Movz, "movz ${reg1}, ${reg2}, ${reg3}"), "Movz");
}

TEST_F(AssemblerMIPSTest, Movn) {
  DriverStr(RepeatRRR(&mips::MipsAssembler::Movn, "movn ${reg1}, ${reg2}, ${reg3}"), "Movn");
}

TEST_F(AssemblerMIPSTest, Seb) {
  DriverStr(RepeatRR(&mips::MipsAssembler::Seb, "seb ${reg1}, ${reg2}"), "Seb");
}

TEST_F(AssemblerMIPSTest, Seh) {
  DriverStr(RepeatRR(&mips::MipsAssembler::Seh, "seh ${reg1}, ${reg2}"), "Seh");
}

TEST_F(AssemblerMIPSTest, Sll) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Sll, 5, "sll ${reg1}, ${reg2}, {imm}"), "Sll");
}

TEST_F(AssemblerMIPSTest, Srl) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Srl, 5, "srl ${reg1}, ${reg2}, {imm}"), "Srl");
}

TEST_F(AssemblerMIPSTest, Sra) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Sra, 5, "sra ${reg1}, ${reg2}, {imm}"), "Sra");
}

TEST_F(AssemblerMIPSTest, Sllv) {
  DriverStr(RepeatRRR(&mips::MipsAssembler::Sllv, "sllv ${reg1}, ${reg2}, ${reg3}"), "Sllv");
}

TEST_F(AssemblerMIPSTest, Srlv) {
  DriverStr(RepeatRRR(&mips::MipsAssembler::Srlv, "srlv ${reg1}, ${reg2}, ${reg3}"), "Srlv");
}

TEST_F(AssemblerMIPSTest, Rotrv) {
  DriverStr(RepeatRRR(&mips::MipsAssembler::Rotrv, "rotrv ${reg1}, ${reg2}, ${reg3}"), "rotrv");
}

TEST_F(AssemblerMIPSTest, Srav) {
  DriverStr(RepeatRRR(&mips::MipsAssembler::Srav, "srav ${reg1}, ${reg2}, ${reg3}"), "Srav");
}

TEST_F(AssemblerMIPSTest, Ins) {
  std::vector<mips::Register*> regs = GetRegisters();
  WarnOnCombinations(regs.size() * regs.size() * 33 * 16);
  std::string expected;
  for (mips::Register* reg1 : regs) {
    for (mips::Register* reg2 : regs) {
      for (int32_t pos = 0; pos < 32; pos++) {
        for (int32_t size = 1; pos + size <= 32; size++) {
          __ Ins(*reg1, *reg2, pos, size);
          std::ostringstream instr;
          instr << "ins $" << *reg1 << ", $" << *reg2 << ", " << pos << ", " << size << "\n";
          expected += instr.str();
        }
      }
    }
  }
  DriverStr(expected, "Ins");
}

TEST_F(AssemblerMIPSTest, Ext) {
  std::vector<mips::Register*> regs = GetRegisters();
  WarnOnCombinations(regs.size() * regs.size() * 33 * 16);
  std::string expected;
  for (mips::Register* reg1 : regs) {
    for (mips::Register* reg2 : regs) {
      for (int32_t pos = 0; pos < 32; pos++) {
        for (int32_t size = 1; pos + size <= 32; size++) {
          __ Ext(*reg1, *reg2, pos, size);
          std::ostringstream instr;
          instr << "ext $" << *reg1 << ", $" << *reg2 << ", " << pos << ", " << size << "\n";
          expected += instr.str();
        }
      }
    }
  }
  DriverStr(expected, "Ext");
}

TEST_F(AssemblerMIPSTest, ClzR2) {
  DriverStr(RepeatRR(&mips::MipsAssembler::ClzR2, "clz ${reg1}, ${reg2}"), "clzR2");
}

TEST_F(AssemblerMIPSTest, CloR2) {
  DriverStr(RepeatRR(&mips::MipsAssembler::CloR2, "clo ${reg1}, ${reg2}"), "cloR2");
}

TEST_F(AssemblerMIPSTest, Lb) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Lb, -16, "lb ${reg1}, {imm}(${reg2})"), "Lb");
}

TEST_F(AssemblerMIPSTest, Lh) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Lh, -16, "lh ${reg1}, {imm}(${reg2})"), "Lh");
}

TEST_F(AssemblerMIPSTest, Lwl) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Lwl, -16, "lwl ${reg1}, {imm}(${reg2})"), "Lwl");
}

TEST_F(AssemblerMIPSTest, Lw) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Lw, -16, "lw ${reg1}, {imm}(${reg2})"), "Lw");
}

TEST_F(AssemblerMIPSTest, Lwr) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Lwr, -16, "lwr ${reg1}, {imm}(${reg2})"), "Lwr");
}

TEST_F(AssemblerMIPSTest, Lbu) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Lbu, -16, "lbu ${reg1}, {imm}(${reg2})"), "Lbu");
}

TEST_F(AssemblerMIPSTest, Lhu) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Lhu, -16, "lhu ${reg1}, {imm}(${reg2})"), "Lhu");
}

TEST_F(AssemblerMIPSTest, Lui) {
  DriverStr(RepeatRIb(&mips::MipsAssembler::Lui, 16, "lui ${reg}, {imm}"), "Lui");
}

TEST_F(AssemblerMIPSTest, Mfhi) {
  DriverStr(RepeatR(&mips::MipsAssembler::Mfhi, "mfhi ${reg}"), "Mfhi");
}

TEST_F(AssemblerMIPSTest, Mflo) {
  DriverStr(RepeatR(&mips::MipsAssembler::Mflo, "mflo ${reg}"), "Mflo");
}

TEST_F(AssemblerMIPSTest, Sb) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Sb, -16, "sb ${reg1}, {imm}(${reg2})"), "Sb");
}

TEST_F(AssemblerMIPSTest, Sh) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Sh, -16, "sh ${reg1}, {imm}(${reg2})"), "Sh");
}

TEST_F(AssemblerMIPSTest, Swl) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Swl, -16, "swl ${reg1}, {imm}(${reg2})"), "Swl");
}

TEST_F(AssemblerMIPSTest, Sw) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Sw, -16, "sw ${reg1}, {imm}(${reg2})"), "Sw");
}

TEST_F(AssemblerMIPSTest, Swr) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Swr, -16, "swr ${reg1}, {imm}(${reg2})"), "Swr");
}

TEST_F(AssemblerMIPSTest, LlR2) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::LlR2, -16, "ll ${reg1}, {imm}(${reg2})"), "LlR2");
}

TEST_F(AssemblerMIPSTest, ScR2) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::ScR2, -16, "sc ${reg1}, {imm}(${reg2})"), "ScR2");
}

TEST_F(AssemblerMIPSTest, Slt) {
  DriverStr(RepeatRRR(&mips::MipsAssembler::Slt, "slt ${reg1}, ${reg2}, ${reg3}"), "Slt");
}

TEST_F(AssemblerMIPSTest, Sltu) {
  DriverStr(RepeatRRR(&mips::MipsAssembler::Sltu, "sltu ${reg1}, ${reg2}, ${reg3}"), "Sltu");
}

TEST_F(AssemblerMIPSTest, Slti) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Slti, -16, "slti ${reg1}, ${reg2}, {imm}"), "Slti");
}

TEST_F(AssemblerMIPSTest, Sltiu) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Sltiu, -16, "sltiu ${reg1}, ${reg2}, {imm}"), "Sltiu");
}

TEST_F(AssemblerMIPSTest, AddS) {
  DriverStr(RepeatFFF(&mips::MipsAssembler::AddS, "add.s ${reg1}, ${reg2}, ${reg3}"), "AddS");
}

TEST_F(AssemblerMIPSTest, AddD) {
  DriverStr(RepeatFFF(&mips::MipsAssembler::AddD, "add.d ${reg1}, ${reg2}, ${reg3}"), "AddD");
}

TEST_F(AssemblerMIPSTest, SubS) {
  DriverStr(RepeatFFF(&mips::MipsAssembler::SubS, "sub.s ${reg1}, ${reg2}, ${reg3}"), "SubS");
}

TEST_F(AssemblerMIPSTest, SubD) {
  DriverStr(RepeatFFF(&mips::MipsAssembler::SubD, "sub.d ${reg1}, ${reg2}, ${reg3}"), "SubD");
}

TEST_F(AssemblerMIPSTest, MulS) {
  DriverStr(RepeatFFF(&mips::MipsAssembler::MulS, "mul.s ${reg1}, ${reg2}, ${reg3}"), "MulS");
}

TEST_F(AssemblerMIPSTest, MulD) {
  DriverStr(RepeatFFF(&mips::MipsAssembler::MulD, "mul.d ${reg1}, ${reg2}, ${reg3}"), "MulD");
}

TEST_F(AssemblerMIPSTest, DivS) {
  DriverStr(RepeatFFF(&mips::MipsAssembler::DivS, "div.s ${reg1}, ${reg2}, ${reg3}"), "DivS");
}

TEST_F(AssemblerMIPSTest, DivD) {
  DriverStr(RepeatFFF(&mips::MipsAssembler::DivD, "div.d ${reg1}, ${reg2}, ${reg3}"), "DivD");
}

TEST_F(AssemblerMIPSTest, MovS) {
  DriverStr(RepeatFF(&mips::MipsAssembler::MovS, "mov.s ${reg1}, ${reg2}"), "MovS");
}

TEST_F(AssemblerMIPSTest, MovD) {
  DriverStr(RepeatFF(&mips::MipsAssembler::MovD, "mov.d ${reg1}, ${reg2}"), "MovD");
}

TEST_F(AssemblerMIPSTest, NegS) {
  DriverStr(RepeatFF(&mips::MipsAssembler::NegS, "neg.s ${reg1}, ${reg2}"), "NegS");
}

TEST_F(AssemblerMIPSTest, NegD) {
  DriverStr(RepeatFF(&mips::MipsAssembler::NegD, "neg.d ${reg1}, ${reg2}"), "NegD");
}

TEST_F(AssemblerMIPSTest, CunS) {
  DriverStr(RepeatIbFF(&mips::MipsAssembler::CunS, 3, "c.un.s $fcc{imm}, ${reg1}, ${reg2}"),
            "CunS");
}

TEST_F(AssemblerMIPSTest, CeqS) {
  DriverStr(RepeatIbFF(&mips::MipsAssembler::CeqS, 3, "c.eq.s $fcc{imm}, ${reg1}, ${reg2}"),
            "CeqS");
}

TEST_F(AssemblerMIPSTest, CueqS) {
  DriverStr(RepeatIbFF(&mips::MipsAssembler::CueqS, 3, "c.ueq.s $fcc{imm}, ${reg1}, ${reg2}"),
            "CueqS");
}

TEST_F(AssemblerMIPSTest, ColtS) {
  DriverStr(RepeatIbFF(&mips::MipsAssembler::ColtS, 3, "c.olt.s $fcc{imm}, ${reg1}, ${reg2}"),
            "ColtS");
}

TEST_F(AssemblerMIPSTest, CultS) {
  DriverStr(RepeatIbFF(&mips::MipsAssembler::CultS, 3, "c.ult.s $fcc{imm}, ${reg1}, ${reg2}"),
            "CultS");
}

TEST_F(AssemblerMIPSTest, ColeS) {
  DriverStr(RepeatIbFF(&mips::MipsAssembler::ColeS, 3, "c.ole.s $fcc{imm}, ${reg1}, ${reg2}"),
            "ColeS");
}

TEST_F(AssemblerMIPSTest, CuleS) {
  DriverStr(RepeatIbFF(&mips::MipsAssembler::CuleS, 3, "c.ule.s $fcc{imm}, ${reg1}, ${reg2}"),
            "CuleS");
}

TEST_F(AssemblerMIPSTest, CunD) {
  DriverStr(RepeatIbFF(&mips::MipsAssembler::CunD, 3, "c.un.d $fcc{imm}, ${reg1}, ${reg2}"),
            "CunD");
}

TEST_F(AssemblerMIPSTest, CeqD) {
  DriverStr(RepeatIbFF(&mips::MipsAssembler::CeqD, 3, "c.eq.d $fcc{imm}, ${reg1}, ${reg2}"),
            "CeqD");
}

TEST_F(AssemblerMIPSTest, CueqD) {
  DriverStr(RepeatIbFF(&mips::MipsAssembler::CueqD, 3, "c.ueq.d $fcc{imm}, ${reg1}, ${reg2}"),
            "CueqD");
}

TEST_F(AssemblerMIPSTest, ColtD) {
  DriverStr(RepeatIbFF(&mips::MipsAssembler::ColtD, 3, "c.olt.d $fcc{imm}, ${reg1}, ${reg2}"),
            "ColtD");
}

TEST_F(AssemblerMIPSTest, CultD) {
  DriverStr(RepeatIbFF(&mips::MipsAssembler::CultD, 3, "c.ult.d $fcc{imm}, ${reg1}, ${reg2}"),
            "CultD");
}

TEST_F(AssemblerMIPSTest, ColeD) {
  DriverStr(RepeatIbFF(&mips::MipsAssembler::ColeD, 3, "c.ole.d $fcc{imm}, ${reg1}, ${reg2}"),
            "ColeD");
}

TEST_F(AssemblerMIPSTest, CuleD) {
  DriverStr(RepeatIbFF(&mips::MipsAssembler::CuleD, 3, "c.ule.d $fcc{imm}, ${reg1}, ${reg2}"),
            "CuleD");
}

TEST_F(AssemblerMIPSTest, Movf) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Movf, 3, "movf ${reg1}, ${reg2}, $fcc{imm}"), "Movf");
}

TEST_F(AssemblerMIPSTest, Movt) {
  DriverStr(RepeatRRIb(&mips::MipsAssembler::Movt, 3, "movt ${reg1}, ${reg2}, $fcc{imm}"), "Movt");
}

TEST_F(AssemblerMIPSTest, CvtSW) {
  DriverStr(RepeatFF(&mips::MipsAssembler::Cvtsw, "cvt.s.w ${reg1}, ${reg2}"), "CvtSW");
}

TEST_F(AssemblerMIPSTest, CvtDW) {
  DriverStr(RepeatFF(&mips::MipsAssembler::Cvtdw, "cvt.d.w ${reg1}, ${reg2}"), "CvtDW");
}

TEST_F(AssemblerMIPSTest, CvtSL) {
  DriverStr(RepeatFF(&mips::MipsAssembler::Cvtsl, "cvt.s.l ${reg1}, ${reg2}"), "CvtSL");
}

TEST_F(AssemblerMIPSTest, CvtDL) {
  DriverStr(RepeatFF(&mips::MipsAssembler::Cvtdl, "cvt.d.l ${reg1}, ${reg2}"), "CvtDL");
}

TEST_F(AssemblerMIPSTest, CvtSD) {
  DriverStr(RepeatFF(&mips::MipsAssembler::Cvtsd, "cvt.s.d ${reg1}, ${reg2}"), "CvtSD");
}

TEST_F(AssemblerMIPSTest, CvtDS) {
  DriverStr(RepeatFF(&mips::MipsAssembler::Cvtds, "cvt.d.s ${reg1}, ${reg2}"), "CvtDS");
}

TEST_F(AssemblerMIPSTest, TruncWS) {
  DriverStr(RepeatFF(&mips::MipsAssembler::TruncWS, "trunc.w.s ${reg1}, ${reg2}"), "TruncWS");
}

TEST_F(AssemblerMIPSTest, TruncWD) {
  DriverStr(RepeatFF(&mips::MipsAssembler::TruncWD, "trunc.w.d ${reg1}, ${reg2}"), "TruncWD");
}

TEST_F(AssemblerMIPSTest, TruncLS) {
  DriverStr(RepeatFF(&mips::MipsAssembler::TruncLS, "trunc.l.s ${reg1}, ${reg2}"), "TruncLS");
}

TEST_F(AssemblerMIPSTest, TruncLD) {
  DriverStr(RepeatFF(&mips::MipsAssembler::TruncLD, "trunc.l.d ${reg1}, ${reg2}"), "TruncLD");
}

TEST_F(AssemblerMIPSTest, Mfc1) {
  DriverStr(RepeatRF(&mips::MipsAssembler::Mfc1, "mfc1 ${reg1}, ${reg2}"), "Mfc1");
}

TEST_F(AssemblerMIPSTest, Mtc1) {
  DriverStr(RepeatRF(&mips::MipsAssembler::Mtc1, "mtc1 ${reg1}, ${reg2}"), "Mtc1");
}

TEST_F(AssemblerMIPSTest, Mfhc1) {
  DriverStr(RepeatRF(&mips::MipsAssembler::Mfhc1, "mfhc1 ${reg1}, ${reg2}"), "Mfhc1");
}

TEST_F(AssemblerMIPSTest, Mthc1) {
  DriverStr(RepeatRF(&mips::MipsAssembler::Mthc1, "mthc1 ${reg1}, ${reg2}"), "Mthc1");
}

TEST_F(AssemblerMIPSTest, Lwc1) {
  DriverStr(RepeatFRIb(&mips::MipsAssembler::Lwc1, -16, "lwc1 ${reg1}, {imm}(${reg2})"), "Lwc1");
}

TEST_F(AssemblerMIPSTest, Ldc1) {
  DriverStr(RepeatFRIb(&mips::MipsAssembler::Ldc1, -16, "ldc1 ${reg1}, {imm}(${reg2})"), "Ldc1");
}

TEST_F(AssemblerMIPSTest, Swc1) {
  DriverStr(RepeatFRIb(&mips::MipsAssembler::Swc1, -16, "swc1 ${reg1}, {imm}(${reg2})"), "Swc1");
}

TEST_F(AssemblerMIPSTest, Sdc1) {
  DriverStr(RepeatFRIb(&mips::MipsAssembler::Sdc1, -16, "sdc1 ${reg1}, {imm}(${reg2})"), "Sdc1");
}

TEST_F(AssemblerMIPSTest, Move) {
  DriverStr(RepeatRR(&mips::MipsAssembler::Move, "or ${reg1}, ${reg2}, $zero"), "Move");
}

TEST_F(AssemblerMIPSTest, Clear) {
  DriverStr(RepeatR(&mips::MipsAssembler::Clear, "or ${reg}, $zero, $zero"), "Clear");
}

TEST_F(AssemblerMIPSTest, Not) {
  DriverStr(RepeatRR(&mips::MipsAssembler::Not, "nor ${reg1}, ${reg2}, $zero"), "Not");
}

TEST_F(AssemblerMIPSTest, LoadFromOffset) {
  __ LoadFromOffset(mips::kLoadSignedByte, mips::A0, mips::A0, 0);
  __ LoadFromOffset(mips::kLoadSignedByte, mips::A0, mips::A1, 0);
  __ LoadFromOffset(mips::kLoadSignedByte, mips::A0, mips::A1, 256);
  __ LoadFromOffset(mips::kLoadSignedByte, mips::A0, mips::A1, 1000);
  __ LoadFromOffset(mips::kLoadSignedByte, mips::A0, mips::A1, 0x8000);
  __ LoadFromOffset(mips::kLoadSignedByte, mips::A0, mips::A1, 0x10000);
  __ LoadFromOffset(mips::kLoadSignedByte, mips::A0, mips::A1, 0x12345678);
  __ LoadFromOffset(mips::kLoadSignedByte, mips::A0, mips::A1, -256);
  __ LoadFromOffset(mips::kLoadSignedByte, mips::A0, mips::A1, 0xFFFF8000);
  __ LoadFromOffset(mips::kLoadSignedByte, mips::A0, mips::A1, 0xABCDEF00);

  __ LoadFromOffset(mips::kLoadUnsignedByte, mips::A0, mips::A0, 0);
  __ LoadFromOffset(mips::kLoadUnsignedByte, mips::A0, mips::A1, 0);
  __ LoadFromOffset(mips::kLoadUnsignedByte, mips::A0, mips::A1, 256);
  __ LoadFromOffset(mips::kLoadUnsignedByte, mips::A0, mips::A1, 1000);
  __ LoadFromOffset(mips::kLoadUnsignedByte, mips::A0, mips::A1, 0x8000);
  __ LoadFromOffset(mips::kLoadUnsignedByte, mips::A0, mips::A1, 0x10000);
  __ LoadFromOffset(mips::kLoadUnsignedByte, mips::A0, mips::A1, 0x12345678);
  __ LoadFromOffset(mips::kLoadUnsignedByte, mips::A0, mips::A1, -256);
  __ LoadFromOffset(mips::kLoadUnsignedByte, mips::A0, mips::A1, 0xFFFF8000);
  __ LoadFromOffset(mips::kLoadUnsignedByte, mips::A0, mips::A1, 0xABCDEF00);

  __ LoadFromOffset(mips::kLoadSignedHalfword, mips::A0, mips::A0, 0);
  __ LoadFromOffset(mips::kLoadSignedHalfword, mips::A0, mips::A1, 0);
  __ LoadFromOffset(mips::kLoadSignedHalfword, mips::A0, mips::A1, 256);
  __ LoadFromOffset(mips::kLoadSignedHalfword, mips::A0, mips::A1, 1000);
  __ LoadFromOffset(mips::kLoadSignedHalfword, mips::A0, mips::A1, 0x8000);
  __ LoadFromOffset(mips::kLoadSignedHalfword, mips::A0, mips::A1, 0x10000);
  __ LoadFromOffset(mips::kLoadSignedHalfword, mips::A0, mips::A1, 0x12345678);
  __ LoadFromOffset(mips::kLoadSignedHalfword, mips::A0, mips::A1, -256);
  __ LoadFromOffset(mips::kLoadSignedHalfword, mips::A0, mips::A1, 0xFFFF8000);
  __ LoadFromOffset(mips::kLoadSignedHalfword, mips::A0, mips::A1, 0xABCDEF00);

  __ LoadFromOffset(mips::kLoadUnsignedHalfword, mips::A0, mips::A0, 0);
  __ LoadFromOffset(mips::kLoadUnsignedHalfword, mips::A0, mips::A1, 0);
  __ LoadFromOffset(mips::kLoadUnsignedHalfword, mips::A0, mips::A1, 256);
  __ LoadFromOffset(mips::kLoadUnsignedHalfword, mips::A0, mips::A1, 1000);
  __ LoadFromOffset(mips::kLoadUnsignedHalfword, mips::A0, mips::A1, 0x8000);
  __ LoadFromOffset(mips::kLoadUnsignedHalfword, mips::A0, mips::A1, 0x10000);
  __ LoadFromOffset(mips::kLoadUnsignedHalfword, mips::A0, mips::A1, 0x12345678);
  __ LoadFromOffset(mips::kLoadUnsignedHalfword, mips::A0, mips::A1, -256);
  __ LoadFromOffset(mips::kLoadUnsignedHalfword, mips::A0, mips::A1, 0xFFFF8000);
  __ LoadFromOffset(mips::kLoadUnsignedHalfword, mips::A0, mips::A1, 0xABCDEF00);

  __ LoadFromOffset(mips::kLoadWord, mips::A0, mips::A0, 0);
  __ LoadFromOffset(mips::kLoadWord, mips::A0, mips::A1, 0);
  __ LoadFromOffset(mips::kLoadWord, mips::A0, mips::A1, 256);
  __ LoadFromOffset(mips::kLoadWord, mips::A0, mips::A1, 1000);
  __ LoadFromOffset(mips::kLoadWord, mips::A0, mips::A1, 0x8000);
  __ LoadFromOffset(mips::kLoadWord, mips::A0, mips::A1, 0x10000);
  __ LoadFromOffset(mips::kLoadWord, mips::A0, mips::A1, 0x12345678);
  __ LoadFromOffset(mips::kLoadWord, mips::A0, mips::A1, -256);
  __ LoadFromOffset(mips::kLoadWord, mips::A0, mips::A1, 0xFFFF8000);
  __ LoadFromOffset(mips::kLoadWord, mips::A0, mips::A1, 0xABCDEF00);

  __ LoadFromOffset(mips::kLoadDoubleword, mips::A0, mips::A0, 0);
  __ LoadFromOffset(mips::kLoadDoubleword, mips::A0, mips::A1, 0);
  __ LoadFromOffset(mips::kLoadDoubleword, mips::A1, mips::A0, 0);
  __ LoadFromOffset(mips::kLoadDoubleword, mips::A0, mips::A2, 0);
  __ LoadFromOffset(mips::kLoadDoubleword, mips::A0, mips::A2, 256);
  __ LoadFromOffset(mips::kLoadDoubleword, mips::A0, mips::A2, 1000);
  __ LoadFromOffset(mips::kLoadDoubleword, mips::A0, mips::A2, 0x8000);
  __ LoadFromOffset(mips::kLoadDoubleword, mips::A0, mips::A2, 0x10000);
  __ LoadFromOffset(mips::kLoadDoubleword, mips::A0, mips::A2, 0x12345678);
  __ LoadFromOffset(mips::kLoadDoubleword, mips::A0, mips::A2, -256);
  __ LoadFromOffset(mips::kLoadDoubleword, mips::A0, mips::A2, 0xFFFF8000);
  __ LoadFromOffset(mips::kLoadDoubleword, mips::A0, mips::A2, 0xABCDEF00);

  const char* expected =
      "lb $a0, 0($a0)\n"
      "lb $a0, 0($a1)\n"
      "lb $a0, 256($a1)\n"
      "lb $a0, 1000($a1)\n"
      "ori $at, $zero, 0x8000\n"
      "addu $at, $at, $a1\n"
      "lb $a0, 0($at)\n"
      "lui $at, 1\n"
      "addu $at, $at, $a1\n"
      "lb $a0, 0($at)\n"
      "lui $at, 0x1234\n"
      "ori $at, 0x5678\n"
      "addu $at, $at, $a1\n"
      "lb $a0, 0($at)\n"
      "lb $a0, -256($a1)\n"
      "lb $a0, 0xFFFF8000($a1)\n"
      "lui $at, 0xABCD\n"
      "ori $at, 0xEF00\n"
      "addu $at, $at, $a1\n"
      "lb $a0, 0($at)\n"

      "lbu $a0, 0($a0)\n"
      "lbu $a0, 0($a1)\n"
      "lbu $a0, 256($a1)\n"
      "lbu $a0, 1000($a1)\n"
      "ori $at, $zero, 0x8000\n"
      "addu $at, $at, $a1\n"
      "lbu $a0, 0($at)\n"
      "lui $at, 1\n"
      "addu $at, $at, $a1\n"
      "lbu $a0, 0($at)\n"
      "lui $at, 0x1234\n"
      "ori $at, 0x5678\n"
      "addu $at, $at, $a1\n"
      "lbu $a0, 0($at)\n"
      "lbu $a0, -256($a1)\n"
      "lbu $a0, 0xFFFF8000($a1)\n"
      "lui $at, 0xABCD\n"
      "ori $at, 0xEF00\n"
      "addu $at, $at, $a1\n"
      "lbu $a0, 0($at)\n"

      "lh $a0, 0($a0)\n"
      "lh $a0, 0($a1)\n"
      "lh $a0, 256($a1)\n"
      "lh $a0, 1000($a1)\n"
      "ori $at, $zero, 0x8000\n"
      "addu $at, $at, $a1\n"
      "lh $a0, 0($at)\n"
      "lui $at, 1\n"
      "addu $at, $at, $a1\n"
      "lh $a0, 0($at)\n"
      "lui $at, 0x1234\n"
      "ori $at, 0x5678\n"
      "addu $at, $at, $a1\n"
      "lh $a0, 0($at)\n"
      "lh $a0, -256($a1)\n"
      "lh $a0, 0xFFFF8000($a1)\n"
      "lui $at, 0xABCD\n"
      "ori $at, 0xEF00\n"
      "addu $at, $at, $a1\n"
      "lh $a0, 0($at)\n"

      "lhu $a0, 0($a0)\n"
      "lhu $a0, 0($a1)\n"
      "lhu $a0, 256($a1)\n"
      "lhu $a0, 1000($a1)\n"
      "ori $at, $zero, 0x8000\n"
      "addu $at, $at, $a1\n"
      "lhu $a0, 0($at)\n"
      "lui $at, 1\n"
      "addu $at, $at, $a1\n"
      "lhu $a0, 0($at)\n"
      "lui $at, 0x1234\n"
      "ori $at, 0x5678\n"
      "addu $at, $at, $a1\n"
      "lhu $a0, 0($at)\n"
      "lhu $a0, -256($a1)\n"
      "lhu $a0, 0xFFFF8000($a1)\n"
      "lui $at, 0xABCD\n"
      "ori $at, 0xEF00\n"
      "addu $at, $at, $a1\n"
      "lhu $a0, 0($at)\n"

      "lw $a0, 0($a0)\n"
      "lw $a0, 0($a1)\n"
      "lw $a0, 256($a1)\n"
      "lw $a0, 1000($a1)\n"
      "ori $at, $zero, 0x8000\n"
      "addu $at, $at, $a1\n"
      "lw $a0, 0($at)\n"
      "lui $at, 1\n"
      "addu $at, $at, $a1\n"
      "lw $a0, 0($at)\n"
      "lui $at, 0x1234\n"
      "ori $at, 0x5678\n"
      "addu $at, $at, $a1\n"
      "lw $a0, 0($at)\n"
      "lw $a0, -256($a1)\n"
      "lw $a0, 0xFFFF8000($a1)\n"
      "lui $at, 0xABCD\n"
      "ori $at, 0xEF00\n"
      "addu $at, $at, $a1\n"
      "lw $a0, 0($at)\n"

      "lw $a1, 4($a0)\n"
      "lw $a0, 0($a0)\n"
      "lw $a0, 0($a1)\n"
      "lw $a1, 4($a1)\n"
      "lw $a1, 0($a0)\n"
      "lw $a2, 4($a0)\n"
      "lw $a0, 0($a2)\n"
      "lw $a1, 4($a2)\n"
      "lw $a0, 256($a2)\n"
      "lw $a1, 260($a2)\n"
      "lw $a0, 1000($a2)\n"
      "lw $a1, 1004($a2)\n"
      "ori $at, $zero, 0x8000\n"
      "addu $at, $at, $a2\n"
      "lw $a0, 0($at)\n"
      "lw $a1, 4($at)\n"
      "lui $at, 1\n"
      "addu $at, $at, $a2\n"
      "lw $a0, 0($at)\n"
      "lw $a1, 4($at)\n"
      "lui $at, 0x1234\n"
      "ori $at, 0x5678\n"
      "addu $at, $at, $a2\n"
      "lw $a0, 0($at)\n"
      "lw $a1, 4($at)\n"
      "lw $a0, -256($a2)\n"
      "lw $a1, -252($a2)\n"
      "lw $a0, 0xFFFF8000($a2)\n"
      "lw $a1, 0xFFFF8004($a2)\n"
      "lui $at, 0xABCD\n"
      "ori $at, 0xEF00\n"
      "addu $at, $at, $a2\n"
      "lw $a0, 0($at)\n"
      "lw $a1, 4($at)\n";
  DriverStr(expected, "LoadFromOffset");
}

TEST_F(AssemblerMIPSTest, LoadSFromOffset) {
  __ LoadSFromOffset(mips::F0, mips::A0, 0);
  __ LoadSFromOffset(mips::F0, mips::A0, 4);
  __ LoadSFromOffset(mips::F0, mips::A0, 256);
  __ LoadSFromOffset(mips::F0, mips::A0, 0x8000);
  __ LoadSFromOffset(mips::F0, mips::A0, 0x10000);
  __ LoadSFromOffset(mips::F0, mips::A0, 0x12345678);
  __ LoadSFromOffset(mips::F0, mips::A0, -256);
  __ LoadSFromOffset(mips::F0, mips::A0, 0xFFFF8000);
  __ LoadSFromOffset(mips::F0, mips::A0, 0xABCDEF00);

  const char* expected =
      "lwc1 $f0, 0($a0)\n"
      "lwc1 $f0, 4($a0)\n"
      "lwc1 $f0, 256($a0)\n"
      "ori $at, $zero, 0x8000\n"
      "addu $at, $at, $a0\n"
      "lwc1 $f0, 0($at)\n"
      "lui $at, 1\n"
      "addu $at, $at, $a0\n"
      "lwc1 $f0, 0($at)\n"
      "lui $at, 0x1234\n"
      "ori $at, 0x5678\n"
      "addu $at, $at, $a0\n"
      "lwc1 $f0, 0($at)\n"
      "lwc1 $f0, -256($a0)\n"
      "lwc1 $f0, 0xFFFF8000($a0)\n"
      "lui $at, 0xABCD\n"
      "ori $at, 0xEF00\n"
      "addu $at, $at, $a0\n"
      "lwc1 $f0, 0($at)\n";
  DriverStr(expected, "LoadSFromOffset");
}


TEST_F(AssemblerMIPSTest, LoadDFromOffset) {
  __ LoadDFromOffset(mips::F0, mips::A0, 0);
  __ LoadDFromOffset(mips::F0, mips::A0, 4);
  __ LoadDFromOffset(mips::F0, mips::A0, 256);
  __ LoadDFromOffset(mips::F0, mips::A0, 0x8000);
  __ LoadDFromOffset(mips::F0, mips::A0, 0x10000);
  __ LoadDFromOffset(mips::F0, mips::A0, 0x12345678);
  __ LoadDFromOffset(mips::F0, mips::A0, -256);
  __ LoadDFromOffset(mips::F0, mips::A0, 0xFFFF8000);
  __ LoadDFromOffset(mips::F0, mips::A0, 0xABCDEF00);

  const char* expected =
      "ldc1 $f0, 0($a0)\n"
      "lwc1 $f0, 4($a0)\n"
      "lwc1 $f1, 8($a0)\n"
      "ldc1 $f0, 256($a0)\n"
      "ori $at, $zero, 0x8000\n"
      "addu $at, $at, $a0\n"
      "ldc1 $f0, 0($at)\n"
      "lui $at, 1\n"
      "addu $at, $at, $a0\n"
      "ldc1 $f0, 0($at)\n"
      "lui $at, 0x1234\n"
      "ori $at, 0x5678\n"
      "addu $at, $at, $a0\n"
      "ldc1 $f0, 0($at)\n"
      "ldc1 $f0, -256($a0)\n"
      "ldc1 $f0, 0xFFFF8000($a0)\n"
      "lui $at, 0xABCD\n"
      "ori $at, 0xEF00\n"
      "addu $at, $at, $a0\n"
      "ldc1 $f0, 0($at)\n";
  DriverStr(expected, "LoadDFromOffset");
}

TEST_F(AssemblerMIPSTest, StoreToOffset) {
  __ StoreToOffset(mips::kStoreByte, mips::A0, mips::A0, 0);
  __ StoreToOffset(mips::kStoreByte, mips::A0, mips::A1, 0);
  __ StoreToOffset(mips::kStoreByte, mips::A0, mips::A1, 256);
  __ StoreToOffset(mips::kStoreByte, mips::A0, mips::A1, 1000);
  __ StoreToOffset(mips::kStoreByte, mips::A0, mips::A1, 0x8000);
  __ StoreToOffset(mips::kStoreByte, mips::A0, mips::A1, 0x10000);
  __ StoreToOffset(mips::kStoreByte, mips::A0, mips::A1, 0x12345678);
  __ StoreToOffset(mips::kStoreByte, mips::A0, mips::A1, -256);
  __ StoreToOffset(mips::kStoreByte, mips::A0, mips::A1, 0xFFFF8000);
  __ StoreToOffset(mips::kStoreByte, mips::A0, mips::A1, 0xABCDEF00);

  __ StoreToOffset(mips::kStoreHalfword, mips::A0, mips::A0, 0);
  __ StoreToOffset(mips::kStoreHalfword, mips::A0, mips::A1, 0);
  __ StoreToOffset(mips::kStoreHalfword, mips::A0, mips::A1, 256);
  __ StoreToOffset(mips::kStoreHalfword, mips::A0, mips::A1, 1000);
  __ StoreToOffset(mips::kStoreHalfword, mips::A0, mips::A1, 0x8000);
  __ StoreToOffset(mips::kStoreHalfword, mips::A0, mips::A1, 0x10000);
  __ StoreToOffset(mips::kStoreHalfword, mips::A0, mips::A1, 0x12345678);
  __ StoreToOffset(mips::kStoreHalfword, mips::A0, mips::A1, -256);
  __ StoreToOffset(mips::kStoreHalfword, mips::A0, mips::A1, 0xFFFF8000);
  __ StoreToOffset(mips::kStoreHalfword, mips::A0, mips::A1, 0xABCDEF00);

  __ StoreToOffset(mips::kStoreWord, mips::A0, mips::A0, 0);
  __ StoreToOffset(mips::kStoreWord, mips::A0, mips::A1, 0);
  __ StoreToOffset(mips::kStoreWord, mips::A0, mips::A1, 256);
  __ StoreToOffset(mips::kStoreWord, mips::A0, mips::A1, 1000);
  __ StoreToOffset(mips::kStoreWord, mips::A0, mips::A1, 0x8000);
  __ StoreToOffset(mips::kStoreWord, mips::A0, mips::A1, 0x10000);
  __ StoreToOffset(mips::kStoreWord, mips::A0, mips::A1, 0x12345678);
  __ StoreToOffset(mips::kStoreWord, mips::A0, mips::A1, -256);
  __ StoreToOffset(mips::kStoreWord, mips::A0, mips::A1, 0xFFFF8000);
  __ StoreToOffset(mips::kStoreWord, mips::A0, mips::A1, 0xABCDEF00);

  __ StoreToOffset(mips::kStoreDoubleword, mips::A0, mips::A2, 0);
  __ StoreToOffset(mips::kStoreDoubleword, mips::A0, mips::A2, 256);
  __ StoreToOffset(mips::kStoreDoubleword, mips::A0, mips::A2, 1000);
  __ StoreToOffset(mips::kStoreDoubleword, mips::A0, mips::A2, 0x8000);
  __ StoreToOffset(mips::kStoreDoubleword, mips::A0, mips::A2, 0x10000);
  __ StoreToOffset(mips::kStoreDoubleword, mips::A0, mips::A2, 0x12345678);
  __ StoreToOffset(mips::kStoreDoubleword, mips::A0, mips::A2, -256);
  __ StoreToOffset(mips::kStoreDoubleword, mips::A0, mips::A2, 0xFFFF8000);
  __ StoreToOffset(mips::kStoreDoubleword, mips::A0, mips::A2, 0xABCDEF00);

  const char* expected =
      "sb $a0, 0($a0)\n"
      "sb $a0, 0($a1)\n"
      "sb $a0, 256($a1)\n"
      "sb $a0, 1000($a1)\n"
      "ori $at, $zero, 0x8000\n"
      "addu $at, $at, $a1\n"
      "sb $a0, 0($at)\n"
      "lui $at, 1\n"
      "addu $at, $at, $a1\n"
      "sb $a0, 0($at)\n"
      "lui $at, 0x1234\n"
      "ori $at, 0x5678\n"
      "addu $at, $at, $a1\n"
      "sb $a0, 0($at)\n"
      "sb $a0, -256($a1)\n"
      "sb $a0, 0xFFFF8000($a1)\n"
      "lui $at, 0xABCD\n"
      "ori $at, 0xEF00\n"
      "addu $at, $at, $a1\n"
      "sb $a0, 0($at)\n"

      "sh $a0, 0($a0)\n"
      "sh $a0, 0($a1)\n"
      "sh $a0, 256($a1)\n"
      "sh $a0, 1000($a1)\n"
      "ori $at, $zero, 0x8000\n"
      "addu $at, $at, $a1\n"
      "sh $a0, 0($at)\n"
      "lui $at, 1\n"
      "addu $at, $at, $a1\n"
      "sh $a0, 0($at)\n"
      "lui $at, 0x1234\n"
      "ori $at, 0x5678\n"
      "addu $at, $at, $a1\n"
      "sh $a0, 0($at)\n"
      "sh $a0, -256($a1)\n"
      "sh $a0, 0xFFFF8000($a1)\n"
      "lui $at, 0xABCD\n"
      "ori $at, 0xEF00\n"
      "addu $at, $at, $a1\n"
      "sh $a0, 0($at)\n"

      "sw $a0, 0($a0)\n"
      "sw $a0, 0($a1)\n"
      "sw $a0, 256($a1)\n"
      "sw $a0, 1000($a1)\n"
      "ori $at, $zero, 0x8000\n"
      "addu $at, $at, $a1\n"
      "sw $a0, 0($at)\n"
      "lui $at, 1\n"
      "addu $at, $at, $a1\n"
      "sw $a0, 0($at)\n"
      "lui $at, 0x1234\n"
      "ori $at, 0x5678\n"
      "addu $at, $at, $a1\n"
      "sw $a0, 0($at)\n"
      "sw $a0, -256($a1)\n"
      "sw $a0, 0xFFFF8000($a1)\n"
      "lui $at, 0xABCD\n"
      "ori $at, 0xEF00\n"
      "addu $at, $at, $a1\n"
      "sw $a0, 0($at)\n"

      "sw $a0, 0($a2)\n"
      "sw $a1, 4($a2)\n"
      "sw $a0, 256($a2)\n"
      "sw $a1, 260($a2)\n"
      "sw $a0, 1000($a2)\n"
      "sw $a1, 1004($a2)\n"
      "ori $at, $zero, 0x8000\n"
      "addu $at, $at, $a2\n"
      "sw $a0, 0($at)\n"
      "sw $a1, 4($at)\n"
      "lui $at, 1\n"
      "addu $at, $at, $a2\n"
      "sw $a0, 0($at)\n"
      "sw $a1, 4($at)\n"
      "lui $at, 0x1234\n"
      "ori $at, 0x5678\n"
      "addu $at, $at, $a2\n"
      "sw $a0, 0($at)\n"
      "sw $a1, 4($at)\n"
      "sw $a0, -256($a2)\n"
      "sw $a1, -252($a2)\n"
      "sw $a0, 0xFFFF8000($a2)\n"
      "sw $a1, 0xFFFF8004($a2)\n"
      "lui $at, 0xABCD\n"
      "ori $at, 0xEF00\n"
      "addu $at, $at, $a2\n"
      "sw $a0, 0($at)\n"
      "sw $a1, 4($at)\n";
  DriverStr(expected, "StoreToOffset");
}

TEST_F(AssemblerMIPSTest, StoreSToOffset) {
  __ StoreSToOffset(mips::F0, mips::A0, 0);
  __ StoreSToOffset(mips::F0, mips::A0, 4);
  __ StoreSToOffset(mips::F0, mips::A0, 256);
  __ StoreSToOffset(mips::F0, mips::A0, 0x8000);
  __ StoreSToOffset(mips::F0, mips::A0, 0x10000);
  __ StoreSToOffset(mips::F0, mips::A0, 0x12345678);
  __ StoreSToOffset(mips::F0, mips::A0, -256);
  __ StoreSToOffset(mips::F0, mips::A0, 0xFFFF8000);
  __ StoreSToOffset(mips::F0, mips::A0, 0xABCDEF00);

  const char* expected =
      "swc1 $f0, 0($a0)\n"
      "swc1 $f0, 4($a0)\n"
      "swc1 $f0, 256($a0)\n"
      "ori $at, $zero, 0x8000\n"
      "addu $at, $at, $a0\n"
      "swc1 $f0, 0($at)\n"
      "lui $at, 1\n"
      "addu $at, $at, $a0\n"
      "swc1 $f0, 0($at)\n"
      "lui $at, 0x1234\n"
      "ori $at, 0x5678\n"
      "addu $at, $at, $a0\n"
      "swc1 $f0, 0($at)\n"
      "swc1 $f0, -256($a0)\n"
      "swc1 $f0, 0xFFFF8000($a0)\n"
      "lui $at, 0xABCD\n"
      "ori $at, 0xEF00\n"
      "addu $at, $at, $a0\n"
      "swc1 $f0, 0($at)\n";
  DriverStr(expected, "StoreSToOffset");
}

TEST_F(AssemblerMIPSTest, StoreDToOffset) {
  __ StoreDToOffset(mips::F0, mips::A0, 0);
  __ StoreDToOffset(mips::F0, mips::A0, 4);
  __ StoreDToOffset(mips::F0, mips::A0, 256);
  __ StoreDToOffset(mips::F0, mips::A0, 0x8000);
  __ StoreDToOffset(mips::F0, mips::A0, 0x10000);
  __ StoreDToOffset(mips::F0, mips::A0, 0x12345678);
  __ StoreDToOffset(mips::F0, mips::A0, -256);
  __ StoreDToOffset(mips::F0, mips::A0, 0xFFFF8000);
  __ StoreDToOffset(mips::F0, mips::A0, 0xABCDEF00);

  const char* expected =
      "sdc1 $f0, 0($a0)\n"
      "swc1 $f0, 4($a0)\n"
      "swc1 $f1, 8($a0)\n"
      "sdc1 $f0, 256($a0)\n"
      "ori $at, $zero, 0x8000\n"
      "addu $at, $at, $a0\n"
      "sdc1 $f0, 0($at)\n"
      "lui $at, 1\n"
      "addu $at, $at, $a0\n"
      "sdc1 $f0, 0($at)\n"
      "lui $at, 0x1234\n"
      "ori $at, 0x5678\n"
      "addu $at, $at, $a0\n"
      "sdc1 $f0, 0($at)\n"
      "sdc1 $f0, -256($a0)\n"
      "sdc1 $f0, 0xFFFF8000($a0)\n"
      "lui $at, 0xABCD\n"
      "ori $at, 0xEF00\n"
      "addu $at, $at, $a0\n"
      "sdc1 $f0, 0($at)\n";
  DriverStr(expected, "StoreDToOffset");
}

TEST_F(AssemblerMIPSTest, B) {
  mips::MipsLabel label1, label2;
  __ B(&label1);
  constexpr size_t kAdduCount1 = 63;
  for (size_t i = 0; i != kAdduCount1; ++i) {
    __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
  }
  __ Bind(&label1);
  __ B(&label2);
  constexpr size_t kAdduCount2 = 64;
  for (size_t i = 0; i != kAdduCount2; ++i) {
    __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
  }
  __ Bind(&label2);
  __ B(&label1);

  std::string expected =
      ".set noreorder\n"
      "b 1f\n"
      "nop\n" +
      RepeatInsn(kAdduCount1, "addu $zero, $zero, $zero\n") +
      "1:\n"
      "b 2f\n"
      "nop\n" +
      RepeatInsn(kAdduCount2, "addu $zero, $zero, $zero\n") +
      "2:\n"
      "b 1b\n"
      "nop\n";
  DriverStr(expected, "B");
}

TEST_F(AssemblerMIPSTest, Beq) {
  BranchCondTwoRegsHelper(&mips::MipsAssembler::Beq, "Beq");
}

TEST_F(AssemblerMIPSTest, Bne) {
  BranchCondTwoRegsHelper(&mips::MipsAssembler::Bne, "Bne");
}

TEST_F(AssemblerMIPSTest, Beqz) {
  mips::MipsLabel label;
  __ Beqz(mips::A0, &label);
  constexpr size_t kAdduCount1 = 63;
  for (size_t i = 0; i != kAdduCount1; ++i) {
    __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
  }
  __ Bind(&label);
  constexpr size_t kAdduCount2 = 64;
  for (size_t i = 0; i != kAdduCount2; ++i) {
    __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
  }
  __ Beqz(mips::A1, &label);

  std::string expected =
      ".set noreorder\n"
      "beq $zero, $a0, 1f\n"
      "nop\n" +
      RepeatInsn(kAdduCount1, "addu $zero, $zero, $zero\n") +
      "1:\n" +
      RepeatInsn(kAdduCount2, "addu $zero, $zero, $zero\n") +
      "beq $zero, $a1, 1b\n"
      "nop\n";
  DriverStr(expected, "Beqz");
}

TEST_F(AssemblerMIPSTest, Bnez) {
  mips::MipsLabel label;
  __ Bnez(mips::A0, &label);
  constexpr size_t kAdduCount1 = 63;
  for (size_t i = 0; i != kAdduCount1; ++i) {
    __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
  }
  __ Bind(&label);
  constexpr size_t kAdduCount2 = 64;
  for (size_t i = 0; i != kAdduCount2; ++i) {
    __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
  }
  __ Bnez(mips::A1, &label);

  std::string expected =
      ".set noreorder\n"
      "bne $zero, $a0, 1f\n"
      "nop\n" +
      RepeatInsn(kAdduCount1, "addu $zero, $zero, $zero\n") +
      "1:\n" +
      RepeatInsn(kAdduCount2, "addu $zero, $zero, $zero\n") +
      "bne $zero, $a1, 1b\n"
      "nop\n";
  DriverStr(expected, "Bnez");
}

TEST_F(AssemblerMIPSTest, Bltz) {
  BranchCondOneRegHelper(&mips::MipsAssembler::Bltz, "Bltz");
}

TEST_F(AssemblerMIPSTest, Bgez) {
  BranchCondOneRegHelper(&mips::MipsAssembler::Bgez, "Bgez");
}

TEST_F(AssemblerMIPSTest, Blez) {
  BranchCondOneRegHelper(&mips::MipsAssembler::Blez, "Blez");
}

TEST_F(AssemblerMIPSTest, Bgtz) {
  BranchCondOneRegHelper(&mips::MipsAssembler::Bgtz, "Bgtz");
}

TEST_F(AssemblerMIPSTest, Blt) {
  mips::MipsLabel label;
  __ Blt(mips::A0, mips::A1, &label);
  constexpr size_t kAdduCount1 = 63;
  for (size_t i = 0; i != kAdduCount1; ++i) {
    __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
  }
  __ Bind(&label);
  constexpr size_t kAdduCount2 = 64;
  for (size_t i = 0; i != kAdduCount2; ++i) {
    __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
  }
  __ Blt(mips::A2, mips::A3, &label);

  std::string expected =
      ".set noreorder\n"
      "slt $at, $a0, $a1\n"
      "bne $zero, $at, 1f\n"
      "nop\n" +
      RepeatInsn(kAdduCount1, "addu $zero, $zero, $zero\n") +
      "1:\n" +
      RepeatInsn(kAdduCount2, "addu $zero, $zero, $zero\n") +
      "slt $at, $a2, $a3\n"
      "bne $zero, $at, 1b\n"
      "nop\n";
  DriverStr(expected, "Blt");
}

TEST_F(AssemblerMIPSTest, Bge) {
  mips::MipsLabel label;
  __ Bge(mips::A0, mips::A1, &label);
  constexpr size_t kAdduCount1 = 63;
  for (size_t i = 0; i != kAdduCount1; ++i) {
    __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
  }
  __ Bind(&label);
  constexpr size_t kAdduCount2 = 64;
  for (size_t i = 0; i != kAdduCount2; ++i) {
    __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
  }
  __ Bge(mips::A2, mips::A3, &label);

  std::string expected =
      ".set noreorder\n"
      "slt $at, $a0, $a1\n"
      "beq $zero, $at, 1f\n"
      "nop\n" +
      RepeatInsn(kAdduCount1, "addu $zero, $zero, $zero\n") +
      "1:\n" +
      RepeatInsn(kAdduCount2, "addu $zero, $zero, $zero\n") +
      "slt $at, $a2, $a3\n"
      "beq $zero, $at, 1b\n"
      "nop\n";
  DriverStr(expected, "Bge");
}

TEST_F(AssemblerMIPSTest, Bltu) {
  mips::MipsLabel label;
  __ Bltu(mips::A0, mips::A1, &label);
  constexpr size_t kAdduCount1 = 63;
  for (size_t i = 0; i != kAdduCount1; ++i) {
    __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
  }
  __ Bind(&label);
  constexpr size_t kAdduCount2 = 64;
  for (size_t i = 0; i != kAdduCount2; ++i) {
    __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
  }
  __ Bltu(mips::A2, mips::A3, &label);

  std::string expected =
      ".set noreorder\n"
      "sltu $at, $a0, $a1\n"
      "bne $zero, $at, 1f\n"
      "nop\n" +
      RepeatInsn(kAdduCount1, "addu $zero, $zero, $zero\n") +
      "1:\n" +
      RepeatInsn(kAdduCount2, "addu $zero, $zero, $zero\n") +
      "sltu $at, $a2, $a3\n"
      "bne $zero, $at, 1b\n"
      "nop\n";
  DriverStr(expected, "Bltu");
}

TEST_F(AssemblerMIPSTest, Bgeu) {
  mips::MipsLabel label;
  __ Bgeu(mips::A0, mips::A1, &label);
  constexpr size_t kAdduCount1 = 63;
  for (size_t i = 0; i != kAdduCount1; ++i) {
    __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
  }
  __ Bind(&label);
  constexpr size_t kAdduCount2 = 64;
  for (size_t i = 0; i != kAdduCount2; ++i) {
    __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
  }
  __ Bgeu(mips::A2, mips::A3, &label);

  std::string expected =
      ".set noreorder\n"
      "sltu $at, $a0, $a1\n"
      "beq $zero, $at, 1f\n"
      "nop\n" +
      RepeatInsn(kAdduCount1, "addu $zero, $zero, $zero\n") +
      "1:\n" +
      RepeatInsn(kAdduCount2, "addu $zero, $zero, $zero\n") +
      "sltu $at, $a2, $a3\n"
      "beq $zero, $at, 1b\n"
      "nop\n";
  DriverStr(expected, "Bgeu");
}

TEST_F(AssemblerMIPSTest, Bc1f) {
  mips::MipsLabel label;
  __ Bc1f(0, &label);
  constexpr size_t kAdduCount1 = 63;
  for (size_t i = 0; i != kAdduCount1; ++i) {
    __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
  }
  __ Bind(&label);
  constexpr size_t kAdduCount2 = 64;
  for (size_t i = 0; i != kAdduCount2; ++i) {
    __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
  }
  __ Bc1f(7, &label);

  std::string expected =
      ".set noreorder\n"
      "bc1f $fcc0, 1f\n"
      "nop\n" +
      RepeatInsn(kAdduCount1, "addu $zero, $zero, $zero\n") +
      "1:\n" +
      RepeatInsn(kAdduCount2, "addu $zero, $zero, $zero\n") +
      "bc1f $fcc7, 1b\n"
      "nop\n";
  DriverStr(expected, "Bc1f");
}

TEST_F(AssemblerMIPSTest, Bc1t) {
  mips::MipsLabel label;
  __ Bc1t(0, &label);
  constexpr size_t kAdduCount1 = 63;
  for (size_t i = 0; i != kAdduCount1; ++i) {
    __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
  }
  __ Bind(&label);
  constexpr size_t kAdduCount2 = 64;
  for (size_t i = 0; i != kAdduCount2; ++i) {
    __ Addu(mips::ZERO, mips::ZERO, mips::ZERO);
  }
  __ Bc1t(7, &label);

  std::string expected =
      ".set noreorder\n"
      "bc1t $fcc0, 1f\n"
      "nop\n" +
      RepeatInsn(kAdduCount1, "addu $zero, $zero, $zero\n") +
      "1:\n" +
      RepeatInsn(kAdduCount2, "addu $zero, $zero, $zero\n") +
      "bc1t $fcc7, 1b\n"
      "nop\n";
  DriverStr(expected, "Bc1t");
}

#undef __

}  // namespace art
