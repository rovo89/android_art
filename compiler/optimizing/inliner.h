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
           const DexCompilationUnit& caller_compilation_unit,
           CompilerDriver* compiler_driver,
           OptimizingCompilerStats* stats,
           size_t depth = 0)
      : HOptimization(outer_graph, true, kInlinerPassName, stats),
        outer_compilation_unit_(outer_compilation_unit),
        caller_compilation_unit_(caller_compilation_unit),
        compiler_driver_(compiler_driver),
        depth_(depth) {}

  void Run() OVERRIDE;

  static constexpr const char* kInlinerPassName = "inliner";

 private:
  bool TryInline(HInvoke* invoke_instruction, uint32_t method_index) const;
  bool TryBuildAndInline(ArtMethod* resolved_method,
                         HInvoke* invoke_instruction,
                         uint32_t method_index,
                         bool can_use_dex_cache) const;

  const DexCompilationUnit& outer_compilation_unit_;
  const DexCompilationUnit& caller_compilation_unit_;
  CompilerDriver* const compiler_driver_;
  const size_t depth_;

  DISALLOW_COPY_AND_ASSIGN(HInliner);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_INLINER_H_
