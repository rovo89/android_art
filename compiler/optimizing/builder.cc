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
  graph_ = new (arena_) HGraph(arena_);

  entry_block_ = new (arena_) HBasicBlock(graph_);
  graph_->AddBlock(entry_block_);

  exit_block_ = new (arena_) HBasicBlock(graph_);
  // The exit block is added at the end of this method to ensure
  // its id is the greatest. This is needed for dominator computation.

  entry_block_->AddInstruction(new (arena_) HGoto(entry_block_));

  current_block_ = new (arena_) HBasicBlock(graph_);
  graph_->AddBlock(current_block_);
  entry_block_->AddSuccessor(current_block_);

  while (code_ptr < code_end) {
    const Instruction& instruction = *Instruction::At(code_ptr);
    if (!AnalyzeDexInstruction(instruction)) return nullptr;
    code_ptr += instruction.SizeInCodeUnits();
  }

  exit_block_->AddInstruction(new (arena_) HExit(exit_block_));
  graph_->AddBlock(exit_block_);
  return graph_;
}

bool HGraphBuilder::AnalyzeDexInstruction(const Instruction& instruction) {
  switch (instruction.Opcode()) {
    case Instruction::RETURN_VOID:
      current_block_->AddInstruction(new (arena_) HReturnVoid(current_block_));
      current_block_->AddSuccessor(exit_block_);
      current_block_ = nullptr;
      break;
    default:
      return false;
  }
  return true;
}

}  // namespace art
