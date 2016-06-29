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

void PrepareForRegisterAllocation::VisitArraySet(HArraySet* instruction) {
  HInstruction* value = instruction->GetValue();
  // PrepareForRegisterAllocation::VisitBoundType may have replaced a
  // BoundType (as value input of this ArraySet) with a NullConstant.
  // If so, this ArraySet no longer needs a type check.
  if (value->IsNullConstant()) {
    DCHECK_EQ(value->GetType(), Primitive::kPrimNot);
    if (instruction->NeedsTypeCheck()) {
      instruction->ClearNeedsTypeCheck();
    }
  }
}

void PrepareForRegisterAllocation::VisitClinitCheck(HClinitCheck* check) {
  // Try to find a static invoke or a new-instance from which this check originated.
  HInstruction* implicit_clinit = nullptr;
  for (const HUseListNode<HInstruction*>& use : check->GetUses()) {
    HInstruction* user = use.GetUser();
    if ((user->IsInvokeStaticOrDirect() || user->IsNewInstance()) &&
        CanMoveClinitCheck(check, user)) {
      implicit_clinit = user;
      if (user->IsInvokeStaticOrDirect()) {
        DCHECK(user->AsInvokeStaticOrDirect()->IsStaticWithExplicitClinitCheck());
        user->AsInvokeStaticOrDirect()->RemoveExplicitClinitCheck(
            HInvokeStaticOrDirect::ClinitCheckRequirement::kImplicit);
      } else {
        DCHECK(user->IsNewInstance());
        // We delegate the initialization duty to the allocation.
        if (user->AsNewInstance()->GetEntrypoint() == kQuickAllocObjectInitialized) {
          user->AsNewInstance()->SetEntrypoint(kQuickAllocObjectResolved);
        }
      }
      break;
    }
  }
  // If we found a static invoke or new-instance for merging, remove the check
  // from dominated static invokes.
  if (implicit_clinit != nullptr) {
    const HUseList<HInstruction*>& uses = check->GetUses();
    for (auto it = uses.begin(), end = uses.end(); it != end; /* ++it below */) {
      HInstruction* user = it->GetUser();
      // All other uses must be dominated.
      DCHECK(implicit_clinit->StrictlyDominates(user) || (implicit_clinit == user));
      ++it;  // Advance before we remove the node, reference to the next node is preserved.
      if (user->IsInvokeStaticOrDirect()) {
        user->AsInvokeStaticOrDirect()->RemoveExplicitClinitCheck(
            HInvokeStaticOrDirect::ClinitCheckRequirement::kNone);
      }
    }
  }

  HLoadClass* load_class = check->GetLoadClass();
  bool can_merge_with_load_class = CanMoveClinitCheck(load_class, check);

  check->ReplaceWith(load_class);

  if (implicit_clinit != nullptr) {
    // Remove the check from the graph. It has been merged into the invoke or new-instance.
    check->GetBlock()->RemoveInstruction(check);
    // Check if we can merge the load class as well.
    if (can_merge_with_load_class && !load_class->HasUses()) {
      load_class->GetBlock()->RemoveInstruction(load_class);
    }
  } else if (can_merge_with_load_class && !load_class->NeedsAccessCheck()) {
    // Pass the initialization duty to the `HLoadClass` instruction,
    // and remove the instruction from the graph.
    load_class->SetMustGenerateClinitCheck(true);
    check->GetBlock()->RemoveInstruction(check);
  }
}

void PrepareForRegisterAllocation::VisitNewInstance(HNewInstance* instruction) {
  HLoadClass* load_class = instruction->InputAt(0)->AsLoadClass();
  bool has_only_one_use = load_class->HasOnlyOneNonEnvironmentUse();
  // Change the entrypoint to kQuickAllocObject if either:
  // - the class is finalizable (only kQuickAllocObject handles finalizable classes),
  // - the class needs access checks (we do not know if it's finalizable),
  // - or the load class has only one use.
  if (instruction->IsFinalizable() || has_only_one_use || load_class->NeedsAccessCheck()) {
    instruction->SetEntrypoint(kQuickAllocObject);
    instruction->ReplaceInput(GetGraph()->GetIntConstant(load_class->GetTypeIndex()), 0);
    // The allocation entry point that deals with access checks does not work with inlined
    // methods, so we need to check whether this allocation comes from an inlined method.
    // We also need to make the same check as for moving clinit check, whether the HLoadClass
    // has the clinit check responsibility or not (HLoadClass can throw anyway).
    if (has_only_one_use &&
        !instruction->GetEnvironment()->IsFromInlinedInvoke() &&
        CanMoveClinitCheck(load_class, instruction)) {
      // We can remove the load class from the graph. If it needed access checks, we delegate
      // the access check to the allocation.
      if (load_class->NeedsAccessCheck()) {
        instruction->SetEntrypoint(kQuickAllocObjectWithAccessCheck);
      }
      load_class->GetBlock()->RemoveInstruction(load_class);
    }
  }
}

