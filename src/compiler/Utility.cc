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
#include "CompilerInternals.h"

static ArenaMemBlock *arenaHead, *currentArena;
static int numArenaBlocks;

#define kArenaBitVectorGrowth    4   /* increase by 4 u4s when limit hit */

/* Allocate the initial memory block for arena-based allocation */
bool oatHeapInit(void)
{
    assert(arenaHead == NULL);
    arenaHead =
        (ArenaMemBlock *) malloc(sizeof(ArenaMemBlock) + ARENA_DEFAULT_SIZE);
    if (arenaHead == NULL) {
        LOG(FATAL) << "No memory left to create compiler heap memory";
    }
    arenaHead->blockSize = ARENA_DEFAULT_SIZE;
    currentArena = arenaHead;
    currentArena->bytesAllocated = 0;
    currentArena->next = NULL;
    numArenaBlocks = 1;

    return true;
}

/* Arena-based malloc for compilation tasks */
void* oatNew(size_t size, bool zero)
{
    size = (size + 3) & ~3;
retry:
    /* Normal case - space is available in the current page */
    if (size + currentArena->bytesAllocated <= currentArena->blockSize) {
        void *ptr;
        ptr = &currentArena->ptr[currentArena->bytesAllocated];
        currentArena->bytesAllocated += size;
        if (zero) {
            memset(ptr, 0, size);
        }
        return ptr;
    } else {
        /*
         * See if there are previously allocated arena blocks before the last
         * reset
         */
        if (currentArena->next) {
            currentArena = currentArena->next;
            goto retry;
        }

        size_t blockSize = (size < ARENA_DEFAULT_SIZE) ?
                          ARENA_DEFAULT_SIZE : size;
        /* Time to allocate a new arena */
        ArenaMemBlock *newArena = (ArenaMemBlock *)
            malloc(sizeof(ArenaMemBlock) + blockSize);
        if (newArena == NULL) {
            LOG(FATAL) << "Arena allocation failure";
        }
        newArena->blockSize = blockSize;
        newArena->bytesAllocated = 0;
        newArena->next = NULL;
        currentArena->next = newArena;
        currentArena = newArena;
        numArenaBlocks++;
        if (numArenaBlocks > 1000) {
            LOG(INFO) << "Total arena pages: " << numArenaBlocks;
        }
        goto retry;
    }
}

/* Reclaim all the arena blocks allocated so far */
void oatArenaReset(void)
{
    ArenaMemBlock *block;

    for (block = arenaHead; block; block = block->next) {
        block->bytesAllocated = 0;
    }
    currentArena = arenaHead;
}

/* Growable List initialization */
void oatInitGrowableList(GrowableList* gList, size_t initLength)
{
    gList->numAllocated = initLength;
    gList->numUsed = 0;
    gList->elemList = (intptr_t *) oatNew(sizeof(intptr_t) * initLength,
                                                  true);
}

/* Expand the capacity of a growable list */
static void expandGrowableList(GrowableList* gList)
{
    int newLength = gList->numAllocated;
    if (newLength < 128) {
        newLength <<= 1;
    } else {
        newLength += 128;
    }
    intptr_t *newArray =
        (intptr_t *) oatNew(sizeof(intptr_t) * newLength, true);
    memcpy(newArray, gList->elemList, sizeof(intptr_t) * gList->numAllocated);
    gList->numAllocated = newLength;
    gList->elemList = newArray;
}

/* Insert a new element into the growable list */
void oatInsertGrowableList(GrowableList* gList, intptr_t elem)
{
    assert(gList->numAllocated != 0);
    if (gList->numUsed == gList->numAllocated) {
        expandGrowableList(gList);
    }
    gList->elemList[gList->numUsed++] = elem;
}

void oatGrowableListIteratorInit(GrowableList* gList,
                                 GrowableListIterator* iterator)
{
    iterator->list = gList;
    iterator->idx = 0;
    iterator->size = gList->numUsed;
}

