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

#include "code_generator.h"

#include "code_generator_arm.h"
#include "code_generator_x86.h"
#include "utils/assembler.h"
#include "utils/arm/assembler_arm.h"
#include "utils/mips/assembler_mips.h"
#include "utils/x86/assembler_x86.h"

namespace art {

void CodeGenerator::Compile(CodeAllocator* allocator) {
  GenerateFrameEntry();
  const GrowableArray<HBasicBlock*>* blocks = graph()->blocks();
  for (size_t i = 0; i < blocks->Size(); i++) {
    CompileBlock(blocks->Get(i));
  }
  size_t code_size = assembler_->CodeSize();
  uint8_t* buffer = allocator->Allocate(code_size);
  MemoryRegion code(buffer, code_size);
  assembler_->FinalizeInstructions(code);
}

void CodeGenerator::CompileBlock(HBasicBlock* block) {
  Bind(GetLabelOf(block));
  for (HInstructionIterator it(block); !it.Done(); it.Advance()) {
    it.Current()->Accept(this);
  }
}

bool CodeGenerator::GoesToNextBlock(HGoto* goto_instruction) const {
  HBasicBlock* successor = goto_instruction->GetSuccessor();
  // We currently iterate over the block in insertion order.
  return goto_instruction->block()->block_id() + 1 == successor->block_id();
}

Label* CodeGenerator::GetLabelOf(HBasicBlock* block) const {
  return block_labels_.GetRawStorage() + block->block_id();
}

bool CodeGenerator::CompileGraph(HGraph* graph,
                                 InstructionSet instruction_set,
                                 CodeAllocator* allocator) {
  switch (instruction_set) {
    case kArm:
    case kThumb2: {
      arm::ArmAssembler assembler;
      arm::CodeGeneratorARM(&assembler, graph).Compile(allocator);
      return true;
    }
    case kMips:
      return false;
    case kX86: {
      x86::X86Assembler assembler;
      x86::CodeGeneratorX86(&assembler, graph).Compile(allocator);
      return true;
    }
    default:
      return false;
  }
}

}  // namespace art
