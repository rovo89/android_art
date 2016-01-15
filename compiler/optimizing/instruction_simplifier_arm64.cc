/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "instruction_simplifier_arm64.h"

#include "common_arm64.h"
#include "mirror/array-inl.h"

namespace art {
namespace arm64 {

using helpers::CanFitInShifterOperand;
using helpers::HasShifterOperand;
using helpers::ShifterOperandSupportsExtension;

void InstructionSimplifierArm64Visitor::TryExtractArrayAccessAddress(HInstruction* access,
                                                                     HInstruction* array,
                                                                     HInstruction* index,
                                                                     int access_size) {
  if (kEmitCompilerReadBarrier) {
    // The read barrier instrumentation does not support the
    // HArm64IntermediateAddress instruction yet.
    //
    // TODO: Handle this case properly in the ARM64 code generator and
    // re-enable this optimization; otherwise, remove this TODO.
    // b/26601270
    return;
  }
  if (index->IsConstant() ||
      (index->IsBoundsCheck() && index->AsBoundsCheck()->GetIndex()->IsConstant())) {
    // When the index is a constant all the addressing can be fitted in the
    // memory access instruction, so do not split the access.
    return;
  }
  if (access->IsArraySet() &&
      access->AsArraySet()->GetValue()->GetType() == Primitive::kPrimNot) {
    // The access may require a runtime call or the original array pointer.
    return;
  }

  // Proceed to extract the base address computation.
  ArenaAllocator* arena = GetGraph()->GetArena();

  HIntConstant* offset =
      GetGraph()->GetIntConstant(mirror::Array::DataOffset(access_size).Uint32Value());
  HArm64IntermediateAddress* address =
      new (arena) HArm64IntermediateAddress(array, offset, kNoDexPc);
  address->SetReferenceTypeInfo(array->GetReferenceTypeInfo());
  access->GetBlock()->InsertInstructionBefore(address, access);
  access->ReplaceInput(address, 0);
  // Both instructions must depend on GC to prevent any instruction that can
  // trigger GC to be inserted between the two.
  access->AddSideEffects(SideEffects::DependsOnGC());
  DCHECK(address->GetSideEffects().Includes(SideEffects::DependsOnGC()));
  DCHECK(access->GetSideEffects().Includes(SideEffects::DependsOnGC()));
  // TODO: Code generation for HArrayGet and HArraySet will check whether the input address
  // is an HArm64IntermediateAddress and generate appropriate code.
  // We would like to replace the `HArrayGet` and `HArraySet` with custom instructions (maybe
  // `HArm64Load` and `HArm64Store`). We defer these changes because these new instructions would
  // not bring any advantages yet.
  // Also see the comments in
  // `InstructionCodeGeneratorARM64::VisitArrayGet()` and
  // `InstructionCodeGeneratorARM64::VisitArraySet()`.
  RecordSimplification();
}

bool InstructionSimplifierArm64Visitor::TryMergeIntoShifterOperand(HInstruction* use,
                                                                   HInstruction* bitfield_op,
                                                                   bool do_merge) {
  DCHECK(HasShifterOperand(use));
  DCHECK(use->IsBinaryOperation() || use->IsNeg());
  DCHECK(CanFitInShifterOperand(bitfield_op));
  DCHECK(!bitfield_op->HasEnvironmentUses());

  Primitive::Type type = use->GetType();
  if (type != Primitive::kPrimInt && type != Primitive::kPrimLong) {
    return false;
  }

  HInstruction* left;
  HInstruction* right;
  if (use->IsBinaryOperation()) {
    left = use->InputAt(0);
    right = use->InputAt(1);
  } else {
    DCHECK(use->IsNeg());
    right = use->AsNeg()->InputAt(0);
    left = GetGraph()->GetConstant(right->GetType(), 0);
  }
  DCHECK(left == bitfield_op || right == bitfield_op);

  if (left == right) {
    // TODO: Handle special transformations in this situation?
    // For example should we transform `(x << 1) + (x << 1)` into `(x << 2)`?
    // Or should this be part of a separate transformation logic?
    return false;
  }

  bool is_commutative = use->IsBinaryOperation() && use->AsBinaryOperation()->IsCommutative();
  HInstruction* other_input;
  if (bitfield_op == right) {
    other_input = left;
  } else {
    if (is_commutative) {
      other_input = right;
    } else {
      return false;
    }
  }

  HArm64DataProcWithShifterOp::OpKind op_kind;
  int shift_amount = 0;
  HArm64DataProcWithShifterOp::GetOpInfoFromInstruction(bitfield_op, &op_kind, &shift_amount);

  if (HArm64DataProcWithShifterOp::IsExtensionOp(op_kind) &&
      !ShifterOperandSupportsExtension(use)) {
    return false;
  }

  if (do_merge) {
    HArm64DataProcWithShifterOp* alu_with_op =
        new (GetGraph()->GetArena()) HArm64DataProcWithShifterOp(use,
                                                                 other_input,
                                                                 bitfield_op->InputAt(0),
                                                                 op_kind,
                                                                 shift_amount,
                                                                 use->GetDexPc());
    use->GetBlock()->ReplaceAndRemoveInstructionWith(use, alu_with_op);
    if (bitfield_op->GetUses().IsEmpty()) {
      bitfield_op->GetBlock()->RemoveInstruction(bitfield_op);
    }
    RecordSimplification();
  }

  return true;
}

// Merge a bitfield move instruction into its uses if it can be merged in all of them.
bool InstructionSimplifierArm64Visitor::TryMergeIntoUsersShifterOperand(HInstruction* bitfield_op) {
  DCHECK(CanFitInShifterOperand(bitfield_op));

  if (bitfield_op->HasEnvironmentUses()) {
    return false;
  }

  const HUseList<HInstruction*>& uses = bitfield_op->GetUses();

  // Check whether we can merge the instruction in all its users' shifter operand.
  for (HUseIterator<HInstruction*> it_use(uses); !it_use.Done(); it_use.Advance()) {
    HInstruction* use = it_use.Current()->GetUser();
    if (!HasShifterOperand(use)) {
      return false;
    }
    if (!CanMergeIntoShifterOperand(use, bitfield_op)) {
      return false;
    }
  }

  // Merge the instruction into its uses.
  for (HUseIterator<HInstruction*> it_use(uses); !it_use.Done(); it_use.Advance()) {
    HInstruction* use = it_use.Current()->GetUser();
    bool merged = MergeIntoShifterOperand(use, bitfield_op);
    DCHECK(merged);
  }

  return true;
}

bool InstructionSimplifierArm64Visitor::TrySimpleMultiplyAccumulatePatterns(
    HMul* mul, HBinaryOperation* input_binop, HInstruction* input_other) {
  DCHECK(Primitive::IsIntOrLongType(mul->GetType()));
  DCHECK(input_binop->IsAdd() || input_binop->IsSub());
  DCHECK_NE(input_binop, input_other);
  if (!input_binop->HasOnlyOneNonEnvironmentUse()) {
    return false;
  }

  // Try to interpret patterns like
  //    a * (b <+/-> 1)
  // as
  //    (a * b) <+/-> a
  HInstruction* input_a = input_other;
  HInstruction* input_b = nullptr;  // Set to a non-null value if we found a pattern to optimize.
  HInstruction::InstructionKind op_kind;

  if (input_binop->IsAdd()) {
    if ((input_binop->GetConstantRight() != nullptr) && input_binop->GetConstantRight()->IsOne()) {
      // Interpret
      //    a * (b + 1)
      // as
      //    (a * b) + a
      input_b = input_binop->GetLeastConstantLeft();
      op_kind = HInstruction::kAdd;
    }
  } else {
    DCHECK(input_binop->IsSub());
    if (input_binop->GetRight()->IsConstant() &&
        input_binop->GetRight()->AsConstant()->IsMinusOne()) {
      // Interpret
      //    a * (b - (-1))
      // as
      //    a + (a * b)
      input_b = input_binop->GetLeft();
      op_kind = HInstruction::kAdd;
    } else if (input_binop->GetLeft()->IsConstant() &&
               input_binop->GetLeft()->AsConstant()->IsOne()) {
      // Interpret
      //    a * (1 - b)
      // as
      //    a - (a * b)
      input_b = input_binop->GetRight();
      op_kind = HInstruction::kSub;
    }
  }

  if (input_b == nullptr) {
    // We did not find a pattern we can optimize.
    return false;
  }

  HArm64MultiplyAccumulate* mulacc = new(GetGraph()->GetArena()) HArm64MultiplyAccumulate(
      mul->GetType(), op_kind, input_a, input_a, input_b, mul->GetDexPc());

  mul->GetBlock()->ReplaceAndRemoveInstructionWith(mul, mulacc);
  input_binop->GetBlock()->RemoveInstruction(input_binop);

  return false;
}

void InstructionSimplifierArm64Visitor::VisitArrayGet(HArrayGet* instruction) {
  TryExtractArrayAccessAddress(instruction,
                               instruction->GetArray(),
                               instruction->GetIndex(),
                               Primitive::ComponentSize(instruction->GetType()));
}

void InstructionSimplifierArm64Visitor::VisitArraySet(HArraySet* instruction) {
  TryExtractArrayAccessAddress(instruction,
                               instruction->GetArray(),
                               instruction->GetIndex(),
                               Primitive::ComponentSize(instruction->GetComponentType()));
}

void InstructionSimplifierArm64Visitor::VisitMul(HMul* instruction) {
  Primitive::Type type = instruction->GetType();
  if (!Primitive::IsIntOrLongType(type)) {
    return;
  }

  HInstruction* use = instruction->HasNonEnvironmentUses()
      ? instruction->GetUses().GetFirst()->GetUser()
      : nullptr;

  if (instruction->HasOnlyOneNonEnvironmentUse() && (use->IsAdd() || use->IsSub())) {
    // Replace code looking like
    //    MUL tmp, x, y
    //    SUB dst, acc, tmp
    // with
    //    MULSUB dst, acc, x, y
    // Note that we do not want to (unconditionally) perform the merge when the
    // multiplication has multiple uses and it can be merged in all of them.
    // Multiple uses could happen on the same control-flow path, and we would
    // then increase the amount of work. In the future we could try to evaluate
    // whether all uses are on different control-flow paths (using dominance and
    // reverse-dominance information) and only perform the merge when they are.
    HInstruction* accumulator = nullptr;
    HBinaryOperation* binop = use->AsBinaryOperation();
    HInstruction* binop_left = binop->GetLeft();
    HInstruction* binop_right = binop->GetRight();
    // Be careful after GVN. This should not happen since the `HMul` has only
    // one use.
    DCHECK_NE(binop_left, binop_right);
    if (binop_right == instruction) {
      accumulator = binop_left;
    } else if (use->IsAdd()) {
      DCHECK_EQ(binop_left, instruction);
      accumulator = binop_right;
    }

    if (accumulator != nullptr) {
      HArm64MultiplyAccumulate* mulacc =
          new (GetGraph()->GetArena()) HArm64MultiplyAccumulate(type,
                                                                binop->GetKind(),
                                                                accumulator,
                                                                instruction->GetLeft(),
                                                                instruction->GetRight());

      binop->GetBlock()->ReplaceAndRemoveInstructionWith(binop, mulacc);
      DCHECK(!instruction->HasUses());
      instruction->GetBlock()->RemoveInstruction(instruction);
      RecordSimplification();
      return;
    }
  }

  // Use multiply accumulate instruction for a few simple patterns.
  // We prefer not applying the following transformations if the left and
  // right inputs perform the same operation.
  // We rely on GVN having squashed the inputs if appropriate. However the
  // results are still correct even if that did not happen.
  if (instruction->GetLeft() == instruction->GetRight()) {
    return;
  }

  HInstruction* left = instruction->GetLeft();
  HInstruction* right = instruction->GetRight();
  if ((right->IsAdd() || right->IsSub()) &&
      TrySimpleMultiplyAccumulatePatterns(instruction, right->AsBinaryOperation(), left)) {
    return;
  }
  if ((left->IsAdd() || left->IsSub()) &&
      TrySimpleMultiplyAccumulatePatterns(instruction, left->AsBinaryOperation(), right)) {
    return;
  }
}

void InstructionSimplifierArm64Visitor::VisitShl(HShl* instruction) {
  if (instruction->InputAt(1)->IsConstant()) {
    TryMergeIntoUsersShifterOperand(instruction);
  }
}

void InstructionSimplifierArm64Visitor::VisitShr(HShr* instruction) {
  if (instruction->InputAt(1)->IsConstant()) {
    TryMergeIntoUsersShifterOperand(instruction);
  }
}

void InstructionSimplifierArm64Visitor::VisitTypeConversion(HTypeConversion* instruction) {
  Primitive::Type result_type = instruction->GetResultType();
  Primitive::Type input_type = instruction->GetInputType();

  if (input_type == result_type) {
    // We let the arch-independent code handle this.
    return;
  }

  if (Primitive::IsIntegralType(result_type) && Primitive::IsIntegralType(input_type)) {
    TryMergeIntoUsersShifterOperand(instruction);
  }
}

void InstructionSimplifierArm64Visitor::VisitUShr(HUShr* instruction) {
  if (instruction->InputAt(1)->IsConstant()) {
    TryMergeIntoUsersShifterOperand(instruction);
  }
}

}  // namespace arm64
}  // namespace art
