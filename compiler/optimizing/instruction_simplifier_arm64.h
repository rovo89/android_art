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

#ifndef ART_COMPILER_OPTIMIZING_INSTRUCTION_SIMPLIFIER_ARM64_H_
#define ART_COMPILER_OPTIMIZING_INSTRUCTION_SIMPLIFIER_ARM64_H_

#include "nodes.h"
#include "optimization.h"

namespace art {
namespace arm64 {

class InstructionSimplifierArm64Visitor : public HGraphVisitor {
 public:
  InstructionSimplifierArm64Visitor(HGraph* graph, OptimizingCompilerStats* stats)
      : HGraphVisitor(graph), stats_(stats) {}

 private:
  void RecordSimplification() {
    if (stats_ != nullptr) {
      stats_->RecordStat(kInstructionSimplificationsArch);
    }
  }

  void TryExtractArrayAccessAddress(HInstruction* access,
                                    HInstruction* array,
                                    HInstruction* index,
                                    int access_size);
  bool TryMergeIntoUsersShifterOperand(HInstruction* instruction);
  bool TryMergeIntoShifterOperand(HInstruction* use,
                                  HInstruction* bitfield_op,
                                  bool do_merge);
  bool CanMergeIntoShifterOperand(HInstruction* use, HInstruction* bitfield_op) {
    return TryMergeIntoShifterOperand(use, bitfield_op, false);
  }
  bool MergeIntoShifterOperand(HInstruction* use, HInstruction* bitfield_op) {
    DCHECK(CanMergeIntoShifterOperand(use, bitfield_op));
    return TryMergeIntoShifterOperand(use, bitfield_op, true);
  }

  /**
   * This simplifier uses a special-purpose BB visitor.
   * (1) No need to visit Phi nodes.
   * (2) Since statements can be removed in a "forward" fashion,
   *     the visitor should test if each statement is still there.
   */
  void VisitBasicBlock(HBasicBlock* block) OVERRIDE {
    // TODO: fragile iteration, provide more robust iterators?
    for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
      HInstruction* instruction = it.Current();
      if (instruction->IsInBlock()) {
        instruction->Accept(this);
      }
    }
  }

  // HInstruction visitors, sorted alphabetically.
  void VisitAnd(HAnd* instruction) OVERRIDE;
  void VisitArrayGet(HArrayGet* instruction) OVERRIDE;
  void VisitArraySet(HArraySet* instruction) OVERRIDE;
  void VisitMul(HMul* instruction) OVERRIDE;
  void VisitOr(HOr* instruction) OVERRIDE;
  void VisitShl(HShl* instruction) OVERRIDE;
  void VisitShr(HShr* instruction) OVERRIDE;
  void VisitTypeConversion(HTypeConversion* instruction) OVERRIDE;
  void VisitUShr(HUShr* instruction) OVERRIDE;
  void VisitXor(HXor* instruction) OVERRIDE;

  OptimizingCompilerStats* stats_;
};


class InstructionSimplifierArm64 : public HOptimization {
 public:
  InstructionSimplifierArm64(HGraph* graph, OptimizingCompilerStats* stats)
    : HOptimization(graph, "instruction_simplifier_arm64", stats) {}

  void Run() OVERRIDE {
    InstructionSimplifierArm64Visitor visitor(graph_, stats_);
    visitor.VisitReversePostOrder();
  }
};

}  // namespace arm64
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_INSTRUCTION_SIMPLIFIER_ARM64_H_
