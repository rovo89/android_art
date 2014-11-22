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

#ifndef ART_COMPILER_UTILS_ARM_ASSEMBLER_ARM_TEST_H_
#define ART_COMPILER_UTILS_ARM_ASSEMBLER_ARM_TEST_H_

#include "utils/assembler_test.h"

namespace art {

template<typename Ass, typename Reg, typename FPReg, typename Imm, typename SOp, typename Cond>
class AssemblerArmTest : public AssemblerTest<Ass, Reg, FPReg, Imm> {
 public:
  typedef AssemblerTest<Ass, Reg, FPReg, Imm> Base;

  using Base::GetRegisters;
  using Base::GetRegName;
  using Base::CreateImmediate;
  using Base::WarnOnCombinations;

  static constexpr int64_t kFullImmRangeThreshold = 32;

  virtual void FillImmediates(std::vector<Imm>& immediates, int64_t imm_min, int64_t imm_max) {
    // Small range: do completely.
    if (imm_max - imm_min <= kFullImmRangeThreshold) {
      for (int64_t i = imm_min; i <= imm_max; ++i) {
        immediates.push_back(CreateImmediate(i));
      }
    } else {
      immediates.push_back(CreateImmediate(imm_min));
      immediates.push_back(CreateImmediate(imm_max));
      if (imm_min < imm_max - 1) {
        immediates.push_back(CreateImmediate(imm_min + 1));
      }
      if (imm_min < imm_max - 2) {
        immediates.push_back(CreateImmediate(imm_min + 2));
      }
      if (imm_min < imm_max - 3) {
        immediates.push_back(CreateImmediate(imm_max - 1));
      }
      if (imm_min < imm_max - 4) {
        immediates.push_back(CreateImmediate((imm_min + imm_max) / 2));
      }
    }
  }

  std::string RepeatRRIIC(void (Ass::*f)(Reg, Reg, Imm, Imm, Cond),
                          int64_t imm1_min, int64_t imm1_max,
                          int64_t imm2_min, int64_t imm2_max,
                          std::string fmt) {
    return RepeatTemplatedRRIIC(f, GetRegisters(), GetRegisters(),
                                &AssemblerArmTest::template GetRegName<RegisterView::kUsePrimaryName>,
                                &AssemblerArmTest::template GetRegName<RegisterView::kUsePrimaryName>,
                                imm1_min, imm1_max, imm2_min, imm2_max,
                                fmt);
  }

  template <typename Reg1, typename Reg2>
  std::string RepeatTemplatedRRIIC(void (Ass::*f)(Reg1, Reg2, Imm, Imm, Cond),
                                   const std::vector<Reg1*> reg1_registers,
                                   const std::vector<Reg2*> reg2_registers,
                                   std::string (AssemblerArmTest::*GetName1)(const Reg1&),
                                   std::string (AssemblerArmTest::*GetName2)(const Reg2&),
                                   int64_t imm1_min, int64_t imm1_max,
                                   int64_t imm2_min, int64_t imm2_max,
                                   std::string fmt) {
    std::vector<Imm> immediates1;
    FillImmediates(immediates1, imm1_min, imm1_max);
    std::vector<Imm> immediates2;
    FillImmediates(immediates2, imm2_min, imm2_max);

    std::vector<Cond>& cond = GetConditions();

    WarnOnCombinations(cond.size() * immediates1.size() * immediates2.size() *
                       reg1_registers.size() * reg2_registers.size());

    std::ostringstream oss;
    bool first = true;
    for (Cond& c : cond) {
      std::string after_cond = fmt;

      size_t cond_index = after_cond.find(COND_TOKEN);
      if (cond_index != std::string::npos) {
        after_cond.replace(cond_index, ConstexprStrLen(IMM1_TOKEN), GetConditionString(c));
      }

      for (Imm i : immediates1) {
        std::string base = after_cond;

        size_t imm1_index = base.find(IMM1_TOKEN);
        if (imm1_index != std::string::npos) {
          std::ostringstream sreg;
          sreg << i;
          std::string imm_string = sreg.str();
          base.replace(imm1_index, ConstexprStrLen(IMM1_TOKEN), imm_string);
        }

        for (Imm j : immediates2) {
          std::string base2 = base;

          size_t imm2_index = base2.find(IMM2_TOKEN);
          if (imm2_index != std::string::npos) {
            std::ostringstream sreg;
            sreg << j;
            std::string imm_string = sreg.str();
            base2.replace(imm2_index, ConstexprStrLen(IMM2_TOKEN), imm_string);
          }

          for (auto reg1 : reg1_registers) {
            std::string base3 = base2;

            std::string reg1_string = (this->*GetName1)(*reg1);
            size_t reg1_index;
            while ((reg1_index = base3.find(Base::REG1_TOKEN)) != std::string::npos) {
              base3.replace(reg1_index, ConstexprStrLen(Base::REG1_TOKEN), reg1_string);
            }

            for (auto reg2 : reg2_registers) {
              std::string base4 = base3;

              std::string reg2_string = (this->*GetName2)(*reg2);
              size_t reg2_index;
              while ((reg2_index = base4.find(Base::REG2_TOKEN)) != std::string::npos) {
                base4.replace(reg2_index, ConstexprStrLen(Base::REG2_TOKEN), reg2_string);
              }

              if (first) {
                first = false;
              } else {
                oss << "\n";
              }
              oss << base4;

              (Base::GetAssembler()->*f)(*reg1, *reg2, i, j, c);
            }
          }
        }
      }
    }
    // Add a newline at the end.
    oss << "\n";

    return oss.str();
  }

