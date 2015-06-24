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
#include "base/bit_vector-inl.h"
#include "base/bit_utils.h"
#include "utils/growable_array.h"
#include "scoped_thread_state_change.h"

namespace art {

void HGraph::AddBlock(HBasicBlock* block) {
  block->SetBlockId(blocks_.Size());
  blocks_.Add(block);
}

void HGraph::FindBackEdges(ArenaBitVector* visited) {
  ArenaBitVector visiting(arena_, blocks_.Size(), false);
  VisitBlockForBackEdges(entry_block_, visited, &visiting);
}

static void RemoveAsUser(HInstruction* instruction) {
  for (size_t i = 0; i < instruction->InputCount(); i++) {
    instruction->RemoveAsUserOfInput(i);
  }

  for (HEnvironment* environment = instruction->GetEnvironment();
       environment != nullptr;
       environment = environment->GetParent()) {
    for (size_t i = 0, e = environment->Size(); i < e; ++i) {
      if (environment->GetInstructionAt(i) != nullptr) {
        environment->RemoveAsUserOfInput(i);
      }
    }
  }
}

void HGraph::RemoveInstructionsAsUsersFromDeadBlocks(const ArenaBitVector& visited) const {
  for (size_t i = 0; i < blocks_.Size(); ++i) {
    if (!visited.IsBitSet(i)) {
      HBasicBlock* block = blocks_.Get(i);
      DCHECK(block->GetPhis().IsEmpty()) << "Phis are not inserted at this stage";
      for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
        RemoveAsUser(it.Current());
      }
    }
  }
}

