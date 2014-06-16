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

#include "arm64_lir.h"
#include "codegen_arm64.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "gc/accounting/card_table.h"
#include "entrypoints/quick/quick_entrypoints.h"

namespace art {

bool Arm64Mir2Lir::GenSpecialCase(BasicBlock* bb, MIR* mir,
                                  const InlineMethod& special) {
  // TODO(Arm64): re-enable this, once hard-float ABI is implemented.
  //   (this currently does not work, as GetArgMappingToPhysicalReg returns InvalidReg()).
  // return Mir2Lir::GenSpecialCase(bb, mir, special);
  return false;
}

/*
 * The sparse table in the literal pool is an array of <key,displacement>
 * pairs.  For each set, we'll load them as a pair using ldp.
 * The test loop will look something like:
 *
 *   adr   r_base, <table>
 *   ldr   r_val, [rA64_SP, v_reg_off]
 *   mov   r_idx, #table_size
 * loop:
 *   cbz   r_idx, quit
 *   ldp   r_key, r_disp, [r_base], #8
 *   sub   r_idx, #1
 *   cmp   r_val, r_key
 *   b.ne  loop
 *   adr   r_base, #0        ; This is the instruction from which we compute displacements
 *   add   r_base, r_disp
 *   br    r_base
 * quit:
 */
void Arm64Mir2Lir::GenSparseSwitch(MIR* mir, uint32_t table_offset,
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
  tab_rec->targets = static_cast<LIR**>(arena_->Alloc(size * sizeof(LIR*), kArenaAllocLIR));
  switch_tables_.Insert(tab_rec);

  // Get the switch value
  rl_src = LoadValue(rl_src, kCoreReg);
  RegStorage r_base = AllocTempWide();
  // Allocate key and disp temps.
  RegStorage r_key = AllocTemp();
  RegStorage r_disp = AllocTemp();
  // Materialize a pointer to the switch table
  NewLIR3(kA64Adr2xd, r_base.GetReg(), 0, WrapPointer(tab_rec));
  // Set up r_idx
  RegStorage r_idx = AllocTemp();
  LoadConstant(r_idx, size);

  // Entry of loop.
  LIR* loop_entry = NewLIR0(kPseudoTargetLabel);
  LIR* branch_out = NewLIR2(kA64Cbz2rt, r_idx.GetReg(), 0);

  // Load next key/disp.
  NewLIR4(kA64LdpPost4rrXD, r_key.GetReg(), r_disp.GetReg(), r_base.GetReg(), 2);
  OpRegRegImm(kOpSub, r_idx, r_idx, 1);

  // Go to next case, if key does not match.
  OpRegReg(kOpCmp, r_key, rl_src.reg);
  OpCondBranch(kCondNe, loop_entry);

  // Key does match: branch to case label.
  LIR* switch_label = NewLIR3(kA64Adr2xd, r_base.GetReg(), 0, -1);
  tab_rec->anchor = switch_label;

  // Add displacement to base branch address and go!
  // TODO(Arm64): generate "add x1, x1, w3, sxtw" rather than "add x1, x1, x3"?
  OpRegRegRegShift(kOpAdd, r_base, r_base, As64BitReg(r_disp), ENCODE_NO_SHIFT);
  NewLIR1(kA64Br1x, r_base.GetReg());

  // Loop exit label.
  LIR* loop_exit = NewLIR0(kPseudoTargetLabel);
  branch_out->target = loop_exit;
}


void Arm64Mir2Lir::GenPackedSwitch(MIR* mir, uint32_t table_offset,
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
  RegStorage table_base = AllocTempWide();
  // Materialize a pointer to the switch table
  NewLIR3(kA64Adr2xd, table_base.GetReg(), 0, WrapPointer(tab_rec));
  int low_key = s4FromSwitchData(&table[2]);
  RegStorage key_reg;
  // Remove the bias, if necessary
  if (low_key == 0) {
    key_reg = rl_src.reg;
  } else {
    key_reg = AllocTemp();
    OpRegRegImm(kOpSub, key_reg, rl_src.reg, low_key);
  }
  // Bounds check - if < 0 or >= size continue following switch
  OpRegImm(kOpCmp, key_reg, size - 1);
  LIR* branch_over = OpCondBranch(kCondHi, NULL);

  // Load the displacement from the switch table
  RegStorage disp_reg = AllocTemp();
  // TODO(Arm64): generate "ldr w3, [x1,w2,sxtw #2]" rather than "ldr w3, [x1,x2,lsl #2]"?
  LoadBaseIndexed(table_base, As64BitReg(key_reg), As64BitReg(disp_reg), 2, k32);

  // Get base branch address.
  RegStorage branch_reg = AllocTempWide();
  LIR* switch_label = NewLIR3(kA64Adr2xd, branch_reg.GetReg(), 0, -1);
  tab_rec->anchor = switch_label;

  // Add displacement to base branch address and go!
  // TODO(Arm64): generate "add x4, x4, w3, sxtw" rather than "add x4, x4, x3"?
  OpRegRegRegShift(kOpAdd, branch_reg, branch_reg, As64BitReg(disp_reg), ENCODE_NO_SHIFT);
  NewLIR1(kA64Br1x, branch_reg.GetReg());

  // branch_over target here
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
void Arm64Mir2Lir::GenFillArrayData(uint32_t table_offset, RegLocation rl_src) {
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
  LoadValueDirectFixed(rl_src, rs_x0);
  LoadWordDisp(rs_rA64_SELF, QUICK_ENTRYPOINT_OFFSET(8, pHandleFillArrayData).Int32Value(),
               rs_rA64_LR);
  // Materialize a pointer to the fill data image
  NewLIR3(kA64Adr2xd, rx1, 0, WrapPointer(tab_rec));
  ClobberCallerSave();
  LIR* call_inst = OpReg(kOpBlx, rs_rA64_LR);
  MarkSafepointPC(call_inst);
}

/*
 * Handle unlocked -> thin locked transition inline or else call out to quick entrypoint. For more
 * details see monitor.cc.
 */
void Arm64Mir2Lir::GenMonitorEnter(int opt_flags, RegLocation rl_src) {
  // x0/w0 = object
  // w1    = thin lock thread id
  // x2    = address of lock word
  // w3    = lock word / store failure
  // TUNING: How much performance we get when we inline this?
  // Since we've already flush all register.
  FlushAllRegs();
  LoadValueDirectFixed(rl_src, rs_w0);
  LockCallTemps();  // Prepare for explicit register usage
  LIR* null_check_branch = nullptr;
  if ((opt_flags & MIR_IGNORE_NULL_CHECK) && !(cu_->disable_opt & (1 << kNullCheckElimination))) {
    null_check_branch = nullptr;  // No null check.
  } else {
    // If the null-check fails its handled by the slow-path to reduce exception related meta-data.
    if (Runtime::Current()->ExplicitNullChecks()) {
      null_check_branch = OpCmpImmBranch(kCondEq, rs_x0, 0, NULL);
    }
  }
  Load32Disp(rs_rA64_SELF, Thread::ThinLockIdOffset<8>().Int32Value(), rs_w1);
  OpRegRegImm(kOpAdd, rs_x2, rs_x0, mirror::Object::MonitorOffset().Int32Value());
  NewLIR2(kA64Ldxr2rX, rw3, rx2);
  MarkPossibleNullPointerException(opt_flags);
  LIR* not_unlocked_branch = OpCmpImmBranch(kCondNe, rs_x1, 0, NULL);
  NewLIR3(kA64Stxr3wrX, rw3, rw1, rx2);
  LIR* lock_success_branch = OpCmpImmBranch(kCondEq, rs_x1, 0, NULL);

  LIR* slow_path_target = NewLIR0(kPseudoTargetLabel);
  not_unlocked_branch->target = slow_path_target;
  if (null_check_branch != nullptr) {
    null_check_branch->target = slow_path_target;
  }
  // TODO: move to a slow path.
  // Go expensive route - artLockObjectFromCode(obj);
  LoadWordDisp(rs_rA64_SELF, QUICK_ENTRYPOINT_OFFSET(8, pLockObject).Int32Value(), rs_rA64_LR);
  ClobberCallerSave();
  LIR* call_inst = OpReg(kOpBlx, rs_rA64_LR);
  MarkSafepointPC(call_inst);

  LIR* success_target = NewLIR0(kPseudoTargetLabel);
  lock_success_branch->target = success_target;
  GenMemBarrier(kLoadLoad);
}

/*
 * Handle thin locked -> unlocked transition inline or else call out to quick entrypoint. For more
 * details see monitor.cc. Note the code below doesn't use ldxr/stxr as the code holds the lock
 * and can only give away ownership if its suspended.
 */
void Arm64Mir2Lir::GenMonitorExit(int opt_flags, RegLocation rl_src) {
  // x0/w0 = object
  // w1    = thin lock thread id
  // w2    = lock word
  // TUNING: How much performance we get when we inline this?
  // Since we've already flush all register.
  FlushAllRegs();
  LoadValueDirectFixed(rl_src, rs_w0);  // Get obj
  LockCallTemps();  // Prepare for explicit register usage
  LIR* null_check_branch = nullptr;
  if ((opt_flags & MIR_IGNORE_NULL_CHECK) && !(cu_->disable_opt & (1 << kNullCheckElimination))) {
    null_check_branch = nullptr;  // No null check.
  } else {
    // If the null-check fails its handled by the slow-path to reduce exception related meta-data.
    if (Runtime::Current()->ExplicitNullChecks()) {
      null_check_branch = OpCmpImmBranch(kCondEq, rs_x0, 0, NULL);
    }
  }
  Load32Disp(rs_rA64_SELF, Thread::ThinLockIdOffset<8>().Int32Value(), rs_w1);
  Load32Disp(rs_x0, mirror::Object::MonitorOffset().Int32Value(), rs_w2);
  MarkPossibleNullPointerException(opt_flags);
  LIR* slow_unlock_branch = OpCmpBranch(kCondNe, rs_w1, rs_w2, NULL);
  GenMemBarrier(kStoreLoad);
  Store32Disp(rs_x0, mirror::Object::MonitorOffset().Int32Value(), rs_xzr);
  LIR* unlock_success_branch = OpUnconditionalBranch(NULL);

  LIR* slow_path_target = NewLIR0(kPseudoTargetLabel);
  slow_unlock_branch->target = slow_path_target;
  if (null_check_branch != nullptr) {
    null_check_branch->target = slow_path_target;
  }
  // TODO: move to a slow path.
  // Go expensive route - artUnlockObjectFromCode(obj);
  LoadWordDisp(rs_rA64_SELF, QUICK_ENTRYPOINT_OFFSET(8, pUnlockObject).Int32Value(), rs_rA64_LR);
  ClobberCallerSave();
  LIR* call_inst = OpReg(kOpBlx, rs_rA64_LR);
  MarkSafepointPC(call_inst);

  LIR* success_target = NewLIR0(kPseudoTargetLabel);
  unlock_success_branch->target = success_target;
}

void Arm64Mir2Lir::GenMoveException(RegLocation rl_dest) {
  int ex_offset = Thread::ExceptionOffset<8>().Int32Value();
  RegLocation rl_result = EvalLoc(rl_dest, kRefReg, true);
  LoadRefDisp(rs_rA64_SELF, ex_offset, rl_result.reg);
  StoreRefDisp(rs_rA64_SELF, ex_offset, rs_xzr);
  StoreValue(rl_dest, rl_result);
}

/*
 * Mark garbage collection card. Skip if the value we're storing is null.
 */
void Arm64Mir2Lir::MarkGCCard(RegStorage val_reg, RegStorage tgt_addr_reg) {
  RegStorage reg_card_base = AllocTempWide();
  RegStorage reg_card_no = AllocTemp();
  LIR* branch_over = OpCmpImmBranch(kCondEq, val_reg, 0, NULL);
  LoadWordDisp(rs_rA64_SELF, Thread::CardTableOffset<8>().Int32Value(), reg_card_base);
  OpRegRegImm(kOpLsr, reg_card_no, tgt_addr_reg, gc::accounting::CardTable::kCardShift);
  // TODO(Arm64): generate "strb wB, [xB, wC, uxtw]" rather than "strb wB, [xB, xC]"?
  StoreBaseIndexed(reg_card_base, As64BitReg(reg_card_no), As32BitReg(reg_card_base),
                   0, kUnsignedByte);
  LIR* target = NewLIR0(kPseudoTargetLabel);
  branch_over->target = target;
  FreeTemp(reg_card_base);
  FreeTemp(reg_card_no);
}

void Arm64Mir2Lir::GenEntrySequence(RegLocation* ArgLocs, RegLocation rl_method) {
  /*
   * On entry, x0 to x7 are live.  Let the register allocation
   * mechanism know so it doesn't try to use any of them when
   * expanding the frame or flushing.
   * Reserve x8 & x9 for temporaries.
   */
  LockTemp(rs_x0);
  LockTemp(rs_x1);
  LockTemp(rs_x2);
  LockTemp(rs_x3);
  LockTemp(rs_x4);
  LockTemp(rs_x5);
  LockTemp(rs_x6);
  LockTemp(rs_x7);
  LockTemp(rs_x8);
  LockTemp(rs_x9);

  /*
   * We can safely skip the stack overflow check if we're
   * a leaf *and* our frame size < fudge factor.
   */
  bool skip_overflow_check = (mir_graph_->MethodIsLeaf() &&
                              (static_cast<size_t>(frame_size_) <
                              Thread::kStackOverflowReservedBytes));

  NewLIR0(kPseudoMethodEntry);

  const bool large_frame = (static_cast<size_t>(frame_size_) > Thread::kStackOverflowReservedUsableBytes);
  const int spill_count = num_core_spills_ + num_fp_spills_;
  const int spill_size = (spill_count * kArm64PointerSize + 15) & ~0xf;  // SP 16 byte alignment.
  const int frame_size_without_spills = frame_size_ - spill_size;

  if (!skip_overflow_check) {
    if (Runtime::Current()->ExplicitStackOverflowChecks()) {
      if (!large_frame) {
        // Load stack limit
        LoadWordDisp(rs_rA64_SELF, Thread::StackEndOffset<8>().Int32Value(), rs_x9);
      }
    } else {
      // TODO(Arm64) Implement implicit checks.
      // Implicit stack overflow check.
      // Generate a load from [sp, #-framesize].  If this is in the stack
      // redzone we will get a segmentation fault.
      // Load32Disp(rs_rA64_SP, -Thread::kStackOverflowReservedBytes, rs_wzr);
      // MarkPossibleStackOverflowException();
      LOG(FATAL) << "Implicit stack overflow checks not implemented.";
    }
  }

  if (frame_size_ > 0) {
    OpRegImm64(kOpSub, rs_rA64_SP, spill_size);
  }

  /* Need to spill any FP regs? */
  if (fp_spill_mask_) {
    int spill_offset = spill_size - kArm64PointerSize*(num_fp_spills_ + num_core_spills_);
    SpillFPRegs(rs_rA64_SP, spill_offset, fp_spill_mask_);
  }

  /* Spill core callee saves. */
  if (core_spill_mask_) {
    int spill_offset = spill_size - kArm64PointerSize*num_core_spills_;
    SpillCoreRegs(rs_rA64_SP, spill_offset, core_spill_mask_);
  }

  if (!skip_overflow_check) {
    if (Runtime::Current()->ExplicitStackOverflowChecks()) {
      class StackOverflowSlowPath: public LIRSlowPath {
      public:
        StackOverflowSlowPath(Mir2Lir* m2l, LIR* branch, size_t sp_displace) :
              LIRSlowPath(m2l, m2l->GetCurrentDexPc(), branch, nullptr),
              sp_displace_(sp_displace) {
        }
        void Compile() OVERRIDE {
          m2l_->ResetRegPool();
          m2l_->ResetDefTracking();
          GenerateTargetLabel(kPseudoThrowTarget);
          // Unwinds stack.
          m2l_->OpRegImm(kOpAdd, rs_rA64_SP, sp_displace_);
          m2l_->ClobberCallerSave();
          ThreadOffset<8> func_offset = QUICK_ENTRYPOINT_OFFSET(8, pThrowStackOverflow);
          m2l_->LockTemp(rs_x8);
          m2l_->LoadWordDisp(rs_rA64_SELF, func_offset.Int32Value(), rs_x8);
          m2l_->NewLIR1(kA64Br1x, rs_x8.GetReg());
          m2l_->FreeTemp(rs_x8);
        }

      private:
        const size_t sp_displace_;
      };

      if (large_frame) {
        // Compare Expected SP against bottom of stack.
        // Branch to throw target if there is not enough room.
        OpRegRegImm(kOpSub, rs_x9, rs_rA64_SP, frame_size_without_spills);
        LoadWordDisp(rs_rA64_SELF, Thread::StackEndOffset<8>().Int32Value(), rs_x8);
        LIR* branch = OpCmpBranch(kCondUlt, rs_rA64_SP, rs_x8, nullptr);
        AddSlowPath(new(arena_)StackOverflowSlowPath(this, branch, spill_size));
        OpRegCopy(rs_rA64_SP, rs_x9);  // Establish stack after checks.
      } else {
        /*
         * If the frame is small enough we are guaranteed to have enough space that remains to
         * handle signals on the user stack.
         * Establishes stack before checks.
         */
        OpRegRegImm(kOpSub, rs_rA64_SP, rs_rA64_SP, frame_size_without_spills);
        LIR* branch = OpCmpBranch(kCondUlt, rs_rA64_SP, rs_x9, nullptr);
        AddSlowPath(new(arena_)StackOverflowSlowPath(this, branch, frame_size_));
      }
    } else {
      OpRegImm(kOpSub, rs_rA64_SP, frame_size_without_spills);
    }
  } else {
    OpRegImm(kOpSub, rs_rA64_SP, frame_size_without_spills);
  }

  FlushIns(ArgLocs, rl_method);

  FreeTemp(rs_x0);
  FreeTemp(rs_x1);
  FreeTemp(rs_x2);
  FreeTemp(rs_x3);
  FreeTemp(rs_x4);
  FreeTemp(rs_x5);
  FreeTemp(rs_x6);
  FreeTemp(rs_x7);
  FreeTemp(rs_x8);
  FreeTemp(rs_x9);
}

void Arm64Mir2Lir::GenExitSequence() {
  /*
   * In the exit path, r0/r1 are live - make sure they aren't
   * allocated by the register utilities as temps.
   */
  LockTemp(rs_x0);
  LockTemp(rs_x1);

  NewLIR0(kPseudoMethodExit);

  /* Need to restore any FP callee saves? */
  if (fp_spill_mask_) {
    int spill_offset = frame_size_ - kArm64PointerSize*(num_fp_spills_ + num_core_spills_);
    UnSpillFPRegs(rs_rA64_SP, spill_offset, fp_spill_mask_);
  }
  if (core_spill_mask_) {
    int spill_offset = frame_size_ - kArm64PointerSize*num_core_spills_;
    UnSpillCoreRegs(rs_rA64_SP, spill_offset, core_spill_mask_);
  }

  OpRegImm64(kOpAdd, rs_rA64_SP, frame_size_);
  NewLIR0(kA64Ret);
}

void Arm64Mir2Lir::GenSpecialExitSequence() {
  NewLIR0(kA64Ret);
}

}  // namespace art
