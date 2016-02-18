/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "select_generator.h"

namespace art {

static constexpr size_t kMaxInstructionsInBranch = 1u;

// Returns true if `block` has only one predecessor, ends with a Goto and
// contains at most `kMaxInstructionsInBranch` other movable instruction with
// no side-effects.
static bool IsSimpleBlock(HBasicBlock* block) {
  if (block->GetPredecessors().size() != 1u) {
    return false;
  }
  DCHECK(block->GetPhis().IsEmpty());

  size_t num_instructions = 0u;
  for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
    HInstruction* instruction = it.Current();
    if (instruction->IsControlFlow()) {
      return instruction->IsGoto() && num_instructions <= kMaxInstructionsInBranch;
    } else if (instruction->CanBeMoved() && !instruction->HasSideEffects()) {
      num_instructions++;
    } else {
      return false;
    }
  }

  LOG(FATAL) << "Unreachable";
  UNREACHABLE();
}

// Returns true if 'block1' and 'block2' are empty, merge into the same single
// successor and the successor can only be reached from them.
static bool BlocksMergeTogether(HBasicBlock* block1, HBasicBlock* block2) {
  return block1->GetSingleSuccessor() == block2->GetSingleSuccessor();
}

// Returns nullptr if `block` has either no phis or there is more than one phi
// with different inputs at `index1` and `index2`. Otherwise returns that phi.
static HPhi* GetSingleChangedPhi(HBasicBlock* block, size_t index1, size_t index2) {
  DCHECK_NE(index1, index2);

  HPhi* select_phi = nullptr;
  for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    HPhi* phi = it.Current()->AsPhi();
    if (phi->InputAt(index1) != phi->InputAt(index2)) {
      if (select_phi == nullptr) {
        // First phi with different inputs for the two indices found.
        select_phi = phi;
      } else {
        // More than one phis has different inputs for the two indices.
        return nullptr;
      }
    }
  }
  return select_phi;
}

void HSelectGenerator::Run() {
  // Iterate in post order in the unlikely case that removing one occurrence of
  // the selection pattern empties a branch block of another occurrence.
  // Otherwise the order does not matter.
  for (HPostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    if (!block->EndsWithIf()) continue;

    // Find elements of the diamond pattern.
    HIf* if_instruction = block->GetLastInstruction()->AsIf();
    HBasicBlock* true_block = if_instruction->IfTrueSuccessor();
    HBasicBlock* false_block = if_instruction->IfFalseSuccessor();
    DCHECK_NE(true_block, false_block);
    if (!IsSimpleBlock(true_block) ||
        !IsSimpleBlock(false_block) ||
        !BlocksMergeTogether(true_block, false_block)) {
      continue;
    }
    HBasicBlock* merge_block = true_block->GetSingleSuccessor();

    // If the branches are not empty, move instructions in front of the If.
    // TODO(dbrazdil): This puts an instruction between If and its condition.
    //                 Implement moving of conditions to first users if possible.
    if (!true_block->IsSingleGoto()) {
      true_block->MoveInstructionBefore(true_block->GetFirstInstruction(), if_instruction);
    }
    if (!false_block->IsSingleGoto()) {
      false_block->MoveInstructionBefore(false_block->GetFirstInstruction(), if_instruction);
    }
    DCHECK(true_block->IsSingleGoto());
    DCHECK(false_block->IsSingleGoto());

    // Find the resulting true/false values.
    size_t predecessor_index_true = merge_block->GetPredecessorIndexOf(true_block);
    size_t predecessor_index_false = merge_block->GetPredecessorIndexOf(false_block);
    DCHECK_NE(predecessor_index_true, predecessor_index_false);

    HPhi* phi = GetSingleChangedPhi(merge_block, predecessor_index_true, predecessor_index_false);
    if (phi == nullptr) {
      continue;
    }
    HInstruction* true_value = phi->InputAt(predecessor_index_true);
    HInstruction* false_value = phi->InputAt(predecessor_index_false);

    // Create the Select instruction and insert it in front of the If.
    HSelect* select = new (graph_->GetArena()) HSelect(if_instruction->InputAt(0),
                                                       true_value,
                                                       false_value,
                                                       if_instruction->GetDexPc());
    if (phi->GetType() == Primitive::kPrimNot) {
      select->SetReferenceTypeInfo(phi->GetReferenceTypeInfo());
    }
    block->InsertInstructionBefore(select, if_instruction);

    // Remove the true branch which removes the corresponding Phi input.
    // If left only with the false branch, the Phi is automatically removed.
    phi->ReplaceInput(select, predecessor_index_false);
    bool only_two_predecessors = (merge_block->GetPredecessors().size() == 2u);
    true_block->DisconnectAndDelete();
    DCHECK_EQ(only_two_predecessors, phi->GetBlock() == nullptr);

    // Merge remaining blocks which are now connected with Goto.
    DCHECK_EQ(block->GetSingleSuccessor(), false_block);
    block->MergeWith(false_block);
    if (only_two_predecessors) {
      DCHECK_EQ(block->GetSingleSuccessor(), merge_block);
      block->MergeWith(merge_block);
    }

    MaybeRecordStat(MethodCompilationStat::kSelectGenerated);

    // No need to update dominance information, as we are simplifying
    // a simple diamond shape, where the join block is merged with the
    // entry block. Any following blocks would have had the join block
    // as a dominator, and `MergeWith` handles changing that to the
    // entry block.
  }
}

}  // namespace art
