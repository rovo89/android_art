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

static inline BasicBlock* NeedsVisit(BasicBlock* bb) {
  if (bb != NULL) {
    if (bb->visited || bb->hidden) {
      bb = NULL;
    }
  }
  return bb;
}

static BasicBlock* NextUnvisitedSuccessor(BasicBlock* bb)
{
  BasicBlock* res = NeedsVisit(bb->fallThrough);
  if (res == NULL) {
    res = NeedsVisit(bb->taken);
    if (res == NULL) {
      if (bb->successorBlockList.blockListType != kNotUsed) {
        GrowableListIterator iterator;
        GrowableListIteratorInit(&bb->successorBlockList.blocks,
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

static void MarkPreOrder(CompilationUnit* cUnit, BasicBlock* block)
{
  block->visited = true;
  /* Enqueue the preOrder block id */
  InsertGrowableList(cUnit, &cUnit->dfsOrder, block->id);
}

static void RecordDFSOrders(CompilationUnit* cUnit, BasicBlock* block)
{
  std::vector<BasicBlock*> succ;
  MarkPreOrder(cUnit, block);
  succ.push_back(block);
  while (!succ.empty()) {
    BasicBlock* curr = succ.back();
    BasicBlock* nextSuccessor = NextUnvisitedSuccessor(curr);
    if (nextSuccessor != NULL) {
      MarkPreOrder(cUnit, nextSuccessor);
      succ.push_back(nextSuccessor);
      continue;
    }
    curr->dfsId = cUnit->dfsPostOrder.numUsed;
    InsertGrowableList(cUnit, &cUnit->dfsPostOrder, curr->id);
    succ.pop_back();
  }
}

#if defined(TEST_DFS)
/* Enter the node to the dfsOrder list then visit its successors */
static void RecursiveRecordDFSOrders(CompilationUnit* cUnit, BasicBlock* block)
{

  if (block->visited || block->hidden) return;
  block->visited = true;

  // Can this block be reached only via previous block fallthrough?
  if ((block->blockType == kDalvikByteCode) &&
      (block->predecessors->numUsed == 1)) {
    DCHECK_GE(cUnit->dfsOrder.numUsed, 1U);
    int prevIdx = cUnit->dfsOrder.numUsed - 1;
    int prevId = cUnit->dfsOrder.elemList[prevIdx];
    BasicBlock* predBB = (BasicBlock*)block->predecessors->elemList[0];
  }

  /* Enqueue the preOrder block id */
  InsertGrowableList(cUnit, &cUnit->dfsOrder, block->id);

  if (block->fallThrough) {
    RecursiveRecordDFSOrders(cUnit, block->fallThrough);
  }
  if (block->taken) RecursiveRecordDFSOrders(cUnit, block->taken);
  if (block->successorBlockList.blockListType != kNotUsed) {
    GrowableListIterator iterator;
    GrowableListIteratorInit(&block->successorBlockList.blocks,
                                &iterator);
    while (true) {
      SuccessorBlockInfo *successorBlockInfo =
          (SuccessorBlockInfo *) GrowableListIteratorNext(&iterator);
      if (successorBlockInfo == NULL) break;
      BasicBlock* succBB = successorBlockInfo->block;
      RecursiveRecordDFSOrders(cUnit, succBB);
    }
  }

  /* Record postorder in basic block and enqueue normal id in dfsPostOrder */
  block->dfsId = cUnit->dfsPostOrder.numUsed;
  InsertGrowableList(cUnit, &cUnit->dfsPostOrder, block->id);
  return;
}
#endif

/* Sort the blocks by the Depth-First-Search */
static void ComputeDFSOrders(CompilationUnit* cUnit)
{
  /* Initialize or reset the DFS preOrder list */
  if (cUnit->dfsOrder.elemList == NULL) {
    CompilerInitGrowableList(cUnit, &cUnit->dfsOrder, cUnit->numBlocks,
                        kListDfsOrder);
  } else {
    /* Just reset the used length on the counter */
    cUnit->dfsOrder.numUsed = 0;
  }

  /* Initialize or reset the DFS postOrder list */
  if (cUnit->dfsPostOrder.elemList == NULL) {
    CompilerInitGrowableList(cUnit, &cUnit->dfsPostOrder, cUnit->numBlocks,
                        kListDfsPostOrder);
  } else {
    /* Just reset the used length on the counter */
    cUnit->dfsPostOrder.numUsed = 0;
  }

#if defined(TEST_DFS)
  // Reset visited flags
  DataFlowAnalysisDispatcher(cUnit, ClearVisitedFlag,
                                kAllNodes, false /* isIterative */);
  // Record pre and post order dfs
  RecursiveRecordDFSOrders(cUnit, cUnit->entryBlock);
  // Copy the results for later comparison and reset the lists
  GrowableList recursiveDfsOrder;
  GrowableList recursiveDfsPostOrder;
  CompilerInitGrowableList(cUnit, &recursiveDfsOrder, cUnit->dfsOrder.numUsed,
                      kListDfsOrder);
  for (unsigned int i = 0; i < cUnit->dfsOrder.numUsed; i++) {
    InsertGrowableList(cUnit, &recursiveDfsOrder,
                          cUnit->dfsOrder.elemList[i]);
  }
  cUnit->dfsOrder.numUsed = 0;
  CompilerInitGrowableList(cUnit, &recursiveDfsPostOrder,
                      cUnit->dfsPostOrder.numUsed, kListDfsOrder);
  for (unsigned int i = 0; i < cUnit->dfsPostOrder.numUsed; i++) {
    InsertGrowableList(cUnit, &recursiveDfsPostOrder,
                          cUnit->dfsPostOrder.elemList[i]);
  }
  cUnit->dfsPostOrder.numUsed = 0;
#endif

  // Reset visited flags from all nodes
  DataFlowAnalysisDispatcher(cUnit, ClearVisitedFlag,
                                kAllNodes, false /* isIterative */);
  // Record dfs orders
  RecordDFSOrders(cUnit, cUnit->entryBlock);

#if defined(TEST_DFS)
  bool mismatch = false;
  mismatch |= (cUnit->dfsOrder.numUsed != recursiveDfsOrder.numUsed);
  for (unsigned int i = 0; i < cUnit->dfsOrder.numUsed; i++) {
    mismatch |= (cUnit->dfsOrder.elemList[i] !=
                 recursiveDfsOrder.elemList[i]);
  }
  mismatch |= (cUnit->dfsPostOrder.numUsed != recursiveDfsPostOrder.numUsed);
  for (unsigned int i = 0; i < cUnit->dfsPostOrder.numUsed; i++) {
    mismatch |= (cUnit->dfsPostOrder.elemList[i] !=
                 recursiveDfsPostOrder.elemList[i]);
  }
  if (mismatch) {
    LOG(INFO) << "Mismatch for "
              << PrettyMethod(cUnit->method_idx, *cUnit->dex_file);
    LOG(INFO) << "New dfs";
    for (unsigned int i = 0; i < cUnit->dfsOrder.numUsed; i++) {
      LOG(INFO) << i << " - " << cUnit->dfsOrder.elemList[i];
    }
    LOG(INFO) << "Recursive dfs";
    for (unsigned int i = 0; i < recursiveDfsOrder.numUsed; i++) {
      LOG(INFO) << i << " - " << recursiveDfsOrder.elemList[i];
    }
    LOG(INFO) << "New post dfs";
    for (unsigned int i = 0; i < cUnit->dfsPostOrder.numUsed; i++) {
      LOG(INFO) << i << " - " << cUnit->dfsPostOrder.elemList[i];
    }
    LOG(INFO) << "Recursive post dfs";
    for (unsigned int i = 0; i < recursiveDfsPostOrder.numUsed; i++) {
      LOG(INFO) << i << " - " << recursiveDfsPostOrder.elemList[i];
    }
  }
  CHECK_EQ(cUnit->dfsOrder.numUsed, recursiveDfsOrder.numUsed);
  for (unsigned int i = 0; i < cUnit->dfsOrder.numUsed; i++) {
    CHECK_EQ(cUnit->dfsOrder.elemList[i], recursiveDfsOrder.elemList[i]);
  }
  CHECK_EQ(cUnit->dfsPostOrder.numUsed, recursiveDfsPostOrder.numUsed);
  for (unsigned int i = 0; i < cUnit->dfsPostOrder.numUsed; i++) {
    CHECK_EQ(cUnit->dfsPostOrder.elemList[i],
             recursiveDfsPostOrder.elemList[i]);
  }
#endif

  cUnit->numReachableBlocks = cUnit->dfsOrder.numUsed;
}

/*
 * Mark block bit on the per-Dalvik register vector to denote that Dalvik
 * register idx is defined in BasicBlock bb.
 */
static bool FillDefBlockMatrix(CompilationUnit* cUnit, BasicBlock* bb)
{
  if (bb->dataFlowInfo == NULL) return false;

  ArenaBitVectorIterator iterator;

  BitVectorIteratorInit(bb->dataFlowInfo->defV, &iterator);
  while (true) {
    int idx = BitVectorIteratorNext(&iterator);
    if (idx == -1) break;
    /* Block bb defines register idx */
    SetBit(cUnit, cUnit->defBlockMatrix[idx], bb->id);
  }
  return true;
}

static void ComputeDefBlockMatrix(CompilationUnit* cUnit)
{
  int numRegisters = cUnit->numDalvikRegisters;
  /* Allocate numDalvikRegisters bit vector pointers */
  cUnit->defBlockMatrix = static_cast<ArenaBitVector**>
      (NewMem(cUnit, sizeof(ArenaBitVector *) * numRegisters, true, kAllocDFInfo));
  int i;

  /* Initialize numRegister vectors with numBlocks bits each */
  for (i = 0; i < numRegisters; i++) {
    cUnit->defBlockMatrix[i] = AllocBitVector(cUnit, cUnit->numBlocks,
                                                 false, kBitMapBMatrix);
  }
  DataFlowAnalysisDispatcher(cUnit, FindLocalLiveIn,
                                kAllNodes, false /* isIterative */);
  DataFlowAnalysisDispatcher(cUnit, FillDefBlockMatrix,
                                kAllNodes, false /* isIterative */);

  /*
   * Also set the incoming parameters as defs in the entry block.
   * Only need to handle the parameters for the outer method.
   */
  int numRegs = cUnit->numDalvikRegisters;
  int inReg = numRegs - cUnit->numIns;
  for (; inReg < numRegs; inReg++) {
    SetBit(cUnit, cUnit->defBlockMatrix[inReg], cUnit->entryBlock->id);
  }
}

/* Compute the post-order traversal of the CFG */
static void ComputeDomPostOrderTraversal(CompilationUnit* cUnit, BasicBlock* bb)
{
  ArenaBitVectorIterator bvIterator;
  BitVectorIteratorInit(bb->iDominated, &bvIterator);
  GrowableList* blockList = &cUnit->blockList;

  /* Iterate through the dominated blocks first */
  while (true) {
    //TUNING: hot call to BitVectorIteratorNext
    int bbIdx = BitVectorIteratorNext(&bvIterator);
    if (bbIdx == -1) break;
    BasicBlock* dominatedBB =
        reinterpret_cast<BasicBlock*>(GrowableListGetElement(blockList, bbIdx));
    ComputeDomPostOrderTraversal(cUnit, dominatedBB);
  }

  /* Enter the current block id */
  InsertGrowableList(cUnit, &cUnit->domPostOrderTraversal, bb->id);

  /* hacky loop detection */
  if (bb->taken && IsBitSet(bb->dominators, bb->taken->id)) {
    cUnit->hasLoop = true;
  }
}

static void CheckForDominanceFrontier(CompilationUnit* cUnit, BasicBlock* domBB,
                                      const BasicBlock* succBB)
{
  /*
   * TODO - evaluate whether phi will ever need to be inserted into exit
   * blocks.
   */
  if (succBB->iDom != domBB &&
    succBB->blockType == kDalvikByteCode &&
    succBB->hidden == false) {
    SetBit(cUnit, domBB->domFrontier, succBB->id);
  }
}

/* Worker function to compute the dominance frontier */
static bool ComputeDominanceFrontier(CompilationUnit* cUnit, BasicBlock* bb)
{
  GrowableList* blockList = &cUnit->blockList;

  /* Calculate DF_local */
  if (bb->taken) {
    CheckForDominanceFrontier(cUnit, bb, bb->taken);
  }
  if (bb->fallThrough) {
    CheckForDominanceFrontier(cUnit, bb, bb->fallThrough);
  }
  if (bb->successorBlockList.blockListType != kNotUsed) {
    GrowableListIterator iterator;
    GrowableListIteratorInit(&bb->successorBlockList.blocks,
                                  &iterator);
      while (true) {
        SuccessorBlockInfo *successorBlockInfo =
            reinterpret_cast<SuccessorBlockInfo*>(GrowableListIteratorNext(&iterator));
        if (successorBlockInfo == NULL) break;
        BasicBlock* succBB = successorBlockInfo->block;
        CheckForDominanceFrontier(cUnit, bb, succBB);
      }
  }

  /* Calculate DF_up */
  ArenaBitVectorIterator bvIterator;
  BitVectorIteratorInit(bb->iDominated, &bvIterator);
  while (true) {
    //TUNING: hot call to BitVectorIteratorNext
    int dominatedIdx = BitVectorIteratorNext(&bvIterator);
    if (dominatedIdx == -1) break;
    BasicBlock* dominatedBB =
        reinterpret_cast<BasicBlock*>(GrowableListGetElement(blockList, dominatedIdx));
    ArenaBitVectorIterator dfIterator;
    BitVectorIteratorInit(dominatedBB->domFrontier, &dfIterator);
    while (true) {
      //TUNING: hot call to BitVectorIteratorNext
      int dfUpIdx = BitVectorIteratorNext(&dfIterator);
      if (dfUpIdx == -1) break;
      BasicBlock* dfUpBlock =
          reinterpret_cast<BasicBlock*>( GrowableListGetElement(blockList, dfUpIdx));
      CheckForDominanceFrontier(cUnit, bb, dfUpBlock);
    }
  }

  return true;
}

/* Worker function for initializing domination-related data structures */
static bool InitializeDominationInfo(CompilationUnit* cUnit, BasicBlock* bb)
{
  int numTotalBlocks = cUnit->blockList.numUsed;

  if (bb->dominators == NULL ) {
    bb->dominators = AllocBitVector(cUnit, numTotalBlocks,
                                       false /* expandable */,
                                       kBitMapDominators);
    bb->iDominated = AllocBitVector(cUnit, numTotalBlocks,
                                       false /* expandable */,
                                       kBitMapIDominated);
    bb->domFrontier = AllocBitVector(cUnit, numTotalBlocks,
                                        false /* expandable */,
                                        kBitMapDomFrontier);
  } else {
    ClearAllBits(bb->dominators);
    ClearAllBits(bb->iDominated);
    ClearAllBits(bb->domFrontier);
  }
  /* Set all bits in the dominator vector */
  SetInitialBits(bb->dominators, numTotalBlocks);

  return true;
}

/*
 * Worker function to compute each block's dominators.  This implementation
 * is only used when kDebugVerifyDataflow is active and should compute
 * the same dominator sets as ComputeBlockDominiators.
 */
static bool SlowComputeBlockDominators(CompilationUnit* cUnit, BasicBlock* bb)
{
  GrowableList* blockList = &cUnit->blockList;
  int numTotalBlocks = blockList->numUsed;
  ArenaBitVector* tempBlockV = cUnit->tempBlockV;
  GrowableListIterator iter;

  /*
   * The dominator of the entry block has been preset to itself and we need
   * to skip the calculation here.
   */
  if (bb == cUnit->entryBlock) return false;

  SetInitialBits(tempBlockV, numTotalBlocks);

  /* Iterate through the predecessors */
  GrowableListIteratorInit(bb->predecessors, &iter);
  while (true) {
    BasicBlock* predBB = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iter));
    if (!predBB) break;
    /* tempBlockV = tempBlockV ^ dominators */
    if (predBB->dominators != NULL) {
      IntersectBitVectors(tempBlockV, tempBlockV, predBB->dominators);
    }
  }
  SetBit(cUnit, tempBlockV, bb->id);
  if (CompareBitVectors(tempBlockV, bb->dominators)) {
    CopyBitVector(bb->dominators, tempBlockV);
    return true;
  }
  return false;
}

/*
 * Worker function to compute the idom.  This implementation is only
 * used when kDebugVerifyDataflow is active and should compute the
 * same iDom as ComputeblockIDom.
 */
static bool SlowComputeBlockIDom(CompilationUnit* cUnit, BasicBlock* bb)
{
  GrowableList* blockList = &cUnit->blockList;
  ArenaBitVector* tempBlockV = cUnit->tempBlockV;
  ArenaBitVectorIterator bvIterator;
  BasicBlock* iDom;

  if (bb == cUnit->entryBlock) return false;

  CopyBitVector(tempBlockV, bb->dominators);
  ClearBit(tempBlockV, bb->id);
  BitVectorIteratorInit(tempBlockV, &bvIterator);

  /* Should not see any dead block */
  DCHECK_NE(CountSetBits(tempBlockV),  0);
  if (CountSetBits(tempBlockV) == 1) {
    iDom = reinterpret_cast<BasicBlock*>
        (GrowableListGetElement(blockList, BitVectorIteratorNext(&bvIterator)));
    bb->iDom = iDom;
  } else {
    int iDomIdx = BitVectorIteratorNext(&bvIterator);
    DCHECK_NE(iDomIdx, -1);
    while (true) {
      int nextDom = BitVectorIteratorNext(&bvIterator);
      if (nextDom == -1) break;
      BasicBlock* nextDomBB =
          reinterpret_cast<BasicBlock*>(GrowableListGetElement(blockList, nextDom));
      /* iDom dominates nextDom - set new iDom */
      if (IsBitSet(nextDomBB->dominators, iDomIdx)) {
          iDomIdx = nextDom;
      }

    }
    iDom = reinterpret_cast<BasicBlock*>(GrowableListGetElement(blockList, iDomIdx));
    /* Set the immediate dominator block for bb */
    bb->iDom = iDom;
  }
  /* Add bb to the iDominated set of the immediate dominator block */
  SetBit(cUnit, iDom->iDominated, bb->id);
  return true;
}

/*
 * Walk through the ordered iDom list until we reach common parent.
 * Given the ordering of iDomList, this common parent represents the
 * last element of the intersection of block1 and block2 dominators.
  */
static int FindCommonParent(CompilationUnit *cUnit, int block1, int block2)
{
  while (block1 != block2) {
    while (block1 < block2) {
      block1 = cUnit->iDomList[block1];
      DCHECK_NE(block1, NOTVISITED);
    }
    while (block2 < block1) {
      block2 = cUnit->iDomList[block2];
      DCHECK_NE(block2, NOTVISITED);
    }
  }
  return block1;
}

/* Worker function to compute each block's immediate dominator */
static bool ComputeblockIDom(CompilationUnit* cUnit, BasicBlock* bb)
{
  GrowableListIterator iter;
  int idom = -1;

  /* Special-case entry block */
  if (bb == cUnit->entryBlock) {
    return false;
  }

  /* Iterate through the predecessors */
  GrowableListIteratorInit(bb->predecessors, &iter);

  /* Find the first processed predecessor */
  while (true) {
    BasicBlock* predBB = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iter));
    CHECK(predBB != NULL);
    if (cUnit->iDomList[predBB->dfsId] != NOTVISITED) {
      idom = predBB->dfsId;
      break;
    }
  }

  /* Scan the rest of the predecessors */
  while (true) {
      BasicBlock* predBB = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iter));
      if (!predBB) break;
      if (cUnit->iDomList[predBB->dfsId] == NOTVISITED) {
        continue;
      } else {
        idom = FindCommonParent(cUnit, predBB->dfsId, idom);
      }
  }

  DCHECK_NE(idom, NOTVISITED);

  /* Did something change? */
  if (cUnit->iDomList[bb->dfsId] != idom) {
    cUnit->iDomList[bb->dfsId] = idom;
    return true;
  }
  return false;
}

