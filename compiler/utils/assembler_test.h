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

#ifndef ART_COMPILER_UTILS_ASSEMBLER_TEST_H_
#define ART_COMPILER_UTILS_ASSEMBLER_TEST_H_

#include "assembler.h"

#include "assembler_test_base.h"
#include "common_runtime_test.h"  // For ScratchFile

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sys/stat.h>

namespace art {

// Helper for a constexpr string length.
constexpr size_t ConstexprStrLen(char const* str, size_t count = 0) {
  return ('\0' == str[0]) ? count : ConstexprStrLen(str+1, count+1);
}

enum class RegisterView {  // private
  kUsePrimaryName,
  kUseSecondaryName,
  kUseTertiaryName,
  kUseQuaternaryName,
};

template<typename Ass, typename Reg, typename FPReg, typename Imm>
class AssemblerTest : public testing::Test {
 public:
  Ass* GetAssembler() {
    return assembler_.get();
  }

  typedef std::string (*TestFn)(AssemblerTest* assembler_test, Ass* assembler);

  void DriverFn(TestFn f, std::string test_name) {
    DriverWrapper(f(this, assembler_.get()), test_name);
  }

  // This driver assumes the assembler has already been called.
  void DriverStr(std::string assembly_string, std::string test_name) {
    DriverWrapper(assembly_string, test_name);
  }

  std::string RepeatR(void (Ass::*f)(Reg), std::string fmt) {
    return RepeatTemplatedRegister<Reg>(f,
        GetRegisters(),
        &AssemblerTest::GetRegName<RegisterView::kUsePrimaryName>,
        fmt);
  }

  std::string Repeatr(void (Ass::*f)(Reg), std::string fmt) {
    return RepeatTemplatedRegister<Reg>(f,
        GetRegisters(),
        &AssemblerTest::GetRegName<RegisterView::kUseSecondaryName>,
        fmt);
  }

  std::string RepeatRR(void (Ass::*f)(Reg, Reg), std::string fmt) {
    return RepeatTemplatedRegisters<Reg, Reg>(f,
        GetRegisters(),
        GetRegisters(),
        &AssemblerTest::GetRegName<RegisterView::kUsePrimaryName>,
        &AssemblerTest::GetRegName<RegisterView::kUsePrimaryName>,
        fmt);
  }

  std::string RepeatRRNoDupes(void (Ass::*f)(Reg, Reg), std::string fmt) {
    return RepeatTemplatedRegistersNoDupes<Reg, Reg>(f,
        GetRegisters(),
        GetRegisters(),
        &AssemblerTest::GetRegName<RegisterView::kUsePrimaryName>,
        &AssemblerTest::GetRegName<RegisterView::kUsePrimaryName>,
        fmt);
  }

  std::string Repeatrr(void (Ass::*f)(Reg, Reg), std::string fmt) {
    return RepeatTemplatedRegisters<Reg, Reg>(f,
        GetRegisters(),
        GetRegisters(),
        &AssemblerTest::GetRegName<RegisterView::kUseSecondaryName>,
        &AssemblerTest::GetRegName<RegisterView::kUseSecondaryName>,
        fmt);
  }

  std::string RepeatRRR(void (Ass::*f)(Reg, Reg, Reg), std::string fmt) {
    return RepeatTemplatedRegisters<Reg, Reg, Reg>(f,
        GetRegisters(),
        GetRegisters(),
        GetRegisters(),
        &AssemblerTest::GetRegName<RegisterView::kUsePrimaryName>,
        &AssemblerTest::GetRegName<RegisterView::kUsePrimaryName>,
        &AssemblerTest::GetRegName<RegisterView::kUsePrimaryName>,
        fmt);
  }

  std::string Repeatrb(void (Ass::*f)(Reg, Reg), std::string fmt) {
    return RepeatTemplatedRegisters<Reg, Reg>(f,
        GetRegisters(),
        GetRegisters(),
        &AssemblerTest::GetRegName<RegisterView::kUseSecondaryName>,
        &AssemblerTest::GetRegName<RegisterView::kUseQuaternaryName>,
        fmt);
  }

  std::string RepeatRr(void (Ass::*f)(Reg, Reg), std::string fmt) {
    return RepeatTemplatedRegisters<Reg, Reg>(f,
        GetRegisters(),
        GetRegisters(),
        &AssemblerTest::GetRegName<RegisterView::kUsePrimaryName>,
        &AssemblerTest::GetRegName<RegisterView::kUseSecondaryName>,
        fmt);
  }

