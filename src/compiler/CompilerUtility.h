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

#ifndef ART_SRC_COMPILER_COMPILER_UTILITY_H_
#define ART_SRC_COMPILER_COMPILER_UTILITY_H_

#include "Dalvik.h"

/* Each arena page has some overhead, so take a few bytes off 8k */
#define ARENA_DEFAULT_SIZE 8100

/* Allocate the initial memory block for arena-based allocation */
bool oatHeapInit(void);

typedef struct ArenaMemBlock {
    size_t blockSize;
    size_t bytesAllocated;
    struct ArenaMemBlock *next;
    char ptr[0];
} ArenaMemBlock;

void* oatNew(size_t size, bool zero);

void oatArenaReset(void);

typedef struct GrowableList {
    size_t numAllocated;
    size_t numUsed;
    intptr_t *elemList;
} GrowableList;

typedef struct GrowableListIterator {
    GrowableList* list;
    size_t idx;
    size_t size;
} GrowableListIterator;

/*
 * Expanding bitmap, used for tracking resources.  Bits are numbered starting
 * from zero.
 *
 * All operations on a BitVector are unsynchronized.
 */
struct ArenaBitVector {
    bool    expandable;     /* expand bitmap if we run out? */
    u4      storageSize;    /* current size, in 32-bit words */
    u4*     storage;
};

/* Handy iterator to walk through the bit positions set to 1 */
struct ArenaBitVectorIterator {
    ArenaBitVector* pBits;
    u4 idx;
    u4 bitSize;
};

#define GET_ELEM_N(LIST, TYPE, N) (((TYPE*) LIST->elemList)[N])

#define BLOCK_NAME_LEN 80

/* Forward declarations */
struct LIR;
struct BasicBlock;
struct CompilationUnit;

void oatInitGrowableList(GrowableList* gList, size_t initLength);
void oatInsertGrowableList(GrowableList* gList, intptr_t elem);
void oatGrowableListIteratorInit(GrowableList* gList,
                                 GrowableListIterator* iterator);
intptr_t oatGrowableListIteratorNext(GrowableListIterator* iterator);
intptr_t oatGrowableListGetElement(const GrowableList* gList, size_t idx);

ArenaBitVector* oatAllocBitVector(unsigned int startBits, bool expandable);
void oatBitVectorIteratorInit(ArenaBitVector* pBits,
                              ArenaBitVectorIterator* iterator);
int oatBitVectorIteratorNext(ArenaBitVectorIterator* iterator);
bool oatSetBit(ArenaBitVector* pBits, unsigned int num);
bool oatClearBit(ArenaBitVector* pBits, unsigned int num);
void oatMarkAllBits(ArenaBitVector* pBits, bool set);
void oatDebugBitVector(char* msg, const ArenaBitVector* bv, int length);
bool oatIsBitSet(const ArenaBitVector* pBits, unsigned int num);
void oatClearAllBits(ArenaBitVector* pBits);
void oatSetInitialBits(ArenaBitVector* pBits, unsigned int numBits);
void oatCopyBitVector(ArenaBitVector* dest, const ArenaBitVector* src);
bool oatIntersectBitVectors(ArenaBitVector* dest, const ArenaBitVector* src1,
                            const ArenaBitVector* src2);
bool oatUnifyBitVectors(ArenaBitVector* dest, const ArenaBitVector* src1,
                        const ArenaBitVector* src2);
bool oatCompareBitVectors(const ArenaBitVector* src1,
                          const ArenaBitVector* src2);
int oatCountSetBits(const ArenaBitVector* pBits);

void oatDumpLIRInsn(CompilationUnit* cUnit, struct LIR* lir,
                    unsigned char* baseAddr);
void oatDumpResourceMask(struct LIR* lir, u8 mask, const char* prefix);
void oatDumpBlockBitVector(const GrowableList* blocks, char* msg,
                           const ArenaBitVector* bv, int length);
void oatGetBlockName(struct BasicBlock* bb, char* name);
const char* oatGetShortyFromTargetIdx(CompilationUnit*, int);
void oatDumpRegLocTable(struct RegLocation*, int);

#endif  // ART_SRC_COMPILER_COMPILER_UTILITY_H_