/* Worker function to compute each block's domintors */
static bool ComputeBlockDominiators(CompilationUnit* cUnit, BasicBlock* bb)
{
  if (bb == cUnit->entryBlock) {
    ClearAllBits(bb->dominators);
  } else {
    CopyBitVector(bb->dominators, bb->iDom->dominators);
  }
  SetBit(cUnit, bb->dominators, bb->id);
  return false;
}

static bool SetDominators(CompilationUnit* cUnit, BasicBlock* bb)
{
  if (bb != cUnit->entryBlock) {
    int iDomDFSIdx = cUnit->iDomList[bb->dfsId];
    DCHECK_NE(iDomDFSIdx, NOTVISITED);
    int iDomIdx = cUnit->dfsPostOrder.elemList[iDomDFSIdx];
    BasicBlock* iDom =
        reinterpret_cast<BasicBlock*>(GrowableListGetElement(&cUnit->blockList, iDomIdx));
    if (cUnit->enableDebug & (1 << kDebugVerifyDataflow)) {
      DCHECK_EQ(bb->iDom->id, iDom->id);
    }
    bb->iDom = iDom;
    /* Add bb to the iDominated set of the immediate dominator block */
    SetBit(cUnit, iDom->iDominated, bb->id);
  }
  return false;
}

/* Compute dominators, immediate dominator, and dominance fronter */
static void ComputeDominators(CompilationUnit* cUnit)
{
  int numReachableBlocks = cUnit->numReachableBlocks;
  int numTotalBlocks = cUnit->blockList.numUsed;

  /* Initialize domination-related data structures */
  DataFlowAnalysisDispatcher(cUnit, InitializeDominationInfo,
                                kReachableNodes, false /* isIterative */);

  /* Initalize & Clear iDomList */
  if (cUnit->iDomList == NULL) {
    cUnit->iDomList = static_cast<int*>(NewMem(cUnit, sizeof(int) * numReachableBlocks,
                                               false, kAllocDFInfo));
  }
  for (int i = 0; i < numReachableBlocks; i++) {
    cUnit->iDomList[i] = NOTVISITED;
  }

  /* For post-order, last block is entry block.  Set its iDom to istelf */
  DCHECK_EQ(cUnit->entryBlock->dfsId, numReachableBlocks-1);
  cUnit->iDomList[cUnit->entryBlock->dfsId] = cUnit->entryBlock->dfsId;

  /* Compute the immediate dominators */
  DataFlowAnalysisDispatcher(cUnit, ComputeblockIDom,
                                kReversePostOrderTraversal,
                                true /* isIterative */);

  /* Set the dominator for the root node */
  ClearAllBits(cUnit->entryBlock->dominators);
  SetBit(cUnit, cUnit->entryBlock->dominators, cUnit->entryBlock->id);

  if (cUnit->tempBlockV == NULL) {
    cUnit->tempBlockV = AllocBitVector(cUnit, numTotalBlocks,
                                          false /* expandable */,
                                          kBitMapTmpBlockV);
  } else {
    ClearAllBits(cUnit->tempBlockV);
  }
  cUnit->entryBlock->iDom = NULL;

  /* For testing, compute sets using alternate mechanism */
  if (cUnit->enableDebug & (1 << kDebugVerifyDataflow)) {
    // Use alternate mechanism to compute dominators for comparison
    DataFlowAnalysisDispatcher(cUnit, SlowComputeBlockDominators,
                                  kPreOrderDFSTraversal,
                                  true /* isIterative */);

   DataFlowAnalysisDispatcher(cUnit, SlowComputeBlockIDom,
                                 kReachableNodes,
                                 false /* isIterative */);
  }

  DataFlowAnalysisDispatcher(cUnit, SetDominators,
                                kReachableNodes,
                                false /* isIterative */);

  DataFlowAnalysisDispatcher(cUnit, ComputeBlockDominiators,
                                kReversePostOrderTraversal,
                                false /* isIterative */);

  /*
   * Now go ahead and compute the post order traversal based on the
   * iDominated sets.
   */
  if (cUnit->domPostOrderTraversal.elemList == NULL) {
    CompilerInitGrowableList(cUnit, &cUnit->domPostOrderTraversal,
                        numReachableBlocks, kListDomPostOrderTraversal);
  } else {
    cUnit->domPostOrderTraversal.numUsed = 0;
  }

  ComputeDomPostOrderTraversal(cUnit, cUnit->entryBlock);
  DCHECK_EQ(cUnit->domPostOrderTraversal.numUsed, static_cast<unsigned>(cUnit->numReachableBlocks));

  /* Now compute the dominance frontier for each block */
  DataFlowAnalysisDispatcher(cUnit, ComputeDominanceFrontier,
                                        kPostOrderDOMTraversal,
                                        false /* isIterative */);
}

