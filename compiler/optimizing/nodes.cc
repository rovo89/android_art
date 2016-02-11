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

#include "code_generator.h"
#include "common_dominator.h"
#include "ssa_builder.h"
#include "base/bit_vector-inl.h"
#include "base/bit_utils.h"
#include "base/stl_util.h"
#include "intrinsics.h"
#include "mirror/class-inl.h"
#include "scoped_thread_state_change.h"

namespace art {

void HGraph::AddBlock(HBasicBlock* block) {
  block->SetBlockId(blocks_.size());
  blocks_.push_back(block);
}

void HGraph::FindBackEdges(ArenaBitVector* visited) {
  // "visited" must be empty on entry, it's an output argument for all visited (i.e. live) blocks.
  DCHECK_EQ(visited->GetHighestBitSet(), -1);

  // Nodes that we're currently visiting, indexed by block id.
  ArenaBitVector visiting(arena_, blocks_.size(), false);
  // Number of successors visited from a given node, indexed by block id.
  ArenaVector<size_t> successors_visited(blocks_.size(), 0u, arena_->Adapter());
  // Stack of nodes that we're currently visiting (same as marked in "visiting" above).
  ArenaVector<HBasicBlock*> worklist(arena_->Adapter());
  constexpr size_t kDefaultWorklistSize = 8;
  worklist.reserve(kDefaultWorklistSize);
  visited->SetBit(entry_block_->GetBlockId());
  visiting.SetBit(entry_block_->GetBlockId());
  worklist.push_back(entry_block_);

  while (!worklist.empty()) {
    HBasicBlock* current = worklist.back();
    uint32_t current_id = current->GetBlockId();
    if (successors_visited[current_id] == current->GetSuccessors().size()) {
      visiting.ClearBit(current_id);
      worklist.pop_back();
    } else {
      HBasicBlock* successor = current->GetSuccessors()[successors_visited[current_id]++];
      uint32_t successor_id = successor->GetBlockId();
      if (visiting.IsBitSet(successor_id)) {
        DCHECK(ContainsElement(worklist, successor));
        successor->AddBackEdge(current);
      } else if (!visited->IsBitSet(successor_id)) {
        visited->SetBit(successor_id);
        visiting.SetBit(successor_id);
        worklist.push_back(successor);
      }
    }
  }
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
  for (size_t i = 0; i < blocks_.size(); ++i) {
    if (!visited.IsBitSet(i)) {
      HBasicBlock* block = blocks_[i];
      if (block == nullptr) continue;
      DCHECK(block->GetPhis().IsEmpty()) << "Phis are not inserted at this stage";
      for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
        RemoveAsUser(it.Current());
      }
    }
  }
}

void HGraph::RemoveDeadBlocks(const ArenaBitVector& visited) {
  for (size_t i = 0; i < blocks_.size(); ++i) {
    if (!visited.IsBitSet(i)) {
      HBasicBlock* block = blocks_[i];
      if (block == nullptr) continue;
      // We only need to update the successor, which might be live.
      for (HBasicBlock* successor : block->GetSuccessors()) {
        successor->RemovePredecessor(block);
      }
      // Remove the block from the list of blocks, so that further analyses
      // never see it.
      blocks_[i] = nullptr;
    }
  }
}

GraphAnalysisResult HGraph::BuildDominatorTree() {
  // (1) Simplify the CFG so that catch blocks have only exceptional incoming
  //     edges. This invariant simplifies building SSA form because Phis cannot
  //     collect both normal- and exceptional-flow values at the same time.
  SimplifyCatchBlocks();

  ArenaBitVector visited(arena_, blocks_.size(), false);

  // (2) Find the back edges in the graph doing a DFS traversal.
  FindBackEdges(&visited);

  // (3) Remove instructions and phis from blocks not visited during
  //     the initial DFS as users from other instructions, so that
  //     users can be safely removed before uses later.
  RemoveInstructionsAsUsersFromDeadBlocks(visited);

  // (4) Remove blocks not visited during the initial DFS.
  //     Step (5) requires dead blocks to be removed from the
  //     predecessors list of live blocks.
  RemoveDeadBlocks(visited);

  // (5) Simplify the CFG now, so that we don't need to recompute
  //     dominators and the reverse post order.
  SimplifyCFG();

  // (6) Compute the dominance information and the reverse post order.
  ComputeDominanceInformation();

  // (7) Analyze loops discover through back edge analysis, and
  //     set the loop information on each block.
  GraphAnalysisResult result = AnalyzeLoops();
  if (result != kAnalysisSuccess) {
    return result;
  }

  // (8) Precompute per-block try membership before entering the SSA builder,
  //     which needs the information to build catch block phis from values of
  //     locals at throwing instructions inside try blocks.
  ComputeTryBlockInformation();

  return kAnalysisSuccess;
}

void HGraph::ClearDominanceInformation() {
  for (HReversePostOrderIterator it(*this); !it.Done(); it.Advance()) {
    it.Current()->ClearDominanceInformation();
  }
  reverse_post_order_.clear();
}

void HGraph::ClearLoopInformation() {
  SetHasIrreducibleLoops(false);
  for (HReversePostOrderIterator it(*this); !it.Done(); it.Advance()) {
    it.Current()->SetLoopInformation(nullptr);
  }
}

void HBasicBlock::ClearDominanceInformation() {
  dominated_blocks_.clear();
  dominator_ = nullptr;
}

HInstruction* HBasicBlock::GetFirstInstructionDisregardMoves() const {
  HInstruction* instruction = GetFirstInstruction();
  while (instruction->IsParallelMove()) {
    instruction = instruction->GetNext();
  }
  return instruction;
}

void HGraph::ComputeDominanceInformation() {
  DCHECK(reverse_post_order_.empty());
  reverse_post_order_.reserve(blocks_.size());
  reverse_post_order_.push_back(entry_block_);

  // Number of visits of a given node, indexed by block id.
  ArenaVector<size_t> visits(blocks_.size(), 0u, arena_->Adapter());
  // Number of successors visited from a given node, indexed by block id.
  ArenaVector<size_t> successors_visited(blocks_.size(), 0u, arena_->Adapter());
  // Nodes for which we need to visit successors.
  ArenaVector<HBasicBlock*> worklist(arena_->Adapter());
  constexpr size_t kDefaultWorklistSize = 8;
  worklist.reserve(kDefaultWorklistSize);
  worklist.push_back(entry_block_);

  while (!worklist.empty()) {
    HBasicBlock* current = worklist.back();
    uint32_t current_id = current->GetBlockId();
    if (successors_visited[current_id] == current->GetSuccessors().size()) {
      worklist.pop_back();
    } else {
      HBasicBlock* successor = current->GetSuccessors()[successors_visited[current_id]++];

      if (successor->GetDominator() == nullptr) {
        successor->SetDominator(current);
      } else {
        // The CommonDominator can work for multiple blocks as long as the
        // domination information doesn't change. However, since we're changing
        // that information here, we can use the finder only for pairs of blocks.
        successor->SetDominator(CommonDominator::ForPair(successor->GetDominator(), current));
      }

      // Once all the forward edges have been visited, we know the immediate
      // dominator of the block. We can then start visiting its successors.
      if (++visits[successor->GetBlockId()] ==
          successor->GetPredecessors().size() - successor->NumberOfBackEdges()) {
        reverse_post_order_.push_back(successor);
        worklist.push_back(successor);
      }
    }
  }

  // Populate `dominated_blocks_` information after computing all dominators.
  // The potential presence of irreducible loops require to do it after.
  for (HReversePostOrderIterator it(*this); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    if (!block->IsEntryBlock()) {
      block->GetDominator()->AddDominatedBlock(block);
    }
  }
}

GraphAnalysisResult HGraph::TryBuildingSsa(StackHandleScopeCollection* handles) {
  GraphAnalysisResult result = BuildDominatorTree();
  if (result != kAnalysisSuccess) {
    return result;
  }

  // Create the inexact Object reference type and store it in the HGraph.
  ScopedObjectAccess soa(Thread::Current());
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  inexact_object_rti_ = ReferenceTypeInfo::Create(
      handles->NewHandle(linker->GetClassRoot(ClassLinker::kJavaLangObject)),
      /* is_exact */ false);

  // Tranforms graph to SSA form.
  result = SsaBuilder(this, handles).BuildSsa();
  if (result != kAnalysisSuccess) {
    return result;
  }

  in_ssa_form_ = true;
  return kAnalysisSuccess;
}

HBasicBlock* HGraph::SplitEdge(HBasicBlock* block, HBasicBlock* successor) {
  HBasicBlock* new_block = new (arena_) HBasicBlock(this, successor->GetDexPc());
  AddBlock(new_block);
  // Use `InsertBetween` to ensure the predecessor index and successor index of
  // `block` and `successor` are preserved.
  new_block->InsertBetween(block, successor);
  return new_block;
}