  std::string RepeatRI(void (Ass::*f)(Reg, const Imm&), size_t imm_bytes, std::string fmt) {
    return RepeatRegisterImm<RegisterView::kUsePrimaryName>(f, imm_bytes, fmt);
  }

  std::string Repeatri(void (Ass::*f)(Reg, const Imm&), size_t imm_bytes, std::string fmt) {
    return RepeatRegisterImm<RegisterView::kUseSecondaryName>(f, imm_bytes, fmt);
  }

  template <typename Reg1, typename Reg2, typename ImmType>
  std::string RepeatTemplatedRegistersImmBits(void (Ass::*f)(Reg1, Reg2, ImmType),
                                              int imm_bits,
                                              const std::vector<Reg1*> reg1_registers,
                                              const std::vector<Reg2*> reg2_registers,
                                              std::string (AssemblerTest::*GetName1)(const Reg1&),
                                              std::string (AssemblerTest::*GetName2)(const Reg2&),
                                              std::string fmt) {
    std::string str;
    std::vector<int64_t> imms = CreateImmediateValuesBits(abs(imm_bits), (imm_bits > 0));

    for (auto reg1 : reg1_registers) {
      for (auto reg2 : reg2_registers) {
        for (int64_t imm : imms) {
          ImmType new_imm = CreateImmediate(imm);
          (assembler_.get()->*f)(*reg1, *reg2, new_imm);
          std::string base = fmt;

          std::string reg1_string = (this->*GetName1)(*reg1);
          size_t reg1_index;
          while ((reg1_index = base.find(REG1_TOKEN)) != std::string::npos) {
            base.replace(reg1_index, ConstexprStrLen(REG1_TOKEN), reg1_string);
          }

          std::string reg2_string = (this->*GetName2)(*reg2);
          size_t reg2_index;
          while ((reg2_index = base.find(REG2_TOKEN)) != std::string::npos) {
            base.replace(reg2_index, ConstexprStrLen(REG2_TOKEN), reg2_string);
          }

          size_t imm_index = base.find(IMM_TOKEN);
          if (imm_index != std::string::npos) {
            std::ostringstream sreg;
            sreg << imm;
            std::string imm_string = sreg.str();
            base.replace(imm_index, ConstexprStrLen(IMM_TOKEN), imm_string);
          }

          if (str.size() > 0) {
            str += "\n";
          }
          str += base;
        }
      }
    }
    // Add a newline at the end.
    str += "\n";
    return str;
  }

  template <typename ImmType, typename Reg1, typename Reg2>
  std::string RepeatTemplatedImmBitsRegisters(void (Ass::*f)(ImmType, Reg1, Reg2),
                                              const std::vector<Reg1*> reg1_registers,
                                              const std::vector<Reg2*> reg2_registers,
                                              std::string (AssemblerTest::*GetName1)(const Reg1&),
                                              std::string (AssemblerTest::*GetName2)(const Reg2&),
                                              int imm_bits,
                                              std::string fmt) {
    std::vector<int64_t> imms = CreateImmediateValuesBits(abs(imm_bits), (imm_bits > 0));

    WarnOnCombinations(reg1_registers.size() * reg2_registers.size() * imms.size());

    std::string str;
    for (auto reg1 : reg1_registers) {
      for (auto reg2 : reg2_registers) {
        for (int64_t imm : imms) {
          ImmType new_imm = CreateImmediate(imm);
          (assembler_.get()->*f)(new_imm, *reg1, *reg2);
          std::string base = fmt;

          std::string reg1_string = (this->*GetName1)(*reg1);
          size_t reg1_index;
          while ((reg1_index = base.find(REG1_TOKEN)) != std::string::npos) {
            base.replace(reg1_index, ConstexprStrLen(REG1_TOKEN), reg1_string);
          }

          std::string reg2_string = (this->*GetName2)(*reg2);
          size_t reg2_index;
          while ((reg2_index = base.find(REG2_TOKEN)) != std::string::npos) {
            base.replace(reg2_index, ConstexprStrLen(REG2_TOKEN), reg2_string);
          }

          size_t imm_index = base.find(IMM_TOKEN);
          if (imm_index != std::string::npos) {
            std::ostringstream sreg;
            sreg << imm;
            std::string imm_string = sreg.str();
            base.replace(imm_index, ConstexprStrLen(IMM_TOKEN), imm_string);
          }

          if (str.size() > 0) {
            str += "\n";
          }
          str += base;
        }
      }
    }
    // Add a newline at the end.
    str += "\n";
    return str;
  }