/*
 * Perform dest U= src1 ^ ~src2
 * This is probably not general enough to be placed in BitVector.[ch].
 */
static void ComputeSuccLineIn(ArenaBitVector* dest, const ArenaBitVector* src1,
                              const ArenaBitVector* src2)
{
  if (dest->storageSize != src1->storageSize ||
    dest->storageSize != src2->storageSize ||
    dest->expandable != src1->expandable ||
    dest->expandable != src2->expandable) {
    LOG(FATAL) << "Incompatible set properties";
  }

  unsigned int idx;
  for (idx = 0; idx < dest->storageSize; idx++) {
    dest->storage[idx] |= src1->storage[idx] & ~src2->storage[idx];
  }
}

/*
 * Iterate through all successor blocks and propagate up the live-in sets.
 * The calculated result is used for phi-node pruning - where we only need to
 * insert a phi node if the variable is live-in to the block.
 */
static bool ComputeBlockLiveIns(CompilationUnit* cUnit, BasicBlock* bb)
{
  ArenaBitVector* tempDalvikRegisterV = cUnit->tempDalvikRegisterV;

  if (bb->dataFlowInfo == NULL) return false;
  CopyBitVector(tempDalvikRegisterV, bb->dataFlowInfo->liveInV);
  if (bb->taken && bb->taken->dataFlowInfo)
    ComputeSuccLineIn(tempDalvikRegisterV, bb->taken->dataFlowInfo->liveInV,
                      bb->dataFlowInfo->defV);
  if (bb->fallThrough && bb->fallThrough->dataFlowInfo)
    ComputeSuccLineIn(tempDalvikRegisterV,
                      bb->fallThrough->dataFlowInfo->liveInV,
                      bb->dataFlowInfo->defV);
  if (bb->successorBlockList.blockListType != kNotUsed) {
    GrowableListIterator iterator;
    GrowableListIteratorInit(&bb->successorBlockList.blocks,
                                &iterator);
    while (true) {
      SuccessorBlockInfo *successorBlockInfo =
          reinterpret_cast<SuccessorBlockInfo*>(GrowableListIteratorNext(&iterator));
      if (successorBlockInfo == NULL) break;
      BasicBlock* succBB = successorBlockInfo->block;
      if (succBB->dataFlowInfo) {
        ComputeSuccLineIn(tempDalvikRegisterV,
                          succBB->dataFlowInfo->liveInV,
                          bb->dataFlowInfo->defV);
      }
    }
  }
  if (CompareBitVectors(tempDalvikRegisterV, bb->dataFlowInfo->liveInV)) {
    CopyBitVector(bb->dataFlowInfo->liveInV, tempDalvikRegisterV);
    return true;
  }
  return false;
}

