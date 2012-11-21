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

#include <stdint.h>
#include <stddef.h>

namespace art {

struct CompilationUnit;

/* Each arena page has some overhead, so take a few bytes off */
#define ARENA_DEFAULT_SIZE ((2 * 1024 * 1024) - 256)

/* Type of allocation for memory tuning */
enum oatAllocKind {
  kAllocMisc,
  kAllocBB,
  kAllocLIR,
  kAllocMIR,
  kAllocDFInfo,
  kAllocGrowableList,
  kAllocGrowableBitMap,
  kAllocDalvikToSSAMap,
  kAllocDebugInfo,
  kAllocSuccessor,
  kAllocRegAlloc,
  kAllocData,
  kAllocPredecessors,
  kNumAllocKinds
};

/* Type of growable list for memory tuning */
enum oatListKind {
  kListMisc = 0,
  kListBlockList,
  kListSSAtoDalvikMap,
  kListDfsOrder,
  kListDfsPostOrder,
  kListDomPostOrderTraversal,
  kListThrowLaunchPads,
  kListSuspendLaunchPads,
  kListSwitchTables,
  kListFillArrayData,
  kListSuccessorBlocks,
  kListPredecessors,
  kNumListKinds
};

/* Type of growable bitmap for memory tuning */
enum oatBitMapKind {
  kBitMapMisc = 0,
  kBitMapUse,
  kBitMapDef,
  kBitMapLiveIn,
  kBitMapBMatrix,
  kBitMapDominators,
  kBitMapIDominated,
  kBitMapDomFrontier,
  kBitMapPhi,
  kBitMapTmpBlocks,
  kBitMapInputBlocks,
  kBitMapRegisterV,
  kBitMapTempSSARegisterV,
  kBitMapNullCheck,
  kBitMapTmpBlockV,
  kBitMapPredecessors,
  kNumBitMapKinds
};

/* Allocate the initial memory block for arena-based allocation */
bool HeapInit(CompilationUnit* cUnit);

/* Collect memory usage statistics */
//#define WITH_MEMSTATS

struct ArenaMemBlock {
  size_t blockSize;
  size_t bytesAllocated;
  ArenaMemBlock *next;
  char ptr[0];
};

void* NewMem(CompilationUnit* cUnit, size_t size, bool zero, oatAllocKind kind);

void ArenaReset(CompilationUnit *cUnit);

struct GrowableList {
  GrowableList() : numAllocated(0), numUsed(0), elemList(NULL) {
  }

  size_t numAllocated;
  size_t numUsed;
  uintptr_t* elemList;
#ifdef WITH_MEMSTATS
  oatListKind kind;
#endif
};

struct GrowableListIterator {
  GrowableList* list;
  size_t idx;
  size_t size;
};

/*
 * Expanding bitmap, used for tracking resources.  Bits are numbered starting
 * from zero.
 *
 * All operations on a BitVector are unsynchronized.
 */
struct ArenaBitVector {
  bool       expandable;     /* expand bitmap if we run out? */
  uint32_t   storageSize;    /* current size, in 32-bit words */
  uint32_t*  storage;
#ifdef WITH_MEMSTATS
  oatBitMapKind kind;      /* for memory use tuning */
#endif
};

/* Handy iterator to walk through the bit positions set to 1 */
struct ArenaBitVectorIterator {
  ArenaBitVector* pBits;
  uint32_t idx;
  uint32_t bitSize;
};

#define GET_ELEM_N(LIST, TYPE, N) ((reinterpret_cast<TYPE*>(LIST->elemList)[N]))

#define BLOCK_NAME_LEN 80

/* Forward declarations */
struct BasicBlock;
struct CompilationUnit;
struct LIR;
struct RegLocation;

void CompilerInitGrowableList(CompilationUnit* cUnit,GrowableList* gList,
                         size_t initLength, oatListKind kind = kListMisc);
void InsertGrowableList(CompilationUnit* cUnit, GrowableList* gList,
                           uintptr_t elem);
void DeleteGrowableList(GrowableList* gList, uintptr_t elem);
void GrowableListIteratorInit(GrowableList* gList,
                                 GrowableListIterator* iterator);
uintptr_t GrowableListIteratorNext(GrowableListIterator* iterator);
uintptr_t GrowableListGetElement(const GrowableList* gList, size_t idx);

ArenaBitVector* AllocBitVector(CompilationUnit* cUnit,
                                  unsigned int startBits, bool expandable,
                                  oatBitMapKind = kBitMapMisc);
void BitVectorIteratorInit(ArenaBitVector* pBits,
                              ArenaBitVectorIterator* iterator);
int BitVectorIteratorNext(ArenaBitVectorIterator* iterator);
bool SetBit(CompilationUnit *cUnit, ArenaBitVector* pBits, unsigned int num);
bool ClearBit(ArenaBitVector* pBits, unsigned int num);
void MarkAllBits(ArenaBitVector* pBits, bool set);
void DebugBitVector(char* msg, const ArenaBitVector* bv, int length);
bool IsBitSet(const ArenaBitVector* pBits, unsigned int num);
void ClearAllBits(ArenaBitVector* pBits);
void SetInitialBits(ArenaBitVector* pBits, unsigned int numBits);
void CopyBitVector(ArenaBitVector* dest, const ArenaBitVector* src);
bool IntersectBitVectors(ArenaBitVector* dest, const ArenaBitVector* src1,
                            const ArenaBitVector* src2);
bool UnifyBitVetors(ArenaBitVector* dest, const ArenaBitVector* src1,
                        const ArenaBitVector* src2);
bool CompareBitVectors(const ArenaBitVector* src1,
                          const ArenaBitVector* src2);
bool TestBitVectors(const ArenaBitVector* src1, const ArenaBitVector* src2);
int CountSetBits(const ArenaBitVector* pBits);

void DumpLIRInsn(CompilationUnit* cUnit, LIR* lir, unsigned char* baseAddr);
void DumpResourceMask(LIR* lir, uint64_t mask, const char* prefix);
void DumpBlockBitVector(const GrowableList* blocks, char* msg,
                           const ArenaBitVector* bv, int length);
void GetBlockName(BasicBlock* bb, char* name);
const char* GetShortyFromTargetIdx(CompilationUnit*, int);
void DumpMemStats(CompilationUnit* cUnit);

}  // namespace art

#endif  // ART_SRC_COMPILER_COMPILER_UTILITY_H_
