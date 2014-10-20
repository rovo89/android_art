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

#include <string>
#include <map>
#include <sstream>

#include "base/bit_vector-inl.h"

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
      std::stringstream error;
      error << "Block " << block->GetBlockId()
            << " lists " << p_count_in_block_predecessors
            << " occurrences of block " << p->GetBlockId()
            << " in its predecessors, whereas block " << p->GetBlockId()
            << " lists " << block_count_in_p_successors
            << " occurrences of block " << block->GetBlockId()
            << " in its successors.";
      errors_.Insert(error.str());
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
      std::stringstream error;
      error << "Block " << block->GetBlockId()
            << " lists " << s_count_in_block_successors
            << " occurrences of block " << s->GetBlockId()
            << " in its successors, whereas block " << s->GetBlockId()
            << " lists " << block_count_in_s_predecessors
            << " occurrences of block " << block->GetBlockId()
            << " in its predecessors.";
      errors_.Insert(error.str());
    }
  }

  // Ensure `block` ends with a branch instruction.
  HInstruction* last_inst = block->GetLastInstruction();
  if (last_inst == nullptr || !last_inst->IsControlFlow()) {
    std::stringstream error;
    error  << "Block " << block->GetBlockId()
           << " does not end with a branch instruction.";
    errors_.Insert(error.str());
  }

  // Visit this block's list of phis.
  for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    // Ensure this block's list of phis contains only phis.
    if (!it.Current()->IsPhi()) {
      std::stringstream error;
      error << "Block " << current_block_->GetBlockId()
            << " has a non-phi in its phi list.";
      errors_.Insert(error.str());
    }
    it.Current()->Accept(this);
  }

  // Visit this block's list of instructions.
  for (HInstructionIterator it(block->GetInstructions()); !it.Done();
       it.Advance()) {
    // Ensure this block's list of instructions does not contains phis.
    if (it.Current()->IsPhi()) {
      std::stringstream error;
      error << "Block " << current_block_->GetBlockId()
            << " has a phi in its non-phi list.";
      errors_.Insert(error.str());
    }
    it.Current()->Accept(this);
  }
}

void GraphChecker::VisitInstruction(HInstruction* instruction) {
  // Ensure `instruction` is associated with `current_block_`.
  if (instruction->GetBlock() != current_block_) {
    std::stringstream error;
    if (instruction->IsPhi()) {
      error << "Phi ";
    } else {
      error << "Instruction ";
    }
    error << instruction->GetId() << " in block "
          << current_block_->GetBlockId();
    if (instruction->GetBlock() != nullptr) {
      error << " associated with block "
            << instruction->GetBlock()->GetBlockId() << ".";
    } else {
      error << " not associated with any block.";
    }
    errors_.Insert(error.str());
  }

  // Ensure the inputs of `instruction` are defined in a block of the graph.
  for (HInputIterator input_it(instruction); !input_it.Done();
       input_it.Advance()) {
    HInstruction* input = input_it.Current();
    const HInstructionList& list = input->IsPhi()
        ? input->GetBlock()->GetPhis()
        : input->GetBlock()->GetInstructions();
    if (!list.Contains(input)) {
      std::stringstream error;
      error << "Input " << input->GetId()
            << " of instruction " << instruction->GetId()
            << " is not defined in a basic block of the control-flow graph.";
      errors_.Insert(error.str());
    }
  }

  // Ensure the uses of `instruction` are defined in a block of the graph.
  for (HUseIterator<HInstruction> use_it(instruction->GetUses());
       !use_it.Done(); use_it.Advance()) {
    HInstruction* use = use_it.Current()->GetUser();
    const HInstructionList& list = use->IsPhi()
        ? use->GetBlock()->GetPhis()
        : use->GetBlock()->GetInstructions();
    if (!list.Contains(use)) {
      std::stringstream error;
      error << "User " << use->GetId()
            << " of instruction " << instruction->GetId()
            << " is not defined in a basic block of the control-flow graph.";
      errors_.Insert(error.str());
    }
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
        std::stringstream error;
        error << "Critical edge between blocks " << block->GetBlockId()
              << " and "  << successor->GetBlockId() << ".";
        errors_.Insert(error.str());
      }
    }
  }

  if (block->IsLoopHeader()) {
    CheckLoop(block);
  }
}

