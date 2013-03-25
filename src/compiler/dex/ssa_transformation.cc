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
#include "dataflow_iterator.h"

#define NOTVISITED (-1)

namespace art {

BasicBlock* MIRGraph::NeedsVisit(BasicBlock* bb) {
  if (bb != NULL) {
    if (bb->visited || bb->hidden) {
      bb = NULL;
    }
  }
  return bb;
}

BasicBlock* MIRGraph::NextUnvisitedSuccessor(BasicBlock* bb)
{
  BasicBlock* res = NeedsVisit(bb->fall_through);
  if (res == NULL) {
    res = NeedsVisit(bb->taken);
    if (res == NULL) {
      if (bb->successor_block_list.block_list_type != kNotUsed) {
        GrowableListIterator iterator;
        GrowableListIteratorInit(&bb->successor_block_list.blocks,
                                    &iterator);
        while (true) {
          SuccessorBlockInfo *sbi = reinterpret_cast<SuccessorBlockInfo*>
              (GrowableListIteratorNext(&iterator));
          if (sbi == NULL) break;
          res = NeedsVisit(sbi->block);
          if (res != NULL) break;
        }
      }
    }
  }
  return res;
}

void MIRGraph::MarkPreOrder(BasicBlock* block)
{
  block->visited = true;
  /* Enqueue the pre_order block id */
  InsertGrowableList(cu_, &dfs_order_, block->id);
}

void MIRGraph::RecordDFSOrders(BasicBlock* block)
{
  std::vector<BasicBlock*> succ;
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
    curr->dfs_id = dfs_post_order_.num_used;
    InsertGrowableList(cu_, &dfs_post_order_, curr->id);
    succ.pop_back();
  }
}

/* Sort the blocks by the Depth-First-Search */
void MIRGraph::ComputeDFSOrders()
{
  /* Initialize or reset the DFS pre_order list */
  if (dfs_order_.elem_list == NULL) {
    CompilerInitGrowableList(cu_, &dfs_order_, GetNumBlocks(), kListDfsOrder);
  } else {
    /* Just reset the used length on the counter */
    dfs_order_.num_used = 0;
  }

  /* Initialize or reset the DFS post_order list */
  if (dfs_post_order_.elem_list == NULL) {
    CompilerInitGrowableList(cu_, &dfs_post_order_, GetNumBlocks(), kListDfsPostOrder);
  } else {
    /* Just reset the used length on the counter */
    dfs_post_order_.num_used = 0;
  }

  // Reset visited flags from all nodes
  AllNodesIterator iter(this, false /* not iterative */);
  for (BasicBlock* bb = iter.Next(); bb != NULL; bb = iter.Next()) {
    ClearVisitedFlag(bb);
  }
  // Record dfs orders
  RecordDFSOrders(GetEntryBlock());

  num_reachable_blocks_ = dfs_order_.num_used;
}

/*
 * Mark block bit on the per-Dalvik register vector to denote that Dalvik
 * register idx is defined in BasicBlock bb.
 */
bool MIRGraph::FillDefBlockMatrix(BasicBlock* bb)
{
  if (bb->data_flow_info == NULL) return false;

  ArenaBitVectorIterator iterator;

  BitVectorIteratorInit(bb->data_flow_info->def_v, &iterator);
  while (true) {
    int idx = BitVectorIteratorNext(&iterator);
    if (idx == -1) break;
    /* Block bb defines register idx */
    SetBit(cu_, def_block_matrix_[idx], bb->id);
  }
  return true;
}

void MIRGraph::ComputeDefBlockMatrix()
{
  int num_registers = cu_->num_dalvik_registers;
  /* Allocate num_dalvik_registers bit vector pointers */
  def_block_matrix_ = static_cast<ArenaBitVector**>
      (NewMem(cu_, sizeof(ArenaBitVector *) * num_registers, true, kAllocDFInfo));
  int i;

  /* Initialize num_register vectors with num_blocks bits each */
  for (i = 0; i < num_registers; i++) {
    def_block_matrix_[i] = AllocBitVector(cu_, GetNumBlocks(), false, kBitMapBMatrix);
  }
  AllNodesIterator iter(this, false /* not iterative */);
  for (BasicBlock* bb = iter.Next(); bb != NULL; bb = iter.Next()) {
    FindLocalLiveIn(bb);
  }
  AllNodesIterator iter2(this, false /* not iterative */);
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
    SetBit(cu_, def_block_matrix_[in_reg], GetEntryBlock()->id);
  }
}

