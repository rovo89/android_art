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

#include "Dalvik.h"
#include "Dataflow.h"

/* Enter the node to the dfsOrder list then visit its successors */
STATIC void recordDFSPreOrder(CompilationUnit* cUnit, BasicBlock* block)
{

    if (block->visited || block->hidden) return;
    block->visited = true;

    /* Enqueue the block id */
    oatInsertGrowableList(&cUnit->dfsOrder, block->id);

    if (block->fallThrough) recordDFSPreOrder(cUnit, block->fallThrough);
    if (block->taken) recordDFSPreOrder(cUnit, block->taken);
    if (block->successorBlockList.blockListType != kNotUsed) {
        GrowableListIterator iterator;
        oatGrowableListIteratorInit(&block->successorBlockList.blocks,
                                    &iterator);
        while (true) {
            SuccessorBlockInfo *successorBlockInfo =
                (SuccessorBlockInfo *) oatGrowableListIteratorNext(&iterator);
            if (successorBlockInfo == NULL) break;
            BasicBlock* succBB = successorBlockInfo->block;
            recordDFSPreOrder(cUnit, succBB);
        }
    }
    return;
}

/* Sort the blocks by the Depth-First-Search pre-order */
STATIC void computeDFSOrder(CompilationUnit* cUnit)
{
    /* Initialize or reset the DFS order list */
    if (cUnit->dfsOrder.elemList == NULL) {
        oatInitGrowableList(&cUnit->dfsOrder, cUnit->numBlocks);
    } else {
        /* Just reset the used length on the counter */
        cUnit->dfsOrder.numUsed = 0;
    }

    oatDataFlowAnalysisDispatcher(cUnit, oatClearVisitedFlag,
                                          kAllNodes,
                                          false /* isIterative */);

    recordDFSPreOrder(cUnit, cUnit->entryBlock);
    cUnit->numReachableBlocks = cUnit->dfsOrder.numUsed;
}

/*
 * Mark block bit on the per-Dalvik register vector to denote that Dalvik
 * register idx is defined in BasicBlock bb.
 */
STATIC bool fillDefBlockMatrix(CompilationUnit* cUnit, BasicBlock* bb)
{
    if (bb->dataFlowInfo == NULL) return false;

    ArenaBitVectorIterator iterator;

    oatBitVectorIteratorInit(bb->dataFlowInfo->defV, &iterator);
    while (true) {
        int idx = oatBitVectorIteratorNext(&iterator);
        if (idx == -1) break;
        /* Block bb defines register idx */
        oatSetBit(cUnit->defBlockMatrix[idx], bb->id);
    }
    return true;
}

STATIC void computeDefBlockMatrix(CompilationUnit* cUnit)
{
    int numRegisters = cUnit->numDalvikRegisters;
    /* Allocate numDalvikRegisters bit vector pointers */
    cUnit->defBlockMatrix = (ArenaBitVector **)
        oatNew(sizeof(ArenaBitVector *) * numRegisters, true);
    int i;

    /* Initialize numRegister vectors with numBlocks bits each */
    for (i = 0; i < numRegisters; i++) {
        cUnit->defBlockMatrix[i] = oatAllocBitVector(cUnit->numBlocks,
                                                             false);
    }
    oatDataFlowAnalysisDispatcher(cUnit, oatFindLocalLiveIn,
                                          kAllNodes,
                                          false /* isIterative */);
    oatDataFlowAnalysisDispatcher(cUnit, fillDefBlockMatrix,
                                          kAllNodes,
                                          false /* isIterative */);

    /*
     * Also set the incoming parameters as defs in the entry block.
     * Only need to handle the parameters for the outer method.
     */
    int inReg = cUnit->method->NumRegisters() - cUnit->method->NumIns();
    for (; inReg < cUnit->method->NumRegisters(); inReg++) {
        oatSetBit(cUnit->defBlockMatrix[inReg],
                          cUnit->entryBlock->id);
    }
}

