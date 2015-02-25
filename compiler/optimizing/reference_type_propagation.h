/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ART_COMPILER_OPTIMIZING_REFERENCE_TYPE_PROPAGATION_H_
#define ART_COMPILER_OPTIMIZING_REFERENCE_TYPE_PROPAGATION_H_

#include "driver/dex_compilation_unit.h"
#include "handle_scope-inl.h"
#include "nodes.h"
#include "optimization.h"
#include "optimizing_compiler_stats.h"

namespace art {

/**
 * Propagates reference types to instructions.
 */
class ReferenceTypePropagation : public HOptimization {
 public:
  ReferenceTypePropagation(HGraph* graph,
                           const DexFile& dex_file,
                           const DexCompilationUnit& dex_compilation_unit,
                           StackHandleScopeCollection* handles)
    : HOptimization(graph, true, kReferenceTypePropagationPassName),
      dex_file_(dex_file),
      dex_compilation_unit_(dex_compilation_unit),
      handles_(handles),
      worklist_(graph->GetArena(), kDefaultWorklistSize) {}

  void Run() OVERRIDE;

  static constexpr const char* kReferenceTypePropagationPassName = "reference_type_propagation";

 private:
  void VisitNewInstance(HNewInstance* new_instance);
  void VisitLoadClass(HLoadClass* load_class);
  void VisitPhi(HPhi* phi);
  void VisitBasicBlock(HBasicBlock* block);

  void UpdateBoundType(HBoundType* bound_type) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void UpdatePhi(HPhi* phi) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void BoundTypeForIfNotNull(HBasicBlock* block);
  void BoundTypeForIfInstanceOf(HBasicBlock* block);

  void ProcessWorklist();
  void AddToWorklist(HInstruction* instr);
  void AddDependentInstructionsToWorklist(HInstruction* instr);

  bool UpdateNullability(HInstruction* instr);
  bool UpdateReferenceTypeInfo(HInstruction* instr);

  ReferenceTypeInfo MergeTypes(const ReferenceTypeInfo& a, const ReferenceTypeInfo& b)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  const DexFile& dex_file_;
  const DexCompilationUnit& dex_compilation_unit_;
  StackHandleScopeCollection* handles_;

  GrowableArray<HInstruction*> worklist_;

  static constexpr size_t kDefaultWorklistSize = 8;

  DISALLOW_COPY_AND_ASSIGN(ReferenceTypePropagation);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_REFERENCE_TYPE_PROPAGATION_H_