void HGraph::RemoveDeadBlocks(const ArenaBitVector& visited) {
  for (size_t i = 0; i < blocks_.Size(); ++i) {
    if (!visited.IsBitSet(i)) {
      HBasicBlock* block = blocks_.Get(i);
      // We only need to update the successor, which might be live.
      for (size_t j = 0; j < block->GetSuccessors().Size(); ++j) {
        block->GetSuccessors().Get(j)->RemovePredecessor(block);
      }
      // Remove the block from the list of blocks, so that further analyses
      // never see it.
      blocks_.Put(i, nullptr);
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

  // (2) Remove instructions and phis from blocks not visited during
  //     the initial DFS as users from other instructions, so that
  //     users can be safely removed before uses later.
  RemoveInstructionsAsUsersFromDeadBlocks(visited);

  // (3) Remove blocks not visited during the initial DFS.
  //     Step (4) requires dead blocks to be removed from the
  //     predecessors list of live blocks.
  RemoveDeadBlocks(visited);

  // (4) Simplify the CFG now, so that we don't need to recompute
  //     dominators and the reverse post order.
  SimplifyCFG();

  // (5) Compute the dominance information and the reverse post order.
  ComputeDominanceInformation();
}

void HGraph::ClearDominanceInformation() {
  for (HReversePostOrderIterator it(*this); !it.Done(); it.Advance()) {
    it.Current()->ClearDominanceInformation();
  }
  reverse_post_order_.Reset();
}

void HBasicBlock::ClearDominanceInformation() {
  dominated_blocks_.Reset();
  dominator_ = nullptr;
}

void HGraph::ComputeDominanceInformation() {
  DCHECK(reverse_post_order_.IsEmpty());
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
    block->GetDominator()->AddDominatedBlock(block);
    reverse_post_order_.Add(block);
    for (size_t i = 0; i < block->GetSuccessors().Size(); i++) {
      VisitBlockForDominatorTree(block->GetSuccessors().Get(i), block, visits);
    }
  }
}

void HGraph::TransformToSsa() {
  DCHECK(!reverse_post_order_.IsEmpty());
  SsaBuilder ssa_builder(this);
  ssa_builder.BuildSsa();
}

void HGraph::SplitCriticalEdge(HBasicBlock* block, HBasicBlock* successor) {
  // Insert a new node between `block` and `successor` to split the
  // critical edge.
  HBasicBlock* new_block = new (arena_) HBasicBlock(this, successor->GetDexPc());
  AddBlock(new_block);
  new_block->AddInstruction(new (arena_) HGoto());
  // Use `InsertBetween` to ensure the predecessor index and successor index of
  // `block` and `successor` are preserved.
  new_block->InsertBetween(block, successor);
  if (successor->IsLoopHeader()) {
    // If we split at a back edge boundary, make the new block the back edge.
    HLoopInformation* info = successor->GetLoopInformation();
    if (info->IsBackEdge(*block)) {
      info->RemoveBackEdge(block);
      info->AddBackEdge(new_block);
    }
  }
}

void HGraph::SimplifyLoop(HBasicBlock* header) {
  HLoopInformation* info = header->GetLoopInformation();

  // Make sure the loop has only one pre header. This simplifies SSA building by having
  // to just look at the pre header to know which locals are initialized at entry of the
  // loop.
  size_t number_of_incomings = header->GetPredecessors().Size() - info->NumberOfBackEdges();
  if (number_of_incomings != 1) {
    HBasicBlock* pre_header = new (arena_) HBasicBlock(this, header->GetDexPc());
    AddBlock(pre_header);
    pre_header->AddInstruction(new (arena_) HGoto());

    for (size_t pred = 0; pred < header->GetPredecessors().Size(); ++pred) {
      HBasicBlock* predecessor = header->GetPredecessors().Get(pred);
      if (!info->IsBackEdge(*predecessor)) {
        predecessor->ReplaceSuccessor(header, pre_header);
        pred--;
      }
    }
    pre_header->AddSuccessor(header);
  }

  // Make sure the first predecessor of a loop header is the incoming block.
  if (info->IsBackEdge(*header->GetPredecessors().Get(0))) {
    HBasicBlock* to_swap = header->GetPredecessors().Get(0);
    for (size_t pred = 1, e = header->GetPredecessors().Size(); pred < e; ++pred) {
      HBasicBlock* predecessor = header->GetPredecessors().Get(pred);
      if (!info->IsBackEdge(*predecessor)) {
        header->predecessors_.Put(pred, to_swap);
        header->predecessors_.Put(0, predecessor);
        break;
      }
    }
  }

  // Place the suspend check at the beginning of the header, so that live registers
  // will be known when allocating registers. Note that code generation can still
  // generate the suspend check at the back edge, but needs to be careful with
  // loop phi spill slots (which are not written to at back edge).
  HInstruction* first_instruction = header->GetFirstInstruction();
  if (!first_instruction->IsSuspendCheck()) {
    HSuspendCheck* check = new (arena_) HSuspendCheck(header->GetDexPc());
    header->InsertInstructionBefore(check, first_instruction);
    first_instruction = check;
  }
  info->SetSuspendCheck(first_instruction->AsSuspendCheck());
}

void HGraph::SimplifyCFG() {
  // Simplify the CFG for future analysis, and code generation:
  // (1): Split critical edges.
  // (2): Simplify loops by having only one back edge, and one preheader.
  for (size_t i = 0; i < blocks_.Size(); ++i) {
    HBasicBlock* block = blocks_.Get(i);
    if (block == nullptr) continue;
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

bool HGraph::AnalyzeNaturalLoops() const {
  // Order does not matter.
  for (HReversePostOrderIterator it(*this); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
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

void HGraph::InsertConstant(HConstant* constant) {
  // New constants are inserted before the final control-flow instruction
  // of the graph, or at its end if called from the graph builder.
  if (entry_block_->EndsWithControlFlowInstruction()) {
    entry_block_->InsertInstructionBefore(constant, entry_block_->GetLastInstruction());
  } else {
    entry_block_->AddInstruction(constant);
  }
}

HNullConstant* HGraph::GetNullConstant() {
  // For simplicity, don't bother reviving the cached null constant if it is
  // not null and not in a block. Otherwise, we need to clear the instruction
  // id and/or any invariants the graph is assuming when adding new instructions.
  if ((cached_null_constant_ == nullptr) || (cached_null_constant_->GetBlock() == nullptr)) {
    cached_null_constant_ = new (arena_) HNullConstant();
    InsertConstant(cached_null_constant_);
  }
  return cached_null_constant_;
}

HConstant* HGraph::GetConstant(Primitive::Type type, int64_t value) {
  switch (type) {
    case Primitive::Type::kPrimBoolean:
      DCHECK(IsUint<1>(value));
      FALLTHROUGH_INTENDED;
    case Primitive::Type::kPrimByte:
    case Primitive::Type::kPrimChar:
    case Primitive::Type::kPrimShort:
    case Primitive::Type::kPrimInt:
      DCHECK(IsInt(Primitive::ComponentSize(type) * kBitsPerByte, value));
      return GetIntConstant(static_cast<int32_t>(value));

    case Primitive::Type::kPrimLong:
      return GetLongConstant(value);

    default:
      LOG(FATAL) << "Unsupported constant type";
      UNREACHABLE();
  }
}

void HGraph::CacheFloatConstant(HFloatConstant* constant) {
  int32_t value = bit_cast<int32_t, float>(constant->GetValue());
  DCHECK(cached_float_constants_.find(value) == cached_float_constants_.end());
  cached_float_constants_.Overwrite(value, constant);
}

void HGraph::CacheDoubleConstant(HDoubleConstant* constant) {
  int64_t value = bit_cast<int64_t, double>(constant->GetValue());
  DCHECK(cached_double_constants_.find(value) == cached_double_constants_.end());
  cached_double_constants_.Overwrite(value, constant);
}

void HLoopInformation::Add(HBasicBlock* block) {
  blocks_.SetBit(block->GetBlockId());
}

void HLoopInformation::Remove(HBasicBlock* block) {
  blocks_.ClearBit(block->GetBlockId());
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
  DCHECK_EQ(blocks_.NumSetBits(), 0u) << "Loop information has already been populated";
  for (size_t i = 0, e = GetBackEdges().Size(); i < e; ++i) {
    HBasicBlock* back_edge = GetBackEdges().Get(i);
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
  }
  return true;
}

void HLoopInformation::Update() {
  HGraph* graph = header_->GetGraph();
  for (uint32_t id : blocks_.Indexes()) {
    HBasicBlock* block = graph->GetBlocks().Get(id);
    // Reset loop information of non-header blocks inside the loop, except
    // members of inner nested loops because those should already have been
    // updated by their own LoopInformation.
    if (block->GetLoopInformation() == this && block != header_) {
      block->SetLoopInformation(nullptr);
    }
  }
  blocks_.ClearAllBits();

  if (back_edges_.IsEmpty()) {
    // The loop has been dismantled, delete its suspend check and remove info
    // from the header.
    DCHECK(HasSuspendCheck());
    header_->RemoveInstruction(suspend_check_);
    header_->SetLoopInformation(nullptr);
    header_ = nullptr;
    suspend_check_ = nullptr;
  } else {
    if (kIsDebugBuild) {
      for (size_t i = 0, e = back_edges_.Size(); i < e; ++i) {
        DCHECK(header_->Dominates(back_edges_.Get(i)));
      }
    }
    // This loop still has reachable back edges. Repopulate the list of blocks.
    bool populate_successful = Populate();
    DCHECK(populate_successful);
  }
}

HBasicBlock* HLoopInformation::GetPreHeader() const {
  return header_->GetDominator();
}

bool HLoopInformation::Contains(const HBasicBlock& block) const {
  return blocks_.IsBitSet(block.GetBlockId());
}

bool HLoopInformation::IsIn(const HLoopInformation& other) const {
  return other.blocks_.IsBitSet(header_->GetBlockId());
}

size_t HLoopInformation::GetLifetimeEnd() const {
  size_t last_position = 0;
  for (size_t i = 0, e = back_edges_.Size(); i < e; ++i) {
    last_position = std::max(back_edges_.Get(i)->GetLifetimeEnd(), last_position);
  }
  return last_position;
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

static void UpdateInputsUsers(HInstruction* instruction) {
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    instruction->InputAt(i)->AddUseAt(instruction, i);
  }
  // Environment should be created later.
  DCHECK(!instruction->HasEnvironment());
}

void HBasicBlock::ReplaceAndRemoveInstructionWith(HInstruction* initial,
                                                  HInstruction* replacement) {
  DCHECK(initial->GetBlock() == this);
  InsertInstructionBefore(replacement, initial);
  initial->ReplaceWith(replacement);
  RemoveInstruction(initial);
}

static void Add(HInstructionList* instruction_list,
                HBasicBlock* block,
                HInstruction* instruction) {
  DCHECK(instruction->GetBlock() == nullptr);
  DCHECK_EQ(instruction->GetId(), -1);
  instruction->SetBlock(block);
  instruction->SetId(block->GetGraph()->GetNextInstructionId());
  UpdateInputsUsers(instruction);
  instruction_list->AddInstruction(instruction);
}

void HBasicBlock::AddInstruction(HInstruction* instruction) {
  Add(&instructions_, this, instruction);
}

void HBasicBlock::AddPhi(HPhi* phi) {
  Add(&phis_, this, phi);
}

void HBasicBlock::InsertInstructionBefore(HInstruction* instruction, HInstruction* cursor) {
  DCHECK(!cursor->IsPhi());
  DCHECK(!instruction->IsPhi());
  DCHECK_EQ(instruction->GetId(), -1);
  DCHECK_NE(cursor->GetId(), -1);
  DCHECK_EQ(cursor->GetBlock(), this);
  DCHECK(!instruction->IsControlFlow());
  instruction->SetBlock(this);
  instruction->SetId(GetGraph()->GetNextInstructionId());
  UpdateInputsUsers(instruction);
  instructions_.InsertInstructionBefore(instruction, cursor);
}

void HBasicBlock::InsertInstructionAfter(HInstruction* instruction, HInstruction* cursor) {
  DCHECK(!cursor->IsPhi());
  DCHECK(!instruction->IsPhi());
  DCHECK_EQ(instruction->GetId(), -1);
  DCHECK_NE(cursor->GetId(), -1);
  DCHECK_EQ(cursor->GetBlock(), this);
  DCHECK(!instruction->IsControlFlow());
  DCHECK(!cursor->IsControlFlow());
  instruction->SetBlock(this);
  instruction->SetId(GetGraph()->GetNextInstructionId());
  UpdateInputsUsers(instruction);
  instructions_.InsertInstructionAfter(instruction, cursor);
}

void HBasicBlock::InsertPhiAfter(HPhi* phi, HPhi* cursor) {
  DCHECK_EQ(phi->GetId(), -1);
  DCHECK_NE(cursor->GetId(), -1);
  DCHECK_EQ(cursor->GetBlock(), this);
  phi->SetBlock(this);
  phi->SetId(GetGraph()->GetNextInstructionId());
  UpdateInputsUsers(phi);
  phis_.InsertInstructionAfter(phi, cursor);
}

static void Remove(HInstructionList* instruction_list,
                   HBasicBlock* block,
                   HInstruction* instruction,
                   bool ensure_safety) {
  DCHECK_EQ(block, instruction->GetBlock());
  instruction->SetBlock(nullptr);
  instruction_list->RemoveInstruction(instruction);
  if (ensure_safety) {
    DCHECK(instruction->GetUses().IsEmpty());
    DCHECK(instruction->GetEnvUses().IsEmpty());
    RemoveAsUser(instruction);
  }
}

void HBasicBlock::RemoveInstruction(HInstruction* instruction, bool ensure_safety) {
  DCHECK(!instruction->IsPhi());
  Remove(&instructions_, this, instruction, ensure_safety);
}

void HBasicBlock::RemovePhi(HPhi* phi, bool ensure_safety) {
  Remove(&phis_, this, phi, ensure_safety);
}

void HBasicBlock::RemoveInstructionOrPhi(HInstruction* instruction, bool ensure_safety) {
  if (instruction->IsPhi()) {
    RemovePhi(instruction->AsPhi(), ensure_safety);
  } else {
    RemoveInstruction(instruction, ensure_safety);
  }
}

void HEnvironment::CopyFrom(const GrowableArray<HInstruction*>& locals) {
  for (size_t i = 0; i < locals.Size(); i++) {
    HInstruction* instruction = locals.Get(i);
    SetRawEnvAt(i, instruction);
    if (instruction != nullptr) {
      instruction->AddEnvUseAt(this, i);
    }
  }
}

void HEnvironment::CopyFrom(HEnvironment* env) {
  for (size_t i = 0; i < env->Size(); i++) {
    HInstruction* instruction = env->GetInstructionAt(i);
    SetRawEnvAt(i, instruction);
    if (instruction != nullptr) {
      instruction->AddEnvUseAt(this, i);
    }
  }
}

void HEnvironment::CopyFromWithLoopPhiAdjustment(HEnvironment* env,
                                                 HBasicBlock* loop_header) {
  DCHECK(loop_header->IsLoopHeader());
  for (size_t i = 0; i < env->Size(); i++) {
    HInstruction* instruction = env->GetInstructionAt(i);
    SetRawEnvAt(i, instruction);
    if (instruction == nullptr) {
      continue;
    }
    if (instruction->IsLoopHeaderPhi() && (instruction->GetBlock() == loop_header)) {
      // At the end of the loop pre-header, the corresponding value for instruction
      // is the first input of the phi.
      HInstruction* initial = instruction->AsPhi()->InputAt(0);
      DCHECK(initial->GetBlock()->Dominates(loop_header));
      SetRawEnvAt(i, initial);
      initial->AddEnvUseAt(this, i);
    } else {
      instruction->AddEnvUseAt(this, i);
    }
  }
}

void HEnvironment::RemoveAsUserOfInput(size_t index) const {
  const HUserRecord<HEnvironment*> user_record = vregs_.Get(index);
  user_record.GetInstruction()->RemoveEnvironmentUser(user_record.GetUseNode());
}

HInstruction* HInstruction::GetNextDisregardingMoves() const {
  HInstruction* next = GetNext();
  while (next != nullptr && next->IsParallelMove()) {
    next = next->GetNext();
  }
  return next;
}

HInstruction* HInstruction::GetPreviousDisregardingMoves() const {
  HInstruction* previous = GetPrevious();
  while (previous != nullptr && previous->IsParallelMove()) {
    previous = previous->GetPrevious();
  }
  return previous;
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
}

void HInstructionList::InsertInstructionBefore(HInstruction* instruction, HInstruction* cursor) {
  DCHECK(Contains(cursor));
  if (cursor == first_instruction_) {
    cursor->previous_ = instruction;
    instruction->next_ = cursor;
    first_instruction_ = instruction;
  } else {
    instruction->previous_ = cursor->previous_;
    instruction->next_ = cursor;
    cursor->previous_ = instruction;
    instruction->previous_->next_ = instruction;
  }
}

void HInstructionList::InsertInstructionAfter(HInstruction* instruction, HInstruction* cursor) {
  DCHECK(Contains(cursor));
  if (cursor == last_instruction_) {
    cursor->next_ = instruction;
    instruction->previous_ = cursor;
    last_instruction_ = instruction;
  } else {
    instruction->next_ = cursor->next_;
    instruction->previous_ = cursor;
    cursor->next_ = instruction;
    instruction->next_->previous_ = instruction;
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

bool HInstructionList::Contains(HInstruction* instruction) const {
  for (HInstructionIterator it(*this); !it.Done(); it.Advance()) {
    if (it.Current() == instruction) {
      return true;
    }
  }
  return false;
}

bool HInstructionList::FoundBefore(const HInstruction* instruction1,
                                   const HInstruction* instruction2) const {
  DCHECK_EQ(instruction1->GetBlock(), instruction2->GetBlock());
  for (HInstructionIterator it(*this); !it.Done(); it.Advance()) {
    if (it.Current() == instruction1) {
      return true;
    }
    if (it.Current() == instruction2) {
      return false;
    }
  }
  LOG(FATAL) << "Did not find an order between two instructions of the same block.";
  return true;
}

bool HInstruction::StrictlyDominates(HInstruction* other_instruction) const {
  if (other_instruction == this) {
    // An instruction does not strictly dominate itself.
    return false;
  }
  HBasicBlock* block = GetBlock();
  HBasicBlock* other_block = other_instruction->GetBlock();
  if (block != other_block) {
    return GetBlock()->Dominates(other_instruction->GetBlock());
  } else {
    // If both instructions are in the same block, ensure this
    // instruction comes before `other_instruction`.
    if (IsPhi()) {
      if (!other_instruction->IsPhi()) {
        // Phis appear before non phi-instructions so this instruction
        // dominates `other_instruction`.
        return true;
      } else {
        // There is no order among phis.
        LOG(FATAL) << "There is no dominance between phis of a same block.";
        return false;
      }
    } else {
      // `this` is not a phi.
      if (other_instruction->IsPhi()) {
        // Phis appear before non phi-instructions so this instruction
        // does not dominate `other_instruction`.
        return false;
      } else {
        // Check whether this instruction comes before
        // `other_instruction` in the instruction list.
        return block->GetInstructions().FoundBefore(this, other_instruction);
      }
    }
  }
}

void HInstruction::ReplaceWith(HInstruction* other) {
  DCHECK(other != nullptr);
  for (HUseIterator<HInstruction*> it(GetUses()); !it.Done(); it.Advance()) {
    HUseListNode<HInstruction*>* current = it.Current();
    HInstruction* user = current->GetUser();
    size_t input_index = current->GetIndex();
    user->SetRawInputAt(input_index, other);
    other->AddUseAt(user, input_index);
  }

  for (HUseIterator<HEnvironment*> it(GetEnvUses()); !it.Done(); it.Advance()) {
    HUseListNode<HEnvironment*>* current = it.Current();
    HEnvironment* user = current->GetUser();
    size_t input_index = current->GetIndex();
    user->SetRawEnvAt(input_index, other);
    other->AddEnvUseAt(user, input_index);
  }

  uses_.Clear();
  env_uses_.Clear();
}

void HInstruction::ReplaceInput(HInstruction* replacement, size_t index) {
  RemoveAsUserOfInput(index);
  SetRawInputAt(index, replacement);
  replacement->AddUseAt(this, index);
}

size_t HInstruction::EnvironmentSize() const {
  return HasEnvironment() ? environment_->Size() : 0;
}

void HPhi::AddInput(HInstruction* input) {
  DCHECK(input->GetBlock() != nullptr);
  inputs_.Add(HUserRecord<HInstruction*>(input));
  input->AddUseAt(this, inputs_.Size() - 1);
}

void HPhi::RemoveInputAt(size_t index) {
  RemoveAsUserOfInput(index);
  inputs_.DeleteAt(index);
  for (size_t i = index, e = InputCount(); i < e; ++i) {
    InputRecordAt(i).GetUseNode()->SetIndex(i);
  }
}

#define DEFINE_ACCEPT(name, super)                                             \
void H##name::Accept(HGraphVisitor* visitor) {                                 \
  visitor->Visit##name(this);                                                  \
}

FOR_EACH_INSTRUCTION(DEFINE_ACCEPT)

#undef DEFINE_ACCEPT

void HGraphVisitor::VisitInsertionOrder() {
  const GrowableArray<HBasicBlock*>& blocks = graph_->GetBlocks();
  for (size_t i = 0 ; i < blocks.Size(); i++) {
    HBasicBlock* block = blocks.Get(i);
    if (block != nullptr) {
      VisitBasicBlock(block);
    }
  }
}

void HGraphVisitor::VisitReversePostOrder() {
  for (HReversePostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    VisitBasicBlock(it.Current());
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

HConstant* HUnaryOperation::TryStaticEvaluation() const {
  if (GetInput()->IsIntConstant()) {
    int32_t value = Evaluate(GetInput()->AsIntConstant()->GetValue());
    return GetBlock()->GetGraph()->GetIntConstant(value);
  } else if (GetInput()->IsLongConstant()) {
    // TODO: Implement static evaluation of long unary operations.
    //
    // Do not exit with a fatal condition here.  Instead, simply
    // return `null' to notify the caller that this instruction
    // cannot (yet) be statically evaluated.
    return nullptr;
  }
  return nullptr;
}

HConstant* HBinaryOperation::TryStaticEvaluation() const {
  if (GetLeft()->IsIntConstant() && GetRight()->IsIntConstant()) {
    int32_t value = Evaluate(GetLeft()->AsIntConstant()->GetValue(),
                             GetRight()->AsIntConstant()->GetValue());
    return GetBlock()->GetGraph()->GetIntConstant(value);
  } else if (GetLeft()->IsLongConstant() && GetRight()->IsLongConstant()) {
    int64_t value = Evaluate(GetLeft()->AsLongConstant()->GetValue(),
                             GetRight()->AsLongConstant()->GetValue());
    if (GetResultType() == Primitive::kPrimLong) {
      return GetBlock()->GetGraph()->GetLongConstant(value);
    } else {
      DCHECK_EQ(GetResultType(), Primitive::kPrimInt);
      return GetBlock()->GetGraph()->GetIntConstant(static_cast<int32_t>(value));
    }
  }
  return nullptr;
}

HConstant* HBinaryOperation::GetConstantRight() const {
  if (GetRight()->IsConstant()) {
    return GetRight()->AsConstant();
  } else if (IsCommutative() && GetLeft()->IsConstant()) {
    return GetLeft()->AsConstant();
  } else {
    return nullptr;
  }
}

// If `GetConstantRight()` returns one of the input, this returns the other
// one. Otherwise it returns null.
HInstruction* HBinaryOperation::GetLeastConstantLeft() const {
  HInstruction* most_constant_right = GetConstantRight();
  if (most_constant_right == nullptr) {
    return nullptr;
  } else if (most_constant_right == GetLeft()) {
    return GetRight();
  } else {
    return GetLeft();
  }
}

bool HCondition::IsBeforeWhenDisregardMoves(HInstruction* instruction) const {
  return this == instruction->GetPreviousDisregardingMoves();
}

bool HInstruction::Equals(HInstruction* other) const {
  if (!InstructionTypeEquals(other)) return false;
  DCHECK_EQ(GetKind(), other->GetKind());
  if (!InstructionDataEquals(other)) return false;
  if (GetType() != other->GetType()) return false;
  if (InputCount() != other->InputCount()) return false;

  for (size_t i = 0, e = InputCount(); i < e; ++i) {
    if (InputAt(i) != other->InputAt(i)) return false;
  }
  DCHECK_EQ(ComputeHashCode(), other->ComputeHashCode());
  return true;
}

std::ostream& operator<<(std::ostream& os, const HInstruction::InstructionKind& rhs) {
#define DECLARE_CASE(type, super) case HInstruction::k##type: os << #type; break;
  switch (rhs) {
    FOR_EACH_INSTRUCTION(DECLARE_CASE)
    default:
      os << "Unknown instruction kind " << static_cast<int>(rhs);
      break;
  }
#undef DECLARE_CASE
  return os;
}

void HInstruction::MoveBefore(HInstruction* cursor) {
  next_->previous_ = previous_;
  if (previous_ != nullptr) {
    previous_->next_ = next_;
  }
  if (block_->instructions_.first_instruction_ == this) {
    block_->instructions_.first_instruction_ = next_;
  }
  DCHECK_NE(block_->instructions_.last_instruction_, this);

  previous_ = cursor->previous_;
  if (previous_ != nullptr) {
    previous_->next_ = this;
  }
  next_ = cursor;
  cursor->previous_ = this;
  block_ = cursor->block_;

  if (block_->instructions_.first_instruction_ == cursor) {
    block_->instructions_.first_instruction_ = this;
  }
}

HBasicBlock* HBasicBlock::SplitAfter(HInstruction* cursor) {
  DCHECK(!cursor->IsControlFlow());
  DCHECK_NE(instructions_.last_instruction_, cursor);
  DCHECK_EQ(cursor->GetBlock(), this);

  HBasicBlock* new_block = new (GetGraph()->GetArena()) HBasicBlock(GetGraph(), GetDexPc());
  new_block->instructions_.first_instruction_ = cursor->GetNext();
  new_block->instructions_.last_instruction_ = instructions_.last_instruction_;
  cursor->next_->previous_ = nullptr;
  cursor->next_ = nullptr;
  instructions_.last_instruction_ = cursor;

  new_block->instructions_.SetBlockOfInstructions(new_block);
  for (size_t i = 0, e = GetSuccessors().Size(); i < e; ++i) {
    HBasicBlock* successor = GetSuccessors().Get(i);
    new_block->successors_.Add(successor);
    successor->predecessors_.Put(successor->GetPredecessorIndexOf(this), new_block);
  }
  successors_.Reset();

  for (size_t i = 0, e = GetDominatedBlocks().Size(); i < e; ++i) {
    HBasicBlock* dominated = GetDominatedBlocks().Get(i);
    dominated->dominator_ = new_block;
    new_block->dominated_blocks_.Add(dominated);
  }
  dominated_blocks_.Reset();
  return new_block;
}

bool HBasicBlock::IsSingleGoto() const {
  HLoopInformation* loop_info = GetLoopInformation();
  // TODO: Remove the null check b/19084197.
  return GetFirstInstruction() != nullptr
         && GetPhis().IsEmpty()
         && GetFirstInstruction() == GetLastInstruction()
         && GetLastInstruction()->IsGoto()
         // Back edges generate the suspend check.
         && (loop_info == nullptr || !loop_info->IsBackEdge(*this));
}

bool HBasicBlock::EndsWithControlFlowInstruction() const {
  return !GetInstructions().IsEmpty() && GetLastInstruction()->IsControlFlow();
}

bool HBasicBlock::EndsWithIf() const {
  return !GetInstructions().IsEmpty() && GetLastInstruction()->IsIf();
}

bool HBasicBlock::HasSinglePhi() const {
  return !GetPhis().IsEmpty() && GetFirstPhi()->GetNext() == nullptr;
}

size_t HInstructionList::CountSize() const {
  size_t size = 0;
  HInstruction* current = first_instruction_;
  for (; current != nullptr; current = current->GetNext()) {
    size++;
  }
  return size;
}

void HInstructionList::SetBlockOfInstructions(HBasicBlock* block) const {
  for (HInstruction* current = first_instruction_;
       current != nullptr;
       current = current->GetNext()) {
    current->SetBlock(block);
  }
}

void HInstructionList::AddAfter(HInstruction* cursor, const HInstructionList& instruction_list) {
  DCHECK(Contains(cursor));
  if (!instruction_list.IsEmpty()) {
    if (cursor == last_instruction_) {
      last_instruction_ = instruction_list.last_instruction_;
    } else {
      cursor->next_->previous_ = instruction_list.last_instruction_;
    }
    instruction_list.last_instruction_->next_ = cursor->next_;
    cursor->next_ = instruction_list.first_instruction_;
    instruction_list.first_instruction_->previous_ = cursor;
  }
}

void HInstructionList::Add(const HInstructionList& instruction_list) {
  if (IsEmpty()) {
    first_instruction_ = instruction_list.first_instruction_;
    last_instruction_ = instruction_list.last_instruction_;
  } else {
    AddAfter(last_instruction_, instruction_list);
  }
}

void HBasicBlock::DisconnectAndDelete() {
  // Dominators must be removed after all the blocks they dominate. This way
  // a loop header is removed last, a requirement for correct loop information
  // iteration.
  DCHECK(dominated_blocks_.IsEmpty());

  // Remove the block from all loops it is included in.
  for (HLoopInformationOutwardIterator it(*this); !it.Done(); it.Advance()) {
    HLoopInformation* loop_info = it.Current();
    loop_info->Remove(this);
    if (loop_info->IsBackEdge(*this)) {
      // If this was the last back edge of the loop, we deliberately leave the
      // loop in an inconsistent state and will fail SSAChecker unless the
      // entire loop is removed during the pass.
      loop_info->RemoveBackEdge(this);
    }
  }

  // Disconnect the block from its predecessors and update their control-flow
  // instructions.
  for (size_t i = 0, e = predecessors_.Size(); i < e; ++i) {
    HBasicBlock* predecessor = predecessors_.Get(i);
    HInstruction* last_instruction = predecessor->GetLastInstruction();
    predecessor->RemoveInstruction(last_instruction);
    predecessor->RemoveSuccessor(this);
    if (predecessor->GetSuccessors().Size() == 1u) {
      DCHECK(last_instruction->IsIf());
      predecessor->AddInstruction(new (graph_->GetArena()) HGoto());
    } else {
      // The predecessor has no remaining successors and therefore must be dead.
      // We deliberately leave it without a control-flow instruction so that the
      // SSAChecker fails unless it is not removed during the pass too.
      DCHECK_EQ(predecessor->GetSuccessors().Size(), 0u);
    }
  }
  predecessors_.Reset();

  // Disconnect the block from its successors and update their phis.
  for (size_t i = 0, e = successors_.Size(); i < e; ++i) {
    HBasicBlock* successor = successors_.Get(i);
    // Delete this block from the list of predecessors.
    size_t this_index = successor->GetPredecessorIndexOf(this);
    successor->predecessors_.DeleteAt(this_index);

    // Check that `successor` has other predecessors, otherwise `this` is the
    // dominator of `successor` which violates the order DCHECKed at the top.
    DCHECK(!successor->predecessors_.IsEmpty());

    // Remove this block's entries in the successor's phis.
    if (successor->predecessors_.Size() == 1u) {
      // The successor has just one predecessor left. Replace phis with the only
      // remaining input.
      for (HInstructionIterator phi_it(successor->GetPhis()); !phi_it.Done(); phi_it.Advance()) {
        HPhi* phi = phi_it.Current()->AsPhi();
        phi->ReplaceWith(phi->InputAt(1 - this_index));
        successor->RemovePhi(phi);
      }
    } else {
      for (HInstructionIterator phi_it(successor->GetPhis()); !phi_it.Done(); phi_it.Advance()) {
        phi_it.Current()->AsPhi()->RemoveInputAt(this_index);
      }
    }
  }
  successors_.Reset();

  // Disconnect from the dominator.
  dominator_->RemoveDominatedBlock(this);
  SetDominator(nullptr);

  // Delete from the graph. The function safely deletes remaining instructions
  // and updates the reverse post order.
  graph_->DeleteDeadBlock(this);
  SetGraph(nullptr);
}

void HBasicBlock::MergeWith(HBasicBlock* other) {
  DCHECK_EQ(GetGraph(), other->GetGraph());
  DCHECK(GetDominatedBlocks().Contains(other));
  DCHECK_EQ(GetSuccessors().Size(), 1u);
  DCHECK_EQ(GetSuccessors().Get(0), other);
  DCHECK_EQ(other->GetPredecessors().Size(), 1u);
  DCHECK_EQ(other->GetPredecessors().Get(0), this);
  DCHECK(other->GetPhis().IsEmpty());

  // Move instructions from `other` to `this`.
  DCHECK(EndsWithControlFlowInstruction());
  RemoveInstruction(GetLastInstruction());
  instructions_.Add(other->GetInstructions());
  other->instructions_.SetBlockOfInstructions(this);
  other->instructions_.Clear();

  // Remove `other` from the loops it is included in.
  for (HLoopInformationOutwardIterator it(*other); !it.Done(); it.Advance()) {
    HLoopInformation* loop_info = it.Current();
    loop_info->Remove(other);
    if (loop_info->IsBackEdge(*other)) {
      loop_info->ReplaceBackEdge(other, this);
    }
  }

  // Update links to the successors of `other`.
  successors_.Reset();
  while (!other->successors_.IsEmpty()) {
    HBasicBlock* successor = other->successors_.Get(0);
    successor->ReplacePredecessor(other, this);
  }

  // Update the dominator tree.
  dominated_blocks_.Delete(other);
  for (size_t i = 0, e = other->GetDominatedBlocks().Size(); i < e; ++i) {
    HBasicBlock* dominated = other->GetDominatedBlocks().Get(i);
    dominated_blocks_.Add(dominated);
    dominated->SetDominator(this);
  }
  other->dominated_blocks_.Reset();
  other->dominator_ = nullptr;

  // Clear the list of predecessors of `other` in preparation of deleting it.
  other->predecessors_.Reset();

  // Delete `other` from the graph. The function updates reverse post order.
  graph_->DeleteDeadBlock(other);
  other->SetGraph(nullptr);
}

void HBasicBlock::MergeWithInlined(HBasicBlock* other) {
  DCHECK_NE(GetGraph(), other->GetGraph());
  DCHECK(GetDominatedBlocks().IsEmpty());
  DCHECK(GetSuccessors().IsEmpty());
  DCHECK(!EndsWithControlFlowInstruction());
  DCHECK_EQ(other->GetPredecessors().Size(), 1u);
  DCHECK(other->GetPredecessors().Get(0)->IsEntryBlock());
  DCHECK(other->GetPhis().IsEmpty());
  DCHECK(!other->IsInLoop());

  // Move instructions from `other` to `this`.
  instructions_.Add(other->GetInstructions());
  other->instructions_.SetBlockOfInstructions(this);

  // Update links to the successors of `other`.
  successors_.Reset();
  while (!other->successors_.IsEmpty()) {
    HBasicBlock* successor = other->successors_.Get(0);
    successor->ReplacePredecessor(other, this);
  }

  // Update the dominator tree.
  for (size_t i = 0, e = other->GetDominatedBlocks().Size(); i < e; ++i) {
    HBasicBlock* dominated = other->GetDominatedBlocks().Get(i);
    dominated_blocks_.Add(dominated);
    dominated->SetDominator(this);
  }
  other->dominated_blocks_.Reset();
  other->dominator_ = nullptr;
  other->graph_ = nullptr;
}

void HBasicBlock::ReplaceWith(HBasicBlock* other) {
  while (!GetPredecessors().IsEmpty()) {
    HBasicBlock* predecessor = GetPredecessors().Get(0);
    predecessor->ReplaceSuccessor(this, other);
  }
  while (!GetSuccessors().IsEmpty()) {
    HBasicBlock* successor = GetSuccessors().Get(0);
    successor->ReplacePredecessor(this, other);
  }
  for (size_t i = 0; i < dominated_blocks_.Size(); ++i) {
    other->AddDominatedBlock(dominated_blocks_.Get(i));
  }
  GetDominator()->ReplaceDominatedBlock(this, other);
  other->SetDominator(GetDominator());
  dominator_ = nullptr;
  graph_ = nullptr;
}

// Create space in `blocks` for adding `number_of_new_blocks` entries
// starting at location `at`. Blocks after `at` are moved accordingly.
static void MakeRoomFor(GrowableArray<HBasicBlock*>* blocks,
                        size_t number_of_new_blocks,
                        size_t at) {
  size_t old_size = blocks->Size();
  size_t new_size = old_size + number_of_new_blocks;
  blocks->SetSize(new_size);
  for (size_t i = old_size - 1, j = new_size - 1; i > at; --i, --j) {
    blocks->Put(j, blocks->Get(i));
  }
}

void HGraph::DeleteDeadBlock(HBasicBlock* block) {
  DCHECK_EQ(block->GetGraph(), this);
  DCHECK(block->GetSuccessors().IsEmpty());
  DCHECK(block->GetPredecessors().IsEmpty());
  DCHECK(block->GetDominatedBlocks().IsEmpty());
  DCHECK(block->GetDominator() == nullptr);

  for (HBackwardInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
    block->RemoveInstruction(it.Current());
  }
  for (HBackwardInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    block->RemovePhi(it.Current()->AsPhi());
  }

  reverse_post_order_.Delete(block);
  blocks_.Put(block->GetBlockId(), nullptr);
}

void HGraph::InlineInto(HGraph* outer_graph, HInvoke* invoke) {
  if (GetBlocks().Size() == 3) {
    // Simple case of an entry block, a body block, and an exit block.
    // Put the body block's instruction into `invoke`'s block.
    HBasicBlock* body = GetBlocks().Get(1);
    DCHECK(GetBlocks().Get(0)->IsEntryBlock());
    DCHECK(GetBlocks().Get(2)->IsExitBlock());
    DCHECK(!body->IsExitBlock());
    HInstruction* last = body->GetLastInstruction();

    invoke->GetBlock()->instructions_.AddAfter(invoke, body->GetInstructions());
    body->GetInstructions().SetBlockOfInstructions(invoke->GetBlock());

    // Replace the invoke with the return value of the inlined graph.
    if (last->IsReturn()) {
      invoke->ReplaceWith(last->InputAt(0));
    } else {
      DCHECK(last->IsReturnVoid());
    }

    invoke->GetBlock()->RemoveInstruction(last);
  } else {
    // Need to inline multiple blocks. We split `invoke`'s block
    // into two blocks, merge the first block of the inlined graph into
    // the first half, and replace the exit block of the inlined graph
    // with the second half.
    ArenaAllocator* allocator = outer_graph->GetArena();
    HBasicBlock* at = invoke->GetBlock();
    HBasicBlock* to = at->SplitAfter(invoke);

    HBasicBlock* first = entry_block_->GetSuccessors().Get(0);
    DCHECK(!first->IsInLoop());
    at->MergeWithInlined(first);
    exit_block_->ReplaceWith(to);

    // Update all predecessors of the exit block (now the `to` block)
    // to not `HReturn` but `HGoto` instead.
    HInstruction* return_value = nullptr;
    bool returns_void = to->GetPredecessors().Get(0)->GetLastInstruction()->IsReturnVoid();
    if (to->GetPredecessors().Size() == 1) {
      HBasicBlock* predecessor = to->GetPredecessors().Get(0);
      HInstruction* last = predecessor->GetLastInstruction();
      if (!returns_void) {
        return_value = last->InputAt(0);
      }
      predecessor->AddInstruction(new (allocator) HGoto());
      predecessor->RemoveInstruction(last);
    } else {
      if (!returns_void) {
        // There will be multiple returns.
        return_value = new (allocator) HPhi(
            allocator, kNoRegNumber, 0, HPhi::ToPhiType(invoke->GetType()));
        to->AddPhi(return_value->AsPhi());
      }
      for (size_t i = 0, e = to->GetPredecessors().Size(); i < e; ++i) {
        HBasicBlock* predecessor = to->GetPredecessors().Get(i);
        HInstruction* last = predecessor->GetLastInstruction();
        if (!returns_void) {
          return_value->AsPhi()->AddInput(last->InputAt(0));
        }
        predecessor->AddInstruction(new (allocator) HGoto());
        predecessor->RemoveInstruction(last);
      }
    }

    if (return_value != nullptr) {
      invoke->ReplaceWith(return_value);
    }

    // Update the meta information surrounding blocks:
    // (1) the graph they are now in,
    // (2) the reverse post order of that graph,
    // (3) the potential loop information they are now in.

    // We don't add the entry block, the exit block, and the first block, which
    // has been merged with `at`.
    static constexpr int kNumberOfSkippedBlocksInCallee = 3;

    // We add the `to` block.
    static constexpr int kNumberOfNewBlocksInCaller = 1;
    size_t blocks_added = (reverse_post_order_.Size() - kNumberOfSkippedBlocksInCallee)
        + kNumberOfNewBlocksInCaller;

    // Find the location of `at` in the outer graph's reverse post order. The new
    // blocks will be added after it.
    size_t index_of_at = 0;
    while (outer_graph->reverse_post_order_.Get(index_of_at) != at) {
      index_of_at++;
    }
    MakeRoomFor(&outer_graph->reverse_post_order_, blocks_added, index_of_at);

    // Do a reverse post order of the blocks in the callee and do (1), (2),
    // and (3) to the blocks that apply.
    HLoopInformation* info = at->GetLoopInformation();
    for (HReversePostOrderIterator it(*this); !it.Done(); it.Advance()) {
      HBasicBlock* current = it.Current();
      if (current != exit_block_ && current != entry_block_ && current != first) {
        DCHECK(!current->IsInLoop());
        DCHECK(current->GetGraph() == this);
        current->SetGraph(outer_graph);
        outer_graph->AddBlock(current);
        outer_graph->reverse_post_order_.Put(++index_of_at, current);
        if (info != nullptr) {
          current->SetLoopInformation(info);
          for (HLoopInformationOutwardIterator loop_it(*at); !loop_it.Done(); loop_it.Advance()) {
            loop_it.Current()->Add(current);
          }
        }
      }
    }

    // Do (1), (2), and (3) to `to`.
    to->SetGraph(outer_graph);
    outer_graph->AddBlock(to);
    outer_graph->reverse_post_order_.Put(++index_of_at, to);
    if (info != nullptr) {
      to->SetLoopInformation(info);
      for (HLoopInformationOutwardIterator loop_it(*at); !loop_it.Done(); loop_it.Advance()) {
        loop_it.Current()->Add(to);
      }
      if (info->IsBackEdge(*at)) {
        // Only `to` can become a back edge, as the inlined blocks
        // are predecessors of `to`.
        info->ReplaceBackEdge(at, to);
      }
    }
  }

  // Update the next instruction id of the outer graph, so that instructions
  // added later get bigger ids than those in the inner graph.
  outer_graph->SetCurrentInstructionId(GetNextInstructionId());

  // Walk over the entry block and:
  // - Move constants from the entry block to the outer_graph's entry block,
  // - Replace HParameterValue instructions with their real value.
  // - Remove suspend checks, that hold an environment.
  // We must do this after the other blocks have been inlined, otherwise ids of
  // constants could overlap with the inner graph.
  size_t parameter_index = 0;
  for (HInstructionIterator it(entry_block_->GetInstructions()); !it.Done(); it.Advance()) {
    HInstruction* current = it.Current();
    if (current->IsNullConstant()) {
      current->ReplaceWith(outer_graph->GetNullConstant());
    } else if (current->IsIntConstant()) {
      current->ReplaceWith(outer_graph->GetIntConstant(current->AsIntConstant()->GetValue()));
    } else if (current->IsLongConstant()) {
      current->ReplaceWith(outer_graph->GetLongConstant(current->AsLongConstant()->GetValue()));
    } else if (current->IsFloatConstant()) {
      current->ReplaceWith(outer_graph->GetFloatConstant(current->AsFloatConstant()->GetValue()));
    } else if (current->IsDoubleConstant()) {
      current->ReplaceWith(outer_graph->GetDoubleConstant(current->AsDoubleConstant()->GetValue()));
    } else if (current->IsParameterValue()) {
      if (kIsDebugBuild
          && invoke->IsInvokeStaticOrDirect()
          && invoke->AsInvokeStaticOrDirect()->IsStaticWithExplicitClinitCheck()) {
        // Ensure we do not use the last input of `invoke`, as it
        // contains a clinit check which is not an actual argument.
        size_t last_input_index = invoke->InputCount() - 1;
        DCHECK(parameter_index != last_input_index);
      }
      current->ReplaceWith(invoke->InputAt(parameter_index++));
    } else {
      DCHECK(current->IsGoto() || current->IsSuspendCheck());
      entry_block_->RemoveInstruction(current);
    }
  }

  // Finally remove the invoke from the caller.
  invoke->GetBlock()->RemoveInstruction(invoke);
}

/*
 * Loop will be transformed to:
 *       old_pre_header
 *             |
 *          if_block
 *           /    \
 *  dummy_block   deopt_block
 *           \    /
 *       new_pre_header
 *             |
 *           header
 */
void HGraph::TransformLoopHeaderForBCE(HBasicBlock* header) {
  DCHECK(header->IsLoopHeader());
  HBasicBlock* pre_header = header->GetDominator();

  // Need this to avoid critical edge.
  HBasicBlock* if_block = new (arena_) HBasicBlock(this, header->GetDexPc());
  // Need this to avoid critical edge.
  HBasicBlock* dummy_block = new (arena_) HBasicBlock(this, header->GetDexPc());
  HBasicBlock* deopt_block = new (arena_) HBasicBlock(this, header->GetDexPc());
  HBasicBlock* new_pre_header = new (arena_) HBasicBlock(this, header->GetDexPc());
  AddBlock(if_block);
  AddBlock(dummy_block);
  AddBlock(deopt_block);
  AddBlock(new_pre_header);

  header->ReplacePredecessor(pre_header, new_pre_header);
  pre_header->successors_.Reset();
  pre_header->dominated_blocks_.Reset();

  pre_header->AddSuccessor(if_block);
  if_block->AddSuccessor(dummy_block);  // True successor
  if_block->AddSuccessor(deopt_block);  // False successor
  dummy_block->AddSuccessor(new_pre_header);
  deopt_block->AddSuccessor(new_pre_header);

  pre_header->dominated_blocks_.Add(if_block);
  if_block->SetDominator(pre_header);
  if_block->dominated_blocks_.Add(dummy_block);
  dummy_block->SetDominator(if_block);
  if_block->dominated_blocks_.Add(deopt_block);
  deopt_block->SetDominator(if_block);
  if_block->dominated_blocks_.Add(new_pre_header);
  new_pre_header->SetDominator(if_block);
  new_pre_header->dominated_blocks_.Add(header);
  header->SetDominator(new_pre_header);

  size_t index_of_header = 0;
  while (reverse_post_order_.Get(index_of_header) != header) {
    index_of_header++;
  }
  MakeRoomFor(&reverse_post_order_, 4, index_of_header - 1);
  reverse_post_order_.Put(index_of_header++, if_block);
  reverse_post_order_.Put(index_of_header++, dummy_block);
  reverse_post_order_.Put(index_of_header++, deopt_block);
  reverse_post_order_.Put(index_of_header++, new_pre_header);

  HLoopInformation* info = pre_header->GetLoopInformation();
  if (info != nullptr) {
    if_block->SetLoopInformation(info);
    dummy_block->SetLoopInformation(info);
    deopt_block->SetLoopInformation(info);
    new_pre_header->SetLoopInformation(info);
    for (HLoopInformationOutwardIterator loop_it(*pre_header);
         !loop_it.Done();
         loop_it.Advance()) {
      loop_it.Current()->Add(if_block);
      loop_it.Current()->Add(dummy_block);
      loop_it.Current()->Add(deopt_block);
      loop_it.Current()->Add(new_pre_header);
    }
  }
}

std::ostream& operator<<(std::ostream& os, const ReferenceTypeInfo& rhs) {
  ScopedObjectAccess soa(Thread::Current());
  os << "["
     << " is_top=" << rhs.IsTop()
     << " type=" << (rhs.IsTop() ? "?" : PrettyClass(rhs.GetTypeHandle().Get()))
     << " is_exact=" << rhs.IsExact()
     << " ]";
  return os;
}

}  // namespace art
