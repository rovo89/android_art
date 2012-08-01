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

namespace art {

#ifdef WITH_MEMSTATS
struct Memstats {
  u4 allocStats[kNumAllocKinds];
  int listSizes[kNumListKinds];
  int listWasted[kNumListKinds];
  int listGrows[kNumListKinds];
  int listMaxElems[kNumListKinds];
  int bitMapSizes[kNumBitMapKinds];
  int bitMapWasted[kNumBitMapKinds];
  int bitMapGrows[kNumBitMapKinds];
};

const char* allocNames[kNumAllocKinds] = {
  "Misc       ",
  "BasicBlock ",
  "LIR        ",
  "MIR        ",
  "DataFlow   ",
  "GrowList   ",
  "GrowBitMap ",
  "Dalvik2SSA ",
  "DebugInfo  ",
  "Successor  ",
  "RegAlloc   ",
  "Data       ",
  "Preds      ",
};

const char* listNames[kNumListKinds] = {
  "Misc                  ",
  "blockList             ",
  "SSAtoDalvik           ",
  "dfsOrder              ",
  "dfsPostOrder          ",
  "domPostOrderTraversal ",
  "throwLaunchPads       ",
  "suspendLaunchPads     ",
  "switchTables          ",
  "fillArrayData         ",
  "SuccessorBlocks       ",
  "Predecessors          ",
};

const char* bitMapNames[kNumBitMapKinds] = {
  "Misc                  ",
  "Use                   ",
  "Def                   ",
  "LiveIn                ",
  "BlockMatrix           ",
  "Dominators            ",
  "IDominated            ",
  "DomFrontier           ",
  "Phi                   ",
  "TmpBlocks             ",
  "InputBlocks           ",
  "RegisterV             ",
  "TempSSARegisterV      ",
  "Null Check            ",
  "TmpBlockV             ",
  "Predecessors          ",
};
#endif

#define kArenaBitVectorGrowth    4   /* increase by 4 u4s when limit hit */

/* Allocate the initial memory block for arena-based allocation */
bool oatHeapInit(CompilationUnit* cUnit)
{
  DCHECK(cUnit->arenaHead == NULL);
  cUnit->arenaHead =
      (ArenaMemBlock *) malloc(sizeof(ArenaMemBlock) + ARENA_DEFAULT_SIZE);
  if (cUnit->arenaHead == NULL) {
    LOG(FATAL) << "No memory left to create compiler heap memory";
  }
  cUnit->arenaHead->blockSize = ARENA_DEFAULT_SIZE;
  cUnit->currentArena = cUnit->arenaHead;
  cUnit->currentArena->bytesAllocated = 0;
  cUnit->currentArena->next = NULL;
  cUnit->numArenaBlocks = 1;
#ifdef WITH_MEMSTATS
  cUnit->mstats = (Memstats*) oatNew(cUnit, sizeof(Memstats), true,
                   kAllocDebugInfo);
#endif
  return true;
}

/* Arena-based malloc for compilation tasks */
void* oatNew(CompilationUnit* cUnit, size_t size, bool zero, oatAllocKind kind)
{
  size = (size + 3) & ~3;
#ifdef WITH_MEMSTATS
  if (cUnit->mstats != NULL) {
    cUnit->mstats->allocStats[kind] += size;
  }
#endif
retry:
  /* Normal case - space is available in the current page */
  if (size + cUnit->currentArena->bytesAllocated <=
    cUnit->currentArena->blockSize) {
    void *ptr;
    ptr = &cUnit->currentArena->ptr[cUnit->currentArena->bytesAllocated];
    cUnit->currentArena->bytesAllocated += size;
    if (zero) {
      memset(ptr, 0, size);
    }
    return ptr;
  } else {
    /*
     * See if there are previously allocated arena blocks before the last
     * reset
     */
    if (cUnit->currentArena->next) {
        cUnit->currentArena = cUnit->currentArena->next;
        cUnit->currentArena->bytesAllocated = 0;
        goto retry;
    }

    size_t blockSize = (size < ARENA_DEFAULT_SIZE) ?  ARENA_DEFAULT_SIZE : size;
    /* Time to allocate a new arena */
    ArenaMemBlock *newArena = (ArenaMemBlock *)
        malloc(sizeof(ArenaMemBlock) + blockSize);
    if (newArena == NULL) {
      LOG(FATAL) << "Arena allocation failure";
    }
    newArena->blockSize = blockSize;
    newArena->bytesAllocated = 0;
    newArena->next = NULL;
    cUnit->currentArena->next = newArena;
    cUnit->currentArena = newArena;
    cUnit->numArenaBlocks++;
    if (cUnit->numArenaBlocks > 20000) {
      LOG(INFO) << "Total arena pages: " << cUnit->numArenaBlocks;
    }
    goto retry;
  }
}

/* Reclaim all the arena blocks allocated so far */
void oatArenaReset(CompilationUnit* cUnit)
{
  ArenaMemBlock* head = cUnit->arenaHead;
  while (head != NULL) {
    ArenaMemBlock* p = head;
    head = head->next;
    free(p);
  }
  cUnit->arenaHead = NULL;
  cUnit->currentArena = NULL;
}

/* Growable List initialization */
void oatInitGrowableList(CompilationUnit* cUnit, GrowableList* gList,
                       size_t initLength, oatListKind kind)
{
  gList->numAllocated = initLength;
  gList->numUsed = 0;
  gList->elemList = (intptr_t *) oatNew(cUnit, sizeof(intptr_t) * initLength,
                                        true, kAllocGrowableList);
#ifdef WITH_MEMSTATS
  cUnit->mstats->listSizes[kind] += sizeof(intptr_t) * initLength;
  gList->kind = kind;
  if ((int)initLength > cUnit->mstats->listMaxElems[kind]) {
    cUnit->mstats->listMaxElems[kind] = initLength;
  }
#endif
}

/* Expand the capacity of a growable list */
void expandGrowableList(CompilationUnit* cUnit, GrowableList* gList)
{
  int newLength = gList->numAllocated;
  if (newLength < 128) {
    newLength <<= 1;
  } else {
    newLength += 128;
  }
  intptr_t *newArray =
      (intptr_t *) oatNew(cUnit, sizeof(intptr_t) * newLength, true,
                          kAllocGrowableList);
  memcpy(newArray, gList->elemList, sizeof(intptr_t) * gList->numAllocated);
#ifdef WITH_MEMSTATS
  cUnit->mstats->listSizes[gList->kind] += sizeof(intptr_t) * newLength;
  cUnit->mstats->listWasted[gList->kind] +=
      sizeof(intptr_t) * gList->numAllocated;
  cUnit->mstats->listGrows[gList->kind]++;
  if (newLength > cUnit->mstats->listMaxElems[gList->kind]) {
    cUnit->mstats->listMaxElems[gList->kind] = newLength;
  }
#endif
  gList->numAllocated = newLength;
  gList->elemList = newArray;
}

/* Insert a new element into the growable list */
void oatInsertGrowableList(CompilationUnit* cUnit, GrowableList* gList,
                         intptr_t elem)
{
  DCHECK_NE(gList->numAllocated, 0U);
  if (gList->numUsed == gList->numAllocated) {
    expandGrowableList(cUnit, gList);
  }
  gList->elemList[gList->numUsed++] = elem;
}

/* Delete an element from a growable list. Element must be present */
void oatDeleteGrowableList(GrowableList* gList, intptr_t elem)
{
  bool found = false;
  for (unsigned int i = 0; i < gList->numUsed; i++) {
    if (!found && gList->elemList[i] == elem) {
      found = true;
    }
    if (found) {
      gList->elemList[i] = gList->elemList[i+1];
    }
  }
  DCHECK_EQ(found, true);
  gList->numUsed--;
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
  DCHECK_EQ(iterator->size, iterator->list->numUsed);
  if (iterator->idx == iterator->size) return 0;
  return iterator->list->elemList[iterator->idx++];
}

intptr_t oatGrowableListGetElement(const GrowableList* gList, size_t idx)
{
  DCHECK_LT(idx, gList->numUsed);
  return gList->elemList[idx];
}

#ifdef WITH_MEMSTATS
/* Dump memory usage stats */
void oatDumpMemStats(CompilationUnit* cUnit)
{
  u4 total = 0;
  for (int i = 0; i < kNumAllocKinds; i++) {
    total += cUnit->mstats->allocStats[i];
  }
  if (total > (10 * 1024 * 1024)) {
    LOG(INFO) << "MEMUSAGE: " << total << " : "
        << PrettyMethod(cUnit->method_idx, *cUnit->dex_file);
    LOG(INFO) << "insnsSize: " << cUnit->insnsSize;
    if (cUnit->disableDataflow) {
        LOG(INFO) << " ** Dataflow disabled ** ";
    }
    LOG(INFO) << "===== Overall allocations";
    for (int i = 0; i < kNumAllocKinds; i++) {
        LOG(INFO) << allocNames[i] << std::setw(10) <<
        cUnit->mstats->allocStats[i];
    }
    LOG(INFO) << "===== GrowableList allocations";
    for (int i = 0; i < kNumListKinds; i++) {
      LOG(INFO) << listNames[i]
                << " S:" << cUnit->mstats->listSizes[i]
                << ", W:" << cUnit->mstats->listWasted[i]
                << ", G:" << cUnit->mstats->listGrows[i]
                << ", E:" << cUnit->mstats->listMaxElems[i];
    }
    LOG(INFO) << "===== GrowableBitMap allocations";
    for (int i = 0; i < kNumBitMapKinds; i++) {
      LOG(INFO) << bitMapNames[i]
                << " S:" << cUnit->mstats->bitMapSizes[i]
                << ", W:" << cUnit->mstats->bitMapWasted[i]
                << ", G:" << cUnit->mstats->bitMapGrows[i];
    }
  }
}
#endif

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

