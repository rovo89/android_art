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

#include "graph_checker.h"

#include <map>
#include <string>
#include <sstream>

#include "base/bit_vector-inl.h"
#include "base/stringprintf.h"

namespace art {

void GraphChecker::VisitBasicBlock(HBasicBlock* block) {
  current_block_ = block;

  // Check consistency with respect to predecessors of `block`.
  const GrowableArray<HBasicBlock*>& predecessors = block->GetPredecessors();
  std::map<HBasicBlock*, size_t> predecessors_count;
  for (size_t i = 0, e = predecessors.Size(); i < e; ++i) {
    HBasicBlock* p = predecessors.Get(i);
    ++predecessors_count[p];
  }
  for (auto& pc : predecessors_count) {
    HBasicBlock* p = pc.first;
    size_t p_count_in_block_predecessors = pc.second;
    const GrowableArray<HBasicBlock*>& p_successors = p->GetSuccessors();
    size_t block_count_in_p_successors = 0;
    for (size_t j = 0, f = p_successors.Size(); j < f; ++j) {
      if (p_successors.Get(j) == block) {
        ++block_count_in_p_successors;
      }
    }
    if (p_count_in_block_predecessors != block_count_in_p_successors) {
      AddError(StringPrintf(
          "Block %d lists %zu occurrences of block %d in its predecessors, whereas "
          "block %d lists %zu occurrences of block %d in its successors.",
          block->GetBlockId(), p_count_in_block_predecessors, p->GetBlockId(),
          p->GetBlockId(), block_count_in_p_successors, block->GetBlockId()));
    }
  }

  // Check consistency with respect to successors of `block`.
  const GrowableArray<HBasicBlock*>& successors = block->GetSuccessors();
  std::map<HBasicBlock*, size_t> successors_count;
  for (size_t i = 0, e = successors.Size(); i < e; ++i) {
    HBasicBlock* s = successors.Get(i);
    ++successors_count[s];
  }
  for (auto& sc : successors_count) {
    HBasicBlock* s = sc.first;
    size_t s_count_in_block_successors = sc.second;
    const GrowableArray<HBasicBlock*>& s_predecessors = s->GetPredecessors();
    size_t block_count_in_s_predecessors = 0;
    for (size_t j = 0, f = s_predecessors.Size(); j < f; ++j) {
      if (s_predecessors.Get(j) == block) {
        ++block_count_in_s_predecessors;
      }
    }
    if (s_count_in_block_successors != block_count_in_s_predecessors) {
      AddError(StringPrintf(
          "Block %d lists %zu occurrences of block %d in its successors, whereas "
          "block %d lists %zu occurrences of block %d in its predecessors.",
          block->GetBlockId(), s_count_in_block_successors, s->GetBlockId(),
          s->GetBlockId(), block_count_in_s_predecessors, block->GetBlockId()));
    }
  }

  // Ensure `block` ends with a branch instruction.
  if (!block->EndsWithControlFlowInstruction()) {
    AddError(StringPrintf("Block %d does not end with a branch instruction.",
                          block->GetBlockId()));
  }

  // Visit this block's list of phis.
  for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    HInstruction* current = it.Current();
    // Ensure this block's list of phis contains only phis.
    if (!current->IsPhi()) {
      AddError(StringPrintf("Block %d has a non-phi in its phi list.",
                            current_block_->GetBlockId()));
    }
    if (current->GetNext() == nullptr && current != block->GetLastPhi()) {
      AddError(StringPrintf("The recorded last phi of block %d does not match "
                            "the actual last phi %d.",
                            current_block_->GetBlockId(),
                            current->GetId()));
    }
    current->Accept(this);
  }

  // Visit this block's list of instructions.
  for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
    HInstruction* current = it.Current();
    // Ensure this block's list of instructions does not contains phis.
    if (current->IsPhi()) {
      AddError(StringPrintf("Block %d has a phi in its non-phi list.",
                            current_block_->GetBlockId()));
    }
    if (current->GetNext() == nullptr && current != block->GetLastInstruction()) {
      AddError(StringPrintf("The recorded last instruction of block %d does not match "
                            "the actual last instruction %d.",
                            current_block_->GetBlockId(),
                            current->GetId()));
    }
    current->Accept(this);
  }
}

