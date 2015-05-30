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

#include "codegen_arm.h"

#include "arm_lir.h"
#include "art_method.h"
#include "base/bit_utils.h"
#include "base/logging.h"
#include "dex/mir_graph.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "gc/accounting/card_table.h"
#include "mirror/object_array-inl.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "utils/dex_cache_arrays_layout-inl.h"

namespace art {

/*
 * The sparse table in the literal pool is an array of <key,displacement>
 * pairs.  For each set, we'll load them as a pair using ldmia.
 * This means that the register number of the temp we use for the key
 * must be lower than the reg for the displacement.
 *
 * The test loop will look something like:
 *
 *   adr   r_base, <table>
 *   ldr   r_val, [rARM_SP, v_reg_off]
 *   mov   r_idx, #table_size
 * lp:
 *   ldmia r_base!, {r_key, r_disp}
 *   sub   r_idx, #1
 *   cmp   r_val, r_key
 *   ifeq
 *   add   rARM_PC, r_disp   ; This is the branch from which we compute displacement
 *   cbnz  r_idx, lp
 */
void ArmMir2Lir::GenLargeSparseSwitch(MIR* mir, uint32_t table_offset, RegLocation rl_src) {
  const uint16_t* table = mir_graph_->GetTable(mir, table_offset);
  // Add the table to the list - we'll process it later
  SwitchTable *tab_rec =
      static_cast<SwitchTable*>(arena_->Alloc(sizeof(SwitchTable), kArenaAllocData));
  tab_rec->switch_mir = mir;
  tab_rec->table = table;
  tab_rec->vaddr = current_dalvik_offset_;
  uint32_t size = table[1];
  switch_tables_.push_back(tab_rec);

  // Get the switch value
  rl_src = LoadValue(rl_src, kCoreReg);
  RegStorage r_base = AllocTemp();
  /* Allocate key and disp temps */
  RegStorage r_key = AllocTemp();
  RegStorage r_disp = AllocTemp();
  // Make sure r_key's register number is less than r_disp's number for ldmia
  if (r_key.GetReg() > r_disp.GetReg()) {
    RegStorage tmp = r_disp;
    r_disp = r_key;
    r_key = tmp;
  }
  // Materialize a pointer to the switch table
  NewLIR3(kThumb2Adr, r_base.GetReg(), 0, WrapPointer(tab_rec));
  // Set up r_idx
  RegStorage r_idx = AllocTemp();
  LoadConstant(r_idx, size);
  // Establish loop branch target
  LIR* target = NewLIR0(kPseudoTargetLabel);
  // Load next key/disp
  NewLIR2(kThumb2LdmiaWB, r_base.GetReg(), (1 << r_key.GetRegNum()) | (1 << r_disp.GetRegNum()));
  OpRegReg(kOpCmp, r_key, rl_src.reg);
  // Go if match. NOTE: No instruction set switch here - must stay Thumb2
  LIR* it = OpIT(kCondEq, "");
  LIR* switch_branch = NewLIR1(kThumb2AddPCR, r_disp.GetReg());
  OpEndIT(it);
  tab_rec->anchor = switch_branch;
  // Needs to use setflags encoding here
  OpRegRegImm(kOpSub, r_idx, r_idx, 1);  // For value == 1, this should set flags.
  DCHECK(last_lir_insn_->u.m.def_mask->HasBit(ResourceMask::kCCode));
  OpCondBranch(kCondNe, target);
}


void ArmMir2Lir::GenLargePackedSwitch(MIR* mir, uint32_t table_offset, RegLocation rl_src) {
  const uint16_t* table = mir_graph_->GetTable(mir, table_offset);
  // Add the table to the list - we'll process it later
  SwitchTable *tab_rec =
      static_cast<SwitchTable*>(arena_->Alloc(sizeof(SwitchTable),  kArenaAllocData));
  tab_rec->switch_mir = mir;
  tab_rec->table = table;
  tab_rec->vaddr = current_dalvik_offset_;
  uint32_t size = table[1];
  switch_tables_.push_back(tab_rec);

  // Get the switch value
  rl_src = LoadValue(rl_src, kCoreReg);
  RegStorage table_base = AllocTemp();
  // Materialize a pointer to the switch table
  NewLIR3(kThumb2Adr, table_base.GetReg(), 0, WrapPointer(tab_rec));
  int low_key = s4FromSwitchData(&table[2]);
  RegStorage keyReg;
  // Remove the bias, if necessary
  if (low_key == 0) {
    keyReg = rl_src.reg;
  } else {
    keyReg = AllocTemp();
    OpRegRegImm(kOpSub, keyReg, rl_src.reg, low_key);
  }
  // Bounds check - if < 0 or >= size continue following switch
  OpRegImm(kOpCmp, keyReg, size-1);
  LIR* branch_over = OpCondBranch(kCondHi, nullptr);

  // Load the displacement from the switch table
  RegStorage disp_reg = AllocTemp();
  LoadBaseIndexed(table_base, keyReg, disp_reg, 2, k32);

  // ..and go! NOTE: No instruction set switch here - must stay Thumb2
  LIR* switch_branch = NewLIR1(kThumb2AddPCR, disp_reg.GetReg());
  tab_rec->anchor = switch_branch;

  /* branch_over target here */
  LIR* target = NewLIR0(kPseudoTargetLabel);
  branch_over->target = target;
}

/*
 * Handle unlocked -> thin locked transition inline or else call out to quick entrypoint. For more
 * details see monitor.cc.
 */
void ArmMir2Lir::GenMonitorEnter(int opt_flags, RegLocation rl_src) {
  FlushAllRegs();
  // FIXME: need separate LoadValues for object references.
  LoadValueDirectFixed(rl_src, rs_r0);  // Get obj
  LockCallTemps();  // Prepare for explicit register usage
  constexpr bool kArchVariantHasGoodBranchPredictor = false;  // TODO: true if cortex-A15.
  if (kArchVariantHasGoodBranchPredictor) {
    LIR* null_check_branch = nullptr;
    if ((opt_flags & MIR_IGNORE_NULL_CHECK) && !(cu_->disable_opt & (1 << kNullCheckElimination))) {
      null_check_branch = nullptr;  // No null check.
    } else {
      // If the null-check fails its handled by the slow-path to reduce exception related meta-data.
      if (!cu_->compiler_driver->GetCompilerOptions().GetImplicitNullChecks()) {
        null_check_branch = OpCmpImmBranch(kCondEq, rs_r0, 0, nullptr);
      }
    }
    Load32Disp(rs_rARM_SELF, Thread::ThinLockIdOffset<4>().Int32Value(), rs_r2);
    NewLIR3(kThumb2Ldrex, rs_r1.GetReg(), rs_r0.GetReg(),
        mirror::Object::MonitorOffset().Int32Value() >> 2);
    MarkPossibleNullPointerException(opt_flags);
    // Zero out the read barrier bits.
    OpRegRegImm(kOpAnd, rs_r3, rs_r1, LockWord::kReadBarrierStateMaskShiftedToggled);
    LIR* not_unlocked_branch = OpCmpImmBranch(kCondNe, rs_r3, 0, nullptr);
    // r1 is zero except for the rb bits here. Copy the read barrier bits into r2.
    OpRegRegReg(kOpOr, rs_r2, rs_r2, rs_r1);
    NewLIR4(kThumb2Strex, rs_r1.GetReg(), rs_r2.GetReg(), rs_r0.GetReg(),
        mirror::Object::MonitorOffset().Int32Value() >> 2);
    LIR* lock_success_branch = OpCmpImmBranch(kCondEq, rs_r1, 0, nullptr);


    LIR* slow_path_target = NewLIR0(kPseudoTargetLabel);
    not_unlocked_branch->target = slow_path_target;
    if (null_check_branch != nullptr) {
      null_check_branch->target = slow_path_target;
    }
    // TODO: move to a slow path.
    // Go expensive route - artLockObjectFromCode(obj);
    LoadWordDisp(rs_rARM_SELF, QUICK_ENTRYPOINT_OFFSET(4, pLockObject).Int32Value(), rs_rARM_LR);
    ClobberCallerSave();
    LIR* call_inst = OpReg(kOpBlx, rs_rARM_LR);
    MarkSafepointPC(call_inst);

    LIR* success_target = NewLIR0(kPseudoTargetLabel);
    lock_success_branch->target = success_target;
    GenMemBarrier(kLoadAny);
  } else {
    // Explicit null-check as slow-path is entered using an IT.
    GenNullCheck(rs_r0, opt_flags);
    Load32Disp(rs_rARM_SELF, Thread::ThinLockIdOffset<4>().Int32Value(), rs_r2);
    NewLIR3(kThumb2Ldrex, rs_r1.GetReg(), rs_r0.GetReg(),
        mirror::Object::MonitorOffset().Int32Value() >> 2);
    MarkPossibleNullPointerException(opt_flags);
    // Zero out the read barrier bits.
    OpRegRegImm(kOpAnd, rs_r3, rs_r1, LockWord::kReadBarrierStateMaskShiftedToggled);
    // r1 will be zero except for the rb bits if the following
    // cmp-and-branch branches to eq where r2 will be used. Copy the
    // read barrier bits into r2.
    OpRegRegReg(kOpOr, rs_r2, rs_r2, rs_r1);
    OpRegImm(kOpCmp, rs_r3, 0);

    LIR* it = OpIT(kCondEq, "");
    NewLIR4(kThumb2Strex/*eq*/, rs_r1.GetReg(), rs_r2.GetReg(), rs_r0.GetReg(),
        mirror::Object::MonitorOffset().Int32Value() >> 2);
    OpEndIT(it);
    OpRegImm(kOpCmp, rs_r1, 0);
    it = OpIT(kCondNe, "T");
    // Go expensive route - artLockObjectFromCode(self, obj);
    LoadWordDisp/*ne*/(rs_rARM_SELF, QUICK_ENTRYPOINT_OFFSET(4, pLockObject).Int32Value(),
                       rs_rARM_LR);
    ClobberCallerSave();
    LIR* call_inst = OpReg(kOpBlx/*ne*/, rs_rARM_LR);
    OpEndIT(it);
    MarkSafepointPC(call_inst);
    GenMemBarrier(kLoadAny);
  }
}

/*
 * Handle thin locked -> unlocked transition inline or else call out to quick entrypoint. For more
 * details see monitor.cc. Note the code below doesn't use ldrex/strex as the code holds the lock
 * and can only give away ownership if its suspended.
 */
void ArmMir2Lir::GenMonitorExit(int opt_flags, RegLocation rl_src) {
  FlushAllRegs();
  LoadValueDirectFixed(rl_src, rs_r0);  // Get obj
  LockCallTemps();  // Prepare for explicit register usage
  LIR* null_check_branch = nullptr;
  Load32Disp(rs_rARM_SELF, Thread::ThinLockIdOffset<4>().Int32Value(), rs_r2);
  constexpr bool kArchVariantHasGoodBranchPredictor = false;  // TODO: true if cortex-A15.
  if (kArchVariantHasGoodBranchPredictor) {
    if ((opt_flags & MIR_IGNORE_NULL_CHECK) && !(cu_->disable_opt & (1 << kNullCheckElimination))) {
      null_check_branch = nullptr;  // No null check.
    } else {
      // If the null-check fails its handled by the slow-path to reduce exception related meta-data.
      if (!cu_->compiler_driver->GetCompilerOptions().GetImplicitNullChecks()) {
        null_check_branch = OpCmpImmBranch(kCondEq, rs_r0, 0, nullptr);
      }
    }
    if (!kUseReadBarrier) {
      Load32Disp(rs_r0, mirror::Object::MonitorOffset().Int32Value(), rs_r1);  // Get lock
    } else {
      NewLIR3(kThumb2Ldrex, rs_r1.GetReg(), rs_r0.GetReg(),
              mirror::Object::MonitorOffset().Int32Value() >> 2);
    }
    MarkPossibleNullPointerException(opt_flags);
    // Zero out the read barrier bits.
    OpRegRegImm(kOpAnd, rs_r3, rs_r1, LockWord::kReadBarrierStateMaskShiftedToggled);
    // Zero out except the read barrier bits.
    OpRegRegImm(kOpAnd, rs_r1, rs_r1, LockWord::kReadBarrierStateMaskShifted);
    LIR* slow_unlock_branch = OpCmpBranch(kCondNe, rs_r3, rs_r2, nullptr);
    GenMemBarrier(kAnyStore);
    LIR* unlock_success_branch;
    if (!kUseReadBarrier) {
      Store32Disp(rs_r0, mirror::Object::MonitorOffset().Int32Value(), rs_r1);
      unlock_success_branch = OpUnconditionalBranch(nullptr);
    } else {
      NewLIR4(kThumb2Strex, rs_r2.GetReg(), rs_r1.GetReg(), rs_r0.GetReg(),
              mirror::Object::MonitorOffset().Int32Value() >> 2);
      unlock_success_branch = OpCmpImmBranch(kCondEq, rs_r2, 0, nullptr);
    }
    LIR* slow_path_target = NewLIR0(kPseudoTargetLabel);
    slow_unlock_branch->target = slow_path_target;
    if (null_check_branch != nullptr) {
      null_check_branch->target = slow_path_target;
    }
    // TODO: move to a slow path.
    // Go expensive route - artUnlockObjectFromCode(obj);
    LoadWordDisp(rs_rARM_SELF, QUICK_ENTRYPOINT_OFFSET(4, pUnlockObject).Int32Value(), rs_rARM_LR);
    ClobberCallerSave();
    LIR* call_inst = OpReg(kOpBlx, rs_rARM_LR);
    MarkSafepointPC(call_inst);

    LIR* success_target = NewLIR0(kPseudoTargetLabel);
    unlock_success_branch->target = success_target;
  } else {
    // Explicit null-check as slow-path is entered using an IT.
    GenNullCheck(rs_r0, opt_flags);
    if (!kUseReadBarrier) {
      Load32Disp(rs_r0, mirror::Object::MonitorOffset().Int32Value(), rs_r1);  // Get lock
    } else {
      // If we use read barriers, we need to use atomic instructions.
      NewLIR3(kThumb2Ldrex, rs_r1.GetReg(), rs_r0.GetReg(),
              mirror::Object::MonitorOffset().Int32Value() >> 2);
    }
    MarkPossibleNullPointerException(opt_flags);
    Load32Disp(rs_rARM_SELF, Thread::ThinLockIdOffset<4>().Int32Value(), rs_r2);
    // Zero out the read barrier bits.
    OpRegRegImm(kOpAnd, rs_r3, rs_r1, LockWord::kReadBarrierStateMaskShiftedToggled);
    // Zero out except the read barrier bits.
    OpRegRegImm(kOpAnd, rs_r1, rs_r1, LockWord::kReadBarrierStateMaskShifted);
    // Is lock unheld on lock or held by us (==thread_id) on unlock?
    OpRegReg(kOpCmp, rs_r3, rs_r2);
    if (!kUseReadBarrier) {
      LIR* it = OpIT(kCondEq, "EE");
      if (GenMemBarrier(kAnyStore)) {
        UpdateIT(it, "TEE");
      }
      Store32Disp/*eq*/(rs_r0, mirror::Object::MonitorOffset().Int32Value(), rs_r1);
      // Go expensive route - UnlockObjectFromCode(obj);
      LoadWordDisp/*ne*/(rs_rARM_SELF, QUICK_ENTRYPOINT_OFFSET(4, pUnlockObject).Int32Value(),
                         rs_rARM_LR);
      ClobberCallerSave();
      LIR* call_inst = OpReg(kOpBlx/*ne*/, rs_rARM_LR);
      OpEndIT(it);
      MarkSafepointPC(call_inst);
    } else {
      // If we use read barriers, we need to use atomic instructions.
      LIR* it = OpIT(kCondEq, "");
      if (GenMemBarrier(kAnyStore)) {
        UpdateIT(it, "T");
      }
      NewLIR4/*eq*/(kThumb2Strex, rs_r2.GetReg(), rs_r1.GetReg(), rs_r0.GetReg(),
                    mirror::Object::MonitorOffset().Int32Value() >> 2);
      OpEndIT(it);
      // Since we know r2 wasn't zero before the above it instruction,
      // if r2 is zero here, we know r3 was equal to r2 and the strex
      // suceeded (we're done). Otherwise (either r3 wasn't equal to r2
      // or the strex failed), call the entrypoint.
      OpRegImm(kOpCmp, rs_r2, 0);
      LIR* it2 = OpIT(kCondNe, "T");
      // Go expensive route - UnlockObjectFromCode(obj);
      LoadWordDisp/*ne*/(rs_rARM_SELF, QUICK_ENTRYPOINT_OFFSET(4, pUnlockObject).Int32Value(),
                         rs_rARM_LR);
      ClobberCallerSave();
      LIR* call_inst = OpReg(kOpBlx/*ne*/, rs_rARM_LR);
      OpEndIT(it2);
      MarkSafepointPC(call_inst);
    }
  }
}

void ArmMir2Lir::GenMoveException(RegLocation rl_dest) {
  int ex_offset = Thread::ExceptionOffset<4>().Int32Value();
  RegLocation rl_result = EvalLoc(rl_dest, kRefReg, true);
  RegStorage reset_reg = AllocTempRef();
  LoadRefDisp(rs_rARM_SELF, ex_offset, rl_result.reg, kNotVolatile);
  LoadConstant(reset_reg, 0);
  StoreRefDisp(rs_rARM_SELF, ex_offset, reset_reg, kNotVolatile);
  FreeTemp(reset_reg);
  StoreValue(rl_dest, rl_result);
}

void ArmMir2Lir::UnconditionallyMarkGCCard(RegStorage tgt_addr_reg) {
  RegStorage reg_card_base = AllocTemp();
  RegStorage reg_card_no = AllocTemp();
  LoadWordDisp(rs_rARM_SELF, Thread::CardTableOffset<4>().Int32Value(), reg_card_base);
  OpRegRegImm(kOpLsr, reg_card_no, tgt_addr_reg, gc::accounting::CardTable::kCardShift);
  StoreBaseIndexed(reg_card_base, reg_card_no, reg_card_base, 0, kUnsignedByte);
  FreeTemp(reg_card_base);
  FreeTemp(reg_card_no);
}

static dwarf::Reg DwarfCoreReg(int num) {
  return dwarf::Reg::ArmCore(num);
}

static dwarf::Reg DwarfFpReg(int num) {
  return dwarf::Reg::ArmFp(num);
}

void ArmMir2Lir::GenEntrySequence(RegLocation* ArgLocs, RegLocation rl_method) {
  DCHECK_EQ(cfi_.GetCurrentCFAOffset(), 0);  // empty stack.
  int spill_count = num_core_spills_ + num_fp_spills_;
  /*
   * On entry, r0, r1, r2 & r3 are live.  Let the register allocation
   * mechanism know so it doesn't try to use any of them when
   * expanding the frame or flushing.  This leaves the utility
   * code with a single temp: r12.  This should be enough.
   */
  LockTemp(rs_r0);
  LockTemp(rs_r1);
  LockTemp(rs_r2);
  LockTemp(rs_r3);

  /*
   * We can safely skip the stack overflow check if we're
   * a leaf *and* our frame size < fudge factor.
   */
  bool skip_overflow_check = mir_graph_->MethodIsLeaf() && !FrameNeedsStackCheck(frame_size_, kArm);
  const size_t kStackOverflowReservedUsableBytes = GetStackOverflowReservedBytes(kArm);
  bool large_frame = (static_cast<size_t>(frame_size_) > kStackOverflowReservedUsableBytes);
  bool generate_explicit_stack_overflow_check = large_frame ||
    !cu_->compiler_driver->GetCompilerOptions().GetImplicitStackOverflowChecks();
  if (!skip_overflow_check) {
    if (generate_explicit_stack_overflow_check) {
      if (!large_frame) {
        /* Load stack limit */
        LockTemp(rs_r12);
        Load32Disp(rs_rARM_SELF, Thread::StackEndOffset<4>().Int32Value(), rs_r12);
      }
    } else {
      // Implicit stack overflow check.
      // Generate a load from [sp, #-overflowsize].  If this is in the stack
      // redzone we will get a segmentation fault.
      //
      // Caveat coder: if someone changes the kStackOverflowReservedBytes value
      // we need to make sure that it's loadable in an immediate field of
      // a sub instruction.  Otherwise we will get a temp allocation and the
      // code size will increase.
      //
      // This is done before the callee save instructions to avoid any possibility
      // of these overflowing.  This uses r12 and that's never saved in a callee
      // save.
      OpRegRegImm(kOpSub, rs_r12, rs_rARM_SP, GetStackOverflowReservedBytes(kArm));
      Load32Disp(rs_r12, 0, rs_r12);
      MarkPossibleStackOverflowException();
    }
  }
  /* Spill core callee saves */
  if (core_spill_mask_ != 0u) {
    if ((core_spill_mask_ & ~(0xffu | (1u << rs_rARM_LR.GetRegNum()))) == 0u) {
      // Spilling only low regs and/or LR, use 16-bit PUSH.
      constexpr int lr_bit_shift = rs_rARM_LR.GetRegNum() - 8;
      NewLIR1(kThumbPush,
              (core_spill_mask_ & ~(1u << rs_rARM_LR.GetRegNum())) |
              ((core_spill_mask_ & (1u << rs_rARM_LR.GetRegNum())) >> lr_bit_shift));
    } else if (IsPowerOfTwo(core_spill_mask_)) {
      // kThumb2Push cannot be used to spill a single register.
      NewLIR1(kThumb2Push1, CTZ(core_spill_mask_));
    } else {
      NewLIR1(kThumb2Push, core_spill_mask_);
    }
    cfi_.AdjustCFAOffset(num_core_spills_ * kArmPointerSize);
    cfi_.RelOffsetForMany(DwarfCoreReg(0), 0, core_spill_mask_, kArmPointerSize);
  }
  /* Need to spill any FP regs? */
  if (num_fp_spills_ != 0u) {
    /*
     * NOTE: fp spills are a little different from core spills in that
     * they are pushed as a contiguous block.  When promoting from
     * the fp set, we must allocate all singles from s16..highest-promoted
     */
    NewLIR1(kThumb2VPushCS, num_fp_spills_);
    cfi_.AdjustCFAOffset(num_fp_spills_ * kArmPointerSize);
    cfi_.RelOffsetForMany(DwarfFpReg(0), 0, fp_spill_mask_, kArmPointerSize);
  }

  const int spill_size = spill_count * 4;
  const int frame_size_without_spills = frame_size_ - spill_size;
  if (!skip_overflow_check) {
    if (generate_explicit_stack_overflow_check) {
      class StackOverflowSlowPath : public LIRSlowPath {
       public:
        StackOverflowSlowPath(Mir2Lir* m2l, LIR* branch, bool restore_lr, size_t sp_displace)
            : LIRSlowPath(m2l, branch), restore_lr_(restore_lr),
              sp_displace_(sp_displace) {
        }
        void Compile() OVERRIDE {
          m2l_->ResetRegPool();
          m2l_->ResetDefTracking();
          GenerateTargetLabel(kPseudoThrowTarget);
          if (restore_lr_) {
            m2l_->LoadWordDisp(rs_rARM_SP, sp_displace_ - 4, rs_rARM_LR);
          }
          m2l_->OpRegImm(kOpAdd, rs_rARM_SP, sp_displace_);
          m2l_->cfi().AdjustCFAOffset(-sp_displace_);
          m2l_->ClobberCallerSave();
          ThreadOffset<4> func_offset = QUICK_ENTRYPOINT_OFFSET(4, pThrowStackOverflow);
          // Load the entrypoint directly into the pc instead of doing a load + branch. Assumes
          // codegen and target are in thumb2 mode.
          // NOTE: native pointer.
          m2l_->LoadWordDisp(rs_rARM_SELF, func_offset.Int32Value(), rs_rARM_PC);
          m2l_->cfi().AdjustCFAOffset(sp_displace_);
        }

       private:
        const bool restore_lr_;
        const size_t sp_displace_;
      };
      if (large_frame) {
        // Note: may need a temp reg, and we only have r12 free at this point.
        OpRegRegImm(kOpSub, rs_rARM_LR, rs_rARM_SP, frame_size_without_spills);
        Load32Disp(rs_rARM_SELF, Thread::StackEndOffset<4>().Int32Value(), rs_r12);
        LIR* branch = OpCmpBranch(kCondUlt, rs_rARM_LR, rs_r12, nullptr);
        // Need to restore LR since we used it as a temp.
        AddSlowPath(new(arena_)StackOverflowSlowPath(this, branch, true, spill_size));
        OpRegCopy(rs_rARM_SP, rs_rARM_LR);     // Establish stack
        cfi_.AdjustCFAOffset(frame_size_without_spills);
      } else {
        /*
         * If the frame is small enough we are guaranteed to have enough space that remains to
         * handle signals on the user stack.  However, we may not have any free temp
         * registers at this point, so we'll temporarily add LR to the temp pool.
         */
        DCHECK(!GetRegInfo(rs_rARM_LR)->IsTemp());
        MarkTemp(rs_rARM_LR);
        FreeTemp(rs_rARM_LR);
        OpRegRegImm(kOpSub, rs_rARM_SP, rs_rARM_SP, frame_size_without_spills);
        cfi_.AdjustCFAOffset(frame_size_without_spills);
        Clobber(rs_rARM_LR);
        UnmarkTemp(rs_rARM_LR);
        LIR* branch = OpCmpBranch(kCondUlt, rs_rARM_SP, rs_r12, nullptr);
        AddSlowPath(new(arena_)StackOverflowSlowPath(this, branch, false, frame_size_));
      }
    } else {
      // Implicit stack overflow check has already been done.  Just make room on the
      // stack for the frame now.
      OpRegImm(kOpSub, rs_rARM_SP, frame_size_without_spills);
      cfi_.AdjustCFAOffset(frame_size_without_spills);
    }
  } else {
    OpRegImm(kOpSub, rs_rARM_SP, frame_size_without_spills);
    cfi_.AdjustCFAOffset(frame_size_without_spills);
  }

  FlushIns(ArgLocs, rl_method);

  // We can promote a PC-relative reference to dex cache arrays to a register
  // if it's used at least twice. Without investigating where we should lazily
  // load the reference, we conveniently load it after flushing inputs.
  if (dex_cache_arrays_base_reg_.Valid()) {
    OpPcRelDexCacheArrayAddr(cu_->dex_file, dex_cache_arrays_min_offset_,
                             dex_cache_arrays_base_reg_);
  }

  FreeTemp(rs_r0);
  FreeTemp(rs_r1);
  FreeTemp(rs_r2);
  FreeTemp(rs_r3);
  FreeTemp(rs_r12);
}

void ArmMir2Lir::GenExitSequence() {
  cfi_.RememberState();
  int spill_count = num_core_spills_ + num_fp_spills_;

  /*
   * In the exit path, r0/r1 are live - make sure they aren't
   * allocated by the register utilities as temps.
   */
  LockTemp(rs_r0);
  LockTemp(rs_r1);

  int adjust = frame_size_ - (spill_count * kArmPointerSize);
  OpRegImm(kOpAdd, rs_rARM_SP, adjust);
  cfi_.AdjustCFAOffset(-adjust);
  /* Need to restore any FP callee saves? */
  if (num_fp_spills_) {
    NewLIR1(kThumb2VPopCS, num_fp_spills_);
    cfi_.AdjustCFAOffset(-num_fp_spills_ * kArmPointerSize);
    cfi_.RestoreMany(DwarfFpReg(0), fp_spill_mask_);
  }
  bool unspill_LR_to_PC = (core_spill_mask_ & (1 << rs_rARM_LR.GetRegNum())) != 0;
  if (unspill_LR_to_PC) {
    core_spill_mask_ &= ~(1 << rs_rARM_LR.GetRegNum());
    core_spill_mask_ |= (1 << rs_rARM_PC.GetRegNum());
  }
  if (core_spill_mask_ != 0u) {
    if ((core_spill_mask_ & ~(0xffu | (1u << rs_rARM_PC.GetRegNum()))) == 0u) {
      // Unspilling only low regs and/or PC, use 16-bit POP.
      constexpr int pc_bit_shift = rs_rARM_PC.GetRegNum() - 8;
      NewLIR1(kThumbPop,
              (core_spill_mask_ & ~(1u << rs_rARM_PC.GetRegNum())) |
              ((core_spill_mask_ & (1u << rs_rARM_PC.GetRegNum())) >> pc_bit_shift));
    } else if (IsPowerOfTwo(core_spill_mask_)) {
      // kThumb2Pop cannot be used to unspill a single register.
      NewLIR1(kThumb2Pop1, CTZ(core_spill_mask_));
    } else {
      NewLIR1(kThumb2Pop, core_spill_mask_);
    }
    // If we pop to PC, there is no further epilogue code.
    if (!unspill_LR_to_PC) {
      cfi_.AdjustCFAOffset(-num_core_spills_ * kArmPointerSize);
      cfi_.RestoreMany(DwarfCoreReg(0), core_spill_mask_);
      DCHECK_EQ(cfi_.GetCurrentCFAOffset(), 0);  // empty stack.
    }
  }
  if (!unspill_LR_to_PC) {
    /* We didn't pop to rARM_PC, so must do a bv rARM_LR */
    NewLIR1(kThumbBx, rs_rARM_LR.GetReg());
  }
  // The CFI should be restored for any code that follows the exit block.
  cfi_.RestoreState();
  cfi_.DefCFAOffset(frame_size_);
}

void ArmMir2Lir::GenSpecialExitSequence() {
  NewLIR1(kThumbBx, rs_rARM_LR.GetReg());
}

void ArmMir2Lir::GenSpecialEntryForSuspend() {
  // Keep 16-byte stack alignment - push r0, i.e. ArtMethod*, r5, r6, lr.
  DCHECK(!IsTemp(rs_r5));
  DCHECK(!IsTemp(rs_r6));
  core_spill_mask_ =
      (1u << rs_r5.GetRegNum()) | (1u << rs_r6.GetRegNum()) | (1u << rs_rARM_LR.GetRegNum());
  num_core_spills_ = 3u;
  fp_spill_mask_ = 0u;
  num_fp_spills_ = 0u;
  frame_size_ = 16u;
  core_vmap_table_.clear();
  fp_vmap_table_.clear();
  NewLIR1(kThumbPush, (1u << rs_r0.GetRegNum()) |                 // ArtMethod*
          (core_spill_mask_ & ~(1u << rs_rARM_LR.GetRegNum())) |  // Spills other than LR.
          (1u << 8));                                             // LR encoded for 16-bit push.
  cfi_.AdjustCFAOffset(frame_size_);
  // Do not generate CFI for scratch register r0.
  cfi_.RelOffsetForMany(DwarfCoreReg(0), 4, core_spill_mask_, kArmPointerSize);
}

void ArmMir2Lir::GenSpecialExitForSuspend() {
  // Pop the frame. (ArtMethod* no longer needed but restore it anyway.)
  NewLIR1(kThumb2Pop, (1u << rs_r0.GetRegNum()) | core_spill_mask_);  // 32-bit because of LR.
  cfi_.AdjustCFAOffset(-frame_size_);
  cfi_.RestoreMany(DwarfCoreReg(0), core_spill_mask_);
}

static bool ArmUseRelativeCall(CompilationUnit* cu, const MethodReference& target_method) {
  // Emit relative calls only within a dex file due to the limited range of the BL insn.
  return cu->dex_file == target_method.dex_file;
}

/*
 * Bit of a hack here - in the absence of a real scheduling pass,
 * emit the next instruction in static & direct invoke sequences.
 */
int ArmMir2Lir::ArmNextSDCallInsn(CompilationUnit* cu, CallInfo* info,
                                  int state, const MethodReference& target_method,
                                  uint32_t unused_idx ATTRIBUTE_UNUSED,
                                  uintptr_t direct_code, uintptr_t direct_method,
                                  InvokeType type) {
  ArmMir2Lir* cg = static_cast<ArmMir2Lir*>(cu->cg.get());
  if (info->string_init_offset != 0) {
    RegStorage arg0_ref = cg->TargetReg(kArg0, kRef);
    switch (state) {
    case 0: {  // Grab target method* from thread pointer
      cg->LoadRefDisp(rs_rARM_SELF, info->string_init_offset, arg0_ref, kNotVolatile);
      break;
    }
    case 1:  // Grab the code from the method*
      if (direct_code == 0) {
        // kInvokeTgt := arg0_ref->entrypoint
        cg->LoadWordDisp(arg0_ref,
                         ArtMethod::EntryPointFromQuickCompiledCodeOffset(
                             kArmPointerSize).Int32Value(), cg->TargetPtrReg(kInvokeTgt));
      }
      break;
    default:
      return -1;
    }
  } else if (direct_code != 0 && direct_method != 0) {
    switch (state) {
    case 0:  // Get the current Method* [sets kArg0]
      if (direct_code != static_cast<uintptr_t>(-1)) {
        cg->LoadConstant(cg->TargetPtrReg(kInvokeTgt), direct_code);
      } else if (ArmUseRelativeCall(cu, target_method)) {
        // Defer to linker patch.
      } else {
        cg->LoadCodeAddress(target_method, type, kInvokeTgt);
      }
      if (direct_method != static_cast<uintptr_t>(-1)) {
        cg->LoadConstant(cg->TargetReg(kArg0, kRef), direct_method);
      } else {
        cg->LoadMethodAddress(target_method, type, kArg0);
      }
      break;
    default:
      return -1;
    }
  } else {
    bool use_pc_rel = cg->CanUseOpPcRelDexCacheArrayLoad();
    RegStorage arg0_ref = cg->TargetReg(kArg0, kRef);
    switch (state) {
    case 0:  // Get the current Method* [sets kArg0]
      // TUNING: we can save a reg copy if Method* has been promoted.
      if (!use_pc_rel) {
        cg->LoadCurrMethodDirect(arg0_ref);
        break;
      }
      ++state;
      FALLTHROUGH_INTENDED;
    case 1:  // Get method->dex_cache_resolved_methods_
      if (!use_pc_rel) {
        cg->LoadRefDisp(arg0_ref,
                        ArtMethod::DexCacheResolvedMethodsOffset().Int32Value(),
                        arg0_ref,
                        kNotVolatile);
      }
      // Set up direct code if known.
      if (direct_code != 0) {
        if (direct_code != static_cast<uintptr_t>(-1)) {
          cg->LoadConstant(cg->TargetPtrReg(kInvokeTgt), direct_code);
        } else if (ArmUseRelativeCall(cu, target_method)) {
          // Defer to linker patch.
        } else {
          CHECK_LT(target_method.dex_method_index, target_method.dex_file->NumMethodIds());
          cg->LoadCodeAddress(target_method, type, kInvokeTgt);
        }
      }
      if (!use_pc_rel || direct_code != 0) {
        break;
      }
      ++state;
      FALLTHROUGH_INTENDED;
    case 2:  // Grab target method*
      CHECK_EQ(cu->dex_file, target_method.dex_file);
      if (!use_pc_rel) {
        cg->LoadRefDisp(arg0_ref,
                        mirror::ObjectArray<mirror::Object>::OffsetOfElement(
                            target_method.dex_method_index).Int32Value(),
                        arg0_ref,
                        kNotVolatile);
      } else {
        size_t offset = cg->dex_cache_arrays_layout_.MethodOffset(target_method.dex_method_index);
        cg->OpPcRelDexCacheArrayLoad(cu->dex_file, offset, arg0_ref, false);
      }
      break;
    case 3:  // Grab the code from the method*
      if (direct_code == 0) {
        // kInvokeTgt := arg0_ref->entrypoint
        cg->LoadWordDisp(arg0_ref,
                         ArtMethod::EntryPointFromQuickCompiledCodeOffset(
                             kArmPointerSize).Int32Value(), cg->TargetPtrReg(kInvokeTgt));
      }
      break;
    default:
      return -1;
    }
  }
  return state + 1;
}

NextCallInsn ArmMir2Lir::GetNextSDCallInsn() {
  return ArmNextSDCallInsn;
}

LIR* ArmMir2Lir::CallWithLinkerFixup(const MethodReference& target_method, InvokeType type) {
  // For ARM, just generate a relative BL instruction that will be filled in at 'link time'.
  // If the target turns out to be too far, the linker will generate a thunk for dispatch.
  int target_method_idx = target_method.dex_method_index;
  const DexFile* target_dex_file = target_method.dex_file;

  // Generate the call instruction and save index, dex_file, and type.
  // NOTE: Method deduplication takes linker patches into account, so we can just pass 0
  // as a placeholder for the offset.
  LIR* call = RawLIR(current_dalvik_offset_, kThumb2Bl, 0,
                     target_method_idx, WrapPointer(target_dex_file), type);
  AppendLIR(call);
  call_method_insns_.push_back(call);
  return call;
}

LIR* ArmMir2Lir::GenCallInsn(const MirMethodLoweringInfo& method_info) {
  LIR* call_insn;
  if (method_info.FastPath() && ArmUseRelativeCall(cu_, method_info.GetTargetMethod()) &&
      (method_info.GetSharpType() == kDirect || method_info.GetSharpType() == kStatic) &&
      method_info.DirectCode() == static_cast<uintptr_t>(-1)) {
    call_insn = CallWithLinkerFixup(method_info.GetTargetMethod(), method_info.GetSharpType());
  } else {
    call_insn = OpReg(kOpBlx, TargetPtrReg(kInvokeTgt));
  }
  return call_insn;
}

}  // namespace art