  LOG(INFO) << "Compiling " << PrettyMethod(cUnit->method_idx, *cUnit->dex_file);
  LOG(INFO) << cUnit->insns << " insns";
  LOG(INFO) << cUnit->numBlocks << " blocks in total";
  GrowableListIterator iterator;

  oatGrowableListIteratorInit(&cUnit->blockList, &iterator);

  while (true) {
    bb = (BasicBlock *) oatGrowableListIteratorNext(&iterator);
    if (bb == NULL) break;
    LOG(INFO) << StringPrintf("Block %d (%s) (insn %04x - %04x%s)",
        bb->id,
        blockTypeNames[bb->blockType],
        bb->startOffset,
        bb->lastMIRInsn ? bb->lastMIRInsn->offset : bb->startOffset,
        bb->lastMIRInsn ? "" : " empty");
    if (bb->taken) {
      LOG(INFO) << "  Taken branch: block " << bb->taken->id
                << "(0x" << std::hex << bb->taken->startOffset << ")";
    }
    if (bb->fallThrough) {
      LOG(INFO) << "  Fallthrough : block " << bb->fallThrough->id
                << " (0x" << std::hex << bb->fallThrough->startOffset << ")";
    }
  }
}

static uint32_t checkMasks[32] = {
  0x00000001, 0x00000002, 0x00000004, 0x00000008, 0x00000010,
  0x00000020, 0x00000040, 0x00000080, 0x00000100, 0x00000200,
  0x00000400, 0x00000800, 0x00001000, 0x00002000, 0x00004000,
  0x00008000, 0x00010000, 0x00020000, 0x00040000, 0x00080000,
  0x00100000, 0x00200000, 0x00400000, 0x00800000, 0x01000000,
  0x02000000, 0x04000000, 0x08000000, 0x10000000, 0x20000000,
  0x40000000, 0x80000000 };

