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
#include "dataflow.h"

namespace art {

// Make sure iterative dfs recording matches old recursive version
//#define TEST_DFS

static BasicBlock* NeedsVisit(BasicBlock* bb) {
  if (bb != NULL) {
    if (bb->visited || bb->hidden) {
      bb = NULL;
    }
  }
  return bb;
}

static BasicBlock* NextUnvisitedSuccessor(BasicBlock* bb)
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

static void MarkPreOrder(CompilationUnit* cu, BasicBlock* block)
{
  block->visited = true;
  /* Enqueue the pre_order block id */
  InsertGrowableList(cu, &cu->dfs_order, block->id);
}

static void RecordDFSOrders(CompilationUnit* cu, BasicBlock* block)
{
  std::vector<BasicBlock*> succ;
  MarkPreOrder(cu, block);
  succ.push_back(block);
  while (!succ.empty()) {
    BasicBlock* curr = succ.back();
    BasicBlock* next_successor = NextUnvisitedSuccessor(curr);
    if (next_successor != NULL) {
      MarkPreOrder(cu, next_successor);
      succ.push_back(next_successor);
      continue;
    }
    curr->dfs_id = cu->dfs_post_order.num_used;
    InsertGrowableList(cu, &cu->dfs_post_order, curr->id);
    succ.pop_back();
  }
}

#if defined(TEST_DFS)
/* Enter the node to the dfs_order list then visit its successors */
static void RecursiveRecordDFSOrders(CompilationUnit* cu, BasicBlock* block)
{

  if (block->visited || block->hidden) return;
  block->visited = true;

  // Can this block be reached only via previous block fallthrough?
  if ((block->block_type == kDalvikByteCode) &&
      (block->predecessors->num_used == 1)) {
    DCHECK_GE(cu->dfs_order.num_used, 1U);
    int prev_idx = cu->dfs_order.num_used - 1;
    int prev_id = cu->dfs_order.elem_list[prev_idx];
    BasicBlock* pred_bb = (BasicBlock*)block->predecessors->elem_list[0];
  }

  /* Enqueue the pre_order block id */
  InsertGrowableList(cu, &cu->dfs_order, block->id);

  if (block->fall_through) {
    RecursiveRecordDFSOrders(cu, block->fall_through);
  }
  if (block->taken) RecursiveRecordDFSOrders(cu, block->taken);
  if (block->successor_block_list.block_list_type != kNotUsed) {
    GrowableListIterator iterator;
    GrowableListIteratorInit(&block->successor_block_list.blocks,
                                &iterator);
    while (true) {
      SuccessorBlockInfo *successor_block_info =
          (SuccessorBlockInfo *) GrowableListIteratorNext(&iterator);
      if (successor_block_info == NULL) break;
      BasicBlock* succ_bb = successor_block_info->block;
      RecursiveRecordDFSOrders(cu, succ_bb);
    }
  }

  /* Record postorder in basic block and enqueue normal id in dfs_post_order */
  block->dfs_id = cu->dfs_post_order.num_used;
  InsertGrowableList(cu, &cu->dfs_post_order, block->id);
  return;
}
#endif

/* Sort the blocks by the Depth-First-Search */
static void ComputeDFSOrders(CompilationUnit* cu)
{
  /* Initialize or reset the DFS pre_order list */
  if (cu->dfs_order.elem_list == NULL) {
    CompilerInitGrowableList(cu, &cu->dfs_order, cu->num_blocks,
                        kListDfsOrder);
  } else {
    /* Just reset the used length on the counter */
    cu->dfs_order.num_used = 0;
  }

  /* Initialize or reset the DFS post_order list */
  if (cu->dfs_post_order.elem_list == NULL) {
    CompilerInitGrowableList(cu, &cu->dfs_post_order, cu->num_blocks,
                        kListDfsPostOrder);
  } else {
    /* Just reset the used length on the counter */
    cu->dfs_post_order.num_used = 0;
  }

#if defined(TEST_DFS)
  // Reset visited flags
  DataFlowAnalysisDispatcher(cu, ClearVisitedFlag,
                                kAllNodes, false /* is_iterative */);
  // Record pre and post order dfs
  RecursiveRecordDFSOrders(cu, cu->entry_block);
  // Copy the results for later comparison and reset the lists
  GrowableList recursive_dfs_order;
  GrowableList recursive_dfs_post_order;
  CompilerInitGrowableList(cu, &recursive_dfs_order, cu->dfs_order.num_used,
                      kListDfsOrder);
  for (unsigned int i = 0; i < cu->dfs_order.num_used; i++) {
    InsertGrowableList(cu, &recursive_dfs_order,
                          cu->dfs_order.elem_list[i]);
  }
  cu->dfs_order.num_used = 0;
  CompilerInitGrowableList(cu, &recursive_dfs_post_order,
                      cu->dfs_post_order.num_used, kListDfsOrder);
  for (unsigned int i = 0; i < cu->dfs_post_order.num_used; i++) {
    InsertGrowableList(cu, &recursive_dfs_post_order,
                          cu->dfs_post_order.elem_list[i]);
  }
  cu->dfs_post_order.num_used = 0;
#endif

  // Reset visited flags from all nodes
  DataFlowAnalysisDispatcher(cu, ClearVisitedFlag,
                                kAllNodes, false /* is_iterative */);
  // Record dfs orders
  RecordDFSOrders(cu, cu->entry_block);

#if defined(TEST_DFS)
  bool mismatch = false;
  mismatch |= (cu->dfs_order.num_used != recursive_dfs_order.num_used);
  for (unsigned int i = 0; i < cu->dfs_order.num_used; i++) {
    mismatch |= (cu->dfs_order.elem_list[i] !=
                 recursive_dfs_order.elem_list[i]);
  }
  mismatch |= (cu->dfs_post_order.num_used != recursive_dfs_post_order.num_used);
  for (unsigned int i = 0; i < cu->dfs_post_order.num_used; i++) {
    mismatch |= (cu->dfs_post_order.elem_list[i] !=
                 recursive_dfs_post_order.elem_list[i]);
  }
  if (mismatch) {
    LOG(INFO) << "Mismatch for "
              << PrettyMethod(cu->method_idx, *cu->dex_file);
    LOG(INFO) << "New dfs";
    for (unsigned int i = 0; i < cu->dfs_order.num_used; i++) {
      LOG(INFO) << i << " - " << cu->dfs_order.elem_list[i];
    }
    LOG(INFO) << "Recursive dfs";
    for (unsigned int i = 0; i < recursive_dfs_order.num_used; i++) {
      LOG(INFO) << i << " - " << recursive_dfs_order.elem_list[i];
    }
    LOG(INFO) << "New post dfs";
    for (unsigned int i = 0; i < cu->dfs_post_order.num_used; i++) {
      LOG(INFO) << i << " - " << cu->dfs_post_order.elem_list[i];
    }
    LOG(INFO) << "Recursive post dfs";
    for (unsigned int i = 0; i < recursive_dfs_post_order.num_used; i++) {
      LOG(INFO) << i << " - " << recursive_dfs_post_order.elem_list[i];
    }
  }
  CHECK_EQ(cu->dfs_order.num_used, recursive_dfs_order.num_used);
  for (unsigned int i = 0; i < cu->dfs_order.num_used; i++) {
    CHECK_EQ(cu->dfs_order.elem_list[i], recursive_dfs_order.elem_list[i]);
  }
  CHECK_EQ(cu->dfs_post_order.num_used, recursive_dfs_post_order.num_used);
  for (unsigned int i = 0; i < cu->dfs_post_order.num_used; i++) {
    CHECK_EQ(cu->dfs_post_order.elem_list[i],
             recursive_dfs_post_order.elem_list[i]);
  }
#endif

  cu->num_reachable_blocks = cu->dfs_order.num_used;
}

/*
 * Mark block bit on the per-Dalvik register vector to denote that Dalvik
 * register idx is defined in BasicBlock bb.
 */
static bool FillDefBlockMatrix(CompilationUnit* cu, BasicBlock* bb)
{
  if (bb->data_flow_info == NULL) return false;

  ArenaBitVectorIterator iterator;

  BitVectorIteratorInit(bb->data_flow_info->def_v, &iterator);
  while (true) {
    int idx = BitVectorIteratorNext(&iterator);
    if (idx == -1) break;
    /* Block bb defines register idx */
    SetBit(cu, cu->def_block_matrix[idx], bb->id);
  }
  return true;
}

static void ComputeDefBlockMatrix(CompilationUnit* cu)
{
  int num_registers = cu->num_dalvik_registers;
  /* Allocate num_dalvik_registers bit vector pointers */
  cu->def_block_matrix = static_cast<ArenaBitVector**>
      (NewMem(cu, sizeof(ArenaBitVector *) * num_registers, true, kAllocDFInfo));
  int i;

  /* Initialize num_register vectors with num_blocks bits each */
  for (i = 0; i < num_registers; i++) {
    cu->def_block_matrix[i] = AllocBitVector(cu, cu->num_blocks,
                                                 false, kBitMapBMatrix);
  }
  DataFlowAnalysisDispatcher(cu, FindLocalLiveIn,
                                kAllNodes, false /* is_iterative */);
  DataFlowAnalysisDispatcher(cu, FillDefBlockMatrix,
                                kAllNodes, false /* is_iterative */);

  /*
   * Also set the incoming parameters as defs in the entry block.
   * Only need to handle the parameters for the outer method.
   */
  int num_regs = cu->num_dalvik_registers;
  int in_reg = num_regs - cu->num_ins;
  for (; in_reg < num_regs; in_reg++) {
    SetBit(cu, cu->def_block_matrix[in_reg], cu->entry_block->id);
  }
}

/* Compute the post-order traversal of the CFG */
static void ComputeDomPostOrderTraversal(CompilationUnit* cu, BasicBlock* bb)
{
  ArenaBitVectorIterator bv_iterator;
  BitVectorIteratorInit(bb->i_dominated, &bv_iterator);
  GrowableList* block_list = &cu->block_list;

  /* Iterate through the dominated blocks first */
  while (true) {
    //TUNING: hot call to BitVectorIteratorNext
    int bb_idx = BitVectorIteratorNext(&bv_iterator);
    if (bb_idx == -1) break;
    BasicBlock* dominated_bb =
        reinterpret_cast<BasicBlock*>(GrowableListGetElement(block_list, bb_idx));
    ComputeDomPostOrderTraversal(cu, dominated_bb);
  }

  /* Enter the current block id */
  InsertGrowableList(cu, &cu->dom_post_order_traversal, bb->id);

  /* hacky loop detection */
  if (bb->taken && IsBitSet(bb->dominators, bb->taken->id)) {
    cu->has_loop = true;
  }
}

static void CheckForDominanceFrontier(CompilationUnit* cu, BasicBlock* dom_bb,
                                      const BasicBlock* succ_bb)
{
  /*
   * TODO - evaluate whether phi will ever need to be inserted into exit
   * blocks.
   */
  if (succ_bb->i_dom != dom_bb &&
    succ_bb->block_type == kDalvikByteCode &&
    succ_bb->hidden == false) {
    SetBit(cu, dom_bb->dom_frontier, succ_bb->id);
  }
}

/* Worker function to compute the dominance frontier */
static bool ComputeDominanceFrontier(CompilationUnit* cu, BasicBlock* bb)
{
  GrowableList* block_list = &cu->block_list;

  /* Calculate DF_local */
  if (bb->taken) {
    CheckForDominanceFrontier(cu, bb, bb->taken);
  }
  if (bb->fall_through) {
    CheckForDominanceFrontier(cu, bb, bb->fall_through);
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
        CheckForDominanceFrontier(cu, bb, succ_bb);
      }
  }

