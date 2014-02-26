/*
 *
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

#include "dex_instruction.h"
#include "builder.h"
#include "nodes.h"

namespace art {

HGraph* HGraphBuilder::BuildGraph(const uint16_t* code_ptr, const uint16_t* code_end) {
  // Setup the graph with the entry block and exit block.
  graph_ = new (arena_) HGraph(arena_);
  entry_block_ = new (arena_) HBasicBlock(graph_);
  graph_->AddBlock(entry_block_);
  entry_block_->AddInstruction(new (arena_) HGoto());
  exit_block_ = new (arena_) HBasicBlock(graph_);
  exit_block_->AddInstruction(new (arena_) HExit());

  // To avoid splitting blocks, we compute ahead of time the instructions that
  // start a new block, and create these blocks.
  ComputeBranchTargets(code_ptr, code_end);

  size_t dex_offset = 0;
  while (code_ptr < code_end) {
    // Update the current block if dex_offset starts a new block.
    MaybeUpdateCurrentBlock(dex_offset);
    const Instruction& instruction = *Instruction::At(code_ptr);
    if (!AnalyzeDexInstruction(instruction, dex_offset)) return nullptr;
    dex_offset += instruction.SizeInCodeUnits();
    code_ptr += instruction.SizeInCodeUnits();
  }

  // Add the exit block at the end to give it the highest id.
  graph_->AddBlock(exit_block_);
  return graph_;
}

void HGraphBuilder::MaybeUpdateCurrentBlock(size_t index) {
  HBasicBlock* block = FindBlockStartingAt(index);
  if (block == nullptr) return;

  if (current_block_ != nullptr) {
    // Branching instructions clear current_block, so we know
    // the last instruction of the current block is not a branching
    // instruction. We add an unconditional goto to the found block.
    current_block_->AddInstruction(new (arena_) HGoto());
    current_block_->AddSuccessor(block);
  }
  graph_->AddBlock(block);
  current_block_ = block;
}

void HGraphBuilder::ComputeBranchTargets(const uint16_t* code_ptr, const uint16_t* code_end) {
  // TODO: Support switch instructions.
  branch_targets_.SetSize(code_end - code_ptr);

  // Create the first block for the dex instructions, single successor of the entry block.
  HBasicBlock* block = new (arena_) HBasicBlock(graph_);
  branch_targets_.Put(0, block);
  entry_block_->AddSuccessor(block);

  // Iterate over all instructions and find branching instructions. Create blocks for
  // the locations these instructions branch to.
  size_t dex_offset = 0;
  while (code_ptr < code_end) {
    const Instruction& instruction = *Instruction::At(code_ptr);
    if (instruction.IsBranch()) {
      int32_t target = instruction.GetTargetOffset() + dex_offset;
      // Create a block for the target instruction.
      if (FindBlockStartingAt(target) == nullptr) {
        block = new (arena_) HBasicBlock(graph_);
        branch_targets_.Put(target, block);
      }
      dex_offset += instruction.SizeInCodeUnits();
      code_ptr += instruction.SizeInCodeUnits();
      if ((code_ptr < code_end) && (FindBlockStartingAt(dex_offset) == nullptr)) {
        block = new (arena_) HBasicBlock(graph_);
        branch_targets_.Put(dex_offset, block);
      }
    } else {
      code_ptr += instruction.SizeInCodeUnits();
      dex_offset += instruction.SizeInCodeUnits();
    }
  }
}

HBasicBlock* HGraphBuilder::FindBlockStartingAt(int32_t index) const {
  DCHECK_GE(index, 0);
  return branch_targets_.Get(index);
}

bool HGraphBuilder::AnalyzeDexInstruction(const Instruction& instruction, int32_t dex_offset) {
  if (current_block_ == nullptr) return true;  // Dead code

  switch (instruction.Opcode()) {
    case Instruction::RETURN_VOID:
      current_block_->AddInstruction(new (arena_) HReturnVoid());
      current_block_->AddSuccessor(exit_block_);
      current_block_ = nullptr;
      break;
    case Instruction::IF_EQ: {
      // TODO: Read the dex register.
      HBasicBlock* target = FindBlockStartingAt(instruction.GetTargetOffset() + dex_offset);
      DCHECK(target != nullptr);
      current_block_->AddInstruction(new (arena_) HIf());
      current_block_->AddSuccessor(target);
      target = FindBlockStartingAt(dex_offset + instruction.SizeInCodeUnits());
      DCHECK(target != nullptr);
      current_block_->AddSuccessor(target);
      current_block_ = nullptr;
      break;
    }
    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32: {
      HBasicBlock* target = FindBlockStartingAt(instruction.GetTargetOffset() + dex_offset);
      DCHECK(target != nullptr);
      current_block_->AddInstruction(new (arena_) HGoto());
      current_block_->AddSuccessor(target);
      current_block_ = nullptr;
      break;
    }
    case Instruction::NOP:
      break;
    default:
      return false;
  }
  return true;
}

}  // namespace art
