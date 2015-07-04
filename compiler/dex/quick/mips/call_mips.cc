/*
 * Copyright (C) 2012 The Android Open Source Project
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

/* This file contains codegen for the Mips ISA */

#include "codegen_mips.h"

#include "art_method.h"
#include "base/logging.h"
#include "dex/mir_graph.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "gc/accounting/card_table.h"
#include "mips_lir.h"
#include "mirror/object_array-inl.h"

namespace art {

bool MipsMir2Lir::GenSpecialCase(BasicBlock* bb, MIR* mir, const InlineMethod& special) {
  // TODO
  UNUSED(bb, mir, special);
  return false;
}

/*
 * The lack of pc-relative loads on Mips presents somewhat of a challenge
 * for our PIC switch table strategy.  To materialize the current location
 * we'll do a dummy JAL and reference our tables using rRA as the
 * base register.  Note that rRA will be used both as the base to
 * locate the switch table data and as the reference base for the switch
 * target offsets stored in the table.  We'll use a special pseudo-instruction
 * to represent the jal and trigger the construction of the
 * switch table offsets (which will happen after final assembly and all
 * labels are fixed).
 *
 * The test loop will look something like:
 *
 *   ori   r_end, rZERO, #table_size  ; size in bytes
 *   jal   BaseLabel         ; stores "return address" (BaseLabel) in rRA
 *   nop                     ; opportunistically fill
 * BaseLabel:
 *   addiu r_base, rRA, <table> - <BaseLabel>    ; table relative to BaseLabel
     addu  r_end, r_end, r_base                   ; end of table
 *   lw    r_val, [rSP, v_reg_off]                ; Test Value
 * loop:
 *   beq   r_base, r_end, done
 *   lw    r_key, 0(r_base)
 *   addu  r_base, 8
 *   bne   r_val, r_key, loop
 *   lw    r_disp, -4(r_base)
 *   addu  rRA, r_disp
 *   jalr  rZERO, rRA
 * done:
 *
 */
void MipsMir2Lir::GenLargeSparseSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src) {
  const uint16_t* table = mir_graph_->GetTable(mir, table_offset);
  // Add the table to the list - we'll process it later.
  SwitchTable* tab_rec =
      static_cast<SwitchTable*>(arena_->Alloc(sizeof(SwitchTable), kArenaAllocData));
  tab_rec->switch_mir = mir;
  tab_rec->table = table;
  tab_rec->vaddr = current_dalvik_offset_;
  int elements = table[1];
  switch_tables_.push_back(tab_rec);

  // The table is composed of 8-byte key/disp pairs.
  int byte_size = elements * 8;

  int size_hi = byte_size >> 16;
  int size_lo = byte_size & 0xffff;

  RegStorage r_end = AllocPtrSizeTemp();
  if (size_hi) {
    NewLIR2(kMipsLui, r_end.GetReg(), size_hi);
  }
  // Must prevent code motion for the curr pc pair.
  GenBarrier();  // Scheduling barrier
  NewLIR0(kMipsCurrPC);  // Really a jal to .+8.
  // Now, fill the branch delay slot.
  if (size_hi) {
    NewLIR3(kMipsOri, r_end.GetReg(), r_end.GetReg(), size_lo);
  } else {
    NewLIR3(kMipsOri, r_end.GetReg(), rZERO, size_lo);
  }
  GenBarrier();  // Scheduling barrier.

  // Construct BaseLabel and set up table base register.
  LIR* base_label = NewLIR0(kPseudoTargetLabel);
  // Remember base label so offsets can be computed later.
  tab_rec->anchor = base_label;
  RegStorage r_base = AllocPtrSizeTemp();
  NewLIR4(kMipsDelta, r_base.GetReg(), 0, WrapPointer(base_label), WrapPointer(tab_rec));
  OpRegRegReg(kOpAdd, r_end, r_end, r_base);

  // Grab switch test value.
  rl_src = LoadValue(rl_src, kCoreReg);

  // Test loop.
  RegStorage r_key = AllocTemp();
  LIR* loop_label = NewLIR0(kPseudoTargetLabel);
  LIR* exit_branch = OpCmpBranch(kCondEq, r_base, r_end, nullptr);
  Load32Disp(r_base, 0, r_key);
  OpRegImm(kOpAdd, r_base, 8);
  OpCmpBranch(kCondNe, rl_src.reg, r_key, loop_label);
  RegStorage r_disp = AllocTemp();
  Load32Disp(r_base, -4, r_disp);
  const RegStorage rs_ra = TargetPtrReg(kLr);
  OpRegRegReg(kOpAdd, rs_ra, rs_ra, r_disp);
  OpReg(kOpBx, rs_ra);
  // Loop exit.
  LIR* exit_label = NewLIR0(kPseudoTargetLabel);
  exit_branch->target = exit_label;
}