  std::string RepeatRRiiC(void (Ass::*f)(Reg, Reg, Imm, Imm, Cond),
                          std::vector<std::pair<Imm, Imm>>& immediates,
                          std::string fmt) {
    return RepeatTemplatedRRiiC<Reg, Reg>(f, GetRegisters(), GetRegisters(),
        &AssemblerArmTest::template GetRegName<RegisterView::kUsePrimaryName>,
        &AssemblerArmTest::template GetRegName<RegisterView::kUsePrimaryName>,
        immediates, fmt);
  }

  template <typename Reg1, typename Reg2>
  std::string RepeatTemplatedRRiiC(void (Ass::*f)(Reg1, Reg2, Imm, Imm, Cond),
        const std::vector<Reg1*> reg1_registers,
        const std::vector<Reg2*> reg2_registers,
        std::string (AssemblerArmTest::*GetName1)(const Reg1&),
        std::string (AssemblerArmTest::*GetName2)(const Reg2&),
        std::vector<std::pair<Imm, Imm>>& immediates,
        std::string fmt) {
    std::vector<Cond>& cond = GetConditions();

    WarnOnCombinations(cond.size() * immediates.size() * reg1_registers.size() *
                       reg2_registers.size());

    std::ostringstream oss;
    bool first = true;
    for (Cond& c : cond) {
      std::string after_cond = fmt;

      size_t cond_index = after_cond.find(COND_TOKEN);
      if (cond_index != std::string::npos) {
        after_cond.replace(cond_index, ConstexprStrLen(IMM1_TOKEN), GetConditionString(c));
      }

      for (std::pair<Imm, Imm>& pair : immediates) {
        Imm i = pair.first;
        Imm j = pair.second;
        std::string after_imm1 = after_cond;

        size_t imm1_index = after_imm1.find(IMM1_TOKEN);
        if (imm1_index != std::string::npos) {
          std::ostringstream sreg;
          sreg << i;
          std::string imm_string = sreg.str();
          after_imm1.replace(imm1_index, ConstexprStrLen(IMM1_TOKEN), imm_string);
        }

        std::string after_imm2 = after_imm1;

        size_t imm2_index = after_imm2.find(IMM2_TOKEN);
        if (imm2_index != std::string::npos) {
          std::ostringstream sreg;
          sreg << j;
          std::string imm_string = sreg.str();
          after_imm2.replace(imm2_index, ConstexprStrLen(IMM2_TOKEN), imm_string);
        }

        for (auto reg1 : reg1_registers) {
          std::string after_reg1 = after_imm2;

          std::string reg1_string = (this->*GetName1)(*reg1);
          size_t reg1_index;
          while ((reg1_index = after_reg1.find(Base::REG1_TOKEN)) != std::string::npos) {
            after_reg1.replace(reg1_index, ConstexprStrLen(Base::REG1_TOKEN), reg1_string);
          }

          for (auto reg2 : reg2_registers) {
            std::string after_reg2 = after_reg1;

            std::string reg2_string = (this->*GetName2)(*reg2);
            size_t reg2_index;
            while ((reg2_index = after_reg2.find(Base::REG2_TOKEN)) != std::string::npos) {
              after_reg2.replace(reg2_index, ConstexprStrLen(Base::REG2_TOKEN), reg2_string);
            }

            if (first) {
              first = false;
            } else {
              oss << "\n";
            }
            oss << after_reg2;

            (Base::GetAssembler()->*f)(*reg1, *reg2, i, j, c);
          }
        }
      }
    }
    // Add a newline at the end.
    oss << "\n";

    return oss.str();
  }