  template <typename RegType, typename ImmType>
  std::string RepeatTemplatedRegisterImmBits(void (Ass::*f)(RegType, ImmType),
                                             int imm_bits,
                                             const std::vector<Reg*> registers,
                                             std::string (AssemblerTest::*GetName)(const RegType&),
                                             std::string fmt) {
    std::string str;
    std::vector<int64_t> imms = CreateImmediateValuesBits(abs(imm_bits), (imm_bits > 0));

    for (auto reg : registers) {
      for (int64_t imm : imms) {
        ImmType new_imm = CreateImmediate(imm);
        (assembler_.get()->*f)(*reg, new_imm);
        std::string base = fmt;

        std::string reg_string = (this->*GetName)(*reg);
        size_t reg_index;
        while ((reg_index = base.find(REG_TOKEN)) != std::string::npos) {
          base.replace(reg_index, ConstexprStrLen(REG_TOKEN), reg_string);
        }

        size_t imm_index = base.find(IMM_TOKEN);
        if (imm_index != std::string::npos) {
          std::ostringstream sreg;
          sreg << imm;
          std::string imm_string = sreg.str();
          base.replace(imm_index, ConstexprStrLen(IMM_TOKEN), imm_string);
        }

        if (str.size() > 0) {
          str += "\n";
        }
        str += base;
      }
    }
    // Add a newline at the end.
    str += "\n";
    return str;
  }

  template <typename ImmType>
  std::string RepeatRRIb(void (Ass::*f)(Reg, Reg, ImmType), int imm_bits, std::string fmt) {
    return RepeatTemplatedRegistersImmBits<Reg, Reg, ImmType>(f,
        imm_bits,
        GetRegisters(),
        GetRegisters(),
        &AssemblerTest::GetRegName<RegisterView::kUsePrimaryName>,
        &AssemblerTest::GetRegName<RegisterView::kUsePrimaryName>,
        fmt);
  }

  template <typename ImmType>
  std::string RepeatRIb(void (Ass::*f)(Reg, ImmType), int imm_bits, std::string fmt) {
    return RepeatTemplatedRegisterImmBits<Reg, ImmType>(f,
        imm_bits,
        GetRegisters(),
        &AssemblerTest::GetRegName<RegisterView::kUsePrimaryName>,
        fmt);
  }

  template <typename ImmType>
  std::string RepeatFRIb(void (Ass::*f)(FPReg, Reg, ImmType), int imm_bits, std::string fmt) {
    return RepeatTemplatedRegistersImmBits<FPReg, Reg, ImmType>(f,
        imm_bits,
        GetFPRegisters(),
        GetRegisters(),
        &AssemblerTest::GetFPRegName,
        &AssemblerTest::GetRegName<RegisterView::kUsePrimaryName>,
        fmt);
  }

  std::string RepeatFF(void (Ass::*f)(FPReg, FPReg), std::string fmt) {
    return RepeatTemplatedRegisters<FPReg, FPReg>(f,
                                                  GetFPRegisters(),
                                                  GetFPRegisters(),
                                                  &AssemblerTest::GetFPRegName,
                                                  &AssemblerTest::GetFPRegName,
                                                  fmt);
  }

  std::string RepeatFFF(void (Ass::*f)(FPReg, FPReg, FPReg), std::string fmt) {
    return RepeatTemplatedRegisters<FPReg, FPReg, FPReg>(f,
                                                         GetFPRegisters(),
                                                         GetFPRegisters(),
                                                         GetFPRegisters(),
                                                         &AssemblerTest::GetFPRegName,
                                                         &AssemblerTest::GetFPRegName,
                                                         &AssemblerTest::GetFPRegName,
                                                         fmt);
  }

  std::string RepeatFFI(void (Ass::*f)(FPReg, FPReg, const Imm&),
                        size_t imm_bytes,
                        std::string fmt) {
    return RepeatTemplatedRegistersImm<FPReg, FPReg>(f,
                                                     GetFPRegisters(),
                                                     GetFPRegisters(),
                                                     &AssemblerTest::GetFPRegName,
                                                     &AssemblerTest::GetFPRegName,
                                                     imm_bytes,
                                                     fmt);
  }

