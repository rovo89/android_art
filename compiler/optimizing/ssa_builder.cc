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

#include "nodes.h"
#include "ssa_type_propagation.h"

namespace art {

void SsaBuilder::BuildSsa() {
  // 1) Visit in reverse post order. We need to have all predecessors of a block visited
  // (with the exception of loops) in order to create the right environment for that
  // block. For loops, we create phis whose inputs will be set in 2).
  for (HReversePostOrderIterator it(*GetGraph()); !it.Done(); it.Advance()) {
    VisitBasicBlock(it.Current());
  }

  // 2) Set inputs of loop phis.
  for (size_t i = 0; i < loop_headers_.Size(); i++) {
    HBasicBlock* block = loop_headers_.Get(i);
    for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
      HPhi* phi = it.Current()->AsPhi();
      for (size_t pred = 0; pred < block->GetPredecessors().Size(); pred++) {
        HInstruction* input = ValueOfLocal(block->GetPredecessors().Get(pred), phi->GetRegNumber());
        phi->AddInput(input);
      }
    }
  }

  // 3) Propagate types of phis.
  SsaTypePropagation type_propagation(GetGraph());
  type_propagation.Run();

  // 4) Clear locals.
  // TODO: Move this to a dead code eliminator phase.
  for (HInstructionIterator it(GetGraph()->GetEntryBlock()->GetInstructions());
       !it.Done();
       it.Advance()) {
    HInstruction* current = it.Current();
    if (current->AsLocal() != nullptr) {
      current->GetBlock()->RemoveInstruction(current);
    }
  }
}

HInstruction* SsaBuilder::ValueOfLocal(HBasicBlock* block, size_t local) {
  return GetLocalsFor(block)->Get(local);
}

void SsaBuilder::VisitBasicBlock(HBasicBlock* block) {
  current_locals_ = GetLocalsFor(block);

  if (block->IsLoopHeader()) {
    // If the block is a loop header, we know we only have visited the pre header
    // because we are visiting in reverse post order. We create phis for all initialized
    // locals from the pre header. Their inputs will be populated at the end of
    // the analysis.
    for (size_t local = 0; local < current_locals_->Size(); local++) {
      HInstruction* incoming = ValueOfLocal(block->GetLoopInformation()->GetPreHeader(), local);
      if (incoming != nullptr) {
        HPhi* phi = new (GetGraph()->GetArena()) HPhi(
            GetGraph()->GetArena(), local, 0, Primitive::kPrimVoid);
        block->AddPhi(phi);
        current_locals_->Put(local, phi);
      }
    }
    // Save the loop header so that the last phase of the analysis knows which
    // blocks need to be updated.
    loop_headers_.Add(block);
  } else if (block->GetPredecessors().Size() > 0) {
    // All predecessors have already been visited because we are visiting in reverse post order.
    // We merge the values of all locals, creating phis if those values differ.
    for (size_t local = 0; local < current_locals_->Size(); local++) {
      bool one_predecessor_has_no_value = false;
      bool is_different = false;
      HInstruction* value = ValueOfLocal(block->GetPredecessors().Get(0), local);

      for (size_t i = 0, e = block->GetPredecessors().Size(); i < e; ++i) {
        HInstruction* current = ValueOfLocal(block->GetPredecessors().Get(i), local);
        if (current == nullptr) {
          one_predecessor_has_no_value = true;
          break;
        } else if (current != value) {
          is_different = true;
        }
      }

      if (one_predecessor_has_no_value) {
        // If one predecessor has no value for this local, we trust the verifier has
        // successfully checked that there is a store dominating any read after this block.
        continue;
      }

      if (is_different) {
        HPhi* phi = new (GetGraph()->GetArena()) HPhi(
            GetGraph()->GetArena(), local, block->GetPredecessors().Size(), Primitive::kPrimVoid);
        for (size_t i = 0; i < block->GetPredecessors().Size(); i++) {
          HInstruction* value = ValueOfLocal(block->GetPredecessors().Get(i), local);
          phi->SetRawInputAt(i, value);
        }
        block->AddPhi(phi);
        value = phi;
      }
      current_locals_->Put(local, value);
    }
  }

  // Visit all instructions. The instructions of interest are:
  // - HLoadLocal: replace them with the current value of the local.
  // - HStoreLocal: update current value of the local and remove the instruction.
  // - Instructions that require an environment: populate their environment
  //   with the current values of the locals.
  for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
    it.Current()->Accept(this);
  }
}

void SsaBuilder::VisitLoadLocal(HLoadLocal* load) {
  load->ReplaceWith(current_locals_->Get(load->GetLocal()->GetRegNumber()));
  load->GetBlock()->RemoveInstruction(load);
}

void SsaBuilder::VisitStoreLocal(HStoreLocal* store) {
  current_locals_->Put(store->GetLocal()->GetRegNumber(), store->InputAt(1));
  store->GetBlock()->RemoveInstruction(store);
}

void SsaBuilder::VisitInstruction(HInstruction* instruction) {
  if (!instruction->NeedsEnvironment()) {
    return;
  }
  HEnvironment* environment = new (GetGraph()->GetArena()) HEnvironment(
      GetGraph()->GetArena(), current_locals_->Size());
  environment->Populate(*current_locals_);
  instruction->SetEnvironment(environment);
}

}  // namespace art
