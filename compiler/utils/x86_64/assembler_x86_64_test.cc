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

#include <inttypes.h>
#include <map>
#include <random>

#include "base/stl_util.h"
#include "utils/assembler_test.h"
#include "utils.h"

namespace art {

TEST(AssemblerX86_64, CreateBuffer) {
  AssemblerBuffer buffer;
  AssemblerBuffer::EnsureCapacity ensured(&buffer);
  buffer.Emit<uint8_t>(0x42);
  ASSERT_EQ(static_cast<size_t>(1), buffer.Size());
  buffer.Emit<int32_t>(42);
  ASSERT_EQ(static_cast<size_t>(5), buffer.Size());
}

#ifdef HAVE_ANDROID_OS
static constexpr size_t kRandomIterations = 1000;  // Devices might be puny, don't stress them...
#else
static constexpr size_t kRandomIterations = 100000;  // Hosts are pretty powerful.
#endif

TEST(AssemblerX86_64, SignExtension) {
  // 32bit.
  for (int32_t i = 0; i < 128; i++) {
    EXPECT_TRUE(IsInt32(8, i)) << i;
  }
  for (int32_t i = 128; i < 255; i++) {
    EXPECT_FALSE(IsInt32(8, i)) << i;
  }
  // Do some higher ones randomly.
  std::random_device rd;
  std::default_random_engine e1(rd());
  std::uniform_int_distribution<int32_t> uniform_dist(256, INT32_MAX);
  for (size_t i = 0; i < kRandomIterations; i++) {
    int32_t value = uniform_dist(e1);
    EXPECT_FALSE(IsInt32(8, value)) << value;
  }

  // Negative ones.
  for (int32_t i = -1; i >= -128; i--) {
    EXPECT_TRUE(IsInt32(8, i)) << i;
  }

  for (int32_t i = -129; i > -256; i--) {
    EXPECT_FALSE(IsInt32(8, i)) << i;
  }

  // Do some lower ones randomly.
  std::uniform_int_distribution<int32_t> uniform_dist2(INT32_MIN, -256);
  for (size_t i = 0; i < 100; i++) {
    int32_t value = uniform_dist2(e1);
    EXPECT_FALSE(IsInt32(8, value)) << value;
  }

  // 64bit.
  for (int64_t i = 0; i < 128; i++) {
    EXPECT_TRUE(IsInt64(8, i)) << i;
  }
  for (int32_t i = 128; i < 255; i++) {
    EXPECT_FALSE(IsInt64(8, i)) << i;
  }
  // Do some higher ones randomly.
  std::uniform_int_distribution<int64_t> uniform_dist3(256, INT64_MAX);
  for (size_t i = 0; i < 100; i++) {
    int64_t value = uniform_dist3(e1);
    EXPECT_FALSE(IsInt64(8, value)) << value;
  }

  // Negative ones.
  for (int64_t i = -1; i >= -128; i--) {
    EXPECT_TRUE(IsInt64(8, i)) << i;
  }

  for (int64_t i = -129; i > -256; i--) {
    EXPECT_FALSE(IsInt64(8, i)) << i;
  }

  // Do some lower ones randomly.
  std::uniform_int_distribution<int64_t> uniform_dist4(INT64_MIN, -256);
  for (size_t i = 0; i < kRandomIterations; i++) {
    int64_t value = uniform_dist4(e1);
    EXPECT_FALSE(IsInt64(8, value)) << value;
  }
}

struct X86_64CpuRegisterCompare {
    bool operator()(const x86_64::CpuRegister& a, const x86_64::CpuRegister& b) const {
        return a.AsRegister() < b.AsRegister();
    }
};

class AssemblerX86_64Test : public AssemblerTest<x86_64::X86_64Assembler, x86_64::CpuRegister,
                                                 x86_64::XmmRegister, x86_64::Immediate> {
 public:
  typedef AssemblerTest<x86_64::X86_64Assembler, x86_64::CpuRegister,
                        x86_64::XmmRegister, x86_64::Immediate> Base;

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

      secondary_register_names_.emplace(x86_64::CpuRegister(x86_64::RAX), "eax");
      secondary_register_names_.emplace(x86_64::CpuRegister(x86_64::RBX), "ebx");
      secondary_register_names_.emplace(x86_64::CpuRegister(x86_64::RCX), "ecx");
      secondary_register_names_.emplace(x86_64::CpuRegister(x86_64::RDX), "edx");
      secondary_register_names_.emplace(x86_64::CpuRegister(x86_64::RBP), "ebp");
      secondary_register_names_.emplace(x86_64::CpuRegister(x86_64::RSP), "esp");
      secondary_register_names_.emplace(x86_64::CpuRegister(x86_64::RSI), "esi");
      secondary_register_names_.emplace(x86_64::CpuRegister(x86_64::RDI), "edi");
      secondary_register_names_.emplace(x86_64::CpuRegister(x86_64::R8), "r8d");
      secondary_register_names_.emplace(x86_64::CpuRegister(x86_64::R9), "r9d");
      secondary_register_names_.emplace(x86_64::CpuRegister(x86_64::R10), "r10d");
      secondary_register_names_.emplace(x86_64::CpuRegister(x86_64::R11), "r11d");
      secondary_register_names_.emplace(x86_64::CpuRegister(x86_64::R12), "r12d");
      secondary_register_names_.emplace(x86_64::CpuRegister(x86_64::R13), "r13d");
      secondary_register_names_.emplace(x86_64::CpuRegister(x86_64::R14), "r14d");
      secondary_register_names_.emplace(x86_64::CpuRegister(x86_64::R15), "r15d");

      fp_registers_.push_back(new x86_64::XmmRegister(x86_64::XMM0));
      fp_registers_.push_back(new x86_64::XmmRegister(x86_64::XMM1));
      fp_registers_.push_back(new x86_64::XmmRegister(x86_64::XMM2));
      fp_registers_.push_back(new x86_64::XmmRegister(x86_64::XMM3));
      fp_registers_.push_back(new x86_64::XmmRegister(x86_64::XMM4));
      fp_registers_.push_back(new x86_64::XmmRegister(x86_64::XMM5));
      fp_registers_.push_back(new x86_64::XmmRegister(x86_64::XMM6));
      fp_registers_.push_back(new x86_64::XmmRegister(x86_64::XMM7));
      fp_registers_.push_back(new x86_64::XmmRegister(x86_64::XMM8));
      fp_registers_.push_back(new x86_64::XmmRegister(x86_64::XMM9));
      fp_registers_.push_back(new x86_64::XmmRegister(x86_64::XMM10));
      fp_registers_.push_back(new x86_64::XmmRegister(x86_64::XMM11));
      fp_registers_.push_back(new x86_64::XmmRegister(x86_64::XMM12));
      fp_registers_.push_back(new x86_64::XmmRegister(x86_64::XMM13));
      fp_registers_.push_back(new x86_64::XmmRegister(x86_64::XMM14));
      fp_registers_.push_back(new x86_64::XmmRegister(x86_64::XMM15));
    }
  }

  void TearDown() OVERRIDE {
    AssemblerTest::TearDown();
    STLDeleteElements(&registers_);
    STLDeleteElements(&fp_registers_);
  }

  std::vector<x86_64::CpuRegister*> GetRegisters() OVERRIDE {
    return registers_;
  }

  std::vector<x86_64::XmmRegister*> GetFPRegisters() OVERRIDE {
    return fp_registers_;
  }

  x86_64::Immediate CreateImmediate(int64_t imm_value) OVERRIDE {
    return x86_64::Immediate(imm_value);
  }

  std::string GetSecondaryRegisterName(const x86_64::CpuRegister& reg) OVERRIDE {
    CHECK(secondary_register_names_.find(reg) != secondary_register_names_.end());
    return secondary_register_names_[reg];
  }

 private:
  std::vector<x86_64::CpuRegister*> registers_;
  std::map<x86_64::CpuRegister, std::string, X86_64CpuRegisterCompare> secondary_register_names_;

  std::vector<x86_64::XmmRegister*> fp_registers_;
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

