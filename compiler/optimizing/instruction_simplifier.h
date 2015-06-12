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

#ifndef ART_COMPILER_OPTIMIZING_INSTRUCTION_SIMPLIFIER_H_
#define ART_COMPILER_OPTIMIZING_INSTRUCTION_SIMPLIFIER_H_

#include "nodes.h"
#include "optimization.h"
#include "optimizing_compiler_stats.h"

namespace art {

/**
 * Implements optimizations specific to each instruction.
 */
class InstructionSimplifier : public HOptimization {
 public:
  InstructionSimplifier(HGraph* graph,
                        OptimizingCompilerStats* stats = nullptr,
                        const char* name = kInstructionSimplifierPassName)
    : HOptimization(graph, true, name, stats) {}

  static constexpr const char* kInstructionSimplifierPassName = "instruction_simplifier";

  void Run() OVERRIDE;

 private:
  DISALLOW_COPY_AND_ASSIGN(InstructionSimplifier);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_INSTRUCTION_SIMPLIFIER_H_