  template <typename ImmType>
  std::string RepeatIbFF(void (Ass::*f)(ImmType, FPReg, FPReg), int imm_bits, std::string fmt) {
    return RepeatTemplatedImmBitsRegisters<ImmType, FPReg, FPReg>(f,
                                                                  GetFPRegisters(),
                                                                  GetFPRegisters(),
                                                                  &AssemblerTest::GetFPRegName,
                                                                  &AssemblerTest::GetFPRegName,
                                                                  imm_bits,
                                                                  fmt);
  }

  std::string RepeatFR(void (Ass::*f)(FPReg, Reg), std::string fmt) {
    return RepeatTemplatedRegisters<FPReg, Reg>(f,
        GetFPRegisters(),
        GetRegisters(),
        &AssemblerTest::GetFPRegName,
        &AssemblerTest::GetRegName<RegisterView::kUsePrimaryName>,
        fmt);
  }

  std::string RepeatFr(void (Ass::*f)(FPReg, Reg), std::string fmt) {
    return RepeatTemplatedRegisters<FPReg, Reg>(f,
        GetFPRegisters(),
        GetRegisters(),
        &AssemblerTest::GetFPRegName,
        &AssemblerTest::GetRegName<RegisterView::kUseSecondaryName>,
        fmt);
  }

  std::string RepeatRF(void (Ass::*f)(Reg, FPReg), std::string fmt) {
    return RepeatTemplatedRegisters<Reg, FPReg>(f,
        GetRegisters(),
        GetFPRegisters(),
        &AssemblerTest::GetRegName<RegisterView::kUsePrimaryName>,
        &AssemblerTest::GetFPRegName,
        fmt);
  }

  std::string RepeatrF(void (Ass::*f)(Reg, FPReg), std::string fmt) {
    return RepeatTemplatedRegisters<Reg, FPReg>(f,
        GetRegisters(),
        GetFPRegisters(),
        &AssemblerTest::GetRegName<RegisterView::kUseSecondaryName>,
        &AssemblerTest::GetFPRegName,
        fmt);
  }

  std::string RepeatI(void (Ass::*f)(const Imm&), size_t imm_bytes, std::string fmt,
                      bool as_uint = false) {
    std::string str;
    std::vector<int64_t> imms = CreateImmediateValues(imm_bytes, as_uint);

    WarnOnCombinations(imms.size());

    for (int64_t imm : imms) {
      Imm new_imm = CreateImmediate(imm);
      (assembler_.get()->*f)(new_imm);
      std::string base = fmt;

      size_t imm_index = base.find(IMM_TOKEN);
      if (imm_index != std::string::npos) {
        std::ostringstream sreg;
        sreg << imm;
        std::string imm_string = sreg.str();
        base.replace(imm_index, ConstexprStrLen(IMM_TOKEN), imm_string);
      }

      if (str.size() > 0) {
        str += "\n";
      }
      str += base;
    }
    // Add a newline at the end.
    str += "\n";
    return str;
  }

  // This is intended to be run as a test.
  bool CheckTools() {
    return test_helper_->CheckTools();
  }

  // The following functions are public so that TestFn can use them...

  virtual std::vector<Reg*> GetRegisters() = 0;

  virtual std::vector<FPReg*> GetFPRegisters() {
    UNIMPLEMENTED(FATAL) << "Architecture does not support floating-point registers";
    UNREACHABLE();
  }

  // Secondary register names are the secondary view on registers, e.g., 32b on 64b systems.
  virtual std::string GetSecondaryRegisterName(const Reg& reg ATTRIBUTE_UNUSED) {
    UNIMPLEMENTED(FATAL) << "Architecture does not support secondary registers";
    UNREACHABLE();
  }

  // Tertiary register names are the tertiary view on registers, e.g., 16b on 64b systems.
  virtual std::string GetTertiaryRegisterName(const Reg& reg ATTRIBUTE_UNUSED) {
    UNIMPLEMENTED(FATAL) << "Architecture does not support tertiary registers";
    UNREACHABLE();
  }

  // Quaternary register names are the quaternary view on registers, e.g., 8b on 64b systems.
  virtual std::string GetQuaternaryRegisterName(const Reg& reg ATTRIBUTE_UNUSED) {
    UNIMPLEMENTED(FATAL) << "Architecture does not support quaternary registers";
    UNREACHABLE();
  }