void GraphChecker::VisitBoundsCheck(HBoundsCheck* check) {
  if (!GetGraph()->HasBoundsChecks()) {
    AddError(StringPrintf("Instruction %s:%d is a HBoundsCheck, "
                          "but HasBoundsChecks() returns false",
                          check->DebugName(),
                          check->GetId()));
  }

  // Perform the instruction base checks too.
  VisitInstruction(check);
}

void GraphChecker::VisitInstruction(HInstruction* instruction) {
  if (seen_ids_.IsBitSet(instruction->GetId())) {
    AddError(StringPrintf("Instruction id %d is duplicate in graph.",
                          instruction->GetId()));
  } else {
    seen_ids_.SetBit(instruction->GetId());
  }

  // Ensure `instruction` is associated with `current_block_`.
  if (instruction->GetBlock() == nullptr) {
    AddError(StringPrintf("%s %d in block %d not associated with any block.",
                          instruction->IsPhi() ? "Phi" : "Instruction",
                          instruction->GetId(),
                          current_block_->GetBlockId()));
  } else if (instruction->GetBlock() != current_block_) {
    AddError(StringPrintf("%s %d in block %d associated with block %d.",
                          instruction->IsPhi() ? "Phi" : "Instruction",
                          instruction->GetId(),
                          current_block_->GetBlockId(),
                          instruction->GetBlock()->GetBlockId()));
  }

  // Ensure the inputs of `instruction` are defined in a block of the graph.
  for (HInputIterator input_it(instruction); !input_it.Done();
       input_it.Advance()) {
    HInstruction* input = input_it.Current();
    const HInstructionList& list = input->IsPhi()
        ? input->GetBlock()->GetPhis()
        : input->GetBlock()->GetInstructions();
    if (!list.Contains(input)) {
      AddError(StringPrintf("Input %d of instruction %d is not defined "
                            "in a basic block of the control-flow graph.",
                            input->GetId(),
                            instruction->GetId()));
    }
  }

  // Ensure the uses of `instruction` are defined in a block of the graph,
  // and the entry in the use list is consistent.
  for (HUseIterator<HInstruction*> use_it(instruction->GetUses());
       !use_it.Done(); use_it.Advance()) {
    HInstruction* use = use_it.Current()->GetUser();
    const HInstructionList& list = use->IsPhi()
        ? use->GetBlock()->GetPhis()
        : use->GetBlock()->GetInstructions();
    if (!list.Contains(use)) {
      AddError(StringPrintf("User %s:%d of instruction %d is not defined "
                            "in a basic block of the control-flow graph.",
                            use->DebugName(),
                            use->GetId(),
                            instruction->GetId()));
    }
    size_t use_index = use_it.Current()->GetIndex();
    if ((use_index >= use->InputCount()) || (use->InputAt(use_index) != instruction)) {
      AddError(StringPrintf("User %s:%d of instruction %d has a wrong "
                            "UseListNode index.",
                            use->DebugName(),
                            use->GetId(),
                            instruction->GetId()));
    }
  }

  // Ensure the environment uses entries are consistent.
  for (HUseIterator<HEnvironment*> use_it(instruction->GetEnvUses());
       !use_it.Done(); use_it.Advance()) {
    HEnvironment* use = use_it.Current()->GetUser();
    size_t use_index = use_it.Current()->GetIndex();
    if ((use_index >= use->Size()) || (use->GetInstructionAt(use_index) != instruction)) {
      AddError(StringPrintf("Environment user of %s:%d has a wrong "
                            "UseListNode index.",
                            instruction->DebugName(),
                            instruction->GetId()));
    }
  }

  // Ensure 'instruction' has pointers to its inputs' use entries.
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    HUserRecord<HInstruction*> input_record = instruction->InputRecordAt(i);
    HInstruction* input = input_record.GetInstruction();
    HUseListNode<HInstruction*>* use_node = input_record.GetUseNode();
    size_t use_index = use_node->GetIndex();
    if ((use_node == nullptr)
        || !input->GetUses().Contains(use_node)
        || (use_index >= e)
        || (use_index != i)) {
      AddError(StringPrintf("Instruction %s:%d has an invalid pointer to use entry "
                            "at input %u (%s:%d).",
                            instruction->DebugName(),
                            instruction->GetId(),
                            static_cast<unsigned>(i),
                            input->DebugName(),
                            input->GetId()));
    }
  }
}