/*
 * Code pattern will look something like:
 *
 *   lw    r_val
 *   jal   BaseLabel         ; stores "return address" (BaseLabel) in rRA
 *   nop                     ; opportunistically fill
 *   [subiu r_val, bias]      ; Remove bias if low_val != 0
 *   bound check -> done
 *   lw    r_disp, [rRA, r_val]
 *   addu  rRA, r_disp
 *   jalr  rZERO, rRA
 * done:
 */
void MipsMir2Lir::GenLargePackedSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src) {
  const uint16_t* table = mir_graph_->GetTable(mir, table_offset);
  // Add the table to the list - we'll process it later.
  SwitchTable* tab_rec =
      static_cast<SwitchTable*>(arena_->Alloc(sizeof(SwitchTable), kArenaAllocData));
  tab_rec->switch_mir = mir;
  tab_rec->table = table;
  tab_rec->vaddr = current_dalvik_offset_;
  int size = table[1];
  switch_tables_.push_back(tab_rec);

  // Get the switch value.
  rl_src = LoadValue(rl_src, kCoreReg);

  // Prepare the bias.  If too big, handle 1st stage here.
  int low_key = s4FromSwitchData(&table[2]);
  bool large_bias = false;
  RegStorage r_key;
  if (low_key == 0) {
    r_key = rl_src.reg;
  } else if ((low_key & 0xffff) != low_key) {
    r_key = AllocTemp();
    LoadConstant(r_key, low_key);
    large_bias = true;
  } else {
    r_key = AllocTemp();
  }

  // Must prevent code motion for the curr pc pair.
  GenBarrier();
  NewLIR0(kMipsCurrPC);  // Really a jal to .+8.
  // Now, fill the branch delay slot with bias strip.
  if (low_key == 0) {
    NewLIR0(kMipsNop);
  } else {
    if (large_bias) {
      OpRegRegReg(kOpSub, r_key, rl_src.reg, r_key);
    } else {
      OpRegRegImm(kOpSub, r_key, rl_src.reg, low_key);
    }
  }
  GenBarrier();  // Scheduling barrier.

  // Construct BaseLabel and set up table base register.
  LIR* base_label = NewLIR0(kPseudoTargetLabel);
  // Remember base label so offsets can be computed later.
  tab_rec->anchor = base_label;

  // Bounds check - if < 0 or >= size continue following switch.
  LIR* branch_over = OpCmpImmBranch(kCondHi, r_key, size-1, nullptr);

  // Materialize the table base pointer.
  RegStorage r_base = AllocPtrSizeTemp();
  NewLIR4(kMipsDelta, r_base.GetReg(), 0, WrapPointer(base_label), WrapPointer(tab_rec));

  // Load the displacement from the switch table.
  RegStorage r_disp = AllocTemp();
  LoadBaseIndexed(r_base, r_key, r_disp, 2, k32);

  // Add to rRA and go.
  const RegStorage rs_ra = TargetPtrReg(kLr);
  OpRegRegReg(kOpAdd, rs_ra, rs_ra, r_disp);
  OpReg(kOpBx, rs_ra);

  // Branch_over target here.
  LIR* target = NewLIR0(kPseudoTargetLabel);
  branch_over->target = target;
}

void MipsMir2Lir::GenMoveException(RegLocation rl_dest) {
  int ex_offset = cu_->target64 ? Thread::ExceptionOffset<8>().Int32Value() :
      Thread::ExceptionOffset<4>().Int32Value();
  RegLocation rl_result = EvalLoc(rl_dest, kRefReg, true);
  RegStorage reset_reg = AllocTempRef();
  LoadRefDisp(TargetPtrReg(kSelf), ex_offset, rl_result.reg, kNotVolatile);
  LoadConstant(reset_reg, 0);
  StoreRefDisp(TargetPtrReg(kSelf), ex_offset, reset_reg, kNotVolatile);
  FreeTemp(reset_reg);
  StoreValue(rl_dest, rl_result);
}

