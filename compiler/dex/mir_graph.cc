/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include "mir_graph.h"

#include <inttypes.h>

#include "base/stl_util.h"
#include "compiler_internals.h"
#include "dex_file-inl.h"
#include "dex_instruction-inl.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "dex/quick/dex_file_method_inliner.h"
#include "leb128.h"

namespace art {

#define MAX_PATTERN_LEN 5

const char* MIRGraph::extended_mir_op_names_[kMirOpLast - kMirOpFirst] = {
  "Phi",
  "Copy",
  "FusedCmplFloat",
  "FusedCmpgFloat",
  "FusedCmplDouble",
  "FusedCmpgDouble",
  "FusedCmpLong",
  "Nop",
  "OpNullCheck",
  "OpRangeCheck",
  "OpDivZeroCheck",
  "Check1",
  "Check2",
  "Select",
  "ConstVector",
  "MoveVector",
  "PackedMultiply",
  "PackedAddition",
  "PackedSubtract",
  "PackedShiftLeft",
  "PackedSignedShiftRight",
  "PackedUnsignedShiftRight",
  "PackedAnd",
  "PackedOr",
  "PackedXor",
  "PackedAddReduce",
  "PackedReduce",
  "PackedSet",
};

MIRGraph::MIRGraph(CompilationUnit* cu, ArenaAllocator* arena)
    : reg_location_(NULL),
      cu_(cu),
      ssa_base_vregs_(NULL),
      ssa_subscripts_(NULL),
      vreg_to_ssa_map_(NULL),
      ssa_last_defs_(NULL),
      is_constant_v_(NULL),
      constant_values_(NULL),
      use_counts_(arena, 256, kGrowableArrayMisc),
      raw_use_counts_(arena, 256, kGrowableArrayMisc),
      num_reachable_blocks_(0),
      dfs_order_(NULL),
      dfs_post_order_(NULL),
      dom_post_order_traversal_(NULL),
      i_dom_list_(NULL),
      def_block_matrix_(NULL),
      temp_dalvik_register_v_(NULL),
      temp_scoped_alloc_(),
      temp_insn_data_(nullptr),
      temp_bit_vector_size_(0u),
      temp_bit_vector_(nullptr),
      block_list_(arena, 100, kGrowableArrayBlockList),
      try_block_addr_(NULL),
      entry_block_(NULL),
      exit_block_(NULL),
      num_blocks_(0),
      current_code_item_(NULL),
      dex_pc_to_block_map_(arena, 0, kGrowableArrayMisc),
      current_method_(kInvalidEntry),
      current_offset_(kInvalidEntry),
      def_count_(0),
      opcode_count_(NULL),
      num_ssa_regs_(0),
      method_sreg_(0),
      attributes_(METHOD_IS_LEAF),  // Start with leaf assumption, change on encountering invoke.
      checkstats_(NULL),
      arena_(arena),
      backward_branches_(0),
      forward_branches_(0),
      compiler_temps_(arena, 6, kGrowableArrayMisc),
      num_non_special_compiler_temps_(0),
      max_available_non_special_compiler_temps_(0),
      punt_to_interpreter_(false),
      merged_df_flags_(0u),
      ifield_lowering_infos_(arena, 0u),
      sfield_lowering_infos_(arena, 0u),
      method_lowering_infos_(arena, 0u) {
  try_block_addr_ = new (arena_) ArenaBitVector(arena_, 0, true /* expandable */);
  max_available_special_compiler_temps_ = std::abs(static_cast<int>(kVRegNonSpecialTempBaseReg))
      - std::abs(static_cast<int>(kVRegTempBaseReg));
}

MIRGraph::~MIRGraph() {
  STLDeleteElements(&m_units_);
}

/*
 * Parse an instruction, return the length of the instruction
 */
int MIRGraph::ParseInsn(const uint16_t* code_ptr, MIR::DecodedInstruction* decoded_instruction) {
  const Instruction* inst = Instruction::At(code_ptr);
  decoded_instruction->opcode = inst->Opcode();
  decoded_instruction->vA = inst->HasVRegA() ? inst->VRegA() : 0;
  decoded_instruction->vB = inst->HasVRegB() ? inst->VRegB() : 0;
  decoded_instruction->vB_wide = inst->HasWideVRegB() ? inst->WideVRegB() : 0;
  decoded_instruction->vC = inst->HasVRegC() ?  inst->VRegC() : 0;
  if (inst->HasVarArgs()) {
    inst->GetVarArgs(decoded_instruction->arg);
  }
  return inst->SizeInCodeUnits();
}


/* Split an existing block from the specified code offset into two */
BasicBlock* MIRGraph::SplitBlock(DexOffset code_offset,
                                 BasicBlock* orig_block, BasicBlock** immed_pred_block_p) {
  DCHECK_GT(code_offset, orig_block->start_offset);
  MIR* insn = orig_block->first_mir_insn;
  MIR* prev = NULL;
  while (insn) {
    if (insn->offset == code_offset) break;
    prev = insn;
    insn = insn->next;
  }
  if (insn == NULL) {
    LOG(FATAL) << "Break split failed";
  }
  BasicBlock *bottom_block = NewMemBB(kDalvikByteCode, num_blocks_++);
  block_list_.Insert(bottom_block);

  bottom_block->start_offset = code_offset;
  bottom_block->first_mir_insn = insn;
  bottom_block->last_mir_insn = orig_block->last_mir_insn;

  /* If this block was terminated by a return, the flag needs to go with the bottom block */
  bottom_block->terminated_by_return = orig_block->terminated_by_return;
  orig_block->terminated_by_return = false;

  /* Handle the taken path */
  bottom_block->taken = orig_block->taken;
  if (bottom_block->taken != NullBasicBlockId) {
    orig_block->taken = NullBasicBlockId;
    BasicBlock* bb_taken = GetBasicBlock(bottom_block->taken);
    bb_taken->predecessors->Delete(orig_block->id);
    bb_taken->predecessors->Insert(bottom_block->id);
  }

  /* Handle the fallthrough path */
  bottom_block->fall_through = orig_block->fall_through;
  orig_block->fall_through = bottom_block->id;
  bottom_block->predecessors->Insert(orig_block->id);
  if (bottom_block->fall_through != NullBasicBlockId) {
    BasicBlock* bb_fall_through = GetBasicBlock(bottom_block->fall_through);
    bb_fall_through->predecessors->Delete(orig_block->id);
    bb_fall_through->predecessors->Insert(bottom_block->id);
  }

  /* Handle the successor list */
  if (orig_block->successor_block_list_type != kNotUsed) {
    bottom_block->successor_block_list_type = orig_block->successor_block_list_type;
    bottom_block->successor_blocks = orig_block->successor_blocks;
    orig_block->successor_block_list_type = kNotUsed;
    orig_block->successor_blocks = NULL;
    GrowableArray<SuccessorBlockInfo*>::Iterator iterator(bottom_block->successor_blocks);
    while (true) {
      SuccessorBlockInfo *successor_block_info = iterator.Next();
      if (successor_block_info == NULL) break;
      BasicBlock *bb = GetBasicBlock(successor_block_info->block);
      bb->predecessors->Delete(orig_block->id);
      bb->predecessors->Insert(bottom_block->id);
    }
  }

  orig_block->last_mir_insn = prev;
  prev->next = NULL;

  /*
   * Update the immediate predecessor block pointer so that outgoing edges
   * can be applied to the proper block.
   */
  if (immed_pred_block_p) {
    DCHECK_EQ(*immed_pred_block_p, orig_block);
    *immed_pred_block_p = bottom_block;
  }

  // Associate dex instructions in the bottom block with the new container.
  DCHECK(insn != nullptr);
  DCHECK(insn != orig_block->first_mir_insn);
  DCHECK(insn == bottom_block->first_mir_insn);
  DCHECK_EQ(insn->offset, bottom_block->start_offset);
  DCHECK(static_cast<int>(insn->dalvikInsn.opcode) == kMirOpCheck ||
         !IsPseudoMirOp(insn->dalvikInsn.opcode));
  DCHECK_EQ(dex_pc_to_block_map_.Get(insn->offset), orig_block->id);
  MIR* p = insn;
  dex_pc_to_block_map_.Put(p->offset, bottom_block->id);
  while (p != bottom_block->last_mir_insn) {
    p = p->next;
    DCHECK(p != nullptr);
    int opcode = p->dalvikInsn.opcode;
    /*
     * Some messiness here to ensure that we only enter real opcodes and only the
     * first half of a potentially throwing instruction that has been split into
     * CHECK and work portions. Since the 2nd half of a split operation is always
     * the first in a BasicBlock, we can't hit it here.
     */
    if ((opcode == kMirOpCheck) || !IsPseudoMirOp(opcode)) {
      DCHECK_EQ(dex_pc_to_block_map_.Get(p->offset), orig_block->id);
      dex_pc_to_block_map_.Put(p->offset, bottom_block->id);
    }
  }

  return bottom_block;
}

/*
 * Given a code offset, find out the block that starts with it. If the offset
 * is in the middle of an existing block, split it into two.  If immed_pred_block_p
 * is not non-null and is the block being split, update *immed_pred_block_p to
 * point to the bottom block so that outgoing edges can be set up properly
 * (by the caller)
 * Utilizes a map for fast lookup of the typical cases.
 */
BasicBlock* MIRGraph::FindBlock(DexOffset code_offset, bool split, bool create,
                                BasicBlock** immed_pred_block_p) {
  if (code_offset >= cu_->code_item->insns_size_in_code_units_) {
    return NULL;
  }

  int block_id = dex_pc_to_block_map_.Get(code_offset);
  BasicBlock* bb = (block_id == 0) ? NULL : block_list_.Get(block_id);

  if ((bb != NULL) && (bb->start_offset == code_offset)) {
    // Does this containing block start with the desired instruction?
    return bb;
  }

  // No direct hit.
  if (!create) {
    return NULL;
  }

  if (bb != NULL) {
    // The target exists somewhere in an existing block.
    return SplitBlock(code_offset, bb, bb == *immed_pred_block_p ?  immed_pred_block_p : NULL);
  }

  // Create a new block.
  bb = NewMemBB(kDalvikByteCode, num_blocks_++);
  block_list_.Insert(bb);
  bb->start_offset = code_offset;
  dex_pc_to_block_map_.Put(bb->start_offset, bb->id);
  return bb;
}


/* Identify code range in try blocks and set up the empty catch blocks */
void MIRGraph::ProcessTryCatchBlocks() {
  int tries_size = current_code_item_->tries_size_;
  DexOffset offset;

  if (tries_size == 0) {
    return;
  }

  for (int i = 0; i < tries_size; i++) {
    const DexFile::TryItem* pTry =
        DexFile::GetTryItems(*current_code_item_, i);
    DexOffset start_offset = pTry->start_addr_;
    DexOffset end_offset = start_offset + pTry->insn_count_;
    for (offset = start_offset; offset < end_offset; offset++) {
      try_block_addr_->SetBit(offset);
    }
  }

  // Iterate over each of the handlers to enqueue the empty Catch blocks
  const byte* handlers_ptr = DexFile::GetCatchHandlerData(*current_code_item_, 0);
  uint32_t handlers_size = DecodeUnsignedLeb128(&handlers_ptr);
  for (uint32_t idx = 0; idx < handlers_size; idx++) {
    CatchHandlerIterator iterator(handlers_ptr);
    for (; iterator.HasNext(); iterator.Next()) {
      uint32_t address = iterator.GetHandlerAddress();
      FindBlock(address, false /* split */, true /*create*/,
                /* immed_pred_block_p */ NULL);
    }
    handlers_ptr = iterator.EndDataPointer();
  }
}

/* Process instructions with the kBranch flag */
BasicBlock* MIRGraph::ProcessCanBranch(BasicBlock* cur_block, MIR* insn, DexOffset cur_offset,
                                       int width, int flags, const uint16_t* code_ptr,
                                       const uint16_t* code_end) {
  DexOffset target = cur_offset;
  switch (insn->dalvikInsn.opcode) {
    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32:
      target += insn->dalvikInsn.vA;
      break;
    case Instruction::IF_EQ:
    case Instruction::IF_NE:
    case Instruction::IF_LT:
    case Instruction::IF_GE:
    case Instruction::IF_GT:
    case Instruction::IF_LE:
      cur_block->conditional_branch = true;
      target += insn->dalvikInsn.vC;
      break;
    case Instruction::IF_EQZ:
    case Instruction::IF_NEZ:
    case Instruction::IF_LTZ:
    case Instruction::IF_GEZ:
    case Instruction::IF_GTZ:
    case Instruction::IF_LEZ:
      cur_block->conditional_branch = true;
      target += insn->dalvikInsn.vB;
      break;
    default:
      LOG(FATAL) << "Unexpected opcode(" << insn->dalvikInsn.opcode << ") with kBranch set";
  }
  CountBranch(target);
  BasicBlock *taken_block = FindBlock(target, /* split */ true, /* create */ true,
                                      /* immed_pred_block_p */ &cur_block);
  cur_block->taken = taken_block->id;
  taken_block->predecessors->Insert(cur_block->id);

  /* Always terminate the current block for conditional branches */
  if (flags & Instruction::kContinue) {
    BasicBlock *fallthrough_block = FindBlock(cur_offset +  width,
                                             /*
                                              * If the method is processed
                                              * in sequential order from the
                                              * beginning, we don't need to
                                              * specify split for continue
                                              * blocks. However, this
                                              * routine can be called by
                                              * compileLoop, which starts
                                              * parsing the method from an
                                              * arbitrary address in the
                                              * method body.
                                              */
                                             true,
                                             /* create */
                                             true,
                                             /* immed_pred_block_p */
                                             &cur_block);
    cur_block->fall_through = fallthrough_block->id;
    fallthrough_block->predecessors->Insert(cur_block->id);
  } else if (code_ptr < code_end) {
    FindBlock(cur_offset + width, /* split */ false, /* create */ true,
                /* immed_pred_block_p */ NULL);
  }
  return cur_block;
}

/* Process instructions with the kSwitch flag */
BasicBlock* MIRGraph::ProcessCanSwitch(BasicBlock* cur_block, MIR* insn, DexOffset cur_offset,
                                       int width, int flags) {
  const uint16_t* switch_data =
      reinterpret_cast<const uint16_t*>(GetCurrentInsns() + cur_offset + insn->dalvikInsn.vB);
  int size;
  const int* keyTable;
  const int* target_table;
  int i;
  int first_key;

  /*
   * Packed switch data format:
   *  ushort ident = 0x0100   magic value
   *  ushort size             number of entries in the table
   *  int first_key           first (and lowest) switch case value
   *  int targets[size]       branch targets, relative to switch opcode
   *
   * Total size is (4+size*2) 16-bit code units.
   */
  if (insn->dalvikInsn.opcode == Instruction::PACKED_SWITCH) {
    DCHECK_EQ(static_cast<int>(switch_data[0]),
              static_cast<int>(Instruction::kPackedSwitchSignature));
    size = switch_data[1];
    first_key = switch_data[2] | (switch_data[3] << 16);
    target_table = reinterpret_cast<const int*>(&switch_data[4]);
    keyTable = NULL;        // Make the compiler happy
  /*
   * Sparse switch data format:
   *  ushort ident = 0x0200   magic value
   *  ushort size             number of entries in the table; > 0
   *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
   *  int targets[size]       branch targets, relative to switch opcode
   *
   * Total size is (2+size*4) 16-bit code units.
   */
  } else {
    DCHECK_EQ(static_cast<int>(switch_data[0]),
              static_cast<int>(Instruction::kSparseSwitchSignature));
    size = switch_data[1];
    keyTable = reinterpret_cast<const int*>(&switch_data[2]);
    target_table = reinterpret_cast<const int*>(&switch_data[2 + size*2]);
    first_key = 0;   // To make the compiler happy
  }

  if (cur_block->successor_block_list_type != kNotUsed) {
    LOG(FATAL) << "Successor block list already in use: "
               << static_cast<int>(cur_block->successor_block_list_type);
  }
  cur_block->successor_block_list_type =
      (insn->dalvikInsn.opcode == Instruction::PACKED_SWITCH) ?  kPackedSwitch : kSparseSwitch;
  cur_block->successor_blocks =
      new (arena_) GrowableArray<SuccessorBlockInfo*>(arena_, size, kGrowableArraySuccessorBlocks);

  for (i = 0; i < size; i++) {
    BasicBlock *case_block = FindBlock(cur_offset + target_table[i], /* split */ true,
                                      /* create */ true, /* immed_pred_block_p */ &cur_block);
    SuccessorBlockInfo *successor_block_info =
        static_cast<SuccessorBlockInfo*>(arena_->Alloc(sizeof(SuccessorBlockInfo),
                                                       kArenaAllocSuccessor));
    successor_block_info->block = case_block->id;
    successor_block_info->key =
        (insn->dalvikInsn.opcode == Instruction::PACKED_SWITCH) ?
        first_key + i : keyTable[i];
    cur_block->successor_blocks->Insert(successor_block_info);
    case_block->predecessors->Insert(cur_block->id);
  }

  /* Fall-through case */
  BasicBlock* fallthrough_block = FindBlock(cur_offset +  width, /* split */ false,
                                            /* create */ true, /* immed_pred_block_p */ NULL);
  cur_block->fall_through = fallthrough_block->id;
  fallthrough_block->predecessors->Insert(cur_block->id);
  return cur_block;
}

/* Process instructions with the kThrow flag */
BasicBlock* MIRGraph::ProcessCanThrow(BasicBlock* cur_block, MIR* insn, DexOffset cur_offset,
                                      int width, int flags, ArenaBitVector* try_block_addr,
                                      const uint16_t* code_ptr, const uint16_t* code_end) {
  bool in_try_block = try_block_addr->IsBitSet(cur_offset);
  bool is_throw = (insn->dalvikInsn.opcode == Instruction::THROW);
  bool build_all_edges =
      (cu_->disable_opt & (1 << kSuppressExceptionEdges)) || is_throw || in_try_block;

  /* In try block */
  if (in_try_block) {
    CatchHandlerIterator iterator(*current_code_item_, cur_offset);

    if (cur_block->successor_block_list_type != kNotUsed) {
      LOG(INFO) << PrettyMethod(cu_->method_idx, *cu_->dex_file);
      LOG(FATAL) << "Successor block list already in use: "
                 << static_cast<int>(cur_block->successor_block_list_type);
    }

    cur_block->successor_block_list_type = kCatch;
    cur_block->successor_blocks =
        new (arena_) GrowableArray<SuccessorBlockInfo*>(arena_, 2, kGrowableArraySuccessorBlocks);

    for (; iterator.HasNext(); iterator.Next()) {
      BasicBlock *catch_block = FindBlock(iterator.GetHandlerAddress(), false /* split*/,
                                         false /* creat */, NULL  /* immed_pred_block_p */);
      catch_block->catch_entry = true;
      if (kIsDebugBuild) {
        catches_.insert(catch_block->start_offset);
      }
      SuccessorBlockInfo *successor_block_info = reinterpret_cast<SuccessorBlockInfo*>
          (arena_->Alloc(sizeof(SuccessorBlockInfo), kArenaAllocSuccessor));
      successor_block_info->block = catch_block->id;
      successor_block_info->key = iterator.GetHandlerTypeIndex();
      cur_block->successor_blocks->Insert(successor_block_info);
      catch_block->predecessors->Insert(cur_block->id);
    }
  } else if (build_all_edges) {
    BasicBlock *eh_block = NewMemBB(kExceptionHandling, num_blocks_++);
    cur_block->taken = eh_block->id;
    block_list_.Insert(eh_block);
    eh_block->start_offset = cur_offset;
    eh_block->predecessors->Insert(cur_block->id);
  }

  if (is_throw) {
    cur_block->explicit_throw = true;
    if (code_ptr < code_end) {
      // Force creation of new block following THROW via side-effect
      FindBlock(cur_offset + width, /* split */ false, /* create */ true,
                /* immed_pred_block_p */ NULL);
    }
    if (!in_try_block) {
       // Don't split a THROW that can't rethrow - we're done.
      return cur_block;
    }
  }

  if (!build_all_edges) {
    /*
     * Even though there is an exception edge here, control cannot return to this
     * method.  Thus, for the purposes of dataflow analysis and optimization, we can
     * ignore the edge.  Doing this reduces compile time, and increases the scope
     * of the basic-block level optimization pass.
     */
    return cur_block;
  }

  /*
   * Split the potentially-throwing instruction into two parts.
   * The first half will be a pseudo-op that captures the exception
   * edges and terminates the basic block.  It always falls through.
   * Then, create a new basic block that begins with the throwing instruction
   * (minus exceptions).  Note: this new basic block must NOT be entered into
   * the block_map.  If the potentially-throwing instruction is the target of a
   * future branch, we need to find the check psuedo half.  The new
   * basic block containing the work portion of the instruction should
   * only be entered via fallthrough from the block containing the
   * pseudo exception edge MIR.  Note also that this new block is
   * not automatically terminated after the work portion, and may
   * contain following instructions.
   *
   * Note also that the dex_pc_to_block_map_ entry for the potentially
   * throwing instruction will refer to the original basic block.
   */
  BasicBlock *new_block = NewMemBB(kDalvikByteCode, num_blocks_++);
  block_list_.Insert(new_block);
  new_block->start_offset = insn->offset;
  cur_block->fall_through = new_block->id;
  new_block->predecessors->Insert(cur_block->id);
  MIR* new_insn = static_cast<MIR*>(arena_->Alloc(sizeof(MIR), kArenaAllocMIR));
  *new_insn = *insn;
  insn->dalvikInsn.opcode =
      static_cast<Instruction::Code>(kMirOpCheck);
  // Associate the two halves
  insn->meta.throw_insn = new_insn;
  new_block->AppendMIR(new_insn);
  return new_block;
}

/* Parse a Dex method and insert it into the MIRGraph at the current insert point. */
void MIRGraph::InlineMethod(const DexFile::CodeItem* code_item, uint32_t access_flags,
                           InvokeType invoke_type, uint16_t class_def_idx,
                           uint32_t method_idx, jobject class_loader, const DexFile& dex_file) {
  current_code_item_ = code_item;
  method_stack_.push_back(std::make_pair(current_method_, current_offset_));
  current_method_ = m_units_.size();
  current_offset_ = 0;
  // TODO: will need to snapshot stack image and use that as the mir context identification.
  m_units_.push_back(new DexCompilationUnit(cu_, class_loader, Runtime::Current()->GetClassLinker(),
                     dex_file, current_code_item_, class_def_idx, method_idx, access_flags,
                     cu_->compiler_driver->GetVerifiedMethod(&dex_file, method_idx)));
  const uint16_t* code_ptr = current_code_item_->insns_;
  const uint16_t* code_end =
      current_code_item_->insns_ + current_code_item_->insns_size_in_code_units_;

  // TODO: need to rework expansion of block list & try_block_addr when inlining activated.
  // TUNING: use better estimate of basic blocks for following resize.
  block_list_.Resize(block_list_.Size() + current_code_item_->insns_size_in_code_units_);
  dex_pc_to_block_map_.SetSize(dex_pc_to_block_map_.Size() + current_code_item_->insns_size_in_code_units_);

  // TODO: replace with explicit resize routine.  Using automatic extension side effect for now.
  try_block_addr_->SetBit(current_code_item_->insns_size_in_code_units_);
  try_block_addr_->ClearBit(current_code_item_->insns_size_in_code_units_);

  // If this is the first method, set up default entry and exit blocks.
  if (current_method_ == 0) {
    DCHECK(entry_block_ == NULL);
    DCHECK(exit_block_ == NULL);
    DCHECK_EQ(num_blocks_, 0);
    // Use id 0 to represent a null block.
    BasicBlock* null_block = NewMemBB(kNullBlock, num_blocks_++);
    DCHECK_EQ(null_block->id, NullBasicBlockId);
    null_block->hidden = true;
    block_list_.Insert(null_block);
    entry_block_ = NewMemBB(kEntryBlock, num_blocks_++);
    block_list_.Insert(entry_block_);
    exit_block_ = NewMemBB(kExitBlock, num_blocks_++);
    block_list_.Insert(exit_block_);
    // TODO: deprecate all "cu->" fields; move what's left to wherever CompilationUnit is allocated.
    cu_->dex_file = &dex_file;
    cu_->class_def_idx = class_def_idx;
    cu_->method_idx = method_idx;
    cu_->access_flags = access_flags;
    cu_->invoke_type = invoke_type;
    cu_->shorty = dex_file.GetMethodShorty(dex_file.GetMethodId(method_idx));
    cu_->num_ins = current_code_item_->ins_size_;
    cu_->num_regs = current_code_item_->registers_size_ - cu_->num_ins;
    cu_->num_outs = current_code_item_->outs_size_;
    cu_->num_dalvik_registers = current_code_item_->registers_size_;
    cu_->insns = current_code_item_->insns_;
    cu_->code_item = current_code_item_;
  } else {
    UNIMPLEMENTED(FATAL) << "Nested inlining not implemented.";
    /*
     * Will need to manage storage for ins & outs, push prevous state and update
     * insert point.
     */
  }

  /* Current block to record parsed instructions */
  BasicBlock *cur_block = NewMemBB(kDalvikByteCode, num_blocks_++);
  DCHECK_EQ(current_offset_, 0U);
  cur_block->start_offset = current_offset_;
  block_list_.Insert(cur_block);
  // TODO: for inlining support, insert at the insert point rather than entry block.
  entry_block_->fall_through = cur_block->id;
  cur_block->predecessors->Insert(entry_block_->id);

  /* Identify code range in try blocks and set up the empty catch blocks */
  ProcessTryCatchBlocks();

  uint64_t merged_df_flags = 0u;

  /* Parse all instructions and put them into containing basic blocks */
  while (code_ptr < code_end) {
    MIR *insn = static_cast<MIR *>(arena_->Alloc(sizeof(MIR), kArenaAllocMIR));
    insn->offset = current_offset_;
    insn->m_unit_index = current_method_;
    int width = ParseInsn(code_ptr, &insn->dalvikInsn);
    insn->width = width;
    Instruction::Code opcode = insn->dalvikInsn.opcode;
    if (opcode_count_ != NULL) {
      opcode_count_[static_cast<int>(opcode)]++;
    }

    int flags = Instruction::FlagsOf(insn->dalvikInsn.opcode);
    int verify_flags = Instruction::VerifyFlagsOf(insn->dalvikInsn.opcode);

    uint64_t df_flags = GetDataFlowAttributes(insn);
    merged_df_flags |= df_flags;

    if (df_flags & DF_HAS_DEFS) {
      def_count_ += (df_flags & DF_A_WIDE) ? 2 : 1;
    }

    if (df_flags & DF_LVN) {
      cur_block->use_lvn = true;  // Run local value numbering on this basic block.
    }

    // Check for inline data block signatures
    if (opcode == Instruction::NOP) {
      // A simple NOP will have a width of 1 at this point, embedded data NOP > 1.
      if ((width == 1) && ((current_offset_ & 0x1) == 0x1) && ((code_end - code_ptr) > 1)) {
        // Could be an aligning nop.  If an embedded data NOP follows, treat pair as single unit.
        uint16_t following_raw_instruction = code_ptr[1];
        if ((following_raw_instruction == Instruction::kSparseSwitchSignature) ||
            (following_raw_instruction == Instruction::kPackedSwitchSignature) ||
            (following_raw_instruction == Instruction::kArrayDataSignature)) {
          width += Instruction::At(code_ptr + 1)->SizeInCodeUnits();
        }
      }
      if (width == 1) {
        // It is a simple nop - treat normally.
        cur_block->AppendMIR(insn);
      } else {
        DCHECK(cur_block->fall_through == NullBasicBlockId);
        DCHECK(cur_block->taken == NullBasicBlockId);
        // Unreachable instruction, mark for no continuation.
        flags &= ~Instruction::kContinue;
      }
    } else {
      cur_block->AppendMIR(insn);
    }

    // Associate the starting dex_pc for this opcode with its containing basic block.
    dex_pc_to_block_map_.Put(insn->offset, cur_block->id);

    code_ptr += width;

    if (flags & Instruction::kBranch) {
      cur_block = ProcessCanBranch(cur_block, insn, current_offset_,
                                   width, flags, code_ptr, code_end);
    } else if (flags & Instruction::kReturn) {
      cur_block->terminated_by_return = true;
      cur_block->fall_through = exit_block_->id;
      exit_block_->predecessors->Insert(cur_block->id);
      /*
       * Terminate the current block if there are instructions
       * afterwards.
       */
      if (code_ptr < code_end) {
        /*
         * Create a fallthrough block for real instructions
         * (incl. NOP).
         */
         FindBlock(current_offset_ + width, /* split */ false, /* create */ true,
                   /* immed_pred_block_p */ NULL);
      }
    } else if (flags & Instruction::kThrow) {
      cur_block = ProcessCanThrow(cur_block, insn, current_offset_, width, flags, try_block_addr_,
                                  code_ptr, code_end);
    } else if (flags & Instruction::kSwitch) {
      cur_block = ProcessCanSwitch(cur_block, insn, current_offset_, width, flags);
    }
    if (verify_flags & Instruction::kVerifyVarArgRange) {
      /*
       * The Quick backend's runtime model includes a gap between a method's
       * argument ("in") vregs and the rest of its vregs.  Handling a range instruction
       * which spans the gap is somewhat complicated, and should not happen
       * in normal usage of dx.  Punt to the interpreter.
       */
      int first_reg_in_range = insn->dalvikInsn.vC;
      int last_reg_in_range = first_reg_in_range + insn->dalvikInsn.vA - 1;
      if (IsInVReg(first_reg_in_range) != IsInVReg(last_reg_in_range)) {
        punt_to_interpreter_ = true;
      }
    }
    current_offset_ += width;
    BasicBlock *next_block = FindBlock(current_offset_, /* split */ false, /* create */
                                      false, /* immed_pred_block_p */ NULL);
    if (next_block) {
      /*
       * The next instruction could be the target of a previously parsed
       * forward branch so a block is already created. If the current
       * instruction is not an unconditional branch, connect them through
       * the fall-through link.
       */
      DCHECK(cur_block->fall_through == NullBasicBlockId ||
             GetBasicBlock(cur_block->fall_through) == next_block ||
             GetBasicBlock(cur_block->fall_through) == exit_block_);

      if ((cur_block->fall_through == NullBasicBlockId) && (flags & Instruction::kContinue)) {
        cur_block->fall_through = next_block->id;
        next_block->predecessors->Insert(cur_block->id);
      }
      cur_block = next_block;
    }
  }
  merged_df_flags_ = merged_df_flags;

  if (cu_->enable_debug & (1 << kDebugDumpCFG)) {
    DumpCFG("/sdcard/1_post_parse_cfg/", true);
  }

  if (cu_->verbose) {
    DumpMIRGraph();
  }
}

void MIRGraph::ShowOpcodeStats() {
  DCHECK(opcode_count_ != NULL);
  LOG(INFO) << "Opcode Count";
  for (int i = 0; i < kNumPackedOpcodes; i++) {
    if (opcode_count_[i] != 0) {
      LOG(INFO) << "-C- " << Instruction::Name(static_cast<Instruction::Code>(i))
                << " " << opcode_count_[i];
    }
  }
}

uint64_t MIRGraph::GetDataFlowAttributes(Instruction::Code opcode) {
  DCHECK_LT((size_t) opcode, (sizeof(oat_data_flow_attributes_) / sizeof(oat_data_flow_attributes_[0])));
  return oat_data_flow_attributes_[opcode];
}

uint64_t MIRGraph::GetDataFlowAttributes(MIR* mir) {
  DCHECK(mir != nullptr);
  Instruction::Code opcode = mir->dalvikInsn.opcode;
  return GetDataFlowAttributes(opcode);
}

// TODO: use a configurable base prefix, and adjust callers to supply pass name.
/* Dump the CFG into a DOT graph */
void MIRGraph::DumpCFG(const char* dir_prefix, bool all_blocks, const char *suffix) {
  FILE* file;
  std::string fname(PrettyMethod(cu_->method_idx, *cu_->dex_file));
  ReplaceSpecialChars(fname);
  fname = StringPrintf("%s%s%x%s.dot", dir_prefix, fname.c_str(),
                      GetBasicBlock(GetEntryBlock()->fall_through)->start_offset,
                      suffix == nullptr ? "" : suffix);
  file = fopen(fname.c_str(), "w");
  if (file == NULL) {
    return;
  }
  fprintf(file, "digraph G {\n");

  fprintf(file, "  rankdir=TB\n");

  int num_blocks = all_blocks ? GetNumBlocks() : num_reachable_blocks_;
  int idx;

  for (idx = 0; idx < num_blocks; idx++) {
    int block_idx = all_blocks ? idx : dfs_order_->Get(idx);
    BasicBlock *bb = GetBasicBlock(block_idx);
    if (bb == NULL) continue;
    if (bb->block_type == kDead) continue;
    if (bb->block_type == kEntryBlock) {
      fprintf(file, "  entry_%d [shape=Mdiamond];\n", bb->id);
    } else if (bb->block_type == kExitBlock) {
      fprintf(file, "  exit_%d [shape=Mdiamond];\n", bb->id);
    } else if (bb->block_type == kDalvikByteCode) {
      fprintf(file, "  block%04x_%d [shape=record,label = \"{ \\\n",
              bb->start_offset, bb->id);
      const MIR *mir;
        fprintf(file, "    {block id %d\\l}%s\\\n", bb->id,
                bb->first_mir_insn ? " | " : " ");
        for (mir = bb->first_mir_insn; mir; mir = mir->next) {
            int opcode = mir->dalvikInsn.opcode;
            if (opcode > kMirOpSelect && opcode < kMirOpLast) {
              if (opcode == kMirOpConstVector) {
                fprintf(file, "    {%04x %s %d %d %d %d %d %d\\l}%s\\\n", mir->offset,
                        extended_mir_op_names_[kMirOpConstVector - kMirOpFirst],
                        mir->dalvikInsn.vA,
                        mir->dalvikInsn.vB,
                        mir->dalvikInsn.arg[0],
                        mir->dalvikInsn.arg[1],
                        mir->dalvikInsn.arg[2],
                        mir->dalvikInsn.arg[3],
                        mir->next ? " | " : " ");
              } else {
                fprintf(file, "    {%04x %s %d %d %d\\l}%s\\\n", mir->offset,
                        extended_mir_op_names_[opcode - kMirOpFirst],
                        mir->dalvikInsn.vA,
                        mir->dalvikInsn.vB,
                        mir->dalvikInsn.vC,
                        mir->next ? " | " : " ");
              }
            } else {
              fprintf(file, "    {%04x %s %s %s\\l}%s\\\n", mir->offset,
                      mir->ssa_rep ? GetDalvikDisassembly(mir) :
                      (opcode < kMirOpFirst) ?
                        Instruction::Name(mir->dalvikInsn.opcode) :
                        extended_mir_op_names_[opcode - kMirOpFirst],
                      (mir->optimization_flags & MIR_IGNORE_RANGE_CHECK) != 0 ? " no_rangecheck" : " ",
                      (mir->optimization_flags & MIR_IGNORE_NULL_CHECK) != 0 ? " no_nullcheck" : " ",
                      mir->next ? " | " : " ");
            }
        }
        fprintf(file, "  }\"];\n\n");
    } else if (bb->block_type == kExceptionHandling) {
      char block_name[BLOCK_NAME_LEN];

      GetBlockName(bb, block_name);
      fprintf(file, "  %s [shape=invhouse];\n", block_name);
    }

    char block_name1[BLOCK_NAME_LEN], block_name2[BLOCK_NAME_LEN];

    if (bb->taken != NullBasicBlockId) {
      GetBlockName(bb, block_name1);
      GetBlockName(GetBasicBlock(bb->taken), block_name2);
      fprintf(file, "  %s:s -> %s:n [style=dotted]\n",
              block_name1, block_name2);
    }
    if (bb->fall_through != NullBasicBlockId) {
      GetBlockName(bb, block_name1);
      GetBlockName(GetBasicBlock(bb->fall_through), block_name2);
      fprintf(file, "  %s:s -> %s:n\n", block_name1, block_name2);
    }

    if (bb->successor_block_list_type != kNotUsed) {
      fprintf(file, "  succ%04x_%d [shape=%s,label = \"{ \\\n",
              bb->start_offset, bb->id,
              (bb->successor_block_list_type == kCatch) ?  "Mrecord" : "record");
      GrowableArray<SuccessorBlockInfo*>::Iterator iterator(bb->successor_blocks);
      SuccessorBlockInfo *successor_block_info = iterator.Next();

      int succ_id = 0;
      while (true) {
        if (successor_block_info == NULL) break;

        BasicBlock *dest_block = GetBasicBlock(successor_block_info->block);
        SuccessorBlockInfo *next_successor_block_info = iterator.Next();

        fprintf(file, "    {<f%d> %04x: %04x\\l}%s\\\n",
                succ_id++,
                successor_block_info->key,
                dest_block->start_offset,
                (next_successor_block_info != NULL) ? " | " : " ");

        successor_block_info = next_successor_block_info;
      }
      fprintf(file, "  }\"];\n\n");

      GetBlockName(bb, block_name1);
      fprintf(file, "  %s:s -> succ%04x_%d:n [style=dashed]\n",
              block_name1, bb->start_offset, bb->id);

      // Link the successor pseudo-block with all of its potential targets.
      GrowableArray<SuccessorBlockInfo*>::Iterator iter(bb->successor_blocks);

      succ_id = 0;
      while (true) {
        SuccessorBlockInfo *successor_block_info = iter.Next();
        if (successor_block_info == NULL) break;

        BasicBlock* dest_block = GetBasicBlock(successor_block_info->block);

        GetBlockName(dest_block, block_name2);
        fprintf(file, "  succ%04x_%d:f%d:e -> %s:n\n", bb->start_offset,
                bb->id, succ_id++, block_name2);
      }
    }
    fprintf(file, "\n");

    if (cu_->verbose) {
      /* Display the dominator tree */
      GetBlockName(bb, block_name1);
      fprintf(file, "  cfg%s [label=\"%s\", shape=none];\n",
              block_name1, block_name1);
      if (bb->i_dom) {
        GetBlockName(GetBasicBlock(bb->i_dom), block_name2);
        fprintf(file, "  cfg%s:s -> cfg%s:n\n\n", block_name2, block_name1);
      }
    }
  }
  fprintf(file, "}\n");
  fclose(file);
}

/* Insert an MIR instruction to the end of a basic block */
void BasicBlock::AppendMIR(MIR* mir) {
  if (first_mir_insn == nullptr) {
    DCHECK(last_mir_insn == nullptr);
    last_mir_insn = first_mir_insn = mir;
    mir->next = nullptr;
  } else {
    last_mir_insn->next = mir;
    mir->next = nullptr;
    last_mir_insn = mir;
  }
}

/* Insert an MIR instruction to the head of a basic block */
void BasicBlock::PrependMIR(MIR* mir) {
  if (first_mir_insn == nullptr) {
    DCHECK(last_mir_insn == nullptr);
    last_mir_insn = first_mir_insn = mir;
    mir->next = nullptr;
  } else {
    mir->next = first_mir_insn;
    first_mir_insn = mir;
  }
}

/* Insert a MIR instruction after the specified MIR */
void BasicBlock::InsertMIRAfter(MIR* current_mir, MIR* new_mir) {
  new_mir->next = current_mir->next;
  current_mir->next = new_mir;

  if (last_mir_insn == current_mir) {
    /* Is the last MIR in the block */
    last_mir_insn = new_mir;
  }
}

MIR* BasicBlock::GetNextUnconditionalMir(MIRGraph* mir_graph, MIR* current) {
  MIR* next_mir = nullptr;

  if (current != nullptr) {
    next_mir = current->next;
  }

  if (next_mir == nullptr) {
    // Only look for next MIR that follows unconditionally.
    if ((taken == NullBasicBlockId) && (fall_through != NullBasicBlockId)) {
      next_mir = mir_graph->GetBasicBlock(fall_through)->first_mir_insn;
    }
  }

  return next_mir;
}

char* MIRGraph::GetDalvikDisassembly(const MIR* mir) {
  MIR::DecodedInstruction insn = mir->dalvikInsn;
  std::string str;
  int flags = 0;
  int opcode = insn.opcode;
  char* ret;
  bool nop = false;
  SSARepresentation* ssa_rep = mir->ssa_rep;
  Instruction::Format dalvik_format = Instruction::k10x;  // Default to no-operand format
  int defs = (ssa_rep != NULL) ? ssa_rep->num_defs : 0;
  int uses = (ssa_rep != NULL) ? ssa_rep->num_uses : 0;

  // Handle special cases.
  if ((opcode == kMirOpCheck) || (opcode == kMirOpCheckPart2)) {
    str.append(extended_mir_op_names_[opcode - kMirOpFirst]);
    str.append(": ");
    // Recover the original Dex instruction
    insn = mir->meta.throw_insn->dalvikInsn;
    ssa_rep = mir->meta.throw_insn->ssa_rep;
    defs = ssa_rep->num_defs;
    uses = ssa_rep->num_uses;
    opcode = insn.opcode;
  } else if (opcode == kMirOpNop) {
    str.append("[");
    // Recover original opcode.
    insn.opcode = Instruction::At(current_code_item_->insns_ + mir->offset)->Opcode();
    opcode = insn.opcode;
    nop = true;
  }

  if (opcode >= kMirOpFirst) {
    str.append(extended_mir_op_names_[opcode - kMirOpFirst]);
  } else {
    dalvik_format = Instruction::FormatOf(insn.opcode);
    flags = Instruction::FlagsOf(insn.opcode);
    str.append(Instruction::Name(insn.opcode));
  }

  if (opcode == kMirOpPhi) {
    BasicBlockId* incoming = mir->meta.phi_incoming;
    str.append(StringPrintf(" %s = (%s",
               GetSSANameWithConst(ssa_rep->defs[0], true).c_str(),
               GetSSANameWithConst(ssa_rep->uses[0], true).c_str()));
    str.append(StringPrintf(":%d", incoming[0]));
    int i;
    for (i = 1; i < uses; i++) {
      str.append(StringPrintf(", %s:%d",
                              GetSSANameWithConst(ssa_rep->uses[i], true).c_str(),
                              incoming[i]));
    }
    str.append(")");
  } else if ((flags & Instruction::kBranch) != 0) {
    // For branches, decode the instructions to print out the branch targets.
    int offset = 0;
    switch (dalvik_format) {
      case Instruction::k21t:
        str.append(StringPrintf(" %s,", GetSSANameWithConst(ssa_rep->uses[0], false).c_str()));
        offset = insn.vB;
        break;
      case Instruction::k22t:
        str.append(StringPrintf(" %s, %s,", GetSSANameWithConst(ssa_rep->uses[0], false).c_str(),
                   GetSSANameWithConst(ssa_rep->uses[1], false).c_str()));
        offset = insn.vC;
        break;
      case Instruction::k10t:
      case Instruction::k20t:
      case Instruction::k30t:
        offset = insn.vA;
        break;
      default:
        LOG(FATAL) << "Unexpected branch format " << dalvik_format << " from " << insn.opcode;
    }
    str.append(StringPrintf(" 0x%x (%c%x)", mir->offset + offset,
                            offset > 0 ? '+' : '-', offset > 0 ? offset : -offset));
  } else {
    // For invokes-style formats, treat wide regs as a pair of singles
    bool show_singles = ((dalvik_format == Instruction::k35c) ||
                         (dalvik_format == Instruction::k3rc));
    if (defs != 0) {
      str.append(StringPrintf(" %s", GetSSANameWithConst(ssa_rep->defs[0], false).c_str()));
      if (uses != 0) {
        str.append(", ");
      }
    }
    for (int i = 0; i < uses; i++) {
      str.append(
          StringPrintf(" %s", GetSSANameWithConst(ssa_rep->uses[i], show_singles).c_str()));
      if (!show_singles && (reg_location_ != NULL) && reg_location_[i].wide) {
        // For the listing, skip the high sreg.
        i++;
      }
      if (i != (uses -1)) {
        str.append(",");
      }
    }
    switch (dalvik_format) {
      case Instruction::k11n:  // Add one immediate from vB
      case Instruction::k21s:
      case Instruction::k31i:
      case Instruction::k21h:
        str.append(StringPrintf(", #%d", insn.vB));
        break;
      case Instruction::k51l:  // Add one wide immediate
        str.append(StringPrintf(", #%" PRId64, insn.vB_wide));
        break;
      case Instruction::k21c:  // One register, one string/type/method index
      case Instruction::k31c:
        str.append(StringPrintf(", index #%d", insn.vB));
        break;
      case Instruction::k22c:  // Two registers, one string/type/method index
        str.append(StringPrintf(", index #%d", insn.vC));
        break;
      case Instruction::k22s:  // Add one immediate from vC
      case Instruction::k22b:
        str.append(StringPrintf(", #%d", insn.vC));
        break;
      default: {
        // Nothing left to print
      }
    }
  }
  if (nop) {
    str.append("]--optimized away");
  }
  int length = str.length() + 1;
  ret = static_cast<char*>(arena_->Alloc(length, kArenaAllocDFInfo));
  strncpy(ret, str.c_str(), length);
  return ret;
}

/* Turn method name into a legal Linux file name */
void MIRGraph::ReplaceSpecialChars(std::string& str) {
  static const struct { const char before; const char after; } match[] = {
    {'/', '-'}, {';', '#'}, {' ', '#'}, {'$', '+'},
    {'(', '@'}, {')', '@'}, {'<', '='}, {'>', '='}
  };
  for (unsigned int i = 0; i < sizeof(match)/sizeof(match[0]); i++) {
    std::replace(str.begin(), str.end(), match[i].before, match[i].after);
  }
}

std::string MIRGraph::GetSSAName(int ssa_reg) {
  // TODO: This value is needed for LLVM and debugging. Currently, we compute this and then copy to
  //       the arena. We should be smarter and just place straight into the arena, or compute the
  //       value more lazily.
  return StringPrintf("v%d_%d", SRegToVReg(ssa_reg), GetSSASubscript(ssa_reg));
}

// Similar to GetSSAName, but if ssa name represents an immediate show that as well.
std::string MIRGraph::GetSSANameWithConst(int ssa_reg, bool singles_only) {
  if (reg_location_ == NULL) {
    // Pre-SSA - just use the standard name
    return GetSSAName(ssa_reg);
  }
  if (IsConst(reg_location_[ssa_reg])) {
    if (!singles_only && reg_location_[ssa_reg].wide) {
      return StringPrintf("v%d_%d#0x%" PRIx64, SRegToVReg(ssa_reg), GetSSASubscript(ssa_reg),
                          ConstantValueWide(reg_location_[ssa_reg]));
    } else {
      return StringPrintf("v%d_%d#0x%x", SRegToVReg(ssa_reg), GetSSASubscript(ssa_reg),
                          ConstantValue(reg_location_[ssa_reg]));
    }
  } else {
    return StringPrintf("v%d_%d", SRegToVReg(ssa_reg), GetSSASubscript(ssa_reg));
  }
}

void MIRGraph::GetBlockName(BasicBlock* bb, char* name) {
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
      snprintf(name, BLOCK_NAME_LEN, "_%d", bb->id);
      break;
  }
}

