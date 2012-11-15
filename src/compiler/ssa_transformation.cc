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

#include "dalvik.h"
#include "dataflow.h"

namespace art {

// Make sure iterative dfs recording matches old recursive version
//#define TEST_DFS

inline BasicBlock* needsVisit(BasicBlock* bb) {
  if (bb != NULL) {
    if (bb->visited || bb->hidden) {
      bb = NULL;
    }
  }
  return bb;
}

BasicBlock* nextUnvisitedSuccessor(BasicBlock* bb)
{
  BasicBlock* res = needsVisit(bb->fallThrough);
  if (res == NULL) {
    res = needsVisit(bb->taken);
    if (res == NULL) {
      if (bb->successorBlockList.blockListType != kNotUsed) {
        GrowableListIterator iterator;
        oatGrowableListIteratorInit(&bb->successorBlockList.blocks,
                                    &iterator);
        while (true) {
          SuccessorBlockInfo *sbi = (SuccessorBlockInfo*)
              oatGrowableListIteratorNext(&iterator);
          if (sbi == NULL) break;
          res = needsVisit(sbi->block);
          if (res != NULL) break;
        }
      }
    }
  }
  return res;
}

void markPreOrder(CompilationUnit* cUnit, BasicBlock* block)
{
  block->visited = true;
  /* Enqueue the preOrder block id */
  oatInsertGrowableList(cUnit, &cUnit->dfsOrder, block->id);
}

void recordDFSOrders(CompilationUnit* cUnit, BasicBlock* block)
{
  std::vector<BasicBlock*> succ;
  markPreOrder(cUnit, block);
  succ.push_back(block);
  while (!succ.empty()) {
    BasicBlock* curr = succ.back();
    BasicBlock* nextSuccessor = nextUnvisitedSuccessor(curr);
    if (nextSuccessor != NULL) {
      markPreOrder(cUnit, nextSuccessor);
      succ.push_back(nextSuccessor);
      continue;
    }
    curr->dfsId = cUnit->dfsPostOrder.numUsed;
    oatInsertGrowableList(cUnit, &cUnit->dfsPostOrder, curr->id);
    succ.pop_back();
  }
}

#if defined(TEST_DFS)
/* Enter the node to the dfsOrder list then visit its successors */
void recursiveRecordDFSOrders(CompilationUnit* cUnit, BasicBlock* block)
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
  oatInsertGrowableList(cUnit, &cUnit->dfsOrder, block->id);

  if (block->fallThrough) {
    recursiveRecordDFSOrders(cUnit, block->fallThrough);
  }
  if (block->taken) recursiveRecordDFSOrders(cUnit, block->taken);
  if (block->successorBlockList.blockListType != kNotUsed) {
    GrowableListIterator iterator;
    oatGrowableListIteratorInit(&block->successorBlockList.blocks,
                                &iterator);
    while (true) {
      SuccessorBlockInfo *successorBlockInfo =
          (SuccessorBlockInfo *) oatGrowableListIteratorNext(&iterator);
      if (successorBlockInfo == NULL) break;
      BasicBlock* succBB = successorBlockInfo->block;
      recursiveRecordDFSOrders(cUnit, succBB);
    }
  }

  /* Record postorder in basic block and enqueue normal id in dfsPostOrder */
  block->dfsId = cUnit->dfsPostOrder.numUsed;
  oatInsertGrowableList(cUnit, &cUnit->dfsPostOrder, block->id);
  return;
}
#endif

