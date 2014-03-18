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
  block->SetBlockId(blocks_.Size());
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
      for (size_t j = 0; j < block->GetSuccessors()->Size(); j++) {
        block->GetSuccessors()->Get(j)->RemovePredecessor(block);
      }
    }
  }
}

void HGraph::VisitBlockForBackEdges(HBasicBlock* block,
                                    ArenaBitVector* visited,
                                    ArenaBitVector* visiting) const {
  int id = block->GetBlockId();
  if (visited->IsBitSet(id)) return;

  visited->SetBit(id);
  visiting->SetBit(id);
  for (size_t i = 0; i < block->GetSuccessors()->Size(); i++) {
    HBasicBlock* successor = block->GetSuccessors()->Get(i);
    if (visiting->IsBitSet(successor->GetBlockId())) {
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
  for (size_t i = 0; i < entry_block_->GetSuccessors()->Size(); i++) {
    VisitBlockForDominatorTree(entry_block_->GetSuccessors()->Get(i), entry_block_, &visits);
  }
}

HBasicBlock* HGraph::FindCommonDominator(HBasicBlock* first, HBasicBlock* second) const {
  ArenaBitVector visited(arena_, blocks_.Size(), false);
  // Walk the dominator tree of the first block and mark the visited blocks.
  while (first != nullptr) {
    visited.SetBit(first->GetBlockId());
    first = first->GetDominator();
  }
  // Walk the dominator tree of the second block until a marked block is found.
  while (second != nullptr) {
    if (visited.IsBitSet(second->GetBlockId())) {
      return second;
    }
    second = second->GetDominator();
  }
  LOG(ERROR) << "Could not find common dominator";
  return nullptr;
}

void HGraph::VisitBlockForDominatorTree(HBasicBlock* block,
                                        HBasicBlock* predecessor,
                                        GrowableArray<size_t>* visits) {
  if (block->GetDominator() == nullptr) {
    block->SetDominator(predecessor);
  } else {
    block->SetDominator(FindCommonDominator(block->GetDominator(), predecessor));
  }

  visits->Increment(block->GetBlockId());
  // Once all the forward edges have been visited, we know the immediate
  // dominator of the block. We can then start visiting its successors.
  if (visits->Get(block->GetBlockId()) ==
      block->GetPredecessors()->Size() - block->NumberOfBackEdges()) {
    dominator_order_.Add(block);
    for (size_t i = 0; i < block->GetSuccessors()->Size(); i++) {
      VisitBlockForDominatorTree(block->GetSuccessors()->Get(i), block, visits);
    }
  }
}

void HBasicBlock::AddInstruction(HInstruction* instruction) {
  DCHECK(instruction->GetBlock() == nullptr);
  DCHECK_EQ(instruction->GetId(), -1);
  instruction->SetBlock(this);
  instruction->SetId(GetGraph()->GetNextInstructionId());
  if (first_instruction_ == nullptr) {
    DCHECK(last_instruction_ == nullptr);
    first_instruction_ = last_instruction_ = instruction;
  } else {
    last_instruction_->next_ = instruction;
    instruction->previous_ = last_instruction_;
    last_instruction_ = instruction;
  }
  for (int i = 0; i < instruction->InputCount(); i++) {
    instruction->InputAt(i)->AddUse(instruction);
  }
}

#define DEFINE_ACCEPT(name)                                                    \
void H##name::Accept(HGraphVisitor* visitor) {                                 \
  visitor->Visit##name(this);                                                  \
}

FOR_EACH_INSTRUCTION(DEFINE_ACCEPT)

#undef DEFINE_ACCEPT

void HGraphVisitor::VisitInsertionOrder() {
  const GrowableArray<HBasicBlock*>* blocks = graph_->GetBlocks();
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