void GraphChecker::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  VisitInstruction(invoke);

  if (invoke->IsStaticWithExplicitClinitCheck()) {
    size_t last_input_index = invoke->InputCount() - 1;
    HInstruction* last_input = invoke->InputAt(last_input_index);
    if (last_input == nullptr) {
      AddError(StringPrintf("Static invoke %s:%d marked as having an explicit clinit check "
                            "has a null pointer as last input.",
                            invoke->DebugName(),
                            invoke->GetId()));
    }
    if (!last_input->IsClinitCheck() && !last_input->IsLoadClass()) {
      AddError(StringPrintf("Static invoke %s:%d marked as having an explicit clinit check "
                            "has a last instruction (%s:%d) which is neither a clinit check "
                            "nor a load class instruction.",
                            invoke->DebugName(),
                            invoke->GetId(),
                            last_input->DebugName(),
                            last_input->GetId()));
    }
  }
}

void GraphChecker::VisitCheckCast(HCheckCast* check) {
  VisitInstruction(check);
  HInstruction* input = check->InputAt(1);
  if (!input->IsLoadClass()) {
    AddError(StringPrintf("%s:%d expects a HLoadClass as second input, not %s:%d.",
                          check->DebugName(),
                          check->GetId(),
                          input->DebugName(),
                          input->GetId()));
  }
}

void GraphChecker::VisitInstanceOf(HInstanceOf* instruction) {
  VisitInstruction(instruction);
  HInstruction* input = instruction->InputAt(1);
  if (!input->IsLoadClass()) {
    AddError(StringPrintf("%s:%d expects a HLoadClass as second input, not %s:%d.",
                          instruction->DebugName(),
                          instruction->GetId(),
                          input->DebugName(),
                          input->GetId()));
  }
}

void SSAChecker::VisitBasicBlock(HBasicBlock* block) {
  super_type::VisitBasicBlock(block);

  // Ensure there is no critical edge (i.e., an edge connecting a
  // block with multiple successors to a block with multiple
  // predecessors).
  if (block->GetSuccessors().Size() > 1) {
    for (size_t j = 0; j < block->GetSuccessors().Size(); ++j) {
      HBasicBlock* successor = block->GetSuccessors().Get(j);
      if (successor->GetPredecessors().Size() > 1) {
        AddError(StringPrintf("Critical edge between blocks %d and %d.",
                              block->GetBlockId(),
                              successor->GetBlockId()));
      }
    }
  }

  // Check Phi uniqueness (no two Phis with the same type refer to the same register).
  for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    HPhi* phi = it.Current()->AsPhi();
    if (phi->GetNextEquivalentPhiWithSameType() != nullptr) {
      std::stringstream type_str;
      type_str << phi->GetType();
      AddError(StringPrintf("Equivalent phi (%d) found for VReg %d with type: %s",
          phi->GetId(), phi->GetRegNumber(), type_str.str().c_str()));
    }
  }

  if (block->IsLoopHeader()) {
    CheckLoop(block);
  }
}

