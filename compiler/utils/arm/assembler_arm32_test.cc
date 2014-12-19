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

#include "assembler_arm32.h"

#include <functional>
#include <type_traits>

#include "base/macros.h"
#include "base/stl_util.h"
#include "utils/arm/assembler_arm_test.h"

namespace art {

using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;
using std::placeholders::_4;
using std::placeholders::_5;

// To speed up tests, don't use all register combinations.
static constexpr bool kUseSparseRegisterList = true;

// To speed up tests, don't use all condition codes.
static constexpr bool kUseSparseConditionList = true;

// To speed up tests, don't use all shift immediates.
static constexpr bool kUseSparseShiftImmediates = true;

class AssemblerArm32Test : public AssemblerArmTest<arm::Arm32Assembler,
                                                   arm::Register, arm::SRegister,
                                                   uint32_t, arm::ShifterOperand, arm::Condition> {
 protected:
  std::string GetArchitectureString() OVERRIDE {
    return "arm";
  }

  std::string GetAssemblerParameters() OVERRIDE {
    // Arm-v7a, cortex-a15 (means we have sdiv).
    return " -march=armv7-a -mcpu=cortex-a15 -mfpu=neon";
  }

  const char* GetAssemblyHeader() OVERRIDE {
    return kArm32AssemblyHeader;
  }

  std::string GetDisassembleParameters() OVERRIDE {
    return " -D -bbinary -marm --no-show-raw-insn";
  }

  void SetUpHelpers() OVERRIDE {
    if (registers_.size() == 0) {
      if (kUseSparseRegisterList) {
        registers_.insert(end(registers_),
                          {  // NOLINT(whitespace/braces)
                              new arm::Register(arm::R0),
                              new arm::Register(arm::R1),
                              new arm::Register(arm::R4),
                              new arm::Register(arm::R8),
                              new arm::Register(arm::R11),
                              new arm::Register(arm::R12),
                              new arm::Register(arm::R13),
                              new arm::Register(arm::R14),
                              new arm::Register(arm::R15)
                          });
      } else {
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

    if (!kUseSparseConditionList) {
      conditions_.push_back(arm::Condition::EQ);
      conditions_.push_back(arm::Condition::NE);
      conditions_.push_back(arm::Condition::CS);
      conditions_.push_back(arm::Condition::CC);
      conditions_.push_back(arm::Condition::MI);
      conditions_.push_back(arm::Condition::PL);
      conditions_.push_back(arm::Condition::VS);
      conditions_.push_back(arm::Condition::VC);
      conditions_.push_back(arm::Condition::HI);
      conditions_.push_back(arm::Condition::LS);
      conditions_.push_back(arm::Condition::GE);
      conditions_.push_back(arm::Condition::LT);
      conditions_.push_back(arm::Condition::GT);
      conditions_.push_back(arm::Condition::LE);
      conditions_.push_back(arm::Condition::AL);
    } else {
      conditions_.push_back(arm::Condition::EQ);
      conditions_.push_back(arm::Condition::NE);
      conditions_.push_back(arm::Condition::CC);
      conditions_.push_back(arm::Condition::VC);
      conditions_.push_back(arm::Condition::HI);
      conditions_.push_back(arm::Condition::LT);
      conditions_.push_back(arm::Condition::AL);
    }

    shifter_operands_.push_back(arm::ShifterOperand(0));
    shifter_operands_.push_back(arm::ShifterOperand(1));
    shifter_operands_.push_back(arm::ShifterOperand(2));
    shifter_operands_.push_back(arm::ShifterOperand(3));
    shifter_operands_.push_back(arm::ShifterOperand(4));
    shifter_operands_.push_back(arm::ShifterOperand(5));
    shifter_operands_.push_back(arm::ShifterOperand(127));
    shifter_operands_.push_back(arm::ShifterOperand(128));
    shifter_operands_.push_back(arm::ShifterOperand(254));
    shifter_operands_.push_back(arm::ShifterOperand(255));

    if (!kUseSparseRegisterList) {
      shifter_operands_.push_back(arm::ShifterOperand(arm::R0));
      shifter_operands_.push_back(arm::ShifterOperand(arm::R1));
      shifter_operands_.push_back(arm::ShifterOperand(arm::R2));
      shifter_operands_.push_back(arm::ShifterOperand(arm::R3));
      shifter_operands_.push_back(arm::ShifterOperand(arm::R4));
      shifter_operands_.push_back(arm::ShifterOperand(arm::R5));
      shifter_operands_.push_back(arm::ShifterOperand(arm::R6));
      shifter_operands_.push_back(arm::ShifterOperand(arm::R7));
      shifter_operands_.push_back(arm::ShifterOperand(arm::R8));
      shifter_operands_.push_back(arm::ShifterOperand(arm::R9));
      shifter_operands_.push_back(arm::ShifterOperand(arm::R10));
      shifter_operands_.push_back(arm::ShifterOperand(arm::R11));
      shifter_operands_.push_back(arm::ShifterOperand(arm::R12));
      shifter_operands_.push_back(arm::ShifterOperand(arm::R13));
    } else {
      shifter_operands_.push_back(arm::ShifterOperand(arm::R0));
      shifter_operands_.push_back(arm::ShifterOperand(arm::R1));
      shifter_operands_.push_back(arm::ShifterOperand(arm::R4));
      shifter_operands_.push_back(arm::ShifterOperand(arm::R8));
      shifter_operands_.push_back(arm::ShifterOperand(arm::R11));
      shifter_operands_.push_back(arm::ShifterOperand(arm::R12));
      shifter_operands_.push_back(arm::ShifterOperand(arm::R13));
    }

    std::vector<arm::Shift> shifts {
      arm::Shift::LSL, arm::Shift::LSR, arm::Shift::ASR, arm::Shift::ROR, arm::Shift::RRX
    };

    // ShifterOperands of form "reg shift-type imm."
    for (arm::Shift shift : shifts) {
      for (arm::Register* reg : registers_) {  // Note: this will pick up the sparse set.
        if (*reg == arm::R15) {  // Skip PC.
          continue;
        }
        if (shift != arm::Shift::RRX) {
          if (!kUseSparseShiftImmediates) {
            for (uint32_t imm = 1; imm < 32; ++imm) {
              shifter_operands_.push_back(arm::ShifterOperand(*reg, shift, imm));
            }
          } else {
            shifter_operands_.push_back(arm::ShifterOperand(*reg, shift, 1));
            shifter_operands_.push_back(arm::ShifterOperand(*reg, shift, 2));
            shifter_operands_.push_back(arm::ShifterOperand(*reg, shift, 3));
            shifter_operands_.push_back(arm::ShifterOperand(*reg, shift, 7));
            shifter_operands_.push_back(arm::ShifterOperand(*reg, shift, 15));
            shifter_operands_.push_back(arm::ShifterOperand(*reg, shift, 16));
            shifter_operands_.push_back(arm::ShifterOperand(*reg, shift, 30));
            shifter_operands_.push_back(arm::ShifterOperand(*reg, shift, 31));
          }
        } else {
          // RRX doesn't have an immediate.
          shifter_operands_.push_back(arm::ShifterOperand(*reg, shift, 0));
        }
      }
    }
  }

  std::vector<arm::ShifterOperand> CreateRegisterShifts(std::vector<arm::Register*>& base_regs,
                                                        int32_t shift_min, int32_t shift_max) {
    std::vector<arm::ShifterOperand> res;
    static constexpr arm::Shift kShifts[] = { arm::Shift::LSL, arm::Shift::LSR, arm::Shift::ASR,
                                              arm::Shift::ROR };

    for (arm::Shift shift : kShifts) {
      for (arm::Register* reg : base_regs) {
        // Take the min, the max, and three values in between.
        res.push_back(arm::ShifterOperand(*reg, shift, shift_min));
        if (shift_min != shift_max) {
          res.push_back(arm::ShifterOperand(*reg, shift, shift_max));
          int32_t middle = (shift_min + shift_max) / 2;
          res.push_back(arm::ShifterOperand(*reg, shift, middle));
          res.push_back(arm::ShifterOperand(*reg, shift, middle - 1));
          res.push_back(arm::ShifterOperand(*reg, shift, middle + 1));
        }
      }
    }

    return res;
  }

  void TearDown() OVERRIDE {
    AssemblerArmTest::TearDown();
    STLDeleteElements(&registers_);
  }

  std::vector<arm::Register*> GetRegisters() OVERRIDE {
    return registers_;
  }

  uint32_t CreateImmediate(int64_t imm_value) OVERRIDE {
    return imm_value;
  }

  std::vector<arm::Condition>& GetConditions() OVERRIDE {
    return conditions_;
  }

  std::string GetConditionString(arm::Condition c) OVERRIDE {
    std::ostringstream oss;
    oss << c;
    return oss.str();
  }

  arm::Register GetPCRegister() OVERRIDE {
    return arm::R15;
  }

  std::vector<arm::ShifterOperand>& GetShiftOperands() OVERRIDE {
    return shifter_operands_;
  }

  std::string GetShiftString(arm::ShifterOperand sop) OVERRIDE {
    std::ostringstream oss;
    if (sop.IsShift()) {
      // Not a rotate...
      if (sop.GetShift() == arm::Shift::RRX) {
        oss << sop.GetRegister() << ", " << sop.GetShift();
      } else {
        oss << sop.GetRegister() << ", " << sop.GetShift() << " #" << sop.GetImmediate();
      }
    } else if (sop.IsRegister()) {
      oss << sop.GetRegister();
    } else {
      CHECK(sop.IsImmediate());
      oss << "#" << sop.GetImmediate();
    }
    return oss.str();
  }

  static const char* GetRegTokenFromDepth(int depth) {
    switch (depth) {
      case 0:
        return Base::REG1_TOKEN;
      case 1:
        return Base::REG2_TOKEN;
      case 2:
        return REG3_TOKEN;
      case 3:
        return REG4_TOKEN;
      default:
        LOG(FATAL) << "Depth problem.";
        UNREACHABLE();
    }
  }

  void ExecuteAndPrint(std::function<void()> f, std::string fmt, std::ostringstream& oss) {
    if (first_) {
      first_ = false;
    } else {
      oss << "\n";
    }
    oss << fmt;

    f();
  }

  void TemplateHelper(std::function<void(arm::Register)> f, int depth ATTRIBUTE_UNUSED,
                      bool without_pc,
                      std::string fmt, std::ostringstream& oss) {
    std::vector<arm::Register*> registers = without_pc ? GetRegistersWithoutPC() : GetRegisters();
    for (auto reg : registers) {
      std::string after_reg = fmt;

      std::string reg_string = GetRegName<RegisterView::kUsePrimaryName>(*reg);
      size_t reg_index;
      const char* reg_token = GetRegTokenFromDepth(depth);

      while ((reg_index = after_reg.find(reg_token)) != std::string::npos) {
        after_reg.replace(reg_index, strlen(reg_token), reg_string);
      }

      ExecuteAndPrint([&] () { f(*reg); }, after_reg, oss);
    }
  }

  void TemplateHelper(std::function<void(const arm::ShifterOperand&)> f, int depth ATTRIBUTE_UNUSED,
                      bool without_pc ATTRIBUTE_UNUSED, std::string fmt, std::ostringstream& oss) {
    for (const arm::ShifterOperand& shift : GetShiftOperands()) {
      std::string after_shift = fmt;

      std::string shift_string = GetShiftString(shift);
      size_t shift_index;
      while ((shift_index = after_shift.find(SHIFT_TOKEN)) != std::string::npos) {
        after_shift.replace(shift_index, ConstexprStrLen(SHIFT_TOKEN), shift_string);
      }

      ExecuteAndPrint([&] () { f(shift); }, after_shift, oss);
    }
  }

  void TemplateHelper(std::function<void(arm::Condition)> f, int depth ATTRIBUTE_UNUSED,
                      bool without_pc ATTRIBUTE_UNUSED, std::string fmt, std::ostringstream& oss) {
    for (arm::Condition c : GetConditions()) {
      std::string after_cond = fmt;

      size_t cond_index = after_cond.find(COND_TOKEN);
      if (cond_index != std::string::npos) {
        after_cond.replace(cond_index, ConstexprStrLen(IMM1_TOKEN), GetConditionString(c));
      }

      ExecuteAndPrint([&] () { f(c); }, after_cond, oss);
    }
  }

  template <typename... Args>
  void TemplateHelper(std::function<void(arm::Register, Args...)> f, int depth, bool without_pc,
                      std::string fmt, std::ostringstream& oss) {
    std::vector<arm::Register*> registers = without_pc ? GetRegistersWithoutPC() : GetRegisters();
    for (auto reg : registers) {
      std::string after_reg = fmt;

      std::string reg_string = GetRegName<RegisterView::kUsePrimaryName>(*reg);
      size_t reg_index;
      const char* reg_token = GetRegTokenFromDepth(depth);

      while ((reg_index = after_reg.find(reg_token)) != std::string::npos) {
        after_reg.replace(reg_index, strlen(reg_token), reg_string);
      }

      auto lambda = [&] (Args... args) { f(*reg, args...); };  // NOLINT [readability/braces] [4]
      TemplateHelper(std::function<void(Args...)>(lambda), depth + 1, without_pc,
          after_reg, oss);
    }
  }

  template <typename... Args>
  void TemplateHelper(std::function<void(const arm::ShifterOperand&, Args...)> f, int depth,
                      bool without_pc, std::string fmt, std::ostringstream& oss) {
    for (const arm::ShifterOperand& shift : GetShiftOperands()) {
      std::string after_shift = fmt;

      std::string shift_string = GetShiftString(shift);
      size_t shift_index;
      while ((shift_index = after_shift.find(SHIFT_TOKEN)) != std::string::npos) {
        after_shift.replace(shift_index, ConstexprStrLen(SHIFT_TOKEN), shift_string);
      }

      auto lambda = [&] (Args... args) { f(shift, args...); };  // NOLINT [readability/braces] [4]
      TemplateHelper(std::function<void(Args...)>(lambda), depth, without_pc,
          after_shift, oss);
    }
  }

  template <typename... Args>
  void TemplateHelper(std::function<void(arm::Condition, Args...)> f, int depth, bool without_pc,
                      std::string fmt, std::ostringstream& oss) {
    for (arm::Condition c : GetConditions()) {
      std::string after_cond = fmt;

      size_t cond_index = after_cond.find(COND_TOKEN);
      if (cond_index != std::string::npos) {
        after_cond.replace(cond_index, ConstexprStrLen(IMM1_TOKEN), GetConditionString(c));
      }

      auto lambda = [&] (Args... args) { f(c, args...); };  // NOLINT [readability/braces] [4]
      TemplateHelper(std::function<void(Args...)>(lambda), depth, without_pc,
          after_cond, oss);
    }
  }

  template <typename T1, typename T2>
  std::function<void(T1, T2)> GetBoundFunction2(void (arm::Arm32Assembler::*f)(T1, T2)) {
    return std::bind(f, GetAssembler(), _1, _2);
  }

  template <typename T1, typename T2, typename T3>
  std::function<void(T1, T2, T3)> GetBoundFunction3(void (arm::Arm32Assembler::*f)(T1, T2, T3)) {
    return std::bind(f, GetAssembler(), _1, _2, _3);
  }

  template <typename T1, typename T2, typename T3, typename T4>
  std::function<void(T1, T2, T3, T4)> GetBoundFunction4(
      void (arm::Arm32Assembler::*f)(T1, T2, T3, T4)) {
    return std::bind(f, GetAssembler(), _1, _2, _3, _4);
  }

  template <typename T1, typename T2, typename T3, typename T4, typename T5>
  std::function<void(T1, T2, T3, T4, T5)> GetBoundFunction5(
      void (arm::Arm32Assembler::*f)(T1, T2, T3, T4, T5)) {
    return std::bind(f, GetAssembler(), _1, _2, _3, _4, _5);
  }

  template <typename... Args>
  void GenericTemplateHelper(std::function<void(Args...)> f, bool without_pc,
                             std::string fmt, std::string test_name) {
    first_ = false;
    WarnOnCombinations(CountHelper<Args...>(without_pc));

    std::ostringstream oss;

    TemplateHelper(f, 0, without_pc, fmt, oss);

    oss << "\n";  // Trailing newline.

    DriverStr(oss.str(), test_name);
  }

  template <typename... Args>
  void T2Helper(void (arm::Arm32Assembler::*f)(Args...), bool without_pc, std::string fmt,
                std::string test_name) {
    GenericTemplateHelper(GetBoundFunction2(f), without_pc, fmt, test_name);
  }

  template <typename... Args>
  void T3Helper(void (arm::Arm32Assembler::*f)(Args...), bool without_pc, std::string fmt,
      std::string test_name) {
    GenericTemplateHelper(GetBoundFunction3(f), without_pc, fmt, test_name);
  }

  template <typename... Args>
  void T4Helper(void (arm::Arm32Assembler::*f)(Args...), bool without_pc, std::string fmt,
      std::string test_name) {
    GenericTemplateHelper(GetBoundFunction4(f), without_pc, fmt, test_name);
  }

  template <typename... Args>
  void T5Helper(void (arm::Arm32Assembler::*f)(Args...), bool without_pc, std::string fmt,
      std::string test_name) {
    GenericTemplateHelper(GetBoundFunction5(f), without_pc, fmt, test_name);
  }

 private:
  template <typename T>
  size_t CountHelper(bool without_pc) {
    size_t tmp;
    if (std::is_same<T, arm::Register>::value) {
      tmp = GetRegisters().size();
      if (without_pc) {
        tmp--;;  // Approximation...
      }
      return tmp;
    } else if (std::is_same<T, const arm::ShifterOperand&>::value) {
      return GetShiftOperands().size();
    } else if (std::is_same<T, arm::Condition>::value) {
      return GetConditions().size();
    } else {
      LOG(WARNING) << "Unknown type while counting.";
      return 1;
    }
  }

  template <typename T1, typename T2, typename... Args>
  size_t CountHelper(bool without_pc) {
    size_t tmp;
    if (std::is_same<T1, arm::Register>::value) {
      tmp = GetRegisters().size();
      if (without_pc) {
        tmp--;;  // Approximation...
      }
    } else if (std::is_same<T1, const arm::ShifterOperand&>::value) {
      tmp =  GetShiftOperands().size();
    } else if (std::is_same<T1, arm::Condition>::value) {
      tmp = GetConditions().size();
    } else {
      LOG(WARNING) << "Unknown type while counting.";
      tmp = 1;
    }
    size_t rec = CountHelper<T2, Args...>(without_pc);
    return rec * tmp;
  }

  bool first_;

  static constexpr const char* kArm32AssemblyHeader = ".arm\n";

  std::vector<arm::Register*> registers_;
  std::vector<arm::Condition> conditions_;
  std::vector<arm::ShifterOperand> shifter_operands_;
};


TEST_F(AssemblerArm32Test, Toolchain) {
  EXPECT_TRUE(CheckTools());
}

TEST_F(AssemblerArm32Test, Sbfx) {
  std::vector<std::pair<uint32_t, uint32_t>> immediates;
  immediates.push_back({0, 1});
  immediates.push_back({0, 8});
  immediates.push_back({0, 15});
  immediates.push_back({0, 16});
  immediates.push_back({0, 31});
  immediates.push_back({0, 32});

  immediates.push_back({1, 1});
  immediates.push_back({1, 15});
  immediates.push_back({1, 31});

  immediates.push_back({8, 1});
  immediates.push_back({8, 15});
  immediates.push_back({8, 16});
  immediates.push_back({8, 24});

  immediates.push_back({31, 1});

  DriverStr(RepeatRRiiC(&arm::Arm32Assembler::sbfx, immediates,
                        "sbfx{cond} {reg1}, {reg2}, #{imm1}, #{imm2}"), "sbfx");
}

TEST_F(AssemblerArm32Test, Ubfx) {
  std::vector<std::pair<uint32_t, uint32_t>> immediates;
  immediates.push_back({0, 1});
  immediates.push_back({0, 8});
  immediates.push_back({0, 15});
  immediates.push_back({0, 16});
  immediates.push_back({0, 31});
  immediates.push_back({0, 32});

  immediates.push_back({1, 1});
  immediates.push_back({1, 15});
  immediates.push_back({1, 31});

  immediates.push_back({8, 1});
  immediates.push_back({8, 15});
  immediates.push_back({8, 16});
  immediates.push_back({8, 24});

  immediates.push_back({31, 1});

  DriverStr(RepeatRRiiC(&arm::Arm32Assembler::ubfx, immediates,
                        "ubfx{cond} {reg1}, {reg2}, #{imm1}, #{imm2}"), "ubfx");
}

TEST_F(AssemblerArm32Test, Mul) {
  T4Helper(&arm::Arm32Assembler::mul, true, "mul{cond} {reg1}, {reg2}, {reg3}", "mul");
}

TEST_F(AssemblerArm32Test, Mla) {
  T5Helper(&arm::Arm32Assembler::mla, true, "mla{cond} {reg1}, {reg2}, {reg3}, {reg4}", "mul");
}

/* TODO: Needs support to filter out register combinations, as rdhi must not be equal to rdlo.
TEST_F(AssemblerArm32Test, Umull) {
  T5Helper(&arm::Arm32Assembler::umull, true, "umull{cond} {reg1}, {reg2}, {reg3}, {reg4}",
           "umull");
}
*/

TEST_F(AssemblerArm32Test, Sdiv) {
  T4Helper(&arm::Arm32Assembler::sdiv, true, "sdiv{cond} {reg1}, {reg2}, {reg3}", "sdiv");
}

TEST_F(AssemblerArm32Test, Udiv) {
  T4Helper(&arm::Arm32Assembler::udiv, true, "udiv{cond} {reg1}, {reg2}, {reg3}", "udiv");
}

TEST_F(AssemblerArm32Test, And) {
  T4Helper(&arm::Arm32Assembler::and_, true, "and{cond} {reg1}, {reg2}, {shift}", "and");
}

TEST_F(AssemblerArm32Test, Eor) {
  T4Helper(&arm::Arm32Assembler::eor, true, "eor{cond} {reg1}, {reg2}, {shift}", "eor");
}

TEST_F(AssemblerArm32Test, Orr) {
  T4Helper(&arm::Arm32Assembler::orr, true, "orr{cond} {reg1}, {reg2}, {shift}", "orr");
}

TEST_F(AssemblerArm32Test, Orrs) {
  T4Helper(&arm::Arm32Assembler::orrs, true, "orr{cond}s {reg1}, {reg2}, {shift}", "orrs");
}

TEST_F(AssemblerArm32Test, Bic) {
  T4Helper(&arm::Arm32Assembler::bic, true, "bic{cond} {reg1}, {reg2}, {shift}", "bic");
}

TEST_F(AssemblerArm32Test, Mov) {
  T3Helper(&arm::Arm32Assembler::mov, true, "mov{cond} {reg1}, {shift}", "mov");
}

TEST_F(AssemblerArm32Test, Movs) {
  T3Helper(&arm::Arm32Assembler::movs, true, "mov{cond}s {reg1}, {shift}", "movs");
}

TEST_F(AssemblerArm32Test, Mvn) {
  T3Helper(&arm::Arm32Assembler::mvn, true, "mvn{cond} {reg1}, {shift}", "mvn");
}

TEST_F(AssemblerArm32Test, Mvns) {
  T3Helper(&arm::Arm32Assembler::mvns, true, "mvn{cond}s {reg1}, {shift}", "mvns");
}

TEST_F(AssemblerArm32Test, Add) {
  T4Helper(&arm::Arm32Assembler::add, false, "add{cond} {reg1}, {reg2}, {shift}", "add");
}

TEST_F(AssemblerArm32Test, Adds) {
  T4Helper(&arm::Arm32Assembler::adds, false, "add{cond}s {reg1}, {reg2}, {shift}", "adds");
}

TEST_F(AssemblerArm32Test, Adc) {
  T4Helper(&arm::Arm32Assembler::adc, false, "adc{cond} {reg1}, {reg2}, {shift}", "adc");
}

TEST_F(AssemblerArm32Test, Sub) {
  T4Helper(&arm::Arm32Assembler::sub, false, "sub{cond} {reg1}, {reg2}, {shift}", "sub");
}

TEST_F(AssemblerArm32Test, Subs) {
  T4Helper(&arm::Arm32Assembler::subs, false, "sub{cond}s {reg1}, {reg2}, {shift}", "subs");
}

TEST_F(AssemblerArm32Test, Sbc) {
  T4Helper(&arm::Arm32Assembler::sbc, false, "sbc{cond} {reg1}, {reg2}, {shift}", "sbc");
}

TEST_F(AssemblerArm32Test, Rsb) {
  T4Helper(&arm::Arm32Assembler::rsb, true, "rsb{cond} {reg1}, {reg2}, {shift}", "rsb");
}

TEST_F(AssemblerArm32Test, Rsbs) {
  T4Helper(&arm::Arm32Assembler::rsbs, true, "rsb{cond}s {reg1}, {reg2}, {shift}", "rsbs");
}

TEST_F(AssemblerArm32Test, Rsc) {
  T4Helper(&arm::Arm32Assembler::rsc, true, "rsc{cond} {reg1}, {reg2}, {shift}", "rsc");
}

/* TODO: Needs support to filter out register combinations, as reg1 must not be equal to reg3.
TEST_F(AssemblerArm32Test, Strex) {
  RRRCWithoutPCHelper(&arm::Arm32Assembler::strex, "strex{cond} {reg1}, {reg2}, [{reg3}]", "strex");
}
*/

TEST_F(AssemblerArm32Test, Clz) {
  T3Helper(&arm::Arm32Assembler::clz, true, "clz{cond} {reg1}, {reg2}", "clz");
}

TEST_F(AssemblerArm32Test, Tst) {
  T3Helper(&arm::Arm32Assembler::tst, true, "tst{cond} {reg1}, {shift}", "tst");
}

TEST_F(AssemblerArm32Test, Teq) {
  T3Helper(&arm::Arm32Assembler::teq, true, "teq{cond} {reg1}, {shift}", "teq");
}

TEST_F(AssemblerArm32Test, Cmp) {
  T3Helper(&arm::Arm32Assembler::cmp, true, "cmp{cond} {reg1}, {shift}", "cmp");
}

TEST_F(AssemblerArm32Test, Cmn) {
  T3Helper(&arm::Arm32Assembler::cmn, true, "cmn{cond} {reg1}, {shift}", "cmn");
}

TEST_F(AssemblerArm32Test, Blx) {
  T2Helper(&arm::Arm32Assembler::blx, true, "blx{cond} {reg1}", "blx");
}

TEST_F(AssemblerArm32Test, Bx) {
  T2Helper(&arm::Arm32Assembler::bx, true, "bx{cond} {reg1}", "bx");
}

TEST_F(AssemblerArm32Test, Vmstat) {
  GetAssembler()->vmstat();

  const char* expected = "vmrs APSR_nzcv, FPSCR\n";

  DriverStr(expected, "vmrs");
}

TEST_F(AssemblerArm32Test, ldrexd) {
  GetAssembler()->ldrexd(arm::R0, arm::R1, arm::R0);
  GetAssembler()->ldrexd(arm::R0, arm::R1, arm::R1);
  GetAssembler()->ldrexd(arm::R0, arm::R1, arm::R2);

  const char* expected =
      "ldrexd r0, r1, [r0]\n"
      "ldrexd r0, r1, [r1]\n"
      "ldrexd r0, r1, [r2]\n";
  DriverStr(expected, "ldrexd");
}

TEST_F(AssemblerArm32Test, strexd) {
  GetAssembler()->strexd(arm::R9, arm::R0, arm::R1, arm::R0);
  GetAssembler()->strexd(arm::R9, arm::R0, arm::R1, arm::R1);
  GetAssembler()->strexd(arm::R9, arm::R0, arm::R1, arm::R2);

  const char* expected =
      "strexd r9, r0, r1, [r0]\n"
      "strexd r9, r0, r1, [r1]\n"
      "strexd r9, r0, r1, [r2]\n";
  DriverStr(expected, "strexd");
}

}  // namespace art