/* Compute the post-order traversal of the CFG */
void MIRGraph::ComputeDomPostOrderTraversal(BasicBlock* bb)
{
  ArenaBitVectorIterator bv_iterator;
  BitVectorIteratorInit(bb->i_dominated, &bv_iterator);

  /* Iterate through the dominated blocks first */
  while (true) {
    //TUNING: hot call to BitVectorIteratorNext
    int bb_idx = BitVectorIteratorNext(&bv_iterator);
    if (bb_idx == -1) break;
    BasicBlock* dominated_bb = GetBasicBlock(bb_idx);
    ComputeDomPostOrderTraversal(dominated_bb);
  }

  /* Enter the current block id */
  InsertGrowableList(cu_, &dom_post_order_traversal_, bb->id);

  /* hacky loop detection */
  if (bb->taken && IsBitSet(bb->dominators, bb->taken->id)) {
    attributes_ |= METHOD_HAS_LOOP;
  }
}

void MIRGraph::CheckForDominanceFrontier(BasicBlock* dom_bb,
                                         const BasicBlock* succ_bb)
{
  /*
   * TODO - evaluate whether phi will ever need to be inserted into exit
   * blocks.
   */
  if (succ_bb->i_dom != dom_bb &&
    succ_bb->block_type == kDalvikByteCode &&
    succ_bb->hidden == false) {
    SetBit(cu_, dom_bb->dom_frontier, succ_bb->id);
  }
}

/* Worker function to compute the dominance frontier */
bool MIRGraph::ComputeDominanceFrontier(BasicBlock* bb)
{
  /* Calculate DF_local */
  if (bb->taken) {
    CheckForDominanceFrontier(bb, bb->taken);
  }
  if (bb->fall_through) {
    CheckForDominanceFrontier(bb, bb->fall_through);
  }
  if (bb->successor_block_list.block_list_type != kNotUsed) {
    GrowableListIterator iterator;
    GrowableListIteratorInit(&bb->successor_block_list.blocks,
                                  &iterator);
      while (true) {
        SuccessorBlockInfo *successor_block_info =
            reinterpret_cast<SuccessorBlockInfo*>(GrowableListIteratorNext(&iterator));
        if (successor_block_info == NULL) break;
        BasicBlock* succ_bb = successor_block_info->block;
        CheckForDominanceFrontier(bb, succ_bb);
      }
  }

  /* Calculate DF_up */
  ArenaBitVectorIterator bv_iterator;
  BitVectorIteratorInit(bb->i_dominated, &bv_iterator);
  while (true) {
    //TUNING: hot call to BitVectorIteratorNext
    int dominated_idx = BitVectorIteratorNext(&bv_iterator);
    if (dominated_idx == -1) break;
    BasicBlock* dominated_bb = GetBasicBlock(dominated_idx);
    ArenaBitVectorIterator df_iterator;
    BitVectorIteratorInit(dominated_bb->dom_frontier, &df_iterator);
    while (true) {
      //TUNING: hot call to BitVectorIteratorNext
      int df_up_idx = BitVectorIteratorNext(&df_iterator);
      if (df_up_idx == -1) break;
      BasicBlock* df_up_block = GetBasicBlock(df_up_idx);
      CheckForDominanceFrontier(bb, df_up_block);
    }
  }

  return true;
}

/* Worker function for initializing domination-related data structures */
bool MIRGraph::InitializeDominationInfo(BasicBlock* bb)
{
  int num_total_blocks = GetBasicBlockListCount();

  if (bb->dominators == NULL ) {
    bb->dominators = AllocBitVector(cu_, num_total_blocks,
                                       false /* expandable */,
                                       kBitMapDominators);
    bb->i_dominated = AllocBitVector(cu_, num_total_blocks,
                                       false /* expandable */,
                                       kBitMapIDominated);
    bb->dom_frontier = AllocBitVector(cu_, num_total_blocks,
                                        false /* expandable */,
                                        kBitMapDomFrontier);
  } else {
    ClearAllBits(bb->dominators);
    ClearAllBits(bb->i_dominated);
    ClearAllBits(bb->dom_frontier);
  }
  /* Set all bits in the dominator vector */
  SetInitialBits(bb->dominators, num_total_blocks);

  return true;
}