TEST_F(AssemblerX86_64Test, MovlRegs) {
  DriverStr(Repeatrr(&x86_64::X86_64Assembler::movl, "mov %{reg2}, %{reg1}"), "movl");
}

TEST_F(AssemblerX86_64Test, MovlImm) {
  DriverStr(Repeatri(&x86_64::X86_64Assembler::movl, 4U, "mov ${imm}, %{reg}"), "movli");
}

TEST_F(AssemblerX86_64Test, AddqRegs) {
  DriverStr(RepeatRR(&x86_64::X86_64Assembler::addq, "addq %{reg2}, %{reg1}"), "addq");
}

TEST_F(AssemblerX86_64Test, AddqImm) {
  DriverStr(RepeatRI(&x86_64::X86_64Assembler::addq, 4U, "addq ${imm}, %{reg}"), "addqi");
}

TEST_F(AssemblerX86_64Test, AddlRegs) {
  DriverStr(Repeatrr(&x86_64::X86_64Assembler::addl, "add %{reg2}, %{reg1}"), "addl");
}

TEST_F(AssemblerX86_64Test, AddlImm) {
  DriverStr(Repeatri(&x86_64::X86_64Assembler::addl, 4U, "add ${imm}, %{reg}"), "addli");
}

TEST_F(AssemblerX86_64Test, ImulqRegs) {
  DriverStr(RepeatRR(&x86_64::X86_64Assembler::imulq, "imulq %{reg2}, %{reg1}"), "imulq");
}

