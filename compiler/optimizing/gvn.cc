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

void GlobalValueNumberer::VisitBasicBlock(HBasicBlock* block) {
  ValueSet* set = nullptr;
  const GrowableArray<HBasicBlock*>& predecessors = block->GetPredecessors();
  if (predecessors.Size() == 0 || predecessors.Get(0)->IsEntryBlock()) {
    // The entry block should only accumulate constant instructions, and
    // the builder puts constants only in the entry block.
    // Therefore, there is no need to propagate the value set to the next block.
    set = new (allocator_) ValueSet(allocator_);
  } else {
    HBasicBlock* dominator = block->GetDominator();
    set = sets_.Get(dominator->GetBlockId())->Copy();
    if (dominator->GetSuccessors().Size() != 1 || dominator->GetSuccessors().Get(0) != block) {
      // We have to copy if the dominator has other successors, or `block` is not a successor
      // of the dominator.
      set = set->Copy();
    }
    if (!set->IsEmpty()) {
      if (block->IsLoopHeader()) {
        DCHECK_EQ(block->GetDominator(), block->GetLoopInformation()->GetPreHeader());
        set->Kill(GetLoopEffects(block));
      } else if (predecessors.Size() > 1) {
        for (size_t i = 0, e = predecessors.Size(); i < e; ++i) {
          set->IntersectionWith(sets_.Get(predecessors.Get(i)->GetBlockId()));
          if (set->IsEmpty()) {
            break;
          }
        }
      }
    }
  }

  sets_.Put(block->GetBlockId(), set);

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
}

}  // namespace art
