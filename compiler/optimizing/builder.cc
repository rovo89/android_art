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

#include "dex_file.h"
#include "dex_instruction.h"
#include "dex_instruction-inl.h"
#include "builder.h"
#include "nodes.h"

namespace art {

void HGraphBuilder::InitializeLocals(int count) {
  locals_.SetSize(count);
  for (int i = 0; i < count; i++) {
    HLocal* local = new (arena_) HLocal(i);
    entry_block_->AddInstruction(local);
    locals_.Put(i, local);
  }
}

static bool CanHandleCodeItem(const DexFile::CodeItem& code_item) {
  if (code_item.tries_size_ > 0) {
    return false;
  } else if (code_item.outs_size_ > 0) {
    return false;
  } else if (code_item.ins_size_ > 0) {
    return false;
  }
  return true;
}

HGraph* HGraphBuilder::BuildGraph(const DexFile::CodeItem& code_item) {
  if (!CanHandleCodeItem(code_item)) {
    return nullptr;
  }

  const uint16_t* code_ptr = code_item.insns_;
  const uint16_t* code_end = code_item.insns_ + code_item.insns_size_in_code_units_;

  // Setup the graph with the entry block and exit block.
  graph_ = new (arena_) HGraph(arena_);
  entry_block_ = new (arena_) HBasicBlock(graph_);
  graph_->AddBlock(entry_block_);
  exit_block_ = new (arena_) HBasicBlock(graph_);
  graph_->SetEntryBlock(entry_block_);
  graph_->SetExitBlock(exit_block_);

  InitializeLocals(code_item.registers_size_);

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
  exit_block_->AddInstruction(new (arena_) HExit());
  entry_block_->AddInstruction(new (arena_) HGoto());
  return graph_;
}

void HGraphBuilder::MaybeUpdateCurrentBlock(size_t index) {
  HBasicBlock* block = FindBlockStartingAt(index);
  if (block == nullptr) {
    return;
  }

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
  if (current_block_ == nullptr) {
    return true;  // Dead code
  }

  switch (instruction.Opcode()) {
    case Instruction::CONST_4: {
      int32_t register_index = instruction.VRegA();
      HIntConstant* constant = GetConstant(instruction.VRegB_11n());
      UpdateLocal(register_index, constant);
      break;
    }

    case Instruction::RETURN_VOID: {
      current_block_->AddInstruction(new (arena_) HReturnVoid());
      current_block_->AddSuccessor(exit_block_);
      current_block_ = nullptr;
      break;
    }

    case Instruction::IF_EQ: {
      HInstruction* first = LoadLocal(instruction.VRegA());
      HInstruction* second = LoadLocal(instruction.VRegB());
      current_block_->AddInstruction(new (arena_) HEqual(first, second));
      current_block_->AddInstruction(new (arena_) HIf(current_block_->GetLastInstruction()));
      HBasicBlock* target = FindBlockStartingAt(instruction.GetTargetOffset() + dex_offset);
      DCHECK(target != nullptr);
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

    case Instruction::RETURN: {
      HInstruction* value = LoadLocal(instruction.VRegA());
      current_block_->AddInstruction(new (arena_) HReturn(value));
      current_block_->AddSuccessor(exit_block_);
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

HIntConstant* HGraphBuilder::GetConstant0() {
  if (constant0_ != nullptr) {
    return constant0_;
  }
  constant0_ = new(arena_) HIntConstant(0);
  entry_block_->AddInstruction(constant0_);
  return constant0_;
}

HIntConstant* HGraphBuilder::GetConstant1() {
  if (constant1_ != nullptr) {
    return constant1_;
  }
  constant1_ = new(arena_) HIntConstant(1);
  entry_block_->AddInstruction(constant1_);
  return constant1_;
}

HIntConstant* HGraphBuilder::GetConstant(int constant) {
  switch (constant) {
    case 0: return GetConstant0();
    case 1: return GetConstant1();
    default: {
      HIntConstant* instruction = new (arena_) HIntConstant(constant);
      entry_block_->AddInstruction(instruction);
      return instruction;
    }
  }
}

HLocal* HGraphBuilder::GetLocalAt(int register_index) const {
  return locals_.Get(register_index);
}

void HGraphBuilder::UpdateLocal(int register_index, HInstruction* instruction) const {
  HLocal* local = GetLocalAt(register_index);
  current_block_->AddInstruction(new (arena_) HStoreLocal(local, instruction));
}

HInstruction* HGraphBuilder::LoadLocal(int register_index) const {
  HLocal* local = GetLocalAt(register_index);
  current_block_->AddInstruction(new (arena_) HLoadLocal(local));
  return current_block_->GetLastInstruction();
}

}  // namespace art