TEST_F(AssemblerX86_64Test, ImulqImm) {
  DriverStr(RepeatRI(&x86_64::X86_64Assembler::imulq, 4U, "imulq ${imm}, %{reg}, %{reg}"),
            "imulqi");
}

TEST_F(AssemblerX86_64Test, ImullRegs) {
  DriverStr(Repeatrr(&x86_64::X86_64Assembler::imull, "imul %{reg2}, %{reg1}"), "imull");
}

TEST_F(AssemblerX86_64Test, ImullImm) {
  DriverStr(Repeatri(&x86_64::X86_64Assembler::imull, 4U, "imull ${imm}, %{reg}, %{reg}"),
            "imulli");
}

TEST_F(AssemblerX86_64Test, Mull) {
  DriverStr(Repeatr(&x86_64::X86_64Assembler::mull, "mull %{reg}"), "mull");
}

TEST_F(AssemblerX86_64Test, SubqRegs) {
  DriverStr(RepeatRR(&x86_64::X86_64Assembler::subq, "subq %{reg2}, %{reg1}"), "subq");
}

TEST_F(AssemblerX86_64Test, SubqImm) {
  DriverStr(RepeatRI(&x86_64::X86_64Assembler::subq, 4U, "subq ${imm}, %{reg}"), "subqi");
}

TEST_F(AssemblerX86_64Test, SublRegs) {
  DriverStr(Repeatrr(&x86_64::X86_64Assembler::subl, "sub %{reg2}, %{reg1}"), "subl");
}

TEST_F(AssemblerX86_64Test, SublImm) {
  DriverStr(Repeatri(&x86_64::X86_64Assembler::subl, 4U, "sub ${imm}, %{reg}"), "subli");
}

// Shll only allows CL as the shift count.
std::string shll_fn(AssemblerX86_64Test::Base* assembler_test, x86_64::X86_64Assembler* assembler) {
  std::ostringstream str;

  std::vector<x86_64::CpuRegister*> registers = assembler_test->GetRegisters();

  x86_64::CpuRegister shifter(x86_64::RCX);
  for (auto reg : registers) {
    assembler->shll(*reg, shifter);
    str << "shll %cl, %" << assembler_test->GetSecondaryRegisterName(*reg) << "\n";
  }

  return str.str();
}

