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

#ifndef ART_COMPILER_OPTIMIZING_SSA_BUILDER_H_
#define ART_COMPILER_OPTIMIZING_SSA_BUILDER_H_

#include "base/arena_containers.h"
#include "nodes.h"
#include "optimization.h"

namespace art {

static constexpr int kDefaultNumberOfLoops = 2;

/**
 * Transforms a graph into SSA form. The liveness guarantees of
 * this transformation are listed below. A DEX register
 * being killed means its value at a given position in the code
 * will not be available to its environment uses. A merge in the
 * following text is materialized as a `HPhi`.
 *
 * (a) Dex registers that do not require merging (that is, they do not
 *     have different values at a join block) are available to all their
 *     environment uses. Note that it does not imply the instruction will
 *     have a physical location after register allocation. See the
 *     SsaLivenessAnalysis phase.
 *
 * (b) Dex registers that require merging, and the merging gives
 *     incompatible types, will be killed for environment uses of that merge.
 *
 * (c) When the `debuggable` flag is passed to the compiler, Dex registers
 *     that require merging and have a proper type after the merge, are
 *     available to all their environment uses. If the `debuggable` flag
 *     is not set, values of Dex registers only used by environments
 *     are killed.
 */
class SsaBuilder : public HGraphVisitor {
 public:
  SsaBuilder(HGraph* graph, StackHandleScopeCollection* handles)
      : HGraphVisitor(graph),
        handles_(handles),
        agets_fixed_(false),
        current_locals_(nullptr),
        loop_headers_(graph->GetArena()->Adapter(kArenaAllocSsaBuilder)),
        ambiguous_agets_(graph->GetArena()->Adapter(kArenaAllocSsaBuilder)),
        ambiguous_asets_(graph->GetArena()->Adapter(kArenaAllocSsaBuilder)),
        uninitialized_strings_(graph->GetArena()->Adapter(kArenaAllocSsaBuilder)),
        locals_for_(graph->GetBlocks().size(),
                    ArenaVector<HInstruction*>(graph->GetArena()->Adapter(kArenaAllocSsaBuilder)),
                    graph->GetArena()->Adapter(kArenaAllocSsaBuilder)) {
    loop_headers_.reserve(kDefaultNumberOfLoops);
  }

  GraphAnalysisResult BuildSsa();

  // Returns locals vector for `block`. If it is a catch block, the vector will be
  // prepopulated with catch phis for vregs which are defined in `current_locals_`.
  ArenaVector<HInstruction*>* GetLocalsFor(HBasicBlock* block);
  HInstruction* ValueOfLocal(HBasicBlock* block, size_t local);

  void VisitBasicBlock(HBasicBlock* block) OVERRIDE;
  void VisitLoadLocal(HLoadLocal* load) OVERRIDE;
  void VisitStoreLocal(HStoreLocal* store) OVERRIDE;
  void VisitInstruction(HInstruction* instruction) OVERRIDE;
  void VisitArrayGet(HArrayGet* aget) OVERRIDE;
  void VisitArraySet(HArraySet* aset) OVERRIDE;
  void VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) OVERRIDE;

 private:
  void SetLoopHeaderPhiInputs();
  void FixEnvironmentPhis();
  void FixNullConstantType();
  void EquivalentPhisCleanup();
  void RunPrimitiveTypePropagation();

  // Attempts to resolve types of aget(-wide) instructions and type values passed
  // to aput(-wide) instructions from reference type information on the array
  // input. Returns false if the type of an array is unknown.
  bool FixAmbiguousArrayOps();

  bool TypeInputsOfPhi(HPhi* phi, ArenaVector<HPhi*>* worklist);
  bool UpdatePrimitiveType(HPhi* phi, ArenaVector<HPhi*>* worklist);
  void ProcessPrimitiveTypePropagationWorklist(ArenaVector<HPhi*>* worklist);

  HInstruction* GetFloatOrDoubleEquivalent(HInstruction* instruction, Primitive::Type type);
  HInstruction* GetReferenceTypeEquivalent(HInstruction* instruction);

  HFloatConstant* GetFloatEquivalent(HIntConstant* constant);
  HDoubleConstant* GetDoubleEquivalent(HLongConstant* constant);
  HPhi* GetFloatDoubleOrReferenceEquivalentOfPhi(HPhi* phi, Primitive::Type type);
  HArrayGet* GetFloatOrDoubleEquivalentOfArrayGet(HArrayGet* aget);

  void RemoveRedundantUninitializedStrings();

  StackHandleScopeCollection* const handles_;

  // True if types of ambiguous ArrayGets have been resolved.
  bool agets_fixed_;

  // Locals for the current block being visited.
  ArenaVector<HInstruction*>* current_locals_;

  // Keep track of loop headers found. The last phase of the analysis iterates
  // over these blocks to set the inputs of their phis.
  ArenaVector<HBasicBlock*> loop_headers_;

  ArenaVector<HArrayGet*> ambiguous_agets_;
  ArenaVector<HArraySet*> ambiguous_asets_;
  ArenaVector<HNewInstance*> uninitialized_strings_;

  // HEnvironment for each block.
  ArenaVector<ArenaVector<HInstruction*>> locals_for_;

  DISALLOW_COPY_AND_ASSIGN(SsaBuilder);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_SSA_BUILDER_H_