/*
 * Walk through the ordered i_dom list until we reach common parent.
 * Given the ordering of i_dom_list, this common parent represents the
 * last element of the intersection of block1 and block2 dominators.
  */
int MIRGraph::FindCommonParent(int block1, int block2)
{
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
bool MIRGraph::ComputeblockIDom(BasicBlock* bb)
{
  GrowableListIterator iter;
  int idom = -1;

  /* Special-case entry block */
  if (bb == GetEntryBlock()) {
    return false;
  }

  /* Iterate through the predecessors */
  GrowableListIteratorInit(bb->predecessors, &iter);

  /* Find the first processed predecessor */
  while (true) {
    BasicBlock* pred_bb = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iter));
    CHECK(pred_bb != NULL);
    if (i_dom_list_[pred_bb->dfs_id] != NOTVISITED) {
      idom = pred_bb->dfs_id;
      break;
    }
  }

  /* Scan the rest of the predecessors */
  while (true) {
      BasicBlock* pred_bb = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iter));
      if (!pred_bb) break;
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
bool MIRGraph::ComputeBlockDominators(BasicBlock* bb)
{
  if (bb == GetEntryBlock()) {
    ClearAllBits(bb->dominators);
  } else {
    CopyBitVector(bb->dominators, bb->i_dom->dominators);
  }
  SetBit(cu_, bb->dominators, bb->id);
  return false;
}

bool MIRGraph::SetDominators(BasicBlock* bb)
{
  if (bb != GetEntryBlock()) {
    int idom_dfs_idx = i_dom_list_[bb->dfs_id];
    DCHECK_NE(idom_dfs_idx, NOTVISITED);
    int i_dom_idx = dfs_post_order_.elem_list[idom_dfs_idx];
    BasicBlock* i_dom = GetBasicBlock(i_dom_idx);
    bb->i_dom = i_dom;
    /* Add bb to the i_dominated set of the immediate dominator block */
    SetBit(cu_, i_dom->i_dominated, bb->id);
  }
  return false;
}

/* Compute dominators, immediate dominator, and dominance fronter */
void MIRGraph::ComputeDominators()
{
  int num_reachable_blocks = num_reachable_blocks_;
  int num_total_blocks = GetBasicBlockListCount();

  /* Initialize domination-related data structures */
  ReachableNodesIterator iter(this, false /* not iterative */);
  for (BasicBlock* bb = iter.Next(); bb != NULL; bb = iter.Next()) {
    InitializeDominationInfo(bb);
  }

  /* Initalize & Clear i_dom_list */
  if (i_dom_list_ == NULL) {
    i_dom_list_ = static_cast<int*>(NewMem(cu_, sizeof(int) * num_reachable_blocks,
                                               false, kAllocDFInfo));
  }
  for (int i = 0; i < num_reachable_blocks; i++) {
    i_dom_list_[i] = NOTVISITED;
  }

  /* For post-order, last block is entry block.  Set its i_dom to istelf */
  DCHECK_EQ(GetEntryBlock()->dfs_id, num_reachable_blocks-1);
  i_dom_list_[GetEntryBlock()->dfs_id] = GetEntryBlock()->dfs_id;

  /* Compute the immediate dominators */
  ReversePostOrderDfsIterator iter2(this, true /* iterative */);
  bool change = false;
  for (BasicBlock* bb = iter2.Next(false); bb != NULL; bb = iter2.Next(change)) {
    change = ComputeblockIDom(bb);
  }

  /* Set the dominator for the root node */
  ClearAllBits(GetEntryBlock()->dominators);
  SetBit(cu_, GetEntryBlock()->dominators, GetEntryBlock()->id);

  if (temp_block_v_ == NULL) {
    temp_block_v_ = AllocBitVector(cu_, num_total_blocks, false /* expandable */, kBitMapTmpBlockV);
  } else {
    ClearAllBits(temp_block_v_);
  }
  GetEntryBlock()->i_dom = NULL;

  ReachableNodesIterator iter3(this, false /* not iterative */);
  for (BasicBlock* bb = iter3.Next(); bb != NULL; bb = iter3.Next()) {
    SetDominators(bb);
  }

  ReversePostOrderDfsIterator iter4(this, false /* not iterative */);
  for (BasicBlock* bb = iter4.Next(); bb != NULL; bb = iter4.Next()) {
    ComputeBlockDominators(bb);
  }

  /*
   * Now go ahead and compute the post order traversal based on the
   * i_dominated sets.
   */
  if (dom_post_order_traversal_.elem_list == NULL) {
    CompilerInitGrowableList(cu_, &dom_post_order_traversal_,
                        num_reachable_blocks, kListDomPostOrderTraversal);
  } else {
    dom_post_order_traversal_.num_used = 0;
  }

  ComputeDomPostOrderTraversal(GetEntryBlock());
  DCHECK_EQ(dom_post_order_traversal_.num_used, static_cast<unsigned>(num_reachable_blocks_));

  /* Now compute the dominance frontier for each block */
  PostOrderDOMIterator iter5(this, false /* not iterative */);
  for (BasicBlock* bb = iter5.Next(); bb != NULL; bb = iter5.Next()) {
    ComputeDominanceFrontier(bb);
  }
}