TEST_F(AssemblerX86_64Test, ShllReg) {
  DriverFn(&shll_fn, "shll");
}

TEST_F(AssemblerX86_64Test, ShllImm) {
  DriverStr(Repeatri(&x86_64::X86_64Assembler::shll, 1U, "shll ${imm}, %{reg}"), "shlli");
}

// Shlq only allows CL as the shift count.
std::string shlq_fn(AssemblerX86_64Test::Base* assembler_test, x86_64::X86_64Assembler* assembler) {
  std::ostringstream str;

  std::vector<x86_64::CpuRegister*> registers = assembler_test->GetRegisters();

  x86_64::CpuRegister shifter(x86_64::RCX);
  for (auto reg : registers) {
    assembler->shlq(*reg, shifter);
    str << "shlq %cl, %" << assembler_test->GetRegisterName(*reg) << "\n";
  }
  printf("%s\n", str.str().c_str());

  return str.str();
}

TEST_F(AssemblerX86_64Test, ShlqReg) {
  DriverFn(&shlq_fn, "shlq");
}

TEST_F(AssemblerX86_64Test, ShlqImm) {
  DriverStr(RepeatRI(&x86_64::X86_64Assembler::shlq, 1U, "shlq ${imm}, %{reg}"), "shlqi");
}

// Shrl only allows CL as the shift count.
std::string shrl_fn(AssemblerX86_64Test::Base* assembler_test, x86_64::X86_64Assembler* assembler) {
  std::ostringstream str;

  std::vector<x86_64::CpuRegister*> registers = assembler_test->GetRegisters();

  x86_64::CpuRegister shifter(x86_64::RCX);
  for (auto reg : registers) {
    assembler->shrl(*reg, shifter);
    str << "shrl %cl, %" << assembler_test->GetSecondaryRegisterName(*reg) << "\n";
  }

  return str.str();
}

TEST_F(AssemblerX86_64Test, ShrlReg) {
  DriverFn(&shrl_fn, "shrl");
}

TEST_F(AssemblerX86_64Test, ShrlImm) {
  DriverStr(Repeatri(&x86_64::X86_64Assembler::shrl, 1U, "shrl ${imm}, %{reg}"), "shrli");
}

// Shrq only allows CL as the shift count.
std::string shrq_fn(AssemblerX86_64Test::Base* assembler_test, x86_64::X86_64Assembler* assembler) {
  std::ostringstream str;

  std::vector<x86_64::CpuRegister*> registers = assembler_test->GetRegisters();

  x86_64::CpuRegister shifter(x86_64::RCX);
  for (auto reg : registers) {
    assembler->shrq(*reg, shifter);
    str << "shrq %cl, %" << assembler_test->GetRegisterName(*reg) << "\n";
  }

  return str.str();
}

TEST_F(AssemblerX86_64Test, ShrqReg) {
  DriverFn(&shrq_fn, "shrq");
}

TEST_F(AssemblerX86_64Test, ShrqImm) {
  DriverStr(RepeatRI(&x86_64::X86_64Assembler::shrq, 1U, "shrq ${imm}, %{reg}"), "shrqi");
}

// Sarl only allows CL as the shift count.
std::string sarl_fn(AssemblerX86_64Test::Base* assembler_test, x86_64::X86_64Assembler* assembler) {
  std::ostringstream str;

  std::vector<x86_64::CpuRegister*> registers = assembler_test->GetRegisters();

  x86_64::CpuRegister shifter(x86_64::RCX);
  for (auto reg : registers) {
    assembler->sarl(*reg, shifter);
    str << "sarl %cl, %" << assembler_test->GetSecondaryRegisterName(*reg) << "\n";
  }

  return str.str();
}

TEST_F(AssemblerX86_64Test, SarlReg) {
  DriverFn(&sarl_fn, "sarl");
}