intptr_t oatGrowableListIteratorNext(GrowableListIterator* iterator)
{
    assert(iterator->size == iterator->list->numUsed);
    if (iterator->idx == iterator->size) return 0;
    return iterator->list->elemList[iterator->idx++];
}

intptr_t oatGrowableListGetElement(const GrowableList* gList, size_t idx)
{
    assert(idx < gList->numUsed);
    return gList->elemList[idx];
}

/* Debug Utility - dump a compilation unit */
void oatDumpCompilationUnit(CompilationUnit* cUnit)
{
    BasicBlock* bb;
    const char* blockTypeNames[] = {
        "Entry Block",
        "Code Block",
        "Exit Block",
        "Exception Handling",
        "Catch Block"
    };

    LOG(INFO) << "Compiling " << art::PrettyMethod(cUnit->method);
    LOG(INFO) << cUnit->insns << " insns";
    LOG(INFO) << cUnit->numBlocks << " blocks in total";
    GrowableListIterator iterator;

    oatGrowableListIteratorInit(&cUnit->blockList, &iterator);

    while (true) {
        bb = (BasicBlock *) oatGrowableListIteratorNext(&iterator);
        if (bb == NULL) break;
        char buf[100];
        snprintf(buf, 100, "Block %d (%s) (insn %04x - %04x%s)",
             bb->id,
             blockTypeNames[bb->blockType],
             bb->startOffset,
             bb->lastMIRInsn ? bb->lastMIRInsn->offset : bb->startOffset,
             bb->lastMIRInsn ? "" : " empty");
        LOG(INFO) << buf;
        if (bb->taken) {
            LOG(INFO) << "  Taken branch: block " << bb->taken->id <<
                 "(0x" << std::hex << bb->taken->startOffset << ")";
        }
        if (bb->fallThrough) {
            LOG(INFO) << "  Fallthrough : block " << bb->fallThrough->id <<
                 " (0x" << std::hex << bb->fallThrough->startOffset << ")";
        }
    }
}

/*
 * Dump the current stats of the compiler.
 */
void oatDumpStats(void)
{
    oatArchDump();
}

/*
 * Allocate a bit vector with enough space to hold at least the specified
 * number of bits.
 *
 * NOTE: memory is allocated from the compiler arena.
 */
ArenaBitVector* oatAllocBitVector(unsigned int startBits, bool expandable)
{
    ArenaBitVector* bv;
    unsigned int count;

    assert(sizeof(bv->storage[0]) == 4);        /* assuming 32-bit units */

    bv = (ArenaBitVector*) oatNew(sizeof(ArenaBitVector), false);

    count = (startBits + 31) >> 5;

    bv->storageSize = count;
    bv->expandable = expandable;
    bv->storage = (u4*) oatNew(count * sizeof(u4), true);
    return bv;
}

/*
 * Determine whether or not the specified bit is set.
 */
bool oatIsBitSet(const ArenaBitVector* pBits, unsigned int num)
{
    assert(num < pBits->storageSize * sizeof(u4) * 8);

    unsigned int val = pBits->storage[num >> 5] & (1 << (num & 0x1f));
    return (val != 0);
}

/*
 * Mark all bits bit as "clear".
 */
void oatClearAllBits(ArenaBitVector* pBits)
{
    unsigned int count = pBits->storageSize;
    memset(pBits->storage, 0, count * sizeof(u4));
}

/*
 * Mark the specified bit as "set".
 *
 * Returns "false" if the bit is outside the range of the vector and we're
 * not allowed to expand.
 *
 * NOTE: memory is allocated from the compiler arena.
 */
