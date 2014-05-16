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

#include "ssa_liveness_analysis.h"
#include "nodes.h"

namespace art {

void SsaLivenessAnalysis::Analyze() {
  LinearizeGraph();
  NumberInstructions();
  ComputeSets();
}

static bool IsLoopExit(HLoopInformation* current, HLoopInformation* to) {
  // `to` is either not part of a loop, or `current` is an inner loop of `to`.
  return to == nullptr || (current != to && current->IsIn(*to));
}

static bool IsLoop(HLoopInformation* info) {
  return info != nullptr;
}

static bool InSameLoop(HLoopInformation* first_loop, HLoopInformation* second_loop) {
  return first_loop == second_loop;
}

static bool IsInnerLoop(HLoopInformation* outer, HLoopInformation* inner) {
  return (inner != outer)
      && (inner != nullptr)
      && (outer != nullptr)
      && inner->IsIn(*outer);
}

static void VisitBlockForLinearization(HBasicBlock* block,
                                       GrowableArray<HBasicBlock*>* order,
                                       ArenaBitVector* visited) {
  if (visited->IsBitSet(block->GetBlockId())) {
    return;
  }
  visited->SetBit(block->GetBlockId());
  size_t number_of_successors = block->GetSuccessors().Size();
  if (number_of_successors == 0) {
    // Nothing to do.
  } else if (number_of_successors == 1) {
    VisitBlockForLinearization(block->GetSuccessors().Get(0), order, visited);
  } else {
    DCHECK_EQ(number_of_successors, 2u);
    HBasicBlock* first_successor = block->GetSuccessors().Get(0);
    HBasicBlock* second_successor = block->GetSuccessors().Get(1);
    HLoopInformation* my_loop = block->GetLoopInformation();
    HLoopInformation* first_loop = first_successor->GetLoopInformation();
    HLoopInformation* second_loop = second_successor->GetLoopInformation();

    if (!IsLoop(my_loop)) {
      // Nothing to do. Current order is fine.
    } else if (IsLoopExit(my_loop, second_loop) && InSameLoop(my_loop, first_loop)) {
      // Visit the loop exit first in post order.
      std::swap(first_successor, second_successor);
    } else if (IsInnerLoop(my_loop, first_loop) && !IsInnerLoop(my_loop, second_loop)) {
      // Visit the inner loop last in post order.
      std::swap(first_successor, second_successor);
    }
    VisitBlockForLinearization(first_successor, order, visited);
    VisitBlockForLinearization(second_successor, order, visited);
  }
  order->Add(block);
}

class HLinearOrderIterator : public ValueObject {
 public:
  explicit HLinearOrderIterator(const GrowableArray<HBasicBlock*>& post_order)
      : post_order_(post_order), index_(post_order.Size()) {}

  bool Done() const { return index_ == 0; }
  HBasicBlock* Current() const { return post_order_.Get(index_ -1); }
  void Advance() { --index_; DCHECK_GE(index_, 0U); }

 private:
  const GrowableArray<HBasicBlock*>& post_order_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HLinearOrderIterator);
};

void SsaLivenessAnalysis::LinearizeGraph() {
  // For simplicity of the implementation, we create post linear order. The order for
  // computing live ranges is the reverse of that order.
  ArenaBitVector visited(graph_.GetArena(), graph_.GetBlocks().Size(), false);
  VisitBlockForLinearization(graph_.GetEntryBlock(), &linear_post_order_, &visited);
}

void SsaLivenessAnalysis::NumberInstructions() {
  int ssa_index = 0;
  for (HLinearOrderIterator it(linear_post_order_); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();

    for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
      HInstruction* current = it.Current();
      if (current->HasUses()) {
        current->SetSsaIndex(ssa_index++);
      }
    }

    for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
      HInstruction* current = it.Current();
      if (current->HasUses()) {
        current->SetSsaIndex(ssa_index++);
      }
    }
  }
  number_of_ssa_values_ = ssa_index;
}