TEST_F(AssemblerX86_64Test, SarlImm) {
  DriverStr(Repeatri(&x86_64::X86_64Assembler::sarl, 1U, "sarl ${imm}, %{reg}"), "sarli");
}

// Sarq only allows CL as the shift count.
std::string sarq_fn(AssemblerX86_64Test::Base* assembler_test, x86_64::X86_64Assembler* assembler) {
  std::ostringstream str;

  std::vector<x86_64::CpuRegister*> registers = assembler_test->GetRegisters();

  x86_64::CpuRegister shifter(x86_64::RCX);
  for (auto reg : registers) {
    assembler->sarq(*reg, shifter);
    str << "sarq %cl, %" << assembler_test->GetRegisterName(*reg) << "\n";
  }

  return str.str();
}

TEST_F(AssemblerX86_64Test, SarqReg) {
  DriverFn(&sarq_fn, "sarq");
}

TEST_F(AssemblerX86_64Test, SarqImm) {
  DriverStr(RepeatRI(&x86_64::X86_64Assembler::sarq, 1U, "sarq ${imm}, %{reg}"), "sarqi");
}

TEST_F(AssemblerX86_64Test, CmpqRegs) {
  DriverStr(RepeatRR(&x86_64::X86_64Assembler::cmpq, "cmpq %{reg2}, %{reg1}"), "cmpq");
}

TEST_F(AssemblerX86_64Test, CmpqImm) {
  DriverStr(RepeatRI(&x86_64::X86_64Assembler::cmpq, 4U  /* cmpq only supports 32b imm */,
                     "cmpq ${imm}, %{reg}"), "cmpqi");
}

TEST_F(AssemblerX86_64Test, CmplRegs) {
  DriverStr(Repeatrr(&x86_64::X86_64Assembler::cmpl, "cmp %{reg2}, %{reg1}"), "cmpl");
}

TEST_F(AssemblerX86_64Test, CmplImm) {
  DriverStr(Repeatri(&x86_64::X86_64Assembler::cmpl, 4U, "cmpl ${imm}, %{reg}"), "cmpli");
}

TEST_F(AssemblerX86_64Test, Testl) {
  // Note: uses different order for GCC than usual. This makes GCC happy, and doesn't have an
  // impact on functional correctness.
  DriverStr(Repeatrr(&x86_64::X86_64Assembler::testl, "testl %{reg1}, %{reg2}"), "testl");
}

TEST_F(AssemblerX86_64Test, Negq) {
  DriverStr(RepeatR(&x86_64::X86_64Assembler::negq, "negq %{reg}"), "negq");
}

TEST_F(AssemblerX86_64Test, Negl) {
  DriverStr(Repeatr(&x86_64::X86_64Assembler::negl, "negl %{reg}"), "negl");
}

TEST_F(AssemblerX86_64Test, Notq) {
  DriverStr(RepeatR(&x86_64::X86_64Assembler::notq, "notq %{reg}"), "notq");
}

TEST_F(AssemblerX86_64Test, Notl) {
  DriverStr(Repeatr(&x86_64::X86_64Assembler::notl, "notl %{reg}"), "notl");
}

TEST_F(AssemblerX86_64Test, AndqRegs) {
  DriverStr(RepeatRR(&x86_64::X86_64Assembler::andq, "andq %{reg2}, %{reg1}"), "andq");
}

TEST_F(AssemblerX86_64Test, AndqImm) {
  DriverStr(RepeatRI(&x86_64::X86_64Assembler::andq, 4U  /* andq only supports 32b imm */,
                     "andq ${imm}, %{reg}"), "andqi");
}

TEST_F(AssemblerX86_64Test, AndlRegs) {
  DriverStr(Repeatrr(&x86_64::X86_64Assembler::andl, "andl %{reg2}, %{reg1}"), "andl");
}