/* Insert phi nodes to for each variable to the dominance frontiers */
static void InsertPhiNodes(CompilationUnit* cUnit)
{
  int dalvikReg;
  const GrowableList* blockList = &cUnit->blockList;
  ArenaBitVector* phiBlocks =
      AllocBitVector(cUnit, cUnit->numBlocks, false, kBitMapPhi);
  ArenaBitVector* tmpBlocks =
      AllocBitVector(cUnit, cUnit->numBlocks, false, kBitMapTmpBlocks);
  ArenaBitVector* inputBlocks =
      AllocBitVector(cUnit, cUnit->numBlocks, false, kBitMapInputBlocks);

  cUnit->tempDalvikRegisterV =
      AllocBitVector(cUnit, cUnit->numDalvikRegisters, false,
                        kBitMapRegisterV);

  DataFlowAnalysisDispatcher(cUnit, ComputeBlockLiveIns,
                                kPostOrderDFSTraversal, true /* isIterative */);

  /* Iterate through each Dalvik register */
  for (dalvikReg = cUnit->numDalvikRegisters - 1; dalvikReg >= 0; dalvikReg--) {
    bool change;
    ArenaBitVectorIterator iterator;

    CopyBitVector(inputBlocks, cUnit->defBlockMatrix[dalvikReg]);
    ClearAllBits(phiBlocks);

    /* Calculate the phi blocks for each Dalvik register */
    do {
      change = false;
      ClearAllBits(tmpBlocks);
      BitVectorIteratorInit(inputBlocks, &iterator);

      while (true) {
        int idx = BitVectorIteratorNext(&iterator);
        if (idx == -1) break;
          BasicBlock* defBB =
              reinterpret_cast<BasicBlock*>(GrowableListGetElement(blockList, idx));

          /* Merge the dominance frontier to tmpBlocks */
          //TUNING: hot call to UnifyBitVetors
          if (defBB->domFrontier != NULL) {
            UnifyBitVetors(tmpBlocks, tmpBlocks, defBB->domFrontier);
          }
        }
        if (CompareBitVectors(phiBlocks, tmpBlocks)) {
          change = true;
          CopyBitVector(phiBlocks, tmpBlocks);

          /*
           * Iterate through the original blocks plus the new ones in
           * the dominance frontier.
           */
          CopyBitVector(inputBlocks, phiBlocks);
          UnifyBitVetors(inputBlocks, inputBlocks,
                             cUnit->defBlockMatrix[dalvikReg]);
      }
    } while (change);

    /*
     * Insert a phi node for dalvikReg in the phiBlocks if the Dalvik
     * register is in the live-in set.
     */
    BitVectorIteratorInit(phiBlocks, &iterator);
    while (true) {
      int idx = BitVectorIteratorNext(&iterator);
      if (idx == -1) break;
      BasicBlock* phiBB =
          reinterpret_cast<BasicBlock*>(GrowableListGetElement(blockList, idx));
      /* Variable will be clobbered before being used - no need for phi */
      if (!IsBitSet(phiBB->dataFlowInfo->liveInV, dalvikReg)) continue;
      MIR *phi = static_cast<MIR*>(NewMem(cUnit, sizeof(MIR), true, kAllocDFInfo));
      phi->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpPhi);
      phi->dalvikInsn.vA = dalvikReg;
      phi->offset = phiBB->startOffset;
      phi->meta.phiNext = cUnit->phiList;
      cUnit->phiList = phi;
      PrependMIR(phiBB, phi);
    }
  }
}

