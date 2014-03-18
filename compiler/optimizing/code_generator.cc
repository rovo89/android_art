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

namespace art {

void CodeGenerator::Compile(CodeAllocator* allocator) {
  const GrowableArray<HBasicBlock*>* blocks = GetGraph()->GetBlocks();
  DCHECK(blocks->Get(0) == GetGraph()->GetEntryBlock());
  DCHECK(GoesToNextBlock(GetGraph()->GetEntryBlock(), blocks->Get(1)));
  CompileEntryBlock();
  for (size_t i = 1; i < blocks->Size(); i++) {
    CompileBlock(blocks->Get(i));
  }
  size_t code_size = GetAssembler()->CodeSize();
  uint8_t* buffer = allocator->Allocate(code_size);
  MemoryRegion code(buffer, code_size);
  GetAssembler()->FinalizeInstructions(code);
}

void CodeGenerator::CompileEntryBlock() {
  HGraphVisitor* location_builder = GetLocationBuilder();
  HGraphVisitor* instruction_visitor = GetInstructionVisitor();
  // The entry block contains all locals for this method. By visiting the entry block,
  // we're computing the required frame size.
  for (HInstructionIterator it(GetGraph()->GetEntryBlock()); !it.Done(); it.Advance()) {
    HInstruction* current = it.Current();
    // Instructions in the entry block should not generate code.
    if (kIsDebugBuild) {
      current->Accept(location_builder);
      DCHECK(current->GetLocations() == nullptr);
    }
    current->Accept(instruction_visitor);
  }
  GenerateFrameEntry();
}

void CodeGenerator::CompileBlock(HBasicBlock* block) {
  Bind(GetLabelOf(block));
  HGraphVisitor* location_builder = GetLocationBuilder();
  HGraphVisitor* instruction_visitor = GetInstructionVisitor();
  for (HInstructionIterator it(block); !it.Done(); it.Advance()) {
    // For each instruction, we emulate a stack-based machine, where the inputs are popped from
    // the runtime stack, and the result is pushed on the stack. We currently can do this because
    // we do not perform any code motion, and the Dex format does not reference individual
    // instructions but uses registers instead (our equivalent of HLocal).
    HInstruction* current = it.Current();
    current->Accept(location_builder);
    InitLocations(current);
    current->Accept(instruction_visitor);
    if (current->GetLocations() != nullptr && current->GetLocations()->Out().IsValid()) {
      Push(current, current->GetLocations()->Out());
    }
  }
}

void CodeGenerator::InitLocations(HInstruction* instruction) {
  if (instruction->GetLocations() == nullptr) return;
  for (int i = 0; i < instruction->InputCount(); i++) {
    Location location = instruction->GetLocations()->InAt(i);
    if (location.IsValid()) {
      // Move the input to the desired location.
      Move(instruction->InputAt(i), location);
    }
  }
}

bool CodeGenerator::GoesToNextBlock(HBasicBlock* current, HBasicBlock* next) const {
  // We currently iterate over the block in insertion order.
  return current->GetBlockId() + 1 == next->GetBlockId();
}

Label* CodeGenerator::GetLabelOf(HBasicBlock* block) const {
  return block_labels_.GetRawStorage() + block->GetBlockId();
}

CodeGenerator* CodeGenerator::Create(ArenaAllocator* allocator,
                                     HGraph* graph,
                                     InstructionSet instruction_set) {
  switch (instruction_set) {
    case kArm:
    case kThumb2: {
      return new (allocator) arm::CodeGeneratorARM(graph);
    }
    case kMips:
      return nullptr;
    case kX86: {
      return new (allocator) x86::CodeGeneratorX86(graph);
    }
    default:
      return nullptr;
  }
}

}  // namespace art