TEST_F(AssemblerX86_64Test, AndlImm) {
  DriverStr(Repeatri(&x86_64::X86_64Assembler::andl, 4U, "andl ${imm}, %{reg}"), "andli");
}

TEST_F(AssemblerX86_64Test, OrqRegs) {
  DriverStr(RepeatRR(&x86_64::X86_64Assembler::orq, "orq %{reg2}, %{reg1}"), "orq");
}

TEST_F(AssemblerX86_64Test, OrlRegs) {
  DriverStr(Repeatrr(&x86_64::X86_64Assembler::orl, "orl %{reg2}, %{reg1}"), "orl");
}

TEST_F(AssemblerX86_64Test, OrlImm) {
  DriverStr(Repeatri(&x86_64::X86_64Assembler::orl, 4U, "orl ${imm}, %{reg}"), "orli");
}

TEST_F(AssemblerX86_64Test, XorqRegs) {
  DriverStr(RepeatRR(&x86_64::X86_64Assembler::xorq, "xorq %{reg2}, %{reg1}"), "xorq");
}

TEST_F(AssemblerX86_64Test, XorqImm) {
  DriverStr(RepeatRI(&x86_64::X86_64Assembler::xorq, 4U, "xorq ${imm}, %{reg}"), "xorqi");
}

TEST_F(AssemblerX86_64Test, XorlRegs) {
  DriverStr(Repeatrr(&x86_64::X86_64Assembler::xorl, "xor %{reg2}, %{reg1}"), "xorl");
}

TEST_F(AssemblerX86_64Test, XorlImm) {
  DriverStr(Repeatri(&x86_64::X86_64Assembler::xorl, 4U, "xor ${imm}, %{reg}"), "xorli");
}

TEST_F(AssemblerX86_64Test, Xchgq) {
  DriverStr(RepeatRR(&x86_64::X86_64Assembler::xchgq, "xchgq %{reg2}, %{reg1}"), "xchgq");
}

TEST_F(AssemblerX86_64Test, Xchgl) {
  // Test is disabled because GCC generates 0x87 0xC0 for xchgl eax, eax. All other cases are the
  // same. Anyone know why it doesn't emit a simple 0x90? It does so for xchgq rax, rax...
  // DriverStr(Repeatrr(&x86_64::X86_64Assembler::xchgl, "xchgl %{reg2}, %{reg1}"), "xchgl");
}

TEST_F(AssemblerX86_64Test, Movl) {
  GetAssembler()->movl(x86_64::CpuRegister(x86_64::RAX), x86_64::Address(
      x86_64::CpuRegister(x86_64::RDI), x86_64::CpuRegister(x86_64::RBX), x86_64::TIMES_4, 12));
  GetAssembler()->movl(x86_64::CpuRegister(x86_64::RAX), x86_64::Address(
      x86_64::CpuRegister(x86_64::RDI), x86_64::CpuRegister(x86_64::R9), x86_64::TIMES_4, 12));
  GetAssembler()->movl(x86_64::CpuRegister(x86_64::R8), x86_64::Address(
      x86_64::CpuRegister(x86_64::RDI), x86_64::CpuRegister(x86_64::R9), x86_64::TIMES_4, 12));
  const char* expected =
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

TEST_F(AssemblerX86_64Test, Movsxd) {
  DriverStr(RepeatRr(&x86_64::X86_64Assembler::movsxd, "movsxd %{reg2}, %{reg1}"), "movsxd");
}

///////////////////
// FP Operations //
///////////////////

TEST_F(AssemblerX86_64Test, Movaps) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::movaps, "movaps %{reg2}, %{reg1}"), "movaps");
}

TEST_F(AssemblerX86_64Test, Movss) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::movss, "movss %{reg2}, %{reg1}"), "movss");
}

TEST_F(AssemblerX86_64Test, Movsd) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::movsd, "movsd %{reg2}, %{reg1}"), "movsd");
}

