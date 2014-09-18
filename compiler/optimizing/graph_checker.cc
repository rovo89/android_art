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
}

void SSAChecker::VisitInstruction(HInstruction* instruction) {
  super_type::VisitInstruction(instruction);

  // Ensure an instruction dominates all its uses (or in the present
  // case, that all uses of an instruction (used as input) are
  // dominated by its definition).
  for (HInputIterator input_it(instruction); !input_it.Done();
       input_it.Advance()) {
    HInstruction* input = input_it.Current();
    if (!input->Dominates(instruction)) {
      std::stringstream error;
      error << "Instruction " << input->GetId()
            << " in block " << input->GetBlock()->GetBlockId()
            << " does not dominate use " << instruction->GetId()
            << " in block " << current_block_->GetBlockId() << ".";
      errors_.Insert(error.str());
    }
  }
}

}  // namespace art
