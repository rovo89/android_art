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

class CodeGenerator;
class CompilerDriver;
class DexCompilationUnit;
class HGraph;
class HInvoke;
class InlineCache;
class OptimizingCompilerStats;

class HInliner : public HOptimization {
 public:
  HInliner(HGraph* outer_graph,
           HGraph* outermost_graph,
           CodeGenerator* codegen,
           const DexCompilationUnit& outer_compilation_unit,
           const DexCompilationUnit& caller_compilation_unit,
           CompilerDriver* compiler_driver,
           StackHandleScopeCollection* handles,
           OptimizingCompilerStats* stats,
           size_t total_number_of_dex_registers,
           size_t depth)
      : HOptimization(outer_graph, kInlinerPassName, stats),
        outermost_graph_(outermost_graph),
        outer_compilation_unit_(outer_compilation_unit),
        caller_compilation_unit_(caller_compilation_unit),
        codegen_(codegen),
        compiler_driver_(compiler_driver),
        total_number_of_dex_registers_(total_number_of_dex_registers),
        depth_(depth),
        number_of_inlined_instructions_(0),
        handles_(handles) {}

  void Run() OVERRIDE;

  static constexpr const char* kInlinerPassName = "inliner";

 private:
  bool TryInline(HInvoke* invoke_instruction);

  // Try to inline `resolved_method` in place of `invoke_instruction`. `do_rtp` is whether
  // reference type propagation can run after the inlining.
  bool TryInline(HInvoke* invoke_instruction, ArtMethod* resolved_method, bool do_rtp = true)
    SHARED_REQUIRES(Locks::mutator_lock_);

  // Try to inline the target of a monomorphic call. If successful, the code
  // in the graph will look like:
  // if (receiver.getClass() != ic.GetMonomorphicType()) deopt
  // ... // inlined code
  bool TryInlineMonomorphicCall(HInvoke* invoke_instruction,
                                ArtMethod* resolved_method,
                                const InlineCache& ic)
    SHARED_REQUIRES(Locks::mutator_lock_);

  // Try to inline targets of a polymorphic call. Currently unimplemented.
  bool TryInlinePolymorphicCall(HInvoke* invoke_instruction,
                                ArtMethod* resolved_method,
                                const InlineCache& ic)
    SHARED_REQUIRES(Locks::mutator_lock_);

  bool TryBuildAndInline(ArtMethod* resolved_method,
                         HInvoke* invoke_instruction,
                         bool same_dex_file,
                         bool do_rtp = true);

  HInstanceFieldGet* BuildGetReceiverClass(ClassLinker* class_linker,
                                           HInstruction* receiver,
                                           uint32_t dex_pc) const
    SHARED_REQUIRES(Locks::mutator_lock_);

  HGraph* const outermost_graph_;
  const DexCompilationUnit& outer_compilation_unit_;
  const DexCompilationUnit& caller_compilation_unit_;
  CodeGenerator* const codegen_;
  CompilerDriver* const compiler_driver_;
  const size_t total_number_of_dex_registers_;
  const size_t depth_;
  size_t number_of_inlined_instructions_;
  StackHandleScopeCollection* const handles_;

  DISALLOW_COPY_AND_ASSIGN(HInliner);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_INLINER_H_