TEST_F(AssemblerX86_64Test, Movd1) {
  DriverStr(RepeatFR(&x86_64::X86_64Assembler::movd, "movd %{reg2}, %{reg1}"), "movd.1");
}

TEST_F(AssemblerX86_64Test, Movd2) {
  DriverStr(RepeatRF(&x86_64::X86_64Assembler::movd, "movd %{reg2}, %{reg1}"), "movd.2");
}

TEST_F(AssemblerX86_64Test, Addss) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::addss, "addss %{reg2}, %{reg1}"), "addss");
}

TEST_F(AssemblerX86_64Test, Addsd) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::addsd, "addsd %{reg2}, %{reg1}"), "addsd");
}

TEST_F(AssemblerX86_64Test, Subss) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::subss, "subss %{reg2}, %{reg1}"), "subss");
}

TEST_F(AssemblerX86_64Test, Subsd) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::subsd, "subsd %{reg2}, %{reg1}"), "subsd");
}

TEST_F(AssemblerX86_64Test, Mulss) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::mulss, "mulss %{reg2}, %{reg1}"), "mulss");
}

TEST_F(AssemblerX86_64Test, Mulsd) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::mulsd, "mulsd %{reg2}, %{reg1}"), "mulsd");
}

TEST_F(AssemblerX86_64Test, Divss) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::divss, "divss %{reg2}, %{reg1}"), "divss");
}

TEST_F(AssemblerX86_64Test, Divsd) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::divsd, "divsd %{reg2}, %{reg1}"), "divsd");
}

TEST_F(AssemblerX86_64Test, Cvtsi2ss) {
  DriverStr(RepeatFr(&x86_64::X86_64Assembler::cvtsi2ss, "cvtsi2ss %{reg2}, %{reg1}"), "cvtsi2ss");
}

TEST_F(AssemblerX86_64Test, Cvtsi2sd) {
  DriverStr(RepeatFr(&x86_64::X86_64Assembler::cvtsi2sd, "cvtsi2sd %{reg2}, %{reg1}"), "cvtsi2sd");
}


TEST_F(AssemblerX86_64Test, Cvtss2si) {
  DriverStr(RepeatrF(&x86_64::X86_64Assembler::cvtss2si, "cvtss2si %{reg2}, %{reg1}"), "cvtss2si");
}


TEST_F(AssemblerX86_64Test, Cvtss2sd) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::cvtss2sd, "cvtss2sd %{reg2}, %{reg1}"), "cvtss2sd");
}


TEST_F(AssemblerX86_64Test, Cvtsd2si) {
  DriverStr(RepeatrF(&x86_64::X86_64Assembler::cvtsd2si, "cvtsd2si %{reg2}, %{reg1}"), "cvtsd2si");
}

TEST_F(AssemblerX86_64Test, Cvttss2si) {
  DriverStr(RepeatrF(&x86_64::X86_64Assembler::cvttss2si, "cvttss2si %{reg2}, %{reg1}"),
            "cvttss2si");
}

TEST_F(AssemblerX86_64Test, Cvttsd2si) {
  DriverStr(RepeatrF(&x86_64::X86_64Assembler::cvttsd2si, "cvttsd2si %{reg2}, %{reg1}"),
            "cvttsd2si");
}

TEST_F(AssemblerX86_64Test, Cvtsd2ss) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::cvtsd2ss, "cvtsd2ss %{reg2}, %{reg1}"), "cvtsd2ss");
}

TEST_F(AssemblerX86_64Test, Cvtdq2pd) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::cvtdq2pd, "cvtdq2pd %{reg2}, %{reg1}"), "cvtdq2pd");
}

TEST_F(AssemblerX86_64Test, Comiss) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::comiss, "comiss %{reg2}, %{reg1}"), "comiss");
}

TEST_F(AssemblerX86_64Test, Comisd) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::comisd, "comisd %{reg2}, %{reg1}"), "comisd");
}

TEST_F(AssemblerX86_64Test, Ucomiss) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::ucomiss, "ucomiss %{reg2}, %{reg1}"), "ucomiss");
}

