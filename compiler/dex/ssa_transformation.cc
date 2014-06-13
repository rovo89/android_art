/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "compiler_internals.h"
#include "dataflow_iterator-inl.h"
#include "utils/scoped_arena_containers.h"

#define NOTVISITED (-1)

namespace art {

void MIRGraph::ClearAllVisitedFlags() {
  AllNodesIterator iter(this);
  for (BasicBlock* bb = iter.Next(); bb != NULL; bb = iter.Next()) {
    bb->visited = false;
  }
}

BasicBlock* MIRGraph::NeedsVisit(BasicBlock* bb) {
  if (bb != NULL) {
    if (bb->visited || bb->hidden) {
      bb = NULL;
    }
  }
  return bb;
}

BasicBlock* MIRGraph::NextUnvisitedSuccessor(BasicBlock* bb) {
  BasicBlock* res = NeedsVisit(GetBasicBlock(bb->fall_through));
  if (res == NULL) {
    res = NeedsVisit(GetBasicBlock(bb->taken));
    if (res == NULL) {
      if (bb->successor_block_list_type != kNotUsed) {
        GrowableArray<SuccessorBlockInfo*>::Iterator iterator(bb->successor_blocks);
        while (true) {
          SuccessorBlockInfo *sbi = iterator.Next();
          if (sbi == NULL) {
            break;
          }
          res = NeedsVisit(GetBasicBlock(sbi->block));
          if (res != NULL) {
            break;
          }
        }
      }
    }
  }
  return res;
}

void MIRGraph::MarkPreOrder(BasicBlock* block) {
  block->visited = true;
  /* Enqueue the pre_order block id */
  if (block->id != NullBasicBlockId) {
    dfs_order_->Insert(block->id);
  }
}

void MIRGraph::RecordDFSOrders(BasicBlock* block) {
  DCHECK(temp_scoped_alloc_.get() != nullptr);
  ScopedArenaVector<BasicBlock*> succ(temp_scoped_alloc_->Adapter());
  MarkPreOrder(block);
  succ.push_back(block);
  while (!succ.empty()) {
    BasicBlock* curr = succ.back();
    BasicBlock* next_successor = NextUnvisitedSuccessor(curr);
    if (next_successor != NULL) {
      MarkPreOrder(next_successor);
      succ.push_back(next_successor);
      continue;
    }
    curr->dfs_id = dfs_post_order_->Size();
    if (curr->id != NullBasicBlockId) {
      dfs_post_order_->Insert(curr->id);
    }
    succ.pop_back();
  }
}

/* Sort the blocks by the Depth-First-Search */
void MIRGraph::ComputeDFSOrders() {
  /* Initialize or reset the DFS pre_order list */
  if (dfs_order_ == NULL) {
    dfs_order_ = new (arena_) GrowableArray<BasicBlockId>(arena_, GetNumBlocks(),
                                                          kGrowableArrayDfsOrder);
  } else {
    /* Just reset the used length on the counter */
    dfs_order_->Reset();
  }

  /* Initialize or reset the DFS post_order list */
  if (dfs_post_order_ == NULL) {
    dfs_post_order_ = new (arena_) GrowableArray<BasicBlockId>(arena_, GetNumBlocks(),
                                                               kGrowableArrayDfsPostOrder);
  } else {
    /* Just reset the used length on the counter */
    dfs_post_order_->Reset();
  }

  // Reset visited flags from all nodes
  ClearAllVisitedFlags();

  // Record dfs orders
  RecordDFSOrders(GetEntryBlock());

  num_reachable_blocks_ = dfs_order_->Size();

  if (num_reachable_blocks_ != num_blocks_) {
    // Hide all unreachable blocks.
    AllNodesIterator iter(this);
    for (BasicBlock* bb = iter.Next(); bb != NULL; bb = iter.Next()) {
      if (!bb->visited) {
        bb->Hide(cu_);
      }
    }
  }
}

/*
 * Mark block bit on the per-Dalvik register vector to denote that Dalvik
 * register idx is defined in BasicBlock bb.
 */
bool MIRGraph::FillDefBlockMatrix(BasicBlock* bb) {
  if (bb->data_flow_info == NULL) {
    return false;
  }

  for (uint32_t idx : bb->data_flow_info->def_v->Indexes()) {
    /* Block bb defines register idx */
    def_block_matrix_[idx]->SetBit(bb->id);
  }
  return true;
}

void MIRGraph::ComputeDefBlockMatrix() {
  int num_registers = cu_->num_dalvik_registers;
  /* Allocate num_dalvik_registers bit vector pointers */
  def_block_matrix_ = static_cast<ArenaBitVector**>
      (arena_->Alloc(sizeof(ArenaBitVector *) * num_registers,
                     kArenaAllocDFInfo));
  int i;

  /* Initialize num_register vectors with num_blocks bits each */
  for (i = 0; i < num_registers; i++) {
    def_block_matrix_[i] =
        new (arena_) ArenaBitVector(arena_, GetNumBlocks(), false, kBitMapBMatrix);
  }
  AllNodesIterator iter(this);
  for (BasicBlock* bb = iter.Next(); bb != NULL; bb = iter.Next()) {
    FindLocalLiveIn(bb);
  }
  AllNodesIterator iter2(this);
  for (BasicBlock* bb = iter2.Next(); bb != NULL; bb = iter2.Next()) {
    FillDefBlockMatrix(bb);
  }

  /*
   * Also set the incoming parameters as defs in the entry block.
   * Only need to handle the parameters for the outer method.
   */
  int num_regs = cu_->num_dalvik_registers;
  int in_reg = num_regs - cu_->num_ins;
  for (; in_reg < num_regs; in_reg++) {
    def_block_matrix_[in_reg]->SetBit(GetEntryBlock()->id);
  }
}

void MIRGraph::ComputeDomPostOrderTraversal(BasicBlock* bb) {
  if (dom_post_order_traversal_ == NULL || max_num_reachable_blocks_ < num_reachable_blocks_) {
    // First time or too small - create the array.
    dom_post_order_traversal_ =
        new (arena_) GrowableArray<BasicBlockId>(arena_, num_reachable_blocks_,
                                        kGrowableArrayDomPostOrderTraversal);
  } else {
    dom_post_order_traversal_->Reset();
  }
  ClearAllVisitedFlags();
  DCHECK(temp_scoped_alloc_.get() != nullptr);
  ScopedArenaVector<std::pair<BasicBlock*, ArenaBitVector::IndexIterator>> work_stack(
      temp_scoped_alloc_->Adapter());
  bb->visited = true;
  work_stack.push_back(std::make_pair(bb, bb->i_dominated->Indexes().begin()));
  while (!work_stack.empty()) {
    std::pair<BasicBlock*, ArenaBitVector::IndexIterator>* curr = &work_stack.back();
    BasicBlock* curr_bb = curr->first;
    ArenaBitVector::IndexIterator* curr_idom_iter = &curr->second;
    while (!curr_idom_iter->Done() && (NeedsVisit(GetBasicBlock(**curr_idom_iter)) == nullptr)) {
      ++*curr_idom_iter;
    }
    // NOTE: work_stack.push_back()/pop_back() invalidate curr and curr_idom_iter.
    if (!curr_idom_iter->Done()) {
      BasicBlock* new_bb = GetBasicBlock(**curr_idom_iter);
      ++*curr_idom_iter;
      new_bb->visited = true;
      work_stack.push_back(std::make_pair(new_bb, new_bb->i_dominated->Indexes().begin()));
    } else {
      // no successor/next
      if (curr_bb->id != NullBasicBlockId) {
        dom_post_order_traversal_->Insert(curr_bb->id);
      }
      work_stack.pop_back();

      /* hacky loop detection */
      if ((curr_bb->taken != NullBasicBlockId) && curr_bb->dominators->IsBitSet(curr_bb->taken)) {
        curr_bb->nesting_depth++;
        attributes_ |= METHOD_HAS_LOOP;
      }
    }
  }
}

void MIRGraph::CheckForDominanceFrontier(BasicBlock* dom_bb,
                                         const BasicBlock* succ_bb) {
  /*
   * TODO - evaluate whether phi will ever need to be inserted into exit
   * blocks.
   */
  if (succ_bb->i_dom != dom_bb->id &&
    succ_bb->block_type == kDalvikByteCode &&
    succ_bb->hidden == false) {
    dom_bb->dom_frontier->SetBit(succ_bb->id);
  }
}

/* Worker function to compute the dominance frontier */
bool MIRGraph::ComputeDominanceFrontier(BasicBlock* bb) {
  /* Calculate DF_local */
  if (bb->taken != NullBasicBlockId) {
    CheckForDominanceFrontier(bb, GetBasicBlock(bb->taken));
  }
  if (bb->fall_through != NullBasicBlockId) {
    CheckForDominanceFrontier(bb, GetBasicBlock(bb->fall_through));
  }
  if (bb->successor_block_list_type != kNotUsed) {
    GrowableArray<SuccessorBlockInfo*>::Iterator iterator(bb->successor_blocks);
      while (true) {
        SuccessorBlockInfo *successor_block_info = iterator.Next();
        if (successor_block_info == NULL) {
          break;
        }
        BasicBlock* succ_bb = GetBasicBlock(successor_block_info->block);
        CheckForDominanceFrontier(bb, succ_bb);
      }
  }

  /* Calculate DF_up */
  for (uint32_t dominated_idx : bb->i_dominated->Indexes()) {
    BasicBlock* dominated_bb = GetBasicBlock(dominated_idx);
    for (uint32_t df_up_block_idx : dominated_bb->dom_frontier->Indexes()) {
      BasicBlock* df_up_block = GetBasicBlock(df_up_block_idx);
      CheckForDominanceFrontier(bb, df_up_block);
    }
  }

  return true;
}

/* Worker function for initializing domination-related data structures */
void MIRGraph::InitializeDominationInfo(BasicBlock* bb) {
  int num_total_blocks = GetBasicBlockListCount();

  if (bb->dominators == NULL) {
    bb->dominators = new (arena_) ArenaBitVector(arena_, num_total_blocks,
                                                 false /* expandable */, kBitMapDominators);
    bb->i_dominated = new (arena_) ArenaBitVector(arena_, num_total_blocks,
                                                  false /* expandable */, kBitMapIDominated);
    bb->dom_frontier = new (arena_) ArenaBitVector(arena_, num_total_blocks,
                                                   false /* expandable */, kBitMapDomFrontier);
  } else {
    bb->dominators->ClearAllBits();
    bb->i_dominated->ClearAllBits();
    bb->dom_frontier->ClearAllBits();
  }
  /* Set all bits in the dominator vector */
  bb->dominators->SetInitialBits(num_total_blocks);

  return;
}

/*
 * Walk through the ordered i_dom list until we reach common parent.
 * Given the ordering of i_dom_list, this common parent represents the
 * last element of the intersection of block1 and block2 dominators.
  */
int MIRGraph::FindCommonParent(int block1, int block2) {
  while (block1 != block2) {
    while (block1 < block2) {
      block1 = i_dom_list_[block1];
      DCHECK_NE(block1, NOTVISITED);
    }
    while (block2 < block1) {
      block2 = i_dom_list_[block2];
      DCHECK_NE(block2, NOTVISITED);
    }
  }
  return block1;
}

/* Worker function to compute each block's immediate dominator */
bool MIRGraph::ComputeblockIDom(BasicBlock* bb) {
  /* Special-case entry block */
  if ((bb->id == NullBasicBlockId) || (bb == GetEntryBlock())) {
    return false;
  }

  /* Iterate through the predecessors */
  GrowableArray<BasicBlockId>::Iterator iter(bb->predecessors);

  /* Find the first processed predecessor */
  int idom = -1;
  while (true) {
    BasicBlock* pred_bb = GetBasicBlock(iter.Next());
    CHECK(pred_bb != NULL);
    if (i_dom_list_[pred_bb->dfs_id] != NOTVISITED) {
      idom = pred_bb->dfs_id;
      break;
    }
  }

  /* Scan the rest of the predecessors */
  while (true) {
      BasicBlock* pred_bb = GetBasicBlock(iter.Next());
      if (!pred_bb) {
        break;
      }
      if (i_dom_list_[pred_bb->dfs_id] == NOTVISITED) {
        continue;
      } else {
        idom = FindCommonParent(pred_bb->dfs_id, idom);
      }
  }

  DCHECK_NE(idom, NOTVISITED);

  /* Did something change? */
  if (i_dom_list_[bb->dfs_id] != idom) {
    i_dom_list_[bb->dfs_id] = idom;
    return true;
  }
  return false;
}

/* Worker function to compute each block's domintors */
bool MIRGraph::ComputeBlockDominators(BasicBlock* bb) {
  if (bb == GetEntryBlock()) {
    bb->dominators->ClearAllBits();
  } else {
    bb->dominators->Copy(GetBasicBlock(bb->i_dom)->dominators);
  }
  bb->dominators->SetBit(bb->id);
  return false;
}

bool MIRGraph::SetDominators(BasicBlock* bb) {
  if (bb != GetEntryBlock()) {
    int idom_dfs_idx = i_dom_list_[bb->dfs_id];
    DCHECK_NE(idom_dfs_idx, NOTVISITED);
    int i_dom_idx = dfs_post_order_->Get(idom_dfs_idx);
    BasicBlock* i_dom = GetBasicBlock(i_dom_idx);
    bb->i_dom = i_dom->id;
    /* Add bb to the i_dominated set of the immediate dominator block */
    i_dom->i_dominated->SetBit(bb->id);
  }
  return false;
}

/* Compute dominators, immediate dominator, and dominance fronter */
void MIRGraph::ComputeDominators() {
  int num_reachable_blocks = num_reachable_blocks_;

  /* Initialize domination-related data structures */
  PreOrderDfsIterator iter(this);
  for (BasicBlock* bb = iter.Next(); bb != NULL; bb = iter.Next()) {
    InitializeDominationInfo(bb);
  }

  /* Initialize & Clear i_dom_list */
  if (max_num_reachable_blocks_ < num_reachable_blocks_) {
    i_dom_list_ = static_cast<int*>(arena_->Alloc(sizeof(int) * num_reachable_blocks,
                                                  kArenaAllocDFInfo));
  }
  for (int i = 0; i < num_reachable_blocks; i++) {
    i_dom_list_[i] = NOTVISITED;
  }

  /* For post-order, last block is entry block.  Set its i_dom to istelf */
  DCHECK_EQ(GetEntryBlock()->dfs_id, num_reachable_blocks-1);
  i_dom_list_[GetEntryBlock()->dfs_id] = GetEntryBlock()->dfs_id;

  /* Compute the immediate dominators */
  RepeatingReversePostOrderDfsIterator iter2(this);
  bool change = false;
  for (BasicBlock* bb = iter2.Next(false); bb != NULL; bb = iter2.Next(change)) {
    change = ComputeblockIDom(bb);
  }

  /* Set the dominator for the root node */
  GetEntryBlock()->dominators->ClearAllBits();
  GetEntryBlock()->dominators->SetBit(GetEntryBlock()->id);

  GetEntryBlock()->i_dom = 0;

  PreOrderDfsIterator iter3(this);
  for (BasicBlock* bb = iter3.Next(); bb != NULL; bb = iter3.Next()) {
    SetDominators(bb);
  }

  ReversePostOrderDfsIterator iter4(this);
  for (BasicBlock* bb = iter4.Next(); bb != NULL; bb = iter4.Next()) {
    ComputeBlockDominators(bb);
  }

  // Compute the dominance frontier for each block.
  ComputeDomPostOrderTraversal(GetEntryBlock());
  PostOrderDOMIterator iter5(this);
  for (BasicBlock* bb = iter5.Next(); bb != NULL; bb = iter5.Next()) {
    ComputeDominanceFrontier(bb);
  }
}

/*
 * Perform dest U= src1 ^ ~src2
 * This is probably not general enough to be placed in BitVector.[ch].
 */
void MIRGraph::ComputeSuccLineIn(ArenaBitVector* dest, const ArenaBitVector* src1,
                                 const ArenaBitVector* src2) {
  if (dest->GetStorageSize() != src1->GetStorageSize() ||
      dest->GetStorageSize() != src2->GetStorageSize() ||
      dest->IsExpandable() != src1->IsExpandable() ||
      dest->IsExpandable() != src2->IsExpandable()) {
    LOG(FATAL) << "Incompatible set properties";
  }

  unsigned int idx;
  for (idx = 0; idx < dest->GetStorageSize(); idx++) {
    dest->GetRawStorage()[idx] |= src1->GetRawStorageWord(idx) & ~(src2->GetRawStorageWord(idx));
  }
}

/*
 * Iterate through all successor blocks and propagate up the live-in sets.
 * The calculated result is used for phi-node pruning - where we only need to
 * insert a phi node if the variable is live-in to the block.
 */
bool MIRGraph::ComputeBlockLiveIns(BasicBlock* bb) {
  DCHECK_EQ(temp_bit_vector_size_, cu_->num_dalvik_registers);
  ArenaBitVector* temp_dalvik_register_v = temp_bit_vector_;

  if (bb->data_flow_info == NULL) {
    return false;
  }
  temp_dalvik_register_v->Copy(bb->data_flow_info->live_in_v);
  BasicBlock* bb_taken = GetBasicBlock(bb->taken);
  BasicBlock* bb_fall_through = GetBasicBlock(bb->fall_through);
  if (bb_taken && bb_taken->data_flow_info)
    ComputeSuccLineIn(temp_dalvik_register_v, bb_taken->data_flow_info->live_in_v,
                      bb->data_flow_info->def_v);
  if (bb_fall_through && bb_fall_through->data_flow_info)
    ComputeSuccLineIn(temp_dalvik_register_v, bb_fall_through->data_flow_info->live_in_v,
                      bb->data_flow_info->def_v);
  if (bb->successor_block_list_type != kNotUsed) {
    GrowableArray<SuccessorBlockInfo*>::Iterator iterator(bb->successor_blocks);
    while (true) {
      SuccessorBlockInfo *successor_block_info = iterator.Next();
      if (successor_block_info == NULL) {
        break;
      }
      BasicBlock* succ_bb = GetBasicBlock(successor_block_info->block);
      if (succ_bb->data_flow_info) {
        ComputeSuccLineIn(temp_dalvik_register_v, succ_bb->data_flow_info->live_in_v,
                          bb->data_flow_info->def_v);
      }
    }
  }
  if (!temp_dalvik_register_v->Equal(bb->data_flow_info->live_in_v)) {
    bb->data_flow_info->live_in_v->Copy(temp_dalvik_register_v);
    return true;
  }
  return false;
}

/* Insert phi nodes to for each variable to the dominance frontiers */
void MIRGraph::InsertPhiNodes() {
  int dalvik_reg;
  ArenaBitVector* phi_blocks = new (temp_scoped_alloc_.get()) ArenaBitVector(
      temp_scoped_alloc_.get(), GetNumBlocks(), false, kBitMapPhi);
  ArenaBitVector* input_blocks = new (temp_scoped_alloc_.get()) ArenaBitVector(
      temp_scoped_alloc_.get(), GetNumBlocks(), false, kBitMapInputBlocks);

  RepeatingPostOrderDfsIterator iter(this);
  bool change = false;
  for (BasicBlock* bb = iter.Next(false); bb != NULL; bb = iter.Next(change)) {
    change = ComputeBlockLiveIns(bb);
  }

  /* Iterate through each Dalvik register */
  for (dalvik_reg = cu_->num_dalvik_registers - 1; dalvik_reg >= 0; dalvik_reg--) {
    input_blocks->Copy(def_block_matrix_[dalvik_reg]);
    phi_blocks->ClearAllBits();
    do {
      // TUNING: When we repeat this, we could skip indexes from the previous pass.
      for (uint32_t idx : input_blocks->Indexes()) {
        BasicBlock* def_bb = GetBasicBlock(idx);
        if (def_bb->dom_frontier != nullptr) {
          phi_blocks->Union(def_bb->dom_frontier);
        }
      }
    } while (input_blocks->Union(phi_blocks));

    /*
     * Insert a phi node for dalvik_reg in the phi_blocks if the Dalvik
     * register is in the live-in set.
     */
    for (uint32_t idx : phi_blocks->Indexes()) {
      BasicBlock* phi_bb = GetBasicBlock(idx);
      /* Variable will be clobbered before being used - no need for phi */
      if (!phi_bb->data_flow_info->live_in_v->IsBitSet(dalvik_reg)) {
        continue;
      }
      MIR *phi = NewMIR();
      phi->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpPhi);
      phi->dalvikInsn.vA = dalvik_reg;
      phi->offset = phi_bb->start_offset;
      phi->m_unit_index = 0;  // Arbitrarily assign all Phi nodes to outermost method.
      phi_bb->PrependMIR(phi);
    }
  }
}