bool PrepareForRegisterAllocation::CanEmitConditionAt(HCondition* condition,
                                                      HInstruction* user) const {
  if (condition->GetNext() != user) {
    return false;
  }

  if (user->IsIf() || user->IsDeoptimize()) {
    return true;
  }

  if (user->IsSelect() && user->AsSelect()->GetCondition() == condition) {
    return true;
  }

  return false;
}

void PrepareForRegisterAllocation::VisitCondition(HCondition* condition) {
  if (condition->HasOnlyOneNonEnvironmentUse()) {
    HInstruction* user = condition->GetUses().front().GetUser();
    if (CanEmitConditionAt(condition, user)) {
      condition->MarkEmittedAtUseSite();
    }
  }
}

void PrepareForRegisterAllocation::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  if (invoke->IsStaticWithExplicitClinitCheck()) {
    size_t last_input_index = invoke->InputCount() - 1;
    HLoadClass* last_input = invoke->InputAt(last_input_index)->AsLoadClass();
    DCHECK(last_input != nullptr)
        << "Last input is not HLoadClass. It is " << last_input->DebugName();

    // Detach the explicit class initialization check from the invoke.
    // Keeping track of the initializing instruction is no longer required
    // at this stage (i.e., after inlining has been performed).
    invoke->RemoveExplicitClinitCheck(HInvokeStaticOrDirect::ClinitCheckRequirement::kNone);

    // Merging with load class should have happened in VisitClinitCheck().
    DCHECK(!CanMoveClinitCheck(last_input, invoke));
  }
}

bool PrepareForRegisterAllocation::CanMoveClinitCheck(HInstruction* input,
                                                      HInstruction* user) const {
  // Determine if input and user come from the same dex instruction, so that we can move
  // the clinit check responsibility from one to the other, i.e. from HClinitCheck (user)
  // to HLoadClass (input), or from HClinitCheck (input) to HInvokeStaticOrDirect (user),
  // or from HLoadClass (input) to HNewInstance (user).

  // Start with a quick dex pc check.
  if (user->GetDexPc() != input->GetDexPc()) {
    return false;
  }

  // Now do a thorough environment check that this is really coming from the same instruction in
  // the same inlined graph. Unfortunately, we have to go through the whole environment chain.
  HEnvironment* user_environment = user->GetEnvironment();
  HEnvironment* input_environment = input->GetEnvironment();
  while (user_environment != nullptr || input_environment != nullptr) {
    if (user_environment == nullptr || input_environment == nullptr) {
      // Different environment chain length. This happens when a method is called
      // once directly and once indirectly through another inlined method.
      return false;
    }
    if (user_environment->GetDexPc() != input_environment->GetDexPc() ||
        user_environment->GetMethodIdx() != input_environment->GetMethodIdx() ||
        !IsSameDexFile(user_environment->GetDexFile(), input_environment->GetDexFile())) {
      return false;
    }
    user_environment = user_environment->GetParent();
    input_environment = input_environment->GetParent();
  }

  // Check for code motion taking the input to a different block.
  if (user->GetBlock() != input->GetBlock()) {
    return false;
  }

  // In debug mode, check that we have not inserted a throwing instruction
  // or an instruction with side effects between input and user.
  if (kIsDebugBuild) {
    for (HInstruction* between = input->GetNext(); between != user; between = between->GetNext()) {
      CHECK(between != nullptr);  // User must be after input in the same block.
      CHECK(!between->CanThrow());
      CHECK(!between->HasSideEffects());
    }
  }
  return true;
}

}  // namespace art
