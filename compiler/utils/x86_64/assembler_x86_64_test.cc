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

#include "assembler_x86_64.h"

#include "utils/assembler_test.h"

namespace art {

TEST(AssemblerX86_64, CreateBuffer) {
  AssemblerBuffer buffer;
  AssemblerBuffer::EnsureCapacity ensured(&buffer);
  buffer.Emit<uint8_t>(0x42);
  ASSERT_EQ(static_cast<size_t>(1), buffer.Size());
  buffer.Emit<int32_t>(42);
  ASSERT_EQ(static_cast<size_t>(5), buffer.Size());
}

class AssemblerX86_64Test : public AssemblerTest<x86_64::X86_64Assembler, x86_64::CpuRegister,
                                                 x86_64::Immediate> {
 protected:
  // Get the typically used name for this architecture, e.g., aarch64, x86-64, ...
  std::string GetArchitectureString() OVERRIDE {
    return "x86_64";
  }

  std::string GetDisassembleParameters() OVERRIDE {
    return " -D -bbinary -mi386:x86-64 -Mx86-64,addr64,data32 --no-show-raw-insn";
  }

  void SetUpHelpers() OVERRIDE {
    if (registers_.size() == 0) {
      registers_.push_back(new x86_64::CpuRegister(x86_64::RAX));
      registers_.push_back(new x86_64::CpuRegister(x86_64::RBX));
      registers_.push_back(new x86_64::CpuRegister(x86_64::RCX));
      registers_.push_back(new x86_64::CpuRegister(x86_64::RDX));
      registers_.push_back(new x86_64::CpuRegister(x86_64::RBP));
      registers_.push_back(new x86_64::CpuRegister(x86_64::RSP));
      registers_.push_back(new x86_64::CpuRegister(x86_64::RSI));
      registers_.push_back(new x86_64::CpuRegister(x86_64::RDI));
      registers_.push_back(new x86_64::CpuRegister(x86_64::R8));
      registers_.push_back(new x86_64::CpuRegister(x86_64::R9));
      registers_.push_back(new x86_64::CpuRegister(x86_64::R10));
      registers_.push_back(new x86_64::CpuRegister(x86_64::R11));
      registers_.push_back(new x86_64::CpuRegister(x86_64::R12));
      registers_.push_back(new x86_64::CpuRegister(x86_64::R13));
      registers_.push_back(new x86_64::CpuRegister(x86_64::R14));
      registers_.push_back(new x86_64::CpuRegister(x86_64::R15));
    }
  }

  std::vector<x86_64::CpuRegister*> GetRegisters() OVERRIDE {
    return registers_;
  }

  x86_64::Immediate* CreateImmediate(int64_t imm_value) OVERRIDE {
    return new x86_64::Immediate(imm_value);
  }

 private:
  std::vector<x86_64::CpuRegister*> registers_;
};


TEST_F(AssemblerX86_64Test, Toolchain) {
  EXPECT_TRUE(CheckTools());
}


TEST_F(AssemblerX86_64Test, PushqRegs) {
  DriverStr(RepeatR(&x86_64::X86_64Assembler::pushq, "pushq %{reg}"), "pushq");
}

TEST_F(AssemblerX86_64Test, PushqImm) {
  DriverStr(RepeatI(&x86_64::X86_64Assembler::pushq, 4U, "pushq ${imm}"), "pushqi");
}


TEST_F(AssemblerX86_64Test, MovqRegs) {
  DriverStr(RepeatRR(&x86_64::X86_64Assembler::movq, "movq %{reg2}, %{reg1}"), "movq");
}

TEST_F(AssemblerX86_64Test, MovqImm) {
  DriverStr(RepeatRI(&x86_64::X86_64Assembler::movq, 8U, "movq ${imm}, %{reg}"), "movqi");
}


TEST_F(AssemblerX86_64Test, AddqRegs) {
  DriverStr(RepeatRR(&x86_64::X86_64Assembler::addq, "addq %{reg2}, %{reg1}"), "addq");
}

TEST_F(AssemblerX86_64Test, AddqImm) {
  DriverStr(RepeatRI(&x86_64::X86_64Assembler::addq, 4U, "addq ${imm}, %{reg}"), "addqi");
}


TEST_F(AssemblerX86_64Test, SubqRegs) {
  DriverStr(RepeatRR(&x86_64::X86_64Assembler::subq, "subq %{reg2}, %{reg1}"), "subq");
}

TEST_F(AssemblerX86_64Test, SubqImm) {
  DriverStr(RepeatRI(&x86_64::X86_64Assembler::subq, 4U, "subq ${imm}, %{reg}"), "subqi");
}


TEST_F(AssemblerX86_64Test, CmpqRegs) {
  DriverStr(RepeatRR(&x86_64::X86_64Assembler::cmpq, "cmpq %{reg2}, %{reg1}"), "cmpq");
}


TEST_F(AssemblerX86_64Test, XorqImm) {
  DriverStr(RepeatRI(&x86_64::X86_64Assembler::xorq, 4U, "xorq ${imm}, %{reg}"), "xorqi");
}

TEST_F(AssemblerX86_64Test, Movl) {
  GetAssembler()->movl(x86_64::CpuRegister(x86_64::R8), x86_64::CpuRegister(x86_64::R11));
  GetAssembler()->movl(x86_64::CpuRegister(x86_64::RAX), x86_64::CpuRegister(x86_64::R11));
  GetAssembler()->movl(x86_64::CpuRegister(x86_64::RAX), x86_64::Address(
      x86_64::CpuRegister(x86_64::RDI), x86_64::CpuRegister(x86_64::RBX), x86_64::TIMES_4, 12));
  GetAssembler()->movl(x86_64::CpuRegister(x86_64::RAX), x86_64::Address(
      x86_64::CpuRegister(x86_64::RDI), x86_64::CpuRegister(x86_64::R9), x86_64::TIMES_4, 12));
  GetAssembler()->movl(x86_64::CpuRegister(x86_64::R8), x86_64::Address(
      x86_64::CpuRegister(x86_64::RDI), x86_64::CpuRegister(x86_64::R9), x86_64::TIMES_4, 12));
  const char* expected =
    "movl %R11d, %R8d\n"
    "movl %R11d, %EAX\n"
    "movl 0xc(%RDI,%RBX,4), %EAX\n"
    "movl 0xc(%RDI,%R9,4), %EAX\n"
    "movl 0xc(%RDI,%R9,4), %R8d\n";

  DriverStr(expected, "movl");
}

TEST_F(AssemblerX86_64Test, Movw) {
  GetAssembler()->movw(x86_64::Address(x86_64::CpuRegister(x86_64::RAX), 0),
                       x86_64::CpuRegister(x86_64::R9));
  const char* expected = "movw %R9w, 0(%RAX)\n";
  DriverStr(expected, "movw");
}


std::string setcc_test_fn(x86_64::X86_64Assembler* assembler) {
  // From Condition
  /*
  kOverflow     =  0,
  kNoOverflow   =  1,
  kBelow        =  2,
  kAboveEqual   =  3,
  kEqual        =  4,
  kNotEqual     =  5,
  kBelowEqual   =  6,
  kAbove        =  7,
  kSign         =  8,
  kNotSign      =  9,
  kParityEven   = 10,
  kParityOdd    = 11,
  kLess         = 12,
  kGreaterEqual = 13,
  kLessEqual    = 14,
  */
  std::string suffixes[15] = { "o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "pe", "po",
                               "l", "ge", "le" };

  std::vector<x86_64::CpuRegister*> registers;
  registers.push_back(new x86_64::CpuRegister(x86_64::RAX));
  registers.push_back(new x86_64::CpuRegister(x86_64::RBX));
  registers.push_back(new x86_64::CpuRegister(x86_64::RCX));
  registers.push_back(new x86_64::CpuRegister(x86_64::RDX));
  registers.push_back(new x86_64::CpuRegister(x86_64::RBP));
  registers.push_back(new x86_64::CpuRegister(x86_64::RSP));
  registers.push_back(new x86_64::CpuRegister(x86_64::RSI));
  registers.push_back(new x86_64::CpuRegister(x86_64::RDI));
  registers.push_back(new x86_64::CpuRegister(x86_64::R8));
  registers.push_back(new x86_64::CpuRegister(x86_64::R9));
  registers.push_back(new x86_64::CpuRegister(x86_64::R10));
  registers.push_back(new x86_64::CpuRegister(x86_64::R11));
  registers.push_back(new x86_64::CpuRegister(x86_64::R12));
  registers.push_back(new x86_64::CpuRegister(x86_64::R13));
  registers.push_back(new x86_64::CpuRegister(x86_64::R14));
  registers.push_back(new x86_64::CpuRegister(x86_64::R15));

  std::string byte_regs[16];
  byte_regs[x86_64::RAX] = "al";
  byte_regs[x86_64::RBX] = "bl";
  byte_regs[x86_64::RCX] = "cl";
  byte_regs[x86_64::RDX] = "dl";
  byte_regs[x86_64::RBP] = "bpl";
  byte_regs[x86_64::RSP] = "spl";
  byte_regs[x86_64::RSI] = "sil";
  byte_regs[x86_64::RDI] = "dil";
  byte_regs[x86_64::R8] = "r8b";
  byte_regs[x86_64::R9] = "r9b";
  byte_regs[x86_64::R10] = "r10b";
  byte_regs[x86_64::R11] = "r11b";
  byte_regs[x86_64::R12] = "r12b";
  byte_regs[x86_64::R13] = "r13b";
  byte_regs[x86_64::R14] = "r14b";
  byte_regs[x86_64::R15] = "r15b";

  std::ostringstream str;

  for (auto reg : registers) {
    for (size_t i = 0; i < 15; ++i) {
      assembler->setcc(static_cast<x86_64::Condition>(i), *reg);
      str << "set" << suffixes[i] << " %" << byte_regs[reg->AsRegister()] << "\n";
    }
  }

  return str.str();
}

TEST_F(AssemblerX86_64Test, SetCC) {
  DriverFn(&setcc_test_fn, "setcc");
}

static x86_64::X86_64ManagedRegister ManagedFromCpu(x86_64::Register r) {
  return x86_64::X86_64ManagedRegister::FromCpuRegister(r);
}

static x86_64::X86_64ManagedRegister ManagedFromFpu(x86_64::FloatRegister r) {
  return x86_64::X86_64ManagedRegister::FromXmmRegister(r);
}

std::string buildframe_test_fn(x86_64::X86_64Assembler* assembler) {
  // TODO: more interesting spill registers / entry spills.

  // Two random spill regs.
  std::vector<ManagedRegister> spill_regs;
  spill_regs.push_back(ManagedFromCpu(x86_64::R10));
  spill_regs.push_back(ManagedFromCpu(x86_64::RSI));

  // Three random entry spills.
  ManagedRegisterEntrySpills entry_spills;
  ManagedRegisterSpill spill(ManagedFromCpu(x86_64::RAX), 8, 0);
  entry_spills.push_back(spill);
  ManagedRegisterSpill spill2(ManagedFromCpu(x86_64::RBX), 8, 8);
  entry_spills.push_back(spill2);
  ManagedRegisterSpill spill3(ManagedFromFpu(x86_64::XMM1), 8, 16);
  entry_spills.push_back(spill3);

  x86_64::X86_64ManagedRegister method_reg = ManagedFromCpu(x86_64::RDI);

  size_t frame_size = 10 * kStackAlignment;
  assembler->BuildFrame(10 * kStackAlignment, method_reg, spill_regs, entry_spills);

  // Construct assembly text counterpart.
  std::ostringstream str;
  // 1) Push the spill_regs.
  str << "pushq %rsi\n";
  str << "pushq %r10\n";
  // 2) Move down the stack pointer.
  ssize_t displacement = static_cast<ssize_t>(frame_size) - (spill_regs.size() * 8 + 8);
  str << "subq $" << displacement << ", %rsp\n";
  // 3) Store method reference.
  str << "movl %edi, (%rsp)\n";
  // 4) Entry spills.
  str << "movq %rax, " << frame_size + 0 << "(%rsp)\n";
  str << "movq %rbx, " << frame_size + 8 << "(%rsp)\n";
  str << "movsd %xmm1, " << frame_size + 16 << "(%rsp)\n";

  return str.str();
}

TEST_F(AssemblerX86_64Test, BuildFrame) {
  DriverFn(&buildframe_test_fn, "BuildFrame");
}

std::string removeframe_test_fn(x86_64::X86_64Assembler* assembler) {
  // TODO: more interesting spill registers / entry spills.

  // Two random spill regs.
  std::vector<ManagedRegister> spill_regs;
  spill_regs.push_back(ManagedFromCpu(x86_64::R10));
  spill_regs.push_back(ManagedFromCpu(x86_64::RSI));

  size_t frame_size = 10 * kStackAlignment;
  assembler->RemoveFrame(10 * kStackAlignment, spill_regs);

  // Construct assembly text counterpart.
  std::ostringstream str;
  // 1) Move up the stack pointer.
  ssize_t displacement = static_cast<ssize_t>(frame_size) - spill_regs.size() * 8 - 8;
  str << "addq $" << displacement << ", %rsp\n";
  // 2) Pop spill regs.
  str << "popq %r10\n";
  str << "popq %rsi\n";
  str << "ret\n";

  return str.str();
}

TEST_F(AssemblerX86_64Test, RemoveFrame) {
  DriverFn(&removeframe_test_fn, "RemoveFrame");
}

std::string increaseframe_test_fn(x86_64::X86_64Assembler* assembler) {
  assembler->IncreaseFrameSize(0U);
  assembler->IncreaseFrameSize(kStackAlignment);
  assembler->IncreaseFrameSize(10 * kStackAlignment);

  // Construct assembly text counterpart.
  std::ostringstream str;
  str << "addq $0, %rsp\n";
  str << "addq $-" << kStackAlignment << ", %rsp\n";
  str << "addq $-" << 10 * kStackAlignment << ", %rsp\n";

  return str.str();
}

TEST_F(AssemblerX86_64Test, IncreaseFrame) {
  DriverFn(&increaseframe_test_fn, "IncreaseFrame");
}

std::string decreaseframe_test_fn(x86_64::X86_64Assembler* assembler) {
  assembler->DecreaseFrameSize(0U);
  assembler->DecreaseFrameSize(kStackAlignment);
  assembler->DecreaseFrameSize(10 * kStackAlignment);

  // Construct assembly text counterpart.
  std::ostringstream str;
  str << "addq $0, %rsp\n";
  str << "addq $" << kStackAlignment << ", %rsp\n";
  str << "addq $" << 10 * kStackAlignment << ", %rsp\n";

  return str.str();
}

TEST_F(AssemblerX86_64Test, DecreaseFrame) {
  DriverFn(&decreaseframe_test_fn, "DecreaseFrame");
}

}  // namespace art
