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

#include "instruction_simplifier.h"

namespace art {

class InstructionSimplifierVisitor : public HGraphVisitor {
 public:
  explicit InstructionSimplifierVisitor(HGraph* graph) : HGraphVisitor(graph) {}

 private:
  void VisitSuspendCheck(HSuspendCheck* check) OVERRIDE;
  void VisitEqual(HEqual* equal) OVERRIDE;
  void VisitArraySet(HArraySet* equal) OVERRIDE;
  void VisitTypeConversion(HTypeConversion* instruction) OVERRIDE;
};

void InstructionSimplifier::Run() {
  InstructionSimplifierVisitor visitor(graph_);
  visitor.VisitInsertionOrder();
}

void InstructionSimplifierVisitor::VisitSuspendCheck(HSuspendCheck* check) {
  HBasicBlock* block = check->GetBlock();
  // Currently always keep the suspend check at entry.
  if (block->IsEntryBlock()) return;

  // Currently always keep suspend checks at loop entry.
  if (block->IsLoopHeader() && block->GetFirstInstruction() == check) {
    DCHECK(block->GetLoopInformation()->GetSuspendCheck() == check);
    return;
  }

  // Remove the suspend check that was added at build time for the baseline
  // compiler.
  block->RemoveInstruction(check);
}

void InstructionSimplifierVisitor::VisitEqual(HEqual* equal) {
  HInstruction* input1 = equal->InputAt(0);
  HInstruction* input2 = equal->InputAt(1);
  if (input1->GetType() == Primitive::kPrimBoolean && input2->IsIntConstant()) {
    if (input2->AsIntConstant()->GetValue() == 1) {
      // Replace (bool_value == 1) with bool_value
      equal->ReplaceWith(equal->InputAt(0));
      equal->GetBlock()->RemoveInstruction(equal);
    } else {
      // Replace (bool_value == 0) with !bool_value
      DCHECK_EQ(input2->AsIntConstant()->GetValue(), 0);
      equal->GetBlock()->ReplaceAndRemoveInstructionWith(
          equal, new (GetGraph()->GetArena()) HNot(Primitive::kPrimBoolean, input1));
    }
  }
}

void InstructionSimplifierVisitor::VisitArraySet(HArraySet* instruction) {
  HInstruction* value = instruction->GetValue();
  if (value->GetType() != Primitive::kPrimNot) return;

  if (value->IsArrayGet()) {
    if (value->AsArrayGet()->GetArray() == instruction->GetArray()) {
      // If the code is just swapping elements in the array, no need for a type check.
      instruction->ClearNeedsTypeCheck();
    }
  }
}

void InstructionSimplifierVisitor::VisitTypeConversion(HTypeConversion* instruction) {
  if (instruction->GetResultType() == instruction->GetInputType()) {
    // Remove the instruction if it's converting to the same type.
    instruction->ReplaceWith(instruction->GetInput());
    instruction->GetBlock()->RemoveInstruction(instruction);
  }
}

}  // namespace art