/*
 * Worker function to insert phi-operands with latest SSA names from
 * predecessor blocks
 */
static bool InsertPhiNodeOperands(CompilationUnit* cUnit, BasicBlock* bb)
{
  GrowableListIterator iter;
  MIR *mir;
  std::vector<int> uses;
  std::vector<int> incomingArc;

  /* Phi nodes are at the beginning of each block */
  for (mir = bb->firstMIRInsn; mir; mir = mir->next) {
    if (mir->dalvikInsn.opcode != static_cast<Instruction::Code>(kMirOpPhi))
      return true;
    int ssaReg = mir->ssaRep->defs[0];
    DCHECK_GE(ssaReg, 0);   // Shouldn't see compiler temps here
    int vReg = SRegToVReg(cUnit, ssaReg);

    uses.clear();
    incomingArc.clear();

    /* Iterate through the predecessors */
    GrowableListIteratorInit(bb->predecessors, &iter);
    while (true) {
      BasicBlock* predBB =
         reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iter));
      if (!predBB) break;
      int ssaReg = predBB->dataFlowInfo->vRegToSSAMap[vReg];
      uses.push_back(ssaReg);
      incomingArc.push_back(predBB->id);
    }

    /* Count the number of SSA registers for a Dalvik register */
    int numUses = uses.size();
    mir->ssaRep->numUses = numUses;
    mir->ssaRep->uses =
        static_cast<int*>(NewMem(cUnit, sizeof(int) * numUses, false, kAllocDFInfo));
    mir->ssaRep->fpUse =
        static_cast<bool*>(NewMem(cUnit, sizeof(bool) * numUses, true, kAllocDFInfo));
    int* incoming =
        static_cast<int*>(NewMem(cUnit, sizeof(int) * numUses, false, kAllocDFInfo));
    // TODO: Ugly, rework (but don't burden each MIR/LIR for Phi-only needs)
    mir->dalvikInsn.vB = reinterpret_cast<uintptr_t>(incoming);

    /* Set the uses array for the phi node */
    int *usePtr = mir->ssaRep->uses;
    for (int i = 0; i < numUses; i++) {
      *usePtr++ = uses[i];
      *incoming++ = incomingArc[i];
    }
  }

  return true;
}