const char* MIRGraph::GetShortyFromTargetIdx(int target_idx) {
  // TODO: for inlining support, use current code unit.
  const DexFile::MethodId& method_id = cu_->dex_file->GetMethodId(target_idx);
  return cu_->dex_file->GetShorty(method_id.proto_idx_);
}

/* Debug Utility - dump a compilation unit */
void MIRGraph::DumpMIRGraph() {
  BasicBlock* bb;
  const char* block_type_names[] = {
    "Null Block",
    "Entry Block",
    "Code Block",
    "Exit Block",
    "Exception Handling",
    "Catch Block"
  };

  LOG(INFO) << "Compiling " << PrettyMethod(cu_->method_idx, *cu_->dex_file);
  LOG(INFO) << cu_->insns << " insns";
  LOG(INFO) << GetNumBlocks() << " blocks in total";
  GrowableArray<BasicBlock*>::Iterator iterator(&block_list_);

  while (true) {
    bb = iterator.Next();
    if (bb == NULL) break;
    LOG(INFO) << StringPrintf("Block %d (%s) (insn %04x - %04x%s)",
        bb->id,
        block_type_names[bb->block_type],
        bb->start_offset,
        bb->last_mir_insn ? bb->last_mir_insn->offset : bb->start_offset,
        bb->last_mir_insn ? "" : " empty");
    if (bb->taken != NullBasicBlockId) {
      LOG(INFO) << "  Taken branch: block " << bb->taken
                << "(0x" << std::hex << GetBasicBlock(bb->taken)->start_offset << ")";
    }
    if (bb->fall_through != NullBasicBlockId) {
      LOG(INFO) << "  Fallthrough : block " << bb->fall_through
                << " (0x" << std::hex << GetBasicBlock(bb->fall_through)->start_offset << ")";
    }
  }
}

