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

#include "gvn.h"

namespace art {

void GlobalValueNumberer::Run() {
  ComputeSideEffects();

  sets_.Put(graph_->GetEntryBlock()->GetBlockId(), new (allocator_) ValueSet(allocator_));

  // Do reverse post order to ensure the non back-edge predecessors of a block are
  // visited before the block itself.
  for (HReversePostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    VisitBasicBlock(it.Current());
  }
}

void GlobalValueNumberer::UpdateLoopEffects(HLoopInformation* info, SideEffects effects) {
  int id = info->GetHeader()->GetBlockId();
  loop_effects_.Put(id, loop_effects_.Get(id).Union(effects));
}

void GlobalValueNumberer::ComputeSideEffects() {
  if (kIsDebugBuild) {
    for (HReversePostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
      HBasicBlock* block = it.Current();
      SideEffects effects = GetBlockEffects(block);
      DCHECK(!effects.HasSideEffects() && !effects.HasDependencies());
      if (block->IsLoopHeader()) {
        effects = GetLoopEffects(block);
        DCHECK(!effects.HasSideEffects() && !effects.HasDependencies());
      }
    }
  }

  // Do a post order visit to ensure we visit a loop header after its loop body.
  for (HPostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();

    SideEffects effects = SideEffects::None();
    // Update `effects` with the side effects of all instructions in this block.
    for (HInstructionIterator inst_it(block->GetInstructions()); !inst_it.Done();
         inst_it.Advance()) {
      HInstruction* instruction = inst_it.Current();
      effects = effects.Union(instruction->GetSideEffects());
      if (effects.HasAllSideEffects()) {
        break;
      }
    }

    block_effects_.Put(block->GetBlockId(), effects);

    if (block->IsLoopHeader()) {
      // The side effects of the loop header are part of the loop.
      UpdateLoopEffects(block->GetLoopInformation(), effects);
      HBasicBlock* pre_header = block->GetLoopInformation()->GetPreHeader();
      if (pre_header->IsInLoop()) {
        // Update the side effects of the outer loop with the side effects of the inner loop.
        // Note that this works because we know all the blocks of the inner loop are visited
        // before the loop header of the outer loop.
        UpdateLoopEffects(pre_header->GetLoopInformation(), GetLoopEffects(block));
      }
    } else if (block->IsInLoop()) {
      // Update the side effects of the loop with the side effects of this block.
      UpdateLoopEffects(block->GetLoopInformation(), effects);
    }
  }
}

SideEffects GlobalValueNumberer::GetLoopEffects(HBasicBlock* block) const {
  DCHECK(block->IsLoopHeader());
  return loop_effects_.Get(block->GetBlockId());
}

SideEffects GlobalValueNumberer::GetBlockEffects(HBasicBlock* block) const {
  return block_effects_.Get(block->GetBlockId());
}

static bool IsLoopExit(HBasicBlock* block, HBasicBlock* successor) {
  HLoopInformation* block_info = block->GetLoopInformation();
  HLoopInformation* other_info = successor->GetLoopInformation();
  return block_info != other_info && (other_info == nullptr || block_info->IsIn(*other_info));
}

void GlobalValueNumberer::VisitBasicBlock(HBasicBlock* block) {
  if (kIsDebugBuild) {
    // Check that all non back-edge processors have been visited.
    for (size_t i = 0, e = block->GetPredecessors().Size(); i < e; ++i) {
      HBasicBlock* predecessor = block->GetPredecessors().Get(i);
      DCHECK(visited_.Get(predecessor->GetBlockId())
             || (block->GetLoopInformation() != nullptr
                 && (block->GetLoopInformation()->GetBackEdges().Get(0) == predecessor)));
    }
    visited_.Put(block->GetBlockId(), true);
  }

  ValueSet* set = sets_.Get(block->GetBlockId());

  if (block->IsLoopHeader()) {
    set->Kill(GetLoopEffects(block));
  }

  HInstruction* current = block->GetFirstInstruction();
  while (current != nullptr) {
    set->Kill(current->GetSideEffects());
    // Save the next instruction in case `current` is removed from the graph.
    HInstruction* next = current->GetNext();
    if (current->CanBeMoved()) {
      HInstruction* existing = set->Lookup(current);
      if (existing != nullptr) {
        current->ReplaceWith(existing);
        current->GetBlock()->RemoveInstruction(current);
      } else {
        set->Add(current);
      }
    }
    current = next;
  }

  if (block == graph_->GetEntryBlock()) {
    // The entry block should only accumulate constant instructions, and
    // the builder puts constants only in the entry block.
    // Therefore, there is no need to propagate the value set to the next block.
    DCHECK_EQ(block->GetDominatedBlocks().Size(), 1u);
    HBasicBlock* dominated = block->GetDominatedBlocks().Get(0);
    sets_.Put(dominated->GetBlockId(), new (allocator_) ValueSet(allocator_));
    return;
  }

  // Copy the value set to dominated blocks. We can re-use
  // the current set for the last dominated block because we are done visiting
  // this block.
  for (size_t i = 0, e = block->GetDominatedBlocks().Size(); i < e; ++i) {
    HBasicBlock* dominated = block->GetDominatedBlocks().Get(i);
    sets_.Put(dominated->GetBlockId(), i == e - 1 ? set : set->Copy());
  }

  // Kill instructions in the value set of each successor. If the successor
  // is a loop exit, then we use the side effects of the loop. If not, we use
  // the side effects of this block.
  for (size_t i = 0, e = block->GetSuccessors().Size(); i < e; ++i) {
    HBasicBlock* successor = block->GetSuccessors().Get(i);
    if (successor->IsLoopHeader()
        && successor->GetLoopInformation()->GetBackEdges().Get(0) == block) {
      // In case of a back edge, we already have visited the loop header.
      // We should not update its value set, because the last dominated block
      // of the loop header uses the same value set.
      DCHECK(visited_.Get(successor->GetBlockId()));
      continue;
    }
    DCHECK(!visited_.Get(successor->GetBlockId()));
    ValueSet* successor_set = sets_.Get(successor->GetBlockId());
    // The dominator sets the set, and we are guaranteed to have visited it already.
    DCHECK(successor_set != nullptr);

    // If this block dominates this successor there is nothing to do.
    // Also if the set is empty, there is nothing to kill.
    if (successor->GetDominator() != block && !successor_set->IsEmpty()) {
      if (block->IsInLoop() && IsLoopExit(block, successor)) {
        // All instructions killed in the loop must be killed for a loop exit.
        SideEffects effects = GetLoopEffects(block->GetLoopInformation()->GetHeader());
        sets_.Get(successor->GetBlockId())->Kill(effects);
      } else {
        // Following block (that might be in the same loop).
        // Just kill instructions based on this block's side effects.
        sets_.Get(successor->GetBlockId())->Kill(GetBlockEffects(block));
      }
    }
  }
}

}  // namespace art