  std::string GetRegisterName(const Reg& reg) {
    return GetRegName<RegisterView::kUsePrimaryName>(reg);
  }

 protected:
  explicit AssemblerTest() {}

  void SetUp() OVERRIDE {
    arena_.reset(new ArenaAllocator(&pool_));
    assembler_.reset(new (arena_.get()) Ass(arena_.get()));
    test_helper_.reset(
        new AssemblerTestInfrastructure(GetArchitectureString(),
                                        GetAssemblerCmdName(),
                                        GetAssemblerParameters(),
                                        GetObjdumpCmdName(),
                                        GetObjdumpParameters(),
                                        GetDisassembleCmdName(),
                                        GetDisassembleParameters(),
                                        GetAssemblyHeader()));

    SetUpHelpers();
  }

  void TearDown() OVERRIDE {
    test_helper_.reset();  // Clean up the helper.
    assembler_.reset();
    arena_.reset();
  }

  // Override this to set up any architecture-specific things, e.g., register vectors.
  virtual void SetUpHelpers() {}

  // Get the typically used name for this architecture, e.g., aarch64, x86_64, ...
  virtual std::string GetArchitectureString() = 0;

  // Get the name of the assembler, e.g., "as" by default.
  virtual std::string GetAssemblerCmdName() {
    return "as";
  }

  // Switches to the assembler command. Default none.
  virtual std::string GetAssemblerParameters() {
    return "";
  }

  // Get the name of the objdump, e.g., "objdump" by default.
  virtual std::string GetObjdumpCmdName() {
    return "objdump";
  }

  // Switches to the objdump command. Default is " -h".
  virtual std::string GetObjdumpParameters() {
    return " -h";
  }

  // Get the name of the objdump, e.g., "objdump" by default.
  virtual std::string GetDisassembleCmdName() {
    return "objdump";
  }

  // Switches to the objdump command. As it's a binary, one needs to push the architecture and
  // such to objdump, so it's architecture-specific and there is no default.
  virtual std::string GetDisassembleParameters() = 0;

  // Create a couple of immediate values up to the number of bytes given.
  virtual std::vector<int64_t> CreateImmediateValues(size_t imm_bytes, bool as_uint = false) {
    std::vector<int64_t> res;
    res.push_back(0);
    if (!as_uint) {
      res.push_back(-1);
    } else {
      res.push_back(0xFF);
    }
    res.push_back(0x12);
    if (imm_bytes >= 2) {
      res.push_back(0x1234);
      if (!as_uint) {
        res.push_back(-0x1234);
      } else {
        res.push_back(0xFFFF);
      }
      if (imm_bytes >= 4) {
        res.push_back(0x12345678);
        if (!as_uint) {
          res.push_back(-0x12345678);
        } else {
          res.push_back(0xFFFFFFFF);
        }
        if (imm_bytes >= 6) {
          res.push_back(0x123456789ABC);
          if (!as_uint) {
            res.push_back(-0x123456789ABC);
          }
          if (imm_bytes >= 8) {
            res.push_back(0x123456789ABCDEF0);
            if (!as_uint) {
              res.push_back(-0x123456789ABCDEF0);
            } else {
              res.push_back(0xFFFFFFFFFFFFFFFF);
            }
          }
        }
      }
    }
    return res;
  }

  const int kMaxBitsExhaustiveTest = 8;

