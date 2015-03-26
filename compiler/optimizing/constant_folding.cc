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

#include "constant_folding.h"

namespace art {

// This visitor tries to simplify operations that yield a constant. For example
// `input * 0` is replaced by a null constant.
class InstructionWithAbsorbingInputSimplifier : public HGraphVisitor {
 public:
  explicit InstructionWithAbsorbingInputSimplifier(HGraph* graph) : HGraphVisitor(graph) {}

 private:
  void VisitShift(HBinaryOperation* shift);

  void VisitAnd(HAnd* instruction) OVERRIDE;
  void VisitMul(HMul* instruction) OVERRIDE;
  void VisitOr(HOr* instruction) OVERRIDE;
  void VisitRem(HRem* instruction) OVERRIDE;
  void VisitShl(HShl* instruction) OVERRIDE;
  void VisitShr(HShr* instruction) OVERRIDE;
  void VisitSub(HSub* instruction) OVERRIDE;
  void VisitUShr(HUShr* instruction) OVERRIDE;
  void VisitXor(HXor* instruction) OVERRIDE;
};

void HConstantFolding::Run() {
  InstructionWithAbsorbingInputSimplifier simplifier(graph_);
  // Process basic blocks in reverse post-order in the dominator tree,
  // so that an instruction turned into a constant, used as input of
  // another instruction, may possibly be used to turn that second
  // instruction into a constant as well.
  for (HReversePostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    // Traverse this block's instructions in (forward) order and
    // replace the ones that can be statically evaluated by a
    // compile-time counterpart.
    for (HInstructionIterator inst_it(block->GetInstructions());
         !inst_it.Done(); inst_it.Advance()) {
      HInstruction* inst = inst_it.Current();
      if (inst->IsBinaryOperation()) {
        // Constant folding: replace `op(a, b)' with a constant at
        // compile time if `a' and `b' are both constants.
        HConstant* constant = inst->AsBinaryOperation()->TryStaticEvaluation();
        if (constant != nullptr) {
          inst->ReplaceWith(constant);
          inst->GetBlock()->RemoveInstruction(inst);
        } else {
          inst->Accept(&simplifier);
        }
      } else if (inst->IsUnaryOperation()) {
        // Constant folding: replace `op(a)' with a constant at compile
        // time if `a' is a constant.
        HConstant* constant = inst->AsUnaryOperation()->TryStaticEvaluation();
        if (constant != nullptr) {
          inst->ReplaceWith(constant);
          inst->GetBlock()->RemoveInstruction(inst);
        }
      } else if (inst->IsDivZeroCheck()) {
        // We can safely remove the check if the input is a non-null constant.
        HDivZeroCheck* check = inst->AsDivZeroCheck();
        HInstruction* check_input = check->InputAt(0);
        if (check_input->IsConstant() && !check_input->AsConstant()->IsZero()) {
          check->ReplaceWith(check_input);
          check->GetBlock()->RemoveInstruction(check);
        }
      }
    }
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitShift(HBinaryOperation* instruction) {
  DCHECK(instruction->IsShl() || instruction->IsShr() || instruction->IsUShr());
  HInstruction* left = instruction->GetLeft();
  if (left->IsConstant() && left->AsConstant()->IsZero()) {
    // Replace code looking like
    //    SHL dst, 0, shift_amount
    // with
    //    CONSTANT 0
    instruction->ReplaceWith(left);
    instruction->GetBlock()->RemoveInstruction(instruction);
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitAnd(HAnd* instruction) {
  HConstant* input_cst = instruction->GetConstantRight();
  if ((input_cst != nullptr) && input_cst->IsZero()) {
    // Replace code looking like
    //    AND dst, src, 0
    // with
    //    CONSTANT 0
    instruction->ReplaceWith(input_cst);
    instruction->GetBlock()->RemoveInstruction(instruction);
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitMul(HMul* instruction) {
  HConstant* input_cst = instruction->GetConstantRight();
  Primitive::Type type = instruction->GetType();
  if (Primitive::IsIntOrLongType(type) &&
      (input_cst != nullptr) && input_cst->IsZero()) {
    // Replace code looking like
    //    MUL dst, src, 0
    // with
    //    CONSTANT 0
    // Integral multiplication by zero always yields zero, but floating-point
    // multiplication by zero does not always do. For example `Infinity * 0.0`
    // should yield a NaN.
    instruction->ReplaceWith(input_cst);
    instruction->GetBlock()->RemoveInstruction(instruction);
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitOr(HOr* instruction) {
  HConstant* input_cst = instruction->GetConstantRight();

  if (input_cst == nullptr) {
    return;
  }

  if (Int64FromConstant(input_cst) == -1) {
    // Replace code looking like
    //    OR dst, src, 0xFFF...FF
    // with
    //    CONSTANT 0xFFF...FF
    instruction->ReplaceWith(input_cst);
    instruction->GetBlock()->RemoveInstruction(instruction);
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitRem(HRem* instruction) {
  Primitive::Type type = instruction->GetType();

  if (!Primitive::IsIntegralType(type)) {
    return;
  }

  HBasicBlock* block = instruction->GetBlock();

  if (instruction->GetLeft()->IsConstant() &&
      instruction->GetLeft()->AsConstant()->IsZero()) {
    // Replace code looking like
    //    REM dst, 0, src
    // with
    //    CONSTANT 0
    instruction->ReplaceWith(instruction->GetLeft());
    block->RemoveInstruction(instruction);
  }

  HConstant* cst_right = instruction->GetRight()->AsConstant();
  if (((cst_right != nullptr) &&
       (cst_right->IsOne() || cst_right->IsMinusOne())) ||
      (instruction->GetLeft() == instruction->GetRight())) {
    // Replace code looking like
    //    REM dst, src, 1
    // or
    //    REM dst, src, -1
    // or
    //    REM dst, src, src
    // with
    //    CONSTANT 0
    instruction->ReplaceWith(GetGraph()->GetConstant(type, 0));
    block->RemoveInstruction(instruction);
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitShl(HShl* instruction) {
  VisitShift(instruction);
}

void InstructionWithAbsorbingInputSimplifier::VisitShr(HShr* instruction) {
  VisitShift(instruction);
}

void InstructionWithAbsorbingInputSimplifier::VisitSub(HSub* instruction) {
  Primitive::Type type = instruction->GetType();

  if (!Primitive::IsIntegralType(type)) {
    return;
  }

  HBasicBlock* block = instruction->GetBlock();

  // We assume that GVN has run before, so we only perform a pointer
  // comparison.  If for some reason the values are equal but the pointers are
  // different, we are still correct and only miss an optimisation
  // opportunity.
  if (instruction->GetLeft() == instruction->GetRight()) {
    // Replace code looking like
    //    SUB dst, src, src
    // with
    //    CONSTANT 0
    // Note that we cannot optimise `x - x` to `0` for floating-point. It does
    // not work when `x` is an infinity.
    instruction->ReplaceWith(GetGraph()->GetConstant(type, 0));
    block->RemoveInstruction(instruction);
  }
}

void InstructionWithAbsorbingInputSimplifier::VisitUShr(HUShr* instruction) {
  VisitShift(instruction);
}

void InstructionWithAbsorbingInputSimplifier::VisitXor(HXor* instruction) {
  if (instruction->GetLeft() == instruction->GetRight()) {
    // Replace code looking like
    //    XOR dst, src, src
    // with
    //    CONSTANT 0
    Primitive::Type type = instruction->GetType();
    HBasicBlock* block = instruction->GetBlock();
    instruction->ReplaceWith(GetGraph()->GetConstant(type, 0));
    block->RemoveInstruction(instruction);
  }
}

}  // namespace art