  std::string RepeatRRC(void (Ass::*f)(Reg, Reg, Cond), std::string fmt) {
    return RepeatTemplatedRRC(f, GetRegisters(), GetRegisters(), GetConditions(),
        &AssemblerArmTest::template GetRegName<RegisterView::kUsePrimaryName>,
        &AssemblerArmTest::template GetRegName<RegisterView::kUsePrimaryName>,
        fmt);
  }

  template <typename Reg1, typename Reg2>
  std::string RepeatTemplatedRRC(void (Ass::*f)(Reg1, Reg2, Cond),
                                 const std::vector<Reg1*>& reg1_registers,
                                 const std::vector<Reg2*>& reg2_registers,
                                 const std::vector<Cond>& cond,
                                 std::string (AssemblerArmTest::*GetName1)(const Reg1&),
                                 std::string (AssemblerArmTest::*GetName2)(const Reg2&),
                                 std::string fmt) {
    WarnOnCombinations(cond.size() * reg1_registers.size() * reg2_registers.size());

    std::ostringstream oss;
    bool first = true;
    for (const Cond& c : cond) {
      std::string after_cond = fmt;

      size_t cond_index = after_cond.find(COND_TOKEN);
      if (cond_index != std::string::npos) {
        after_cond.replace(cond_index, ConstexprStrLen(IMM1_TOKEN), GetConditionString(c));
      }

      for (auto reg1 : reg1_registers) {
        std::string after_reg1 = after_cond;

        std::string reg1_string = (this->*GetName1)(*reg1);
        size_t reg1_index;
        while ((reg1_index = after_reg1.find(Base::REG1_TOKEN)) != std::string::npos) {
          after_reg1.replace(reg1_index, ConstexprStrLen(Base::REG1_TOKEN), reg1_string);
        }

        for (auto reg2 : reg2_registers) {
          std::string after_reg2 = after_reg1;

          std::string reg2_string = (this->*GetName2)(*reg2);
          size_t reg2_index;
          while ((reg2_index = after_reg2.find(Base::REG2_TOKEN)) != std::string::npos) {
            after_reg2.replace(reg2_index, ConstexprStrLen(Base::REG2_TOKEN), reg2_string);
          }

          if (first) {
            first = false;
          } else {
            oss << "\n";
          }
          oss << after_reg2;

          (Base::GetAssembler()->*f)(*reg1, *reg2, c);
        }
      }
    }
    // Add a newline at the end.
    oss << "\n";

    return oss.str();
  }

  std::string RepeatRRRC(void (Ass::*f)(Reg, Reg, Reg, Cond), std::string fmt) {
    return RepeatTemplatedRRRC(f, GetRegisters(), GetRegisters(), GetRegisters(), GetConditions(),
                               &AssemblerArmTest::template GetRegName<RegisterView::kUsePrimaryName>,
                               &AssemblerArmTest::template GetRegName<RegisterView::kUsePrimaryName>,
                               &AssemblerArmTest::template GetRegName<RegisterView::kUsePrimaryName>,
                               fmt);
  }