  // Create a couple of immediate values up to the number of bits given.
  virtual std::vector<int64_t> CreateImmediateValuesBits(const int imm_bits, bool as_uint = false) {
    CHECK_GT(imm_bits, 0);
    CHECK_LE(imm_bits, 64);
    std::vector<int64_t> res;

    if (imm_bits <= kMaxBitsExhaustiveTest) {
      if (as_uint) {
        for (uint64_t i = MinInt<uint64_t>(imm_bits); i <= MaxInt<uint64_t>(imm_bits); i++) {
          res.push_back(static_cast<int64_t>(i));
        }
      } else {
        for (int64_t i = MinInt<int64_t>(imm_bits); i <= MaxInt<int64_t>(imm_bits); i++) {
          res.push_back(i);
        }
      }
    } else {
      if (as_uint) {
        for (uint64_t i = MinInt<uint64_t>(kMaxBitsExhaustiveTest);
             i <= MaxInt<uint64_t>(kMaxBitsExhaustiveTest);
             i++) {
          res.push_back(static_cast<int64_t>(i));
        }
        for (int i = 0; i <= imm_bits; i++) {
          uint64_t j = (MaxInt<uint64_t>(kMaxBitsExhaustiveTest) + 1) +
                       ((MaxInt<uint64_t>(imm_bits) -
                        (MaxInt<uint64_t>(kMaxBitsExhaustiveTest) + 1))
                        * i / imm_bits);
          res.push_back(static_cast<int64_t>(j));
        }
      } else {
        for (int i = 0; i <= imm_bits; i++) {
          int64_t j = MinInt<int64_t>(imm_bits) +
                      ((((MinInt<int64_t>(kMaxBitsExhaustiveTest) - 1) -
                         MinInt<int64_t>(imm_bits))
                        * i) / imm_bits);
          res.push_back(static_cast<int64_t>(j));
        }
        for (int64_t i = MinInt<int64_t>(kMaxBitsExhaustiveTest);
             i <= MaxInt<int64_t>(kMaxBitsExhaustiveTest);
             i++) {
          res.push_back(static_cast<int64_t>(i));
        }
        for (int i = 0; i <= imm_bits; i++) {
          int64_t j = (MaxInt<int64_t>(kMaxBitsExhaustiveTest) + 1) +
                      ((MaxInt<int64_t>(imm_bits) - (MaxInt<int64_t>(kMaxBitsExhaustiveTest) + 1))
                       * i / imm_bits);
          res.push_back(static_cast<int64_t>(j));
        }
      }
    }

    return res;
  }

  // Create an immediate from the specific value.
  virtual Imm CreateImmediate(int64_t imm_value) = 0;

  template <typename RegType>
  std::string RepeatTemplatedRegister(void (Ass::*f)(RegType),
                                      const std::vector<RegType*> registers,
                                      std::string (AssemblerTest::*GetName)(const RegType&),
                                      std::string fmt) {
    std::string str;
    for (auto reg : registers) {
      (assembler_.get()->*f)(*reg);
      std::string base = fmt;

      std::string reg_string = (this->*GetName)(*reg);
      size_t reg_index;
      if ((reg_index = base.find(REG_TOKEN)) != std::string::npos) {
        base.replace(reg_index, ConstexprStrLen(REG_TOKEN), reg_string);
      }

      if (str.size() > 0) {
        str += "\n";
      }
      str += base;
    }
    // Add a newline at the end.
    str += "\n";
    return str;
  }

  template <typename Reg1, typename Reg2>
  std::string RepeatTemplatedRegisters(void (Ass::*f)(Reg1, Reg2),
                                       const std::vector<Reg1*> reg1_registers,
                                       const std::vector<Reg2*> reg2_registers,
                                       std::string (AssemblerTest::*GetName1)(const Reg1&),
                                       std::string (AssemblerTest::*GetName2)(const Reg2&),
                                       std::string fmt) {
    WarnOnCombinations(reg1_registers.size() * reg2_registers.size());

    std::string str;
    for (auto reg1 : reg1_registers) {
      for (auto reg2 : reg2_registers) {
        (assembler_.get()->*f)(*reg1, *reg2);
        std::string base = fmt;

        std::string reg1_string = (this->*GetName1)(*reg1);
        size_t reg1_index;
        while ((reg1_index = base.find(REG1_TOKEN)) != std::string::npos) {
          base.replace(reg1_index, ConstexprStrLen(REG1_TOKEN), reg1_string);
        }

        std::string reg2_string = (this->*GetName2)(*reg2);
        size_t reg2_index;
        while ((reg2_index = base.find(REG2_TOKEN)) != std::string::npos) {
          base.replace(reg2_index, ConstexprStrLen(REG2_TOKEN), reg2_string);
        }

        if (str.size() > 0) {
          str += "\n";
        }
        str += base;
      }
    }
    // Add a newline at the end.
    str += "\n";
    return str;
  }

