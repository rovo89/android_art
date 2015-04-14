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

static Primitive::Type MergeTypes(Primitive::Type existing, Primitive::Type new_type) {
  // We trust the verifier has already done the necessary checking.
  switch (existing) {
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
    case Primitive::kPrimNot:
      return existing;
    default:
      // Phis are initialized with a void type, so if we are asked
      // to merge with a void type, we should use the existing one.
      return new_type == Primitive::kPrimVoid
          ? existing
          : HPhi::ToPhiType(new_type);
  }
}

// Re-compute and update the type of the instruction. Returns
// whether or not the type was changed.
bool PrimitiveTypePropagation::UpdateType(HPhi* phi) {
  DCHECK(phi->IsLive());
  Primitive::Type existing = phi->GetType();

  Primitive::Type new_type = existing;
  for (size_t i = 0, e = phi->InputCount(); i < e; ++i) {
    Primitive::Type input_type = phi->InputAt(i)->GetType();
    new_type = MergeTypes(new_type, input_type);
  }
  phi->SetType(new_type);

  if (new_type == Primitive::kPrimDouble
      || new_type == Primitive::kPrimFloat
      || new_type == Primitive::kPrimNot) {
    // If the phi is of floating point type, we need to update its inputs to that
    // type. For inputs that are phis, we need to recompute their types.
    for (size_t i = 0, e = phi->InputCount(); i < e; ++i) {
      HInstruction* input = phi->InputAt(i);
      if (input->GetType() != new_type) {
        HInstruction* equivalent = (new_type == Primitive::kPrimNot)
            ? SsaBuilder::GetReferenceTypeEquivalent(input)
            : SsaBuilder::GetFloatOrDoubleEquivalent(phi, input, new_type);
        phi->ReplaceInput(equivalent, i);
        if (equivalent->IsPhi()) {
          equivalent->AsPhi()->SetLive();
          AddToWorklist(equivalent->AsPhi());
        } else if (equivalent == input) {
          // The input has changed its type. It can be an input of other phis,
          // so we need to put phi users in the work list.
          AddDependentInstructionsToWorklist(equivalent);
        }
      }
    }
  }

  return existing != new_type;
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
  while (!worklist_.IsEmpty()) {
    HPhi* instruction = worklist_.Pop();
    if (UpdateType(instruction)) {
      AddDependentInstructionsToWorklist(instruction);
    }
  }
}

void PrimitiveTypePropagation::AddToWorklist(HPhi* instruction) {
  DCHECK(instruction->IsLive());
  worklist_.Add(instruction);
}

void PrimitiveTypePropagation::AddDependentInstructionsToWorklist(HInstruction* instruction) {
  for (HUseIterator<HInstruction*> it(instruction->GetUses()); !it.Done(); it.Advance()) {
    HPhi* phi = it.Current()->GetUser()->AsPhi();
    if (phi != nullptr && phi->IsLive() && phi->GetType() != instruction->GetType()) {
      AddToWorklist(phi);
    }
  }
}

}  // namespace art
