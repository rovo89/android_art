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
#include "ssa_builder.h"
#include "utils/growable_array.h"

namespace art {

void HGraph::AddBlock(HBasicBlock* block) {
  block->SetBlockId(blocks_.Size());
  blocks_.Add(block);
}

void HGraph::FindBackEdges(ArenaBitVector* visited) {
  ArenaBitVector visiting(arena_, blocks_.Size(), false);
  VisitBlockForBackEdges(entry_block_, visited, &visiting);
}

void HGraph::RemoveDeadBlocks(const ArenaBitVector& visited) const {
  for (size_t i = 0; i < blocks_.Size(); ++i) {
    if (!visited.IsBitSet(i)) {
      HBasicBlock* block = blocks_.Get(i);
      for (size_t j = 0; j < block->GetSuccessors().Size(); ++j) {
        block->GetSuccessors().Get(j)->RemovePredecessor(block, false);
      }
      for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
        block->RemovePhi(it.Current()->AsPhi());
      }
      for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
        block->RemoveInstruction(it.Current());
      }
    }
  }
}

void HGraph::VisitBlockForBackEdges(HBasicBlock* block,
                                    ArenaBitVector* visited,
                                    ArenaBitVector* visiting) {
  int id = block->GetBlockId();
  if (visited->IsBitSet(id)) return;

  visited->SetBit(id);
  visiting->SetBit(id);
  for (size_t i = 0; i < block->GetSuccessors().Size(); i++) {
    HBasicBlock* successor = block->GetSuccessors().Get(i);
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

  // (3) Simplify the CFG now, so that we don't need to recompute
  //     dominators and the reverse post order.
  SimplifyCFG();

  // (4) Compute the immediate dominator of each block. We visit
  //     the successors of a block only when all its forward branches
  //     have been processed.
  GrowableArray<size_t> visits(arena_, blocks_.Size());
  visits.SetSize(blocks_.Size());
  reverse_post_order_.Add(entry_block_);
  for (size_t i = 0; i < entry_block_->GetSuccessors().Size(); i++) {
    VisitBlockForDominatorTree(entry_block_->GetSuccessors().Get(i), entry_block_, &visits);
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
      block->GetPredecessors().Size() - block->NumberOfBackEdges()) {
    reverse_post_order_.Add(block);
    for (size_t i = 0; i < block->GetSuccessors().Size(); i++) {
      VisitBlockForDominatorTree(block->GetSuccessors().Get(i), block, visits);
    }
  }
}

void HGraph::TransformToSSA() {
  DCHECK(!reverse_post_order_.IsEmpty());
  SsaBuilder ssa_builder(this);
  ssa_builder.BuildSsa();
}

void HGraph::SplitCriticalEdge(HBasicBlock* block, HBasicBlock* successor) {
  // Insert a new node between `block` and `successor` to split the
  // critical edge.
  HBasicBlock* new_block = new (arena_) HBasicBlock(this);
  AddBlock(new_block);
  new_block->AddInstruction(new (arena_) HGoto());
  block->RemoveSuccessor(successor);
  block->AddSuccessor(new_block);
  new_block->AddSuccessor(successor);
  if (successor->IsLoopHeader()) {
    // If we split at a back edge boundary, make the new block the back edge.
    HLoopInformation* info = successor->GetLoopInformation();
    if (info->IsBackEdge(block)) {
      info->RemoveBackEdge(block);
      info->AddBackEdge(new_block);
    }
  }
}

void HGraph::SimplifyLoop(HBasicBlock* header) {
  HLoopInformation* info = header->GetLoopInformation();

  // If there are more than one back edge, make them branch to the same block that
  // will become the only back edge. This simplifies finding natural loops in the
  // graph.
  if (info->NumberOfBackEdges() > 1) {
    HBasicBlock* new_back_edge = new (arena_) HBasicBlock(this);
    AddBlock(new_back_edge);
    new_back_edge->AddInstruction(new (arena_) HGoto());
    for (size_t pred = 0, e = info->GetBackEdges().Size(); pred < e; ++pred) {
      HBasicBlock* back_edge = info->GetBackEdges().Get(pred);
      header->RemovePredecessor(back_edge);
      back_edge->AddSuccessor(new_back_edge);
    }
    info->ClearBackEdges();
    info->AddBackEdge(new_back_edge);
    new_back_edge->AddSuccessor(header);
  }

  // Make sure the loop has only one pre header. This simplifies SSA building by having
  // to just look at the pre header to know which locals are initialized at entry of the
  // loop.
  size_t number_of_incomings = header->GetPredecessors().Size() - info->NumberOfBackEdges();
  if (number_of_incomings != 1) {
    HBasicBlock* pre_header = new (arena_) HBasicBlock(this);
    AddBlock(pre_header);
    pre_header->AddInstruction(new (arena_) HGoto());

    ArenaBitVector back_edges(arena_, GetBlocks().Size(), false);
    HBasicBlock* back_edge = info->GetBackEdges().Get(0);
    for (size_t pred = 0; pred < header->GetPredecessors().Size(); ++pred) {
      HBasicBlock* predecessor = header->GetPredecessors().Get(pred);
      if (predecessor != back_edge) {
        header->RemovePredecessor(predecessor);
        pred--;
        predecessor->AddSuccessor(pre_header);
      }
    }
    pre_header->AddSuccessor(header);
  }
}

void HGraph::SimplifyCFG() {
  // Simplify the CFG for future analysis, and code generation:
  // (1): Split critical edges.
  // (2): Simplify loops by having only one back edge, and one preheader.
  for (size_t i = 0; i < blocks_.Size(); ++i) {
    HBasicBlock* block = blocks_.Get(i);
    if (block->GetSuccessors().Size() > 1) {
      for (size_t j = 0; j < block->GetSuccessors().Size(); ++j) {
        HBasicBlock* successor = block->GetSuccessors().Get(j);
        if (successor->GetPredecessors().Size() > 1) {
          SplitCriticalEdge(block, successor);
          --j;
        }
      }
    }
    if (block->IsLoopHeader()) {
      SimplifyLoop(block);
    }
  }
}

bool HGraph::FindNaturalLoops() const {
  for (size_t i = 0; i < blocks_.Size(); ++i) {
    HBasicBlock* block = blocks_.Get(i);
    if (block->IsLoopHeader()) {
      HLoopInformation* info = block->GetLoopInformation();
      if (!info->Populate()) {
        // Abort if the loop is non natural. We currently bailout in such cases.
        return false;
      }
    }
  }
  return true;
}

void HLoopInformation::PopulateRecursive(HBasicBlock* block) {
  if (blocks_.IsBitSet(block->GetBlockId())) {
    return;
  }

  blocks_.SetBit(block->GetBlockId());
  block->SetInLoop(this);
  for (size_t i = 0, e = block->GetPredecessors().Size(); i < e; ++i) {
    PopulateRecursive(block->GetPredecessors().Get(i));
  }
}

bool HLoopInformation::Populate() {
  DCHECK_EQ(GetBackEdges().Size(), 1u);
  HBasicBlock* back_edge = GetBackEdges().Get(0);
  DCHECK(back_edge->GetDominator() != nullptr);
  if (!header_->Dominates(back_edge)) {
    // This loop is not natural. Do not bother going further.
    return false;
  }

  // Populate this loop: starting with the back edge, recursively add predecessors
  // that are not already part of that loop. Set the header as part of the loop
  // to end the recursion.
  // This is a recursive implementation of the algorithm described in
  // "Advanced Compiler Design & Implementation" (Muchnick) p192.
  blocks_.SetBit(header_->GetBlockId());
  PopulateRecursive(back_edge);
  return true;
}

HBasicBlock* HLoopInformation::GetPreHeader() const {
  DCHECK_EQ(header_->GetPredecessors().Size(), 2u);
  return header_->GetDominator();
}

bool HLoopInformation::Contains(const HBasicBlock& block) const {
  return blocks_.IsBitSet(block.GetBlockId());
}

bool HLoopInformation::IsIn(const HLoopInformation& other) const {
  return other.blocks_.IsBitSet(header_->GetBlockId());
}

bool HBasicBlock::Dominates(HBasicBlock* other) const {
  // Walk up the dominator tree from `other`, to find out if `this`
  // is an ancestor.
  HBasicBlock* current = other;
  while (current != nullptr) {
    if (current == this) {
      return true;
    }
    current = current->GetDominator();
  }
  return false;
}

static void Add(HInstructionList* instruction_list,
                HBasicBlock* block,
                HInstruction* instruction) {
  DCHECK(instruction->GetBlock() == nullptr);
  DCHECK_EQ(instruction->GetId(), -1);
  instruction->SetBlock(block);
  instruction->SetId(block->GetGraph()->GetNextInstructionId());
  instruction_list->AddInstruction(instruction);
}

void HBasicBlock::AddInstruction(HInstruction* instruction) {
  Add(&instructions_, this, instruction);
}

void HBasicBlock::AddPhi(HPhi* phi) {
  Add(&phis_, this, phi);
}

static void Remove(HInstructionList* instruction_list,
                   HBasicBlock* block,
                   HInstruction* instruction) {
  DCHECK_EQ(block, instruction->GetBlock());
  DCHECK(instruction->GetUses() == nullptr);
  DCHECK(instruction->GetEnvUses() == nullptr);
  instruction->SetBlock(nullptr);
  instruction_list->RemoveInstruction(instruction);

  for (size_t i = 0; i < instruction->InputCount(); i++) {
    instruction->InputAt(i)->RemoveUser(instruction, i);
  }
}

void HBasicBlock::RemoveInstruction(HInstruction* instruction) {
  Remove(&instructions_, this, instruction);
}

void HBasicBlock::RemovePhi(HPhi* phi) {
  Remove(&phis_, this, phi);
}

void HInstruction::RemoveUser(HInstruction* user, size_t input_index) {
  HUseListNode<HInstruction>* previous = nullptr;
  HUseListNode<HInstruction>* current = uses_;
  while (current != nullptr) {
    if (current->GetUser() == user && current->GetIndex() == input_index) {
      if (previous == NULL) {
        uses_ = current->GetTail();
      } else {
        previous->SetTail(current->GetTail());
      }
    }
    previous = current;
    current = current->GetTail();
  }
}

void HInstructionList::AddInstruction(HInstruction* instruction) {
  if (first_instruction_ == nullptr) {
    DCHECK(last_instruction_ == nullptr);
    first_instruction_ = last_instruction_ = instruction;
  } else {
    last_instruction_->next_ = instruction;
    instruction->previous_ = last_instruction_;
    last_instruction_ = instruction;
  }
  for (size_t i = 0; i < instruction->InputCount(); i++) {
    instruction->InputAt(i)->AddUseAt(instruction, i);
  }
}

void HInstructionList::RemoveInstruction(HInstruction* instruction) {
  if (instruction->previous_ != nullptr) {
    instruction->previous_->next_ = instruction->next_;
  }
  if (instruction->next_ != nullptr) {
    instruction->next_->previous_ = instruction->previous_;
  }
  if (instruction == first_instruction_) {
    first_instruction_ = instruction->next_;
  }
  if (instruction == last_instruction_) {
    last_instruction_ = instruction->previous_;
  }
}

void HInstruction::ReplaceWith(HInstruction* other) {
  for (HUseIterator<HInstruction> it(GetUses()); !it.Done(); it.Advance()) {
    HUseListNode<HInstruction>* current = it.Current();
    HInstruction* user = current->GetUser();
    size_t input_index = current->GetIndex();
    user->SetRawInputAt(input_index, other);
    other->AddUseAt(user, input_index);
  }

  for (HUseIterator<HEnvironment> it(GetEnvUses()); !it.Done(); it.Advance()) {
    HUseListNode<HEnvironment>* current = it.Current();
    HEnvironment* user = current->GetUser();
    size_t input_index = current->GetIndex();
    user->SetRawEnvAt(input_index, other);
    other->AddEnvUseAt(user, input_index);
  }

  uses_ = nullptr;
  env_uses_ = nullptr;
}

void HPhi::AddInput(HInstruction* input) {
  DCHECK(input->GetBlock() != nullptr);
  inputs_.Add(input);
  input->AddUseAt(this, inputs_.Size() - 1);
}

#define DEFINE_ACCEPT(name)                                                    \
void H##name::Accept(HGraphVisitor* visitor) {                                 \
  visitor->Visit##name(this);                                                  \
}

FOR_EACH_INSTRUCTION(DEFINE_ACCEPT)

#undef DEFINE_ACCEPT

void HGraphVisitor::VisitInsertionOrder() {
  const GrowableArray<HBasicBlock*>& blocks = graph_->GetBlocks();
  for (size_t i = 0 ; i < blocks.Size(); i++) {
    VisitBasicBlock(blocks.Get(i));
  }
}

void HGraphVisitor::VisitBasicBlock(HBasicBlock* block) {
  for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    it.Current()->Accept(this);
  }
  for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
    it.Current()->Accept(this);
  }
}

}  // namespace art