  /* Calculate DF_up */
  ArenaBitVectorIterator bv_iterator;
  BitVectorIteratorInit(bb->i_dominated, &bv_iterator);
  while (true) {
    //TUNING: hot call to BitVectorIteratorNext
    int dominated_idx = BitVectorIteratorNext(&bv_iterator);
    if (dominated_idx == -1) break;
    BasicBlock* dominated_bb =
        reinterpret_cast<BasicBlock*>(GrowableListGetElement(block_list, dominated_idx));
    ArenaBitVectorIterator df_iterator;
    BitVectorIteratorInit(dominated_bb->dom_frontier, &df_iterator);
    while (true) {
      //TUNING: hot call to BitVectorIteratorNext
      int df_up_idx = BitVectorIteratorNext(&df_iterator);
      if (df_up_idx == -1) break;
      BasicBlock* df_up_block =
          reinterpret_cast<BasicBlock*>( GrowableListGetElement(block_list, df_up_idx));
      CheckForDominanceFrontier(cu, bb, df_up_block);
    }
  }

  return true;
}

/* Worker function for initializing domination-related data structures */
static bool InitializeDominationInfo(CompilationUnit* cu, BasicBlock* bb)
{
  int num_total_blocks = cu->block_list.num_used;

  if (bb->dominators == NULL ) {
    bb->dominators = AllocBitVector(cu, num_total_blocks,
                                       false /* expandable */,
                                       kBitMapDominators);
    bb->i_dominated = AllocBitVector(cu, num_total_blocks,
                                       false /* expandable */,
                                       kBitMapIDominated);
    bb->dom_frontier = AllocBitVector(cu, num_total_blocks,
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
 * Worker function to compute each block's dominators.  This implementation
 * is only used when kDebugVerifyDataflow is active and should compute
 * the same dominator sets as ComputeBlockDominiators.
 */
static bool SlowComputeBlockDominators(CompilationUnit* cu, BasicBlock* bb)
{
  GrowableList* block_list = &cu->block_list;
  int num_total_blocks = block_list->num_used;
  ArenaBitVector* temp_block_v = cu->temp_block_v;
  GrowableListIterator iter;

  /*
   * The dominator of the entry block has been preset to itself and we need
   * to skip the calculation here.
   */
  if (bb == cu->entry_block) return false;

  SetInitialBits(temp_block_v, num_total_blocks);

  /* Iterate through the predecessors */
  GrowableListIteratorInit(bb->predecessors, &iter);
  while (true) {
    BasicBlock* pred_bb = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iter));
    if (!pred_bb) break;
    /* temp_block_v = temp_block_v ^ dominators */
    if (pred_bb->dominators != NULL) {
      IntersectBitVectors(temp_block_v, temp_block_v, pred_bb->dominators);
    }
  }
  SetBit(cu, temp_block_v, bb->id);
  if (CompareBitVectors(temp_block_v, bb->dominators)) {
    CopyBitVector(bb->dominators, temp_block_v);
    return true;
  }
  return false;
}

/*
 * Worker function to compute the idom.  This implementation is only
 * used when kDebugVerifyDataflow is active and should compute the
 * same i_dom as ComputeblockIDom.
 */
static bool SlowComputeBlockIDom(CompilationUnit* cu, BasicBlock* bb)
{
  GrowableList* block_list = &cu->block_list;
  ArenaBitVector* temp_block_v = cu->temp_block_v;
  ArenaBitVectorIterator bv_iterator;
  BasicBlock* i_dom;

  if (bb == cu->entry_block) return false;

  CopyBitVector(temp_block_v, bb->dominators);
  ClearBit(temp_block_v, bb->id);
  BitVectorIteratorInit(temp_block_v, &bv_iterator);

  /* Should not see any dead block */
  DCHECK_NE(CountSetBits(temp_block_v),  0);
  if (CountSetBits(temp_block_v) == 1) {
    i_dom = reinterpret_cast<BasicBlock*>
        (GrowableListGetElement(block_list, BitVectorIteratorNext(&bv_iterator)));
    bb->i_dom = i_dom;
  } else {
    int i_dom_idx = BitVectorIteratorNext(&bv_iterator);
    DCHECK_NE(i_dom_idx, -1);
    while (true) {
      int next_dom = BitVectorIteratorNext(&bv_iterator);
      if (next_dom == -1) break;
      BasicBlock* next_dom_bb =
          reinterpret_cast<BasicBlock*>(GrowableListGetElement(block_list, next_dom));
      /* i_dom dominates next_dom - set new i_dom */
      if (IsBitSet(next_dom_bb->dominators, i_dom_idx)) {
          i_dom_idx = next_dom;
      }

    }
    i_dom = reinterpret_cast<BasicBlock*>(GrowableListGetElement(block_list, i_dom_idx));
    /* Set the immediate dominator block for bb */
    bb->i_dom = i_dom;
  }
  /* Add bb to the i_dominated set of the immediate dominator block */
  SetBit(cu, i_dom->i_dominated, bb->id);
  return true;
}

/*
 * Walk through the ordered i_dom list until we reach common parent.
 * Given the ordering of i_dom_list, this common parent represents the
 * last element of the intersection of block1 and block2 dominators.
  */
static int FindCommonParent(CompilationUnit *cu, int block1, int block2)
{
  while (block1 != block2) {
    while (block1 < block2) {
      block1 = cu->i_dom_list[block1];
      DCHECK_NE(block1, NOTVISITED);
    }
    while (block2 < block1) {
      block2 = cu->i_dom_list[block2];
      DCHECK_NE(block2, NOTVISITED);
    }
  }
  return block1;
}

/* Worker function to compute each block's immediate dominator */
static bool ComputeblockIDom(CompilationUnit* cu, BasicBlock* bb)
{
  GrowableListIterator iter;
  int idom = -1;

  /* Special-case entry block */
  if (bb == cu->entry_block) {
    return false;
  }

  /* Iterate through the predecessors */
  GrowableListIteratorInit(bb->predecessors, &iter);

  /* Find the first processed predecessor */
  while (true) {
    BasicBlock* pred_bb = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iter));
    CHECK(pred_bb != NULL);
    if (cu->i_dom_list[pred_bb->dfs_id] != NOTVISITED) {
      idom = pred_bb->dfs_id;
      break;
    }
  }

  /* Scan the rest of the predecessors */
  while (true) {
      BasicBlock* pred_bb = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iter));
      if (!pred_bb) break;
      if (cu->i_dom_list[pred_bb->dfs_id] == NOTVISITED) {
        continue;
      } else {
        idom = FindCommonParent(cu, pred_bb->dfs_id, idom);
      }
  }

  DCHECK_NE(idom, NOTVISITED);

  /* Did something change? */
  if (cu->i_dom_list[bb->dfs_id] != idom) {
    cu->i_dom_list[bb->dfs_id] = idom;
    return true;
  }
  return false;
}