TEST_F(AssemblerX86_64Test, Ucomisd) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::ucomisd, "ucomisd %{reg2}, %{reg1}"), "ucomisd");
}

TEST_F(AssemblerX86_64Test, Sqrtss) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::sqrtss, "sqrtss %{reg2}, %{reg1}"), "sqrtss");
}

TEST_F(AssemblerX86_64Test, Sqrtsd) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::sqrtsd, "sqrtsd %{reg2}, %{reg1}"), "sqrtsd");
}

TEST_F(AssemblerX86_64Test, Xorps) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::xorps, "xorps %{reg2}, %{reg1}"), "xorps");
}

TEST_F(AssemblerX86_64Test, Xorpd) {
  DriverStr(RepeatFF(&x86_64::X86_64Assembler::xorpd, "xorpd %{reg2}, %{reg1}"), "xorpd");
}

// X87

std::string x87_fn(AssemblerX86_64Test::Base* assembler_test ATTRIBUTE_UNUSED,
                   x86_64::X86_64Assembler* assembler) {
  std::ostringstream str;

  assembler->fincstp();
  str << "fincstp\n";

  assembler->fsin();
  str << "fsin\n";

  assembler->fcos();
  str << "fcos\n";

  assembler->fptan();
  str << "fptan\n";

  return str.str();
}

TEST_F(AssemblerX86_64Test, X87) {
  DriverFn(&x87_fn, "x87");
}

////////////////
// CALL / JMP //
////////////////

TEST_F(AssemblerX86_64Test, Call) {
  DriverStr(RepeatR(&x86_64::X86_64Assembler::call, "call *%{reg}"), "call");
}

TEST_F(AssemblerX86_64Test, Jmp) {
  DriverStr(RepeatR(&x86_64::X86_64Assembler::jmp, "jmp *%{reg}"), "jmp");
}

TEST_F(AssemblerX86_64Test, Enter) {
  DriverStr(RepeatI(&x86_64::X86_64Assembler::enter, 2U  /* 16b immediate */, "enter ${imm}, $0",
                    true  /* Only non-negative number */), "enter");
}

TEST_F(AssemblerX86_64Test, RetImm) {
  DriverStr(RepeatI(&x86_64::X86_64Assembler::ret, 2U  /* 16b immediate */, "ret ${imm}",
                    true  /* Only non-negative number */), "reti");
}

std::string ret_and_leave_fn(AssemblerX86_64Test::Base* assembler_test ATTRIBUTE_UNUSED,
                             x86_64::X86_64Assembler* assembler) {
  std::ostringstream str;

  assembler->ret();
  str << "ret\n";

  assembler->leave();
  str << "leave\n";

  return str.str();
}

TEST_F(AssemblerX86_64Test, RetAndLeave) {
  DriverFn(&ret_and_leave_fn, "retleave");
}

//////////
// MISC //
//////////

std::string setcc_test_fn(AssemblerX86_64Test::Base* assembler_test,
                          x86_64::X86_64Assembler* assembler) {
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

  std::vector<x86_64::CpuRegister*> registers = assembler_test->GetRegisters();

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

std::string buildframe_test_fn(AssemblerX86_64Test::Base* assembler_test ATTRIBUTE_UNUSED,
                               x86_64::X86_64Assembler* assembler) {
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

std::string removeframe_test_fn(AssemblerX86_64Test::Base* assembler_test ATTRIBUTE_UNUSED,
                                x86_64::X86_64Assembler* assembler) {
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

std::string increaseframe_test_fn(AssemblerX86_64Test::Base* assembler_test ATTRIBUTE_UNUSED,
                                  x86_64::X86_64Assembler* assembler) {
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

std::string decreaseframe_test_fn(AssemblerX86_64Test::Base* assembler_test ATTRIBUTE_UNUSED,
                                  x86_64::X86_64Assembler* assembler) {
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