/* Sort the blocks by the Depth-First-Search */
void computeDFSOrders(CompilationUnit* cUnit)
{
  /* Initialize or reset the DFS preOrder list */
  if (cUnit->dfsOrder.elemList == NULL) {
    oatInitGrowableList(cUnit, &cUnit->dfsOrder, cUnit->numBlocks,
                        kListDfsOrder);
  } else {
    /* Just reset the used length on the counter */
    cUnit->dfsOrder.numUsed = 0;
  }

  /* Initialize or reset the DFS postOrder list */
  if (cUnit->dfsPostOrder.elemList == NULL) {
    oatInitGrowableList(cUnit, &cUnit->dfsPostOrder, cUnit->numBlocks,
                        kListDfsPostOrder);
  } else {
    /* Just reset the used length on the counter */
    cUnit->dfsPostOrder.numUsed = 0;
  }

#if defined(TEST_DFS)
  // Reset visited flags
  oatDataFlowAnalysisDispatcher(cUnit, oatClearVisitedFlag,
                                kAllNodes, false /* isIterative */);
  // Record pre and post order dfs
  recursiveRecordDFSOrders(cUnit, cUnit->entryBlock);
  // Copy the results for later comparison and reset the lists
  GrowableList recursiveDfsOrder;
  GrowableList recursiveDfsPostOrder;
  oatInitGrowableList(cUnit, &recursiveDfsOrder, cUnit->dfsOrder.numUsed,
                      kListDfsOrder);
  for (unsigned int i = 0; i < cUnit->dfsOrder.numUsed; i++) {
    oatInsertGrowableList(cUnit, &recursiveDfsOrder,
                          cUnit->dfsOrder.elemList[i]);
  }
  cUnit->dfsOrder.numUsed = 0;
  oatInitGrowableList(cUnit, &recursiveDfsPostOrder,
                      cUnit->dfsPostOrder.numUsed, kListDfsOrder);
  for (unsigned int i = 0; i < cUnit->dfsPostOrder.numUsed; i++) {
    oatInsertGrowableList(cUnit, &recursiveDfsPostOrder,
                          cUnit->dfsPostOrder.elemList[i]);
  }
  cUnit->dfsPostOrder.numUsed = 0;
#endif

  // Reset visited flags from all nodes
  oatDataFlowAnalysisDispatcher(cUnit, oatClearVisitedFlag,
                                kAllNodes, false /* isIterative */);
  // Record dfs orders
  recordDFSOrders(cUnit, cUnit->entryBlock);

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
bool fillDefBlockMatrix(CompilationUnit* cUnit, BasicBlock* bb)
{
  if (bb->dataFlowInfo == NULL) return false;

  ArenaBitVectorIterator iterator;

  oatBitVectorIteratorInit(bb->dataFlowInfo->defV, &iterator);
  while (true) {
    int idx = oatBitVectorIteratorNext(&iterator);
    if (idx == -1) break;
    /* Block bb defines register idx */
    oatSetBit(cUnit, cUnit->defBlockMatrix[idx], bb->id);
  }
  return true;
}

void computeDefBlockMatrix(CompilationUnit* cUnit)
{
  int numRegisters = cUnit->numDalvikRegisters;
  /* Allocate numDalvikRegisters bit vector pointers */
  cUnit->defBlockMatrix = (ArenaBitVector **)
      oatNew(cUnit, sizeof(ArenaBitVector *) * numRegisters, true,
             kAllocDFInfo);
  int i;

  /* Initialize numRegister vectors with numBlocks bits each */
  for (i = 0; i < numRegisters; i++) {
    cUnit->defBlockMatrix[i] = oatAllocBitVector(cUnit, cUnit->numBlocks,
                                                 false, kBitMapBMatrix);
  }
  oatDataFlowAnalysisDispatcher(cUnit, oatFindLocalLiveIn,
                                kAllNodes, false /* isIterative */);
  oatDataFlowAnalysisDispatcher(cUnit, fillDefBlockMatrix,
                                kAllNodes, false /* isIterative */);

  /*
   * Also set the incoming parameters as defs in the entry block.
   * Only need to handle the parameters for the outer method.
   */
  int numRegs = cUnit->numDalvikRegisters;
  int inReg = numRegs - cUnit->numIns;
  for (; inReg < numRegs; inReg++) {
    oatSetBit(cUnit, cUnit->defBlockMatrix[inReg], cUnit->entryBlock->id);
  }
}

/* Compute the post-order traversal of the CFG */
void computeDomPostOrderTraversal(CompilationUnit* cUnit, BasicBlock* bb)
{
  ArenaBitVectorIterator bvIterator;
  oatBitVectorIteratorInit(bb->iDominated, &bvIterator);
  GrowableList* blockList = &cUnit->blockList;

  /* Iterate through the dominated blocks first */
  while (true) {
    //TUNING: hot call to oatBitVectorIteratorNext
    int bbIdx = oatBitVectorIteratorNext(&bvIterator);
    if (bbIdx == -1) break;
    BasicBlock* dominatedBB =
        (BasicBlock* ) oatGrowableListGetElement(blockList, bbIdx);
    computeDomPostOrderTraversal(cUnit, dominatedBB);
  }

  /* Enter the current block id */
  oatInsertGrowableList(cUnit, &cUnit->domPostOrderTraversal, bb->id);

  /* hacky loop detection */
  if (bb->taken && oatIsBitSet(bb->dominators, bb->taken->id)) {
    cUnit->hasLoop = true;
  }
}

void checkForDominanceFrontier(CompilationUnit* cUnit, BasicBlock* domBB,
                             const BasicBlock* succBB)
{
  /*
   * TODO - evaluate whether phi will ever need to be inserted into exit
   * blocks.
   */
  if (succBB->iDom != domBB &&
    succBB->blockType == kDalvikByteCode &&
    succBB->hidden == false) {
    oatSetBit(cUnit, domBB->domFrontier, succBB->id);
  }
}

/* Worker function to compute the dominance frontier */
bool computeDominanceFrontier(CompilationUnit* cUnit, BasicBlock* bb)
{
  GrowableList* blockList = &cUnit->blockList;

  /* Calculate DF_local */
  if (bb->taken) {
    checkForDominanceFrontier(cUnit, bb, bb->taken);
  }
  if (bb->fallThrough) {
    checkForDominanceFrontier(cUnit, bb, bb->fallThrough);
  }
  if (bb->successorBlockList.blockListType != kNotUsed) {
    GrowableListIterator iterator;
    oatGrowableListIteratorInit(&bb->successorBlockList.blocks,
                                  &iterator);
      while (true) {
        SuccessorBlockInfo *successorBlockInfo =
            (SuccessorBlockInfo *) oatGrowableListIteratorNext(&iterator);
        if (successorBlockInfo == NULL) break;
        BasicBlock* succBB = successorBlockInfo->block;
        checkForDominanceFrontier(cUnit, bb, succBB);
      }
  }

  /* Calculate DF_up */
  ArenaBitVectorIterator bvIterator;
  oatBitVectorIteratorInit(bb->iDominated, &bvIterator);
  while (true) {
    //TUNING: hot call to oatBitVectorIteratorNext
    int dominatedIdx = oatBitVectorIteratorNext(&bvIterator);
    if (dominatedIdx == -1) break;
    BasicBlock* dominatedBB = (BasicBlock* )
        oatGrowableListGetElement(blockList, dominatedIdx);
    ArenaBitVectorIterator dfIterator;
    oatBitVectorIteratorInit(dominatedBB->domFrontier, &dfIterator);
    while (true) {
      //TUNING: hot call to oatBitVectorIteratorNext
      int dfUpIdx = oatBitVectorIteratorNext(&dfIterator);
      if (dfUpIdx == -1) break;
      BasicBlock* dfUpBlock = (BasicBlock* )
          oatGrowableListGetElement(blockList, dfUpIdx);
      checkForDominanceFrontier(cUnit, bb, dfUpBlock);
    }
  }

  return true;
}

/* Worker function for initializing domination-related data structures */
bool initializeDominationInfo(CompilationUnit* cUnit, BasicBlock* bb)
{
  int numTotalBlocks = cUnit->blockList.numUsed;

  if (bb->dominators == NULL ) {
    bb->dominators = oatAllocBitVector(cUnit, numTotalBlocks,
                                       false /* expandable */,
                                       kBitMapDominators);
    bb->iDominated = oatAllocBitVector(cUnit, numTotalBlocks,
                                       false /* expandable */,
                                       kBitMapIDominated);
    bb->domFrontier = oatAllocBitVector(cUnit, numTotalBlocks,
                                        false /* expandable */,
                                        kBitMapDomFrontier);
  } else {
    oatClearAllBits(bb->dominators);
    oatClearAllBits(bb->iDominated);
    oatClearAllBits(bb->domFrontier);
  }
  /* Set all bits in the dominator vector */
  oatSetInitialBits(bb->dominators, numTotalBlocks);

  return true;
}

/*
 * Worker function to compute each block's dominators.  This implementation
 * is only used when kDebugVerifyDataflow is active and should compute
 * the same dominator sets as computeBlockDominators.
 */
bool slowComputeBlockDominators(CompilationUnit* cUnit, BasicBlock* bb)
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

  oatSetInitialBits(tempBlockV, numTotalBlocks);

  /* Iterate through the predecessors */
  oatGrowableListIteratorInit(bb->predecessors, &iter);
  while (true) {
    BasicBlock* predBB = (BasicBlock*)oatGrowableListIteratorNext(&iter);
    if (!predBB) break;
    /* tempBlockV = tempBlockV ^ dominators */
    if (predBB->dominators != NULL) {
      oatIntersectBitVectors(tempBlockV, tempBlockV, predBB->dominators);
    }
  }
  oatSetBit(cUnit, tempBlockV, bb->id);
  if (oatCompareBitVectors(tempBlockV, bb->dominators)) {
    oatCopyBitVector(bb->dominators, tempBlockV);
    return true;
  }
  return false;
}

