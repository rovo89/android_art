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

#include "ssa_phi_elimination.h"

namespace art {

void SsaDeadPhiElimination::Run() {
  MarkDeadPhis();
  EliminateDeadPhis();
}

void SsaDeadPhiElimination::MarkDeadPhis() {
  // Add to the worklist phis referenced by non-phi instructions.
  for (HReversePostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    for (HInstructionIterator inst_it(block->GetPhis()); !inst_it.Done(); inst_it.Advance()) {
      HPhi* phi = inst_it.Current()->AsPhi();
      // Set dead ahead of running through uses. The phi may have no use.
      phi->SetDead();
      for (HUseIterator<HInstruction*> use_it(phi->GetUses()); !use_it.Done(); use_it.Advance()) {
        HUseListNode<HInstruction*>* current = use_it.Current();
        HInstruction* user = current->GetUser();
        if (!user->IsPhi()) {
          worklist_.Add(phi);
          phi->SetLive();
          break;
        }
      }
    }
  }

  // Process the worklist by propagating liveness to phi inputs.
  while (!worklist_.IsEmpty()) {
    HPhi* phi = worklist_.Pop();
    for (HInputIterator it(phi); !it.Done(); it.Advance()) {
      HInstruction* input = it.Current();
      if (input->IsPhi() && input->AsPhi()->IsDead()) {
        worklist_.Add(input->AsPhi());
        input->AsPhi()->SetLive();
      }
    }
  }
}

void SsaDeadPhiElimination::EliminateDeadPhis() {
  // Remove phis that are not live. Visit in post order so that phis
  // that are not inputs of loop phis can be removed when they have
  // no users left (dead phis might use dead phis).
  for (HPostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    HInstruction* current = block->GetFirstPhi();
    HInstruction* next = nullptr;
    HPhi* phi;
    while (current != nullptr) {
      phi = current->AsPhi();
      next = current->GetNext();
      if (phi->IsDead()) {
        // Make sure the phi is only used by other dead phis.
        if (kIsDebugBuild) {
          for (HUseIterator<HInstruction*> use_it(phi->GetUses()); !use_it.Done();
               use_it.Advance()) {
            HInstruction* user = use_it.Current()->GetUser();
            DCHECK(user->IsLoopHeaderPhi()) << user->GetId();
            DCHECK(user->AsPhi()->IsDead()) << user->GetId();
          }
        }
        // Remove the phi from use lists of its inputs.
        for (size_t i = 0, e = phi->InputCount(); i < e; ++i) {
          phi->RemoveAsUserOfInput(i);
        }
        // Remove the phi from environments that use it.
        for (HUseIterator<HEnvironment*> use_it(phi->GetEnvUses()); !use_it.Done();
             use_it.Advance()) {
          HUseListNode<HEnvironment*>* user_node = use_it.Current();
          HEnvironment* user = user_node->GetUser();
          user->SetRawEnvAt(user_node->GetIndex(), nullptr);
        }
        // Delete it from the instruction list.
        block->RemovePhi(phi, /*ensure_safety=*/ false);
      }
      current = next;
    }
  }
}

void SsaRedundantPhiElimination::Run() {
  // Add all phis in the worklist.
  for (HReversePostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    for (HInstructionIterator inst_it(block->GetPhis()); !inst_it.Done(); inst_it.Advance()) {
      worklist_.Add(inst_it.Current()->AsPhi());
    }
  }

  while (!worklist_.IsEmpty()) {
    HPhi* phi = worklist_.Pop();

    // If the phi has already been processed, continue.
    if (!phi->IsInBlock()) {
      continue;
    }

    // Find if the inputs of the phi are the same instruction.
    HInstruction* candidate = phi->InputAt(0);
    // A loop phi cannot have itself as the first phi. Note that this
    // check relies on our simplification pass ensuring the pre-header
    // block is first in the list of predecessors of the loop header.
    DCHECK(!phi->IsLoopHeaderPhi() || phi->GetBlock()->IsLoopPreHeaderFirstPredecessor());
    DCHECK_NE(phi, candidate);

    for (size_t i = 1; i < phi->InputCount(); ++i) {
      HInstruction* input = phi->InputAt(i);
      // For a loop phi, if the input is the phi, the phi is still candidate for
      // elimination.
      if (input != candidate && input != phi) {
        candidate = nullptr;
        break;
      }
    }

    // If the inputs are not the same, continue.
    if (candidate == nullptr) {
      continue;
    }

    if (phi->IsInLoop()) {
      // Because we're updating the users of this phi, we may have new
      // phis candidate for elimination if this phi is in a loop. Add phis that
      // used this phi to the worklist.
      for (HUseIterator<HInstruction*> it(phi->GetUses()); !it.Done(); it.Advance()) {
        HUseListNode<HInstruction*>* current = it.Current();
        HInstruction* user = current->GetUser();
        if (user->IsPhi()) {
          worklist_.Add(user->AsPhi());
        }
      }
    }
    phi->ReplaceWith(candidate);
    phi->GetBlock()->RemovePhi(phi);
  }
}

}  // namespace art