/*
 * Allocate a bit vector with enough space to hold at least the specified
 * number of bits.
 *
 * NOTE: memory is allocated from the compiler arena.
 */
ArenaBitVector* oatAllocBitVector(CompilationUnit* cUnit,
                                unsigned int startBits, bool expandable,
                                oatBitMapKind kind)
{
  ArenaBitVector* bv;
  unsigned int count;

  DCHECK_EQ(sizeof(bv->storage[0]), 4U);        /* assuming 32-bit units */

  bv = (ArenaBitVector*) oatNew(cUnit, sizeof(ArenaBitVector), false,
                                kAllocGrowableBitMap);

  count = (startBits + 31) >> 5;

  bv->storageSize = count;
  bv->expandable = expandable;
  bv->storage = (u4*) oatNew(cUnit, count * sizeof(u4), true,
                             kAllocGrowableBitMap);
#ifdef WITH_MEMSTATS
  bv->kind = kind;
  cUnit->mstats->bitMapSizes[kind] += count * sizeof(u4);
#endif
  return bv;
}

/*
 * Determine whether or not the specified bit is set.
 */
bool oatIsBitSet(const ArenaBitVector* pBits, unsigned int num)
{
  DCHECK_LT(num, pBits->storageSize * sizeof(u4) * 8);

  unsigned int val = pBits->storage[num >> 5] & checkMasks[num & 0x1f];
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
bool oatSetBit(CompilationUnit* cUnit, ArenaBitVector* pBits, unsigned int num)
{
  if (num >= pBits->storageSize * sizeof(u4) * 8) {
    if (!pBits->expandable) {
      LOG(FATAL) << "Can't expand";
    }

    /* Round up to word boundaries for "num+1" bits */
    unsigned int newSize = (num + 1 + 31) >> 5;
    DCHECK_GT(newSize, pBits->storageSize);
    u4 *newStorage = (u4*)oatNew(cUnit, newSize * sizeof(u4), false,
                                 kAllocGrowableBitMap);
    memcpy(newStorage, pBits->storage, pBits->storageSize * sizeof(u4));
    memset(&newStorage[pBits->storageSize], 0,
           (newSize - pBits->storageSize) * sizeof(u4));
#ifdef WITH_MEMSTATS
    cUnit->mstats->bitMapWasted[pBits->kind] +=
        pBits->storageSize * sizeof(u4);
    cUnit->mstats->bitMapSizes[pBits->kind] += newSize * sizeof(u4);
    cUnit->mstats->bitMapGrows[pBits->kind]++;
#endif
    pBits->storage = newStorage;
    pBits->storageSize = newSize;
  }

  pBits->storage[num >> 5] |= checkMasks[num & 0x1f];
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

  pBits->storage[num >> 5] &= ~checkMasks[num & 0x1f];
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
      BasicBlock *bb = (BasicBlock *) oatGrowableListGetElement(blocks, i);
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
void checkSizes(const ArenaBitVector* bv1, const ArenaBitVector* bv2)
{
  if (bv1->storageSize != bv2->storageSize) {
    LOG(FATAL) << "Mismatched vector sizes (" << bv1->storageSize
               << ", " << bv2->storageSize << ")";
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
  DCHECK(src1 != NULL);
  DCHECK(src2 != NULL);
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
  DCHECK(src1 != NULL);
  DCHECK(src2 != NULL);
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
 * Return true if any bits collide.  Vectors must be same size.
 */
bool oatTestBitVectors(const ArenaBitVector* src1,
                     const ArenaBitVector* src2)
{
  DCHECK_EQ(src1->storageSize, src2->storageSize);
  for (uint32_t idx = 0; idx < src1->storageSize; idx++) {
    if (src1->storage[idx] & src2->storage[idx]) return true;
  }
  return false;
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
  ArenaBitVector* pBits = iterator->pBits;
  u4 bitIndex = iterator->idx;
  u4 bitSize = iterator->bitSize;

  DCHECK_EQ(bitSize, pBits->storageSize * sizeof(u4) * 8);

  if (bitIndex >= bitSize) return -1;

  u4 wordIndex = bitIndex >> 5;
  u4 endWordIndex = bitSize >> 5;
  u4* storage = pBits->storage;
  u4 word = storage[wordIndex++];

  // Mask out any bits in the first word we've already considered
  word &= ~((1 << (bitIndex & 0x1f))-1);

  for (; wordIndex <= endWordIndex;) {
    u4 bitPos = bitIndex & 0x1f;
    if (word == 0) {
      bitIndex += (32 - bitPos);
      word = storage[wordIndex++];
      continue;
    }
    for (; bitPos < 32; bitPos++) {
      if (word & (1 << bitPos)) {
        iterator->idx = bitIndex + 1;
        return bitIndex;
      }
      bitIndex++;
    }
    word = storage[wordIndex++];
  }
  iterator->idx = iterator->bitSize;
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
  DCHECK_LE(((numBits + 31) >> 5), pBits->storageSize);
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
      snprintf(name, BLOCK_NAME_LEN, "entry_%d", bb->id);
      break;
    case kExitBlock:
      snprintf(name, BLOCK_NAME_LEN, "exit_%d", bb->id);
      break;
    case kDalvikByteCode:
      snprintf(name, BLOCK_NAME_LEN, "block%04x_%d", bb->startOffset, bb->id);
      break;
    case kExceptionHandling:
      snprintf(name, BLOCK_NAME_LEN, "exception%04x_%d", bb->startOffset,
               bb->id);
      break;
    default:
      snprintf(name, BLOCK_NAME_LEN, "??_%d", bb->id);
      break;
  }
}

const char* oatGetShortyFromTargetIdx(CompilationUnit *cUnit, int targetIdx)
{
  const DexFile::MethodId& methodId = cUnit->dex_file->GetMethodId(targetIdx);
  return cUnit->dex_file->GetShorty(methodId.proto_idx_);
}

}  // namespace art
