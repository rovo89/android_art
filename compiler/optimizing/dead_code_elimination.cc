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

#include "dead_code_elimination.h"

#include "utils/array_ref.h"
#include "base/bit_vector-inl.h"
#include "ssa_phi_elimination.h"

namespace art {

static void MarkReachableBlocks(HGraph* graph, ArenaBitVector* visited) {
  ArenaVector<HBasicBlock*> worklist(graph->GetArena()->Adapter());
  constexpr size_t kDefaultWorlistSize = 8;
  worklist.reserve(kDefaultWorlistSize);
  visited->SetBit(graph->GetEntryBlock()->GetBlockId());
  worklist.push_back(graph->GetEntryBlock());

  while (!worklist.empty()) {
    HBasicBlock* block = worklist.back();
    worklist.pop_back();
    int block_id = block->GetBlockId();
    DCHECK(visited->IsBitSet(block_id));

    ArrayRef<HBasicBlock* const> live_successors(block->GetSuccessors());
    HInstruction* last_instruction = block->GetLastInstruction();
    if (last_instruction->IsIf()) {
      HIf* if_instruction = last_instruction->AsIf();
      HInstruction* condition = if_instruction->InputAt(0);
      if (condition->IsIntConstant()) {
        if (condition->AsIntConstant()->IsOne()) {
          live_successors = live_successors.SubArray(0u, 1u);
          DCHECK_EQ(live_successors[0], if_instruction->IfTrueSuccessor());
        } else {
          DCHECK(condition->AsIntConstant()->IsZero());
          live_successors = live_successors.SubArray(1u, 1u);
          DCHECK_EQ(live_successors[0], if_instruction->IfFalseSuccessor());
        }
      }
    } else if (last_instruction->IsPackedSwitch()) {
      HPackedSwitch* switch_instruction = last_instruction->AsPackedSwitch();
      HInstruction* switch_input = switch_instruction->InputAt(0);
      if (switch_input->IsIntConstant()) {
        int32_t switch_value = switch_input->AsIntConstant()->GetValue();
        int32_t start_value = switch_instruction->GetStartValue();
        // Note: Though the spec forbids packed-switch values to wrap around, we leave
        // that task to the verifier and use unsigned arithmetic with it's "modulo 2^32"
        // semantics to check if the value is in range, wrapped or not.
        uint32_t switch_index =
            static_cast<uint32_t>(switch_value) - static_cast<uint32_t>(start_value);
        if (switch_index < switch_instruction->GetNumEntries()) {
          live_successors = live_successors.SubArray(switch_index, 1u);
          DCHECK_EQ(live_successors[0], block->GetSuccessors()[switch_index]);
        } else {
          live_successors = live_successors.SubArray(switch_instruction->GetNumEntries(), 1u);
          DCHECK_EQ(live_successors[0], switch_instruction->GetDefaultBlock());
        }
      }
    }

    for (HBasicBlock* successor : live_successors) {
      // Add only those successors that have not been visited yet.
      if (!visited->IsBitSet(successor->GetBlockId())) {
        visited->SetBit(successor->GetBlockId());
        worklist.push_back(successor);
      }
    }
  }
}

static void MarkLoopHeadersContaining(const HBasicBlock& block, ArenaBitVector* set) {
  for (HLoopInformationOutwardIterator it(block); !it.Done(); it.Advance()) {
    set->SetBit(it.Current()->GetHeader()->GetBlockId());
  }
}

void HDeadCodeElimination::MaybeRecordDeadBlock(HBasicBlock* block) {
  if (stats_ != nullptr) {
    stats_->RecordStat(MethodCompilationStat::kRemovedDeadInstruction,
                       block->GetPhis().CountSize() + block->GetInstructions().CountSize());
  }
}

void HDeadCodeElimination::RemoveDeadBlocks() {
  // Classify blocks as reachable/unreachable.
  ArenaAllocator* allocator = graph_->GetArena();
  ArenaBitVector live_blocks(allocator, graph_->GetBlocks().size(), false);
  ArenaBitVector affected_loops(allocator, graph_->GetBlocks().size(), false);

  MarkReachableBlocks(graph_, &live_blocks);
  bool removed_one_or_more_blocks = false;

  // Remove all dead blocks. Iterate in post order because removal needs the
  // block's chain of dominators and nested loops need to be updated from the
  // inside out.
  for (HPostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    HBasicBlock* block  = it.Current();
    int id = block->GetBlockId();
    if (live_blocks.IsBitSet(id)) {
      if (affected_loops.IsBitSet(id)) {
        DCHECK(block->IsLoopHeader());
        block->GetLoopInformation()->Update();
      }
    } else {
      MaybeRecordDeadBlock(block);
      MarkLoopHeadersContaining(*block, &affected_loops);
      block->DisconnectAndDelete();
      removed_one_or_more_blocks = true;
    }
  }

  // If we removed at least one block, we need to recompute the full
  // dominator tree and try block membership.
  if (removed_one_or_more_blocks) {
    graph_->ClearDominanceInformation();
    graph_->ComputeDominanceInformation();
    graph_->ComputeTryBlockInformation();
  }

  // Connect successive blocks created by dead branches. Order does not matter.
  for (HReversePostOrderIterator it(*graph_); !it.Done();) {
    HBasicBlock* block  = it.Current();
    if (block->IsEntryBlock() || !block->GetLastInstruction()->IsGoto()) {
      it.Advance();
      continue;
    }
    HBasicBlock* successor = block->GetSingleSuccessor();
    if (successor->IsExitBlock() || successor->GetPredecessors().size() != 1u) {
      it.Advance();
      continue;
    }
    block->MergeWith(successor);

    // Reiterate on this block in case it can be merged with its new successor.
  }
}

void HDeadCodeElimination::RemoveDeadInstructions() {
  // Process basic blocks in post-order in the dominator tree, so that
  // a dead instruction depending on another dead instruction is removed.
  for (HPostOrderIterator b(*graph_); !b.Done(); b.Advance()) {
    HBasicBlock* block = b.Current();
    // Traverse this block's instructions in backward order and remove
    // the unused ones.
    HBackwardInstructionIterator i(block->GetInstructions());
    // Skip the first iteration, as the last instruction of a block is
    // a branching instruction.
    DCHECK(i.Current()->IsControlFlow());
    for (i.Advance(); !i.Done(); i.Advance()) {
      HInstruction* inst = i.Current();
      DCHECK(!inst->IsControlFlow());
      if (!inst->HasSideEffects()
          && !inst->CanThrow()
          && !inst->IsSuspendCheck()
          // If we added an explicit barrier then we should keep it.
          && !inst->IsMemoryBarrier()
          && !inst->IsParameterValue()
          && !inst->HasUses()) {
        block->RemoveInstruction(inst);
        MaybeRecordStat(MethodCompilationStat::kRemovedDeadInstruction);
      }
    }
  }
}

void HDeadCodeElimination::Run() {
  RemoveDeadBlocks();
  SsaRedundantPhiElimination(graph_).Run();
  RemoveDeadInstructions();
}

}  // namespace art