/*
 * Perform dest U= src1 ^ ~src2
 * This is probably not general enough to be placed in BitVector.[ch].
 */
void MIRGraph::ComputeSuccLineIn(ArenaBitVector* dest, const ArenaBitVector* src1,
                                 const ArenaBitVector* src2)
{
  if (dest->storage_size != src1->storage_size ||
    dest->storage_size != src2->storage_size ||
    dest->expandable != src1->expandable ||
    dest->expandable != src2->expandable) {
    LOG(FATAL) << "Incompatible set properties";
  }

  unsigned int idx;
  for (idx = 0; idx < dest->storage_size; idx++) {
    dest->storage[idx] |= src1->storage[idx] & ~src2->storage[idx];
  }
}

/*
 * Iterate through all successor blocks and propagate up the live-in sets.
 * The calculated result is used for phi-node pruning - where we only need to
 * insert a phi node if the variable is live-in to the block.
 */
bool MIRGraph::ComputeBlockLiveIns(BasicBlock* bb)
{
  ArenaBitVector* temp_dalvik_register_v = temp_dalvik_register_v_;

  if (bb->data_flow_info == NULL) return false;
  CopyBitVector(temp_dalvik_register_v, bb->data_flow_info->live_in_v);
  if (bb->taken && bb->taken->data_flow_info)
    ComputeSuccLineIn(temp_dalvik_register_v, bb->taken->data_flow_info->live_in_v,
                      bb->data_flow_info->def_v);
  if (bb->fall_through && bb->fall_through->data_flow_info)
    ComputeSuccLineIn(temp_dalvik_register_v, bb->fall_through->data_flow_info->live_in_v,
                      bb->data_flow_info->def_v);
  if (bb->successor_block_list.block_list_type != kNotUsed) {
    GrowableListIterator iterator;
    GrowableListIteratorInit(&bb->successor_block_list.blocks,
                                &iterator);
    while (true) {
      SuccessorBlockInfo *successor_block_info =
          reinterpret_cast<SuccessorBlockInfo*>(GrowableListIteratorNext(&iterator));
      if (successor_block_info == NULL) break;
      BasicBlock* succ_bb = successor_block_info->block;
      if (succ_bb->data_flow_info) {
        ComputeSuccLineIn(temp_dalvik_register_v, succ_bb->data_flow_info->live_in_v,
                          bb->data_flow_info->def_v);
      }
    }
  }
  if (CompareBitVectors(temp_dalvik_register_v, bb->data_flow_info->live_in_v)) {
    CopyBitVector(bb->data_flow_info->live_in_v, temp_dalvik_register_v);
    return true;
  }
  return false;
}

