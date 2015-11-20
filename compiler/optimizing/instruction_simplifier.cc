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

#include "intrinsics.h"
#include "mirror/class-inl.h"
#include "scoped_thread_state_change.h"

namespace art {

class InstructionSimplifierVisitor : public HGraphDelegateVisitor {
 public:
  InstructionSimplifierVisitor(HGraph* graph, OptimizingCompilerStats* stats)
      : HGraphDelegateVisitor(graph),
        stats_(stats) {}

  void Run();

 private:
  void RecordSimplification() {
    simplification_occurred_ = true;
    simplifications_at_current_position_++;
    if (stats_) {
      stats_->RecordStat(kInstructionSimplifications);
    }
  }

  bool TryMoveNegOnInputsAfterBinop(HBinaryOperation* binop);
  void VisitShift(HBinaryOperation* shift);

  void VisitSuspendCheck(HSuspendCheck* check) OVERRIDE;
  void VisitEqual(HEqual* equal) OVERRIDE;
  void VisitNotEqual(HNotEqual* equal) OVERRIDE;
  void VisitBooleanNot(HBooleanNot* bool_not) OVERRIDE;
  void VisitInstanceFieldSet(HInstanceFieldSet* equal) OVERRIDE;
  void VisitStaticFieldSet(HStaticFieldSet* equal) OVERRIDE;
  void VisitArraySet(HArraySet* equal) OVERRIDE;
  void VisitTypeConversion(HTypeConversion* instruction) OVERRIDE;
  void VisitNullCheck(HNullCheck* instruction) OVERRIDE;
  void VisitArrayLength(HArrayLength* instruction) OVERRIDE;
  void VisitCheckCast(HCheckCast* instruction) OVERRIDE;
  void VisitAdd(HAdd* instruction) OVERRIDE;
  void VisitAnd(HAnd* instruction) OVERRIDE;
  void VisitCondition(HCondition* instruction) OVERRIDE;
  void VisitGreaterThan(HGreaterThan* condition) OVERRIDE;
  void VisitGreaterThanOrEqual(HGreaterThanOrEqual* condition) OVERRIDE;
  void VisitLessThan(HLessThan* condition) OVERRIDE;
  void VisitLessThanOrEqual(HLessThanOrEqual* condition) OVERRIDE;
  void VisitDiv(HDiv* instruction) OVERRIDE;
  void VisitMul(HMul* instruction) OVERRIDE;
  void VisitNeg(HNeg* instruction) OVERRIDE;
  void VisitNot(HNot* instruction) OVERRIDE;
  void VisitOr(HOr* instruction) OVERRIDE;
  void VisitShl(HShl* instruction) OVERRIDE;
  void VisitShr(HShr* instruction) OVERRIDE;
  void VisitSub(HSub* instruction) OVERRIDE;
  void VisitUShr(HUShr* instruction) OVERRIDE;
  void VisitXor(HXor* instruction) OVERRIDE;
  void VisitInstanceOf(HInstanceOf* instruction) OVERRIDE;
  void VisitFakeString(HFakeString* fake_string) OVERRIDE;
  void VisitInvoke(HInvoke* invoke) OVERRIDE;
  void VisitDeoptimize(HDeoptimize* deoptimize) OVERRIDE;

  bool CanEnsureNotNullAt(HInstruction* instr, HInstruction* at) const;

  void SimplifySystemArrayCopy(HInvoke* invoke);
  void SimplifyStringEquals(HInvoke* invoke);