static void DoDFSPreOrderSSARename(CompilationUnit* cUnit, BasicBlock* block)
{

  if (block->visited || block->hidden) return;
  block->visited = true;

  /* Process this block */
  DoSSAConversion(cUnit, block);
  int mapSize = sizeof(int) * cUnit->numDalvikRegisters;

  /* Save SSA map snapshot */
  int* savedSSAMap = static_cast<int*>(NewMem(cUnit, mapSize, false, kAllocDalvikToSSAMap));
  memcpy(savedSSAMap, cUnit->vRegToSSAMap, mapSize);

  if (block->fallThrough) {
    DoDFSPreOrderSSARename(cUnit, block->fallThrough);
    /* Restore SSA map snapshot */
    memcpy(cUnit->vRegToSSAMap, savedSSAMap, mapSize);
  }
  if (block->taken) {
    DoDFSPreOrderSSARename(cUnit, block->taken);
    /* Restore SSA map snapshot */
    memcpy(cUnit->vRegToSSAMap, savedSSAMap, mapSize);
  }
  if (block->successorBlockList.blockListType != kNotUsed) {
    GrowableListIterator iterator;
    GrowableListIteratorInit(&block->successorBlockList.blocks, &iterator);
    while (true) {
      SuccessorBlockInfo *successorBlockInfo =
          reinterpret_cast<SuccessorBlockInfo*>(GrowableListIteratorNext(&iterator));
      if (successorBlockInfo == NULL) break;
      BasicBlock* succBB = successorBlockInfo->block;
      DoDFSPreOrderSSARename(cUnit, succBB);
      /* Restore SSA map snapshot */
      memcpy(cUnit->vRegToSSAMap, savedSSAMap, mapSize);
    }
  }
  cUnit->vRegToSSAMap = savedSSAMap;
  return;
}

