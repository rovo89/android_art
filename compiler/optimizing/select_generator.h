/*
 * Copyright (C) 2016 The Android Open Source Project
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

/*
 * This optimization recognizes the common diamond selection pattern and
 * replaces it with an instance of the HSelect instruction.
 *
 * Recognized pattern:
 *
 *          If [ Condition ]
 *            /          \
 *      false branch  true branch
 *            \          /
 *     Phi [FalseValue, TrueValue]
 *
 * The pattern will be simplified if `true_branch` and `false_branch` each
 * contain at most one instruction without any side effects.
 *
 * Blocks are merged into one and Select replaces the If and the Phi:
 *              true branch
 *              false branch
 *              Select [FalseValue, TrueValue, Condition]
 *
 * Note: In order to recognize no side-effect blocks, this optimization must be
 * run after the instruction simplifier has removed redundant suspend checks.
 */

#ifndef ART_COMPILER_OPTIMIZING_SELECT_GENERATOR_H_
#define ART_COMPILER_OPTIMIZING_SELECT_GENERATOR_H_

#include "optimization.h"

namespace art {

class HSelectGenerator : public HOptimization {
 public:
  HSelectGenerator(HGraph* graph, OptimizingCompilerStats* stats)
    : HOptimization(graph, kSelectGeneratorPassName, stats) {}

  void Run() OVERRIDE;

  static constexpr const char* kSelectGeneratorPassName = "select_generator";

 private:
  DISALLOW_COPY_AND_ASSIGN(HSelectGenerator);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_SELECT_GENERATOR_H_