  template <typename Reg1, typename Reg2>
  std::string RepeatTemplatedRegistersNoDupes(void (Ass::*f)(Reg1, Reg2),
                                              const std::vector<Reg1*> reg1_registers,
                                              const std::vector<Reg2*> reg2_registers,
                                              std::string (AssemblerTest::*GetName1)(const Reg1&),
                                              std::string (AssemblerTest::*GetName2)(const Reg2&),
                                              std::string fmt) {
    WarnOnCombinations(reg1_registers.size() * reg2_registers.size());

    std::string str;
    for (auto reg1 : reg1_registers) {
      for (auto reg2 : reg2_registers) {
        if (reg1 == reg2) continue;
        (assembler_.get()->*f)(*reg1, *reg2);
        std::string base = fmt;

        std::string reg1_string = (this->*GetName1)(*reg1);
        size_t reg1_index;
        while ((reg1_index = base.find(REG1_TOKEN)) != std::string::npos) {
          base.replace(reg1_index, ConstexprStrLen(REG1_TOKEN), reg1_string);
        }

        std::string reg2_string = (this->*GetName2)(*reg2);
        size_t reg2_index;
        while ((reg2_index = base.find(REG2_TOKEN)) != std::string::npos) {
          base.replace(reg2_index, ConstexprStrLen(REG2_TOKEN), reg2_string);
        }

        if (str.size() > 0) {
          str += "\n";
        }
        str += base;
      }
    }
    // Add a newline at the end.
    str += "\n";
    return str;
  }

  template <typename Reg1, typename Reg2, typename Reg3>
  std::string RepeatTemplatedRegisters(void (Ass::*f)(Reg1, Reg2, Reg3),
                                       const std::vector<Reg1*> reg1_registers,
                                       const std::vector<Reg2*> reg2_registers,
                                       const std::vector<Reg3*> reg3_registers,
                                       std::string (AssemblerTest::*GetName1)(const Reg1&),
                                       std::string (AssemblerTest::*GetName2)(const Reg2&),
                                       std::string (AssemblerTest::*GetName3)(const Reg3&),
                                       std::string fmt) {
    std::string str;
    for (auto reg1 : reg1_registers) {
      for (auto reg2 : reg2_registers) {
        for (auto reg3 : reg3_registers) {
          (assembler_.get()->*f)(*reg1, *reg2, *reg3);
          std::string base = fmt;

          std::string reg1_string = (this->*GetName1)(*reg1);
          size_t reg1_index;
          while ((reg1_index = base.find(REG1_TOKEN)) != std::string::npos) {
            base.replace(reg1_index, ConstexprStrLen(REG1_TOKEN), reg1_string);
          }

          std::string reg2_string = (this->*GetName2)(*reg2);
          size_t reg2_index;
          while ((reg2_index = base.find(REG2_TOKEN)) != std::string::npos) {
            base.replace(reg2_index, ConstexprStrLen(REG2_TOKEN), reg2_string);
          }

          std::string reg3_string = (this->*GetName3)(*reg3);
          size_t reg3_index;
          while ((reg3_index = base.find(REG3_TOKEN)) != std::string::npos) {
            base.replace(reg3_index, ConstexprStrLen(REG3_TOKEN), reg3_string);
          }

          if (str.size() > 0) {
            str += "\n";
          }
          str += base;
        }
      }
    }
    // Add a newline at the end.
    str += "\n";
    return str;
  }

  template <typename Reg1, typename Reg2>
  std::string RepeatTemplatedRegistersImm(void (Ass::*f)(Reg1, Reg2, const Imm&),
                                          const std::vector<Reg1*> reg1_registers,
                                          const std::vector<Reg2*> reg2_registers,
                                          std::string (AssemblerTest::*GetName1)(const Reg1&),
                                          std::string (AssemblerTest::*GetName2)(const Reg2&),
                                          size_t imm_bytes,
                                          std::string fmt) {
    std::vector<int64_t> imms = CreateImmediateValues(imm_bytes);
    WarnOnCombinations(reg1_registers.size() * reg2_registers.size() * imms.size());

    std::string str;
    for (auto reg1 : reg1_registers) {
      for (auto reg2 : reg2_registers) {
        for (int64_t imm : imms) {
          Imm new_imm = CreateImmediate(imm);
          (assembler_.get()->*f)(*reg1, *reg2, new_imm);
          std::string base = fmt;

          std::string reg1_string = (this->*GetName1)(*reg1);
          size_t reg1_index;
          while ((reg1_index = base.find(REG1_TOKEN)) != std::string::npos) {
            base.replace(reg1_index, ConstexprStrLen(REG1_TOKEN), reg1_string);
          }

          std::string reg2_string = (this->*GetName2)(*reg2);
          size_t reg2_index;
          while ((reg2_index = base.find(REG2_TOKEN)) != std::string::npos) {
            base.replace(reg2_index, ConstexprStrLen(REG2_TOKEN), reg2_string);
          }

          size_t imm_index = base.find(IMM_TOKEN);
          if (imm_index != std::string::npos) {
            std::ostringstream sreg;
            sreg << imm;
            std::string imm_string = sreg.str();
            base.replace(imm_index, ConstexprStrLen(IMM_TOKEN), imm_string);
          }

          if (str.size() > 0) {
            str += "\n";
          }
          str += base;
        }
      }
    }
    // Add a newline at the end.
    str += "\n";
    return str;
  }