void HGraph::SplitCriticalEdge(HBasicBlock* block, HBasicBlock* successor) {
  // Insert a new node between `block` and `successor` to split the
  // critical edge.
  HBasicBlock* new_block = SplitEdge(block, successor);
  new_block->AddInstruction(new (arena_) HGoto(successor->GetDexPc()));
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
  // loop. Also, don't allow the entry block to be a pre header: this simplifies inlining
  // this graph.
  size_t number_of_incomings = header->GetPredecessors().size() - info->NumberOfBackEdges();
  if (number_of_incomings != 1 || (GetEntryBlock()->GetSingleSuccessor() == header)) {
    HBasicBlock* pre_header = new (arena_) HBasicBlock(this, header->GetDexPc());
    AddBlock(pre_header);
    pre_header->AddInstruction(new (arena_) HGoto(header->GetDexPc()));

    for (size_t pred = 0; pred < header->GetPredecessors().size(); ++pred) {
      HBasicBlock* predecessor = header->GetPredecessors()[pred];
      if (!info->IsBackEdge(*predecessor)) {
        predecessor->ReplaceSuccessor(header, pre_header);
        pred--;
      }
    }
    pre_header->AddSuccessor(header);
  }

  // Make sure the first predecessor of a loop header is the incoming block.
  if (info->IsBackEdge(*header->GetPredecessors()[0])) {
    HBasicBlock* to_swap = header->GetPredecessors()[0];
    for (size_t pred = 1, e = header->GetPredecessors().size(); pred < e; ++pred) {
      HBasicBlock* predecessor = header->GetPredecessors()[pred];
      if (!info->IsBackEdge(*predecessor)) {
        header->predecessors_[pred] = to_swap;
        header->predecessors_[0] = predecessor;
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

static bool CheckIfPredecessorAtIsExceptional(const HBasicBlock& block, size_t pred_idx) {
  HBasicBlock* predecessor = block.GetPredecessors()[pred_idx];
  if (!predecessor->EndsWithTryBoundary()) {
    // Only edges from HTryBoundary can be exceptional.
    return false;
  }
  HTryBoundary* try_boundary = predecessor->GetLastInstruction()->AsTryBoundary();
  if (try_boundary->GetNormalFlowSuccessor() == &block) {
    // This block is the normal-flow successor of `try_boundary`, but it could
    // also be one of its exception handlers if catch blocks have not been
    // simplified yet. Predecessors are unordered, so we will consider the first
    // occurrence to be the normal edge and a possible second occurrence to be
    // the exceptional edge.
    return !block.IsFirstIndexOfPredecessor(predecessor, pred_idx);
  } else {
    // This is not the normal-flow successor of `try_boundary`, hence it must be
    // one of its exception handlers.
    DCHECK(try_boundary->HasExceptionHandler(block));
    return true;
  }
}

void HGraph::SimplifyCatchBlocks() {
  // NOTE: We're appending new blocks inside the loop, so we need to use index because iterators
  // can be invalidated. We remember the initial size to avoid iterating over the new blocks.
  for (size_t block_id = 0u, end = blocks_.size(); block_id != end; ++block_id) {
    HBasicBlock* catch_block = blocks_[block_id];
    if (catch_block == nullptr || !catch_block->IsCatchBlock()) {
      continue;
    }

    bool exceptional_predecessors_only = true;
    for (size_t j = 0; j < catch_block->GetPredecessors().size(); ++j) {
      if (!CheckIfPredecessorAtIsExceptional(*catch_block, j)) {
        exceptional_predecessors_only = false;
        break;
      }
    }

    if (!exceptional_predecessors_only) {
      // Catch block has normal-flow predecessors and needs to be simplified.
      // Splitting the block before its first instruction moves all its
      // instructions into `normal_block` and links the two blocks with a Goto.
      // Afterwards, incoming normal-flow edges are re-linked to `normal_block`,
      // leaving `catch_block` with the exceptional edges only.
      //
      // Note that catch blocks with normal-flow predecessors cannot begin with
      // a move-exception instruction, as guaranteed by the verifier. However,
      // trivially dead predecessors are ignored by the verifier and such code
      // has not been removed at this stage. We therefore ignore the assumption
      // and rely on GraphChecker to enforce it after initial DCE is run (b/25492628).
      HBasicBlock* normal_block = catch_block->SplitCatchBlockAfterMoveException();
      if (normal_block == nullptr) {
        // Catch block is either empty or only contains a move-exception. It must
        // therefore be dead and will be removed during initial DCE. Do nothing.
        DCHECK(!catch_block->EndsWithControlFlowInstruction());
      } else {
        // Catch block was split. Re-link normal-flow edges to the new block.
        for (size_t j = 0; j < catch_block->GetPredecessors().size(); ++j) {
          if (!CheckIfPredecessorAtIsExceptional(*catch_block, j)) {
            catch_block->GetPredecessors()[j]->ReplaceSuccessor(catch_block, normal_block);
            --j;
          }
        }
      }
    }
  }
}

void HGraph::ComputeTryBlockInformation() {
  // Iterate in reverse post order to propagate try membership information from
  // predecessors to their successors.
  for (HReversePostOrderIterator it(*this); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    if (block->IsEntryBlock() || block->IsCatchBlock()) {
      // Catch blocks after simplification have only exceptional predecessors
      // and hence are never in tries.
      continue;
    }

    // Infer try membership from the first predecessor. Having simplified loops,
    // the first predecessor can never be a back edge and therefore it must have
    // been visited already and had its try membership set.
    HBasicBlock* first_predecessor = block->GetPredecessors()[0];
    DCHECK(!block->IsLoopHeader() || !block->GetLoopInformation()->IsBackEdge(*first_predecessor));
    const HTryBoundary* try_entry = first_predecessor->ComputeTryEntryOfSuccessors();
    if (try_entry != nullptr &&
        (block->GetTryCatchInformation() == nullptr ||
         try_entry != &block->GetTryCatchInformation()->GetTryEntry())) {
      // We are either setting try block membership for the first time or it
      // has changed.
      block->SetTryCatchInformation(new (arena_) TryCatchInformation(*try_entry));
    }
  }
}

void HGraph::SimplifyCFG() {
// Simplify the CFG for future analysis, and code generation:
  // (1): Split critical edges.
  // (2): Simplify loops by having only one preheader.
  // NOTE: We're appending new blocks inside the loop, so we need to use index because iterators
  // can be invalidated. We remember the initial size to avoid iterating over the new blocks.
  for (size_t block_id = 0u, end = blocks_.size(); block_id != end; ++block_id) {
    HBasicBlock* block = blocks_[block_id];
    if (block == nullptr) continue;
    if (block->GetSuccessors().size() > 1) {
      // Only split normal-flow edges. We cannot split exceptional edges as they
      // are synthesized (approximate real control flow), and we do not need to
      // anyway. Moves that would be inserted there are performed by the runtime.
      ArrayRef<HBasicBlock* const> normal_successors = block->GetNormalSuccessors();
      for (size_t j = 0, e = normal_successors.size(); j < e; ++j) {
        HBasicBlock* successor = normal_successors[j];
        DCHECK(!successor->IsCatchBlock());
        if (successor == exit_block_) {
          // Throw->TryBoundary->Exit. Special case which we do not want to split
          // because Goto->Exit is not allowed.
          DCHECK(block->IsSingleTryBoundary());
          DCHECK(block->GetSinglePredecessor()->GetLastInstruction()->IsThrow());
        } else if (successor->GetPredecessors().size() > 1) {
          SplitCriticalEdge(block, successor);
          // SplitCriticalEdge could have invalidated the `normal_successors`
          // ArrayRef. We must re-acquire it.
          normal_successors = block->GetNormalSuccessors();
          DCHECK_EQ(normal_successors[j]->GetSingleSuccessor(), successor);
          DCHECK_EQ(e, normal_successors.size());
        }
      }
    }
    if (block->IsLoopHeader()) {
      SimplifyLoop(block);
    } else if (!block->IsEntryBlock() && block->GetFirstInstruction()->IsSuspendCheck()) {
      // We are being called by the dead code elimiation pass, and what used to be
      // a loop got dismantled. Just remove the suspend check.
      block->RemoveInstruction(block->GetFirstInstruction());
    }
  }
}

GraphAnalysisResult HGraph::AnalyzeLoops() const {
  // Order does not matter.
  for (HReversePostOrderIterator it(*this); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    if (block->IsLoopHeader()) {
      if (block->IsCatchBlock()) {
        // TODO: Dealing with exceptional back edges could be tricky because
        //       they only approximate the real control flow. Bail out for now.
        return kAnalysisFailThrowCatchLoop;
      }
      block->GetLoopInformation()->Populate();
    }
  }
  return kAnalysisSuccess;
}

void HLoopInformation::Dump(std::ostream& os) {
  os << "header: " << header_->GetBlockId() << std::endl;
  os << "pre header: " << GetPreHeader()->GetBlockId() << std::endl;
  for (HBasicBlock* block : back_edges_) {
    os << "back edge: " << block->GetBlockId() << std::endl;
  }
  for (HBasicBlock* block : header_->GetPredecessors()) {
    os << "predecessor: " << block->GetBlockId() << std::endl;
  }
  for (uint32_t idx : blocks_.Indexes()) {
    os << "  in loop: " << idx << std::endl;
  }
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

HNullConstant* HGraph::GetNullConstant(uint32_t dex_pc) {
  // For simplicity, don't bother reviving the cached null constant if it is
  // not null and not in a block. Otherwise, we need to clear the instruction
  // id and/or any invariants the graph is assuming when adding new instructions.
  if ((cached_null_constant_ == nullptr) || (cached_null_constant_->GetBlock() == nullptr)) {
    cached_null_constant_ = new (arena_) HNullConstant(dex_pc);
    cached_null_constant_->SetReferenceTypeInfo(inexact_object_rti_);
    InsertConstant(cached_null_constant_);
  }
  if (kIsDebugBuild) {
    ScopedObjectAccess soa(Thread::Current());
    DCHECK(cached_null_constant_->GetReferenceTypeInfo().IsValid());
  }
  return cached_null_constant_;
}

HCurrentMethod* HGraph::GetCurrentMethod() {
  // For simplicity, don't bother reviving the cached current method if it is
  // not null and not in a block. Otherwise, we need to clear the instruction
  // id and/or any invariants the graph is assuming when adding new instructions.
  if ((cached_current_method_ == nullptr) || (cached_current_method_->GetBlock() == nullptr)) {
    cached_current_method_ = new (arena_) HCurrentMethod(
        Is64BitInstructionSet(instruction_set_) ? Primitive::kPrimLong : Primitive::kPrimInt,
        entry_block_->GetDexPc());
    if (entry_block_->GetFirstInstruction() == nullptr) {
      entry_block_->AddInstruction(cached_current_method_);
    } else {
      entry_block_->InsertInstructionBefore(
          cached_current_method_, entry_block_->GetFirstInstruction());
    }
  }
  return cached_current_method_;
}

HConstant* HGraph::GetConstant(Primitive::Type type, int64_t value, uint32_t dex_pc) {
  switch (type) {
    case Primitive::Type::kPrimBoolean:
      DCHECK(IsUint<1>(value));
      FALLTHROUGH_INTENDED;
    case Primitive::Type::kPrimByte:
    case Primitive::Type::kPrimChar:
    case Primitive::Type::kPrimShort:
    case Primitive::Type::kPrimInt:
      DCHECK(IsInt(Primitive::ComponentSize(type) * kBitsPerByte, value));
      return GetIntConstant(static_cast<int32_t>(value), dex_pc);

    case Primitive::Type::kPrimLong:
      return GetLongConstant(value, dex_pc);

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
  for (HBasicBlock* predecessor : block->GetPredecessors()) {
    PopulateRecursive(predecessor);
  }
}

void HLoopInformation::PopulateIrreducibleRecursive(HBasicBlock* block) {
  if (blocks_.IsBitSet(block->GetBlockId())) {
    return;
  }

  if (block->IsLoopHeader()) {
    // If we hit a loop header in an irreducible loop, we first check if the
    // pre header of that loop belongs to the currently analyzed loop. If it does,
    // then we visit the back edges.
    // Note that we cannot use GetPreHeader, as the loop may have not been populated
    // yet.
    HBasicBlock* pre_header = block->GetPredecessors()[0];
    PopulateIrreducibleRecursive(pre_header);
    if (blocks_.IsBitSet(pre_header->GetBlockId())) {
      blocks_.SetBit(block->GetBlockId());
      block->SetInLoop(this);
      HLoopInformation* info = block->GetLoopInformation();
      for (HBasicBlock* back_edge : info->GetBackEdges()) {
        PopulateIrreducibleRecursive(back_edge);
      }
    }
  } else {
    // Visit all predecessors. If one predecessor is part of the loop, this
    // block is also part of this loop.
    for (HBasicBlock* predecessor : block->GetPredecessors()) {
      PopulateIrreducibleRecursive(predecessor);
      if (blocks_.IsBitSet(predecessor->GetBlockId())) {
        blocks_.SetBit(block->GetBlockId());
        block->SetInLoop(this);
      }
    }
  }
}

void HLoopInformation::Populate() {
  DCHECK_EQ(blocks_.NumSetBits(), 0u) << "Loop information has already been populated";
  // Populate this loop: starting with the back edge, recursively add predecessors
  // that are not already part of that loop. Set the header as part of the loop
  // to end the recursion.
  // This is a recursive implementation of the algorithm described in
  // "Advanced Compiler Design & Implementation" (Muchnick) p192.
  blocks_.SetBit(header_->GetBlockId());
  header_->SetInLoop(this);
  for (HBasicBlock* back_edge : GetBackEdges()) {
    DCHECK(back_edge->GetDominator() != nullptr);
    if (!header_->Dominates(back_edge)) {
      irreducible_ = true;
      header_->GetGraph()->SetHasIrreducibleLoops(true);
      PopulateIrreducibleRecursive(back_edge);
    } else {
      if (header_->GetGraph()->IsCompilingOsr()) {
        irreducible_ = true;
        header_->GetGraph()->SetHasIrreducibleLoops(true);
      }
      PopulateRecursive(back_edge);
    }
  }
}

HBasicBlock* HLoopInformation::GetPreHeader() const {
  HBasicBlock* block = header_->GetPredecessors()[0];
  DCHECK(irreducible_ || (block == header_->GetDominator()));
  return block;
}

bool HLoopInformation::Contains(const HBasicBlock& block) const {
  return blocks_.IsBitSet(block.GetBlockId());
}

bool HLoopInformation::IsIn(const HLoopInformation& other) const {
  return other.blocks_.IsBitSet(header_->GetBlockId());
}

bool HLoopInformation::IsDefinedOutOfTheLoop(HInstruction* instruction) const {
  return !blocks_.IsBitSet(instruction->GetBlock()->GetBlockId());
}

size_t HLoopInformation::GetLifetimeEnd() const {
  size_t last_position = 0;
  for (HBasicBlock* back_edge : GetBackEdges()) {
    last_position = std::max(back_edge->GetLifetimeEnd(), last_position);
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
  if (initial->IsControlFlow()) {
    // We can only replace a control flow instruction with another control flow instruction.
    DCHECK(replacement->IsControlFlow());
    DCHECK_EQ(replacement->GetId(), -1);
    DCHECK_EQ(replacement->GetType(), Primitive::kPrimVoid);
    DCHECK_EQ(initial->GetBlock(), this);
    DCHECK_EQ(initial->GetType(), Primitive::kPrimVoid);
    DCHECK(initial->GetUses().IsEmpty());
    DCHECK(initial->GetEnvUses().IsEmpty());
    replacement->SetBlock(this);
    replacement->SetId(GetGraph()->GetNextInstructionId());
    instructions_.InsertInstructionBefore(replacement, initial);
    UpdateInputsUsers(replacement);
  } else {
    InsertInstructionBefore(replacement, initial);
    initial->ReplaceWith(replacement);
  }
  RemoveInstruction(initial);
}

void HBasicBlock::MoveInstructionBefore(HInstruction* insn, HInstruction* cursor) {
  DCHECK(!cursor->IsPhi());
  DCHECK(!insn->IsPhi());
  DCHECK(!insn->IsControlFlow());
  DCHECK(insn->CanBeMoved());
  DCHECK(!insn->HasSideEffects());

  HBasicBlock* from_block = insn->GetBlock();
  HBasicBlock* to_block = cursor->GetBlock();
  DCHECK(from_block != to_block);

  from_block->RemoveInstruction(insn, /* ensure_safety */ false);
  insn->SetBlock(to_block);
  to_block->instructions_.InsertInstructionBefore(insn, cursor);
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

void HEnvironment::CopyFrom(const ArenaVector<HInstruction*>& locals) {
  for (size_t i = 0; i < locals.size(); i++) {
    HInstruction* instruction = locals[i];
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
      SetRawEnvAt(i, initial);
      initial->AddEnvUseAt(this, i);
    } else {
      instruction->AddEnvUseAt(this, i);
    }
  }
}

void HEnvironment::RemoveAsUserOfInput(size_t index) const {
  const HUserRecord<HEnvironment*>& user_record = vregs_[index];
  user_record.GetInstruction()->RemoveEnvironmentUser(user_record.GetUseNode());
}

HInstruction::InstructionKind HInstruction::GetKind() const {
  return GetKindInternal();
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
  inputs_.push_back(HUserRecord<HInstruction*>(input));
  input->AddUseAt(this, inputs_.size() - 1);
}

void HPhi::RemoveInputAt(size_t index) {
  RemoveAsUserOfInput(index);
  inputs_.erase(inputs_.begin() + index);
  for (size_t i = index, e = InputCount(); i < e; ++i) {
    DCHECK_EQ(InputRecordAt(i).GetUseNode()->GetIndex(), i + 1u);
    InputRecordAt(i).GetUseNode()->SetIndex(i);
  }
}

#define DEFINE_ACCEPT(name, super)                                             \
void H##name::Accept(HGraphVisitor* visitor) {                                 \
  visitor->Visit##name(this);                                                  \
}

FOR_EACH_CONCRETE_INSTRUCTION(DEFINE_ACCEPT)

#undef DEFINE_ACCEPT

void HGraphVisitor::VisitInsertionOrder() {
  const ArenaVector<HBasicBlock*>& blocks = graph_->GetBlocks();
  for (HBasicBlock* block : blocks) {
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

HConstant* HTypeConversion::TryStaticEvaluation() const {
  HGraph* graph = GetBlock()->GetGraph();
  if (GetInput()->IsIntConstant()) {
    int32_t value = GetInput()->AsIntConstant()->GetValue();
    switch (GetResultType()) {
      case Primitive::kPrimLong:
        return graph->GetLongConstant(static_cast<int64_t>(value), GetDexPc());
      case Primitive::kPrimFloat:
        return graph->GetFloatConstant(static_cast<float>(value), GetDexPc());
      case Primitive::kPrimDouble:
        return graph->GetDoubleConstant(static_cast<double>(value), GetDexPc());
      default:
        return nullptr;
    }
  } else if (GetInput()->IsLongConstant()) {
    int64_t value = GetInput()->AsLongConstant()->GetValue();
    switch (GetResultType()) {
      case Primitive::kPrimInt:
        return graph->GetIntConstant(static_cast<int32_t>(value), GetDexPc());
      case Primitive::kPrimFloat:
        return graph->GetFloatConstant(static_cast<float>(value), GetDexPc());
      case Primitive::kPrimDouble:
        return graph->GetDoubleConstant(static_cast<double>(value), GetDexPc());
      default:
        return nullptr;
    }
  } else if (GetInput()->IsFloatConstant()) {
    float value = GetInput()->AsFloatConstant()->GetValue();
    switch (GetResultType()) {
      case Primitive::kPrimInt:
        if (std::isnan(value))
          return graph->GetIntConstant(0, GetDexPc());
        if (value >= kPrimIntMax)
          return graph->GetIntConstant(kPrimIntMax, GetDexPc());
        if (value <= kPrimIntMin)
          return graph->GetIntConstant(kPrimIntMin, GetDexPc());
        return graph->GetIntConstant(static_cast<int32_t>(value), GetDexPc());
      case Primitive::kPrimLong:
        if (std::isnan(value))
          return graph->GetLongConstant(0, GetDexPc());
        if (value >= kPrimLongMax)
          return graph->GetLongConstant(kPrimLongMax, GetDexPc());
        if (value <= kPrimLongMin)
          return graph->GetLongConstant(kPrimLongMin, GetDexPc());
        return graph->GetLongConstant(static_cast<int64_t>(value), GetDexPc());
      case Primitive::kPrimDouble:
        return graph->GetDoubleConstant(static_cast<double>(value), GetDexPc());
      default:
        return nullptr;
    }
  } else if (GetInput()->IsDoubleConstant()) {
    double value = GetInput()->AsDoubleConstant()->GetValue();
    switch (GetResultType()) {
      case Primitive::kPrimInt:
        if (std::isnan(value))
          return graph->GetIntConstant(0, GetDexPc());
        if (value >= kPrimIntMax)
          return graph->GetIntConstant(kPrimIntMax, GetDexPc());
        if (value <= kPrimLongMin)
          return graph->GetIntConstant(kPrimIntMin, GetDexPc());
        return graph->GetIntConstant(static_cast<int32_t>(value), GetDexPc());
      case Primitive::kPrimLong:
        if (std::isnan(value))
          return graph->GetLongConstant(0, GetDexPc());
        if (value >= kPrimLongMax)
          return graph->GetLongConstant(kPrimLongMax, GetDexPc());
        if (value <= kPrimLongMin)
          return graph->GetLongConstant(kPrimLongMin, GetDexPc());
        return graph->GetLongConstant(static_cast<int64_t>(value), GetDexPc());
      case Primitive::kPrimFloat:
        return graph->GetFloatConstant(static_cast<float>(value), GetDexPc());
      default:
        return nullptr;
    }
  }
  return nullptr;
}

HConstant* HUnaryOperation::TryStaticEvaluation() const {
  if (GetInput()->IsIntConstant()) {
    return Evaluate(GetInput()->AsIntConstant());
  } else if (GetInput()->IsLongConstant()) {
    return Evaluate(GetInput()->AsLongConstant());
  }
  return nullptr;
}

HConstant* HBinaryOperation::TryStaticEvaluation() const {
  if (GetLeft()->IsIntConstant()) {
    if (GetRight()->IsIntConstant()) {
      return Evaluate(GetLeft()->AsIntConstant(), GetRight()->AsIntConstant());
    } else if (GetRight()->IsLongConstant()) {
      return Evaluate(GetLeft()->AsIntConstant(), GetRight()->AsLongConstant());
    }
  } else if (GetLeft()->IsLongConstant()) {
    if (GetRight()->IsIntConstant()) {
      return Evaluate(GetLeft()->AsLongConstant(), GetRight()->AsIntConstant());
    } else if (GetRight()->IsLongConstant()) {
      return Evaluate(GetLeft()->AsLongConstant(), GetRight()->AsLongConstant());
    }
  } else if (GetLeft()->IsNullConstant() && GetRight()->IsNullConstant()) {
    return Evaluate(GetLeft()->AsNullConstant(), GetRight()->AsNullConstant());
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

void HInstruction::MoveBeforeFirstUserAndOutOfLoops() {
  DCHECK(!CanThrow());
  DCHECK(!HasSideEffects());
  DCHECK(!HasEnvironmentUses());
  DCHECK(HasNonEnvironmentUses());
  DCHECK(!IsPhi());  // Makes no sense for Phi.
  DCHECK_EQ(InputCount(), 0u);

  // Find the target block.
  HUseIterator<HInstruction*> uses_it(GetUses());
  HBasicBlock* target_block = uses_it.Current()->GetUser()->GetBlock();
  uses_it.Advance();
  while (!uses_it.Done() && uses_it.Current()->GetUser()->GetBlock() == target_block) {
    uses_it.Advance();
  }
  if (!uses_it.Done()) {
    // This instruction has uses in two or more blocks. Find the common dominator.
    CommonDominator finder(target_block);
    for (; !uses_it.Done(); uses_it.Advance()) {
      finder.Update(uses_it.Current()->GetUser()->GetBlock());
    }
    target_block = finder.Get();
    DCHECK(target_block != nullptr);
  }
  // Move to the first dominator not in a loop.
  while (target_block->IsInLoop()) {
    target_block = target_block->GetDominator();
    DCHECK(target_block != nullptr);
  }

  // Find insertion position.
  HInstruction* insert_pos = nullptr;
  for (HUseIterator<HInstruction*> uses_it2(GetUses()); !uses_it2.Done(); uses_it2.Advance()) {
    if (uses_it2.Current()->GetUser()->GetBlock() == target_block &&
        (insert_pos == nullptr || uses_it2.Current()->GetUser()->StrictlyDominates(insert_pos))) {
      insert_pos = uses_it2.Current()->GetUser();
    }
  }
  if (insert_pos == nullptr) {
    // No user in `target_block`, insert before the control flow instruction.
    insert_pos = target_block->GetLastInstruction();
    DCHECK(insert_pos->IsControlFlow());
    // Avoid splitting HCondition from HIf to prevent unnecessary materialization.
    if (insert_pos->IsIf()) {
      HInstruction* if_input = insert_pos->AsIf()->InputAt(0);
      if (if_input == insert_pos->GetPrevious()) {
        insert_pos = if_input;
      }
    }
  }
  MoveBefore(insert_pos);
}

HBasicBlock* HBasicBlock::SplitBefore(HInstruction* cursor) {
  DCHECK(!graph_->IsInSsaForm()) << "Support for SSA form not implemented.";
  DCHECK_EQ(cursor->GetBlock(), this);

  HBasicBlock* new_block = new (GetGraph()->GetArena()) HBasicBlock(GetGraph(),
                                                                    cursor->GetDexPc());
  new_block->instructions_.first_instruction_ = cursor;
  new_block->instructions_.last_instruction_ = instructions_.last_instruction_;
  instructions_.last_instruction_ = cursor->previous_;
  if (cursor->previous_ == nullptr) {
    instructions_.first_instruction_ = nullptr;
  } else {
    cursor->previous_->next_ = nullptr;
    cursor->previous_ = nullptr;
  }

  new_block->instructions_.SetBlockOfInstructions(new_block);
  AddInstruction(new (GetGraph()->GetArena()) HGoto(new_block->GetDexPc()));

  for (HBasicBlock* successor : GetSuccessors()) {
    new_block->successors_.push_back(successor);
    successor->predecessors_[successor->GetPredecessorIndexOf(this)] = new_block;
  }
  successors_.clear();
  AddSuccessor(new_block);

  GetGraph()->AddBlock(new_block);
  return new_block;
}

HBasicBlock* HBasicBlock::CreateImmediateDominator() {
  DCHECK(!graph_->IsInSsaForm()) << "Support for SSA form not implemented.";
  DCHECK(!IsCatchBlock()) << "Support for updating try/catch information not implemented.";

  HBasicBlock* new_block = new (GetGraph()->GetArena()) HBasicBlock(GetGraph(), GetDexPc());

  for (HBasicBlock* predecessor : GetPredecessors()) {
    new_block->predecessors_.push_back(predecessor);
    predecessor->successors_[predecessor->GetSuccessorIndexOf(this)] = new_block;
  }
  predecessors_.clear();
  AddPredecessor(new_block);

  GetGraph()->AddBlock(new_block);
  return new_block;
}

HBasicBlock* HBasicBlock::SplitCatchBlockAfterMoveException() {
  DCHECK(!graph_->IsInSsaForm()) << "Support for SSA form not implemented.";
  DCHECK(IsCatchBlock()) << "This method is intended for catch blocks only.";

  HInstruction* first_insn = GetFirstInstruction();
  HInstruction* split_before = nullptr;

  if (first_insn != nullptr && first_insn->IsLoadException()) {
    // Catch block starts with a LoadException. Split the block after
    // the StoreLocal and ClearException which must come after the load.
    DCHECK(first_insn->GetNext()->IsStoreLocal());
    DCHECK(first_insn->GetNext()->GetNext()->IsClearException());
    split_before = first_insn->GetNext()->GetNext()->GetNext();
  } else {
    // Catch block does not load the exception. Split at the beginning
    // to create an empty catch block.
    split_before = first_insn;
  }

  if (split_before == nullptr) {
    // Catch block has no instructions after the split point (must be dead).
    // Do not split it but rather signal error by returning nullptr.
    return nullptr;
  } else {
    return SplitBefore(split_before);
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
  for (HBasicBlock* successor : GetSuccessors()) {
    new_block->successors_.push_back(successor);
    successor->predecessors_[successor->GetPredecessorIndexOf(this)] = new_block;
  }
  successors_.clear();

  for (HBasicBlock* dominated : GetDominatedBlocks()) {
    dominated->dominator_ = new_block;
    new_block->dominated_blocks_.push_back(dominated);
  }
  dominated_blocks_.clear();
  return new_block;
}

const HTryBoundary* HBasicBlock::ComputeTryEntryOfSuccessors() const {
  if (EndsWithTryBoundary()) {
    HTryBoundary* try_boundary = GetLastInstruction()->AsTryBoundary();
    if (try_boundary->IsEntry()) {
      DCHECK(!IsTryBlock());
      return try_boundary;
    } else {
      DCHECK(IsTryBlock());
      DCHECK(try_catch_information_->GetTryEntry().HasSameExceptionHandlersAs(*try_boundary));
      return nullptr;
    }
  } else if (IsTryBlock()) {
    return &try_catch_information_->GetTryEntry();
  } else {
    return nullptr;
  }
}

bool HBasicBlock::HasThrowingInstructions() const {
  for (HInstructionIterator it(GetInstructions()); !it.Done(); it.Advance()) {
    if (it.Current()->CanThrow()) {
      return true;
    }
  }
  return false;
}

static bool HasOnlyOneInstruction(const HBasicBlock& block) {
  return block.GetPhis().IsEmpty()
      && !block.GetInstructions().IsEmpty()
      && block.GetFirstInstruction() == block.GetLastInstruction();
}

bool HBasicBlock::IsSingleGoto() const {
  return HasOnlyOneInstruction(*this) && GetLastInstruction()->IsGoto();
}

bool HBasicBlock::IsSingleTryBoundary() const {
  return HasOnlyOneInstruction(*this) && GetLastInstruction()->IsTryBoundary();
}

bool HBasicBlock::EndsWithControlFlowInstruction() const {
  return !GetInstructions().IsEmpty() && GetLastInstruction()->IsControlFlow();
}

bool HBasicBlock::EndsWithIf() const {
  return !GetInstructions().IsEmpty() && GetLastInstruction()->IsIf();
}

bool HBasicBlock::EndsWithTryBoundary() const {
  return !GetInstructions().IsEmpty() && GetLastInstruction()->IsTryBoundary();
}

bool HBasicBlock::HasSinglePhi() const {
  return !GetPhis().IsEmpty() && GetFirstPhi()->GetNext() == nullptr;
}

ArrayRef<HBasicBlock* const> HBasicBlock::GetNormalSuccessors() const {
  if (EndsWithTryBoundary()) {
    // The normal-flow successor of HTryBoundary is always stored at index zero.
    DCHECK_EQ(successors_[0], GetLastInstruction()->AsTryBoundary()->GetNormalFlowSuccessor());
    return ArrayRef<HBasicBlock* const>(successors_).SubArray(0u, 1u);
  } else {
    // All successors of blocks not ending with TryBoundary are normal.
    return ArrayRef<HBasicBlock* const>(successors_);
  }
}

ArrayRef<HBasicBlock* const> HBasicBlock::GetExceptionalSuccessors() const {
  if (EndsWithTryBoundary()) {
    return GetLastInstruction()->AsTryBoundary()->GetExceptionHandlers();
  } else {
    // Blocks not ending with TryBoundary do not have exceptional successors.
    return ArrayRef<HBasicBlock* const>();
  }
}

bool HTryBoundary::HasSameExceptionHandlersAs(const HTryBoundary& other) const {
  ArrayRef<HBasicBlock* const> handlers1 = GetExceptionHandlers();
  ArrayRef<HBasicBlock* const> handlers2 = other.GetExceptionHandlers();

  size_t length = handlers1.size();
  if (length != handlers2.size()) {
    return false;
  }

  // Exception handlers need to be stored in the same order.
  for (size_t i = 0; i < length; ++i) {
    if (handlers1[i] != handlers2[i]) {
      return false;
    }
  }
  return true;
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

// Should be called on instructions in a dead block in post order. This method
// assumes `insn` has been removed from all users with the exception of catch
// phis because of missing exceptional edges in the graph. It removes the
// instruction from catch phi uses, together with inputs of other catch phis in
// the catch block at the same index, as these must be dead too.
static void RemoveUsesOfDeadInstruction(HInstruction* insn) {
  DCHECK(!insn->HasEnvironmentUses());
  while (insn->HasNonEnvironmentUses()) {
    HUseListNode<HInstruction*>* use = insn->GetUses().GetFirst();
    size_t use_index = use->GetIndex();
    HBasicBlock* user_block =  use->GetUser()->GetBlock();
    DCHECK(use->GetUser()->IsPhi() && user_block->IsCatchBlock());
    for (HInstructionIterator phi_it(user_block->GetPhis()); !phi_it.Done(); phi_it.Advance()) {
      phi_it.Current()->AsPhi()->RemoveInputAt(use_index);
    }
  }
}

void HBasicBlock::DisconnectAndDelete() {
  // Dominators must be removed after all the blocks they dominate. This way
  // a loop header is removed last, a requirement for correct loop information
  // iteration.
  DCHECK(dominated_blocks_.empty());

  // (1) Remove the block from all loops it is included in.
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

  // (2) Disconnect the block from its predecessors and update their
  //     control-flow instructions.
  for (HBasicBlock* predecessor : predecessors_) {
    HInstruction* last_instruction = predecessor->GetLastInstruction();
    if (last_instruction->IsTryBoundary() && !IsCatchBlock()) {
      // This block is the only normal-flow successor of the TryBoundary which
      // makes `predecessor` dead. Since DCE removes blocks in post order,
      // exception handlers of this TryBoundary were already visited and any
      // remaining handlers therefore must be live. We remove `predecessor` from
      // their list of predecessors.
      DCHECK_EQ(last_instruction->AsTryBoundary()->GetNormalFlowSuccessor(), this);
      while (predecessor->GetSuccessors().size() > 1) {
        HBasicBlock* handler = predecessor->GetSuccessors()[1];
        DCHECK(handler->IsCatchBlock());
        predecessor->RemoveSuccessor(handler);
        handler->RemovePredecessor(predecessor);
      }
    }

    predecessor->RemoveSuccessor(this);
    uint32_t num_pred_successors = predecessor->GetSuccessors().size();
    if (num_pred_successors == 1u) {
      // If we have one successor after removing one, then we must have
      // had an HIf, HPackedSwitch or HTryBoundary, as they have more than one
      // successor. Replace those with a HGoto.
      DCHECK(last_instruction->IsIf() ||
             last_instruction->IsPackedSwitch() ||
             (last_instruction->IsTryBoundary() && IsCatchBlock()));
      predecessor->RemoveInstruction(last_instruction);
      predecessor->AddInstruction(new (graph_->GetArena()) HGoto(last_instruction->GetDexPc()));
    } else if (num_pred_successors == 0u) {
      // The predecessor has no remaining successors and therefore must be dead.
      // We deliberately leave it without a control-flow instruction so that the
      // SSAChecker fails unless it is not removed during the pass too.
      predecessor->RemoveInstruction(last_instruction);
    } else {
      // There are multiple successors left. The removed block might be a successor
      // of a PackedSwitch which will be completely removed (perhaps replaced with
      // a Goto), or we are deleting a catch block from a TryBoundary. In either
      // case, leave `last_instruction` as is for now.
      DCHECK(last_instruction->IsPackedSwitch() ||
             (last_instruction->IsTryBoundary() && IsCatchBlock()));
    }
  }
  predecessors_.clear();

  // (3) Disconnect the block from its successors and update their phis.
  for (HBasicBlock* successor : successors_) {
    // Delete this block from the list of predecessors.
    size_t this_index = successor->GetPredecessorIndexOf(this);
    successor->predecessors_.erase(successor->predecessors_.begin() + this_index);

    // Check that `successor` has other predecessors, otherwise `this` is the
    // dominator of `successor` which violates the order DCHECKed at the top.
    DCHECK(!successor->predecessors_.empty());

    // Remove this block's entries in the successor's phis. Skip exceptional
    // successors because catch phi inputs do not correspond to predecessor
    // blocks but throwing instructions. Their inputs will be updated in step (4).
    if (!successor->IsCatchBlock()) {
      if (successor->predecessors_.size() == 1u) {
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
  }
  successors_.clear();

  // (4) Remove instructions and phis. Instructions should have no remaining uses
  //     except in catch phis. If an instruction is used by a catch phi at `index`,
  //     remove `index`-th input of all phis in the catch block since they are
  //     guaranteed dead. Note that we may miss dead inputs this way but the
  //     graph will always remain consistent.
  for (HBackwardInstructionIterator it(GetInstructions()); !it.Done(); it.Advance()) {
    HInstruction* insn = it.Current();
    RemoveUsesOfDeadInstruction(insn);
    RemoveInstruction(insn);
  }
  for (HInstructionIterator it(GetPhis()); !it.Done(); it.Advance()) {
    HPhi* insn = it.Current()->AsPhi();
    RemoveUsesOfDeadInstruction(insn);
    RemovePhi(insn);
  }

  // Disconnect from the dominator.
  dominator_->RemoveDominatedBlock(this);
  SetDominator(nullptr);

  // Delete from the graph, update reverse post order.
  graph_->DeleteDeadEmptyBlock(this);
  SetGraph(nullptr);
}

void HBasicBlock::MergeWith(HBasicBlock* other) {
  DCHECK_EQ(GetGraph(), other->GetGraph());
  DCHECK(ContainsElement(dominated_blocks_, other));
  DCHECK_EQ(GetSingleSuccessor(), other);
  DCHECK_EQ(other->GetSinglePredecessor(), this);
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
  successors_.clear();
  while (!other->successors_.empty()) {
    HBasicBlock* successor = other->GetSuccessors()[0];
    successor->ReplacePredecessor(other, this);
  }

  // Update the dominator tree.
  RemoveDominatedBlock(other);
  for (HBasicBlock* dominated : other->GetDominatedBlocks()) {
    dominated_blocks_.push_back(dominated);
    dominated->SetDominator(this);
  }
  other->dominated_blocks_.clear();
  other->dominator_ = nullptr;

  // Clear the list of predecessors of `other` in preparation of deleting it.
  other->predecessors_.clear();

  // Delete `other` from the graph. The function updates reverse post order.
  graph_->DeleteDeadEmptyBlock(other);
  other->SetGraph(nullptr);
}

void HBasicBlock::MergeWithInlined(HBasicBlock* other) {
  DCHECK_NE(GetGraph(), other->GetGraph());
  DCHECK(GetDominatedBlocks().empty());
  DCHECK(GetSuccessors().empty());
  DCHECK(!EndsWithControlFlowInstruction());
  DCHECK(other->GetSinglePredecessor()->IsEntryBlock());
  DCHECK(other->GetPhis().IsEmpty());
  DCHECK(!other->IsInLoop());

  // Move instructions from `other` to `this`.
  instructions_.Add(other->GetInstructions());
  other->instructions_.SetBlockOfInstructions(this);

  // Update links to the successors of `other`.
  successors_.clear();
  while (!other->successors_.empty()) {
    HBasicBlock* successor = other->GetSuccessors()[0];
    successor->ReplacePredecessor(other, this);
  }

  // Update the dominator tree.
  for (HBasicBlock* dominated : other->GetDominatedBlocks()) {
    dominated_blocks_.push_back(dominated);
    dominated->SetDominator(this);
  }
  other->dominated_blocks_.clear();
  other->dominator_ = nullptr;
  other->graph_ = nullptr;
}

void HBasicBlock::ReplaceWith(HBasicBlock* other) {
  while (!GetPredecessors().empty()) {
    HBasicBlock* predecessor = GetPredecessors()[0];
    predecessor->ReplaceSuccessor(this, other);
  }
  while (!GetSuccessors().empty()) {
    HBasicBlock* successor = GetSuccessors()[0];
    successor->ReplacePredecessor(this, other);
  }
  for (HBasicBlock* dominated : GetDominatedBlocks()) {
    other->AddDominatedBlock(dominated);
  }
  GetDominator()->ReplaceDominatedBlock(this, other);
  other->SetDominator(GetDominator());
  dominator_ = nullptr;
  graph_ = nullptr;
}

// Create space in `blocks` for adding `number_of_new_blocks` entries
// starting at location `at`. Blocks after `at` are moved accordingly.
static void MakeRoomFor(ArenaVector<HBasicBlock*>* blocks,
                        size_t number_of_new_blocks,
                        size_t after) {
  DCHECK_LT(after, blocks->size());
  size_t old_size = blocks->size();
  size_t new_size = old_size + number_of_new_blocks;
  blocks->resize(new_size);
  std::copy_backward(blocks->begin() + after + 1u, blocks->begin() + old_size, blocks->end());
}

void HGraph::DeleteDeadEmptyBlock(HBasicBlock* block) {
  DCHECK_EQ(block->GetGraph(), this);
  DCHECK(block->GetSuccessors().empty());
  DCHECK(block->GetPredecessors().empty());
  DCHECK(block->GetDominatedBlocks().empty());
  DCHECK(block->GetDominator() == nullptr);
  DCHECK(block->GetInstructions().IsEmpty());
  DCHECK(block->GetPhis().IsEmpty());

  if (block->IsExitBlock()) {
    exit_block_ = nullptr;
  }

  RemoveElement(reverse_post_order_, block);
  blocks_[block->GetBlockId()] = nullptr;
}

HInstruction* HGraph::InlineInto(HGraph* outer_graph, HInvoke* invoke) {
  DCHECK(HasExitBlock()) << "Unimplemented scenario";
  // Update the environments in this graph to have the invoke's environment
  // as parent.
  {
    HReversePostOrderIterator it(*this);
    it.Advance();  // Skip the entry block, we do not need to update the entry's suspend check.
    for (; !it.Done(); it.Advance()) {
      HBasicBlock* block = it.Current();
      for (HInstructionIterator instr_it(block->GetInstructions());
           !instr_it.Done();
           instr_it.Advance()) {
        HInstruction* current = instr_it.Current();
        if (current->NeedsEnvironment()) {
          current->GetEnvironment()->SetAndCopyParentChain(
              outer_graph->GetArena(), invoke->GetEnvironment());
        }
      }
    }
  }
  outer_graph->UpdateMaximumNumberOfOutVRegs(GetMaximumNumberOfOutVRegs());
  if (HasBoundsChecks()) {
    outer_graph->SetHasBoundsChecks(true);
  }

  HInstruction* return_value = nullptr;
  if (GetBlocks().size() == 3) {
    // Simple case of an entry block, a body block, and an exit block.
    // Put the body block's instruction into `invoke`'s block.
    HBasicBlock* body = GetBlocks()[1];
    DCHECK(GetBlocks()[0]->IsEntryBlock());
    DCHECK(GetBlocks()[2]->IsExitBlock());
    DCHECK(!body->IsExitBlock());
    DCHECK(!body->IsInLoop());
    HInstruction* last = body->GetLastInstruction();

    invoke->GetBlock()->instructions_.AddAfter(invoke, body->GetInstructions());
    body->GetInstructions().SetBlockOfInstructions(invoke->GetBlock());

    // Replace the invoke with the return value of the inlined graph.
    if (last->IsReturn()) {
      return_value = last->InputAt(0);
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

    HBasicBlock* first = entry_block_->GetSuccessors()[0];
    DCHECK(!first->IsInLoop());
    at->MergeWithInlined(first);
    exit_block_->ReplaceWith(to);

    // Update all predecessors of the exit block (now the `to` block)
    // to not `HReturn` but `HGoto` instead.
    bool returns_void = to->GetPredecessors()[0]->GetLastInstruction()->IsReturnVoid();
    if (to->GetPredecessors().size() == 1) {
      HBasicBlock* predecessor = to->GetPredecessors()[0];
      HInstruction* last = predecessor->GetLastInstruction();
      if (!returns_void) {
        return_value = last->InputAt(0);
      }
      predecessor->AddInstruction(new (allocator) HGoto(last->GetDexPc()));
      predecessor->RemoveInstruction(last);
    } else {
      if (!returns_void) {
        // There will be multiple returns.
        return_value = new (allocator) HPhi(
            allocator, kNoRegNumber, 0, HPhi::ToPhiType(invoke->GetType()), to->GetDexPc());
        to->AddPhi(return_value->AsPhi());
      }
      for (HBasicBlock* predecessor : to->GetPredecessors()) {
        HInstruction* last = predecessor->GetLastInstruction();
        if (!returns_void) {
          return_value->AsPhi()->AddInput(last->InputAt(0));
        }
        predecessor->AddInstruction(new (allocator) HGoto(last->GetDexPc()));
        predecessor->RemoveInstruction(last);
      }
    }

    // Update the meta information surrounding blocks:
    // (1) the graph they are now in,
    // (2) the reverse post order of that graph,
    // (3) their potential loop information, inner and outer,
    // (4) try block membership.
    // Note that we do not need to update catch phi inputs because they
    // correspond to the register file of the outer method which the inlinee
    // cannot modify.

    // We don't add the entry block, the exit block, and the first block, which
    // has been merged with `at`.
    static constexpr int kNumberOfSkippedBlocksInCallee = 3;

    // We add the `to` block.
    static constexpr int kNumberOfNewBlocksInCaller = 1;
    size_t blocks_added = (reverse_post_order_.size() - kNumberOfSkippedBlocksInCallee)
        + kNumberOfNewBlocksInCaller;

    // Find the location of `at` in the outer graph's reverse post order. The new
    // blocks will be added after it.
    size_t index_of_at = IndexOfElement(outer_graph->reverse_post_order_, at);
    MakeRoomFor(&outer_graph->reverse_post_order_, blocks_added, index_of_at);

    HLoopInformation* loop_info = at->GetLoopInformation();
    // Copy TryCatchInformation if `at` is a try block, not if it is a catch block.
    TryCatchInformation* try_catch_info = at->IsTryBlock() ? at->GetTryCatchInformation() : nullptr;

    // Do a reverse post order of the blocks in the callee and do (1), (2), (3)
    // and (4) to the blocks that apply.
    for (HReversePostOrderIterator it(*this); !it.Done(); it.Advance()) {
      HBasicBlock* current = it.Current();
      if (current != exit_block_ && current != entry_block_ && current != first) {
        DCHECK(current->GetTryCatchInformation() == nullptr);
        DCHECK(current->GetGraph() == this);
        current->SetGraph(outer_graph);
        outer_graph->AddBlock(current);
        outer_graph->reverse_post_order_[++index_of_at] = current;
        if (!current->IsInLoop()) {
          current->SetLoopInformation(loop_info);
        } else if (current->IsLoopHeader()) {
          // Clear the information of which blocks are contained in that loop. Since the
          // information is stored as a bit vector based on block ids, we have to update
          // it, as those block ids were specific to the callee graph and we are now adding
          // these blocks to the caller graph.
          current->GetLoopInformation()->ClearAllBlocks();
        }
        if (current->IsInLoop()) {
          for (HLoopInformationOutwardIterator loop_it(*current);
               !loop_it.Done();
               loop_it.Advance()) {
            loop_it.Current()->Add(current);
          }
        }
        current->SetTryCatchInformation(try_catch_info);
      }
    }

    // Do (1), (2), (3) and (4) to `to`.
    to->SetGraph(outer_graph);
    outer_graph->AddBlock(to);
    outer_graph->reverse_post_order_[++index_of_at] = to;
    if (loop_info != nullptr) {
      if (!to->IsInLoop()) {
        to->SetLoopInformation(loop_info);
      }
      for (HLoopInformationOutwardIterator loop_it(*at); !loop_it.Done(); loop_it.Advance()) {
        loop_it.Current()->Add(to);
      }
      if (loop_info->IsBackEdge(*at)) {
        // Only `to` can become a back edge, as the inlined blocks
        // are predecessors of `to`.
        loop_info->ReplaceBackEdge(at, to);
      }
    }
    to->SetTryCatchInformation(try_catch_info);
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
    HInstruction* replacement = nullptr;
    if (current->IsNullConstant()) {
      replacement = outer_graph->GetNullConstant(current->GetDexPc());
    } else if (current->IsIntConstant()) {
      replacement = outer_graph->GetIntConstant(
          current->AsIntConstant()->GetValue(), current->GetDexPc());
    } else if (current->IsLongConstant()) {
      replacement = outer_graph->GetLongConstant(
          current->AsLongConstant()->GetValue(), current->GetDexPc());
    } else if (current->IsFloatConstant()) {
      replacement = outer_graph->GetFloatConstant(
          current->AsFloatConstant()->GetValue(), current->GetDexPc());
    } else if (current->IsDoubleConstant()) {
      replacement = outer_graph->GetDoubleConstant(
          current->AsDoubleConstant()->GetValue(), current->GetDexPc());
    } else if (current->IsParameterValue()) {
      if (kIsDebugBuild
          && invoke->IsInvokeStaticOrDirect()
          && invoke->AsInvokeStaticOrDirect()->IsStaticWithExplicitClinitCheck()) {
        // Ensure we do not use the last input of `invoke`, as it
        // contains a clinit check which is not an actual argument.
        size_t last_input_index = invoke->InputCount() - 1;
        DCHECK(parameter_index != last_input_index);
      }
      replacement = invoke->InputAt(parameter_index++);
    } else if (current->IsCurrentMethod()) {
      replacement = outer_graph->GetCurrentMethod();
    } else {
      DCHECK(current->IsGoto() || current->IsSuspendCheck());
      entry_block_->RemoveInstruction(current);
    }
    if (replacement != nullptr) {
      current->ReplaceWith(replacement);
      // If the current is the return value then we need to update the latter.
      if (current == return_value) {
        DCHECK_EQ(entry_block_, return_value->GetBlock());
        return_value = replacement;
      }
    }
  }

  if (return_value != nullptr) {
    invoke->ReplaceWith(return_value);
  }

  // Finally remove the invoke from the caller.
  invoke->GetBlock()->RemoveInstruction(invoke);

  return return_value;
}

/*
 * Loop will be transformed to:
 *       old_pre_header
 *             |
 *          if_block
 *           /    \
 *  true_block   false_block
 *           \    /
 *       new_pre_header
 *             |
 *           header
 */
void HGraph::TransformLoopHeaderForBCE(HBasicBlock* header) {
  DCHECK(header->IsLoopHeader());
  HBasicBlock* old_pre_header = header->GetDominator();

  // Need extra block to avoid critical edge.
  HBasicBlock* if_block = new (arena_) HBasicBlock(this, header->GetDexPc());
  HBasicBlock* true_block = new (arena_) HBasicBlock(this, header->GetDexPc());
  HBasicBlock* false_block = new (arena_) HBasicBlock(this, header->GetDexPc());
  HBasicBlock* new_pre_header = new (arena_) HBasicBlock(this, header->GetDexPc());
  AddBlock(if_block);
  AddBlock(true_block);
  AddBlock(false_block);
  AddBlock(new_pre_header);

  header->ReplacePredecessor(old_pre_header, new_pre_header);
  old_pre_header->successors_.clear();
  old_pre_header->dominated_blocks_.clear();

  old_pre_header->AddSuccessor(if_block);
  if_block->AddSuccessor(true_block);  // True successor
  if_block->AddSuccessor(false_block);  // False successor
  true_block->AddSuccessor(new_pre_header);
  false_block->AddSuccessor(new_pre_header);

  old_pre_header->dominated_blocks_.push_back(if_block);
  if_block->SetDominator(old_pre_header);
  if_block->dominated_blocks_.push_back(true_block);
  true_block->SetDominator(if_block);
  if_block->dominated_blocks_.push_back(false_block);
  false_block->SetDominator(if_block);
  if_block->dominated_blocks_.push_back(new_pre_header);
  new_pre_header->SetDominator(if_block);
  new_pre_header->dominated_blocks_.push_back(header);
  header->SetDominator(new_pre_header);

  // Fix reverse post order.
  size_t index_of_header = IndexOfElement(reverse_post_order_, header);
  MakeRoomFor(&reverse_post_order_, 4, index_of_header - 1);
  reverse_post_order_[index_of_header++] = if_block;
  reverse_post_order_[index_of_header++] = true_block;
  reverse_post_order_[index_of_header++] = false_block;
  reverse_post_order_[index_of_header++] = new_pre_header;

  // Fix loop information.
  HLoopInformation* loop_info = old_pre_header->GetLoopInformation();
  if (loop_info != nullptr) {
    if_block->SetLoopInformation(loop_info);
    true_block->SetLoopInformation(loop_info);
    false_block->SetLoopInformation(loop_info);
    new_pre_header->SetLoopInformation(loop_info);
    // Add blocks to all enveloping loops.
    for (HLoopInformationOutwardIterator loop_it(*old_pre_header);
         !loop_it.Done();
         loop_it.Advance()) {
      loop_it.Current()->Add(if_block);
      loop_it.Current()->Add(true_block);
      loop_it.Current()->Add(false_block);
      loop_it.Current()->Add(new_pre_header);
    }
  }

  // Fix try/catch information.
  TryCatchInformation* try_catch_info = old_pre_header->IsTryBlock()
      ? old_pre_header->GetTryCatchInformation()
      : nullptr;
  if_block->SetTryCatchInformation(try_catch_info);
  true_block->SetTryCatchInformation(try_catch_info);
  false_block->SetTryCatchInformation(try_catch_info);
  new_pre_header->SetTryCatchInformation(try_catch_info);
}

static void CheckAgainstUpperBound(ReferenceTypeInfo rti, ReferenceTypeInfo upper_bound_rti)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  if (rti.IsValid()) {
    DCHECK(upper_bound_rti.IsSupertypeOf(rti))
        << " upper_bound_rti: " << upper_bound_rti
        << " rti: " << rti;
    DCHECK(!upper_bound_rti.GetTypeHandle()->CannotBeAssignedFromOtherTypes() || rti.IsExact());
  }
}

void HInstruction::SetReferenceTypeInfo(ReferenceTypeInfo rti) {
  if (kIsDebugBuild) {
    DCHECK_EQ(GetType(), Primitive::kPrimNot);
    ScopedObjectAccess soa(Thread::Current());
    DCHECK(rti.IsValid()) << "Invalid RTI for " << DebugName();
    if (IsBoundType()) {
      // Having the test here spares us from making the method virtual just for
      // the sake of a DCHECK.
      CheckAgainstUpperBound(rti, AsBoundType()->GetUpperBound());
    }
  }
  reference_type_info_ = rti;
}

void HBoundType::SetUpperBound(const ReferenceTypeInfo& upper_bound, bool can_be_null) {
  if (kIsDebugBuild) {
    ScopedObjectAccess soa(Thread::Current());
    DCHECK(upper_bound.IsValid());
    DCHECK(!upper_bound_.IsValid()) << "Upper bound should only be set once.";
    CheckAgainstUpperBound(GetReferenceTypeInfo(), upper_bound);
  }
  upper_bound_ = upper_bound;
  upper_can_be_null_ = can_be_null;
}

ReferenceTypeInfo::ReferenceTypeInfo() : type_handle_(TypeHandle()), is_exact_(false) {}

ReferenceTypeInfo::ReferenceTypeInfo(TypeHandle type_handle, bool is_exact)
    : type_handle_(type_handle), is_exact_(is_exact) {
  if (kIsDebugBuild) {
    ScopedObjectAccess soa(Thread::Current());
    DCHECK(IsValidHandle(type_handle));
  }
}

std::ostream& operator<<(std::ostream& os, const ReferenceTypeInfo& rhs) {
  ScopedObjectAccess soa(Thread::Current());
  os << "["
     << " is_valid=" << rhs.IsValid()
     << " type=" << (!rhs.IsValid() ? "?" : PrettyClass(rhs.GetTypeHandle().Get()))
     << " is_exact=" << rhs.IsExact()
     << " ]";
  return os;
}

bool HInstruction::HasAnyEnvironmentUseBefore(HInstruction* other) {
  // For now, assume that instructions in different blocks may use the
  // environment.
  // TODO: Use the control flow to decide if this is true.
  if (GetBlock() != other->GetBlock()) {
    return true;
  }

  // We know that we are in the same block. Walk from 'this' to 'other',
  // checking to see if there is any instruction with an environment.
  HInstruction* current = this;
  for (; current != other && current != nullptr; current = current->GetNext()) {
    // This is a conservative check, as the instruction result may not be in
    // the referenced environment.
    if (current->HasEnvironment()) {
      return true;
    }
  }

  // We should have been called with 'this' before 'other' in the block.
  // Just confirm this.
  DCHECK(current != nullptr);
  return false;
}

void HInvoke::SetIntrinsic(Intrinsics intrinsic,
                           IntrinsicNeedsEnvironmentOrCache needs_env_or_cache,
                           IntrinsicSideEffects side_effects,
                           IntrinsicExceptions exceptions) {
  intrinsic_ = intrinsic;
  IntrinsicOptimizations opt(this);

  // Adjust method's side effects from intrinsic table.
  switch (side_effects) {
    case kNoSideEffects: SetSideEffects(SideEffects::None()); break;
    case kReadSideEffects: SetSideEffects(SideEffects::AllReads()); break;
    case kWriteSideEffects: SetSideEffects(SideEffects::AllWrites()); break;
    case kAllSideEffects: SetSideEffects(SideEffects::AllExceptGCDependency()); break;
  }

  if (needs_env_or_cache == kNoEnvironmentOrCache) {
    opt.SetDoesNotNeedDexCache();
    opt.SetDoesNotNeedEnvironment();
  } else {
    // If we need an environment, that means there will be a call, which can trigger GC.
    SetSideEffects(GetSideEffects().Union(SideEffects::CanTriggerGC()));
  }
  // Adjust method's exception status from intrinsic table.
  SetCanThrow(exceptions == kCanThrow);
}

bool HNewInstance::IsStringAlloc() const {
  ScopedObjectAccess soa(Thread::Current());
  return GetReferenceTypeInfo().IsStringClass();
}

bool HInvoke::NeedsEnvironment() const {
  if (!IsIntrinsic()) {
    return true;
  }
  IntrinsicOptimizations opt(*this);
  return !opt.GetDoesNotNeedEnvironment();
}

bool HInvokeStaticOrDirect::NeedsDexCacheOfDeclaringClass() const {
  if (GetMethodLoadKind() != MethodLoadKind::kDexCacheViaMethod) {
    return false;
  }
  if (!IsIntrinsic()) {
    return true;
  }
  IntrinsicOptimizations opt(*this);
  return !opt.GetDoesNotNeedDexCache();
}

void HInvokeStaticOrDirect::InsertInputAt(size_t index, HInstruction* input) {
  inputs_.insert(inputs_.begin() + index, HUserRecord<HInstruction*>(input));
  input->AddUseAt(this, index);
  // Update indexes in use nodes of inputs that have been pushed further back by the insert().
  for (size_t i = index + 1u, size = inputs_.size(); i != size; ++i) {
    DCHECK_EQ(InputRecordAt(i).GetUseNode()->GetIndex(), i - 1u);
    InputRecordAt(i).GetUseNode()->SetIndex(i);
  }
}

void HInvokeStaticOrDirect::RemoveInputAt(size_t index) {
  RemoveAsUserOfInput(index);
  inputs_.erase(inputs_.begin() + index);
  // Update indexes in use nodes of inputs that have been pulled forward by the erase().
  for (size_t i = index, e = InputCount(); i < e; ++i) {
    DCHECK_EQ(InputRecordAt(i).GetUseNode()->GetIndex(), i + 1u);
    InputRecordAt(i).GetUseNode()->SetIndex(i);
  }
}

std::ostream& operator<<(std::ostream& os, HInvokeStaticOrDirect::MethodLoadKind rhs) {
  switch (rhs) {
    case HInvokeStaticOrDirect::MethodLoadKind::kStringInit:
      return os << "string_init";
    case HInvokeStaticOrDirect::MethodLoadKind::kRecursive:
      return os << "recursive";
    case HInvokeStaticOrDirect::MethodLoadKind::kDirectAddress:
      return os << "direct";
    case HInvokeStaticOrDirect::MethodLoadKind::kDirectAddressWithFixup:
      return os << "direct_fixup";
    case HInvokeStaticOrDirect::MethodLoadKind::kDexCachePcRelative:
      return os << "dex_cache_pc_relative";
    case HInvokeStaticOrDirect::MethodLoadKind::kDexCacheViaMethod:
      return os << "dex_cache_via_method";
    default:
      LOG(FATAL) << "Unknown MethodLoadKind: " << static_cast<int>(rhs);
      UNREACHABLE();
  }
}

std::ostream& operator<<(std::ostream& os, HInvokeStaticOrDirect::ClinitCheckRequirement rhs) {
  switch (rhs) {
    case HInvokeStaticOrDirect::ClinitCheckRequirement::kExplicit:
      return os << "explicit";
    case HInvokeStaticOrDirect::ClinitCheckRequirement::kImplicit:
      return os << "implicit";
    case HInvokeStaticOrDirect::ClinitCheckRequirement::kNone:
      return os << "none";
    default:
      LOG(FATAL) << "Unknown ClinitCheckRequirement: " << static_cast<int>(rhs);
      UNREACHABLE();
  }
}

void HInstruction::RemoveEnvironmentUsers() {
  for (HUseIterator<HEnvironment*> use_it(GetEnvUses()); !use_it.Done(); use_it.Advance()) {
    HUseListNode<HEnvironment*>* user_node = use_it.Current();
    HEnvironment* user = user_node->GetUser();
    user->SetRawEnvAt(user_node->GetIndex(), nullptr);
  }
  env_uses_.Clear();
}

// Returns an instruction with the opposite boolean value from 'cond'.
HInstruction* HGraph::InsertOppositeCondition(HInstruction* cond, HInstruction* cursor) {
  ArenaAllocator* allocator = GetArena();

  if (cond->IsCondition() &&
      !Primitive::IsFloatingPointType(cond->InputAt(0)->GetType())) {
    // Can't reverse floating point conditions.  We have to use HBooleanNot in that case.
    HInstruction* lhs = cond->InputAt(0);
    HInstruction* rhs = cond->InputAt(1);
    HInstruction* replacement = nullptr;
    switch (cond->AsCondition()->GetOppositeCondition()) {  // get *opposite*
      case kCondEQ: replacement = new (allocator) HEqual(lhs, rhs); break;
      case kCondNE: replacement = new (allocator) HNotEqual(lhs, rhs); break;
      case kCondLT: replacement = new (allocator) HLessThan(lhs, rhs); break;
      case kCondLE: replacement = new (allocator) HLessThanOrEqual(lhs, rhs); break;
      case kCondGT: replacement = new (allocator) HGreaterThan(lhs, rhs); break;
      case kCondGE: replacement = new (allocator) HGreaterThanOrEqual(lhs, rhs); break;
      case kCondB:  replacement = new (allocator) HBelow(lhs, rhs); break;
      case kCondBE: replacement = new (allocator) HBelowOrEqual(lhs, rhs); break;
      case kCondA:  replacement = new (allocator) HAbove(lhs, rhs); break;
      case kCondAE: replacement = new (allocator) HAboveOrEqual(lhs, rhs); break;
      default:
        LOG(FATAL) << "Unexpected condition";
        UNREACHABLE();
    }
    cursor->GetBlock()->InsertInstructionBefore(replacement, cursor);
    return replacement;
  } else if (cond->IsIntConstant()) {
    HIntConstant* int_const = cond->AsIntConstant();
    if (int_const->IsZero()) {
      return GetIntConstant(1);
    } else {
      DCHECK(int_const->IsOne());
      return GetIntConstant(0);
    }
  } else {
    HInstruction* replacement = new (allocator) HBooleanNot(cond);
    cursor->GetBlock()->InsertInstructionBefore(replacement, cursor);
    return replacement;
  }
}

std::ostream& operator<<(std::ostream& os, const MoveOperands& rhs) {
  os << "["
     << " source=" << rhs.GetSource()
     << " destination=" << rhs.GetDestination()
     << " type=" << rhs.GetType()
     << " instruction=";
  if (rhs.GetInstruction() != nullptr) {
    os << rhs.GetInstruction()->DebugName() << ' ' << rhs.GetInstruction()->GetId();
  } else {
    os << "null";
  }
  os << " ]";
  return os;
}

std::ostream& operator<<(std::ostream& os, TypeCheckKind rhs) {
  switch (rhs) {
    case TypeCheckKind::kUnresolvedCheck:
      return os << "unresolved_check";
    case TypeCheckKind::kExactCheck:
      return os << "exact_check";
    case TypeCheckKind::kClassHierarchyCheck:
      return os << "class_hierarchy_check";
    case TypeCheckKind::kAbstractClassCheck:
      return os << "abstract_class_check";
    case TypeCheckKind::kInterfaceCheck:
      return os << "interface_check";
    case TypeCheckKind::kArrayObjectCheck:
      return os << "array_object_check";
    case TypeCheckKind::kArrayCheck:
      return os << "array_check";
    default:
      LOG(FATAL) << "Unknown TypeCheckKind: " << static_cast<int>(rhs);
      UNREACHABLE();
  }
}

}  // namespace art