void MipsMir2Lir::UnconditionallyMarkGCCard(RegStorage tgt_addr_reg) {
  RegStorage reg_card_base = AllocPtrSizeTemp();
  RegStorage reg_card_no = AllocPtrSizeTemp();
  if (cu_->target64) {
    // NOTE: native pointer.
    LoadWordDisp(TargetPtrReg(kSelf), Thread::CardTableOffset<8>().Int32Value(), reg_card_base);
    OpRegRegImm(kOpLsr, reg_card_no, tgt_addr_reg, gc::accounting::CardTable::kCardShift);
    StoreBaseIndexed(reg_card_base, reg_card_no, As32BitReg(reg_card_base), 0, kUnsignedByte);
  } else {
    // NOTE: native pointer.
    LoadWordDisp(TargetPtrReg(kSelf), Thread::CardTableOffset<4>().Int32Value(), reg_card_base);
    OpRegRegImm(kOpLsr, reg_card_no, tgt_addr_reg, gc::accounting::CardTable::kCardShift);
    StoreBaseIndexed(reg_card_base, reg_card_no, reg_card_base, 0, kUnsignedByte);
  }
  FreeTemp(reg_card_base);
  FreeTemp(reg_card_no);
}

static dwarf::Reg DwarfCoreReg(int num) {
  return dwarf::Reg::MipsCore(num);
}

void MipsMir2Lir::GenEntrySequence(RegLocation* ArgLocs, RegLocation rl_method) {
  DCHECK_EQ(cfi_.GetCurrentCFAOffset(), 0);
  int spill_count = num_core_spills_ + num_fp_spills_;
  /*
   * On entry, A0, A1, A2 & A3 are live. On Mips64, A4, A5, A6 & A7 are also live.
   * Let the register allocation mechanism know so it doesn't try to use any of them when
   * expanding the frame or flushing.
   */
  const RegStorage arg0 = TargetReg(kArg0);
  const RegStorage arg1 = TargetReg(kArg1);
  const RegStorage arg2 = TargetReg(kArg2);
  const RegStorage arg3 = TargetReg(kArg3);
  const RegStorage arg4 = TargetReg(kArg4);
  const RegStorage arg5 = TargetReg(kArg5);
  const RegStorage arg6 = TargetReg(kArg6);
  const RegStorage arg7 = TargetReg(kArg7);

  LockTemp(arg0);
  LockTemp(arg1);
  LockTemp(arg2);
  LockTemp(arg3);
  if (cu_->target64) {
    LockTemp(arg4);
    LockTemp(arg5);
    LockTemp(arg6);
    LockTemp(arg7);
  }

  bool skip_overflow_check;
  InstructionSet target = (cu_->target64) ? kMips64 : kMips;
  int ptr_size = cu_->target64 ? 8 : 4;

  /*
   * We can safely skip the stack overflow check if we're
   * a leaf *and* our frame size < fudge factor.
   */

  skip_overflow_check = mir_graph_->MethodIsLeaf() && !FrameNeedsStackCheck(frame_size_, target);
  RegStorage check_reg = AllocPtrSizeTemp();
  RegStorage new_sp = AllocPtrSizeTemp();
  const RegStorage rs_sp = TargetPtrReg(kSp);
  const size_t kStackOverflowReservedUsableBytes = GetStackOverflowReservedBytes(target);
  const bool large_frame = static_cast<size_t>(frame_size_) > kStackOverflowReservedUsableBytes;
  bool generate_explicit_stack_overflow_check = large_frame ||
    !cu_->compiler_driver->GetCompilerOptions().GetImplicitStackOverflowChecks();

  if (!skip_overflow_check) {
    if (generate_explicit_stack_overflow_check) {
      // Load stack limit.
      if (cu_->target64) {
        LoadWordDisp(TargetPtrReg(kSelf), Thread::StackEndOffset<8>().Int32Value(), check_reg);
      } else {
        Load32Disp(TargetPtrReg(kSelf), Thread::StackEndOffset<4>().Int32Value(), check_reg);
      }
    } else {
      // Implicit stack overflow check.
      // Generate a load from [sp, #-overflowsize].  If this is in the stack
      // redzone we will get a segmentation fault.
      Load32Disp(rs_sp, -kStackOverflowReservedUsableBytes, rs_rZERO);
      MarkPossibleStackOverflowException();
    }
  }
  // Spill core callee saves.
  SpillCoreRegs();
  // NOTE: promotion of FP regs currently unsupported, thus no FP spill.
  DCHECK_EQ(num_fp_spills_, 0);
  const int frame_sub = frame_size_ - spill_count * ptr_size;
  if (!skip_overflow_check && generate_explicit_stack_overflow_check) {
    class StackOverflowSlowPath : public LIRSlowPath {
     public:
      StackOverflowSlowPath(Mir2Lir* m2l, LIR* branch, size_t sp_displace)
          : LIRSlowPath(m2l, branch), sp_displace_(sp_displace) {
      }
      void Compile() OVERRIDE {
        m2l_->ResetRegPool();
        m2l_->ResetDefTracking();
        GenerateTargetLabel(kPseudoThrowTarget);
        // RA is offset 0 since we push in reverse order.
        m2l_->LoadWordDisp(m2l_->TargetPtrReg(kSp), 0, m2l_->TargetPtrReg(kLr));
        m2l_->OpRegImm(kOpAdd, m2l_->TargetPtrReg(kSp), sp_displace_);
        m2l_->cfi().AdjustCFAOffset(-sp_displace_);
        m2l_->ClobberCallerSave();
        RegStorage r_tgt = m2l_->CallHelperSetup(kQuickThrowStackOverflow);  // Doesn't clobber LR.
        m2l_->CallHelper(r_tgt, kQuickThrowStackOverflow, false /* MarkSafepointPC */,
                         false /* UseLink */);
        m2l_->cfi().AdjustCFAOffset(sp_displace_);
      }

     private:
      const size_t sp_displace_;
    };
    OpRegRegImm(kOpSub, new_sp, rs_sp, frame_sub);
    LIR* branch = OpCmpBranch(kCondUlt, new_sp, check_reg, nullptr);
    AddSlowPath(new(arena_)StackOverflowSlowPath(this, branch, spill_count * ptr_size));
    // TODO: avoid copy for small frame sizes.
    OpRegCopy(rs_sp, new_sp);  // Establish stack.
    cfi_.AdjustCFAOffset(frame_sub);
  } else {
    // Here if skip_overflow_check or doing implicit stack overflow check.
    // Just make room on the stack for the frame now.
    OpRegImm(kOpSub, rs_sp, frame_sub);
    cfi_.AdjustCFAOffset(frame_sub);
  }

  FlushIns(ArgLocs, rl_method);

  FreeTemp(arg0);
  FreeTemp(arg1);
  FreeTemp(arg2);
  FreeTemp(arg3);
  if (cu_->target64) {
    FreeTemp(arg4);
    FreeTemp(arg5);
    FreeTemp(arg6);
    FreeTemp(arg7);
  }
}