  template <RegisterView kRegView>
  std::string GetRegName(const Reg& reg) {
    std::ostringstream sreg;
    switch (kRegView) {
      case RegisterView::kUsePrimaryName:
        sreg << reg;
        break;

      case RegisterView::kUseSecondaryName:
        sreg << GetSecondaryRegisterName(reg);
        break;

      case RegisterView::kUseTertiaryName:
        sreg << GetTertiaryRegisterName(reg);
        break;

      case RegisterView::kUseQuaternaryName:
        sreg << GetQuaternaryRegisterName(reg);
        break;
    }
    return sreg.str();
  }

  std::string GetFPRegName(const FPReg& reg) {
    std::ostringstream sreg;
    sreg << reg;
    return sreg.str();
  }

  // If the assembly file needs a header, return it in a sub-class.
  virtual const char* GetAssemblyHeader() {
    return nullptr;
  }

  void WarnOnCombinations(size_t count) {
    if (count > kWarnManyCombinationsThreshold) {
      GTEST_LOG_(WARNING) << "Many combinations (" << count << "), test generation might be slow.";
    }
  }

  static constexpr const char* REG_TOKEN = "{reg}";
  static constexpr const char* REG1_TOKEN = "{reg1}";
  static constexpr const char* REG2_TOKEN = "{reg2}";
  static constexpr const char* REG3_TOKEN = "{reg3}";
  static constexpr const char* IMM_TOKEN = "{imm}";

 private:
  template <RegisterView kRegView>
  std::string RepeatRegisterImm(void (Ass::*f)(Reg, const Imm&), size_t imm_bytes,
                                  std::string fmt) {
    const std::vector<Reg*> registers = GetRegisters();
    std::string str;
    std::vector<int64_t> imms = CreateImmediateValues(imm_bytes);

    WarnOnCombinations(registers.size() * imms.size());

    for (auto reg : registers) {
      for (int64_t imm : imms) {
        Imm new_imm = CreateImmediate(imm);
        (assembler_.get()->*f)(*reg, new_imm);
        std::string base = fmt;

        std::string reg_string = GetRegName<kRegView>(*reg);
        size_t reg_index;
        while ((reg_index = base.find(REG_TOKEN)) != std::string::npos) {
          base.replace(reg_index, ConstexprStrLen(REG_TOKEN), reg_string);
        }

        size_t imm_index = base.find(IMM_TOKEN);
        if (imm_index != std::string::npos) {
          std::ostringstream sreg;
          sreg << imm;
          std::string imm_string = sreg.str();
          base.replace(imm_index, ConstexprStrLen(IMM_TOKEN), imm_string);
        }

        if (str.size() > 0) {
          str += "\n";
        }
        str += base;
      }
    }
    // Add a newline at the end.
    str += "\n";
    return str;
  }

  // Override this to pad the code with NOPs to a certain size if needed.
  virtual void Pad(std::vector<uint8_t>& data ATTRIBUTE_UNUSED) {
  }

  void DriverWrapper(std::string assembly_text, std::string test_name) {
    assembler_->FinalizeCode();
    size_t cs = assembler_->CodeSize();
    std::unique_ptr<std::vector<uint8_t>> data(new std::vector<uint8_t>(cs));
    MemoryRegion code(&(*data)[0], data->size());
    assembler_->FinalizeInstructions(code);
    Pad(*data);
    test_helper_->Driver(*data, assembly_text, test_name);
  }

  static constexpr size_t kWarnManyCombinationsThreshold = 500;

  ArenaPool pool_;
  std::unique_ptr<ArenaAllocator> arena_;
  std::unique_ptr<Ass> assembler_;
  std::unique_ptr<AssemblerTestInfrastructure> test_helper_;

  DISALLOW_COPY_AND_ASSIGN(AssemblerTest);
};

}  // namespace art

#endif  // ART_COMPILER_UTILS_ASSEMBLER_TEST_H_
