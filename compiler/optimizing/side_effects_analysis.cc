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

#include "side_effects_analysis.h"

namespace art {

void SideEffectsAnalysis::Run() {
  // Inlining might have created more blocks, so we need to increase the size
  // if needed.
  block_effects_.SetSize(graph_->GetBlocks().Size());
  loop_effects_.SetSize(graph_->GetBlocks().Size());

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
  has_run_ = true;
}

SideEffects SideEffectsAnalysis::GetLoopEffects(HBasicBlock* block) const {
  DCHECK(block->IsLoopHeader());
  return loop_effects_.Get(block->GetBlockId());
}

SideEffects SideEffectsAnalysis::GetBlockEffects(HBasicBlock* block) const {
  return block_effects_.Get(block->GetBlockId());
}

void SideEffectsAnalysis::UpdateLoopEffects(HLoopInformation* info, SideEffects effects) {
  int id = info->GetHeader()->GetBlockId();
  loop_effects_.Put(id, loop_effects_.Get(id).Union(effects));
}

}  // namespace art
