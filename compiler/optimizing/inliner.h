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
  // reference type propagation can run after the inlining. If the inlining is successful, this
  // method will replace and remove the `invoke_instruction`.
  bool TryInlineAndReplace(HInvoke* invoke_instruction, ArtMethod* resolved_method, bool do_rtp)
    SHARED_REQUIRES(Locks::mutator_lock_);

  bool TryBuildAndInline(HInvoke* invoke_instruction,
                         ArtMethod* resolved_method,
                         HInstruction** return_replacement)
    SHARED_REQUIRES(Locks::mutator_lock_);

  bool TryBuildAndInlineHelper(HInvoke* invoke_instruction,
                               ArtMethod* resolved_method,
                               bool same_dex_file,
                               HInstruction** return_replacement);

  // Run simple optimizations on `callee_graph`.
  // Returns the number of inlined instructions.
  size_t RunOptimizations(HGraph* callee_graph,
                          const DexFile::CodeItem* code_item,
                          const DexCompilationUnit& dex_compilation_unit);

  // Try to recognize known simple patterns and replace invoke call with appropriate instructions.
  bool TryPatternSubstitution(HInvoke* invoke_instruction,
                              ArtMethod* resolved_method,
                              HInstruction** return_replacement)
    SHARED_REQUIRES(Locks::mutator_lock_);

  // Create a new HInstanceFieldGet.
  HInstanceFieldGet* CreateInstanceFieldGet(Handle<mirror::DexCache> dex_cache,
                                            uint32_t field_index,
                                            HInstruction* obj);
  // Create a new HInstanceFieldSet.
  HInstanceFieldSet* CreateInstanceFieldSet(Handle<mirror::DexCache> dex_cache,
                                            uint32_t field_index,
                                            HInstruction* obj,
                                            HInstruction* value);

  // Try to inline the target of a monomorphic call. If successful, the code
  // in the graph will look like:
  // if (receiver.getClass() != ic.GetMonomorphicType()) deopt
  // ... // inlined code
  bool TryInlineMonomorphicCall(HInvoke* invoke_instruction,
                                ArtMethod* resolved_method,
                                const InlineCache& ic)
    SHARED_REQUIRES(Locks::mutator_lock_);

  // Try to inline targets of a polymorphic call.
  bool TryInlinePolymorphicCall(HInvoke* invoke_instruction,
                                ArtMethod* resolved_method,
                                const InlineCache& ic)
    SHARED_REQUIRES(Locks::mutator_lock_);

  bool TryInlinePolymorphicCallToSameTarget(HInvoke* invoke_instruction,
                                            ArtMethod* resolved_method,
                                            const InlineCache& ic)
    SHARED_REQUIRES(Locks::mutator_lock_);


  HInstanceFieldGet* BuildGetReceiverClass(ClassLinker* class_linker,
                                           HInstruction* receiver,
                                           uint32_t dex_pc) const
    SHARED_REQUIRES(Locks::mutator_lock_);

  void FixUpReturnReferenceType(HInvoke* invoke_instruction,
                                ArtMethod* resolved_method,
                                HInstruction* return_replacement,
                                bool do_rtp)
    SHARED_REQUIRES(Locks::mutator_lock_);

  // Add a type guard on the given `receiver`. This will add to the graph:
  // i0 = HFieldGet(receiver, klass)
  // i1 = HLoadClass(class_index, is_referrer)
  // i2 = HNotEqual(i0, i1)
  //
  // And if `with_deoptimization` is true:
  // HDeoptimize(i2)
  //
  // The method returns the `HNotEqual`, that will be used for polymorphic inlining.
  HInstruction* AddTypeGuard(HInstruction* receiver,
                             HInstruction* cursor,
                             HBasicBlock* bb_cursor,
                             uint32_t class_index,
                             bool is_referrer,
                             HInstruction* invoke_instruction,
                             bool with_deoptimization)
    SHARED_REQUIRES(Locks::mutator_lock_);

  /*
   * Ad-hoc implementation for implementing a diamond pattern in the graph for
   * polymorphic inlining:
   * 1) `compare` becomes the input of the new `HIf`.
   * 2) Everything up until `invoke_instruction` is in the then branch (could
   *    contain multiple blocks).
   * 3) `invoke_instruction` is moved to the otherwise block.
   * 4) If `return_replacement` is not null, the merge block will have
   *    a phi whose inputs are `return_replacement` and `invoke_instruction`.
   *
   * Before:
   *             Block1
   *             compare
   *              ...
   *         invoke_instruction
   *
   * After:
   *            Block1
   *            compare
   *              if
   *          /        \
   *         /          \
   *   Then block    Otherwise block
   *      ...       invoke_instruction
   *       \              /
   *        \            /
   *          Merge block
   *  phi(return_replacement, invoke_instruction)
   */
  void CreateDiamondPatternForPolymorphicInline(HInstruction* compare,
                                                HInstruction* return_replacement,
                                                HInstruction* invoke_instruction);

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