/*
 * Worker function to compute the idom.  This implementation is only
 * used when kDebugVerifyDataflow is active and should compute the
 * same iDom as computeBlockIDom.
 */
bool slowComputeBlockIDom(CompilationUnit* cUnit, BasicBlock* bb)
{
  GrowableList* blockList = &cUnit->blockList;
  ArenaBitVector* tempBlockV = cUnit->tempBlockV;
  ArenaBitVectorIterator bvIterator;
  BasicBlock* iDom;

  if (bb == cUnit->entryBlock) return false;

  oatCopyBitVector(tempBlockV, bb->dominators);
  oatClearBit(tempBlockV, bb->id);
  oatBitVectorIteratorInit(tempBlockV, &bvIterator);

  /* Should not see any dead block */
  DCHECK_NE(oatCountSetBits(tempBlockV),  0);
  if (oatCountSetBits(tempBlockV) == 1) {
    iDom = (BasicBlock* )
        oatGrowableListGetElement(blockList,
                                  oatBitVectorIteratorNext(&bvIterator));
    bb->iDom = iDom;
  } else {
    int iDomIdx = oatBitVectorIteratorNext(&bvIterator);
    DCHECK_NE(iDomIdx, -1);
    while (true) {
      int nextDom = oatBitVectorIteratorNext(&bvIterator);
      if (nextDom == -1) break;
      BasicBlock* nextDomBB = (BasicBlock* )
          oatGrowableListGetElement(blockList, nextDom);
      /* iDom dominates nextDom - set new iDom */
      if (oatIsBitSet(nextDomBB->dominators, iDomIdx)) {
          iDomIdx = nextDom;
      }

    }
    iDom = (BasicBlock* ) oatGrowableListGetElement(blockList, iDomIdx);
    /* Set the immediate dominator block for bb */
    bb->iDom = iDom;
  }
  /* Add bb to the iDominated set of the immediate dominator block */
  oatSetBit(cUnit, iDom->iDominated, bb->id);
  return true;
}

