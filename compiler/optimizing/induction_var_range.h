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

#ifndef ART_COMPILER_OPTIMIZING_INDUCTION_VAR_RANGE_H_
#define ART_COMPILER_OPTIMIZING_INDUCTION_VAR_RANGE_H_

#include "induction_var_analysis.h"

namespace art {

/**
 * This class implements range analysis on expressions within loops. It takes the results
 * of induction variable analysis in the constructor and provides a public API to obtain
 * a conservative lower and upper bound value on each instruction in the HIR.
 *
 * The range analysis is done with a combination of symbolic and partial integral evaluation
 * of expressions. The analysis avoids complications with wrap-around arithmetic on the integral
 * parts but all clients should be aware that wrap-around may occur on any of the symbolic parts.
 * For example, given a known range for [0,100] for i, the evaluation yields range [-100,100]
 * for expression -2*i+100, which is exact, and range [x,x+100] for expression i+x, which may
 * wrap-around anywhere in the range depending on the actual value of x.
 */
class InductionVarRange {
 public:
  /*
   * A value that can be represented as "a * instruction + b" for 32-bit constants, where
   * Value() denotes an unknown lower and upper bound. Although range analysis could yield
   * more complex values, the format is sufficiently powerful to represent useful cases
   * and feeds directly into optimizations like bounds check elimination.
   */
  struct Value {
    Value() : instruction(nullptr), a_constant(0), b_constant(0), is_known(false) {}
    Value(HInstruction* i, int32_t a, int32_t b)
        : instruction(a != 0 ? i : nullptr), a_constant(a), b_constant(b), is_known(true) {}
    explicit Value(int32_t b) : Value(nullptr, 0, b) {}
    // Representation as: a_constant x instruction + b_constant.
    HInstruction* instruction;
    int32_t a_constant;
    int32_t b_constant;
    // If true, represented by prior fields. Otherwise unknown value.
    bool is_known;
  };

  explicit InductionVarRange(HInductionVarAnalysis* induction);

  /**
   * Given a context denoted by the first instruction, returns a,
   * possibly conservative, lower bound on the instruction's value.
   */
  Value GetMinInduction(HInstruction* context, HInstruction* instruction);

  /**
   * Given a context denoted by the first instruction, returns a,
   * possibly conservative, upper bound on the instruction's value.
   */
  Value GetMaxInduction(HInstruction* context, HInstruction* instruction);

 private:
  //
  // Private helper methods.
  //

  Value GetInduction(HInstruction* context, HInstruction* instruction, bool is_min);

  static Value GetFetch(HInstruction* instruction,
                        HInductionVarAnalysis::InductionInfo* trip,
                        bool in_body,
                        bool is_min);
  static Value GetVal(HInductionVarAnalysis::InductionInfo* info,
                      HInductionVarAnalysis::InductionInfo* trip,
                      bool in_body,
                      bool is_min);
  static Value GetMul(HInductionVarAnalysis::InductionInfo* info1,
                      HInductionVarAnalysis::InductionInfo* info2,
                      HInductionVarAnalysis::InductionInfo* trip,
                      bool in_body,
                      bool is_min);
  static Value GetDiv(HInductionVarAnalysis::InductionInfo* info1,
                      HInductionVarAnalysis::InductionInfo* info2,
                      HInductionVarAnalysis::InductionInfo* trip,
                      bool in_body,
                      bool is_min);

  static bool GetConstant(HInductionVarAnalysis::InductionInfo* info, int32_t *value);

  static Value AddValue(Value v1, Value v2);
  static Value SubValue(Value v1, Value v2);
  static Value MulValue(Value v1, Value v2);
  static Value DivValue(Value v1, Value v2);
  static Value MergeVal(Value v1, Value v2, bool is_min);

  /** Results of prior induction variable analysis. */
  HInductionVarAnalysis *induction_analysis_;

  friend class HInductionVarAnalysis;
  friend class InductionVarRangeTest;

  DISALLOW_COPY_AND_ASSIGN(InductionVarRange);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_INDUCTION_VAR_RANGE_H_