/*
 * Worker function to insert phi-operands with latest SSA names from
 * predecessor blocks
 */
bool MIRGraph::InsertPhiNodeOperands(BasicBlock* bb) {
  /* Phi nodes are at the beginning of each block */
  for (MIR* mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    if (mir->dalvikInsn.opcode != static_cast<Instruction::Code>(kMirOpPhi))
      return true;
    int ssa_reg = mir->ssa_rep->defs[0];
    DCHECK_GE(ssa_reg, 0);   // Shouldn't see compiler temps here
    int v_reg = SRegToVReg(ssa_reg);

    /* Iterate through the predecessors */
    GrowableArray<BasicBlockId>::Iterator iter(bb->predecessors);
    size_t num_uses = bb->predecessors->Size();
    AllocateSSAUseData(mir, num_uses);
    int* uses = mir->ssa_rep->uses;
    BasicBlockId* incoming =
        static_cast<BasicBlockId*>(arena_->Alloc(sizeof(BasicBlockId) * num_uses,
                                                 kArenaAllocDFInfo));
    mir->meta.phi_incoming = incoming;
    int idx = 0;
    while (true) {
      BasicBlock* pred_bb = GetBasicBlock(iter.Next());
      if (!pred_bb) {
       break;
      }
      int ssa_reg = pred_bb->data_flow_info->vreg_to_ssa_map_exit[v_reg];
      uses[idx] = ssa_reg;
      incoming[idx] = pred_bb->id;
      idx++;
    }
  }

  return true;
}