/* Worker function to compute each block's domintors */
static bool ComputeBlockDominiators(CompilationUnit* cu, BasicBlock* bb)
{
  if (bb == cu->entry_block) {
    ClearAllBits(bb->dominators);
  } else {
    CopyBitVector(bb->dominators, bb->i_dom->dominators);
  }
  SetBit(cu, bb->dominators, bb->id);
  return false;
}

static bool SetDominators(CompilationUnit* cu, BasicBlock* bb)
{
  if (bb != cu->entry_block) {
    int idom_dfs_idx = cu->i_dom_list[bb->dfs_id];
    DCHECK_NE(idom_dfs_idx, NOTVISITED);
    int i_dom_idx = cu->dfs_post_order.elem_list[idom_dfs_idx];
    BasicBlock* i_dom =
        reinterpret_cast<BasicBlock*>(GrowableListGetElement(&cu->block_list, i_dom_idx));
    if (cu->enable_debug & (1 << kDebugVerifyDataflow)) {
      DCHECK_EQ(bb->i_dom->id, i_dom->id);
    }
    bb->i_dom = i_dom;
    /* Add bb to the i_dominated set of the immediate dominator block */
    SetBit(cu, i_dom->i_dominated, bb->id);
  }
  return false;
}

/* Compute dominators, immediate dominator, and dominance fronter */
static void ComputeDominators(CompilationUnit* cu)
{
  int num_reachable_blocks = cu->num_reachable_blocks;
  int num_total_blocks = cu->block_list.num_used;

  /* Initialize domination-related data structures */
  DataFlowAnalysisDispatcher(cu, InitializeDominationInfo,
                                kReachableNodes, false /* is_iterative */);

  /* Initalize & Clear i_dom_list */
  if (cu->i_dom_list == NULL) {
    cu->i_dom_list = static_cast<int*>(NewMem(cu, sizeof(int) * num_reachable_blocks,
                                               false, kAllocDFInfo));
  }
  for (int i = 0; i < num_reachable_blocks; i++) {
    cu->i_dom_list[i] = NOTVISITED;
  }

  /* For post-order, last block is entry block.  Set its i_dom to istelf */
  DCHECK_EQ(cu->entry_block->dfs_id, num_reachable_blocks-1);
  cu->i_dom_list[cu->entry_block->dfs_id] = cu->entry_block->dfs_id;

  /* Compute the immediate dominators */
  DataFlowAnalysisDispatcher(cu, ComputeblockIDom,
                                kReversePostOrderTraversal,
                                true /* is_iterative */);

  /* Set the dominator for the root node */
  ClearAllBits(cu->entry_block->dominators);
  SetBit(cu, cu->entry_block->dominators, cu->entry_block->id);

  if (cu->temp_block_v == NULL) {
    cu->temp_block_v = AllocBitVector(cu, num_total_blocks,
                                          false /* expandable */,
                                          kBitMapTmpBlockV);
  } else {
    ClearAllBits(cu->temp_block_v);
  }
  cu->entry_block->i_dom = NULL;

  /* For testing, compute sets using alternate mechanism */
  if (cu->enable_debug & (1 << kDebugVerifyDataflow)) {
    // Use alternate mechanism to compute dominators for comparison
    DataFlowAnalysisDispatcher(cu, SlowComputeBlockDominators,
                                  kPreOrderDFSTraversal,
                                  true /* is_iterative */);

   DataFlowAnalysisDispatcher(cu, SlowComputeBlockIDom,
                                 kReachableNodes,
                                 false /* is_iterative */);
  }

  DataFlowAnalysisDispatcher(cu, SetDominators,
                                kReachableNodes,
                                false /* is_iterative */);

  DataFlowAnalysisDispatcher(cu, ComputeBlockDominiators,
                                kReversePostOrderTraversal,
                                false /* is_iterative */);

  /*
   * Now go ahead and compute the post order traversal based on the
   * i_dominated sets.
   */
  if (cu->dom_post_order_traversal.elem_list == NULL) {
    CompilerInitGrowableList(cu, &cu->dom_post_order_traversal,
                        num_reachable_blocks, kListDomPostOrderTraversal);
  } else {
    cu->dom_post_order_traversal.num_used = 0;
  }

  ComputeDomPostOrderTraversal(cu, cu->entry_block);
  DCHECK_EQ(cu->dom_post_order_traversal.num_used, static_cast<unsigned>(cu->num_reachable_blocks));

  /* Now compute the dominance frontier for each block */
  DataFlowAnalysisDispatcher(cu, ComputeDominanceFrontier,
                                        kPostOrderDOMTraversal,
                                        false /* is_iterative */);
}