/*
 * Build an array of location records for the incoming arguments.
 * Note: one location record per word of arguments, with dummy
 * high-word loc for wide arguments.  Also pull up any following
 * MOVE_RESULT and incorporate it into the invoke.
 */
CallInfo* MIRGraph::NewMemCallInfo(BasicBlock* bb, MIR* mir, InvokeType type,
                                  bool is_range) {
  CallInfo* info = static_cast<CallInfo*>(arena_->Alloc(sizeof(CallInfo),
                                                        kArenaAllocMisc));
  MIR* move_result_mir = FindMoveResult(bb, mir);
  if (move_result_mir == NULL) {
    info->result.location = kLocInvalid;
  } else {
    info->result = GetRawDest(move_result_mir);
    move_result_mir->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
  }
  info->num_arg_words = mir->ssa_rep->num_uses;
  info->args = (info->num_arg_words == 0) ? NULL : static_cast<RegLocation*>
      (arena_->Alloc(sizeof(RegLocation) * info->num_arg_words, kArenaAllocMisc));
  for (int i = 0; i < info->num_arg_words; i++) {
    info->args[i] = GetRawSrc(mir, i);
  }
  info->opt_flags = mir->optimization_flags;
  info->type = type;
  info->is_range = is_range;
  info->index = mir->dalvikInsn.vB;
  info->offset = mir->offset;
  info->mir = mir;
  return info;
}