void MipsMir2Lir::GenExitSequence() {
  cfi_.RememberState();
  /*
   * In the exit path, rMIPS_RET0/rMIPS_RET1 are live - make sure they aren't
   * allocated by the register utilities as temps.
   */
  LockTemp(TargetPtrReg(kRet0));
  LockTemp(TargetPtrReg(kRet1));

  UnSpillCoreRegs();
  OpReg(kOpBx, TargetPtrReg(kLr));
  // The CFI should be restored for any code that follows the exit block.
  cfi_.RestoreState();
  cfi_.DefCFAOffset(frame_size_);
}

void MipsMir2Lir::GenSpecialExitSequence() {
  OpReg(kOpBx, TargetPtrReg(kLr));
}

void MipsMir2Lir::GenSpecialEntryForSuspend() {
  // Keep 16-byte stack alignment - push A0, i.e. ArtMethod*, 2 filler words and RA for mips32,
  // but A0 and RA for mips64.
  core_spill_mask_ = (1u << TargetPtrReg(kLr).GetRegNum());
  num_core_spills_ = 1u;
  fp_spill_mask_ = 0u;
  num_fp_spills_ = 0u;
  frame_size_ = 16u;
  core_vmap_table_.clear();
  fp_vmap_table_.clear();
  const RegStorage rs_sp = TargetPtrReg(kSp);
  OpRegImm(kOpSub, rs_sp, frame_size_);
  cfi_.AdjustCFAOffset(frame_size_);
  StoreWordDisp(rs_sp, frame_size_ - (cu_->target64 ? 8 : 4), TargetPtrReg(kLr));
  cfi_.RelOffset(DwarfCoreReg(rRA), frame_size_ - (cu_->target64 ? 8 : 4));
  StoreWordDisp(rs_sp, 0, TargetPtrReg(kArg0));
  // Do not generate CFI for scratch register A0.
}

void MipsMir2Lir::GenSpecialExitForSuspend() {
  // Pop the frame. Don't pop ArtMethod*, it's no longer needed.
  const RegStorage rs_sp = TargetPtrReg(kSp);
  LoadWordDisp(rs_sp, frame_size_ - (cu_->target64 ? 8 : 4), TargetPtrReg(kLr));
  cfi_.Restore(DwarfCoreReg(rRA));
  OpRegImm(kOpAdd, rs_sp, frame_size_);
  cfi_.AdjustCFAOffset(-frame_size_);
}

