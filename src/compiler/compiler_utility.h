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
enum oat_alloc_kind {
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
enum oat_list_kind {
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
enum oat_bit_map_kind {
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
bool HeapInit(CompilationUnit* cu);

/* Collect memory usage statistics */
//#define WITH_MEMSTATS

struct ArenaMemBlock {
  size_t block_size;
  size_t bytes_allocated;
  ArenaMemBlock *next;
  char ptr[0];
};

void* NewMem(CompilationUnit* cu, size_t size, bool zero, oat_alloc_kind kind);

void ArenaReset(CompilationUnit *cu);

struct GrowableList {
  GrowableList() : num_allocated(0), num_used(0), elem_list(NULL) {
  }

  size_t num_allocated;
  size_t num_used;
  uintptr_t* elem_list;
#ifdef WITH_MEMSTATS
  oat_list_kind kind;
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
  uint32_t   storage_size;    /* current size, in 32-bit words */
  uint32_t*  storage;
#ifdef WITH_MEMSTATS
  oat_bit_map_kind kind;      /* for memory use tuning */
#endif
};

/* Handy iterator to walk through the bit positions set to 1 */
struct ArenaBitVectorIterator {
  ArenaBitVector* p_bits;
  uint32_t idx;
  uint32_t bit_size;
};

#define GET_ELEM_N(LIST, TYPE, N) ((reinterpret_cast<TYPE*>(LIST->elem_list)[N]))

#define BLOCK_NAME_LEN 80

/* Forward declarations */
struct BasicBlock;
struct CompilationUnit;
struct LIR;
struct RegLocation;

void CompilerInitGrowableList(CompilationUnit* cu, GrowableList* g_list,
                         size_t init_length, oat_list_kind kind = kListMisc);
void InsertGrowableList(CompilationUnit* cu, GrowableList* g_list,
                           uintptr_t elem);
void DeleteGrowableList(GrowableList* g_list, uintptr_t elem);
void GrowableListIteratorInit(GrowableList* g_list,
                                 GrowableListIterator* iterator);
uintptr_t GrowableListIteratorNext(GrowableListIterator* iterator);
uintptr_t GrowableListGetElement(const GrowableList* g_list, size_t idx);

ArenaBitVector* AllocBitVector(CompilationUnit* cu,
                                  unsigned int start_bits, bool expandable,
                                  oat_bit_map_kind = kBitMapMisc);
void BitVectorIteratorInit(ArenaBitVector* p_bits,
                              ArenaBitVectorIterator* iterator);
int BitVectorIteratorNext(ArenaBitVectorIterator* iterator);
bool SetBit(CompilationUnit *cu, ArenaBitVector* p_bits, unsigned int num);
bool ClearBit(ArenaBitVector* p_bits, unsigned int num);
void MarkAllBits(ArenaBitVector* p_bits, bool set);
void DebugBitVector(char* msg, const ArenaBitVector* bv, int length);
bool IsBitSet(const ArenaBitVector* p_bits, unsigned int num);
void ClearAllBits(ArenaBitVector* p_bits);
void SetInitialBits(ArenaBitVector* p_bits, unsigned int num_bits);
void CopyBitVector(ArenaBitVector* dest, const ArenaBitVector* src);
bool IntersectBitVectors(ArenaBitVector* dest, const ArenaBitVector* src1,
                            const ArenaBitVector* src2);
bool UnifyBitVetors(ArenaBitVector* dest, const ArenaBitVector* src1,
                        const ArenaBitVector* src2);
bool CompareBitVectors(const ArenaBitVector* src1,
                          const ArenaBitVector* src2);
bool TestBitVectors(const ArenaBitVector* src1, const ArenaBitVector* src2);
int CountSetBits(const ArenaBitVector* p_bits);

void DumpLIRInsn(CompilationUnit* cu, LIR* lir, unsigned char* base_addr);
void DumpResourceMask(LIR* lir, uint64_t mask, const char* prefix);
void DumpBlockBitVector(const GrowableList* blocks, char* msg,
                           const ArenaBitVector* bv, int length);
void GetBlockName(BasicBlock* bb, char* name);
const char* GetShortyFromTargetIdx(CompilationUnit*, int);
void DumpMemStats(CompilationUnit* cu);

}  // namespace art

#endif  // ART_SRC_COMPILER_COMPILER_UTILITY_H_