/* Compute the post-order traversal of the CFG */
STATIC void computeDomPostOrderTraversal(CompilationUnit* cUnit, BasicBlock* bb)
{
    ArenaBitVectorIterator bvIterator;
    oatBitVectorIteratorInit(bb->iDominated, &bvIterator);
    GrowableList* blockList = &cUnit->blockList;

    /* Iterate through the dominated blocks first */
    while (true) {
        int bbIdx = oatBitVectorIteratorNext(&bvIterator);
        if (bbIdx == -1) break;
        BasicBlock* dominatedBB =
            (BasicBlock* ) oatGrowableListGetElement(blockList, bbIdx);
        computeDomPostOrderTraversal(cUnit, dominatedBB);
    }

    /* Enter the current block id */
    oatInsertGrowableList(&cUnit->domPostOrderTraversal, bb->id);

    /* hacky loop detection */
    if (bb->taken && oatIsBitSet(bb->dominators, bb->taken->id)) {
        cUnit->hasLoop = true;
    }
}

STATIC void checkForDominanceFrontier(BasicBlock* domBB,
                                      const BasicBlock* succBB)
{
    /*
     * TODO - evaluate whether phi will ever need to be inserted into exit
     * blocks.
     */
    if (succBB->iDom != domBB &&
        succBB->blockType == kDalvikByteCode &&
        succBB->hidden == false) {
        oatSetBit(domBB->domFrontier, succBB->id);
    }
}

/* Worker function to compute the dominance frontier */
STATIC bool computeDominanceFrontier(CompilationUnit* cUnit, BasicBlock* bb)
{
    GrowableList* blockList = &cUnit->blockList;

    /* Calculate DF_local */
    if (bb->taken) {
        checkForDominanceFrontier(bb, bb->taken);
    }
    if (bb->fallThrough) {
        checkForDominanceFrontier(bb, bb->fallThrough);
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
            checkForDominanceFrontier(bb, succBB);
        }
    }

    /* Calculate DF_up */
    ArenaBitVectorIterator bvIterator;
    oatBitVectorIteratorInit(bb->iDominated, &bvIterator);
    while (true) {
        int dominatedIdx = oatBitVectorIteratorNext(&bvIterator);
        if (dominatedIdx == -1) break;
        BasicBlock* dominatedBB = (BasicBlock* )
            oatGrowableListGetElement(blockList, dominatedIdx);
        ArenaBitVectorIterator dfIterator;
        oatBitVectorIteratorInit(dominatedBB->domFrontier, &dfIterator);
        while (true) {
            int dfUpIdx = oatBitVectorIteratorNext(&dfIterator);
            if (dfUpIdx == -1) break;
            BasicBlock* dfUpBlock = (BasicBlock* )
                oatGrowableListGetElement(blockList, dfUpIdx);
            checkForDominanceFrontier(bb, dfUpBlock);
        }
    }

    return true;
}

/* Worker function for initializing domination-related data structures */
STATIC bool initializeDominationInfo(CompilationUnit* cUnit, BasicBlock* bb)
{
    int numTotalBlocks = cUnit->blockList.numUsed;

    if (bb->dominators == NULL ) {
        bb->dominators = oatAllocBitVector(numTotalBlocks,
                                                   false /* expandable */);
        bb->iDominated = oatAllocBitVector(numTotalBlocks,
                                                   false /* expandable */);
        bb->domFrontier = oatAllocBitVector(numTotalBlocks,
                                                   false /* expandable */);
    } else {
        oatClearAllBits(bb->dominators);
        oatClearAllBits(bb->iDominated);
        oatClearAllBits(bb->domFrontier);
    }
    /* Set all bits in the dominator vector */
    oatSetInitialBits(bb->dominators, numTotalBlocks);

    return true;
}

/* Worker function to compute each block's dominators */
STATIC bool computeBlockDominators(CompilationUnit* cUnit, BasicBlock* bb)
{
    GrowableList* blockList = &cUnit->blockList;
    int numTotalBlocks = blockList->numUsed;
    ArenaBitVector* tempBlockV = cUnit->tempBlockV;
    ArenaBitVectorIterator bvIterator;

    /*
     * The dominator of the entry block has been preset to itself and we need
     * to skip the calculation here.
     */
    if (bb == cUnit->entryBlock) return false;

    oatSetInitialBits(tempBlockV, numTotalBlocks);

    /* Iterate through the predecessors */
    oatBitVectorIteratorInit(bb->predecessors, &bvIterator);
    while (true) {
        int predIdx = oatBitVectorIteratorNext(&bvIterator);
        if (predIdx == -1) break;
        BasicBlock* predBB = (BasicBlock* ) oatGrowableListGetElement(
                                 blockList, predIdx);
        /* tempBlockV = tempBlockV ^ dominators */
        if (predBB->dominators != NULL) {
            oatIntersectBitVectors(tempBlockV, tempBlockV, predBB->dominators);
        }
    }
    oatSetBit(tempBlockV, bb->id);
    if (oatCompareBitVectors(tempBlockV, bb->dominators)) {
        oatCopyBitVector(bb->dominators, tempBlockV);
        return true;
    }
    return false;
}

