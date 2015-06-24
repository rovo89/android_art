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

#include "base/bit_vector-inl.h"

namespace art {

static void MarkReachableBlocks(HBasicBlock* block, ArenaBitVector* visited) {
  int block_id = block->GetBlockId();
  if (visited->IsBitSet(block_id)) {
    return;
  }
  visited->SetBit(block_id);

  HInstruction* last_instruction = block->GetLastInstruction();
  if (last_instruction->IsIf()) {
    HIf* if_instruction = last_instruction->AsIf();
    HInstruction* condition = if_instruction->InputAt(0);
    if (!condition->IsIntConstant()) {
      MarkReachableBlocks(if_instruction->IfTrueSuccessor(), visited);
      MarkReachableBlocks(if_instruction->IfFalseSuccessor(), visited);
    } else if (condition->AsIntConstant()->IsOne()) {
      MarkReachableBlocks(if_instruction->IfTrueSuccessor(), visited);
    } else {
      DCHECK(condition->AsIntConstant()->IsZero());
      MarkReachableBlocks(if_instruction->IfFalseSuccessor(), visited);
    }
  } else {
    for (size_t i = 0, e = block->GetSuccessors().Size(); i < e; ++i) {
      MarkReachableBlocks(block->GetSuccessors().Get(i), visited);
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
  ArenaBitVector live_blocks(allocator, graph_->GetBlocks().Size(), false);
  ArenaBitVector affected_loops(allocator, graph_->GetBlocks().Size(), false);

  MarkReachableBlocks(graph_->GetEntryBlock(), &live_blocks);
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
  // dominator tree.
  if (removed_one_or_more_blocks) {
    graph_->ClearDominanceInformation();
    graph_->ComputeDominanceInformation();
  }

  // Connect successive blocks created by dead branches. Order does not matter.
  for (HReversePostOrderIterator it(*graph_); !it.Done();) {
    HBasicBlock* block  = it.Current();
    if (block->IsEntryBlock() || block->GetSuccessors().Size() != 1u) {
      it.Advance();
      continue;
    }
    HBasicBlock* successor = block->GetSuccessors().Get(0);
    if (successor->IsExitBlock() || successor->GetPredecessors().Size() != 1u) {
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
          && !inst->IsMemoryBarrier()  // If we added an explicit barrier then we should keep it.
          && !inst->HasUses()) {
        block->RemoveInstruction(inst);
        MaybeRecordStat(MethodCompilationStat::kRemovedDeadInstruction);
      }
    }
  }
}

void HDeadCodeElimination::Run() {
  RemoveDeadBlocks();
  RemoveDeadInstructions();
}

}  // namespace art