  template <typename Reg1, typename Reg2, typename Reg3>
  std::string RepeatTemplatedRRRC(void (Ass::*f)(Reg1, Reg2, Reg3, Cond),
                                  const std::vector<Reg1*>& reg1_registers,
                                  const std::vector<Reg2*>& reg2_registers,
                                  const std::vector<Reg3*>& reg3_registers,
                                  const std::vector<Cond>& cond,
                                  std::string (AssemblerArmTest::*GetName1)(const Reg1&),
                                  std::string (AssemblerArmTest::*GetName2)(const Reg2&),
                                  std::string (AssemblerArmTest::*GetName3)(const Reg3&),
                                  std::string fmt) {
    WarnOnCombinations(cond.size() * reg1_registers.size() * reg2_registers.size() *
                       reg3_registers.size());

    std::ostringstream oss;
    bool first = true;
    for (const Cond& c : cond) {
      std::string after_cond = fmt;

      size_t cond_index = after_cond.find(COND_TOKEN);
      if (cond_index != std::string::npos) {
        after_cond.replace(cond_index, ConstexprStrLen(IMM1_TOKEN), GetConditionString(c));
      }

      for (auto reg1 : reg1_registers) {
        std::string after_reg1 = after_cond;

        std::string reg1_string = (this->*GetName1)(*reg1);
        size_t reg1_index;
        while ((reg1_index = after_reg1.find(Base::REG1_TOKEN)) != std::string::npos) {
          after_reg1.replace(reg1_index, ConstexprStrLen(Base::REG1_TOKEN), reg1_string);
        }

        for (auto reg2 : reg2_registers) {
          std::string after_reg2 = after_reg1;

          std::string reg2_string = (this->*GetName2)(*reg2);
          size_t reg2_index;
          while ((reg2_index = after_reg2.find(Base::REG2_TOKEN)) != std::string::npos) {
            after_reg2.replace(reg2_index, ConstexprStrLen(Base::REG2_TOKEN), reg2_string);
          }

          for (auto reg3 : reg3_registers) {
            std::string after_reg3 = after_reg2;

            std::string reg3_string = (this->*GetName3)(*reg3);
            size_t reg3_index;
            while ((reg3_index = after_reg3.find(REG3_TOKEN)) != std::string::npos) {
              after_reg3.replace(reg3_index, ConstexprStrLen(REG3_TOKEN), reg3_string);
            }

            if (first) {
              first = false;
            } else {
              oss << "\n";
            }
            oss << after_reg3;

            (Base::GetAssembler()->*f)(*reg1, *reg2, *reg3, c);
          }
        }
      }
    }
    // Add a newline at the end.
    oss << "\n";

    return oss.str();
  }

  template <typename RegT>
  std::string RepeatTemplatedRSC(void (Ass::*f)(RegT, SOp, Cond),
                                 const std::vector<RegT*>& registers,
                                 const std::vector<SOp>& shifts,
                                 const std::vector<Cond>& cond,
                                 std::string (AssemblerArmTest::*GetName)(const RegT&),
                                 std::string fmt) {
    WarnOnCombinations(cond.size() * registers.size() * shifts.size());

    std::ostringstream oss;
    bool first = true;
    for (const Cond& c : cond) {
      std::string after_cond = fmt;

      size_t cond_index = after_cond.find(COND_TOKEN);
      if (cond_index != std::string::npos) {
        after_cond.replace(cond_index, ConstexprStrLen(IMM1_TOKEN), GetConditionString(c));
      }

      for (const SOp& shift : shifts) {
        std::string after_shift = after_cond;

        std::string shift_string = GetShiftString(shift);
        size_t shift_index;
        while ((shift_index = after_shift.find(Base::SHIFT_TOKEN)) != std::string::npos) {
          after_shift.replace(shift_index, ConstexprStrLen(Base::SHIFT_TOKEN), shift_string);
        }

        for (auto reg : registers) {
          std::string after_reg = after_shift;

          std::string reg_string = (this->*GetName)(*reg);
          size_t reg_index;
          while ((reg_index = after_reg.find(Base::REG_TOKEN)) != std::string::npos) {
            after_reg.replace(reg_index, ConstexprStrLen(Base::REG_TOKEN), reg_string);
          }

          if (first) {
            first = false;
          } else {
            oss << "\n";
          }
          oss << after_reg;

          (Base::GetAssembler()->*f)(*reg, shift, c);
        }
      }
    }
    // Add a newline at the end.
    oss << "\n";

    return oss.str();
  }