void SSAChecker::CheckLoop(HBasicBlock* loop_header) {
  int id = loop_header->GetBlockId();
  HLoopInformation* loop_information = loop_header->GetLoopInformation();

  // Ensure the pre-header block is first in the list of
  // predecessors of a loop header.
  if (!loop_header->IsLoopPreHeaderFirstPredecessor()) {
    AddError(StringPrintf(
        "Loop pre-header is not the first predecessor of the loop header %d.",
        id));
  }

  // Ensure the loop header has only one incoming branch and the remaining
  // predecessors are back edges.
  size_t num_preds = loop_header->GetPredecessors().Size();
  if (num_preds < 2) {
    AddError(StringPrintf(
        "Loop header %d has less than two predecessors: %zu.",
        id,
        num_preds));
  } else {
    HBasicBlock* first_predecessor = loop_header->GetPredecessors().Get(0);
    if (loop_information->IsBackEdge(*first_predecessor)) {
      AddError(StringPrintf(
          "First predecessor of loop header %d is a back edge.",
          id));
    }
    for (size_t i = 1, e = loop_header->GetPredecessors().Size(); i < e; ++i) {
      HBasicBlock* predecessor = loop_header->GetPredecessors().Get(i);
      if (!loop_information->IsBackEdge(*predecessor)) {
        AddError(StringPrintf(
            "Loop header %d has multiple incoming (non back edge) blocks.",
            id));
      }
    }
  }

  const ArenaBitVector& loop_blocks = loop_information->GetBlocks();

  // Ensure back edges belong to the loop.
  size_t num_back_edges = loop_information->GetBackEdges().Size();
  if (num_back_edges == 0) {
    AddError(StringPrintf(
        "Loop defined by header %d has no back edge.",
        id));
  } else {
    for (size_t i = 0; i < num_back_edges; ++i) {
      int back_edge_id = loop_information->GetBackEdges().Get(i)->GetBlockId();
      if (!loop_blocks.IsBitSet(back_edge_id)) {
        AddError(StringPrintf(
            "Loop defined by header %d has an invalid back edge %d.",
            id,
            back_edge_id));
      }
    }
  }

  // Ensure all blocks in the loop are live and dominated by the loop header.
  for (uint32_t i : loop_blocks.Indexes()) {
    HBasicBlock* loop_block = GetGraph()->GetBlocks().Get(i);
    if (loop_block == nullptr) {
      AddError(StringPrintf("Loop defined by header %d contains a previously removed block %d.",
                            id,
                            i));
    } else if (!loop_header->Dominates(loop_block)) {
      AddError(StringPrintf("Loop block %d not dominated by loop header %d.",
                            i,
                            id));
    }
  }

  // If this is a nested loop, ensure the outer loops contain a superset of the blocks.
  for (HLoopInformationOutwardIterator it(*loop_header); !it.Done(); it.Advance()) {
    HLoopInformation* outer_info = it.Current();
    if (!loop_blocks.IsSubsetOf(&outer_info->GetBlocks())) {
      AddError(StringPrintf("Blocks of loop defined by header %d are not a subset of blocks of "
                            "an outer loop defined by header %d.",
                            id,
                            outer_info->GetHeader()->GetBlockId()));
    }
  }
}

void SSAChecker::VisitInstruction(HInstruction* instruction) {
  super_type::VisitInstruction(instruction);

  // Ensure an instruction dominates all its uses.
  for (HUseIterator<HInstruction*> use_it(instruction->GetUses());
       !use_it.Done(); use_it.Advance()) {
    HInstruction* use = use_it.Current()->GetUser();
    if (!use->IsPhi() && !instruction->StrictlyDominates(use)) {
      AddError(StringPrintf("Instruction %d in block %d does not dominate "
                            "use %d in block %d.",
                            instruction->GetId(), current_block_->GetBlockId(),
                            use->GetId(), use->GetBlock()->GetBlockId()));
    }
  }

  // Ensure an instruction having an environment is dominated by the
  // instructions contained in the environment.
  for (HEnvironment* environment = instruction->GetEnvironment();
       environment != nullptr;
       environment = environment->GetParent()) {
    for (size_t i = 0, e = environment->Size(); i < e; ++i) {
      HInstruction* env_instruction = environment->GetInstructionAt(i);
      if (env_instruction != nullptr
          && !env_instruction->StrictlyDominates(instruction)) {
        AddError(StringPrintf("Instruction %d in environment of instruction %d "
                              "from block %d does not dominate instruction %d.",
                              env_instruction->GetId(),
                              instruction->GetId(),
                              current_block_->GetBlockId(),
                              instruction->GetId()));
      }
    }
  }
}

