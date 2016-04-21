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
class SsaBuilder : public ValueObject {
 public:
  SsaBuilder(HGraph* graph,
             Handle<mirror::DexCache> dex_cache,
             StackHandleScopeCollection* handles)
      : graph_(graph),
        dex_cache_(dex_cache),
        handles_(handles),
        agets_fixed_(false),
        ambiguous_agets_(graph->GetArena()->Adapter(kArenaAllocGraphBuilder)),
        ambiguous_asets_(graph->GetArena()->Adapter(kArenaAllocGraphBuilder)),
        uninitialized_strings_(graph->GetArena()->Adapter(kArenaAllocGraphBuilder)) {
    graph_->InitializeInexactObjectRTI(handles);
  }

  GraphAnalysisResult BuildSsa();

  HInstruction* GetFloatOrDoubleEquivalent(HInstruction* instruction, Primitive::Type type);
  HInstruction* GetReferenceTypeEquivalent(HInstruction* instruction);

  void MaybeAddAmbiguousArrayGet(HArrayGet* aget) {
    Primitive::Type type = aget->GetType();
    DCHECK(!Primitive::IsFloatingPointType(type));
    if (Primitive::IsIntOrLongType(type)) {
      ambiguous_agets_.push_back(aget);
    }
  }

  void MaybeAddAmbiguousArraySet(HArraySet* aset) {
    Primitive::Type type = aset->GetValue()->GetType();
    if (Primitive::IsIntOrLongType(type)) {
      ambiguous_asets_.push_back(aset);
    }
  }

  void AddUninitializedString(HNewInstance* string) {
    // In some rare cases (b/27847265), the same NewInstance may be seen
    // multiple times. We should only consider it once for removal, so we
    // ensure it is not added more than once.
    // Note that we cannot check whether this really is a NewInstance of String
    // before RTP. We DCHECK that in RemoveRedundantUninitializedStrings.
    if (!ContainsElement(uninitialized_strings_, string)) {
      uninitialized_strings_.push_back(string);
    }
  }

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

  HFloatConstant* GetFloatEquivalent(HIntConstant* constant);
  HDoubleConstant* GetDoubleEquivalent(HLongConstant* constant);
  HPhi* GetFloatDoubleOrReferenceEquivalentOfPhi(HPhi* phi, Primitive::Type type);
  HArrayGet* GetFloatOrDoubleEquivalentOfArrayGet(HArrayGet* aget);

  void RemoveRedundantUninitializedStrings();

  HGraph* graph_;
  Handle<mirror::DexCache> dex_cache_;
  StackHandleScopeCollection* const handles_;

  // True if types of ambiguous ArrayGets have been resolved.
  bool agets_fixed_;

  ArenaVector<HArrayGet*> ambiguous_agets_;
  ArenaVector<HArraySet*> ambiguous_asets_;
  ArenaVector<HNewInstance*> uninitialized_strings_;

  DISALLOW_COPY_AND_ASSIGN(SsaBuilder);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_SSA_BUILDER_H_
