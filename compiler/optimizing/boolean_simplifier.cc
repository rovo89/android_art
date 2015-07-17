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

#include "boolean_simplifier.h"

namespace art {

void HBooleanSimplifier::TryRemovingNegatedCondition(HBasicBlock* block) {
  DCHECK(block->EndsWithIf());

  // Check if the condition is a Boolean negation.
  HIf* if_instruction = block->GetLastInstruction()->AsIf();
  HInstruction* boolean_not = if_instruction->InputAt(0);
  if (!boolean_not->IsBooleanNot()) {
    return;
  }

  // Make BooleanNot's input the condition of the If and swap branches.
  if_instruction->ReplaceInput(boolean_not->InputAt(0), 0);
  block->SwapSuccessors();

  // Remove the BooleanNot if it is now unused.
  if (!boolean_not->HasUses()) {
    boolean_not->GetBlock()->RemoveInstruction(boolean_not);
  }
}

// Returns true if 'block1' and 'block2' are empty, merge into the same single
// successor and the successor can only be reached from them.
static bool BlocksDoMergeTogether(HBasicBlock* block1, HBasicBlock* block2) {
  if (!block1->IsSingleGoto() || !block2->IsSingleGoto()) return false;
  HBasicBlock* succ1 = block1->GetSuccessors().Get(0);
  HBasicBlock* succ2 = block2->GetSuccessors().Get(0);
  return succ1 == succ2 && succ1->GetPredecessors().Size() == 2u;
}

// Returns true if the outcome of the branching matches the boolean value of
// the branching condition.
static bool PreservesCondition(HInstruction* input_true, HInstruction* input_false) {
  return input_true->IsIntConstant() && input_true->AsIntConstant()->IsOne()
      && input_false->IsIntConstant() && input_false->AsIntConstant()->IsZero();
}

// Returns true if the outcome of the branching is exactly opposite of the
// boolean value of the branching condition.
static bool NegatesCondition(HInstruction* input_true, HInstruction* input_false) {
  return input_true->IsIntConstant() && input_true->AsIntConstant()->IsZero()
      && input_false->IsIntConstant() && input_false->AsIntConstant()->IsOne();
}

// Returns an instruction with the opposite boolean value from 'cond'.
static HInstruction* GetOppositeCondition(HInstruction* cond) {
  HGraph* graph = cond->GetBlock()->GetGraph();
  ArenaAllocator* allocator = graph->GetArena();

  if (cond->IsCondition()) {
    HInstruction* lhs = cond->InputAt(0);
    HInstruction* rhs = cond->InputAt(1);
    if (cond->IsEqual()) {
      return new (allocator) HNotEqual(lhs, rhs);
    } else if (cond->IsNotEqual()) {
      return new (allocator) HEqual(lhs, rhs);
    } else if (cond->IsLessThan()) {
      return new (allocator) HGreaterThanOrEqual(lhs, rhs);
    } else if (cond->IsLessThanOrEqual()) {
      return new (allocator) HGreaterThan(lhs, rhs);
    } else if (cond->IsGreaterThan()) {
      return new (allocator) HLessThanOrEqual(lhs, rhs);
    } else {
      DCHECK(cond->IsGreaterThanOrEqual());
      return new (allocator) HLessThan(lhs, rhs);
    }
  } else if (cond->IsIntConstant()) {
    HIntConstant* int_const = cond->AsIntConstant();
    if (int_const->IsZero()) {
      return graph->GetIntConstant(1);
    } else {
      DCHECK(int_const->IsOne());
      return graph->GetIntConstant(0);
    }
  } else {
    // General case when 'cond' is another instruction of type boolean,
    // as verified by SSAChecker.
    return new (allocator) HBooleanNot(cond);
  }
}

void HBooleanSimplifier::TryRemovingBooleanSelection(HBasicBlock* block) {
  DCHECK(block->EndsWithIf());

  // Find elements of the pattern.
  HIf* if_instruction = block->GetLastInstruction()->AsIf();
  HBasicBlock* true_block = if_instruction->IfTrueSuccessor();
  HBasicBlock* false_block = if_instruction->IfFalseSuccessor();
  if (!BlocksDoMergeTogether(true_block, false_block)) {
    return;
  }
  HBasicBlock* merge_block = true_block->GetSuccessors().Get(0);
  if (!merge_block->HasSinglePhi()) {
    return;
  }
  HPhi* phi = merge_block->GetFirstPhi()->AsPhi();
  HInstruction* true_value = phi->InputAt(merge_block->GetPredecessorIndexOf(true_block));
  HInstruction* false_value = phi->InputAt(merge_block->GetPredecessorIndexOf(false_block));

  // Check if the selection negates/preserves the value of the condition and
  // if so, generate a suitable replacement instruction.
  HInstruction* if_condition = if_instruction->InputAt(0);
  HInstruction* replacement;
  if (NegatesCondition(true_value, false_value)) {
    replacement = GetOppositeCondition(if_condition);
    if (replacement->GetBlock() == nullptr) {
      block->InsertInstructionBefore(replacement, if_instruction);
    }
  } else if (PreservesCondition(true_value, false_value)) {
    replacement = if_condition;
  } else {
    return;
  }

  // Replace the selection outcome with the new instruction.
  phi->ReplaceWith(replacement);
  merge_block->RemovePhi(phi);

  // Delete the true branch and merge the resulting chain of blocks
  // 'block->false_block->merge_block' into one.
  true_block->DisconnectAndDelete();
  block->MergeWith(false_block);
  block->MergeWith(merge_block);

  // No need to update any dominance information, as we are simplifying
  // a simple diamond shape, where the join block is merged with the
  // entry block. Any following blocks would have had the join block
  // as a dominator, and `MergeWith` handles changing that to the
  // entry block.
}

void HBooleanSimplifier::Run() {
  // Iterate in post order in the unlikely case that removing one occurrence of
  // the selection pattern empties a branch block of another occurrence.
  // Otherwise the order does not matter.
  for (HPostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    if (!block->EndsWithIf()) continue;

    // If condition is negated, remove the negation and swap the branches.
    TryRemovingNegatedCondition(block);

    // If this is a boolean-selection diamond pattern, replace its result with
    // the condition value (or its negation) and simplify the graph.
    TryRemovingBooleanSelection(block);
  }
}

}  // namespace art