  template <typename Reg1, typename Reg2>
  std::string RepeatTemplatedRRSC(void (Ass::*f)(Reg1, Reg2, const SOp&, Cond),
                                  const std::vector<Reg1*>& reg1_registers,
                                  const std::vector<Reg2*>& reg2_registers,
                                  const std::vector<SOp>& shifts,
                                  const std::vector<Cond>& cond,
                                  std::string (AssemblerArmTest::*GetName1)(const Reg1&),
                                  std::string (AssemblerArmTest::*GetName2)(const Reg2&),
                                  std::string fmt) {
    WarnOnCombinations(cond.size() * reg1_registers.size() * reg2_registers.size() * shifts.size());

    std::ostringstream oss;
    bool first = true;
    for (const Cond& c : cond) {
      std::string after_cond = fmt;

      size_t cond_index = after_cond.find(COND_TOKEN);
      if (cond_index != std::string::npos) {
        after_cond.replace(cond_index, ConstexprStrLen(IMM1_TOKEN), GetConditionString(c));
      }

      for (const SOp& shift : shifts) {
        std::string after_shift = after_cond;

        std::string shift_string = GetShiftString(shift);
        size_t shift_index;
        while ((shift_index = after_shift.find(SHIFT_TOKEN)) != std::string::npos) {
          after_shift.replace(shift_index, ConstexprStrLen(SHIFT_TOKEN), shift_string);
        }

        for (auto reg1 : reg1_registers) {
          std::string after_reg1 = after_shift;

          std::string reg1_string = (this->*GetName1)(*reg1);
          size_t reg1_index;
          while ((reg1_index = after_reg1.find(Base::REG1_TOKEN)) != std::string::npos) {
            after_reg1.replace(reg1_index, ConstexprStrLen(Base::REG1_TOKEN), reg1_string);
          }

          for (auto reg2 : reg2_registers) {
            std::string after_reg2 = after_reg1;

            std::string reg2_string = (this->*GetName2)(*reg2);
            size_t reg2_index;
            while ((reg2_index = after_reg2.find(Base::REG2_TOKEN)) != std::string::npos) {
              after_reg2.replace(reg2_index, ConstexprStrLen(Base::REG2_TOKEN), reg2_string);
            }

            if (first) {
              first = false;
            } else {
              oss << "\n";
            }
            oss << after_reg2;

            (Base::GetAssembler()->*f)(*reg1, *reg2, shift, c);
          }
        }
      }
    }
    // Add a newline at the end.
    oss << "\n";

    return oss.str();
  }

 protected:
  explicit AssemblerArmTest() {}

  virtual std::vector<Cond>& GetConditions() = 0;
  virtual std::string GetConditionString(Cond c) = 0;

  virtual std::vector<SOp>& GetShiftOperands() = 0;
  virtual std::string GetShiftString(SOp sop) = 0;

  virtual Reg GetPCRegister() = 0;
  virtual std::vector<Reg*> GetRegistersWithoutPC() {
    std::vector<Reg*> without_pc = GetRegisters();
    Reg pc_reg = GetPCRegister();

    for (auto it = without_pc.begin(); it != without_pc.end(); ++it) {
      if (**it == pc_reg) {
        without_pc.erase(it);
        break;
      }
    }

    return without_pc;
  }

  static constexpr const char* IMM1_TOKEN = "{imm1}";
  static constexpr const char* IMM2_TOKEN = "{imm2}";
  static constexpr const char* REG3_TOKEN = "{reg3}";
  static constexpr const char* REG4_TOKEN = "{reg4}";
  static constexpr const char* COND_TOKEN = "{cond}";
  static constexpr const char* SHIFT_TOKEN = "{shift}";

 private:
  DISALLOW_COPY_AND_ASSIGN(AssemblerArmTest);
};

}  // namespace art

#endif  // ART_COMPILER_UTILS_ARM_ASSEMBLER_ARM_TEST_H_
