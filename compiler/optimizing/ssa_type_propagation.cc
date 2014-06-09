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
      return new_type;
  }
}

// Re-compute and update the type of the instruction. Returns
// whether or not the type was changed.
static bool UpdateType(HPhi* phi) {
  Primitive::Type existing = phi->GetType();

  Primitive::Type new_type = Primitive::kPrimVoid;
  for (size_t i = 0, e = phi->InputCount(); i < e; ++i) {
    Primitive::Type input_type = phi->InputAt(i)->GetType();
    new_type = MergeTypes(new_type, input_type);
  }
  phi->SetType(new_type);
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
      phi->SetType(phi->InputAt(0)->GetType());
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
