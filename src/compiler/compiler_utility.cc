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

namespace art {

const char* extended_mir_op_names[kMirOpLast - kMirOpFirst] = {
  "kMirOpPhi",
  "kMirOpCopy",
  "kMirFusedCmplFloat",
  "kMirFusedCmpgFloat",
  "kMirFusedCmplDouble",
  "kMirFusedCmpgDouble",
  "kMirFusedCmpLong",
  "kMirNop",
  "kMirOpNullCheck",
  "kMirOpRangeCheck",
  "kMirOpDivZeroCheck",
  "kMirOpCheck",
};

#ifdef WITH_MEMSTATS
struct Memstats {
  uint32_t alloc_stats[kNumAllocKinds];
  int list_sizes[kNumListKinds];
  int list_wasted[kNumListKinds];
  int list_grows[kNumListKinds];
  int list_max_elems[kNumListKinds];
  int bit_map_sizes[kNumBitMapKinds];
  int bit_map_wasted[kNumBitMapKinds];
  int bit_map_grows[kNumBitMapKinds];
};

const char* alloc_names[kNumAllocKinds] = {
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

const char* list_names[kNumListKinds] = {
  "Misc                  ",
  "block_list             ",
  "SSAtoDalvik           ",
  "dfs_order              ",
  "dfs_post_order          ",
  "dom_post_order_traversal ",
  "throw_launch_pads       ",
  "suspend_launch_pads     ",
  "switch_tables          ",
  "fill_array_data         ",
  "SuccessorBlocks       ",
  "Predecessors          ",
};

const char* bit_map_names[kNumBitMapKinds] = {
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

#define kArenaBitVectorGrowth    4   /* increase by 4 uint32_ts when limit hit */

/* Allocate the initial memory block for arena-based allocation */
bool HeapInit(CompilationUnit* cu)
{
  DCHECK(cu->arena_head == NULL);
  cu->arena_head =
      static_cast<ArenaMemBlock*>(malloc(sizeof(ArenaMemBlock) + ARENA_DEFAULT_SIZE));
  if (cu->arena_head == NULL) {
    LOG(FATAL) << "No memory left to create compiler heap memory";
  }
  cu->arena_head->block_size = ARENA_DEFAULT_SIZE;
  cu->current_arena = cu->arena_head;
  cu->current_arena->bytes_allocated = 0;
  cu->current_arena->next = NULL;
  cu->num_arena_blocks = 1;
#ifdef WITH_MEMSTATS
  cu->mstats = (Memstats*) NewMem(cu, sizeof(Memstats), true,
                   kAllocDebugInfo);
#endif
  return true;
}

/* Arena-based malloc for compilation tasks */
void* NewMem(CompilationUnit* cu, size_t size, bool zero, oat_alloc_kind kind)
{
  size = (size + 3) & ~3;
#ifdef WITH_MEMSTATS
  if (cu->mstats != NULL) {
    cu->mstats->alloc_stats[kind] += size;
  }
#endif
retry:
  /* Normal case - space is available in the current page */
  if (size + cu->current_arena->bytes_allocated <=
    cu->current_arena->block_size) {
    void *ptr;
    ptr = &cu->current_arena->ptr[cu->current_arena->bytes_allocated];
    cu->current_arena->bytes_allocated += size;
    if (zero) {
      memset(ptr, 0, size);
    }
    return ptr;
  } else {
    /*
     * See if there are previously allocated arena blocks before the last
     * reset
     */
    if (cu->current_arena->next) {
        cu->current_arena = cu->current_arena->next;
        cu->current_arena->bytes_allocated = 0;
        goto retry;
    }

    size_t block_size = (size < ARENA_DEFAULT_SIZE) ?  ARENA_DEFAULT_SIZE : size;
    /* Time to allocate a new arena */
    ArenaMemBlock *new_arena =
        static_cast<ArenaMemBlock*>(malloc(sizeof(ArenaMemBlock) + block_size));
    if (new_arena == NULL) {
      LOG(FATAL) << "Arena allocation failure";
    }
    new_arena->block_size = block_size;
    new_arena->bytes_allocated = 0;
    new_arena->next = NULL;
    cu->current_arena->next = new_arena;
    cu->current_arena = new_arena;
    cu->num_arena_blocks++;
    if (cu->num_arena_blocks > 20000) {
      LOG(INFO) << "Total arena pages: " << cu->num_arena_blocks;
    }
    goto retry;
  }
}

/* Reclaim all the arena blocks allocated so far */
void ArenaReset(CompilationUnit* cu)
{
  ArenaMemBlock* head = cu->arena_head;
  while (head != NULL) {
    ArenaMemBlock* p = head;
    head = head->next;
    free(p);
  }
  cu->arena_head = NULL;
  cu->current_arena = NULL;
}

/* Growable List initialization */
void CompilerInitGrowableList(CompilationUnit* cu, GrowableList* g_list,
                       size_t init_length, oat_list_kind kind)
{
  g_list->num_allocated = init_length;
  g_list->num_used = 0;
  g_list->elem_list = static_cast<uintptr_t *>(NewMem(cu, sizeof(intptr_t) * init_length,
                                             true, kAllocGrowableList));
#ifdef WITH_MEMSTATS
  cu->mstats->list_sizes[kind] += sizeof(uintptr_t) * init_length;
  g_list->kind = kind;
  if (static_cast<int>(init_length) > cu->mstats->list_max_elems[kind]) {
    cu->mstats->list_max_elems[kind] = init_length;
  }
#endif
}

/* Expand the capacity of a growable list */
static void ExpandGrowableList(CompilationUnit* cu, GrowableList* g_list)
{
  int new_length = g_list->num_allocated;
  if (new_length < 128) {
    new_length <<= 1;
  } else {
    new_length += 128;
  }
  uintptr_t *new_array =
      static_cast<uintptr_t*>(NewMem(cu, sizeof(uintptr_t) * new_length, true,
                                     kAllocGrowableList));
  memcpy(new_array, g_list->elem_list, sizeof(uintptr_t) * g_list->num_allocated);
#ifdef WITH_MEMSTATS
  cu->mstats->list_sizes[g_list->kind] += sizeof(uintptr_t) * new_length;
  cu->mstats->list_wasted[g_list->kind] +=
      sizeof(uintptr_t) * g_list->num_allocated;
  cu->mstats->list_grows[g_list->kind]++;
  if (new_length > cu->mstats->list_max_elems[g_list->kind]) {
    cu->mstats->list_max_elems[g_list->kind] = new_length;
  }
#endif
  g_list->num_allocated = new_length;
  g_list->elem_list = new_array;
}

/* Insert a new element into the growable list */
void InsertGrowableList(CompilationUnit* cu, GrowableList* g_list,
                           uintptr_t elem)
{
  DCHECK_NE(g_list->num_allocated, 0U);
  if (g_list->num_used == g_list->num_allocated) {
    ExpandGrowableList(cu, g_list);
  }
  g_list->elem_list[g_list->num_used++] = elem;
}

/* Delete an element from a growable list. Element must be present */
void DeleteGrowableList(GrowableList* g_list, uintptr_t elem)
{
  bool found = false;
  for (unsigned int i = 0; i < g_list->num_used; i++) {
    if (!found && g_list->elem_list[i] == elem) {
      found = true;
    }
    if (found) {
      g_list->elem_list[i] = g_list->elem_list[i+1];
    }
  }
  DCHECK_EQ(found, true);
  g_list->num_used--;
}

void GrowableListIteratorInit(GrowableList* g_list,
                               GrowableListIterator* iterator)
{
  iterator->list = g_list;
  iterator->idx = 0;
  iterator->size = g_list->num_used;
}

uintptr_t GrowableListIteratorNext(GrowableListIterator* iterator)
{
  DCHECK_EQ(iterator->size, iterator->list->num_used);
  if (iterator->idx == iterator->size) return 0;
  return iterator->list->elem_list[iterator->idx++];
}

uintptr_t GrowableListGetElement(const GrowableList* g_list, size_t idx)
{
  DCHECK_LT(idx, g_list->num_used);
  return g_list->elem_list[idx];
}

#ifdef WITH_MEMSTATS
/* Dump memory usage stats */
void DumpMemStats(CompilationUnit* cu)
{
  uint32_t total = 0;
  for (int i = 0; i < kNumAllocKinds; i++) {
    total += cu->mstats->alloc_stats[i];
  }
  if (total > (10 * 1024 * 1024)) {
    LOG(INFO) << "MEMUSAGE: " << total << " : "
        << PrettyMethod(cu->method_idx, *cu->dex_file);
    LOG(INFO) << "insns_size: " << cu->insns_size;
    if (cu->disable_dataflow) {
        LOG(INFO) << " ** Dataflow disabled ** ";
    }
    LOG(INFO) << "===== Overall allocations";
    for (int i = 0; i < kNumAllocKinds; i++) {
        LOG(INFO) << alloc_names[i] << std::setw(10) <<
        cu->mstats->alloc_stats[i];
    }
    LOG(INFO) << "===== GrowableList allocations";
    for (int i = 0; i < kNumListKinds; i++) {
      LOG(INFO) << list_names[i]
                << " S:" << cu->mstats->list_sizes[i]
                << ", W:" << cu->mstats->list_wasted[i]
                << ", G:" << cu->mstats->list_grows[i]
                << ", E:" << cu->mstats->list_max_elems[i];
    }
    LOG(INFO) << "===== GrowableBitMap allocations";
    for (int i = 0; i < kNumBitMapKinds; i++) {
      LOG(INFO) << bit_map_names[i]
                << " S:" << cu->mstats->bit_map_sizes[i]
                << ", W:" << cu->mstats->bit_map_wasted[i]
                << ", G:" << cu->mstats->bit_map_grows[i];
    }
  }
}
#endif

/* Debug Utility - dump a compilation unit */
void DumpCompilationUnit(CompilationUnit* cu)
{
  BasicBlock* bb;
  const char* block_type_names[] = {
    "Entry Block",
    "Code Block",
    "Exit Block",
    "Exception Handling",
    "Catch Block"
  };

  LOG(INFO) << "Compiling " << PrettyMethod(cu->method_idx, *cu->dex_file);
  LOG(INFO) << cu->insns << " insns";
  LOG(INFO) << cu->num_blocks << " blocks in total";
  GrowableListIterator iterator;

  GrowableListIteratorInit(&cu->block_list, &iterator);

  while (true) {
    bb = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iterator));
    if (bb == NULL) break;
    LOG(INFO) << StringPrintf("Block %d (%s) (insn %04x - %04x%s)",
        bb->id,
        block_type_names[bb->block_type],
        bb->start_offset,
        bb->last_mir_insn ? bb->last_mir_insn->offset : bb->start_offset,
        bb->last_mir_insn ? "" : " empty");
    if (bb->taken) {
      LOG(INFO) << "  Taken branch: block " << bb->taken->id
                << "(0x" << std::hex << bb->taken->start_offset << ")";
    }
    if (bb->fall_through) {
      LOG(INFO) << "  Fallthrough : block " << bb->fall_through->id
                << " (0x" << std::hex << bb->fall_through->start_offset << ")";
    }
  }
}

static uint32_t check_masks[32] = {
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
ArenaBitVector* AllocBitVector(CompilationUnit* cu,
                                unsigned int start_bits, bool expandable,
                                oat_bit_map_kind kind)
{
  ArenaBitVector* bv;
  unsigned int count;

  DCHECK_EQ(sizeof(bv->storage[0]), 4U);        /* assuming 32-bit units */

  bv = static_cast<ArenaBitVector*>(NewMem(cu, sizeof(ArenaBitVector), false,
                                                kAllocGrowableBitMap));

  count = (start_bits + 31) >> 5;

  bv->storage_size = count;
  bv->expandable = expandable;
  bv->storage = static_cast<uint32_t*>(NewMem(cu, count * sizeof(uint32_t), true,
                                              kAllocGrowableBitMap));
#ifdef WITH_MEMSTATS
  bv->kind = kind;
  cu->mstats->bit_map_sizes[kind] += count * sizeof(uint32_t);
#endif
  return bv;
}

/*
 * Determine whether or not the specified bit is set.
 */
bool IsBitSet(const ArenaBitVector* p_bits, unsigned int num)
{
  DCHECK_LT(num, p_bits->storage_size * sizeof(uint32_t) * 8);

  unsigned int val = p_bits->storage[num >> 5] & check_masks[num & 0x1f];
  return (val != 0);
}

/*
 * Mark all bits bit as "clear".
 */
void ClearAllBits(ArenaBitVector* p_bits)
{
  unsigned int count = p_bits->storage_size;
  memset(p_bits->storage, 0, count * sizeof(uint32_t));
}

/*
 * Mark the specified bit as "set".
 *
 * Returns "false" if the bit is outside the range of the vector and we're
 * not allowed to expand.
 *
 * NOTE: memory is allocated from the compiler arena.
 */
bool SetBit(CompilationUnit* cu, ArenaBitVector* p_bits, unsigned int num)
{
  if (num >= p_bits->storage_size * sizeof(uint32_t) * 8) {
    if (!p_bits->expandable) {
      LOG(FATAL) << "Can't expand";
    }

    /* Round up to word boundaries for "num+1" bits */
    unsigned int new_size = (num + 1 + 31) >> 5;
    DCHECK_GT(new_size, p_bits->storage_size);
    uint32_t *new_storage = static_cast<uint32_t*>(NewMem(cu, new_size * sizeof(uint32_t), false,
                                                         kAllocGrowableBitMap));
    memcpy(new_storage, p_bits->storage, p_bits->storage_size * sizeof(uint32_t));
    memset(&new_storage[p_bits->storage_size], 0,
           (new_size - p_bits->storage_size) * sizeof(uint32_t));
#ifdef WITH_MEMSTATS
    cu->mstats->bit_map_wasted[p_bits->kind] +=
        p_bits->storage_size * sizeof(uint32_t);
    cu->mstats->bit_map_sizes[p_bits->kind] += new_size * sizeof(uint32_t);
    cu->mstats->bit_map_grows[p_bits->kind]++;
#endif
    p_bits->storage = new_storage;
    p_bits->storage_size = new_size;
  }

  p_bits->storage[num >> 5] |= check_masks[num & 0x1f];
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
bool ClearBit(ArenaBitVector* p_bits, unsigned int num)
{
  if (num >= p_bits->storage_size * sizeof(uint32_t) * 8) {
    LOG(FATAL) << "Attempt to clear a bit not set in the vector yet";;
  }

  p_bits->storage[num >> 5] &= ~check_masks[num & 0x1f];
  return true;
}

/* Initialize the iterator structure */
void BitVectorIteratorInit(ArenaBitVector* p_bits,
                            ArenaBitVectorIterator* iterator)
{
  iterator->p_bits = p_bits;
  iterator->bit_size = p_bits->storage_size * sizeof(uint32_t) * 8;
  iterator->idx = 0;
}

/*
 * If the vector sizes don't match, log an error and abort.
 */
static void CheckSizes(const ArenaBitVector* bv1, const ArenaBitVector* bv2)
{
  if (bv1->storage_size != bv2->storage_size) {
    LOG(FATAL) << "Mismatched vector sizes (" << bv1->storage_size
               << ", " << bv2->storage_size << ")";
  }
}

/*
 * Copy a whole vector to the other. Only do that when the both vectors have
 * the same size.
 */
void CopyBitVector(ArenaBitVector* dest, const ArenaBitVector* src)
{
  /* if dest is expandable and < src, we could expand dest to match */
  CheckSizes(dest, src);

  memcpy(dest->storage, src->storage, sizeof(uint32_t) * dest->storage_size);
}

/*
 * Intersect two bit vectors and store the result to the dest vector.
 */

bool IntersectBitVectors(ArenaBitVector* dest, const ArenaBitVector* src1,
                          const ArenaBitVector* src2)
{
  DCHECK(src1 != NULL);
  DCHECK(src2 != NULL);
  if (dest->storage_size != src1->storage_size ||
      dest->storage_size != src2->storage_size ||
      dest->expandable != src1->expandable ||
      dest->expandable != src2->expandable)
    return false;

  unsigned int idx;
  for (idx = 0; idx < dest->storage_size; idx++) {
    dest->storage[idx] = src1->storage[idx] & src2->storage[idx];
  }
  return true;
}

/*
 * Unify two bit vectors and store the result to the dest vector.
 */
bool UnifyBitVetors(ArenaBitVector* dest, const ArenaBitVector* src1,
                      const ArenaBitVector* src2)
{
  DCHECK(src1 != NULL);
  DCHECK(src2 != NULL);
  if (dest->storage_size != src1->storage_size ||
      dest->storage_size != src2->storage_size ||
      dest->expandable != src1->expandable ||
      dest->expandable != src2->expandable)
    return false;

  unsigned int idx;
  for (idx = 0; idx < dest->storage_size; idx++) {
    dest->storage[idx] = src1->storage[idx] | src2->storage[idx];
  }
  return true;
}

/*
 * Return true if any bits collide.  Vectors must be same size.
 */
bool TestBitVectors(const ArenaBitVector* src1,
                     const ArenaBitVector* src2)
{
  DCHECK_EQ(src1->storage_size, src2->storage_size);
  for (uint32_t idx = 0; idx < src1->storage_size; idx++) {
    if (src1->storage[idx] & src2->storage[idx]) return true;
  }
  return false;
}

/*
 * Compare two bit vectors and return true if difference is seen.
 */
bool CompareBitVectors(const ArenaBitVector* src1,
                        const ArenaBitVector* src2)
{
  if (src1->storage_size != src2->storage_size ||
      src1->expandable != src2->expandable)
    return true;

  unsigned int idx;
  for (idx = 0; idx < src1->storage_size; idx++) {
    if (src1->storage[idx] != src2->storage[idx]) return true;
  }
  return false;
}

/*
 * Count the number of bits that are set.
 */
int CountSetBits(const ArenaBitVector* p_bits)
{
  unsigned int word;
  unsigned int count = 0;

  for (word = 0; word < p_bits->storage_size; word++) {
    uint32_t val = p_bits->storage[word];

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
int BitVectorIteratorNext(ArenaBitVectorIterator* iterator)
{
  ArenaBitVector* p_bits = iterator->p_bits;
  uint32_t bit_index = iterator->idx;
  uint32_t bit_size = iterator->bit_size;

  DCHECK_EQ(bit_size, p_bits->storage_size * sizeof(uint32_t) * 8);

  if (bit_index >= bit_size) return -1;

  uint32_t word_index = bit_index >> 5;
  uint32_t end_word_index = bit_size >> 5;
  uint32_t* storage = p_bits->storage;
  uint32_t word = storage[word_index++];

  // Mask out any bits in the first word we've already considered
  word &= ~((1 << (bit_index & 0x1f))-1);

  for (; word_index <= end_word_index;) {
    uint32_t bit_pos = bit_index & 0x1f;
    if (word == 0) {
      bit_index += (32 - bit_pos);
      word = storage[word_index++];
      continue;
    }
    for (; bit_pos < 32; bit_pos++) {
      if (word & (1 << bit_pos)) {
        iterator->idx = bit_index + 1;
        return bit_index;
      }
      bit_index++;
    }
    word = storage[word_index++];
  }
  iterator->idx = iterator->bit_size;
  return -1;
}

/*
 * Mark specified number of bits as "set". Cannot set all bits like ClearAll
 * since there might be unused bits - setting those to one will confuse the
 * iterator.
 */
void SetInitialBits(ArenaBitVector* p_bits, unsigned int num_bits)
{
  unsigned int idx;
  DCHECK_LE(((num_bits + 31) >> 5), p_bits->storage_size);
  for (idx = 0; idx < (num_bits >> 5); idx++) {
    p_bits->storage[idx] = -1;
  }
  unsigned int rem_num_bits = num_bits & 0x1f;
  if (rem_num_bits) {
    p_bits->storage[idx] = (1 << rem_num_bits) - 1;
  }
}

void GetBlockName(BasicBlock* bb, char* name)
{
  switch (bb->block_type) {
    case kEntryBlock:
      snprintf(name, BLOCK_NAME_LEN, "entry_%d", bb->id);
      break;
    case kExitBlock:
      snprintf(name, BLOCK_NAME_LEN, "exit_%d", bb->id);
      break;
    case kDalvikByteCode:
      snprintf(name, BLOCK_NAME_LEN, "block%04x_%d", bb->start_offset, bb->id);
      break;
    case kExceptionHandling:
      snprintf(name, BLOCK_NAME_LEN, "exception%04x_%d", bb->start_offset,
               bb->id);
      break;
    default:
      snprintf(name, BLOCK_NAME_LEN, "??_%d", bb->id);
      break;
  }
}

const char* GetShortyFromTargetIdx(CompilationUnit *cu, int target_idx)
{
  const DexFile::MethodId& method_id = cu->dex_file->GetMethodId(target_idx);
  return cu->dex_file->GetShorty(method_id.proto_idx_);
}

/* Allocate a new basic block */
BasicBlock* NewMemBB(CompilationUnit* cu, BBType block_type, int block_id)
{
  BasicBlock* bb = static_cast<BasicBlock*>(NewMem(cu, sizeof(BasicBlock), true, kAllocBB));
  bb->block_type = block_type;
  bb->id = block_id;
  bb->predecessors = static_cast<GrowableList*>
      (NewMem(cu, sizeof(GrowableList), false, kAllocPredecessors));
  CompilerInitGrowableList(cu, bb->predecessors,
                      (block_type == kExitBlock) ? 2048 : 2,
                      kListPredecessors);
  cu->block_id_map.Put(block_id, block_id);
  return bb;
}

/* Insert an MIR instruction to the end of a basic block */
void AppendMIR(BasicBlock* bb, MIR* mir)
{
  if (bb->first_mir_insn == NULL) {
    DCHECK(bb->last_mir_insn == NULL);
    bb->last_mir_insn = bb->first_mir_insn = mir;
    mir->prev = mir->next = NULL;
  } else {
    bb->last_mir_insn->next = mir;
    mir->prev = bb->last_mir_insn;
    mir->next = NULL;
    bb->last_mir_insn = mir;
  }
}

/* Insert an MIR instruction to the head of a basic block */
void PrependMIR(BasicBlock* bb, MIR* mir)
{
  if (bb->first_mir_insn == NULL) {
    DCHECK(bb->last_mir_insn == NULL);
    bb->last_mir_insn = bb->first_mir_insn = mir;
    mir->prev = mir->next = NULL;
  } else {
    bb->first_mir_insn->prev = mir;
    mir->next = bb->first_mir_insn;
    mir->prev = NULL;
    bb->first_mir_insn = mir;
  }
}

/* Insert a MIR instruction after the specified MIR */
void InsertMIRAfter(BasicBlock* bb, MIR* current_mir, MIR* new_mir)
{
  new_mir->prev = current_mir;
  new_mir->next = current_mir->next;
  current_mir->next = new_mir;

  if (new_mir->next) {
    /* Is not the last MIR in the block */
    new_mir->next->prev = new_mir;
  } else {
    /* Is the last MIR in the block */
    bb->last_mir_insn = new_mir;
  }
}

/*
 * Append an LIR instruction to the LIR list maintained by a compilation
 * unit
 */
void AppendLIR(CompilationUnit *cu, LIR* lir)
{
  if (cu->first_lir_insn == NULL) {
    DCHECK(cu->last_lir_insn == NULL);
     cu->last_lir_insn = cu->first_lir_insn = lir;
    lir->prev = lir->next = NULL;
  } else {
    cu->last_lir_insn->next = lir;
    lir->prev = cu->last_lir_insn;
    lir->next = NULL;
    cu->last_lir_insn = lir;
  }
}

/*
 * Insert an LIR instruction before the current instruction, which cannot be the
 * first instruction.
 *
 * prev_lir <-> new_lir <-> current_lir
 */
void InsertLIRBefore(LIR* current_lir, LIR* new_lir)
{
  DCHECK(current_lir->prev != NULL);
  LIR *prev_lir = current_lir->prev;

  prev_lir->next = new_lir;
  new_lir->prev = prev_lir;
  new_lir->next = current_lir;
  current_lir->prev = new_lir;
}

/*
 * Insert an LIR instruction after the current instruction, which cannot be the
 * first instruction.
 *
 * current_lir -> new_lir -> old_next
 */
void InsertLIRAfter(LIR* current_lir, LIR* new_lir)
{
  new_lir->prev = current_lir;
  new_lir->next = current_lir->next;
  current_lir->next = new_lir;
  new_lir->next->prev = new_lir;
}

}  // namespace art
