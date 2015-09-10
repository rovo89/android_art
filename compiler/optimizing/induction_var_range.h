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
 * This class implements induction variable based range analysis on expressions within loops.
 * It takes the results of induction variable analysis in the constructor and provides a public
 * API to obtain a conservative lower and upper bound value on each instruction in the HIR.
 *
 * For example, given a linear induction 2 * i + x where 0 <= i <= 10, range analysis yields lower
 * bound value x and upper bound value x + 20 for the expression, thus, the range [x, x + 20].
 */
class InductionVarRange {
 public:
  /*
   * A value that can be represented as "a * instruction + b" for 32-bit constants, where
   * Value(INT_MIN) and Value(INT_MAX) denote an unknown lower and upper bound, respectively.
   * Although range analysis could yield more complex values, the format is sufficiently powerful
   * to represent useful cases and feeds directly into optimizations like bounds check elimination.
   */
  struct Value {
    Value(HInstruction* i, int32_t a, int32_t b)
        : instruction(a != 0 ? i : nullptr),
          a_constant(a),
          b_constant(b) {}
    explicit Value(int32_t b) : Value(nullptr, 0, b) {}
    HInstruction* instruction;
    int32_t a_constant;
    int32_t b_constant;
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

  HInductionVarAnalysis::InductionInfo* GetTripCount(HLoopInformation* loop,
                                                     HInstruction* context);

  static Value GetFetch(HInstruction* instruction,
                        HInductionVarAnalysis::InductionInfo* trip,
                        int32_t fail_value);

  static Value GetMin(HInductionVarAnalysis::InductionInfo* info,
                      HInductionVarAnalysis::InductionInfo* trip);
  static Value GetMax(HInductionVarAnalysis::InductionInfo* info,
                      HInductionVarAnalysis::InductionInfo* trip);
  static Value GetMul(HInductionVarAnalysis::InductionInfo* info1,
                      HInductionVarAnalysis::InductionInfo* info2,
                      HInductionVarAnalysis::InductionInfo* trip,
                      int32_t fail_value);
  static Value GetDiv(HInductionVarAnalysis::InductionInfo* info1,
                      HInductionVarAnalysis::InductionInfo* info2,
                      HInductionVarAnalysis::InductionInfo* trip,
                      int32_t fail_value);

  static Value AddValue(Value v1, Value v2, int32_t fail_value);
  static Value SubValue(Value v1, Value v2, int32_t fail_value);
  static Value MulValue(Value v1, Value v2, int32_t fail_value);
  static Value DivValue(Value v1, Value v2, int32_t fail_value);
  static Value MinValue(Value v1, Value v2);
  static Value MaxValue(Value v1, Value v2);

  /** Results of prior induction variable analysis. */
  HInductionVarAnalysis *induction_analysis_;

  friend class HInductionVarAnalysis;
  friend class InductionVarRangeTest;

  DISALLOW_COPY_AND_ASSIGN(InductionVarRange);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_INDUCTION_VAR_RANGE_H_
