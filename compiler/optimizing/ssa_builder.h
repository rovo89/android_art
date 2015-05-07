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
  explicit SsaBuilder(HGraph* graph)
      : HGraphVisitor(graph),
        current_locals_(nullptr),
        loop_headers_(graph->GetArena(), kDefaultNumberOfLoops),
        locals_for_(graph->GetArena(), graph->GetBlocks().Size()) {
    locals_for_.SetSize(graph->GetBlocks().Size());
  }

  void BuildSsa();

  GrowableArray<HInstruction*>* GetLocalsFor(HBasicBlock* block) {
    GrowableArray<HInstruction*>* locals = locals_for_.Get(block->GetBlockId());
    if (locals == nullptr) {
      locals = new (GetGraph()->GetArena()) GrowableArray<HInstruction*>(
          GetGraph()->GetArena(), GetGraph()->GetNumberOfVRegs());
      locals->SetSize(GetGraph()->GetNumberOfVRegs());
      locals_for_.Put(block->GetBlockId(), locals);
    }
    return locals;
  }

  HInstruction* ValueOfLocal(HBasicBlock* block, size_t local);

  void VisitBasicBlock(HBasicBlock* block);
  void VisitLoadLocal(HLoadLocal* load);
  void VisitStoreLocal(HStoreLocal* store);
  void VisitInstruction(HInstruction* instruction);
  void VisitTemporary(HTemporary* instruction);

  static HInstruction* GetFloatOrDoubleEquivalent(HInstruction* user,
                                                  HInstruction* instruction,
                                                  Primitive::Type type);

  static HInstruction* GetReferenceTypeEquivalent(HInstruction* instruction);

  static constexpr const char* kSsaBuilderPassName = "ssa_builder";

 private:
  void FixNullConstantType();
  void EquivalentPhisCleanup();

  static HFloatConstant* GetFloatEquivalent(HIntConstant* constant);
  static HDoubleConstant* GetDoubleEquivalent(HLongConstant* constant);
  static HPhi* GetFloatDoubleOrReferenceEquivalentOfPhi(HPhi* phi, Primitive::Type type);

  // Locals for the current block being visited.
  GrowableArray<HInstruction*>* current_locals_;

  // Keep track of loop headers found. The last phase of the analysis iterates
  // over these blocks to set the inputs of their phis.
  GrowableArray<HBasicBlock*> loop_headers_;

  // HEnvironment for each block.
  GrowableArray<GrowableArray<HInstruction*>*> locals_for_;

  DISALLOW_COPY_AND_ASSIGN(SsaBuilder);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_SSA_BUILDER_H_