// Allocate a new basic block.
BasicBlock* MIRGraph::NewMemBB(BBType block_type, int block_id) {
  BasicBlock* bb = static_cast<BasicBlock*>(arena_->Alloc(sizeof(BasicBlock),
                                                          kArenaAllocBB));
  bb->block_type = block_type;
  bb->id = block_id;
  // TUNING: better estimate of the exit block predecessors?
  bb->predecessors = new (arena_) GrowableArray<BasicBlockId>(arena_,
                                                             (block_type == kExitBlock) ? 2048 : 2,
                                                             kGrowableArrayPredecessors);
  bb->successor_block_list_type = kNotUsed;
  block_id_map_.Put(block_id, block_id);
  return bb;
}

void MIRGraph::InitializeConstantPropagation() {
  is_constant_v_ = new (arena_) ArenaBitVector(arena_, GetNumSSARegs(), false);
  constant_values_ = static_cast<int*>(arena_->Alloc(sizeof(int) * GetNumSSARegs(), kArenaAllocDFInfo));
}

void MIRGraph::InitializeMethodUses() {
  // The gate starts by initializing the use counts
  int num_ssa_regs = GetNumSSARegs();
  use_counts_.Resize(num_ssa_regs + 32);
  raw_use_counts_.Resize(num_ssa_regs + 32);
  // Initialize list
  for (int i = 0; i < num_ssa_regs; i++) {
    use_counts_.Insert(0);
    raw_use_counts_.Insert(0);
  }
}

