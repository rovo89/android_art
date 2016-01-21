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

#include "instruction_simplifier_shared.h"

namespace art {

namespace {

bool TrySimpleMultiplyAccumulatePatterns(HMul* mul,
                                         HBinaryOperation* input_binop,
                                         HInstruction* input_other) {
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

  ArenaAllocator* arena = mul->GetBlock()->GetGraph()->GetArena();
  HMultiplyAccumulate* mulacc = new(arena) HMultiplyAccumulate(
      mul->GetType(), op_kind, input_a, input_a, input_b, mul->GetDexPc());

  mul->GetBlock()->ReplaceAndRemoveInstructionWith(mul, mulacc);
  input_binop->GetBlock()->RemoveInstruction(input_binop);

  return true;
}

}  // namespace

bool TryCombineMultiplyAccumulate(HMul* mul, InstructionSet isa) {
  Primitive::Type type = mul->GetType();
  switch (isa) {
    case kArm:
    case kThumb2:
      if (type != Primitive::kPrimInt) {
        return false;
      }
      break;
    case kArm64:
      if (!Primitive::IsIntOrLongType(type)) {
        return false;
      }
      break;
    default:
      return false;
  }

  HInstruction* use = mul->HasNonEnvironmentUses()
      ? mul->GetUses().GetFirst()->GetUser()
      : nullptr;

  ArenaAllocator* arena = mul->GetBlock()->GetGraph()->GetArena();

  if (mul->HasOnlyOneNonEnvironmentUse()) {
    if (use->IsAdd() || use->IsSub()) {
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
      if (binop_right == mul) {
        accumulator = binop_left;
      } else if (use->IsAdd()) {
        DCHECK_EQ(binop_left, mul);
        accumulator = binop_right;
      }

      if (accumulator != nullptr) {
        HMultiplyAccumulate* mulacc =
            new (arena) HMultiplyAccumulate(type,
                                            binop->GetKind(),
                                            accumulator,
                                            mul->GetLeft(),
                                            mul->GetRight());

        binop->GetBlock()->ReplaceAndRemoveInstructionWith(binop, mulacc);
        DCHECK(!mul->HasUses());
        mul->GetBlock()->RemoveInstruction(mul);
        return true;
      }
    } else if (use->IsNeg() && isa != kArm) {
      HMultiplyAccumulate* mulacc =
          new (arena) HMultiplyAccumulate(type,
                                          HInstruction::kSub,
                                          mul->GetBlock()->GetGraph()->GetConstant(type, 0),
                                          mul->GetLeft(),
                                          mul->GetRight());

      use->GetBlock()->ReplaceAndRemoveInstructionWith(use, mulacc);
      DCHECK(!mul->HasUses());
      mul->GetBlock()->RemoveInstruction(mul);
      return true;
    }
  }

  // Use multiply accumulate instruction for a few simple patterns.
  // We prefer not applying the following transformations if the left and
  // right inputs perform the same operation.
  // We rely on GVN having squashed the inputs if appropriate. However the
  // results are still correct even if that did not happen.
  if (mul->GetLeft() == mul->GetRight()) {
    return false;
  }

  HInstruction* left = mul->GetLeft();
  HInstruction* right = mul->GetRight();
  if ((right->IsAdd() || right->IsSub()) &&
      TrySimpleMultiplyAccumulatePatterns(mul, right->AsBinaryOperation(), left)) {
    return true;
  }
  if ((left->IsAdd() || left->IsSub()) &&
      TrySimpleMultiplyAccumulatePatterns(mul, left->AsBinaryOperation(), right)) {
    return true;
  }
  return false;
}

}  // namespace art