static Primitive::Type PrimitiveKind(Primitive::Type type) {
  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimShort:
    case Primitive::kPrimChar:
    case Primitive::kPrimInt:
      return Primitive::kPrimInt;
    default:
      return type;
  }
}

void SSAChecker::VisitPhi(HPhi* phi) {
  VisitInstruction(phi);

  // Ensure the first input of a phi is not itself.
  if (phi->InputAt(0) == phi) {
    AddError(StringPrintf("Loop phi %d in block %d is its own first input.",
                          phi->GetId(),
                          phi->GetBlock()->GetBlockId()));
  }

  // Ensure the number of inputs of a phi is the same as the number of
  // its predecessors.
  const GrowableArray<HBasicBlock*>& predecessors =
    phi->GetBlock()->GetPredecessors();
  if (phi->InputCount() != predecessors.Size()) {
    AddError(StringPrintf(
        "Phi %d in block %d has %zu inputs, "
        "but block %d has %zu predecessors.",
        phi->GetId(), phi->GetBlock()->GetBlockId(), phi->InputCount(),
        phi->GetBlock()->GetBlockId(), predecessors.Size()));
  } else {
    // Ensure phi input at index I either comes from the Ith
    // predecessor or from a block that dominates this predecessor.
    for (size_t i = 0, e = phi->InputCount(); i < e; ++i) {
      HInstruction* input = phi->InputAt(i);
      HBasicBlock* predecessor = predecessors.Get(i);
      if (!(input->GetBlock() == predecessor
            || input->GetBlock()->Dominates(predecessor))) {
        AddError(StringPrintf(
            "Input %d at index %zu of phi %d from block %d is not defined in "
            "predecessor number %zu nor in a block dominating it.",
            input->GetId(), i, phi->GetId(), phi->GetBlock()->GetBlockId(),
            i));
      }
    }
  }
  // Ensure that the inputs have the same primitive kind as the phi.
  for (size_t i = 0, e = phi->InputCount(); i < e; ++i) {
    HInstruction* input = phi->InputAt(i);
    if (PrimitiveKind(input->GetType()) != PrimitiveKind(phi->GetType())) {
        AddError(StringPrintf(
            "Input %d at index %zu of phi %d from block %d does not have the "
            "same type as the phi: %s versus %s",
            input->GetId(), i, phi->GetId(), phi->GetBlock()->GetBlockId(),
            Primitive::PrettyDescriptor(input->GetType()),
            Primitive::PrettyDescriptor(phi->GetType())));
    }
  }
  if (phi->GetType() != HPhi::ToPhiType(phi->GetType())) {
    AddError(StringPrintf("Phi %d in block %d does not have an expected phi type: %s",
                          phi->GetId(),
                          phi->GetBlock()->GetBlockId(),
                          Primitive::PrettyDescriptor(phi->GetType())));
  }
}

void SSAChecker::HandleBooleanInput(HInstruction* instruction, size_t input_index) {
  HInstruction* input = instruction->InputAt(input_index);
  if (input->IsIntConstant()) {
    int32_t value = input->AsIntConstant()->GetValue();
    if (value != 0 && value != 1) {
      AddError(StringPrintf(
          "%s instruction %d has a non-Boolean constant input %d whose value is: %d.",
          instruction->DebugName(),
          instruction->GetId(),
          static_cast<int>(input_index),
          value));
    }
  } else if (input->GetType() == Primitive::kPrimInt
             && (input->IsPhi() || input->IsAnd() || input->IsOr() || input->IsXor())) {
    // TODO: We need a data-flow analysis to determine if the Phi or
    //       binary operation is actually Boolean. Allow for now.
  } else if (input->GetType() != Primitive::kPrimBoolean) {
    AddError(StringPrintf(
        "%s instruction %d has a non-Boolean input %d whose type is: %s.",
        instruction->DebugName(),
        instruction->GetId(),
        static_cast<int>(input_index),
        Primitive::PrettyDescriptor(input->GetType())));
  }
}