void SsaLivenessAnalysis::ComputeSets() {
  for (HLinearOrderIterator it(linear_post_order_); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    block_infos_.Put(
        block->GetBlockId(),
        new (graph_.GetArena()) BlockInfo(graph_.GetArena(), *block, number_of_ssa_values_));
  }

  // Compute the initial live_in, live_out, and kill sets. This method does not handle
  // backward branches, therefore live_in and live_out sets are not yet correct.
  ComputeInitialSets();

  // Do a fixed point calculation to take into account backward branches,
  // that will update live_in of loop headers, and therefore live_out and live_in
  // of blocks in the loop.
  ComputeLiveInAndLiveOutSets();
}

void SsaLivenessAnalysis::ComputeInitialSets() {
  // Do a post orderr visit, adding inputs of instructions live in the block where
  // that instruction is defined, and killing instructions that are being visited.
  for (HPostOrderIterator it(graph_); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();

    BitVector* kill = GetKillSet(*block);
    BitVector* live_in = GetLiveInSet(*block);

    for (HBackwardInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
      HInstruction* current = it.Current();
      if (current->HasSsaIndex()) {
        kill->SetBit(current->GetSsaIndex());
        live_in->ClearBit(current->GetSsaIndex());
      }

      // All inputs of an instruction must be live.
      for (size_t i = 0, e = current->InputCount(); i < e; ++i) {
        DCHECK(current->InputAt(i)->HasSsaIndex());
        live_in->SetBit(current->InputAt(i)->GetSsaIndex());
      }

      if (current->HasEnvironment()) {
        // All instructions in the environment must be live.
        GrowableArray<HInstruction*>* environment = current->GetEnvironment()->GetVRegs();
        for (size_t i = 0, e = environment->Size(); i < e; ++i) {
          HInstruction* instruction = environment->Get(i);
          if (instruction != nullptr) {
            DCHECK(instruction->HasSsaIndex());
            live_in->SetBit(instruction->GetSsaIndex());
          }
        }
      }
    }

    for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
      HInstruction* current = it.Current();
      if (current->HasSsaIndex()) {
        kill->SetBit(current->GetSsaIndex());
        live_in->ClearBit(current->GetSsaIndex());
      }

      // Mark a phi input live_in for its corresponding predecessor.
      for (size_t i = 0, e = current->InputCount(); i < e; ++i) {
        HInstruction* input = current->InputAt(i);

        HBasicBlock* predecessor = block->GetPredecessors().Get(i);
        size_t ssa_index = input->GetSsaIndex();
        BitVector* predecessor_kill = GetKillSet(*predecessor);
        BitVector* predecessor_live_in = GetLiveInSet(*predecessor);

        // Phi inputs from a back edge have already been visited. If the back edge
        // block defines that input, we should not add it to its live_in.
        if (!predecessor_kill->IsBitSet(ssa_index)) {
          predecessor_live_in->SetBit(ssa_index);
        }
      }
    }
  }
}

void SsaLivenessAnalysis::ComputeLiveInAndLiveOutSets() {
  bool changed;
  do {
    changed = false;

    for (HPostOrderIterator it(graph_); !it.Done(); it.Advance()) {
      const HBasicBlock& block = *it.Current();

      // The live_in set depends on the kill set (which does not
      // change in this loop), and the live_out set.  If the live_out
      // set does not change, there is no need to update the live_in set.
      if (UpdateLiveOut(block) && UpdateLiveIn(block)) {
        changed = true;
      }
    }
  } while (changed);
}

bool SsaLivenessAnalysis::UpdateLiveOut(const HBasicBlock& block) {
  BitVector* live_out = GetLiveOutSet(block);
  bool changed = false;
  // The live_out set of a block is the union of live_in sets of its successors.
  for (size_t i = 0, e = block.GetSuccessors().Size(); i < e; ++i) {
    HBasicBlock* successor = block.GetSuccessors().Get(i);
    if (live_out->Union(GetLiveInSet(*successor))) {
      changed = true;
    }
  }
  return changed;
}


bool SsaLivenessAnalysis::UpdateLiveIn(const HBasicBlock& block) {
  BitVector* live_out = GetLiveOutSet(block);
  BitVector* kill = GetKillSet(block);
  BitVector* live_in = GetLiveInSet(block);
  // If live_out is updated (because of backward branches), we need to make
  // sure instructions in live_out are also in live_in, unless they are killed
  // by this block.
  return live_in->UnionIfNotIn(live_out, kill);
}

}  // namespace art
