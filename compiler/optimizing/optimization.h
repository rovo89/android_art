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

#ifndef ART_COMPILER_OPTIMIZING_OPTIMIZATION_H_
#define ART_COMPILER_OPTIMIZING_OPTIMIZATION_H_

#include "base/arena_object.h"
#include "nodes.h"
#include "optimizing_compiler_stats.h"

namespace art {

/**
 * Abstraction to implement an optimization pass.
 */
class HOptimization : public ArenaObject<kArenaAllocMisc> {
 public:
  HOptimization(HGraph* graph,
                bool is_in_ssa_form,
                const char* pass_name,
                OptimizingCompilerStats* stats = nullptr)
      : graph_(graph),
        stats_(stats),
        is_in_ssa_form_(is_in_ssa_form),
        pass_name_(pass_name) {}

  virtual ~HOptimization() {}

  // Return the name of the pass.
  const char* GetPassName() const { return pass_name_; }

  // Peform the analysis itself.
  virtual void Run() = 0;

  // Verify the graph; abort if it is not valid.
  void Check();

 protected:
  void MaybeRecordStat(MethodCompilationStat compilation_stat, size_t count = 1) const;

  HGraph* const graph_;
  // Used to record stats about the optimization.
  OptimizingCompilerStats* const stats_;

 private:
  // Does the analyzed graph use the SSA form?
  const bool is_in_ssa_form_;
  // Optimization pass name.
  const char* pass_name_;

  DISALLOW_COPY_AND_ASSIGN(HOptimization);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_OPTIMIZATION_H_