/* Insert phi nodes to for each variable to the dominance frontiers */
void MIRGraph::InsertPhiNodes()
{
  int dalvik_reg;
  ArenaBitVector* phi_blocks = AllocBitVector(cu_, GetNumBlocks(), false, kBitMapPhi);
  ArenaBitVector* tmp_blocks = AllocBitVector(cu_, GetNumBlocks(), false, kBitMapTmpBlocks);
  ArenaBitVector* input_blocks = AllocBitVector(cu_, GetNumBlocks(), false, kBitMapInputBlocks);

  temp_dalvik_register_v_ =
      AllocBitVector(cu_, cu_->num_dalvik_registers, false, kBitMapRegisterV);

  PostOrderDfsIterator iter(this, true /* iterative */);
  bool change = false;
  for (BasicBlock* bb = iter.Next(false); bb != NULL; bb = iter.Next(change)) {
    change = ComputeBlockLiveIns(bb);
  }

  /* Iterate through each Dalvik register */
  for (dalvik_reg = cu_->num_dalvik_registers - 1; dalvik_reg >= 0; dalvik_reg--) {
    bool change;
    ArenaBitVectorIterator iterator;

    CopyBitVector(input_blocks, def_block_matrix_[dalvik_reg]);
    ClearAllBits(phi_blocks);

    /* Calculate the phi blocks for each Dalvik register */
    do {
      change = false;
      ClearAllBits(tmp_blocks);
      BitVectorIteratorInit(input_blocks, &iterator);

      while (true) {
        int idx = BitVectorIteratorNext(&iterator);
        if (idx == -1) break;
          BasicBlock* def_bb = GetBasicBlock(idx);

          /* Merge the dominance frontier to tmp_blocks */
          //TUNING: hot call to UnifyBitVetors
          if (def_bb->dom_frontier != NULL) {
            UnifyBitVetors(tmp_blocks, tmp_blocks, def_bb->dom_frontier);
          }
        }
        if (CompareBitVectors(phi_blocks, tmp_blocks)) {
          change = true;
          CopyBitVector(phi_blocks, tmp_blocks);

          /*
           * Iterate through the original blocks plus the new ones in
           * the dominance frontier.
           */
          CopyBitVector(input_blocks, phi_blocks);
          UnifyBitVetors(input_blocks, input_blocks, def_block_matrix_[dalvik_reg]);
      }
    } while (change);

    /*
     * Insert a phi node for dalvik_reg in the phi_blocks if the Dalvik
     * register is in the live-in set.
     */
    BitVectorIteratorInit(phi_blocks, &iterator);
    while (true) {
      int idx = BitVectorIteratorNext(&iterator);
      if (idx == -1) break;
      BasicBlock* phi_bb = GetBasicBlock(idx);
      /* Variable will be clobbered before being used - no need for phi */
      if (!IsBitSet(phi_bb->data_flow_info->live_in_v, dalvik_reg)) continue;
      MIR *phi = static_cast<MIR*>(NewMem(cu_, sizeof(MIR), true, kAllocDFInfo));
      phi->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpPhi);
      phi->dalvikInsn.vA = dalvik_reg;
      phi->offset = phi_bb->start_offset;
      phi->m_unit_index = 0; // Arbitrarily assign all Phi nodes to outermost method.
      PrependMIR(phi_bb, phi);
    }
  }
}

/*
 * Worker function to insert phi-operands with latest SSA names from
 * predecessor blocks
 */
bool MIRGraph::InsertPhiNodeOperands(BasicBlock* bb)
{
  GrowableListIterator iter;
  MIR *mir;
  std::vector<int> uses;
  std::vector<int> incoming_arc;

  /* Phi nodes are at the beginning of each block */
  for (mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    if (mir->dalvikInsn.opcode != static_cast<Instruction::Code>(kMirOpPhi))
      return true;
    int ssa_reg = mir->ssa_rep->defs[0];
    DCHECK_GE(ssa_reg, 0);   // Shouldn't see compiler temps here
    int v_reg = SRegToVReg(ssa_reg);

    uses.clear();
    incoming_arc.clear();

    /* Iterate through the predecessors */
    GrowableListIteratorInit(bb->predecessors, &iter);
    while (true) {
      BasicBlock* pred_bb =
         reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iter));
      if (!pred_bb) break;
      int ssa_reg = pred_bb->data_flow_info->vreg_to_ssa_map[v_reg];
      uses.push_back(ssa_reg);
      incoming_arc.push_back(pred_bb->id);
    }

    /* Count the number of SSA registers for a Dalvik register */
    int num_uses = uses.size();
    mir->ssa_rep->num_uses = num_uses;
    mir->ssa_rep->uses =
        static_cast<int*>(NewMem(cu_, sizeof(int) * num_uses, false, kAllocDFInfo));
    mir->ssa_rep->fp_use =
        static_cast<bool*>(NewMem(cu_, sizeof(bool) * num_uses, true, kAllocDFInfo));
    int* incoming =
        static_cast<int*>(NewMem(cu_, sizeof(int) * num_uses, false, kAllocDFInfo));
    // TODO: Ugly, rework (but don't burden each MIR/LIR for Phi-only needs)
    mir->dalvikInsn.vB = reinterpret_cast<uintptr_t>(incoming);

    /* Set the uses array for the phi node */
    int *use_ptr = mir->ssa_rep->uses;
    for (int i = 0; i < num_uses; i++) {
      *use_ptr++ = uses[i];
      *incoming++ = incoming_arc[i];
    }
  }

  return true;
}

