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

#include "primitive_type_propagation.h"

#include "nodes.h"
#include "ssa_builder.h"

namespace art {

bool PrimitiveTypePropagation::TypePhiFromInputs(HPhi* phi) {
  Primitive::Type common_type = phi->GetType();

  for (HInputIterator it(phi); !it.Done(); it.Advance()) {
    HInstruction* input = it.Current();
    if (input->IsPhi() && input->AsPhi()->IsDead()) {
      // Phis are constructed live so if an input is a dead phi, it must have
      // been made dead due to type conflict. Mark this phi conflicting too.
      return false;
    }

    Primitive::Type input_type = HPhi::ToPhiType(input->GetType());
    if (common_type == Primitive::kPrimVoid) {
      // Setting type for the first time.
      common_type = input_type;
    } else if (common_type == input_type) {
      // No change in type.
    } else if (input_type == Primitive::kPrimVoid) {
      // Input is a phi which has not been typed yet. Do nothing.
      DCHECK(input->IsPhi());
    } else if (Primitive::ComponentSize(common_type) != Primitive::ComponentSize(input_type)) {
      // Types are of different sizes, e.g. int vs. long. Must be a conflict.
      return false;
    } else if (Primitive::IsIntegralType(common_type)) {
      // Previous inputs were integral, this one is not but is of the same size.
      // This does not imply conflict since some bytecode instruction types are
      // ambiguous. TypeInputsOfPhi will either type them or detect a conflict.
      DCHECK(Primitive::IsFloatingPointType(input_type) || input_type == Primitive::kPrimNot);
      common_type = input_type;
    } else if (Primitive::IsIntegralType(input_type)) {
      // Input is integral, common type is not. Same as in the previous case, if
      // there is a conflict, it will be detected during TypeInputsOfPhi.
      DCHECK(Primitive::IsFloatingPointType(common_type) || common_type == Primitive::kPrimNot);
    } else {
      // Combining float and reference types. Clearly a conflict.
      DCHECK((common_type == Primitive::kPrimFloat && input_type == Primitive::kPrimNot) ||
             (common_type == Primitive::kPrimNot && input_type == Primitive::kPrimFloat));
      return false;
    }
  }

  // We have found a candidate type for the phi. Set it and return true. We may
  // still discover conflict whilst typing the individual inputs in TypeInputsOfPhi.
  phi->SetType(common_type);
  return true;
}

bool PrimitiveTypePropagation::TypeInputsOfPhi(HPhi* phi) {
  Primitive::Type common_type = phi->GetType();
  if (common_type == Primitive::kPrimVoid || Primitive::IsIntegralType(common_type)) {
    // Phi either contains only other untyped phis (common_type == kPrimVoid),
    // or `common_type` is integral and we do not need to retype ambiguous inputs
    // because they are always constructed with the integral type candidate.
    if (kIsDebugBuild) {
      for (size_t i = 0, e = phi->InputCount(); i < e; ++i) {
        HInstruction* input = phi->InputAt(i);
        if (common_type == Primitive::kPrimVoid) {
          DCHECK(input->IsPhi() && input->GetType() == Primitive::kPrimVoid);
        } else {
          DCHECK((input->IsPhi() && input->GetType() == Primitive::kPrimVoid) ||
                 HPhi::ToPhiType(input->GetType()) == common_type);
        }
      }
    }
    // Inputs did not need to be replaced, hence no conflict. Report success.
    return true;
  } else {
    DCHECK(common_type == Primitive::kPrimNot || Primitive::IsFloatingPointType(common_type));
    for (size_t i = 0, e = phi->InputCount(); i < e; ++i) {
      HInstruction* input = phi->InputAt(i);
      if (input->GetType() != common_type) {
        // Input type does not match phi's type. Try to retype the input or
        // generate a suitably typed equivalent.
        HInstruction* equivalent = (common_type == Primitive::kPrimNot)
            ? SsaBuilder::GetReferenceTypeEquivalent(input)
            : SsaBuilder::GetFloatOrDoubleEquivalent(phi, input, common_type);
        if (equivalent == nullptr) {
          // Input could not be typed. Report conflict.
          return false;
        }

        phi->ReplaceInput(equivalent, i);
        if (equivalent->IsPhi()) {
          AddToWorklist(equivalent->AsPhi());
        } else if (equivalent == input) {
          // The input has changed its type. It can be an input of other phis,
          // so we need to put phi users in the work list.
          AddDependentInstructionsToWorklist(input);
        }
      }
    }
    // All inputs either matched the type of the phi or we successfully replaced
    // them with a suitable equivalent. Report success.
    return true;
  }
}

bool PrimitiveTypePropagation::UpdateType(HPhi* phi) {
  DCHECK(phi->IsLive());
  Primitive::Type original_type = phi->GetType();

  // Try to type the phi in two stages:
  // (1) find a candidate type for the phi by merging types of all its inputs,
  // (2) try to type the phi's inputs to that candidate type.
  // Either of these stages may detect a type conflict and fail, in which case
  // we immediately abort.
  if (!TypePhiFromInputs(phi) || !TypeInputsOfPhi(phi)) {
    // Conflict detected. Mark the phi dead and return true because it changed.
    phi->SetDead();
    return true;
  }

  // Return true if the type of the phi has changed.
  return phi->GetType() != original_type;
}

void PrimitiveTypePropagation::Run() {
  for (HReversePostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    VisitBasicBlock(it.Current());
  }
  ProcessWorklist();
}

void PrimitiveTypePropagation::VisitBasicBlock(HBasicBlock* block) {
  if (block->IsLoopHeader()) {
    for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
      HPhi* phi = it.Current()->AsPhi();
      if (phi->IsLive()) {
        AddToWorklist(phi);
      }
    }
  } else {
    for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
      // Eagerly compute the type of the phi, for quicker convergence. Note
      // that we don't need to add users to the worklist because we are
      // doing a reverse post-order visit, therefore either the phi users are
      // non-loop phi and will be visited later in the visit, or are loop-phis,
      // and they are already in the work list.
      HPhi* phi = it.Current()->AsPhi();
      if (phi->IsLive()) {
        UpdateType(phi);
      }
    }
  }
}

void PrimitiveTypePropagation::ProcessWorklist() {
  while (!worklist_.empty()) {
    HPhi* phi = worklist_.back();
    worklist_.pop_back();
    // The phi could have been made dead as a result of conflicts while in the
    // worklist. If it is now dead, there is no point in updating its type.
    if (phi->IsLive() && UpdateType(phi)) {
      AddDependentInstructionsToWorklist(phi);
    }
  }
}

void PrimitiveTypePropagation::AddToWorklist(HPhi* instruction) {
  DCHECK(instruction->IsLive());
  worklist_.push_back(instruction);
}

void PrimitiveTypePropagation::AddDependentInstructionsToWorklist(HInstruction* instruction) {
  // If `instruction` is a dead phi, type conflict was just identified. All its
  // live phi users, and transitively users of those users, therefore need to be
  // marked dead/conflicting too, so we add them to the worklist. Otherwise we
  // add users whose type does not match and needs to be updated.
  bool add_all_live_phis = instruction->IsPhi() && instruction->AsPhi()->IsDead();
  for (HUseIterator<HInstruction*> it(instruction->GetUses()); !it.Done(); it.Advance()) {
    HInstruction* user = it.Current()->GetUser();
    if (user->IsPhi() && user->AsPhi()->IsLive()) {
      if (add_all_live_phis || user->GetType() != instruction->GetType()) {
        AddToWorklist(user->AsPhi());
      }
    }
  }
}

}  // namespace art