bool oatSetBit(ArenaBitVector* pBits, unsigned int num)
{
    if (num >= pBits->storageSize * sizeof(u4) * 8) {
        if (!pBits->expandable) {
            LOG(FATAL) << "Can't expand";
        }

        /* Round up to word boundaries for "num+1" bits */
        unsigned int newSize = (num + 1 + 31) >> 5;
        assert(newSize > pBits->storageSize);
        u4 *newStorage = (u4*)oatNew(newSize * sizeof(u4), false);
        memcpy(newStorage, pBits->storage, pBits->storageSize * sizeof(u4));
        memset(&newStorage[pBits->storageSize], 0,
               (newSize - pBits->storageSize) * sizeof(u4));
        pBits->storage = newStorage;
        pBits->storageSize = newSize;
    }

    pBits->storage[num >> 5] |= 1 << (num & 0x1f);
    return true;
}

/*
 * Mark the specified bit as "unset".
 *
 * Returns "false" if the bit is outside the range of the vector and we're
 * not allowed to expand.
 *
 * NOTE: memory is allocated from the compiler arena.
 */
bool oatClearBit(ArenaBitVector* pBits, unsigned int num)
{
    if (num >= pBits->storageSize * sizeof(u4) * 8) {
        LOG(FATAL) << "Attempt to clear a bit not set in the vector yet";;
    }

    pBits->storage[num >> 5] &= ~(1 << (num & 0x1f));
    return true;
}

/*
 * If set is true, mark all bits as 1. Otherwise mark all bits as 0.
 */
void oatMarkAllBits(ArenaBitVector* pBits, bool set)
{
    int value = set ? -1 : 0;
    memset(pBits->storage, value, pBits->storageSize * (int)sizeof(u4));
}

void oatDebugBitVector(char* msg, const ArenaBitVector* bv, int length)
{
    int i;

    LOG(INFO) <<  msg;
    for (i = 0; i < length; i++) {
        if (oatIsBitSet(bv, i)) {
            LOG(INFO) << "    Bit " << i << " is set";
        }
    }
}

void oatAbort(CompilationUnit* cUnit)
{
    LOG(FATAL) << "Compiler aborting";
}

void oatDumpBlockBitVector(const GrowableList* blocks, char* msg,
                           const ArenaBitVector* bv, int length)
{
    int i;

    LOG(INFO) <<  msg;
    for (i = 0; i < length; i++) {
        if (oatIsBitSet(bv, i)) {
            BasicBlock *bb =
                (BasicBlock *) oatGrowableListGetElement(blocks, i);
            char blockName[BLOCK_NAME_LEN];
            oatGetBlockName(bb, blockName);
            LOG(INFO) << "Bit " << i << " / " << blockName << " is set";
        }
    }
}
/* Initialize the iterator structure */
void oatBitVectorIteratorInit(ArenaBitVector* pBits,
                              ArenaBitVectorIterator* iterator)
{
    iterator->pBits = pBits;
    iterator->bitSize = pBits->storageSize * sizeof(u4) * 8;
    iterator->idx = 0;
}

/*
 * If the vector sizes don't match, log an error and abort.
 */
static void checkSizes(const ArenaBitVector* bv1, const ArenaBitVector* bv2)
{
    if (bv1->storageSize != bv2->storageSize) {
        LOG(FATAL) << "Mismatched vector sizes (" << bv1->storageSize <<
            ", " << bv2->storageSize << ")";
    }
}

/*
 * Copy a whole vector to the other. Only do that when the both vectors have
 * the same size.
 */
void oatCopyBitVector(ArenaBitVector* dest, const ArenaBitVector* src)
{
    /* if dest is expandable and < src, we could expand dest to match */
    checkSizes(dest, src);

    memcpy(dest->storage, src->storage, sizeof(u4) * dest->storageSize);
}

/*
 * Intersect two bit vectors and store the result to the dest vector.
 */

bool oatIntersectBitVectors(ArenaBitVector* dest, const ArenaBitVector* src1,
                            const ArenaBitVector* src2)
{
    if (dest->storageSize != src1->storageSize ||
        dest->storageSize != src2->storageSize ||
        dest->expandable != src1->expandable ||
        dest->expandable != src2->expandable)
        return false;

    unsigned int idx;
    for (idx = 0; idx < dest->storageSize; idx++) {
        dest->storage[idx] = src1->storage[idx] & src2->storage[idx];
    }
    return true;
}