void MIRGraph::InitializeSSATransformation() {
  /* Compute the DFS order */
  ComputeDFSOrders();

  /* Compute the dominator info */
  ComputeDominators();

  /* Allocate data structures in preparation for SSA conversion */
  CompilerInitializeSSAConversion();

  /* Find out the "Dalvik reg def x block" relation */
  ComputeDefBlockMatrix();

  /* Insert phi nodes to dominance frontiers for all variables */
  InsertPhiNodes();

  /* Rename register names by local defs and phi nodes */
  ClearAllVisitedFlags();
  DoDFSPreOrderSSARename(GetEntryBlock());
}

ChildBlockIterator::ChildBlockIterator(BasicBlock* bb, MIRGraph* mir_graph)
    : basic_block_(bb), mir_graph_(mir_graph), visited_fallthrough_(false),
      visited_taken_(false), have_successors_(false) {
  // Check if we actually do have successors.
  if (basic_block_ != 0 && basic_block_->successor_block_list_type != kNotUsed) {
    have_successors_ = true;
    successor_iter_.Reset(basic_block_->successor_blocks);
  }
}

BasicBlock* ChildBlockIterator::Next() {
  // We check if we have a basic block. If we don't we cannot get next child.
  if (basic_block_ == nullptr) {
    return nullptr;
  }

  // If we haven't visited fallthrough, return that.
  if (visited_fallthrough_ == false) {
    visited_fallthrough_ = true;

    BasicBlock* result = mir_graph_->GetBasicBlock(basic_block_->fall_through);
    if (result != nullptr) {
      return result;
    }
  }

  // If we haven't visited taken, return that.
  if (visited_taken_ == false) {
    visited_taken_ = true;

    BasicBlock* result = mir_graph_->GetBasicBlock(basic_block_->taken);
    if (result != nullptr) {
      return result;
    }
  }

  // We visited both taken and fallthrough. Now check if we have successors we need to visit.
  if (have_successors_ == true) {
    // Get information about next successor block.
    SuccessorBlockInfo* successor_block_info = successor_iter_.Next();

    // If we don't have anymore successors, return nullptr.
    if (successor_block_info != nullptr) {
      return mir_graph_->GetBasicBlock(successor_block_info->block);
    }
  }

  // We do not have anything.
  return nullptr;
}

}  // namespace art
