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

#include "prepare_for_register_allocation.h"

namespace art {

void PrepareForRegisterAllocation::Run() {
  // Order does not matter.
  for (HReversePostOrderIterator it(*GetGraph()); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    // No need to visit the phis.
    for (HInstructionIterator inst_it(block->GetInstructions()); !inst_it.Done();
         inst_it.Advance()) {
      inst_it.Current()->Accept(this);
    }
  }
}

void PrepareForRegisterAllocation::VisitNullCheck(HNullCheck* check) {
  check->ReplaceWith(check->InputAt(0));
}

void PrepareForRegisterAllocation::VisitDivZeroCheck(HDivZeroCheck* check) {
  check->ReplaceWith(check->InputAt(0));
}

void PrepareForRegisterAllocation::VisitBoundsCheck(HBoundsCheck* check) {
  check->ReplaceWith(check->InputAt(0));
}

void PrepareForRegisterAllocation::VisitBoundType(HBoundType* bound_type) {
  bound_type->ReplaceWith(bound_type->InputAt(0));
  bound_type->GetBlock()->RemoveInstruction(bound_type);
}

void PrepareForRegisterAllocation::VisitClinitCheck(HClinitCheck* check) {
  HLoadClass* cls = check->GetLoadClass();
  check->ReplaceWith(cls);
  if (check->GetPrevious() == cls) {
    // Pass the initialization duty to the `HLoadClass` instruction,
    // and remove the instruction from the graph.
    cls->SetMustGenerateClinitCheck(true);
    check->GetBlock()->RemoveInstruction(check);
  }
}

void PrepareForRegisterAllocation::VisitCondition(HCondition* condition) {
  bool needs_materialization = false;
  if (!condition->GetUses().HasOnlyOneUse() || !condition->GetEnvUses().IsEmpty()) {
    needs_materialization = true;
  } else {
    HInstruction* user = condition->GetUses().GetFirst()->GetUser();
    if (!user->IsIf() && !user->IsDeoptimize()) {
      needs_materialization = true;
    } else {
      // TODO: if there is no intervening instructions with side-effect between this condition
      // and the If instruction, we should move the condition just before the If.
      if (condition->GetNext() != user) {
        needs_materialization = true;
      }
    }
  }
  if (!needs_materialization) {
    condition->ClearNeedsMaterialization();
  }
}

void PrepareForRegisterAllocation::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  if (invoke->IsStaticWithExplicitClinitCheck()) {
    size_t last_input_index = invoke->InputCount() - 1;
    HLoadClass* last_input = invoke->InputAt(last_input_index)->AsLoadClass();
    DCHECK(last_input != nullptr)
        << "Last input is not HLoadClass. It is " << last_input->DebugName();

    // Remove a load class instruction as last input of a static
    // invoke, which has been added (along with a clinit check,
    // removed by PrepareForRegisterAllocation::VisitClinitCheck
    // previously) by the graph builder during the creation of the
    // static invoke instruction, but is no longer required at this
    // stage (i.e., after inlining has been performed).
    invoke->RemoveLoadClassAsLastInput();

    // The static call will initialize the class so there's no need for a clinit check if
    // it's the first user.
    // There is one special case where we still need the clinit check, when inlining. Because
    // currently the callee is responsible for reporting parameters to the GC, the code
    // that walks the stack during `artQuickResolutionTrampoline` cannot be interrupted for GC.
    // Therefore we cannot allocate any object in that code, including loading a new class.
    if (last_input == invoke->GetPrevious() && !invoke->IsFromInlinedInvoke()) {
      last_input->SetMustGenerateClinitCheck(false);

      // If the load class instruction is no longer used, remove it from
      // the graph.
      if (!last_input->HasUses()) {
        last_input->GetBlock()->RemoveInstruction(last_input);
      }
    }
  }
}

}  // namespace art
