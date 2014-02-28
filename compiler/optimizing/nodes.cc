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

#include "nodes.h"
#include "utils/growable_array.h"

namespace art {

void HGraph::AddBlock(HBasicBlock* block) {
  block->set_block_id(blocks_.Size());
  blocks_.Add(block);
}

void HGraph::FindBackEdges(ArenaBitVector* visited) const {
  ArenaBitVector visiting(arena_, blocks_.Size(), false);
  VisitBlockForBackEdges(entry_block_, visited, &visiting);
}

void HGraph::RemoveDeadBlocks(const ArenaBitVector& visited) const {
  for (size_t i = 0; i < blocks_.Size(); i++) {
    if (!visited.IsBitSet(i)) {
      HBasicBlock* block = blocks_.Get(i);
      for (size_t j = 0; j < block->successors()->Size(); j++) {
        block->successors()->Get(j)->RemovePredecessor(block);
      }
    }
  }
}

void HGraph::VisitBlockForBackEdges(HBasicBlock* block,
                                    ArenaBitVector* visited,
                                    ArenaBitVector* visiting) const {
  int id = block->block_id();
  if (visited->IsBitSet(id)) return;

  visited->SetBit(id);
  visiting->SetBit(id);
  for (size_t i = 0; i < block->successors()->Size(); i++) {
    HBasicBlock* successor = block->successors()->Get(i);
    if (visiting->IsBitSet(successor->block_id())) {
      successor->AddBackEdge(block);
    } else {
      VisitBlockForBackEdges(successor, visited, visiting);
    }
  }
  visiting->ClearBit(id);
}

void HGraph::BuildDominatorTree() {
  ArenaBitVector visited(arena_, blocks_.Size(), false);

  // (1) Find the back edges in the graph doing a DFS traversal.
  FindBackEdges(&visited);

  // (2) Remove blocks not visited during the initial DFS.
  //     Step (3) requires dead blocks to be removed from the
  //     predecessors list of live blocks.
  RemoveDeadBlocks(visited);

  // (3) Compute the immediate dominator of each block. We visit
  //     the successors of a block only when all its forward branches
  //     have been processed.
  GrowableArray<size_t> visits(arena_, blocks_.Size());
  visits.SetSize(blocks_.Size());
  dominator_order_.Add(entry_block_);
  for (size_t i = 0; i < entry_block_->successors()->Size(); i++) {
    VisitBlockForDominatorTree(entry_block_->successors()->Get(i), entry_block_, &visits);
  }
}

HBasicBlock* HGraph::FindCommonDominator(HBasicBlock* first, HBasicBlock* second) const {
  ArenaBitVector visited(arena_, blocks_.Size(), false);
  // Walk the dominator tree of the first block and mark the visited blocks.
  while (first != nullptr) {
    visited.SetBit(first->block_id());
    first = first->dominator();
  }
  // Walk the dominator tree of the second block until a marked block is found.
  while (second != nullptr) {
    if (visited.IsBitSet(second->block_id())) {
      return second;
    }
    second = second->dominator();
  }
  LOG(ERROR) << "Could not find common dominator";
  return nullptr;
}

void HGraph::VisitBlockForDominatorTree(HBasicBlock* block,
                                        HBasicBlock* predecessor,
                                        GrowableArray<size_t>* visits) {
  if (block->dominator() == nullptr) {
    block->set_dominator(predecessor);
  } else {
    block->set_dominator(FindCommonDominator(block->dominator(), predecessor));
  }

  visits->Increment(block->block_id());
  // Once all the forward edges have been visited, we know the immediate
  // dominator of the block. We can then start visiting its successors.
  if (visits->Get(block->block_id()) ==
      block->predecessors()->Size() - block->NumberOfBackEdges()) {
    dominator_order_.Add(block);
    for (size_t i = 0; i < block->successors()->Size(); i++) {
      VisitBlockForDominatorTree(block->successors()->Get(i), block, visits);
    }
  }
}

void HBasicBlock::AddInstruction(HInstruction* instruction) {
  instruction->set_block(this);
  if (first_instruction_ == nullptr) {
    DCHECK(last_instruction_ == nullptr);
    first_instruction_ = last_instruction_ = instruction;
  } else {
    last_instruction_->next_ = instruction;
    instruction->previous_ = last_instruction_;
    last_instruction_ = instruction;
  }
}

#define DEFINE_ACCEPT(name)                                                    \
void H##name::Accept(HGraphVisitor* visitor) {                                 \
  visitor->Visit##name(this);                                                  \
}

FOR_EACH_INSTRUCTION(DEFINE_ACCEPT)

#undef DEFINE_ACCEPT

void HGraphVisitor::VisitInsertionOrder() {
  const GrowableArray<HBasicBlock*>* blocks = graph_->blocks();
  for (size_t i = 0 ; i < blocks->Size(); i++) {
    VisitBasicBlock(blocks->Get(i));
  }
}

void HGraphVisitor::VisitBasicBlock(HBasicBlock* block) {
  for (HInstructionIterator it(block); !it.Done(); it.Advance()) {
    it.Current()->Accept(this);
  }
}

}  // namespace art