/* Worker function to compute the idom */
STATIC bool computeImmediateDominator(CompilationUnit* cUnit, BasicBlock* bb)
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
        iDom = (BasicBlock* ) oatGrowableListGetElement(
                       blockList, oatBitVectorIteratorNext(&bvIterator));
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
    oatSetBit(iDom->iDominated, bb->id);
    return true;
}

/* Compute dominators, immediate dominator, and dominance fronter */
STATIC void computeDominators(CompilationUnit* cUnit)
{
    int numReachableBlocks = cUnit->numReachableBlocks;
    int numTotalBlocks = cUnit->blockList.numUsed;

    /* Initialize domination-related data structures */
    oatDataFlowAnalysisDispatcher(cUnit, initializeDominationInfo,
                                          kReachableNodes,
                                          false /* isIterative */);

    /* Set the dominator for the root node */
    oatClearAllBits(cUnit->entryBlock->dominators);
    oatSetBit(cUnit->entryBlock->dominators, cUnit->entryBlock->id);

    if (cUnit->tempBlockV == NULL) {
        cUnit->tempBlockV = oatAllocBitVector(numTotalBlocks,
                                                  false /* expandable */);
    } else {
        oatClearAllBits(cUnit->tempBlockV);
    }
    oatDataFlowAnalysisDispatcher(cUnit, computeBlockDominators,
                                          kPreOrderDFSTraversal,
                                          true /* isIterative */);

    cUnit->entryBlock->iDom = NULL;
    oatDataFlowAnalysisDispatcher(cUnit, computeImmediateDominator,
                                          kReachableNodes,
                                          false /* isIterative */);

    /*
     * Now go ahead and compute the post order traversal based on the
     * iDominated sets.
     */
    if (cUnit->domPostOrderTraversal.elemList == NULL) {
        oatInitGrowableList(&cUnit->domPostOrderTraversal, numReachableBlocks);
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
STATIC void computeSuccLiveIn(ArenaBitVector* dest,
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
STATIC bool computeBlockLiveIns(CompilationUnit* cUnit, BasicBlock* bb)
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
STATIC void insertPhiNodes(CompilationUnit* cUnit)
{
    int dalvikReg;
    const GrowableList* blockList = &cUnit->blockList;
    ArenaBitVector* phiBlocks =
        oatAllocBitVector(cUnit->numBlocks, false);
    ArenaBitVector* tmpBlocks =
        oatAllocBitVector(cUnit->numBlocks, false);
    ArenaBitVector* inputBlocks =
        oatAllocBitVector(cUnit->numBlocks, false);

    cUnit->tempDalvikRegisterV =
        oatAllocBitVector(cUnit->numDalvikRegisters, false);

    oatDataFlowAnalysisDispatcher(cUnit, computeBlockLiveIns,
                                          kPostOrderDFSTraversal,
                                          true /* isIterative */);

    /* Iterate through each Dalvik register */
    for (dalvikReg = 0; dalvikReg < cUnit->numDalvikRegisters; dalvikReg++) {
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
            MIR *phi = (MIR *) oatNew(sizeof(MIR), true);
            phi->dalvikInsn.opcode = (Opcode)kMirOpPhi;
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
STATIC bool insertPhiNodeOperands(CompilationUnit* cUnit, BasicBlock* bb)
{
    ArenaBitVector* ssaRegV = cUnit->tempSSARegisterV;
    ArenaBitVectorIterator bvIterator;
    GrowableList* blockList = &cUnit->blockList;
    MIR *mir;

    /* Phi nodes are at the beginning of each block */
    for (mir = bb->firstMIRInsn; mir; mir = mir->next) {
        if (mir->dalvikInsn.opcode != (Opcode)kMirOpPhi)
            return true;
        int ssaReg = mir->ssaRep->defs[0];
        int encodedDalvikValue =
            (int) oatGrowableListGetElement(cUnit->ssaToDalvikMap, ssaReg);
        int dalvikReg = DECODE_REG(encodedDalvikValue);

        oatClearAllBits(ssaRegV);

        /* Iterate through the predecessors */
        oatBitVectorIteratorInit(bb->predecessors, &bvIterator);
        while (true) {
            int predIdx = oatBitVectorIteratorNext(&bvIterator);
            if (predIdx == -1) break;
            BasicBlock* predBB = (BasicBlock* ) oatGrowableListGetElement(
                                     blockList, predIdx);
            int encodedSSAValue =
                predBB->dataFlowInfo->dalvikToSSAMap[dalvikReg];
            int ssaReg = DECODE_REG(encodedSSAValue);
            oatSetBit(ssaRegV, ssaReg);
        }

        /* Count the number of SSA registers for a Dalvik register */
        int numUses = oatCountSetBits(ssaRegV);
        mir->ssaRep->numUses = numUses;
        mir->ssaRep->uses =
            (int *) oatNew(sizeof(int) * numUses, false);
        mir->ssaRep->fpUse =
            (bool *) oatNew(sizeof(bool) * numUses, true);

        ArenaBitVectorIterator phiIterator;

        oatBitVectorIteratorInit(ssaRegV, &phiIterator);
        int *usePtr = mir->ssaRep->uses;

        /* Set the uses array for the phi node */
        while (true) {
            int ssaRegIdx = oatBitVectorIteratorNext(&phiIterator);
            if (ssaRegIdx == -1) break;
            *usePtr++ = ssaRegIdx;
        }
    }

    return true;
}

STATIC void doDFSPreOrderSSARename(CompilationUnit* cUnit, BasicBlock* block)
{

    if (block->visited || block->hidden) return;
    block->visited = true;

    /* Process this block */
    oatDoSSAConversion(cUnit, block);
    int mapSize = sizeof(int) * cUnit->method->NumRegisters();

    /* Save SSA map snapshot */
    int* savedSSAMap = (int*)oatNew(mapSize, false);
    memcpy(savedSSAMap, cUnit->dalvikToSSAMap, mapSize);

    if (block->fallThrough) {
        doDFSPreOrderSSARename(cUnit, block->fallThrough);
        /* Restore SSA map snapshot */
        memcpy(cUnit->dalvikToSSAMap, savedSSAMap, mapSize);
    }
    if (block->taken) {
        doDFSPreOrderSSARename(cUnit, block->taken);
        /* Restore SSA map snapshot */
        memcpy(cUnit->dalvikToSSAMap, savedSSAMap, mapSize);
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
            memcpy(cUnit->dalvikToSSAMap, savedSSAMap, mapSize);
        }
    }
    cUnit->dalvikToSSAMap = savedSSAMap;
    return;
}

/* Perform SSA transformation for the whole method */
void oatMethodSSATransformation(CompilationUnit* cUnit)
{
    /* Compute the DFS order */
    computeDFSOrder(cUnit);

    /* Compute the dominator info */
    computeDominators(cUnit);

    /* Allocate data structures in preparation for SSA conversion */
    oatInitializeSSAConversion(cUnit);

    /* Find out the "Dalvik reg def x block" relation */
    computeDefBlockMatrix(cUnit);

    /* Insert phi nodes to dominance frontiers for all variables */
    insertPhiNodes(cUnit);

    /* Rename register names by local defs and phi nodes */
    oatDataFlowAnalysisDispatcher(cUnit, oatClearVisitedFlag,
                                          kAllNodes,
                                          false /* isIterative */);
    doDFSPreOrderSSARename(cUnit, cUnit->entryBlock);

    /*
     * Shared temp bit vector used by each block to count the number of defs
     * from all the predecessor blocks.
     */
    cUnit->tempSSARegisterV = oatAllocBitVector(cUnit->numSSARegs,
                                                        false);

    /* Insert phi-operands with latest SSA names from predecessor blocks */
    oatDataFlowAnalysisDispatcher(cUnit, insertPhiNodeOperands,
                                          kReachableNodes,
                                          false /* isIterative */);
}
