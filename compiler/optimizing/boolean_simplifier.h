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

// This optimization recognizes two common patterns:
//  (a) Boolean selection: Casting a boolean to an integer or negating it is
//      carried out with an If statement selecting from zero/one integer
//      constants. Because Boolean values are represented as zero/one, the
//      pattern can be replaced with the condition instruction itself or its
//      negation, depending on the layout.
//  (b) Negated condition: Instruction simplifier may replace an If's condition
//      with a boolean value. If this value is the result of a Boolean negation,
//      the true/false branches can be swapped and negation removed.

// Example: Negating a boolean value
//     B1:
//       z1   ParameterValue
//       i2   IntConstant 0
//       i3   IntConstant 1
//       v4   Goto B2
//     B2:
//       z5   NotEquals [ z1 i2 ]
//       v6   If [ z5 ] then B3 else B4
//     B3:
//       v7   Goto B5
//     B4:
//       v8   Goto B5
//     B5:
//       i9   Phi [ i3 i2 ]
//       v10  Return [ i9 ]
// turns into
//     B1:
//       z1   ParameterValue
//       i2   IntConstant 0
//       v4   Goto B2
//     B2:
//       z11  Equals [ z1 i2 ]
//       v10  Return [ z11 ]
//     B3, B4, B5: removed

// Note: in order to recognize empty blocks, this optimization must be run
// after the instruction simplifier has removed redundant suspend checks.

#ifndef ART_COMPILER_OPTIMIZING_BOOLEAN_SIMPLIFIER_H_
#define ART_COMPILER_OPTIMIZING_BOOLEAN_SIMPLIFIER_H_

#include "optimization.h"

namespace art {

class HBooleanSimplifier : public HOptimization {
 public:
  explicit HBooleanSimplifier(HGraph* graph)
    : HOptimization(graph, true, kBooleanSimplifierPassName) {}

  void Run() OVERRIDE;

  static constexpr const char* kBooleanSimplifierPassName = "boolean_simplifier";

 private:
  void TryRemovingNegatedCondition(HBasicBlock* block);
  void TryRemovingBooleanSelection(HBasicBlock* block);

  DISALLOW_COPY_AND_ASSIGN(HBooleanSimplifier);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_BOOLEAN_SIMPLIFIER_H_
