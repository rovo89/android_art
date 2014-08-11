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
#include <queue>

#include "base/stl_util.h"
#include "compiler_internals.h"
#include "dex_file-inl.h"
#include "dex_instruction-inl.h"
#include "dex/global_value_numbering.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "dex/quick/dex_file_method_inliner.h"
#include "leb128.h"
#include "pass_driver_me_post_opt.h"
#include "utils/scoped_arena_containers.h"

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
  "ReserveVectorRegisters",
  "ReturnVectorRegisters",
};

MIRGraph::MIRGraph(CompilationUnit* cu, ArenaAllocator* arena)
    : reg_location_(NULL),
      block_id_map_(std::less<unsigned int>(), arena->Adapter()),
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
      max_num_reachable_blocks_(0),
      dfs_order_(NULL),
      dfs_post_order_(NULL),
      dom_post_order_traversal_(NULL),
      topological_order_(nullptr),
      topological_order_loop_ends_(nullptr),
      topological_order_indexes_(nullptr),
      topological_order_loop_head_stack_(nullptr),
      i_dom_list_(NULL),
      def_block_matrix_(NULL),
      temp_scoped_alloc_(),
      temp_insn_data_(nullptr),
      temp_bit_vector_size_(0u),
      temp_bit_vector_(nullptr),
      temp_gvn_(),
      block_list_(arena, 100, kGrowableArrayBlockList),
      try_block_addr_(NULL),
      entry_block_(NULL),
      exit_block_(NULL),
      num_blocks_(0),
      current_code_item_(NULL),
      dex_pc_to_block_map_(arena, 0, kGrowableArrayMisc),
      m_units_(arena->Adapter()),
      method_stack_(arena->Adapter()),
      current_method_(kInvalidEntry),
      current_offset_(kInvalidEntry),
      def_count_(0),
      opcode_count_(NULL),
      num_ssa_regs_(0),
      extended_basic_blocks_(arena->Adapter()),
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
      method_lowering_infos_(arena, 0u),
      gen_suspend_test_list_(arena, 0u),
      qcm(nullptr) {
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
  BasicBlock* bottom_block = NewMemBB(kDalvikByteCode, num_blocks_++);
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
    orig_block->successor_blocks = nullptr;
    GrowableArray<SuccessorBlockInfo*>::Iterator iterator(bottom_block->successor_blocks);
    while (true) {
      SuccessorBlockInfo* successor_block_info = iterator.Next();
      if (successor_block_info == nullptr) break;
      BasicBlock* bb = GetBasicBlock(successor_block_info->block);
      if (bb != nullptr) {
        bb->predecessors->Delete(orig_block->id);
        bb->predecessors->Insert(bottom_block->id);
      }
    }
  }

  orig_block->last_mir_insn = prev;
  prev->next = nullptr;

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
         !MIR::DecodedInstruction::IsPseudoMirOp(insn->dalvikInsn.opcode));
  DCHECK_EQ(dex_pc_to_block_map_.Get(insn->offset), orig_block->id);
  MIR* p = insn;
  dex_pc_to_block_map_.Put(p->offset, bottom_block->id);
  while (p != bottom_block->last_mir_insn) {
    p = p->next;
    DCHECK(p != nullptr);
    p->bb = bottom_block->id;
    int opcode = p->dalvikInsn.opcode;
    /*
     * Some messiness here to ensure that we only enter real opcodes and only the
     * first half of a potentially throwing instruction that has been split into
     * CHECK and work portions. Since the 2nd half of a split operation is always
     * the first in a BasicBlock, we can't hit it here.
     */
    if ((opcode == kMirOpCheck) || !MIR::DecodedInstruction::IsPseudoMirOp(opcode)) {
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

  // Iterate over each of the handlers to enqueue the empty Catch blocks.
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

bool MIRGraph::IsBadMonitorExitCatch(NarrowDexOffset monitor_exit_offset,
                                     NarrowDexOffset catch_offset) {
  // Catches for monitor-exit during stack unwinding have the pattern
  //   move-exception (move)* (goto)? monitor-exit throw
  // In the currently generated dex bytecode we see these catching a bytecode range including
  // either its own or an identical monitor-exit, http://b/15745363 . This function checks if
  // it's the case for a given monitor-exit and catch block so that we can ignore it.
  // (We don't want to ignore all monitor-exit catches since one could enclose a synchronized
  // block in a try-block and catch the NPE, Error or Throwable and we should let it through;
  // even though a throwing monitor-exit certainly indicates a bytecode error.)
  const Instruction* monitor_exit = Instruction::At(cu_->code_item->insns_ + monitor_exit_offset);
  DCHECK(monitor_exit->Opcode() == Instruction::MONITOR_EXIT);
  int monitor_reg = monitor_exit->VRegA_11x();
  const Instruction* check_insn = Instruction::At(cu_->code_item->insns_ + catch_offset);
  DCHECK(check_insn->Opcode() == Instruction::MOVE_EXCEPTION);
  if (check_insn->VRegA_11x() == monitor_reg) {
    // Unexpected move-exception to the same register. Probably not the pattern we're looking for.
    return false;
  }
  check_insn = check_insn->Next();
  while (true) {
    int dest = -1;
    bool wide = false;
    switch (check_insn->Opcode()) {
      case Instruction::MOVE_WIDE:
        wide = true;
        // Intentional fall-through.
      case Instruction::MOVE_OBJECT:
      case Instruction::MOVE:
        dest = check_insn->VRegA_12x();
        break;

      case Instruction::MOVE_WIDE_FROM16:
        wide = true;
        // Intentional fall-through.
      case Instruction::MOVE_OBJECT_FROM16:
      case Instruction::MOVE_FROM16:
        dest = check_insn->VRegA_22x();
        break;

      case Instruction::MOVE_WIDE_16:
        wide = true;
        // Intentional fall-through.
      case Instruction::MOVE_OBJECT_16:
      case Instruction::MOVE_16:
        dest = check_insn->VRegA_32x();
        break;

      case Instruction::GOTO:
      case Instruction::GOTO_16:
      case Instruction::GOTO_32:
        check_insn = check_insn->RelativeAt(check_insn->GetTargetOffset());
        // Intentional fall-through.
      default:
        return check_insn->Opcode() == Instruction::MONITOR_EXIT &&
            check_insn->VRegA_11x() == monitor_reg;
    }

    if (dest == monitor_reg || (wide && dest + 1 == monitor_reg)) {
      return false;
    }

    check_insn = check_insn->Next();
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
  BasicBlock* taken_block = FindBlock(target, /* split */ true, /* create */ true,
                                      /* immed_pred_block_p */ &cur_block);
  cur_block->taken = taken_block->id;
  taken_block->predecessors->Insert(cur_block->id);

  /* Always terminate the current block for conditional branches */
  if (flags & Instruction::kContinue) {
    BasicBlock* fallthrough_block = FindBlock(cur_offset +  width,
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
    keyTable = NULL;        // Make the compiler happy.
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
    first_key = 0;   // To make the compiler happy.
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
    BasicBlock* case_block = FindBlock(cur_offset + target_table[i], /* split */ true,
                                      /* create */ true, /* immed_pred_block_p */ &cur_block);
    SuccessorBlockInfo* successor_block_info =
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

    for (; iterator.HasNext(); iterator.Next()) {
      BasicBlock* catch_block = FindBlock(iterator.GetHandlerAddress(), false /* split*/,
                                         false /* creat */, NULL  /* immed_pred_block_p */);
      if (insn->dalvikInsn.opcode == Instruction::MONITOR_EXIT &&
          IsBadMonitorExitCatch(insn->offset, catch_block->start_offset)) {
        // Don't allow monitor-exit to catch its own exception, http://b/15745363 .
        continue;
      }
      if (cur_block->successor_block_list_type == kNotUsed) {
        cur_block->successor_block_list_type = kCatch;
        cur_block->successor_blocks = new (arena_) GrowableArray<SuccessorBlockInfo*>(
            arena_, 2, kGrowableArraySuccessorBlocks);
      }
      catch_block->catch_entry = true;
      if (kIsDebugBuild) {
        catches_.insert(catch_block->start_offset);
      }
      SuccessorBlockInfo* successor_block_info = reinterpret_cast<SuccessorBlockInfo*>
          (arena_->Alloc(sizeof(SuccessorBlockInfo), kArenaAllocSuccessor));
      successor_block_info->block = catch_block->id;
      successor_block_info->key = iterator.GetHandlerTypeIndex();
      cur_block->successor_blocks->Insert(successor_block_info);
      catch_block->predecessors->Insert(cur_block->id);
    }
    in_try_block = (cur_block->successor_block_list_type != kNotUsed);
  }
  if (!in_try_block && build_all_edges) {
    BasicBlock* eh_block = NewMemBB(kExceptionHandling, num_blocks_++);
    cur_block->taken = eh_block->id;
    block_list_.Insert(eh_block);
    eh_block->start_offset = cur_offset;
    eh_block->predecessors->Insert(cur_block->id);
  }

  if (is_throw) {
    cur_block->explicit_throw = true;
    if (code_ptr < code_end) {
      // Force creation of new block following THROW via side-effect.
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
  BasicBlock* new_block = NewMemBB(kDalvikByteCode, num_blocks_++);
  block_list_.Insert(new_block);
  new_block->start_offset = insn->offset;
  cur_block->fall_through = new_block->id;
  new_block->predecessors->Insert(cur_block->id);
  MIR* new_insn = NewMIR();
  *new_insn = *insn;
  insn->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpCheck);
  // Associate the two halves.
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
    DCHECK_EQ(num_blocks_, 0U);
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
  BasicBlock* cur_block = NewMemBB(kDalvikByteCode, num_blocks_++);
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
    MIR *insn = NewMIR();
    insn->offset = current_offset_;
    insn->m_unit_index = current_method_;
    int width = ParseInsn(code_ptr, &insn->dalvikInsn);
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

    // Check for inline data block signatures.
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
    if (verify_flags & Instruction::kVerifyVarArgRange ||
        verify_flags & Instruction::kVerifyVarArgRangeNonZero) {
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
    BasicBlock* next_block = FindBlock(current_offset_, /* split */ false, /* create */
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


const char * MIRGraph::GetExtendedMirOpName(int index){
    return extended_mir_op_names_[index];
}

// TODO: use a configurable base prefix, and adjust callers to supply pass name.
/* Dump the CFG into a DOT graph */
void MIRGraph::DumpCFG(const char* dir_prefix, bool all_blocks, const char *suffix) {
  FILE* file;
  static AtomicInteger cnt(0);

  // Increment counter to get a unique file number.
  cnt++;

  std::string fname(PrettyMethod(cu_->method_idx, *cu_->dex_file));
  ReplaceSpecialChars(fname);
  fname = StringPrintf("%s%s%x%s_%d.dot", dir_prefix, fname.c_str(),
                      GetBasicBlock(GetEntryBlock()->fall_through)->start_offset,
                      suffix == nullptr ? "" : suffix,
                      cnt.LoadRelaxed());
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
    BasicBlock* bb = GetBasicBlock(block_idx);
    if (bb == NULL) continue;
    if (bb->block_type == kDead) continue;
    if (bb->hidden) continue;
    if (bb->block_type == kEntryBlock) {
      fprintf(file, "  entry_%d [shape=Mdiamond];\n", bb->id);
    } else if (bb->block_type == kExitBlock) {
      fprintf(file, "  exit_%d [shape=Mdiamond];\n", bb->id);
    } else if (bb->block_type == kDalvikByteCode) {
      fprintf(file, "  block%04x_%d [shape=record,label = \"{ \\\n",
              bb->start_offset, bb->id);
      const MIR* mir;
        fprintf(file, "    {block id %d\\l}%s\\\n", bb->id,
                bb->first_mir_insn ? " | " : " ");
        for (mir = bb->first_mir_insn; mir; mir = mir->next) {
            int opcode = mir->dalvikInsn.opcode;
            if (opcode > kMirOpSelect && opcode < kMirOpLast) {
              if (opcode == kMirOpConstVector) {
                fprintf(file, "    {%04x %s %d %d %d %d %d %d\\l}%s\\\n", mir->offset,
                        MIRGraph::GetExtendedMirOpName(kMirOpConstVector - kMirOpFirst),
                        mir->dalvikInsn.vA,
                        mir->dalvikInsn.vB,
                        mir->dalvikInsn.arg[0],
                        mir->dalvikInsn.arg[1],
                        mir->dalvikInsn.arg[2],
                        mir->dalvikInsn.arg[3],
                        mir->next ? " | " : " ");
              } else {
                fprintf(file, "    {%04x %s %d %d %d\\l}%s\\\n", mir->offset,
                        MIRGraph::GetExtendedMirOpName(opcode - kMirOpFirst),
                        mir->dalvikInsn.vA,
                        mir->dalvikInsn.vB,
                        mir->dalvikInsn.vC,
                        mir->next ? " | " : " ");
              }
            } else {
              fprintf(file, "    {%04x %s %s %s %s\\l}%s\\\n", mir->offset,
                      mir->ssa_rep ? GetDalvikDisassembly(mir) :
                      !MIR::DecodedInstruction::IsPseudoMirOp(opcode) ?
                        Instruction::Name(mir->dalvikInsn.opcode) :
                        MIRGraph::GetExtendedMirOpName(opcode - kMirOpFirst),
                      (mir->optimization_flags & MIR_IGNORE_RANGE_CHECK) != 0 ? " no_rangecheck" : " ",
                      (mir->optimization_flags & MIR_IGNORE_NULL_CHECK) != 0 ? " no_nullcheck" : " ",
                      (mir->optimization_flags & MIR_IGNORE_SUSPEND_CHECK) != 0 ? " no_suspendcheck" : " ",
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
      SuccessorBlockInfo* successor_block_info = iterator.Next();

      int succ_id = 0;
      while (true) {
        if (successor_block_info == NULL) break;

        BasicBlock* dest_block = GetBasicBlock(successor_block_info->block);
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
        SuccessorBlockInfo* successor_block_info = iter.Next();
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

/* Insert an MIR instruction to the end of a basic block. */
void BasicBlock::AppendMIR(MIR* mir) {
  // Insert it after the last MIR.
  InsertMIRListAfter(last_mir_insn, mir, mir);
}

void BasicBlock::AppendMIRList(MIR* first_list_mir, MIR* last_list_mir) {
  // Insert it after the last MIR.
  InsertMIRListAfter(last_mir_insn, first_list_mir, last_list_mir);
}

void BasicBlock::AppendMIRList(const std::vector<MIR*>& insns) {
  for (std::vector<MIR*>::const_iterator it = insns.begin(); it != insns.end(); it++) {
    MIR* new_mir = *it;

    // Add a copy of each MIR.
    InsertMIRListAfter(last_mir_insn, new_mir, new_mir);
  }
}

/* Insert a MIR instruction after the specified MIR. */
void BasicBlock::InsertMIRAfter(MIR* current_mir, MIR* new_mir) {
  InsertMIRListAfter(current_mir, new_mir, new_mir);
}

void BasicBlock::InsertMIRListAfter(MIR* insert_after, MIR* first_list_mir, MIR* last_list_mir) {
  // If no MIR, we are done.
  if (first_list_mir == nullptr || last_list_mir == nullptr) {
    return;
  }

  // If insert_after is null, assume BB is empty.
  if (insert_after == nullptr) {
    first_mir_insn = first_list_mir;
    last_mir_insn = last_list_mir;
    last_list_mir->next = nullptr;
  } else {
    MIR* after_list = insert_after->next;
    insert_after->next = first_list_mir;
    last_list_mir->next = after_list;
    if (after_list == nullptr) {
      last_mir_insn = last_list_mir;
    }
  }

  // Set this BB to be the basic block of the MIRs.
  MIR* last = last_list_mir->next;
  for (MIR* mir = first_list_mir; mir != last; mir = mir->next) {
    mir->bb = id;
  }
}

/* Insert an MIR instruction to the head of a basic block. */
void BasicBlock::PrependMIR(MIR* mir) {
  InsertMIRListBefore(first_mir_insn, mir, mir);
}

void BasicBlock::PrependMIRList(MIR* first_list_mir, MIR* last_list_mir) {
  // Insert it before the first MIR.
  InsertMIRListBefore(first_mir_insn, first_list_mir, last_list_mir);
}

void BasicBlock::PrependMIRList(const std::vector<MIR*>& to_add) {
  for (std::vector<MIR*>::const_iterator it = to_add.begin(); it != to_add.end(); it++) {
    MIR* mir = *it;

    InsertMIRListBefore(first_mir_insn, mir, mir);
  }
}

/* Insert a MIR instruction before the specified MIR. */
void BasicBlock::InsertMIRBefore(MIR* current_mir, MIR* new_mir) {
  // Insert as a single element list.
  return InsertMIRListBefore(current_mir, new_mir, new_mir);
}

MIR* BasicBlock::FindPreviousMIR(MIR* mir) {
  MIR* current = first_mir_insn;

  while (current != nullptr) {
    MIR* next = current->next;

    if (next == mir) {
      return current;
    }

    current = next;
  }

  return nullptr;
}

void BasicBlock::InsertMIRListBefore(MIR* insert_before, MIR* first_list_mir, MIR* last_list_mir) {
  // If no MIR, we are done.
  if (first_list_mir == nullptr || last_list_mir == nullptr) {
    return;
  }

  // If insert_before is null, assume BB is empty.
  if (insert_before == nullptr) {
    first_mir_insn = first_list_mir;
    last_mir_insn = last_list_mir;
    last_list_mir->next = nullptr;
  } else {
    if (first_mir_insn == insert_before) {
      last_list_mir->next = first_mir_insn;
      first_mir_insn = first_list_mir;
    } else {
      // Find the preceding MIR.
      MIR* before_list = FindPreviousMIR(insert_before);
      DCHECK(before_list != nullptr);
      before_list->next = first_list_mir;
      last_list_mir->next = insert_before;
    }
  }

  // Set this BB to be the basic block of the MIRs.
  for (MIR* mir = first_list_mir; mir != last_list_mir->next; mir = mir->next) {
    mir->bb = id;
  }
}

bool BasicBlock::RemoveMIR(MIR* mir) {
  // Remove as a single element list.
  return RemoveMIRList(mir, mir);
}

bool BasicBlock::RemoveMIRList(MIR* first_list_mir, MIR* last_list_mir) {
  if (first_list_mir == nullptr) {
    return false;
  }

  // Try to find the MIR.
  MIR* before_list = nullptr;
  MIR* after_list = nullptr;

  // If we are removing from the beginning of the MIR list.
  if (first_mir_insn == first_list_mir) {
    before_list = nullptr;
  } else {
    before_list = FindPreviousMIR(first_list_mir);
    if (before_list == nullptr) {
      // We did not find the mir.
      return false;
    }
  }

  // Remove the BB information and also find the after_list.
  for (MIR* mir = first_list_mir; mir != last_list_mir; mir = mir->next) {
    mir->bb = NullBasicBlockId;
  }

  after_list = last_list_mir->next;

  // If there is nothing before the list, after_list is the first_mir.
  if (before_list == nullptr) {
    first_mir_insn = after_list;
  } else {
    before_list->next = after_list;
  }

  // If there is nothing after the list, before_list is last_mir.
  if (after_list == nullptr) {
    last_mir_insn = before_list;
  }

  return true;
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
  Instruction::Format dalvik_format = Instruction::k10x;  // Default to no-operand format.
  int defs = (ssa_rep != NULL) ? ssa_rep->num_defs : 0;
  int uses = (ssa_rep != NULL) ? ssa_rep->num_uses : 0;

  // Handle special cases.
  if ((opcode == kMirOpCheck) || (opcode == kMirOpCheckPart2)) {
    str.append(MIRGraph::GetExtendedMirOpName(opcode - kMirOpFirst));
    str.append(": ");
    // Recover the original Dex instruction.
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

  if (MIR::DecodedInstruction::IsPseudoMirOp(opcode)) {
    str.append(MIRGraph::GetExtendedMirOpName(opcode - kMirOpFirst));
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
    // For invokes-style formats, treat wide regs as a pair of singles.
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
      case Instruction::k11n:  // Add one immediate from vB.
      case Instruction::k21s:
      case Instruction::k31i:
      case Instruction::k21h:
        str.append(StringPrintf(", #%d", insn.vB));
        break;
      case Instruction::k51l:  // Add one wide immediate.
        str.append(StringPrintf(", #%" PRId64, insn.vB_wide));
        break;
      case Instruction::k21c:  // One register, one string/type/method index.
      case Instruction::k31c:
        str.append(StringPrintf(", index #%d", insn.vB));
        break;
      case Instruction::k22c:  // Two registers, one string/type/method index.
        str.append(StringPrintf(", index #%d", insn.vC));
        break;
      case Instruction::k22s:  // Add one immediate from vC.
      case Instruction::k22b:
        str.append(StringPrintf(", #%d", insn.vC));
        break;
      default: {
        // Nothing left to print.
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
    // Pre-SSA - just use the standard name.
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

// Allocate a new MIR.
MIR* MIRGraph::NewMIR() {
  MIR* mir = new (arena_) MIR();
  return mir;
}

// Allocate a new basic block.
BasicBlock* MIRGraph::NewMemBB(BBType block_type, int block_id) {
  BasicBlock* bb = new (arena_) BasicBlock();

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
  // The gate starts by initializing the use counts.
  int num_ssa_regs = GetNumSSARegs();
  use_counts_.Resize(num_ssa_regs + 32);
  raw_use_counts_.Resize(num_ssa_regs + 32);
  // Initialize list.
  for (int i = 0; i < num_ssa_regs; i++) {
    use_counts_.Insert(0);
    raw_use_counts_.Insert(0);
  }
}

void MIRGraph::SSATransformationStart() {
  DCHECK(temp_scoped_alloc_.get() == nullptr);
  temp_scoped_alloc_.reset(ScopedArenaAllocator::Create(&cu_->arena_stack));
  temp_bit_vector_size_ = cu_->num_dalvik_registers;
  temp_bit_vector_ = new (temp_scoped_alloc_.get()) ArenaBitVector(
      temp_scoped_alloc_.get(), temp_bit_vector_size_, false, kBitMapRegisterV);

  // Update the maximum number of reachable blocks.
  max_num_reachable_blocks_ = num_reachable_blocks_;
}

void MIRGraph::SSATransformationEnd() {
  // Verify the dataflow information after the pass.
  if (cu_->enable_debug & (1 << kDebugVerifyDataflow)) {
    VerifyDataflow();
  }

  temp_bit_vector_size_ = 0u;
  temp_bit_vector_ = nullptr;
  DCHECK(temp_scoped_alloc_.get() != nullptr);
  temp_scoped_alloc_.reset();
}

static BasicBlock* SelectTopologicalSortOrderFallBack(
    MIRGraph* mir_graph, const ArenaBitVector* current_loop,
    const ScopedArenaVector<size_t>* visited_cnt_values, ScopedArenaAllocator* allocator,
    ScopedArenaVector<BasicBlockId>* tmp_stack) {
  // No true loop head has been found but there may be true loop heads after the mess we need
  // to resolve. To avoid taking one of those, pick the candidate with the highest number of
  // reachable unvisited nodes. That candidate will surely be a part of a loop.
  BasicBlock* fall_back = nullptr;
  size_t fall_back_num_reachable = 0u;
  // Reuse the same bit vector for each candidate to mark reachable unvisited blocks.
  ArenaBitVector candidate_reachable(allocator, mir_graph->GetNumBlocks(), false, kBitMapMisc);
  AllNodesIterator iter(mir_graph);
  for (BasicBlock* candidate = iter.Next(); candidate != nullptr; candidate = iter.Next()) {
    if (candidate->hidden ||                            // Hidden, or
        candidate->visited ||                           // already processed, or
        (*visited_cnt_values)[candidate->id] == 0u ||   // no processed predecessors, or
        (current_loop != nullptr &&                     // outside current loop.
         !current_loop->IsBitSet(candidate->id))) {
      continue;
    }
    DCHECK(tmp_stack->empty());
    tmp_stack->push_back(candidate->id);
    candidate_reachable.ClearAllBits();
    size_t num_reachable = 0u;
    while (!tmp_stack->empty()) {
      BasicBlockId current_id = tmp_stack->back();
      tmp_stack->pop_back();
      BasicBlock* current_bb = mir_graph->GetBasicBlock(current_id);
      DCHECK(current_bb != nullptr);
      ChildBlockIterator child_iter(current_bb, mir_graph);
      BasicBlock* child_bb = child_iter.Next();
      for ( ; child_bb != nullptr; child_bb = child_iter.Next()) {
        DCHECK(!child_bb->hidden);
        if (child_bb->visited ||                            // Already processed, or
            (current_loop != nullptr &&                     // outside current loop.
             !current_loop->IsBitSet(child_bb->id))) {
          continue;
        }
        if (!candidate_reachable.IsBitSet(child_bb->id)) {
          candidate_reachable.SetBit(child_bb->id);
          tmp_stack->push_back(child_bb->id);
          num_reachable += 1u;
        }
      }
    }
    if (fall_back_num_reachable < num_reachable) {
      fall_back_num_reachable = num_reachable;
      fall_back = candidate;
    }
  }
  return fall_back;
}

// Compute from which unvisited blocks is bb_id reachable through unvisited blocks.
static void ComputeUnvisitedReachableFrom(MIRGraph* mir_graph, BasicBlockId bb_id,
                                          ArenaBitVector* reachable,
                                          ScopedArenaVector<BasicBlockId>* tmp_stack) {
  // NOTE: Loop heads indicated by the "visited" flag.
  DCHECK(tmp_stack->empty());
  reachable->ClearAllBits();
  tmp_stack->push_back(bb_id);
  while (!tmp_stack->empty()) {
    BasicBlockId current_id = tmp_stack->back();
    tmp_stack->pop_back();
    BasicBlock* current_bb = mir_graph->GetBasicBlock(current_id);
    DCHECK(current_bb != nullptr);
    GrowableArray<BasicBlockId>::Iterator iter(current_bb->predecessors);
    BasicBlock* pred_bb = mir_graph->GetBasicBlock(iter.Next());
    for ( ; pred_bb != nullptr; pred_bb = mir_graph->GetBasicBlock(iter.Next())) {
      if (!pred_bb->visited && !reachable->IsBitSet(pred_bb->id)) {
        reachable->SetBit(pred_bb->id);
        tmp_stack->push_back(pred_bb->id);
      }
    }
  }
}

void MIRGraph::ComputeTopologicalSortOrder() {
  ScopedArenaAllocator allocator(&cu_->arena_stack);
  unsigned int num_blocks = GetNumBlocks();

  ScopedArenaQueue<BasicBlock*> q(allocator.Adapter());
  ScopedArenaVector<size_t> visited_cnt_values(num_blocks, 0u, allocator.Adapter());
  ScopedArenaVector<BasicBlockId> loop_head_stack(allocator.Adapter());
  size_t max_nested_loops = 0u;
  ArenaBitVector loop_exit_blocks(&allocator, num_blocks, false, kBitMapMisc);
  loop_exit_blocks.ClearAllBits();

  // Count the number of blocks to process and add the entry block(s).
  GrowableArray<BasicBlock*>::Iterator iterator(&block_list_);
  unsigned int num_blocks_to_process = 0u;
  for (BasicBlock* bb = iterator.Next(); bb != nullptr; bb = iterator.Next()) {
    if (bb->hidden == true) {
      continue;
    }

    num_blocks_to_process += 1u;

    if (bb->predecessors->Size() == 0u) {
      // Add entry block to the queue.
      q.push(bb);
    }
  }

  // Create the topological order if need be.
  if (topological_order_ == nullptr) {
    topological_order_ = new (arena_) GrowableArray<BasicBlockId>(arena_, num_blocks);
    topological_order_loop_ends_ = new (arena_) GrowableArray<uint16_t>(arena_, num_blocks);
    topological_order_indexes_ = new (arena_) GrowableArray<uint16_t>(arena_, num_blocks);
  }
  topological_order_->Reset();
  topological_order_loop_ends_->Reset();
  topological_order_indexes_->Reset();
  topological_order_loop_ends_->Resize(num_blocks);
  topological_order_indexes_->Resize(num_blocks);
  for (BasicBlockId i = 0; i != num_blocks; ++i) {
    topological_order_loop_ends_->Insert(0u);
    topological_order_indexes_->Insert(static_cast<uint16_t>(-1));
  }

  // Mark all blocks as unvisited.
  ClearAllVisitedFlags();

  // For loop heads, keep track from which blocks they are reachable not going through other
  // loop heads. Other loop heads are excluded to detect the heads of nested loops. The children
  // in this set go into the loop body, the other children are jumping over the loop.
  ScopedArenaVector<ArenaBitVector*> loop_head_reachable_from(allocator.Adapter());
  loop_head_reachable_from.resize(num_blocks, nullptr);
  // Reuse the same temp stack whenever calculating a loop_head_reachable_from[loop_head_id].
  ScopedArenaVector<BasicBlockId> tmp_stack(allocator.Adapter());

  while (num_blocks_to_process != 0u) {
    BasicBlock* bb = nullptr;
    if (!q.empty()) {
      num_blocks_to_process -= 1u;
      // Get top.
      bb = q.front();
      q.pop();
      if (bb->visited) {
        // Loop head: it was already processed, mark end and copy exit blocks to the queue.
        DCHECK(q.empty()) << PrettyMethod(cu_->method_idx, *cu_->dex_file);
        uint16_t idx = static_cast<uint16_t>(topological_order_->Size());
        topological_order_loop_ends_->Put(topological_order_indexes_->Get(bb->id), idx);
        DCHECK_EQ(loop_head_stack.back(), bb->id);
        loop_head_stack.pop_back();
        ArenaBitVector* reachable =
            loop_head_stack.empty() ? nullptr : loop_head_reachable_from[loop_head_stack.back()];
        for (BasicBlockId candidate_id : loop_exit_blocks.Indexes()) {
          if (reachable == nullptr || reachable->IsBitSet(candidate_id)) {
            q.push(GetBasicBlock(candidate_id));
            // NOTE: The BitVectorSet::IndexIterator will not check the pointed-to bit again,
            // so clearing the bit has no effect on the iterator.
            loop_exit_blocks.ClearBit(candidate_id);
          }
        }
        continue;
      }
    } else {
      // Find the new loop head.
      AllNodesIterator iter(this);
      while (true) {
        BasicBlock* candidate = iter.Next();
        if (candidate == nullptr) {
          // We did not find a true loop head, fall back to a reachable block in any loop.
          ArenaBitVector* current_loop =
              loop_head_stack.empty() ? nullptr : loop_head_reachable_from[loop_head_stack.back()];
          bb = SelectTopologicalSortOrderFallBack(this, current_loop, &visited_cnt_values,
                                                  &allocator, &tmp_stack);
          DCHECK(bb != nullptr) << PrettyMethod(cu_->method_idx, *cu_->dex_file);
          if (kIsDebugBuild && cu_->dex_file != nullptr) {
            LOG(INFO) << "Topological sort order: Using fall-back in "
                << PrettyMethod(cu_->method_idx, *cu_->dex_file) << " BB #" << bb->id
                << " @0x" << std::hex << bb->start_offset
                << ", num_blocks = " << std::dec << num_blocks;
          }
          break;
        }
        if (candidate->hidden ||                            // Hidden, or
            candidate->visited ||                           // already processed, or
            visited_cnt_values[candidate->id] == 0u ||      // no processed predecessors, or
            (!loop_head_stack.empty() &&                    // outside current loop.
             !loop_head_reachable_from[loop_head_stack.back()]->IsBitSet(candidate->id))) {
          continue;
        }

        GrowableArray<BasicBlockId>::Iterator pred_iter(candidate->predecessors);
        BasicBlock* pred_bb = GetBasicBlock(pred_iter.Next());
        for ( ; pred_bb != nullptr; pred_bb = GetBasicBlock(pred_iter.Next())) {
          if (pred_bb != candidate && !pred_bb->visited &&
              !pred_bb->dominators->IsBitSet(candidate->id)) {
            break;  // Keep non-null pred_bb to indicate failure.
          }
        }
        if (pred_bb == nullptr) {
          bb = candidate;
          break;
        }
      }
      // Compute blocks from which the loop head is reachable and process those blocks first.
      ArenaBitVector* reachable =
          new (&allocator) ArenaBitVector(&allocator, num_blocks, false, kBitMapMisc);
      loop_head_reachable_from[bb->id] = reachable;
      ComputeUnvisitedReachableFrom(this, bb->id, reachable, &tmp_stack);
      // Now mark as loop head. (Even if it's only a fall back when we don't find a true loop.)
      loop_head_stack.push_back(bb->id);
      max_nested_loops = std::max(max_nested_loops, loop_head_stack.size());
    }

    DCHECK_EQ(bb->hidden, false);
    DCHECK_EQ(bb->visited, false);
    bb->visited = true;

    // Now add the basic block.
    uint16_t idx = static_cast<uint16_t>(topological_order_->Size());
    topological_order_indexes_->Put(bb->id, idx);
    topological_order_->Insert(bb->id);

    // Update visited_cnt_values for children.
    ChildBlockIterator succIter(bb, this);
    BasicBlock* successor = succIter.Next();
    for ( ; successor != nullptr; successor = succIter.Next()) {
      if (successor->hidden) {
        continue;
      }

      // One more predecessor was visited.
      visited_cnt_values[successor->id] += 1u;
      if (visited_cnt_values[successor->id] == successor->predecessors->Size()) {
        if (loop_head_stack.empty() ||
            loop_head_reachable_from[loop_head_stack.back()]->IsBitSet(successor->id)) {
          q.push(successor);
        } else {
          DCHECK(!loop_exit_blocks.IsBitSet(successor->id));
          loop_exit_blocks.SetBit(successor->id);
        }
      }
    }
  }

  // Prepare the loop head stack for iteration.
  topological_order_loop_head_stack_ =
      new (arena_) GrowableArray<std::pair<uint16_t, bool>>(arena_, max_nested_loops);
}

bool BasicBlock::IsExceptionBlock() const {
  if (block_type == kExceptionHandling) {
    return true;
  }
  return false;
}

bool MIRGraph::HasSuspendTestBetween(BasicBlock* source, BasicBlockId target_id) {
  BasicBlock* target = GetBasicBlock(target_id);

  if (source == nullptr || target == nullptr)
    return false;

  int idx;
  for (idx = gen_suspend_test_list_.Size() - 1; idx >= 0; idx--) {
    BasicBlock* bb = gen_suspend_test_list_.Get(idx);
    if (bb == source)
      return true;  // The block has been inserted by a suspend check before.
    if (source->dominators->IsBitSet(bb->id) && bb->dominators->IsBitSet(target_id))
      return true;
  }

  return false;
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
    for (SuccessorBlockInfo* successor_block_info = successor_iter_.Next();
      successor_block_info != nullptr;
      successor_block_info = successor_iter_.Next()) {
      // If block was replaced by zero block, take next one.
      if (successor_block_info->block != NullBasicBlockId) {
        return mir_graph_->GetBasicBlock(successor_block_info->block);
      }
    }
  }

  // We do not have anything.
  return nullptr;
}

BasicBlock* BasicBlock::Copy(CompilationUnit* c_unit) {
  MIRGraph* mir_graph = c_unit->mir_graph.get();
  return Copy(mir_graph);
}

BasicBlock* BasicBlock::Copy(MIRGraph* mir_graph) {
  BasicBlock* result_bb = mir_graph->CreateNewBB(block_type);

  // We don't do a memcpy style copy here because it would lead to a lot of things
  // to clean up. Let us do it by hand instead.
  // Copy in taken and fallthrough.
  result_bb->fall_through = fall_through;
  result_bb->taken = taken;

  // Copy successor links if needed.
  ArenaAllocator* arena = mir_graph->GetArena();

  result_bb->successor_block_list_type = successor_block_list_type;
  if (result_bb->successor_block_list_type != kNotUsed) {
    size_t size = successor_blocks->Size();
    result_bb->successor_blocks = new (arena) GrowableArray<SuccessorBlockInfo*>(arena, size, kGrowableArraySuccessorBlocks);
    GrowableArray<SuccessorBlockInfo*>::Iterator iterator(successor_blocks);
    while (true) {
      SuccessorBlockInfo* sbi_old = iterator.Next();
      if (sbi_old == nullptr) {
        break;
      }
      SuccessorBlockInfo* sbi_new = static_cast<SuccessorBlockInfo*>(arena->Alloc(sizeof(SuccessorBlockInfo), kArenaAllocSuccessor));
      memcpy(sbi_new, sbi_old, sizeof(SuccessorBlockInfo));
      result_bb->successor_blocks->Insert(sbi_new);
    }
  }

  // Copy offset, method.
  result_bb->start_offset = start_offset;

  // Now copy instructions.
  for (MIR* mir = first_mir_insn; mir != 0; mir = mir->next) {
    // Get a copy first.
    MIR* copy = mir->Copy(mir_graph);

    // Append it.
    result_bb->AppendMIR(copy);
  }

  return result_bb;
}

MIR* MIR::Copy(MIRGraph* mir_graph) {
  MIR* res = mir_graph->NewMIR();
  *res = *this;

  // Remove links
  res->next = nullptr;
  res->bb = NullBasicBlockId;
  res->ssa_rep = nullptr;

  return res;
}

MIR* MIR::Copy(CompilationUnit* c_unit) {
  return Copy(c_unit->mir_graph.get());
}

uint32_t SSARepresentation::GetStartUseIndex(Instruction::Code opcode) {
  // Default result.
  int res = 0;

  // We are basically setting the iputs to their igets counterparts.
  switch (opcode) {
    case Instruction::IPUT:
    case Instruction::IPUT_OBJECT:
    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
    case Instruction::IPUT_CHAR:
    case Instruction::IPUT_SHORT:
    case Instruction::IPUT_QUICK:
    case Instruction::IPUT_OBJECT_QUICK:
    case Instruction::APUT:
    case Instruction::APUT_OBJECT:
    case Instruction::APUT_BOOLEAN:
    case Instruction::APUT_BYTE:
    case Instruction::APUT_CHAR:
    case Instruction::APUT_SHORT:
    case Instruction::SPUT:
    case Instruction::SPUT_OBJECT:
    case Instruction::SPUT_BOOLEAN:
    case Instruction::SPUT_BYTE:
    case Instruction::SPUT_CHAR:
    case Instruction::SPUT_SHORT:
      // Skip the VR containing what to store.
      res = 1;
      break;
    case Instruction::IPUT_WIDE:
    case Instruction::IPUT_WIDE_QUICK:
    case Instruction::APUT_WIDE:
    case Instruction::SPUT_WIDE:
      // Skip the two VRs containing what to store.
      res = 2;
      break;
    default:
      // Do nothing in the general case.
      break;
  }

  return res;
}

/**
 * @brief Given a decoded instruction, it checks whether the instruction
 * sets a constant and if it does, more information is provided about the
 * constant being set.
 * @param ptr_value pointer to a 64-bit holder for the constant.
 * @param wide Updated by function whether a wide constant is being set by bytecode.
 * @return Returns false if the decoded instruction does not represent a constant bytecode.
 */
bool MIR::DecodedInstruction::GetConstant(int64_t* ptr_value, bool* wide) const {
  bool sets_const = true;
  int64_t value = vB;

  DCHECK(ptr_value != nullptr);
  DCHECK(wide != nullptr);

  switch (opcode) {
    case Instruction::CONST_4:
    case Instruction::CONST_16:
    case Instruction::CONST:
      *wide = false;
      value <<= 32;      // In order to get the sign extend.
      value >>= 32;
      break;
    case Instruction::CONST_HIGH16:
      *wide = false;
      value <<= 48;      // In order to get the sign extend.
      value >>= 32;
      break;
    case Instruction::CONST_WIDE_16:
    case Instruction::CONST_WIDE_32:
      *wide = true;
      value <<= 32;      // In order to get the sign extend.
      value >>= 32;
      break;
    case Instruction::CONST_WIDE:
      *wide = true;
      value = vB_wide;
      break;
    case Instruction::CONST_WIDE_HIGH16:
      *wide = true;
      value <<= 48;      // In order to get the sign extend.
      break;
    default:
      sets_const = false;
      break;
  }

  if (sets_const) {
    *ptr_value = value;
  }

  return sets_const;
}

void BasicBlock::ResetOptimizationFlags(uint16_t reset_flags) {
  // Reset flags for all MIRs in bb.
  for (MIR* mir = first_mir_insn; mir != NULL; mir = mir->next) {
    mir->optimization_flags &= (~reset_flags);
  }
}

void BasicBlock::Hide(CompilationUnit* c_unit) {
  // First lets make it a dalvik bytecode block so it doesn't have any special meaning.
  block_type = kDalvikByteCode;

  // Mark it as hidden.
  hidden = true;

  // Detach it from its MIRs so we don't generate code for them. Also detached MIRs
  // are updated to know that they no longer have a parent.
  for (MIR* mir = first_mir_insn; mir != nullptr; mir = mir->next) {
    mir->bb = NullBasicBlockId;
  }
  first_mir_insn = nullptr;
  last_mir_insn = nullptr;

  GrowableArray<BasicBlockId>::Iterator iterator(predecessors);

  MIRGraph* mir_graph = c_unit->mir_graph.get();
  while (true) {
    BasicBlock* pred_bb = mir_graph->GetBasicBlock(iterator.Next());
    if (pred_bb == nullptr) {
      break;
    }

    // Sadly we have to go through the children by hand here.
    pred_bb->ReplaceChild(id, NullBasicBlockId);
  }

  // Iterate through children of bb we are hiding.
  ChildBlockIterator successorChildIter(this, mir_graph);

  for (BasicBlock* childPtr = successorChildIter.Next(); childPtr != 0; childPtr = successorChildIter.Next()) {
    // Replace child with null child.
    childPtr->predecessors->Delete(id);
  }
}

bool BasicBlock::IsSSALiveOut(const CompilationUnit* c_unit, int ssa_reg) {
  // In order to determine if the ssa reg is live out, we scan all the MIRs. We remember
  // the last SSA number of the same dalvik register. At the end, if it is different than ssa_reg,
  // then it is not live out of this BB.
  int dalvik_reg = c_unit->mir_graph->SRegToVReg(ssa_reg);

  int last_ssa_reg = -1;

  // Walk through the MIRs backwards.
  for (MIR* mir = first_mir_insn; mir != nullptr; mir = mir->next) {
    // Get ssa rep.
    SSARepresentation *ssa_rep = mir->ssa_rep;

    // Go through the defines for this MIR.
    for (int i = 0; i < ssa_rep->num_defs; i++) {
      DCHECK(ssa_rep->defs != nullptr);

      // Get the ssa reg.
      int def_ssa_reg = ssa_rep->defs[i];

      // Get dalvik reg.
      int def_dalvik_reg = c_unit->mir_graph->SRegToVReg(def_ssa_reg);

      // Compare dalvik regs.
      if (dalvik_reg == def_dalvik_reg) {
        // We found a def of the register that we are being asked about.
        // Remember it.
        last_ssa_reg = def_ssa_reg;
      }
    }
  }

  if (last_ssa_reg == -1) {
    // If we get to this point we couldn't find a define of register user asked about.
    // Let's assume the user knows what he's doing so we can be safe and say that if we
    // couldn't find a def, it is live out.
    return true;
  }

  // If it is not -1, we found a match, is it ssa_reg?
  return (ssa_reg == last_ssa_reg);
}

bool BasicBlock::ReplaceChild(BasicBlockId old_bb, BasicBlockId new_bb) {
  // We need to check taken, fall_through, and successor_blocks to replace.
  bool found = false;
  if (taken == old_bb) {
    taken = new_bb;
    found = true;
  }

  if (fall_through == old_bb) {
    fall_through = new_bb;
    found = true;
  }

  if (successor_block_list_type != kNotUsed) {
    GrowableArray<SuccessorBlockInfo*>::Iterator iterator(successor_blocks);
    while (true) {
      SuccessorBlockInfo* successor_block_info = iterator.Next();
      if (successor_block_info == nullptr) {
        break;
      }
      if (successor_block_info->block == old_bb) {
        successor_block_info->block = new_bb;
        found = true;
      }
    }
  }

  return found;
}

void BasicBlock::UpdatePredecessor(BasicBlockId old_parent, BasicBlockId new_parent) {
  GrowableArray<BasicBlockId>::Iterator iterator(predecessors);
  bool found = false;

  while (true) {
    BasicBlockId pred_bb_id = iterator.Next();

    if (pred_bb_id == NullBasicBlockId) {
      break;
    }

    if (pred_bb_id == old_parent) {
      size_t idx = iterator.GetIndex() - 1;
      predecessors->Put(idx, new_parent);
      found = true;
      break;
    }
  }

  // If not found, add it.
  if (found == false) {
    predecessors->Insert(new_parent);
  }
}

// Create a new basic block with block_id as num_blocks_ that is
// post-incremented.
BasicBlock* MIRGraph::CreateNewBB(BBType block_type) {
  BasicBlock* res = NewMemBB(block_type, num_blocks_++);
  block_list_.Insert(res);
  return res;
}

void MIRGraph::CalculateBasicBlockInformation() {
  PassDriverMEPostOpt driver(cu_);
  driver.Launch();
}

void MIRGraph::InitializeBasicBlockData() {
  num_blocks_ = block_list_.Size();
}

}  // namespace art