void SSAChecker::VisitIf(HIf* instruction) {
  VisitInstruction(instruction);
  HandleBooleanInput(instruction, 0);
}

void SSAChecker::VisitBooleanNot(HBooleanNot* instruction) {
  VisitInstruction(instruction);
  HandleBooleanInput(instruction, 0);
}

void SSAChecker::VisitCondition(HCondition* op) {
  VisitInstruction(op);
  if (op->GetType() != Primitive::kPrimBoolean) {
    AddError(StringPrintf(
        "Condition %s %d has a non-Boolean result type: %s.",
        op->DebugName(), op->GetId(),
        Primitive::PrettyDescriptor(op->GetType())));
  }
  HInstruction* lhs = op->InputAt(0);
  HInstruction* rhs = op->InputAt(1);
  if (PrimitiveKind(lhs->GetType()) != PrimitiveKind(rhs->GetType())) {
    AddError(StringPrintf(
        "Condition %s %d has inputs of different types: %s, and %s.",
        op->DebugName(), op->GetId(),
        Primitive::PrettyDescriptor(lhs->GetType()),
        Primitive::PrettyDescriptor(rhs->GetType())));
  }
  if (!op->IsEqual() && !op->IsNotEqual()) {
    if ((lhs->GetType() == Primitive::kPrimNot)) {
      AddError(StringPrintf(
          "Condition %s %d uses an object as left-hand side input.",
          op->DebugName(), op->GetId()));
    } else if (rhs->GetType() == Primitive::kPrimNot) {
      AddError(StringPrintf(
          "Condition %s %d uses an object as right-hand side input.",
          op->DebugName(), op->GetId()));
    }
  }
}

void SSAChecker::VisitBinaryOperation(HBinaryOperation* op) {
  VisitInstruction(op);
  if (op->IsUShr() || op->IsShr() || op->IsShl()) {
    if (PrimitiveKind(op->InputAt(1)->GetType()) != Primitive::kPrimInt) {
      AddError(StringPrintf(
          "Shift operation %s %d has a non-int kind second input: "
          "%s of type %s.",
          op->DebugName(), op->GetId(),
          op->InputAt(1)->DebugName(),
          Primitive::PrettyDescriptor(op->InputAt(1)->GetType())));
    }
  } else {
    if (PrimitiveKind(op->InputAt(0)->GetType()) != PrimitiveKind(op->InputAt(1)->GetType())) {
      AddError(StringPrintf(
          "Binary operation %s %d has inputs of different types: "
          "%s, and %s.",
          op->DebugName(), op->GetId(),
          Primitive::PrettyDescriptor(op->InputAt(0)->GetType()),
          Primitive::PrettyDescriptor(op->InputAt(1)->GetType())));
    }
  }

  if (op->IsCompare()) {
    if (op->GetType() != Primitive::kPrimInt) {
      AddError(StringPrintf(
          "Compare operation %d has a non-int result type: %s.",
          op->GetId(),
          Primitive::PrettyDescriptor(op->GetType())));
    }
  } else {
    // Use the first input, so that we can also make this check for shift operations.
    if (PrimitiveKind(op->GetType()) != PrimitiveKind(op->InputAt(0)->GetType())) {
      AddError(StringPrintf(
          "Binary operation %s %d has a result type different "
          "from its input type: %s vs %s.",
          op->DebugName(), op->GetId(),
          Primitive::PrettyDescriptor(op->GetType()),
          Primitive::PrettyDescriptor(op->InputAt(0)->GetType())));
    }
  }
}

void SSAChecker::VisitConstant(HConstant* instruction) {
  HBasicBlock* block = instruction->GetBlock();
  if (!block->IsEntryBlock()) {
    AddError(StringPrintf(
        "%s %d should be in the entry block but is in block %d.",
        instruction->DebugName(),
        instruction->GetId(),
        block->GetBlockId()));
  }
}

}  // namespace art