/*
 * Walk through the ordered iDom list until we reach common parent.
 * Given the ordering of iDomList, this common parent represents the
 * last element of the intersection of block1 and block2 dominators.
  */
int findCommonParent(CompilationUnit *cUnit, int block1, int block2)
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
bool computeBlockIDom(CompilationUnit* cUnit, BasicBlock* bb)
{
  GrowableListIterator iter;
  int idom = -1;

  /* Special-case entry block */
  if (bb == cUnit->entryBlock) {
    return false;
  }

  /* Iterate through the predecessors */
  oatGrowableListIteratorInit(bb->predecessors, &iter);

  /* Find the first processed predecessor */
  while (true) {
    BasicBlock* predBB = (BasicBlock*)oatGrowableListIteratorNext(&iter);
    CHECK(predBB != NULL);
    if (cUnit->iDomList[predBB->dfsId] != NOTVISITED) {
      idom = predBB->dfsId;
      break;
    }
  }

  /* Scan the rest of the predecessors */
  while (true) {
      BasicBlock* predBB = (BasicBlock*)oatGrowableListIteratorNext(&iter);
      if (!predBB) break;
      if (cUnit->iDomList[predBB->dfsId] == NOTVISITED) {
        continue;
      } else {
        idom = findCommonParent(cUnit, predBB->dfsId, idom);
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
bool computeBlockDominators(CompilationUnit* cUnit, BasicBlock* bb)
{
  if (bb == cUnit->entryBlock) {
    oatClearAllBits(bb->dominators);
  } else {
    oatCopyBitVector(bb->dominators, bb->iDom->dominators);
  }
  oatSetBit(cUnit, bb->dominators, bb->id);
  return false;
}

bool setDominators(CompilationUnit* cUnit, BasicBlock* bb)
{
  if (bb != cUnit->entryBlock) {
    int iDomDFSIdx = cUnit->iDomList[bb->dfsId];
    DCHECK_NE(iDomDFSIdx, NOTVISITED);
    int iDomIdx = cUnit->dfsPostOrder.elemList[iDomDFSIdx];
    BasicBlock* iDom = (BasicBlock*)
          oatGrowableListGetElement(&cUnit->blockList, iDomIdx);
    if (cUnit->enableDebug & (1 << kDebugVerifyDataflow)) {
      DCHECK_EQ(bb->iDom->id, iDom->id);
    }
    bb->iDom = iDom;
    /* Add bb to the iDominated set of the immediate dominator block */
    oatSetBit(cUnit, iDom->iDominated, bb->id);
  }
  return false;
}

/* Compute dominators, immediate dominator, and dominance fronter */
void computeDominators(CompilationUnit* cUnit)
{
  int numReachableBlocks = cUnit->numReachableBlocks;
  int numTotalBlocks = cUnit->blockList.numUsed;

  /* Initialize domination-related data structures */
  oatDataFlowAnalysisDispatcher(cUnit, initializeDominationInfo,
                                kReachableNodes, false /* isIterative */);

  /* Initalize & Clear iDomList */
  if (cUnit->iDomList == NULL) {
    cUnit->iDomList = (int*)oatNew(cUnit, sizeof(int) * numReachableBlocks,
                                   false, kAllocDFInfo);
  }
  for (int i = 0; i < numReachableBlocks; i++) {
    cUnit->iDomList[i] = NOTVISITED;
  }

  /* For post-order, last block is entry block.  Set its iDom to istelf */
  DCHECK_EQ(cUnit->entryBlock->dfsId, numReachableBlocks-1);
  cUnit->iDomList[cUnit->entryBlock->dfsId] = cUnit->entryBlock->dfsId;

  /* Compute the immediate dominators */
  oatDataFlowAnalysisDispatcher(cUnit, computeBlockIDom,
                                kReversePostOrderTraversal,
                                true /* isIterative */);

  /* Set the dominator for the root node */
  oatClearAllBits(cUnit->entryBlock->dominators);
  oatSetBit(cUnit, cUnit->entryBlock->dominators, cUnit->entryBlock->id);

  if (cUnit->tempBlockV == NULL) {
    cUnit->tempBlockV = oatAllocBitVector(cUnit, numTotalBlocks,
                                          false /* expandable */,
                                          kBitMapTmpBlockV);
  } else {
    oatClearAllBits(cUnit->tempBlockV);
  }
  cUnit->entryBlock->iDom = NULL;

  /* For testing, compute sets using alternate mechanism */
  if (cUnit->enableDebug & (1 << kDebugVerifyDataflow)) {
    // Use alternate mechanism to compute dominators for comparison
    oatDataFlowAnalysisDispatcher(cUnit, slowComputeBlockDominators,
                                  kPreOrderDFSTraversal,
                                  true /* isIterative */);

   oatDataFlowAnalysisDispatcher(cUnit, slowComputeBlockIDom,
                                 kReachableNodes,
                                 false /* isIterative */);
  }

  oatDataFlowAnalysisDispatcher(cUnit, setDominators,
                                kReachableNodes,
                                false /* isIterative */);

  oatDataFlowAnalysisDispatcher(cUnit, computeBlockDominators,
                                kReversePostOrderTraversal,
                                false /* isIterative */);

  /*
   * Now go ahead and compute the post order traversal based on the
   * iDominated sets.
   */
  if (cUnit->domPostOrderTraversal.elemList == NULL) {
    oatInitGrowableList(cUnit, &cUnit->domPostOrderTraversal,
                        numReachableBlocks, kListDomPostOrderTraversal);
  } else {
    cUnit->domPostOrderTraversal.numUsed = 0;
  }

  computeDomPostOrderTraversal(cUnit, cUnit->entryBlock);
  DCHECK_EQ(cUnit->domPostOrderTraversal.numUsed,
         (unsigned) cUnit->numReachableBlocks);

  /* Now compute the dominance frontier for each block */
  oatDataFlowAnalysisDispatcher(cUnit, computeDominanceFrontier,
                                        kPostOrderDOMTraversal,
                                        false /* isIterative */);
}

/*
 * Perform dest U= src1 ^ ~src2
 * This is probably not general enough to be placed in BitVector.[ch].
 */
void computeSuccLiveIn(ArenaBitVector* dest,
                            const ArenaBitVector* src1,
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
bool computeBlockLiveIns(CompilationUnit* cUnit, BasicBlock* bb)
{
  ArenaBitVector* tempDalvikRegisterV = cUnit->tempDalvikRegisterV;

  if (bb->dataFlowInfo == NULL) return false;
  oatCopyBitVector(tempDalvikRegisterV, bb->dataFlowInfo->liveInV);
  if (bb->taken && bb->taken->dataFlowInfo)
    computeSuccLiveIn(tempDalvikRegisterV, bb->taken->dataFlowInfo->liveInV,
                      bb->dataFlowInfo->defV);
  if (bb->fallThrough && bb->fallThrough->dataFlowInfo)
    computeSuccLiveIn(tempDalvikRegisterV,
                      bb->fallThrough->dataFlowInfo->liveInV,
                      bb->dataFlowInfo->defV);
  if (bb->successorBlockList.blockListType != kNotUsed) {
    GrowableListIterator iterator;
    oatGrowableListIteratorInit(&bb->successorBlockList.blocks,
                                &iterator);
    while (true) {
      SuccessorBlockInfo *successorBlockInfo =
          (SuccessorBlockInfo *) oatGrowableListIteratorNext(&iterator);
      if (successorBlockInfo == NULL) break;
      BasicBlock* succBB = successorBlockInfo->block;
      if (succBB->dataFlowInfo) {
        computeSuccLiveIn(tempDalvikRegisterV,
                          succBB->dataFlowInfo->liveInV,
                          bb->dataFlowInfo->defV);
      }
    }
  }
  if (oatCompareBitVectors(tempDalvikRegisterV, bb->dataFlowInfo->liveInV)) {
    oatCopyBitVector(bb->dataFlowInfo->liveInV, tempDalvikRegisterV);
    return true;
  }
  return false;
}

/* Insert phi nodes to for each variable to the dominance frontiers */
void insertPhiNodes(CompilationUnit* cUnit)
{
  int dalvikReg;
  const GrowableList* blockList = &cUnit->blockList;
  ArenaBitVector* phiBlocks =
      oatAllocBitVector(cUnit, cUnit->numBlocks, false, kBitMapPhi);
  ArenaBitVector* tmpBlocks =
      oatAllocBitVector(cUnit, cUnit->numBlocks, false, kBitMapTmpBlocks);
  ArenaBitVector* inputBlocks =
      oatAllocBitVector(cUnit, cUnit->numBlocks, false, kBitMapInputBlocks);

  cUnit->tempDalvikRegisterV =
      oatAllocBitVector(cUnit, cUnit->numDalvikRegisters, false,
                        kBitMapRegisterV);

  oatDataFlowAnalysisDispatcher(cUnit, computeBlockLiveIns,
                                kPostOrderDFSTraversal, true /* isIterative */);

  /* Iterate through each Dalvik register */
  for (dalvikReg = cUnit->numDalvikRegisters - 1; dalvikReg >= 0; dalvikReg--) {
    bool change;
    ArenaBitVectorIterator iterator;

    oatCopyBitVector(inputBlocks, cUnit->defBlockMatrix[dalvikReg]);
    oatClearAllBits(phiBlocks);

    /* Calculate the phi blocks for each Dalvik register */
    do {
      change = false;
      oatClearAllBits(tmpBlocks);
      oatBitVectorIteratorInit(inputBlocks, &iterator);

      while (true) {
        int idx = oatBitVectorIteratorNext(&iterator);
        if (idx == -1) break;
          BasicBlock* defBB =
              (BasicBlock* ) oatGrowableListGetElement(blockList, idx);

          /* Merge the dominance frontier to tmpBlocks */
          //TUNING: hot call to oatUnifyBitVectors
          if (defBB->domFrontier != NULL) {
            oatUnifyBitVectors(tmpBlocks, tmpBlocks, defBB->domFrontier);
          }
        }
        if (oatCompareBitVectors(phiBlocks, tmpBlocks)) {
          change = true;
          oatCopyBitVector(phiBlocks, tmpBlocks);

          /*
           * Iterate through the original blocks plus the new ones in
           * the dominance frontier.
           */
          oatCopyBitVector(inputBlocks, phiBlocks);
          oatUnifyBitVectors(inputBlocks, inputBlocks,
                             cUnit->defBlockMatrix[dalvikReg]);
      }
    } while (change);

    /*
     * Insert a phi node for dalvikReg in the phiBlocks if the Dalvik
     * register is in the live-in set.
     */
    oatBitVectorIteratorInit(phiBlocks, &iterator);
    while (true) {
      int idx = oatBitVectorIteratorNext(&iterator);
      if (idx == -1) break;
      BasicBlock* phiBB =
          (BasicBlock* ) oatGrowableListGetElement(blockList, idx);
      /* Variable will be clobbered before being used - no need for phi */
      if (!oatIsBitSet(phiBB->dataFlowInfo->liveInV, dalvikReg)) continue;
      MIR *phi = (MIR *) oatNew(cUnit, sizeof(MIR), true, kAllocDFInfo);
      phi->dalvikInsn.opcode = (Instruction::Code)kMirOpPhi;
      phi->dalvikInsn.vA = dalvikReg;
      phi->offset = phiBB->startOffset;
      phi->meta.phiNext = cUnit->phiList;
      cUnit->phiList = phi;
      oatPrependMIR(phiBB, phi);
    }
  }
}

/*
 * Worker function to insert phi-operands with latest SSA names from
 * predecessor blocks
 */
bool insertPhiNodeOperands(CompilationUnit* cUnit, BasicBlock* bb)
{
  GrowableListIterator iter;
  MIR *mir;
  std::vector<int> uses;
  std::vector<int> incomingArc;

  /* Phi nodes are at the beginning of each block */
  for (mir = bb->firstMIRInsn; mir; mir = mir->next) {
    if (mir->dalvikInsn.opcode != (Instruction::Code)kMirOpPhi)
      return true;
    int ssaReg = mir->ssaRep->defs[0];
    DCHECK_GE(ssaReg, 0);   // Shouldn't see compiler temps here
    int vReg = SRegToVReg(cUnit, ssaReg);

    uses.clear();
    incomingArc.clear();

    /* Iterate through the predecessors */
    oatGrowableListIteratorInit(bb->predecessors, &iter);
    while (true) {
      BasicBlock* predBB =
         (BasicBlock*)oatGrowableListIteratorNext(&iter);
      if (!predBB) break;
      int ssaReg = predBB->dataFlowInfo->vRegToSSAMap[vReg];
      uses.push_back(ssaReg);
      incomingArc.push_back(predBB->id);
    }

    /* Count the number of SSA registers for a Dalvik register */
    int numUses = uses.size();
    mir->ssaRep->numUses = numUses;
    mir->ssaRep->uses =
        (int*) oatNew(cUnit, sizeof(int) * numUses, false, kAllocDFInfo);
    mir->ssaRep->fpUse =
        (bool*) oatNew(cUnit, sizeof(bool) * numUses, true, kAllocDFInfo);
    int* incoming =
        (int*) oatNew(cUnit, sizeof(int) * numUses, false, kAllocDFInfo);
    // TODO: Ugly, rework (but don't burden each MIR/LIR for Phi-only needs)
    mir->dalvikInsn.vB = (intptr_t) incoming;

    /* Set the uses array for the phi node */
    int *usePtr = mir->ssaRep->uses;
    for (int i = 0; i < numUses; i++) {
      *usePtr++ = uses[i];
      *incoming++ = incomingArc[i];
    }
  }

  return true;
}

void doDFSPreOrderSSARename(CompilationUnit* cUnit, BasicBlock* block)
{

  if (block->visited || block->hidden) return;
  block->visited = true;

  /* Process this block */
  oatDoSSAConversion(cUnit, block);
  int mapSize = sizeof(int) * cUnit->numDalvikRegisters;

  /* Save SSA map snapshot */
  int* savedSSAMap = (int*)oatNew(cUnit, mapSize, false,
                                  kAllocDalvikToSSAMap);
  memcpy(savedSSAMap, cUnit->vRegToSSAMap, mapSize);

  if (block->fallThrough) {
    doDFSPreOrderSSARename(cUnit, block->fallThrough);
    /* Restore SSA map snapshot */
    memcpy(cUnit->vRegToSSAMap, savedSSAMap, mapSize);
  }
  if (block->taken) {
    doDFSPreOrderSSARename(cUnit, block->taken);
    /* Restore SSA map snapshot */
    memcpy(cUnit->vRegToSSAMap, savedSSAMap, mapSize);
  }
  if (block->successorBlockList.blockListType != kNotUsed) {
    GrowableListIterator iterator;
    oatGrowableListIteratorInit(&block->successorBlockList.blocks,
                                &iterator);
    while (true) {
      SuccessorBlockInfo *successorBlockInfo =
          (SuccessorBlockInfo *) oatGrowableListIteratorNext(&iterator);
      if (successorBlockInfo == NULL) break;
      BasicBlock* succBB = successorBlockInfo->block;
      doDFSPreOrderSSARename(cUnit, succBB);
      /* Restore SSA map snapshot */
      memcpy(cUnit->vRegToSSAMap, savedSSAMap, mapSize);
    }
  }
  cUnit->vRegToSSAMap = savedSSAMap;
  return;
}

/* Perform SSA transformation for the whole method */
void oatMethodSSATransformation(CompilationUnit* cUnit)
{
  /* Compute the DFS order */
  computeDFSOrders(cUnit);

  if (!cUnit->disableDataflow) {
    /* Compute the dominator info */
    computeDominators(cUnit);
  }

  /* Allocate data structures in preparation for SSA conversion */
  oatInitializeSSAConversion(cUnit);

  if (!cUnit->disableDataflow) {
    /* Find out the "Dalvik reg def x block" relation */
    computeDefBlockMatrix(cUnit);

    /* Insert phi nodes to dominance frontiers for all variables */
    insertPhiNodes(cUnit);
  }

  /* Rename register names by local defs and phi nodes */
  oatDataFlowAnalysisDispatcher(cUnit, oatClearVisitedFlag,
                                kAllNodes, false /* isIterative */);
  doDFSPreOrderSSARename(cUnit, cUnit->entryBlock);

  if (!cUnit->disableDataflow) {
    /*
     * Shared temp bit vector used by each block to count the number of defs
     * from all the predecessor blocks.
     */
    cUnit->tempSSARegisterV = oatAllocBitVector(cUnit, cUnit->numSSARegs,
         false, kBitMapTempSSARegisterV);

    cUnit->tempSSABlockIdV =
        (int*)oatNew(cUnit, sizeof(int) * cUnit->numSSARegs, false,
                     kAllocDFInfo);

    /* Insert phi-operands with latest SSA names from predecessor blocks */
    oatDataFlowAnalysisDispatcher(cUnit, insertPhiNodeOperands,
                                  kReachableNodes, false /* isIterative */);
  }
}

}  // namespace art