/*
 * Perform dest U= src1 ^ ~src2
 * This is probably not general enough to be placed in BitVector.[ch].
 */
static void ComputeSuccLineIn(ArenaBitVector* dest, const ArenaBitVector* src1,
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
static bool ComputeBlockLiveIns(CompilationUnit* cu, BasicBlock* bb)
{
  ArenaBitVector* temp_dalvik_register_v = cu->temp_dalvik_register_v;

  if (bb->data_flow_info == NULL) return false;
  CopyBitVector(temp_dalvik_register_v, bb->data_flow_info->live_in_v);
  if (bb->taken && bb->taken->data_flow_info)
    ComputeSuccLineIn(temp_dalvik_register_v, bb->taken->data_flow_info->live_in_v,
                      bb->data_flow_info->def_v);
  if (bb->fall_through && bb->fall_through->data_flow_info)
    ComputeSuccLineIn(temp_dalvik_register_v,
                      bb->fall_through->data_flow_info->live_in_v,
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
        ComputeSuccLineIn(temp_dalvik_register_v,
                          succ_bb->data_flow_info->live_in_v,
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
static void InsertPhiNodes(CompilationUnit* cu)
{
  int dalvik_reg;
  const GrowableList* block_list = &cu->block_list;
  ArenaBitVector* phi_blocks =
      AllocBitVector(cu, cu->num_blocks, false, kBitMapPhi);
  ArenaBitVector* tmp_blocks =
      AllocBitVector(cu, cu->num_blocks, false, kBitMapTmpBlocks);
  ArenaBitVector* input_blocks =
      AllocBitVector(cu, cu->num_blocks, false, kBitMapInputBlocks);

  cu->temp_dalvik_register_v =
      AllocBitVector(cu, cu->num_dalvik_registers, false,
                        kBitMapRegisterV);

  DataFlowAnalysisDispatcher(cu, ComputeBlockLiveIns,
                                kPostOrderDFSTraversal, true /* is_iterative */);

  /* Iterate through each Dalvik register */
  for (dalvik_reg = cu->num_dalvik_registers - 1; dalvik_reg >= 0; dalvik_reg--) {
    bool change;
    ArenaBitVectorIterator iterator;

    CopyBitVector(input_blocks, cu->def_block_matrix[dalvik_reg]);
    ClearAllBits(phi_blocks);

    /* Calculate the phi blocks for each Dalvik register */
    do {
      change = false;
      ClearAllBits(tmp_blocks);
      BitVectorIteratorInit(input_blocks, &iterator);

      while (true) {
        int idx = BitVectorIteratorNext(&iterator);
        if (idx == -1) break;
          BasicBlock* def_bb =
              reinterpret_cast<BasicBlock*>(GrowableListGetElement(block_list, idx));

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
          UnifyBitVetors(input_blocks, input_blocks,
                             cu->def_block_matrix[dalvik_reg]);
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
      BasicBlock* phi_bb =
          reinterpret_cast<BasicBlock*>(GrowableListGetElement(block_list, idx));
      /* Variable will be clobbered before being used - no need for phi */
      if (!IsBitSet(phi_bb->data_flow_info->live_in_v, dalvik_reg)) continue;
      MIR *phi = static_cast<MIR*>(NewMem(cu, sizeof(MIR), true, kAllocDFInfo));
      phi->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpPhi);
      phi->dalvikInsn.vA = dalvik_reg;
      phi->offset = phi_bb->start_offset;
      phi->meta.phi_next = cu->phi_list;
      cu->phi_list = phi;
      PrependMIR(phi_bb, phi);
    }
  }
}

/*
 * Worker function to insert phi-operands with latest SSA names from
 * predecessor blocks
 */
static bool InsertPhiNodeOperands(CompilationUnit* cu, BasicBlock* bb)
{
  GrowableListIterator iter;
  MIR *mir;
  std::vector<int> uses;
  std::vector<int> incoming_arc;

  /* Phi nodes are at the beginning of each block */
  for (mir = bb->first_mir_insn; mir; mir = mir->next) {
    if (mir->dalvikInsn.opcode != static_cast<Instruction::Code>(kMirOpPhi))
      return true;
    int ssa_reg = mir->ssa_rep->defs[0];
    DCHECK_GE(ssa_reg, 0);   // Shouldn't see compiler temps here
    int v_reg = SRegToVReg(cu, ssa_reg);

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
        static_cast<int*>(NewMem(cu, sizeof(int) * num_uses, false, kAllocDFInfo));
    mir->ssa_rep->fp_use =
        static_cast<bool*>(NewMem(cu, sizeof(bool) * num_uses, true, kAllocDFInfo));
    int* incoming =
        static_cast<int*>(NewMem(cu, sizeof(int) * num_uses, false, kAllocDFInfo));
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

static void DoDFSPreOrderSSARename(CompilationUnit* cu, BasicBlock* block)
{

  if (block->visited || block->hidden) return;
  block->visited = true;

  /* Process this block */
  DoSSAConversion(cu, block);
  int map_size = sizeof(int) * cu->num_dalvik_registers;

  /* Save SSA map snapshot */
  int* saved_ssa_map = static_cast<int*>(NewMem(cu, map_size, false, kAllocDalvikToSSAMap));
  memcpy(saved_ssa_map, cu->vreg_to_ssa_map, map_size);

  if (block->fall_through) {
    DoDFSPreOrderSSARename(cu, block->fall_through);
    /* Restore SSA map snapshot */
    memcpy(cu->vreg_to_ssa_map, saved_ssa_map, map_size);
  }
  if (block->taken) {
    DoDFSPreOrderSSARename(cu, block->taken);
    /* Restore SSA map snapshot */
    memcpy(cu->vreg_to_ssa_map, saved_ssa_map, map_size);
  }
  if (block->successor_block_list.block_list_type != kNotUsed) {
    GrowableListIterator iterator;
    GrowableListIteratorInit(&block->successor_block_list.blocks, &iterator);
    while (true) {
      SuccessorBlockInfo *successor_block_info =
          reinterpret_cast<SuccessorBlockInfo*>(GrowableListIteratorNext(&iterator));
      if (successor_block_info == NULL) break;
      BasicBlock* succ_bb = successor_block_info->block;
      DoDFSPreOrderSSARename(cu, succ_bb);
      /* Restore SSA map snapshot */
      memcpy(cu->vreg_to_ssa_map, saved_ssa_map, map_size);
    }
  }
  cu->vreg_to_ssa_map = saved_ssa_map;
  return;
}

/* Perform SSA transformation for the whole method */
void SSATransformation(CompilationUnit* cu)
{
  /* Compute the DFS order */
  ComputeDFSOrders(cu);

  if (!cu->disable_dataflow) {
    /* Compute the dominator info */
    ComputeDominators(cu);
  }

  /* Allocate data structures in preparation for SSA conversion */
  CompilerInitializeSSAConversion(cu);

  if (!cu->disable_dataflow) {
    /* Find out the "Dalvik reg def x block" relation */
    ComputeDefBlockMatrix(cu);

    /* Insert phi nodes to dominance frontiers for all variables */
    InsertPhiNodes(cu);
  }

  /* Rename register names by local defs and phi nodes */
  DataFlowAnalysisDispatcher(cu, ClearVisitedFlag,
                                kAllNodes, false /* is_iterative */);
  DoDFSPreOrderSSARename(cu, cu->entry_block);

  if (!cu->disable_dataflow) {
    /*
     * Shared temp bit vector used by each block to count the number of defs
     * from all the predecessor blocks.
     */
    cu->temp_ssa_register_v = AllocBitVector(cu, cu->num_ssa_regs,
         false, kBitMapTempSSARegisterV);

    cu->temp_ssa_block_id_v =
        static_cast<int*>(NewMem(cu, sizeof(int) * cu->num_ssa_regs, false, kAllocDFInfo));

    /* Insert phi-operands with latest SSA names from predecessor blocks */
    DataFlowAnalysisDispatcher(cu, InsertPhiNodeOperands,
                                  kReachableNodes, false /* is_iterative */);
  }
}

}  // namespace art