void SSAChecker::CheckLoop(HBasicBlock* loop_header) {
  int id = loop_header->GetBlockId();

  // Ensure the pre-header block is first in the list of
  // predecessors of a loop header.
  if (!loop_header->IsLoopPreHeaderFirstPredecessor()) {
    std::stringstream error;
    error << "Loop pre-header is not the first predecessor of the loop header "
          << id << ".";
    errors_.Insert(error.str());
  }

  // Ensure the loop header has only two predecessors and that only the
  // second one is a back edge.
  if (loop_header->GetPredecessors().Size() < 2) {
    std::stringstream error;
    error << "Loop header " << id << " has less than two predecessors.";
    errors_.Insert(error.str());
  } else if (loop_header->GetPredecessors().Size() > 2) {
    std::stringstream error;
    error << "Loop header " << id << " has more than two predecessors.";
    errors_.Insert(error.str());
  } else {
    HLoopInformation* loop_information = loop_header->GetLoopInformation();
    HBasicBlock* first_predecessor = loop_header->GetPredecessors().Get(0);
    if (loop_information->IsBackEdge(first_predecessor)) {
      std::stringstream error;
      error << "First predecessor of loop header " << id << " is a back edge.";
      errors_.Insert(error.str());
    }
    HBasicBlock* second_predecessor = loop_header->GetPredecessors().Get(1);
    if (!loop_information->IsBackEdge(second_predecessor)) {
      std::stringstream error;
      error << "Second predecessor of loop header " << id
            << " is not a back edge.";
      errors_.Insert(error.str());
    }
  }

  // Ensure there is only one back edge per loop.
  size_t num_back_edges =
    loop_header->GetLoopInformation()->GetBackEdges().Size();
  if (num_back_edges != 1) {
      std::stringstream error;
      error << "Loop defined by header " << id << " has "
            << num_back_edges << " back edge(s).";
      errors_.Insert(error.str());
  }

  // Ensure all blocks in the loop are dominated by the loop header.
  const ArenaBitVector& loop_blocks =
    loop_header->GetLoopInformation()->GetBlocks();
  for (uint32_t i : loop_blocks.Indexes()) {
    HBasicBlock* loop_block = GetGraph()->GetBlocks().Get(i);
    if (!loop_header->Dominates(loop_block)) {
      std::stringstream error;
      error << "Loop block " << loop_block->GetBlockId()
            << " not dominated by loop header " << id;
      errors_.Insert(error.str());
    }
  }
}

void SSAChecker::VisitInstruction(HInstruction* instruction) {
  super_type::VisitInstruction(instruction);

  // Ensure an instruction dominates all its uses.
  for (HUseIterator<HInstruction> use_it(instruction->GetUses());
       !use_it.Done(); use_it.Advance()) {
    HInstruction* use = use_it.Current()->GetUser();
    if (!use->IsPhi() && !instruction->StrictlyDominates(use)) {
      std::stringstream error;
      error << "Instruction " << instruction->GetId()
            << " in block " << current_block_->GetBlockId()
            << " does not dominate use " << use->GetId()
            << " in block " << use->GetBlock()->GetBlockId() << ".";
      errors_.Insert(error.str());
    }
  }

  // Ensure an instruction having an environment is dominated by the
  // instructions contained in the environment.
  HEnvironment* environment = instruction->GetEnvironment();
  if (environment != nullptr) {
    for (size_t i = 0, e = environment->Size(); i < e; ++i) {
      HInstruction* env_instruction = environment->GetInstructionAt(i);
      if (env_instruction != nullptr
          && !env_instruction->StrictlyDominates(instruction)) {
        std::stringstream error;
        error << "Instruction " << env_instruction->GetId()
              << " in environment of instruction " << instruction->GetId()
              << " from block " << current_block_->GetBlockId()
              << " does not dominate instruction " << instruction->GetId()
              << ".";
        errors_.Insert(error.str());
      }
    }
  }
}

void SSAChecker::VisitPhi(HPhi* phi) {
  VisitInstruction(phi);

  // Ensure the first input of a phi is not itself.
  if (phi->InputAt(0) == phi) {
      std::stringstream error;
      error << "Loop phi " << phi->GetId()
            << " in block " << phi->GetBlock()->GetBlockId()
            << " is its own first input.";
      errors_.Insert(error.str());
  }

  // Ensure the number of phi inputs is the same as the number of
  // its predecessors.
  const GrowableArray<HBasicBlock*>& predecessors =
    phi->GetBlock()->GetPredecessors();
  if (phi->InputCount() != predecessors.Size()) {
    std::stringstream error;
    error << "Phi " << phi->GetId()
          << " in block " << phi->GetBlock()->GetBlockId()
          << " has " << phi->InputCount() << " inputs, but block "
          << phi->GetBlock()->GetBlockId() << " has "
          << predecessors.Size() << " predecessors.";
    errors_.Insert(error.str());
  } else {
    // Ensure phi input at index I either comes from the Ith
    // predecessor or from a block that dominates this predecessor.
    for (size_t i = 0, e = phi->InputCount(); i < e; ++i) {
      HInstruction* input = phi->InputAt(i);
      HBasicBlock* predecessor = predecessors.Get(i);
      if (!(input->GetBlock() == predecessor
            || input->GetBlock()->Dominates(predecessor))) {
        std::stringstream error;
        error << "Input " << input->GetId() << " at index " << i
              << " of phi " << phi->GetId()
              << " from block " << phi->GetBlock()->GetBlockId()
              << " is not defined in predecessor number " << i
              << " nor in a block dominating it.";
        errors_.Insert(error.str());
      }
    }
  }
}

}  // namespace art