/* Perform SSA transformation for the whole method */
void SSATransformation(CompilationUnit* cUnit)
{
  /* Compute the DFS order */
  ComputeDFSOrders(cUnit);

  if (!cUnit->disableDataflow) {
    /* Compute the dominator info */
    ComputeDominators(cUnit);
  }

  /* Allocate data structures in preparation for SSA conversion */
  CompilerInitializeSSAConversion(cUnit);

  if (!cUnit->disableDataflow) {
    /* Find out the "Dalvik reg def x block" relation */
    ComputeDefBlockMatrix(cUnit);

    /* Insert phi nodes to dominance frontiers for all variables */
    InsertPhiNodes(cUnit);
  }

  /* Rename register names by local defs and phi nodes */
  DataFlowAnalysisDispatcher(cUnit, ClearVisitedFlag,
                                kAllNodes, false /* isIterative */);
  DoDFSPreOrderSSARename(cUnit, cUnit->entryBlock);

  if (!cUnit->disableDataflow) {
    /*
     * Shared temp bit vector used by each block to count the number of defs
     * from all the predecessor blocks.
     */
    cUnit->tempSSARegisterV = AllocBitVector(cUnit, cUnit->numSSARegs,
         false, kBitMapTempSSARegisterV);

    cUnit->tempSSABlockIdV =
        static_cast<int*>(NewMem(cUnit, sizeof(int) * cUnit->numSSARegs, false, kAllocDFInfo));

    /* Insert phi-operands with latest SSA names from predecessor blocks */
    DataFlowAnalysisDispatcher(cUnit, InsertPhiNodeOperands,
                                  kReachableNodes, false /* isIterative */);
  }
}

}  // namespace art