  OptimizingCompilerStats* stats_;
  bool simplification_occurred_ = false;
  int simplifications_at_current_position_ = 0;
  // We ensure we do not loop infinitely. The value is a finger in the air guess
  // that should allow enough simplification.
  static constexpr int kMaxSamePositionSimplifications = 10;
};

void InstructionSimplifier::Run() {
  InstructionSimplifierVisitor visitor(graph_, stats_);
  visitor.Run();
}

void InstructionSimplifierVisitor::Run() {
  // Iterate in reverse post order to open up more simplifications to users
  // of instructions that got simplified.
  for (HReversePostOrderIterator it(*GetGraph()); !it.Done();) {
    // The simplification of an instruction to another instruction may yield
    // possibilities for other simplifications. So although we perform a reverse
    // post order visit, we sometimes need to revisit an instruction index.
    simplification_occurred_ = false;
    VisitBasicBlock(it.Current());
    if (simplification_occurred_ &&
        (simplifications_at_current_position_ < kMaxSamePositionSimplifications)) {
      // New simplifications may be applicable to the instruction at the
      // current index, so don't advance the iterator.
      continue;
    }
    simplifications_at_current_position_ = 0;
    it.Advance();
  }
}

namespace {

bool AreAllBitsSet(HConstant* constant) {
  return Int64FromConstant(constant) == -1;
}

}  // namespace

// Returns true if the code was simplified to use only one negation operation
// after the binary operation instead of one on each of the inputs.
bool InstructionSimplifierVisitor::TryMoveNegOnInputsAfterBinop(HBinaryOperation* binop) {
  DCHECK(binop->IsAdd() || binop->IsSub());
  DCHECK(binop->GetLeft()->IsNeg() && binop->GetRight()->IsNeg());
  HNeg* left_neg = binop->GetLeft()->AsNeg();
  HNeg* right_neg = binop->GetRight()->AsNeg();
  if (!left_neg->HasOnlyOneNonEnvironmentUse() ||
      !right_neg->HasOnlyOneNonEnvironmentUse()) {
    return false;
  }
  // Replace code looking like
  //    NEG tmp1, a
  //    NEG tmp2, b
  //    ADD dst, tmp1, tmp2
  // with
  //    ADD tmp, a, b
  //    NEG dst, tmp
  // Note that we cannot optimize `(-a) + (-b)` to `-(a + b)` for floating-point.
  // When `a` is `-0.0` and `b` is `0.0`, the former expression yields `0.0`,
  // while the later yields `-0.0`.
  if (!Primitive::IsIntegralType(binop->GetType())) {
    return false;
  }
  binop->ReplaceInput(left_neg->GetInput(), 0);
  binop->ReplaceInput(right_neg->GetInput(), 1);
  left_neg->GetBlock()->RemoveInstruction(left_neg);
  right_neg->GetBlock()->RemoveInstruction(right_neg);
  HNeg* neg = new (GetGraph()->GetArena()) HNeg(binop->GetType(), binop);
  binop->GetBlock()->InsertInstructionBefore(neg, binop->GetNext());
  binop->ReplaceWithExceptInReplacementAtIndex(neg, 0);
  RecordSimplification();
  return true;
}

void InstructionSimplifierVisitor::VisitShift(HBinaryOperation* instruction) {
  DCHECK(instruction->IsShl() || instruction->IsShr() || instruction->IsUShr());
  HConstant* input_cst = instruction->GetConstantRight();
  HInstruction* input_other = instruction->GetLeastConstantLeft();

  if (input_cst != nullptr) {
    if (input_cst->IsZero()) {
      // Replace code looking like
      //    SHL dst, src, 0
      // with
      //    src
      instruction->ReplaceWith(input_other);
      instruction->GetBlock()->RemoveInstruction(instruction);
    } else if (instruction->IsShl() && input_cst->IsOne()) {
      // Replace Shl looking like
      //    SHL dst, src, 1
      // with
      //    ADD dst, src, src
      HAdd *add = new(GetGraph()->GetArena()) HAdd(instruction->GetType(),
                                                   input_other,
                                                   input_other);
      instruction->GetBlock()->ReplaceAndRemoveInstructionWith(instruction, add);
      RecordSimplification();
    }
  }
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

bool InstructionSimplifierVisitor::CanEnsureNotNullAt(HInstruction* input, HInstruction* at) const {
  if (!input->CanBeNull()) {
    return true;
  }

  for (HUseIterator<HInstruction*> it(input->GetUses()); !it.Done(); it.Advance()) {
    HInstruction* use = it.Current()->GetUser();
    if (use->IsNullCheck() && use->StrictlyDominates(at)) {
      return true;
    }
  }

  return false;
}

// Returns whether doing a type test between the class of `object` against `klass` has
// a statically known outcome. The result of the test is stored in `outcome`.
static bool TypeCheckHasKnownOutcome(HLoadClass* klass, HInstruction* object, bool* outcome) {
  DCHECK(!object->IsNullConstant()) << "Null constants should be special cased";
  ReferenceTypeInfo obj_rti = object->GetReferenceTypeInfo();
  ScopedObjectAccess soa(Thread::Current());
  if (!obj_rti.IsValid()) {
    // We run the simplifier before the reference type propagation so type info might not be
    // available.
    return false;
  }

  ReferenceTypeInfo class_rti = klass->GetLoadedClassRTI();
  if (!class_rti.IsValid()) {
    // Happens when the loaded class is unresolved.
    return false;
  }
  DCHECK(class_rti.IsExact());
  if (class_rti.IsSupertypeOf(obj_rti)) {
    *outcome = true;
    return true;
  } else if (obj_rti.IsExact()) {
    // The test failed at compile time so will also fail at runtime.
    *outcome = false;
    return true;
  } else if (!class_rti.IsInterface()
             && !obj_rti.IsInterface()
             && !obj_rti.IsSupertypeOf(class_rti)) {
    // Different type hierarchy. The test will fail.
    *outcome = false;
    return true;
  }
  return false;
}

void InstructionSimplifierVisitor::VisitCheckCast(HCheckCast* check_cast) {
  HInstruction* object = check_cast->InputAt(0);
  HLoadClass* load_class = check_cast->InputAt(1)->AsLoadClass();
  if (load_class->NeedsAccessCheck()) {
    // If we need to perform an access check we cannot remove the instruction.
    return;
  }

  if (CanEnsureNotNullAt(object, check_cast)) {
    check_cast->ClearMustDoNullCheck();
  }

  if (object->IsNullConstant()) {
    check_cast->GetBlock()->RemoveInstruction(check_cast);
    if (stats_ != nullptr) {
      stats_->RecordStat(MethodCompilationStat::kRemovedCheckedCast);
    }
    return;
  }

  bool outcome;
  if (TypeCheckHasKnownOutcome(load_class, object, &outcome)) {
    if (outcome) {
      check_cast->GetBlock()->RemoveInstruction(check_cast);
      if (stats_ != nullptr) {
        stats_->RecordStat(MethodCompilationStat::kRemovedCheckedCast);
      }
      if (!load_class->HasUses()) {
        // We cannot rely on DCE to remove the class because the `HLoadClass` thinks it can throw.
        // However, here we know that it cannot because the checkcast was successfull, hence
        // the class was already loaded.
        load_class->GetBlock()->RemoveInstruction(load_class);
      }
    } else {
      // Don't do anything for exceptional cases for now. Ideally we should remove
      // all instructions and blocks this instruction dominates.
    }
  }
}

void InstructionSimplifierVisitor::VisitInstanceOf(HInstanceOf* instruction) {
  HInstruction* object = instruction->InputAt(0);
  HLoadClass* load_class = instruction->InputAt(1)->AsLoadClass();
  if (load_class->NeedsAccessCheck()) {
    // If we need to perform an access check we cannot remove the instruction.
    return;
  }

  bool can_be_null = true;
  if (CanEnsureNotNullAt(object, instruction)) {
    can_be_null = false;
    instruction->ClearMustDoNullCheck();
  }

  HGraph* graph = GetGraph();
  if (object->IsNullConstant()) {
    instruction->ReplaceWith(graph->GetIntConstant(0));
    instruction->GetBlock()->RemoveInstruction(instruction);
    RecordSimplification();
    return;
  }

  bool outcome;
  if (TypeCheckHasKnownOutcome(load_class, object, &outcome)) {
    if (outcome && can_be_null) {
      // Type test will succeed, we just need a null test.
      HNotEqual* test = new (graph->GetArena()) HNotEqual(graph->GetNullConstant(), object);
      instruction->GetBlock()->InsertInstructionBefore(test, instruction);
      instruction->ReplaceWith(test);
    } else {
      // We've statically determined the result of the instanceof.
      instruction->ReplaceWith(graph->GetIntConstant(outcome));
    }
    RecordSimplification();
    instruction->GetBlock()->RemoveInstruction(instruction);
    if (outcome && !load_class->HasUses()) {
      // We cannot rely on DCE to remove the class because the `HLoadClass` thinks it can throw.
      // However, here we know that it cannot because the instanceof check was successfull, hence
      // the class was already loaded.
      load_class->GetBlock()->RemoveInstruction(load_class);
    }
  }
}

void InstructionSimplifierVisitor::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  if ((instruction->GetValue()->GetType() == Primitive::kPrimNot)
      && CanEnsureNotNullAt(instruction->GetValue(), instruction)) {
    instruction->ClearValueCanBeNull();
  }
}

void InstructionSimplifierVisitor::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  if ((instruction->GetValue()->GetType() == Primitive::kPrimNot)
      && CanEnsureNotNullAt(instruction->GetValue(), instruction)) {
    instruction->ClearValueCanBeNull();
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
  HInstruction* input_const = equal->GetConstantRight();
  if (input_const != nullptr) {
    HInstruction* input_value = equal->GetLeastConstantLeft();
    if (input_value->GetType() == Primitive::kPrimBoolean && input_const->IsIntConstant()) {
      HBasicBlock* block = equal->GetBlock();
      // We are comparing the boolean to a constant which is of type int and can
      // be any constant.
      if (input_const->AsIntConstant()->IsOne()) {
        // Replace (bool_value == true) with bool_value
        equal->ReplaceWith(input_value);
        block->RemoveInstruction(equal);
        RecordSimplification();
      } else if (input_const->AsIntConstant()->IsZero()) {
        // Replace (bool_value == false) with !bool_value
        block->ReplaceAndRemoveInstructionWith(
            equal, new (block->GetGraph()->GetArena()) HBooleanNot(input_value));
        RecordSimplification();
      } else {
        // Replace (bool_value == integer_not_zero_nor_one_constant) with false
        equal->ReplaceWith(GetGraph()->GetIntConstant(0));
        block->RemoveInstruction(equal);
        RecordSimplification();
      }
    } else {
      VisitCondition(equal);
    }
  } else {
    VisitCondition(equal);
  }
}

void InstructionSimplifierVisitor::VisitNotEqual(HNotEqual* not_equal) {
  HInstruction* input_const = not_equal->GetConstantRight();
  if (input_const != nullptr) {
    HInstruction* input_value = not_equal->GetLeastConstantLeft();
    if (input_value->GetType() == Primitive::kPrimBoolean && input_const->IsIntConstant()) {
      HBasicBlock* block = not_equal->GetBlock();
      // We are comparing the boolean to a constant which is of type int and can
      // be any constant.
      if (input_const->AsIntConstant()->IsOne()) {
        // Replace (bool_value != true) with !bool_value
        block->ReplaceAndRemoveInstructionWith(
            not_equal, new (block->GetGraph()->GetArena()) HBooleanNot(input_value));
        RecordSimplification();
      } else if (input_const->AsIntConstant()->IsZero()) {
        // Replace (bool_value != false) with bool_value
        not_equal->ReplaceWith(input_value);
        block->RemoveInstruction(not_equal);
        RecordSimplification();
      } else {
        // Replace (bool_value != integer_not_zero_nor_one_constant) with true
        not_equal->ReplaceWith(GetGraph()->GetIntConstant(1));
        block->RemoveInstruction(not_equal);
        RecordSimplification();
      }
    } else {
      VisitCondition(not_equal);
    }
  } else {
    VisitCondition(not_equal);
  }
}

void InstructionSimplifierVisitor::VisitBooleanNot(HBooleanNot* bool_not) {
  HInstruction* parent = bool_not->InputAt(0);
  if (parent->IsBooleanNot()) {
    HInstruction* value = parent->InputAt(0);
    // Replace (!(!bool_value)) with bool_value
    bool_not->ReplaceWith(value);
    bool_not->GetBlock()->RemoveInstruction(bool_not);
    // It is possible that `parent` is dead at this point but we leave
    // its removal to DCE for simplicity.
    RecordSimplification();
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

  if (CanEnsureNotNullAt(value, instruction)) {
    instruction->ClearValueCanBeNull();
  }

  if (value->IsArrayGet()) {
    if (value->AsArrayGet()->GetArray() == instruction->GetArray()) {
      // If the code is just swapping elements in the array, no need for a type check.
      instruction->ClearNeedsTypeCheck();
      return;
    }
  }

  if (value->IsNullConstant()) {
    instruction->ClearNeedsTypeCheck();
    return;
  }

  ScopedObjectAccess soa(Thread::Current());
  ReferenceTypeInfo array_rti = instruction->GetArray()->GetReferenceTypeInfo();
  ReferenceTypeInfo value_rti = value->GetReferenceTypeInfo();
  if (!array_rti.IsValid()) {
    return;
  }

  if (value_rti.IsValid() && array_rti.CanArrayHold(value_rti)) {
    instruction->ClearNeedsTypeCheck();
    return;
  }

  if (array_rti.IsObjectArray()) {
    if (array_rti.IsExact()) {
      instruction->ClearNeedsTypeCheck();
      return;
    }
    instruction->SetStaticTypeOfArrayIsObjectArray();
  }
}

void InstructionSimplifierVisitor::VisitTypeConversion(HTypeConversion* instruction) {
  if (instruction->GetResultType() == instruction->GetInputType()) {
    // Remove the instruction if it's converting to the same type.
    instruction->ReplaceWith(instruction->GetInput());
    instruction->GetBlock()->RemoveInstruction(instruction);
  }
}

void InstructionSimplifierVisitor::VisitAdd(HAdd* instruction) {
  HConstant* input_cst = instruction->GetConstantRight();
  HInstruction* input_other = instruction->GetLeastConstantLeft();
  if ((input_cst != nullptr) && input_cst->IsZero()) {
    // Replace code looking like
    //    ADD dst, src, 0
    // with
    //    src
    // Note that we cannot optimize `x + 0.0` to `x` for floating-point. When
    // `x` is `-0.0`, the former expression yields `0.0`, while the later
    // yields `-0.0`.
    if (Primitive::IsIntegralType(instruction->GetType())) {
      instruction->ReplaceWith(input_other);
      instruction->GetBlock()->RemoveInstruction(instruction);
      return;
    }
  }

  HInstruction* left = instruction->GetLeft();
  HInstruction* right = instruction->GetRight();
  bool left_is_neg = left->IsNeg();
  bool right_is_neg = right->IsNeg();

  if (left_is_neg && right_is_neg) {
    if (TryMoveNegOnInputsAfterBinop(instruction)) {
      return;
    }
  }

  HNeg* neg = left_is_neg ? left->AsNeg() : right->AsNeg();
  if ((left_is_neg ^ right_is_neg) && neg->HasOnlyOneNonEnvironmentUse()) {
    // Replace code looking like
    //    NEG tmp, b
    //    ADD dst, a, tmp
    // with
    //    SUB dst, a, b
    // We do not perform the optimization if the input negation has environment
    // uses or multiple non-environment uses as it could lead to worse code. In
    // particular, we do not want the live range of `b` to be extended if we are
    // not sure the initial 'NEG' instruction can be removed.
    HInstruction* other = left_is_neg ? right : left;
    HSub* sub = new(GetGraph()->GetArena()) HSub(instruction->GetType(), other, neg->GetInput());
    instruction->GetBlock()->ReplaceAndRemoveInstructionWith(instruction, sub);
    RecordSimplification();
    neg->GetBlock()->RemoveInstruction(neg);
  }
}

void InstructionSimplifierVisitor::VisitAnd(HAnd* instruction) {
  HConstant* input_cst = instruction->GetConstantRight();
  HInstruction* input_other = instruction->GetLeastConstantLeft();

  if (input_cst != nullptr) {
    int64_t value = Int64FromConstant(input_cst);
    if (value == -1) {
      // Replace code looking like
      //    AND dst, src, 0xFFF...FF
      // with
      //    src
      instruction->ReplaceWith(input_other);
      instruction->GetBlock()->RemoveInstruction(instruction);
      RecordSimplification();
      return;
    }
    // Eliminate And from UShr+And if the And-mask contains all the bits that
    // can be non-zero after UShr. Transform Shr+And to UShr if the And-mask
    // precisely clears the shifted-in sign bits.
    if ((input_other->IsUShr() || input_other->IsShr()) && input_other->InputAt(1)->IsConstant()) {
      size_t reg_bits = (instruction->GetResultType() == Primitive::kPrimLong) ? 64 : 32;
      size_t shift = Int64FromConstant(input_other->InputAt(1)->AsConstant()) & (reg_bits - 1);
      size_t num_tail_bits_set = CTZ(value + 1);
      if ((num_tail_bits_set >= reg_bits - shift) && input_other->IsUShr()) {
        // This AND clears only bits known to be clear, for example "(x >>> 24) & 0xff".
        instruction->ReplaceWith(input_other);
        instruction->GetBlock()->RemoveInstruction(instruction);
        RecordSimplification();
        return;
      }  else if ((num_tail_bits_set == reg_bits - shift) && IsPowerOfTwo(value + 1) &&
          input_other->HasOnlyOneNonEnvironmentUse()) {
        DCHECK(input_other->IsShr());  // For UShr, we would have taken the branch above.
        // Replace SHR+AND with USHR, for example "(x >> 24) & 0xff" -> "x >>> 24".
        HUShr* ushr = new (GetGraph()->GetArena()) HUShr(instruction->GetType(),
                                                         input_other->InputAt(0),
                                                         input_other->InputAt(1),
                                                         input_other->GetDexPc());
        instruction->GetBlock()->ReplaceAndRemoveInstructionWith(instruction, ushr);
        input_other->GetBlock()->RemoveInstruction(input_other);
        RecordSimplification();
        return;
      }
    }
  }

  // We assume that GVN has run before, so we only perform a pointer comparison.
  // If for some reason the values are equal but the pointers are different, we
  // are still correct and only miss an optimization opportunity.
  if (instruction->GetLeft() == instruction->GetRight()) {
    // Replace code looking like
    //    AND dst, src, src
    // with
    //    src
    instruction->ReplaceWith(instruction->GetLeft());
    instruction->GetBlock()->RemoveInstruction(instruction);
  }
}

void InstructionSimplifierVisitor::VisitGreaterThan(HGreaterThan* condition) {
  VisitCondition(condition);
}

void InstructionSimplifierVisitor::VisitGreaterThanOrEqual(HGreaterThanOrEqual* condition) {
  VisitCondition(condition);
}

void InstructionSimplifierVisitor::VisitLessThan(HLessThan* condition) {
  VisitCondition(condition);
}

void InstructionSimplifierVisitor::VisitLessThanOrEqual(HLessThanOrEqual* condition) {
  VisitCondition(condition);
}

// TODO: unsigned comparisons too?

void InstructionSimplifierVisitor::VisitCondition(HCondition* condition) {
  // Try to fold an HCompare into this HCondition.

  // This simplification is currently supported on x86, x86_64, ARM and ARM64.
  // TODO: Implement it for MIPS and MIPS64.
  InstructionSet instruction_set = GetGraph()->GetInstructionSet();
  if (instruction_set == kMips || instruction_set == kMips64) {
    return;
  }

  HInstruction* left = condition->GetLeft();
  HInstruction* right = condition->GetRight();
  // We can only replace an HCondition which compares a Compare to 0.
  // Both 'dx' and 'jack' generate a compare to 0 when compiling a
  // condition with a long, float or double comparison as input.
  if (!left->IsCompare() || !right->IsConstant() || right->AsIntConstant()->GetValue() != 0) {
    // Conversion is not possible.
    return;
  }

  // Is the Compare only used for this purpose?
  if (!left->GetUses().HasOnlyOneUse()) {
    // Someone else also wants the result of the compare.
    return;
  }

  if (!left->GetEnvUses().IsEmpty()) {
    // There is a reference to the compare result in an environment. Do we really need it?
    if (GetGraph()->IsDebuggable()) {
      return;
    }

    // We have to ensure that there are no deopt points in the sequence.
    if (left->HasAnyEnvironmentUseBefore(condition)) {
      return;
    }
  }

  // Clean up any environment uses from the HCompare, if any.
  left->RemoveEnvironmentUsers();

  // We have decided to fold the HCompare into the HCondition. Transfer the information.
  condition->SetBias(left->AsCompare()->GetBias());

  // Replace the operands of the HCondition.
  condition->ReplaceInput(left->InputAt(0), 0);
  condition->ReplaceInput(left->InputAt(1), 1);

  // Remove the HCompare.
  left->GetBlock()->RemoveInstruction(left);

  RecordSimplification();
}

void InstructionSimplifierVisitor::VisitDiv(HDiv* instruction) {
  HConstant* input_cst = instruction->GetConstantRight();
  HInstruction* input_other = instruction->GetLeastConstantLeft();
  Primitive::Type type = instruction->GetType();

  if ((input_cst != nullptr) && input_cst->IsOne()) {
    // Replace code looking like
    //    DIV dst, src, 1
    // with
    //    src
    instruction->ReplaceWith(input_other);
    instruction->GetBlock()->RemoveInstruction(instruction);
    return;
  }

  if ((input_cst != nullptr) && input_cst->IsMinusOne()) {
    // Replace code looking like
    //    DIV dst, src, -1
    // with
    //    NEG dst, src
    instruction->GetBlock()->ReplaceAndRemoveInstructionWith(
        instruction, new (GetGraph()->GetArena()) HNeg(type, input_other));
    RecordSimplification();
    return;
  }

  if ((input_cst != nullptr) && Primitive::IsFloatingPointType(type)) {
    // Try replacing code looking like
    //    DIV dst, src, constant
    // with
    //    MUL dst, src, 1 / constant
    HConstant* reciprocal = nullptr;
    if (type == Primitive::Primitive::kPrimDouble) {
      double value = input_cst->AsDoubleConstant()->GetValue();
      if (CanDivideByReciprocalMultiplyDouble(bit_cast<int64_t, double>(value))) {
        reciprocal = GetGraph()->GetDoubleConstant(1.0 / value);
      }
    } else {
      DCHECK_EQ(type, Primitive::kPrimFloat);
      float value = input_cst->AsFloatConstant()->GetValue();
      if (CanDivideByReciprocalMultiplyFloat(bit_cast<int32_t, float>(value))) {
        reciprocal = GetGraph()->GetFloatConstant(1.0f / value);
      }
    }

    if (reciprocal != nullptr) {
      instruction->GetBlock()->ReplaceAndRemoveInstructionWith(
          instruction, new (GetGraph()->GetArena()) HMul(type, input_other, reciprocal));
      RecordSimplification();
      return;
    }
  }
}

void InstructionSimplifierVisitor::VisitMul(HMul* instruction) {
  HConstant* input_cst = instruction->GetConstantRight();
  HInstruction* input_other = instruction->GetLeastConstantLeft();
  Primitive::Type type = instruction->GetType();
  HBasicBlock* block = instruction->GetBlock();
  ArenaAllocator* allocator = GetGraph()->GetArena();

  if (input_cst == nullptr) {
    return;
  }

  if (input_cst->IsOne()) {
    // Replace code looking like
    //    MUL dst, src, 1
    // with
    //    src
    instruction->ReplaceWith(input_other);
    instruction->GetBlock()->RemoveInstruction(instruction);
    return;
  }

  if (input_cst->IsMinusOne() &&
      (Primitive::IsFloatingPointType(type) || Primitive::IsIntOrLongType(type))) {
    // Replace code looking like
    //    MUL dst, src, -1
    // with
    //    NEG dst, src
    HNeg* neg = new (allocator) HNeg(type, input_other);
    block->ReplaceAndRemoveInstructionWith(instruction, neg);
    RecordSimplification();
    return;
  }

  if (Primitive::IsFloatingPointType(type) &&
      ((input_cst->IsFloatConstant() && input_cst->AsFloatConstant()->GetValue() == 2.0f) ||
       (input_cst->IsDoubleConstant() && input_cst->AsDoubleConstant()->GetValue() == 2.0))) {
    // Replace code looking like
    //    FP_MUL dst, src, 2.0
    // with
    //    FP_ADD dst, src, src
    // The 'int' and 'long' cases are handled below.
    block->ReplaceAndRemoveInstructionWith(instruction,
                                           new (allocator) HAdd(type, input_other, input_other));
    RecordSimplification();
    return;
  }

  if (Primitive::IsIntOrLongType(type)) {
    int64_t factor = Int64FromConstant(input_cst);
    // Even though constant propagation also takes care of the zero case, other
    // optimizations can lead to having a zero multiplication.
    if (factor == 0) {
      // Replace code looking like
      //    MUL dst, src, 0
      // with
      //    0
      instruction->ReplaceWith(input_cst);
      instruction->GetBlock()->RemoveInstruction(instruction);
    } else if (IsPowerOfTwo(factor)) {
      // Replace code looking like
      //    MUL dst, src, pow_of_2
      // with
      //    SHL dst, src, log2(pow_of_2)
      HIntConstant* shift = GetGraph()->GetIntConstant(WhichPowerOf2(factor));
      HShl* shl = new(allocator) HShl(type, input_other, shift);
      block->ReplaceAndRemoveInstructionWith(instruction, shl);
      RecordSimplification();
    } else if (IsPowerOfTwo(factor - 1)) {
      // Transform code looking like
      //    MUL dst, src, (2^n + 1)
      // into
      //    SHL tmp, src, n
      //    ADD dst, src, tmp
      HShl* shl = new (allocator) HShl(type,
                                       input_other,
                                       GetGraph()->GetIntConstant(WhichPowerOf2(factor - 1)));
      HAdd* add = new (allocator) HAdd(type, input_other, shl);

      block->InsertInstructionBefore(shl, instruction);
      block->ReplaceAndRemoveInstructionWith(instruction, add);
      RecordSimplification();
    } else if (IsPowerOfTwo(factor + 1)) {
      // Transform code looking like
      //    MUL dst, src, (2^n - 1)
      // into
      //    SHL tmp, src, n
      //    SUB dst, tmp, src
      HShl* shl = new (allocator) HShl(type,
                                       input_other,
                                       GetGraph()->GetIntConstant(WhichPowerOf2(factor + 1)));
      HSub* sub = new (allocator) HSub(type, shl, input_other);

      block->InsertInstructionBefore(shl, instruction);
      block->ReplaceAndRemoveInstructionWith(instruction, sub);
      RecordSimplification();
    }
  }
}

void InstructionSimplifierVisitor::VisitNeg(HNeg* instruction) {
  HInstruction* input = instruction->GetInput();
  if (input->IsNeg()) {
    // Replace code looking like
    //    NEG tmp, src
    //    NEG dst, tmp
    // with
    //    src
    HNeg* previous_neg = input->AsNeg();
    instruction->ReplaceWith(previous_neg->GetInput());
    instruction->GetBlock()->RemoveInstruction(instruction);
    // We perform the optimization even if the input negation has environment
    // uses since it allows removing the current instruction. But we only delete
    // the input negation only if it is does not have any uses left.
    if (!previous_neg->HasUses()) {
      previous_neg->GetBlock()->RemoveInstruction(previous_neg);
    }
    RecordSimplification();
    return;
  }

  if (input->IsSub() && input->HasOnlyOneNonEnvironmentUse() &&
      !Primitive::IsFloatingPointType(input->GetType())) {
    // Replace code looking like
    //    SUB tmp, a, b
    //    NEG dst, tmp
    // with
    //    SUB dst, b, a
    // We do not perform the optimization if the input subtraction has
    // environment uses or multiple non-environment uses as it could lead to
    // worse code. In particular, we do not want the live ranges of `a` and `b`
    // to be extended if we are not sure the initial 'SUB' instruction can be
    // removed.
    // We do not perform optimization for fp because we could lose the sign of zero.
    HSub* sub = input->AsSub();
    HSub* new_sub =
        new (GetGraph()->GetArena()) HSub(instruction->GetType(), sub->GetRight(), sub->GetLeft());
    instruction->GetBlock()->ReplaceAndRemoveInstructionWith(instruction, new_sub);
    if (!sub->HasUses()) {
      sub->GetBlock()->RemoveInstruction(sub);
    }
    RecordSimplification();
  }
}

void InstructionSimplifierVisitor::VisitNot(HNot* instruction) {
  HInstruction* input = instruction->GetInput();
  if (input->IsNot()) {
    // Replace code looking like
    //    NOT tmp, src
    //    NOT dst, tmp
    // with
    //    src
    // We perform the optimization even if the input negation has environment
    // uses since it allows removing the current instruction. But we only delete
    // the input negation only if it is does not have any uses left.
    HNot* previous_not = input->AsNot();
    instruction->ReplaceWith(previous_not->GetInput());
    instruction->GetBlock()->RemoveInstruction(instruction);
    if (!previous_not->HasUses()) {
      previous_not->GetBlock()->RemoveInstruction(previous_not);
    }
    RecordSimplification();
  }
}

void InstructionSimplifierVisitor::VisitOr(HOr* instruction) {
  HConstant* input_cst = instruction->GetConstantRight();
  HInstruction* input_other = instruction->GetLeastConstantLeft();

  if ((input_cst != nullptr) && input_cst->IsZero()) {
    // Replace code looking like
    //    OR dst, src, 0
    // with
    //    src
    instruction->ReplaceWith(input_other);
    instruction->GetBlock()->RemoveInstruction(instruction);
    return;
  }

  // We assume that GVN has run before, so we only perform a pointer comparison.
  // If for some reason the values are equal but the pointers are different, we
  // are still correct and only miss an optimization opportunity.
  if (instruction->GetLeft() == instruction->GetRight()) {
    // Replace code looking like
    //    OR dst, src, src
    // with
    //    src
    instruction->ReplaceWith(instruction->GetLeft());
    instruction->GetBlock()->RemoveInstruction(instruction);
  }
}

void InstructionSimplifierVisitor::VisitShl(HShl* instruction) {
  VisitShift(instruction);
}

void InstructionSimplifierVisitor::VisitShr(HShr* instruction) {
  VisitShift(instruction);
}

void InstructionSimplifierVisitor::VisitSub(HSub* instruction) {
  HConstant* input_cst = instruction->GetConstantRight();
  HInstruction* input_other = instruction->GetLeastConstantLeft();

  Primitive::Type type = instruction->GetType();
  if (Primitive::IsFloatingPointType(type)) {
    return;
  }

  if ((input_cst != nullptr) && input_cst->IsZero()) {
    // Replace code looking like
    //    SUB dst, src, 0
    // with
    //    src
    // Note that we cannot optimize `x - 0.0` to `x` for floating-point. When
    // `x` is `-0.0`, the former expression yields `0.0`, while the later
    // yields `-0.0`.
    instruction->ReplaceWith(input_other);
    instruction->GetBlock()->RemoveInstruction(instruction);
    return;
  }

  HBasicBlock* block = instruction->GetBlock();
  ArenaAllocator* allocator = GetGraph()->GetArena();

  HInstruction* left = instruction->GetLeft();
  HInstruction* right = instruction->GetRight();
  if (left->IsConstant()) {
    if (Int64FromConstant(left->AsConstant()) == 0) {
      // Replace code looking like
      //    SUB dst, 0, src
      // with
      //    NEG dst, src
      // Note that we cannot optimize `0.0 - x` to `-x` for floating-point. When
      // `x` is `0.0`, the former expression yields `0.0`, while the later
      // yields `-0.0`.
      HNeg* neg = new (allocator) HNeg(type, right);
      block->ReplaceAndRemoveInstructionWith(instruction, neg);
      RecordSimplification();
      return;
    }
  }

  if (left->IsNeg() && right->IsNeg()) {
    if (TryMoveNegOnInputsAfterBinop(instruction)) {
      return;
    }
  }

  if (right->IsNeg() && right->HasOnlyOneNonEnvironmentUse()) {
    // Replace code looking like
    //    NEG tmp, b
    //    SUB dst, a, tmp
    // with
    //    ADD dst, a, b
    HAdd* add = new(GetGraph()->GetArena()) HAdd(type, left, right->AsNeg()->GetInput());
    instruction->GetBlock()->ReplaceAndRemoveInstructionWith(instruction, add);
    RecordSimplification();
    right->GetBlock()->RemoveInstruction(right);
    return;
  }

  if (left->IsNeg() && left->HasOnlyOneNonEnvironmentUse()) {
    // Replace code looking like
    //    NEG tmp, a
    //    SUB dst, tmp, b
    // with
    //    ADD tmp, a, b
    //    NEG dst, tmp
    // The second version is not intrinsically better, but enables more
    // transformations.
    HAdd* add = new(GetGraph()->GetArena()) HAdd(type, left->AsNeg()->GetInput(), right);
    instruction->GetBlock()->InsertInstructionBefore(add, instruction);
    HNeg* neg = new (GetGraph()->GetArena()) HNeg(instruction->GetType(), add);
    instruction->GetBlock()->InsertInstructionBefore(neg, instruction);
    instruction->ReplaceWith(neg);
    instruction->GetBlock()->RemoveInstruction(instruction);
    RecordSimplification();
    left->GetBlock()->RemoveInstruction(left);
  }
}

void InstructionSimplifierVisitor::VisitUShr(HUShr* instruction) {
  VisitShift(instruction);
}

void InstructionSimplifierVisitor::VisitXor(HXor* instruction) {
  HConstant* input_cst = instruction->GetConstantRight();
  HInstruction* input_other = instruction->GetLeastConstantLeft();

  if ((input_cst != nullptr) && input_cst->IsZero()) {
    // Replace code looking like
    //    XOR dst, src, 0
    // with
    //    src
    instruction->ReplaceWith(input_other);
    instruction->GetBlock()->RemoveInstruction(instruction);
    return;
  }

  if ((input_cst != nullptr) && AreAllBitsSet(input_cst)) {
    // Replace code looking like
    //    XOR dst, src, 0xFFF...FF
    // with
    //    NOT dst, src
    HNot* bitwise_not = new (GetGraph()->GetArena()) HNot(instruction->GetType(), input_other);
    instruction->GetBlock()->ReplaceAndRemoveInstructionWith(instruction, bitwise_not);
    RecordSimplification();
    return;
  }
}

void InstructionSimplifierVisitor::VisitFakeString(HFakeString* instruction) {
  HInstruction* actual_string = nullptr;

  // Find the string we need to replace this instruction with. The actual string is
  // the return value of a StringFactory call.
  for (HUseIterator<HInstruction*> it(instruction->GetUses()); !it.Done(); it.Advance()) {
    HInstruction* use = it.Current()->GetUser();
    if (use->IsInvokeStaticOrDirect()
        && use->AsInvokeStaticOrDirect()->IsStringFactoryFor(instruction)) {
      use->AsInvokeStaticOrDirect()->RemoveFakeStringArgumentAsLastInput();
      actual_string = use;
      break;
    }
  }

  // Check that there is no other instruction that thinks it is the factory for that string.
  if (kIsDebugBuild) {
    CHECK(actual_string != nullptr);
    for (HUseIterator<HInstruction*> it(instruction->GetUses()); !it.Done(); it.Advance()) {
      HInstruction* use = it.Current()->GetUser();
      if (use->IsInvokeStaticOrDirect()) {
        CHECK(!use->AsInvokeStaticOrDirect()->IsStringFactoryFor(instruction));
      }
    }
  }

  // We need to remove any environment uses of the fake string that are not dominated by
  // `actual_string` to null.
  for (HUseIterator<HEnvironment*> it(instruction->GetEnvUses()); !it.Done(); it.Advance()) {
    HEnvironment* environment = it.Current()->GetUser();
    if (!actual_string->StrictlyDominates(environment->GetHolder())) {
      environment->RemoveAsUserOfInput(it.Current()->GetIndex());
      environment->SetRawEnvAt(it.Current()->GetIndex(), nullptr);
    }
  }

  // Only uses dominated by `actual_string` must remain. We can safely replace and remove
  // `instruction`.
  instruction->ReplaceWith(actual_string);
  instruction->GetBlock()->RemoveInstruction(instruction);
}

void InstructionSimplifierVisitor::SimplifyStringEquals(HInvoke* instruction) {
  HInstruction* argument = instruction->InputAt(1);
  HInstruction* receiver = instruction->InputAt(0);
  if (receiver == argument) {
    // Because String.equals is an instance call, the receiver is
    // a null check if we don't know it's null. The argument however, will
    // be the actual object. So we cannot end up in a situation where both
    // are equal but could be null.
    DCHECK(CanEnsureNotNullAt(argument, instruction));
    instruction->ReplaceWith(GetGraph()->GetIntConstant(1));
    instruction->GetBlock()->RemoveInstruction(instruction);
  } else {
    StringEqualsOptimizations optimizations(instruction);
    if (CanEnsureNotNullAt(argument, instruction)) {
      optimizations.SetArgumentNotNull();
    }
    ScopedObjectAccess soa(Thread::Current());
    ReferenceTypeInfo argument_rti = argument->GetReferenceTypeInfo();
    if (argument_rti.IsValid() && argument_rti.IsStringClass()) {
      optimizations.SetArgumentIsString();
    }
  }
}

static bool IsArrayLengthOf(HInstruction* potential_length, HInstruction* potential_array) {
  if (potential_length->IsArrayLength()) {
    return potential_length->InputAt(0) == potential_array;
  }

  if (potential_array->IsNewArray()) {
    return potential_array->InputAt(0) == potential_length;
  }

  return false;
}

void InstructionSimplifierVisitor::SimplifySystemArrayCopy(HInvoke* instruction) {
  HInstruction* source = instruction->InputAt(0);
  HInstruction* destination = instruction->InputAt(2);
  HInstruction* count = instruction->InputAt(4);
  SystemArrayCopyOptimizations optimizations(instruction);
  if (CanEnsureNotNullAt(source, instruction)) {
    optimizations.SetSourceIsNotNull();
  }
  if (CanEnsureNotNullAt(destination, instruction)) {
    optimizations.SetDestinationIsNotNull();
  }
  if (destination == source) {
    optimizations.SetDestinationIsSource();
  }

  if (IsArrayLengthOf(count, source)) {
    optimizations.SetCountIsSourceLength();
  }

  if (IsArrayLengthOf(count, destination)) {
    optimizations.SetCountIsDestinationLength();
  }

  {
    ScopedObjectAccess soa(Thread::Current());
    ReferenceTypeInfo destination_rti = destination->GetReferenceTypeInfo();
    if (destination_rti.IsValid()) {
      if (destination_rti.IsObjectArray()) {
        if (destination_rti.IsExact()) {
          optimizations.SetDoesNotNeedTypeCheck();
        }
        optimizations.SetDestinationIsTypedObjectArray();
      }
      if (destination_rti.IsPrimitiveArrayClass()) {
        optimizations.SetDestinationIsPrimitiveArray();
      } else if (destination_rti.IsNonPrimitiveArrayClass()) {
        optimizations.SetDestinationIsNonPrimitiveArray();
      }
    }
    ReferenceTypeInfo source_rti = source->GetReferenceTypeInfo();
    if (source_rti.IsValid()) {
      if (destination_rti.IsValid() && destination_rti.CanArrayHoldValuesOf(source_rti)) {
        optimizations.SetDoesNotNeedTypeCheck();
      }
      if (source_rti.IsPrimitiveArrayClass()) {
        optimizations.SetSourceIsPrimitiveArray();
      } else if (source_rti.IsNonPrimitiveArrayClass()) {
        optimizations.SetSourceIsNonPrimitiveArray();
      }
    }
  }
}

void InstructionSimplifierVisitor::VisitInvoke(HInvoke* instruction) {
  if (instruction->GetIntrinsic() == Intrinsics::kStringEquals) {
    SimplifyStringEquals(instruction);
  } else if (instruction->GetIntrinsic() == Intrinsics::kSystemArrayCopy) {
    SimplifySystemArrayCopy(instruction);
  }
}

void InstructionSimplifierVisitor::VisitDeoptimize(HDeoptimize* deoptimize) {
  HInstruction* cond = deoptimize->InputAt(0);
  if (cond->IsConstant()) {
    if (cond->AsIntConstant()->IsZero()) {
      // Never deopt: instruction can be removed.
      deoptimize->GetBlock()->RemoveInstruction(deoptimize);
    } else {
      // Always deopt.
    }
  }
}

}  // namespace art
