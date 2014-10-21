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

#include "ssa_builder.h"
#include "ssa_type_propagation.h"

#include "nodes.h"

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
          : new_type;
  }
}

// Re-compute and update the type of the instruction. Returns
// whether or not the type was changed.
bool SsaTypePropagation::UpdateType(HPhi* phi) {
  Primitive::Type existing = phi->GetType();

  Primitive::Type new_type = existing;
  for (size_t i = 0, e = phi->InputCount(); i < e; ++i) {
    Primitive::Type input_type = phi->InputAt(i)->GetType();
    new_type = MergeTypes(new_type, input_type);
  }
  phi->SetType(new_type);

  if (new_type == Primitive::kPrimDouble || new_type == Primitive::kPrimFloat) {
    // If the phi is of floating point type, we need to update its inputs to that
    // type. For inputs that are phis, we need to recompute their types.
    for (size_t i = 0, e = phi->InputCount(); i < e; ++i) {
      HInstruction* input = phi->InputAt(i);
      if (input->GetType() != new_type) {
        HInstruction* equivalent = SsaBuilder::GetFloatOrDoubleEquivalent(phi, input, new_type);
        phi->ReplaceInput(equivalent, i);
        if (equivalent->IsPhi()) {
          AddToWorklist(equivalent->AsPhi());
        }
      }
    }
  }

  return existing != new_type;
}

void SsaTypePropagation::Run() {
  for (HReversePostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    VisitBasicBlock(it.Current());
  }
  ProcessWorklist();
}

void SsaTypePropagation::VisitBasicBlock(HBasicBlock* block) {
  if (block->IsLoopHeader()) {
    for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
      HPhi* phi = it.Current()->AsPhi();
      // Set the initial type for the phi. Use the non back edge input for reaching
      // a fixed point faster.
      Primitive::Type phi_type = phi->GetType();
      // We merge with the existing type, that has been set by the SSA builder.
      DCHECK(phi_type == Primitive::kPrimVoid
          || phi_type == Primitive::kPrimFloat
          || phi_type == Primitive::kPrimDouble);
      phi->SetType(MergeTypes(phi->InputAt(0)->GetType(), phi->GetType()));
      AddToWorklist(phi);
    }
  } else {
    for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
      HPhi* phi = it.Current()->AsPhi();
      if (UpdateType(phi)) {
        AddDependentInstructionsToWorklist(phi);
      }
    }
  }
}

void SsaTypePropagation::ProcessWorklist() {
  while (!worklist_.IsEmpty()) {
    HPhi* instruction = worklist_.Pop();
    if (UpdateType(instruction)) {
      AddDependentInstructionsToWorklist(instruction);
    }
  }
}

void SsaTypePropagation::AddToWorklist(HPhi* instruction) {
  worklist_.Add(instruction);
}

void SsaTypePropagation::AddDependentInstructionsToWorklist(HPhi* instruction) {
  for (HUseIterator<HInstruction> it(instruction->GetUses()); !it.Done(); it.Advance()) {
    HPhi* phi = it.Current()->GetUser()->AsPhi();
    if (phi != nullptr) {
      AddToWorklist(phi);
    }
  }
}

}  // namespace art
