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

#include "builder.h"

#include "art_field-inl.h"
#include "base/arena_bit_vector.h"
#include "base/bit_vector-inl.h"
#include "base/logging.h"
#include "dex/verified_method.h"
#include "driver/compiler_options.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache.h"
#include "nodes.h"
#include "primitive.h"
#include "thread.h"
#include "utils/dex_cache_arrays_layout-inl.h"

namespace art {

void HGraphBuilder::MaybeRecordStat(MethodCompilationStat compilation_stat) {
  if (compilation_stats_ != nullptr) {
    compilation_stats_->RecordStat(compilation_stat);
  }
}

bool HGraphBuilder::SkipCompilation(size_t number_of_branches) {
  if (compiler_driver_ == nullptr) {
    // Note that the compiler driver is null when unit testing.
    return false;
  }

  const CompilerOptions& compiler_options = compiler_driver_->GetCompilerOptions();
  CompilerFilter::Filter compiler_filter = compiler_options.GetCompilerFilter();
  if (compiler_filter == CompilerFilter::kEverything) {
    return false;
  }

  if (compiler_options.IsHugeMethod(code_item_.insns_size_in_code_units_)) {
    VLOG(compiler) << "Skip compilation of huge method "
                   << PrettyMethod(dex_compilation_unit_->GetDexMethodIndex(), *dex_file_)
                   << ": " << code_item_.insns_size_in_code_units_ << " code units";
    MaybeRecordStat(MethodCompilationStat::kNotCompiledHugeMethod);
    return true;
  }

  // If it's large and contains no branches, it's likely to be machine generated initialization.
  if (compiler_options.IsLargeMethod(code_item_.insns_size_in_code_units_)
      && (number_of_branches == 0)) {
    VLOG(compiler) << "Skip compilation of large method with no branch "
                   << PrettyMethod(dex_compilation_unit_->GetDexMethodIndex(), *dex_file_)
                   << ": " << code_item_.insns_size_in_code_units_ << " code units";
    MaybeRecordStat(MethodCompilationStat::kNotCompiledLargeMethodNoBranches);
    return true;
  }

  return false;
}

GraphAnalysisResult HGraphBuilder::BuildGraph() {
  DCHECK(graph_->GetBlocks().empty());

  graph_->SetNumberOfVRegs(code_item_.registers_size_);
  graph_->SetNumberOfInVRegs(code_item_.ins_size_);
  graph_->SetMaximumNumberOfOutVRegs(code_item_.outs_size_);
  graph_->SetHasTryCatch(code_item_.tries_size_ != 0);

  // 1) Create basic blocks and link them together. Basic blocks are left
  //    unpopulated with the exception of synthetic blocks, e.g. HTryBoundaries.
  if (!block_builder_.Build()) {
    return kAnalysisInvalidBytecode;
  }

  // 2) Decide whether to skip this method based on its code size and number
  //    of branches.
  if (SkipCompilation(block_builder_.GetNumberOfBranches())) {
    return kAnalysisSkipped;
  }

  // 3) Build the dominator tree and fill in loop and try/catch metadata.
  GraphAnalysisResult result = graph_->BuildDominatorTree();
  if (result != kAnalysisSuccess) {
    return result;
  }

  // 4) Populate basic blocks with instructions.
  if (!instruction_builder_.Build()) {
    return kAnalysisInvalidBytecode;
  }

  // 5) Type the graph and eliminate dead/redundant phis.
  return ssa_builder_.BuildSsa();
}

}  // namespace art