/*
 * Unify two bit vectors and store the result to the dest vector.
 */
bool oatUnifyBitVectors(ArenaBitVector* dest, const ArenaBitVector* src1,
                        const ArenaBitVector* src2)
{
    if (dest->storageSize != src1->storageSize ||
        dest->storageSize != src2->storageSize ||
        dest->expandable != src1->expandable ||
        dest->expandable != src2->expandable)
        return false;

    unsigned int idx;
    for (idx = 0; idx < dest->storageSize; idx++) {
        dest->storage[idx] = src1->storage[idx] | src2->storage[idx];
    }
    return true;
}

/*
 * Compare two bit vectors and return true if difference is seen.
 */
bool oatCompareBitVectors(const ArenaBitVector* src1,
                          const ArenaBitVector* src2)
{
    if (src1->storageSize != src2->storageSize ||
        src1->expandable != src2->expandable)
        return true;

    unsigned int idx;
    for (idx = 0; idx < src1->storageSize; idx++) {
        if (src1->storage[idx] != src2->storage[idx]) return true;
    }
    return false;
}

/*
 * Count the number of bits that are set.
 */
int oatCountSetBits(const ArenaBitVector* pBits)
{
    unsigned int word;
    unsigned int count = 0;

    for (word = 0; word < pBits->storageSize; word++) {
        u4 val = pBits->storage[word];

        if (val != 0) {
            if (val == 0xffffffff) {
                count += 32;
            } else {
                /* count the number of '1' bits */
                while (val != 0) {
                    val &= val - 1;
                    count++;
                }
            }
        }
    }

    return count;
}

/* Return the next position set to 1. -1 means end-of-element reached */
int oatBitVectorIteratorNext(ArenaBitVectorIterator* iterator)
{
    const ArenaBitVector* pBits = iterator->pBits;
    u4 bitIndex = iterator->idx;

    assert(iterator->bitSize == pBits->storageSize * sizeof(u4) * 8);
    if (bitIndex >= iterator->bitSize) return -1;

    for (; bitIndex < iterator->bitSize; bitIndex++) {
        unsigned int wordIndex = bitIndex >> 5;
        unsigned int mask = 1 << (bitIndex & 0x1f);
        if (pBits->storage[wordIndex] & mask) {
            iterator->idx = bitIndex+1;
            return bitIndex;
        }
    }
    /* No more set bits */
    return -1;
}

/*
 * Mark specified number of bits as "set". Cannot set all bits like ClearAll
 * since there might be unused bits - setting those to one will confuse the
 * iterator.
 */
void oatSetInitialBits(ArenaBitVector* pBits, unsigned int numBits)
{
    unsigned int idx;
    assert(((numBits + 31) >> 5) <= pBits->storageSize);
    for (idx = 0; idx < (numBits >> 5); idx++) {
        pBits->storage[idx] = -1;
    }
    unsigned int remNumBits = numBits & 0x1f;
    if (remNumBits) {
        pBits->storage[idx] = (1 << remNumBits) - 1;
    }
}

void oatGetBlockName(BasicBlock* bb, char* name)
{
    switch (bb->blockType) {
        case kEntryBlock:
            snprintf(name, BLOCK_NAME_LEN, "entry");
            break;
        case kExitBlock:
            snprintf(name, BLOCK_NAME_LEN, "exit");
            break;
        case kDalvikByteCode:
            snprintf(name, BLOCK_NAME_LEN, "block%04x", bb->startOffset);
            break;
        case kExceptionHandling:
            snprintf(name, BLOCK_NAME_LEN, "exception%04x", bb->startOffset);
            break;
        default:
            snprintf(name, BLOCK_NAME_LEN, "??");
            break;
    }
}