void MIRGraph::DoDFSPreOrderSSARename(BasicBlock* block)
{

  if (block->visited || block->hidden) return;
  block->visited = true;

  /* Process this block */
  DoSSAConversion(block);
  int map_size = sizeof(int) * cu_->num_dalvik_registers;

  /* Save SSA map snapshot */
  int* saved_ssa_map = static_cast<int*>(NewMem(cu_, map_size, false, kAllocDalvikToSSAMap));
  memcpy(saved_ssa_map, vreg_to_ssa_map_, map_size);

  if (block->fall_through) {
    DoDFSPreOrderSSARename(block->fall_through);
    /* Restore SSA map snapshot */
    memcpy(vreg_to_ssa_map_, saved_ssa_map, map_size);
  }
  if (block->taken) {
    DoDFSPreOrderSSARename(block->taken);
    /* Restore SSA map snapshot */
    memcpy(vreg_to_ssa_map_, saved_ssa_map, map_size);
  }
  if (block->successor_block_list.block_list_type != kNotUsed) {
    GrowableListIterator iterator;
    GrowableListIteratorInit(&block->successor_block_list.blocks, &iterator);
    while (true) {
      SuccessorBlockInfo *successor_block_info =
          reinterpret_cast<SuccessorBlockInfo*>(GrowableListIteratorNext(&iterator));
      if (successor_block_info == NULL) break;
      BasicBlock* succ_bb = successor_block_info->block;
      DoDFSPreOrderSSARename(succ_bb);
      /* Restore SSA map snapshot */
      memcpy(vreg_to_ssa_map_, saved_ssa_map, map_size);
    }
  }
  vreg_to_ssa_map_ = saved_ssa_map;
  return;
}

/* Perform SSA transformation for the whole method */
void MIRGraph::SSATransformation()
{
  /* Compute the DFS order */
  ComputeDFSOrders();

  /* Compute the dominator info */
  ComputeDominators();

  /* Allocate data structures in preparation for SSA conversion */
  CompilerInitializeSSAConversion();

  /* Find out the "Dalvik reg def x block" relation */
  ComputeDefBlockMatrix();

  /* Insert phi nodes to dominance frontiers for all variables */
  InsertPhiNodes();

  /* Rename register names by local defs and phi nodes */
  AllNodesIterator iter1(this, false /* not iterative */);
  for (BasicBlock* bb = iter1.Next(); bb != NULL; bb = iter1.Next()) {
    ClearVisitedFlag(bb);
  }
  DoDFSPreOrderSSARename(GetEntryBlock());

  /*
   * Shared temp bit vector used by each block to count the number of defs
   * from all the predecessor blocks.
   */
  temp_ssa_register_v_ = AllocBitVector(cu_, GetNumSSARegs(), false, kBitMapTempSSARegisterV);

  /* Insert phi-operands with latest SSA names from predecessor blocks */
  ReachableNodesIterator iter2(this, false /* not iterative */);
  for (BasicBlock* bb = iter2.Next(); bb != NULL; bb = iter2.Next()) {
    InsertPhiNodeOperands(bb);
  }

  if (cu_->enable_debug & (1 << kDebugDumpCFG)) {
    DumpCFG("/sdcard/3_post_ssa_cfg/", false);
  }
  if (cu_->enable_debug & (1 << kDebugVerifyDataflow)) {
    VerifyDataflow();
  }
}

}  // namespace art
