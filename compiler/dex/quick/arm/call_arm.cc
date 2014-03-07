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

/* This file contains codegen for the Thumb2 ISA. */

#include "arm_lir.h"
#include "codegen_arm.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "entrypoints/quick/quick_entrypoints.h"

namespace art {

/*
 * The sparse table in the literal pool is an array of <key,displacement>
 * pairs.  For each set, we'll load them as a pair using ldmia.
 * This means that the register number of the temp we use for the key
 * must be lower than the reg for the displacement.
 *
 * The test loop will look something like:
 *
 *   adr   rBase, <table>
 *   ldr   r_val, [rARM_SP, v_reg_off]
 *   mov   r_idx, #table_size
 * lp:
 *   ldmia rBase!, {r_key, r_disp}
 *   sub   r_idx, #1
 *   cmp   r_val, r_key
 *   ifeq
 *   add   rARM_PC, r_disp   ; This is the branch from which we compute displacement
 *   cbnz  r_idx, lp
 */
void ArmMir2Lir::GenSparseSwitch(MIR* mir, uint32_t table_offset,
                                 RegLocation rl_src) {
  const uint16_t* table = cu_->insns + current_dalvik_offset_ + table_offset;
  if (cu_->verbose) {
    DumpSparseSwitchTable(table);
  }
  // Add the table to the list - we'll process it later
  SwitchTable *tab_rec =
      static_cast<SwitchTable*>(arena_->Alloc(sizeof(SwitchTable), kArenaAllocData));
  tab_rec->table = table;
  tab_rec->vaddr = current_dalvik_offset_;
  uint32_t size = table[1];
  tab_rec->targets = static_cast<LIR**>(arena_->Alloc(size * sizeof(LIR*),
                                                     kArenaAllocLIR));
  switch_tables_.Insert(tab_rec);

  // Get the switch value
  rl_src = LoadValue(rl_src, kCoreReg);
  int rBase = AllocTemp();
  /* Allocate key and disp temps */
  int r_key = AllocTemp();
  int r_disp = AllocTemp();
  // Make sure r_key's register number is less than r_disp's number for ldmia
  if (r_key > r_disp) {
    int tmp = r_disp;
    r_disp = r_key;
    r_key = tmp;
  }
  // Materialize a pointer to the switch table
  NewLIR3(kThumb2Adr, rBase, 0, WrapPointer(tab_rec));
  // Set up r_idx
  int r_idx = AllocTemp();
  LoadConstant(r_idx, size);
  // Establish loop branch target
  LIR* target = NewLIR0(kPseudoTargetLabel);
  // Load next key/disp
  NewLIR2(kThumb2LdmiaWB, rBase, (1 << r_key) | (1 << r_disp));
  OpRegReg(kOpCmp, r_key, rl_src.reg.GetReg());
  // Go if match. NOTE: No instruction set switch here - must stay Thumb2
  OpIT(kCondEq, "");
  LIR* switch_branch = NewLIR1(kThumb2AddPCR, r_disp);
  tab_rec->anchor = switch_branch;
  // Needs to use setflags encoding here
  OpRegRegImm(kOpSub, r_idx, r_idx, 1);  // For value == 1, this should set flags.
  DCHECK(last_lir_insn_->u.m.def_mask & ENCODE_CCODE);
  OpCondBranch(kCondNe, target);
}


void ArmMir2Lir::GenPackedSwitch(MIR* mir, uint32_t table_offset,
                                 RegLocation rl_src) {
  const uint16_t* table = cu_->insns + current_dalvik_offset_ + table_offset;
  if (cu_->verbose) {
    DumpPackedSwitchTable(table);
  }
  // Add the table to the list - we'll process it later
  SwitchTable *tab_rec =
      static_cast<SwitchTable*>(arena_->Alloc(sizeof(SwitchTable),  kArenaAllocData));
  tab_rec->table = table;
  tab_rec->vaddr = current_dalvik_offset_;
  uint32_t size = table[1];
  tab_rec->targets =
      static_cast<LIR**>(arena_->Alloc(size * sizeof(LIR*), kArenaAllocLIR));
  switch_tables_.Insert(tab_rec);

  // Get the switch value
  rl_src = LoadValue(rl_src, kCoreReg);
  int table_base = AllocTemp();
  // Materialize a pointer to the switch table
  NewLIR3(kThumb2Adr, table_base, 0, WrapPointer(tab_rec));
  int low_key = s4FromSwitchData(&table[2]);
  int keyReg;
  // Remove the bias, if necessary
  if (low_key == 0) {
    keyReg = rl_src.reg.GetReg();
  } else {
    keyReg = AllocTemp();
    OpRegRegImm(kOpSub, keyReg, rl_src.reg.GetReg(), low_key);
  }
  // Bounds check - if < 0 or >= size continue following switch
  OpRegImm(kOpCmp, keyReg, size-1);
  LIR* branch_over = OpCondBranch(kCondHi, NULL);

  // Load the displacement from the switch table
  int disp_reg = AllocTemp();
  LoadBaseIndexed(table_base, keyReg, disp_reg, 2, kWord);

  // ..and go! NOTE: No instruction set switch here - must stay Thumb2
  LIR* switch_branch = NewLIR1(kThumb2AddPCR, disp_reg);
  tab_rec->anchor = switch_branch;

  /* branch_over target here */
  LIR* target = NewLIR0(kPseudoTargetLabel);
  branch_over->target = target;
}

/*
 * Array data table format:
 *  ushort ident = 0x0300   magic value
 *  ushort width            width of each element in the table
 *  uint   size             number of elements in the table
 *  ubyte  data[size*width] table of data values (may contain a single-byte
 *                          padding at the end)
 *
 * Total size is 4+(width * size + 1)/2 16-bit code units.
 */
void ArmMir2Lir::GenFillArrayData(uint32_t table_offset, RegLocation rl_src) {
  const uint16_t* table = cu_->insns + current_dalvik_offset_ + table_offset;
  // Add the table to the list - we'll process it later
  FillArrayData *tab_rec =
      static_cast<FillArrayData*>(arena_->Alloc(sizeof(FillArrayData), kArenaAllocData));
  tab_rec->table = table;
  tab_rec->vaddr = current_dalvik_offset_;
  uint16_t width = tab_rec->table[1];
  uint32_t size = tab_rec->table[2] | ((static_cast<uint32_t>(tab_rec->table[3])) << 16);
  tab_rec->size = (size * width) + 8;

  fill_array_data_.Insert(tab_rec);

  // Making a call - use explicit registers
  FlushAllRegs();   /* Everything to home location */
  LoadValueDirectFixed(rl_src, r0);
  LoadWordDisp(rARM_SELF, QUICK_ENTRYPOINT_OFFSET(pHandleFillArrayData).Int32Value(),
               rARM_LR);
  // Materialize a pointer to the fill data image
  NewLIR3(kThumb2Adr, r1, 0, WrapPointer(tab_rec));
  ClobberCallerSave();
  LIR* call_inst = OpReg(kOpBlx, rARM_LR);
  MarkSafepointPC(call_inst);
}

/*
 * Handle unlocked -> thin locked transition inline or else call out to quick entrypoint. For more
 * details see monitor.cc.
 */
void ArmMir2Lir::GenMonitorEnter(int opt_flags, RegLocation rl_src) {
  FlushAllRegs();
  LoadValueDirectFixed(rl_src, r0);  // Get obj
  LockCallTemps();  // Prepare for explicit register usage
  constexpr bool kArchVariantHasGoodBranchPredictor = false;  // TODO: true if cortex-A15.
  if (kArchVariantHasGoodBranchPredictor) {
    LIR* null_check_branch;
    if ((opt_flags & MIR_IGNORE_NULL_CHECK) && !(cu_->disable_opt & (1 << kNullCheckElimination))) {
      null_check_branch = nullptr;  // No null check.
    } else {
      // If the null-check fails its handled by the slow-path to reduce exception related meta-data.
      null_check_branch = OpCmpImmBranch(kCondEq, r0, 0, NULL);
    }
    LoadWordDisp(rARM_SELF, Thread::ThinLockIdOffset().Int32Value(), r2);
    NewLIR3(kThumb2Ldrex, r1, r0, mirror::Object::MonitorOffset().Int32Value() >> 2);
    LIR* not_unlocked_branch = OpCmpImmBranch(kCondNe, r1, 0, NULL);
    NewLIR4(kThumb2Strex, r1, r2, r0, mirror::Object::MonitorOffset().Int32Value() >> 2);
    LIR* lock_success_branch = OpCmpImmBranch(kCondEq, r1, 0, NULL);


    LIR* slow_path_target = NewLIR0(kPseudoTargetLabel);
    not_unlocked_branch->target = slow_path_target;
    if (null_check_branch != nullptr) {
      null_check_branch->target = slow_path_target;
    }
    // TODO: move to a slow path.
    // Go expensive route - artLockObjectFromCode(obj);
    LoadWordDisp(rARM_SELF, QUICK_ENTRYPOINT_OFFSET(pLockObject).Int32Value(), rARM_LR);
    ClobberCallerSave();
    LIR* call_inst = OpReg(kOpBlx, rARM_LR);
    MarkSafepointPC(call_inst);

    LIR* success_target = NewLIR0(kPseudoTargetLabel);
    lock_success_branch->target = success_target;
    GenMemBarrier(kLoadLoad);
  } else {
    // Explicit null-check as slow-path is entered using an IT.
    GenNullCheck(rl_src.s_reg_low, r0, opt_flags);
    LoadWordDisp(rARM_SELF, Thread::ThinLockIdOffset().Int32Value(), r2);
    NewLIR3(kThumb2Ldrex, r1, r0, mirror::Object::MonitorOffset().Int32Value() >> 2);
    OpRegImm(kOpCmp, r1, 0);
    OpIT(kCondEq, "");
    NewLIR4(kThumb2Strex/*eq*/, r1, r2, r0, mirror::Object::MonitorOffset().Int32Value() >> 2);
    OpRegImm(kOpCmp, r1, 0);
    OpIT(kCondNe, "T");
    // Go expensive route - artLockObjectFromCode(self, obj);
    LoadWordDisp/*ne*/(rARM_SELF, QUICK_ENTRYPOINT_OFFSET(pLockObject).Int32Value(), rARM_LR);
    ClobberCallerSave();
    LIR* call_inst = OpReg(kOpBlx/*ne*/, rARM_LR);
    MarkSafepointPC(call_inst);
    GenMemBarrier(kLoadLoad);
  }
}

/*
 * Handle thin locked -> unlocked transition inline or else call out to quick entrypoint. For more
 * details see monitor.cc. Note the code below doesn't use ldrex/strex as the code holds the lock
 * and can only give away ownership if its suspended.
 */
void ArmMir2Lir::GenMonitorExit(int opt_flags, RegLocation rl_src) {
  FlushAllRegs();
  LoadValueDirectFixed(rl_src, r0);  // Get obj
  LockCallTemps();  // Prepare for explicit register usage
  LIR* null_check_branch;
  LoadWordDisp(rARM_SELF, Thread::ThinLockIdOffset().Int32Value(), r2);
  constexpr bool kArchVariantHasGoodBranchPredictor = false;  // TODO: true if cortex-A15.
  if (kArchVariantHasGoodBranchPredictor) {
    if ((opt_flags & MIR_IGNORE_NULL_CHECK) && !(cu_->disable_opt & (1 << kNullCheckElimination))) {
      null_check_branch = nullptr;  // No null check.
    } else {
      // If the null-check fails its handled by the slow-path to reduce exception related meta-data.
      null_check_branch = OpCmpImmBranch(kCondEq, r0, 0, NULL);
    }
    LoadWordDisp(r0, mirror::Object::MonitorOffset().Int32Value(), r1);
    LoadConstantNoClobber(r3, 0);
    LIR* slow_unlock_branch = OpCmpBranch(kCondNe, r1, r2, NULL);
    StoreWordDisp(r0, mirror::Object::MonitorOffset().Int32Value(), r3);
    LIR* unlock_success_branch = OpUnconditionalBranch(NULL);

    LIR* slow_path_target = NewLIR0(kPseudoTargetLabel);
    slow_unlock_branch->target = slow_path_target;
    if (null_check_branch != nullptr) {
      null_check_branch->target = slow_path_target;
    }
    // TODO: move to a slow path.
    // Go expensive route - artUnlockObjectFromCode(obj);
    LoadWordDisp(rARM_SELF, QUICK_ENTRYPOINT_OFFSET(pUnlockObject).Int32Value(), rARM_LR);
    ClobberCallerSave();
    LIR* call_inst = OpReg(kOpBlx, rARM_LR);
    MarkSafepointPC(call_inst);

    LIR* success_target = NewLIR0(kPseudoTargetLabel);
    unlock_success_branch->target = success_target;
    GenMemBarrier(kStoreLoad);
  } else {
    // Explicit null-check as slow-path is entered using an IT.
    GenNullCheck(rl_src.s_reg_low, r0, opt_flags);
    LoadWordDisp(r0, mirror::Object::MonitorOffset().Int32Value(), r1);  // Get lock
    LoadWordDisp(rARM_SELF, Thread::ThinLockIdOffset().Int32Value(), r2);
    LoadConstantNoClobber(r3, 0);
    // Is lock unheld on lock or held by us (==thread_id) on unlock?
    OpRegReg(kOpCmp, r1, r2);
    OpIT(kCondEq, "EE");
    StoreWordDisp/*eq*/(r0, mirror::Object::MonitorOffset().Int32Value(), r3);
    // Go expensive route - UnlockObjectFromCode(obj);
    LoadWordDisp/*ne*/(rARM_SELF, QUICK_ENTRYPOINT_OFFSET(pUnlockObject).Int32Value(), rARM_LR);
    ClobberCallerSave();
    LIR* call_inst = OpReg(kOpBlx/*ne*/, rARM_LR);
    MarkSafepointPC(call_inst);
    GenMemBarrier(kStoreLoad);
  }
}

void ArmMir2Lir::GenMoveException(RegLocation rl_dest) {
  int ex_offset = Thread::ExceptionOffset().Int32Value();
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  int reset_reg = AllocTemp();
  LoadWordDisp(rARM_SELF, ex_offset, rl_result.reg.GetReg());
  LoadConstant(reset_reg, 0);
  StoreWordDisp(rARM_SELF, ex_offset, reset_reg);
  FreeTemp(reset_reg);
  StoreValue(rl_dest, rl_result);
}

/*
 * Mark garbage collection card. Skip if the value we're storing is null.
 */
void ArmMir2Lir::MarkGCCard(int val_reg, int tgt_addr_reg) {
  int reg_card_base = AllocTemp();
  int reg_card_no = AllocTemp();
  LIR* branch_over = OpCmpImmBranch(kCondEq, val_reg, 0, NULL);
  LoadWordDisp(rARM_SELF, Thread::CardTableOffset().Int32Value(), reg_card_base);
  OpRegRegImm(kOpLsr, reg_card_no, tgt_addr_reg, gc::accounting::CardTable::kCardShift);
  StoreBaseIndexed(reg_card_base, reg_card_no, reg_card_base, 0,
                   kUnsignedByte);
  LIR* target = NewLIR0(kPseudoTargetLabel);
  branch_over->target = target;
  FreeTemp(reg_card_base);
  FreeTemp(reg_card_no);
}

void ArmMir2Lir::GenEntrySequence(RegLocation* ArgLocs, RegLocation rl_method) {
  int spill_count = num_core_spills_ + num_fp_spills_;
  /*
   * On entry, r0, r1, r2 & r3 are live.  Let the register allocation
   * mechanism know so it doesn't try to use any of them when
   * expanding the frame or flushing.  This leaves the utility
   * code with a single temp: r12.  This should be enough.
   */
  LockTemp(r0);
  LockTemp(r1);
  LockTemp(r2);
  LockTemp(r3);

  /*
   * We can safely skip the stack overflow check if we're
   * a leaf *and* our frame size < fudge factor.
   */
  bool skip_overflow_check = (mir_graph_->MethodIsLeaf() &&
                            (static_cast<size_t>(frame_size_) <
                            Thread::kStackOverflowReservedBytes));
  NewLIR0(kPseudoMethodEntry);
  if (!skip_overflow_check) {
    /* Load stack limit */
    LoadWordDisp(rARM_SELF, Thread::StackEndOffset().Int32Value(), r12);
  }
  /* Spill core callee saves */
  NewLIR1(kThumb2Push, core_spill_mask_);
  /* Need to spill any FP regs? */
  if (num_fp_spills_) {
    /*
     * NOTE: fp spills are a little different from core spills in that
     * they are pushed as a contiguous block.  When promoting from
     * the fp set, we must allocate all singles from s16..highest-promoted
     */
    NewLIR1(kThumb2VPushCS, num_fp_spills_);
  }
  if (!skip_overflow_check) {
    OpRegRegImm(kOpSub, rARM_LR, rARM_SP, frame_size_ - (spill_count * 4));
    GenRegRegCheck(kCondUlt, rARM_LR, r12, kThrowStackOverflow);
    OpRegCopy(rARM_SP, rARM_LR);     // Establish stack
  } else {
    OpRegImm(kOpSub, rARM_SP, frame_size_ - (spill_count * 4));
  }

  FlushIns(ArgLocs, rl_method);

  FreeTemp(r0);
  FreeTemp(r1);
  FreeTemp(r2);
  FreeTemp(r3);
}

void ArmMir2Lir::GenExitSequence() {
  int spill_count = num_core_spills_ + num_fp_spills_;
  /*
   * In the exit path, r0/r1 are live - make sure they aren't
   * allocated by the register utilities as temps.
   */
  LockTemp(r0);
  LockTemp(r1);

  NewLIR0(kPseudoMethodExit);
  OpRegImm(kOpAdd, rARM_SP, frame_size_ - (spill_count * 4));
  /* Need to restore any FP callee saves? */
  if (num_fp_spills_) {
    NewLIR1(kThumb2VPopCS, num_fp_spills_);
  }
  if (core_spill_mask_ & (1 << rARM_LR)) {
    /* Unspill rARM_LR to rARM_PC */
    core_spill_mask_ &= ~(1 << rARM_LR);
    core_spill_mask_ |= (1 << rARM_PC);
  }
  NewLIR1(kThumb2Pop, core_spill_mask_);
  if (!(core_spill_mask_ & (1 << rARM_PC))) {
    /* We didn't pop to rARM_PC, so must do a bv rARM_LR */
    NewLIR1(kThumbBx, rARM_LR);
  }
}

void ArmMir2Lir::GenSpecialExitSequence() {
  NewLIR1(kThumbBx, rARM_LR);
}

}  // namespace art