void MIRGraph::DoDFSPreOrderSSARename(BasicBlock* block) {
  if (block->visited || block->hidden) {
    return;
  }
  block->visited = true;

  /* Process this block */
  DoSSAConversion(block);
  int map_size = sizeof(int) * cu_->num_dalvik_registers;

  /* Save SSA map snapshot */
  ScopedArenaAllocator allocator(&cu_->arena_stack);
  int* saved_ssa_map =
      static_cast<int*>(allocator.Alloc(map_size, kArenaAllocDalvikToSSAMap));
  memcpy(saved_ssa_map, vreg_to_ssa_map_, map_size);

  if (block->fall_through != NullBasicBlockId) {
    DoDFSPreOrderSSARename(GetBasicBlock(block->fall_through));
    /* Restore SSA map snapshot */
    memcpy(vreg_to_ssa_map_, saved_ssa_map, map_size);
  }
  if (block->taken != NullBasicBlockId) {
    DoDFSPreOrderSSARename(GetBasicBlock(block->taken));
    /* Restore SSA map snapshot */
    memcpy(vreg_to_ssa_map_, saved_ssa_map, map_size);
  }
  if (block->successor_block_list_type != kNotUsed) {
    GrowableArray<SuccessorBlockInfo*>::Iterator iterator(block->successor_blocks);
    while (true) {
      SuccessorBlockInfo *successor_block_info = iterator.Next();
      if (successor_block_info == NULL) {
        break;
      }
      BasicBlock* succ_bb = GetBasicBlock(successor_block_info->block);
      DoDFSPreOrderSSARename(succ_bb);
      /* Restore SSA map snapshot */
      memcpy(vreg_to_ssa_map_, saved_ssa_map, map_size);
    }
  }
  return;
}

}  // namespace art
