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

#ifndef ART_COMPILER_OPTIMIZING_INLINER_H_
#define ART_COMPILER_OPTIMIZING_INLINER_H_

#include "invoke_type.h"
#include "optimization.h"

namespace art {

class CompilerDriver;
class DexCompilationUnit;
class HGraph;
class HInvoke;
class OptimizingCompilerStats;

class HInliner : public HOptimization {
 public:
  HInliner(HGraph* outer_graph,
           const DexCompilationUnit& outer_compilation_unit,
           CompilerDriver* compiler_driver,
           OptimizingCompilerStats* stats)
      : HOptimization(outer_graph, true, "inliner"),
        outer_compilation_unit_(outer_compilation_unit),
        compiler_driver_(compiler_driver),
        outer_stats_(stats) {}

  void Run() OVERRIDE;

 private:
  bool TryInline(HInvoke* invoke_instruction, uint32_t method_index, InvokeType invoke_type) const;

  const DexCompilationUnit& outer_compilation_unit_;
  CompilerDriver* const compiler_driver_;
  OptimizingCompilerStats* const outer_stats_;

  DISALLOW_COPY_AND_ASSIGN(HInliner);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_INLINER_H_