/*
 * Bit of a hack here - in the absence of a real scheduling pass,
 * emit the next instruction in static & direct invoke sequences.
 */
static int NextSDCallInsn(CompilationUnit* cu, CallInfo* info, int state,
                          const MethodReference& target_method, uint32_t, uintptr_t direct_code,
                          uintptr_t direct_method, InvokeType type) {
  Mir2Lir* cg = static_cast<Mir2Lir*>(cu->cg.get());
  if (info->string_init_offset != 0) {
    RegStorage arg0_ref = cg->TargetReg(kArg0, kRef);
    switch (state) {
    case 0: {  // Grab target method* from thread pointer
      cg->LoadWordDisp(cg->TargetPtrReg(kSelf), info->string_init_offset, arg0_ref);
      break;
    }
    case 1:  // Grab the code from the method*
      if (direct_code == 0) {
        int32_t offset = ArtMethod::EntryPointFromQuickCompiledCodeOffset(
            InstructionSetPointerSize(cu->instruction_set)).Int32Value();
        cg->LoadWordDisp(arg0_ref, offset, cg->TargetPtrReg(kInvokeTgt));
      }
      break;
    default:
      return -1;
    }
  } else if (direct_code != 0 && direct_method != 0) {
    switch (state) {
      case 0:  // Get the current Method* [sets kArg0]
        if (direct_code != static_cast<uintptr_t>(-1)) {
          if (cu->target64) {
            cg->LoadConstantWide(cg->TargetPtrReg(kInvokeTgt), direct_code);
          } else {
            cg->LoadConstant(cg->TargetPtrReg(kInvokeTgt), direct_code);
          }
        } else {
          cg->LoadCodeAddress(target_method, type, kInvokeTgt);
        }
        if (direct_method != static_cast<uintptr_t>(-1)) {
          if (cu->target64) {
            cg->LoadConstantWide(cg->TargetReg(kArg0, kRef), direct_method);
          } else {
            cg->LoadConstant(cg->TargetReg(kArg0, kRef), direct_method);
          }
        } else {
          cg->LoadMethodAddress(target_method, type, kArg0);
        }
        break;
      default:
        return -1;
    }
  } else {
    RegStorage arg0_ref = cg->TargetReg(kArg0, kRef);
    switch (state) {
      case 0:  // Get the current Method* [sets kArg0]
        // TUNING: we can save a reg copy if Method* has been promoted.
        cg->LoadCurrMethodDirect(arg0_ref);
        break;
      case 1:  // Get method->dex_cache_resolved_methods_
        cg->LoadRefDisp(arg0_ref,
                        ArtMethod::DexCacheResolvedMethodsOffset().Int32Value(),
                        arg0_ref,
                        kNotVolatile);
        // Set up direct code if known.
        if (direct_code != 0) {
          if (direct_code != static_cast<uintptr_t>(-1)) {
            if (cu->target64) {
              cg->LoadConstantWide(cg->TargetPtrReg(kInvokeTgt), direct_code);
            } else {
              cg->LoadConstant(cg->TargetPtrReg(kInvokeTgt), direct_code);
            }
          } else {
            CHECK_LT(target_method.dex_method_index, target_method.dex_file->NumMethodIds());
            cg->LoadCodeAddress(target_method, type, kInvokeTgt);
          }
        }
        break;
      case 2: {
        // Grab target method*
        CHECK_EQ(cu->dex_file, target_method.dex_file);
        const size_t pointer_size = GetInstructionSetPointerSize(cu->instruction_set);
        cg->LoadWordDisp(arg0_ref,
                         mirror::Array::DataOffset(pointer_size).Uint32Value() +
                         target_method.dex_method_index * pointer_size, arg0_ref);
        break;
      }
      case 3:  // Grab the code from the method*
        if (direct_code == 0) {
          int32_t offset = ArtMethod::EntryPointFromQuickCompiledCodeOffset(
              InstructionSetPointerSize(cu->instruction_set)).Int32Value();
          // Get the compiled code address [use *alt_from or kArg0, set kInvokeTgt]
          cg->LoadWordDisp(arg0_ref, offset, cg->TargetPtrReg(kInvokeTgt));
        }
        break;
      default:
        return -1;
    }
  }
  return state + 1;
}

NextCallInsn MipsMir2Lir::GetNextSDCallInsn() {
  return NextSDCallInsn;
}

LIR* MipsMir2Lir::GenCallInsn(const MirMethodLoweringInfo& method_info ATTRIBUTE_UNUSED) {
  return OpReg(kOpBlx, TargetPtrReg(kInvokeTgt));
}

}  // namespace art
