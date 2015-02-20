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

#include "mirror/class-inl.h"
#include "scoped_thread_state_change.h"

namespace art {

class InstructionSimplifierVisitor : public HGraphVisitor {
 public:
  InstructionSimplifierVisitor(HGraph* graph, OptimizingCompilerStats* stats)
      : HGraphVisitor(graph), stats_(stats) {}

 private:
  void VisitSuspendCheck(HSuspendCheck* check) OVERRIDE;
  void VisitEqual(HEqual* equal) OVERRIDE;
  void VisitArraySet(HArraySet* equal) OVERRIDE;
  void VisitTypeConversion(HTypeConversion* instruction) OVERRIDE;
  void VisitNullCheck(HNullCheck* instruction) OVERRIDE;
  void VisitArrayLength(HArrayLength* instruction) OVERRIDE;
  void VisitCheckCast(HCheckCast* instruction) OVERRIDE;

  OptimizingCompilerStats* stats_;
};

void InstructionSimplifier::Run() {
  InstructionSimplifierVisitor visitor(graph_, stats_);
  visitor.VisitInsertionOrder();
}

void InstructionSimplifierVisitor::VisitNullCheck(HNullCheck* null_check) {
  HInstruction* obj = null_check->InputAt(0);
  if (!obj->CanBeNull()) {
    null_check->ReplaceWith(obj);
    null_check->GetBlock()->RemoveInstruction(null_check);
    if (stats_ != nullptr) {
      stats_->RecordStat(MethodCompilationStat::kRemovedNullCheck);
    }
  }
}

void InstructionSimplifierVisitor::VisitCheckCast(HCheckCast* check_cast) {
  HLoadClass* load_class = check_cast->InputAt(1)->AsLoadClass();
  if (!load_class->IsResolved()) {
    // If the class couldn't be resolve it's not safe to compare against it. It's
    // default type would be Top which might be wider that the actual class type
    // and thus producing wrong results.
    return;
  }
  ReferenceTypeInfo obj_rti = check_cast->InputAt(0)->GetReferenceTypeInfo();
  ReferenceTypeInfo class_rti = load_class->GetLoadedClassRTI();
  ScopedObjectAccess soa(Thread::Current());
  if (class_rti.IsSupertypeOf(obj_rti)) {
    check_cast->GetBlock()->RemoveInstruction(check_cast);
    if (stats_ != nullptr) {
      stats_->RecordStat(MethodCompilationStat::kRemovedCheckedCast);
    }
  }
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
      // We should replace (bool_value == 0) with !bool_value, but we unfortunately
      // do not have such instruction.
      DCHECK_EQ(input2->AsIntConstant()->GetValue(), 0);
    }
  }
}

void InstructionSimplifierVisitor::VisitArrayLength(HArrayLength* instruction) {
  HInstruction* input = instruction->InputAt(0);
  // If the array is a NewArray with constant size, replace the array length
  // with the constant instruction. This helps the bounds check elimination phase.
  if (input->IsNewArray()) {
    input = input->InputAt(0);
    if (input->IsIntConstant()) {
      instruction->ReplaceWith(input);
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
