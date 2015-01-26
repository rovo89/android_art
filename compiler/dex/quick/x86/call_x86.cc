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

/* This file contains codegen for the X86 ISA */

#include "codegen_x86.h"

#include "base/logging.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "driver/compiler_driver.h"
#include "gc/accounting/card_table.h"
#include "mirror/art_method.h"
#include "mirror/object_array-inl.h"
#include "x86_lir.h"

namespace art {

/*
 * The sparse table in the literal pool is an array of <key,displacement>
 * pairs.
 */
void X86Mir2Lir::GenLargeSparseSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src) {
  GenSmallSparseSwitch(mir, table_offset, rl_src);
}

/*
 * We override InsertCaseLabel, because the first parameter represents
 * a basic block id, instead of a dex offset.
 */
LIR* X86Mir2Lir::InsertCaseLabel(DexOffset bbid, int keyVal) {
  LIR* boundary_lir = &block_label_list_[bbid];
  LIR* res = boundary_lir;
  if (cu_->verbose) {
    // Only pay the expense if we're pretty-printing.
    LIR* new_label = static_cast<LIR*>(arena_->Alloc(sizeof(LIR), kArenaAllocLIR));
    BasicBlock* bb = mir_graph_->GetBasicBlock(bbid);
    DCHECK(bb != nullptr);
    new_label->dalvik_offset = bb->start_offset;;
    new_label->opcode = kPseudoCaseLabel;
    new_label->operands[0] = keyVal;
    new_label->flags.fixup = kFixupLabel;
    DCHECK(!new_label->flags.use_def_invalid);
    new_label->u.m.def_mask = &kEncodeAll;
    InsertLIRAfter(boundary_lir, new_label);
    res = new_label;
  }
  return res;
}

void X86Mir2Lir::MarkPackedCaseLabels(Mir2Lir::SwitchTable* tab_rec) {
  const uint16_t* table = tab_rec->table;
  const int32_t *targets = reinterpret_cast<const int32_t*>(&table[4]);
  int entries = table[1];
  int low_key = s4FromSwitchData(&table[2]);
  for (int i = 0; i < entries; i++) {
    // The value at targets[i] is a basic block id, instead of a dex offset.
    tab_rec->targets[i] = InsertCaseLabel(targets[i], i + low_key);
  }
}

/*
 * We convert and create a new packed switch table that stores
 * basic block ids to targets[] by examining successor blocks.
 * Note that the original packed switch table stores dex offsets to targets[].
 */
const uint16_t* X86Mir2Lir::ConvertPackedSwitchTable(MIR* mir, const uint16_t* table) {
  /*
   * The original packed switch data format:
   *  ushort ident = 0x0100  magic value
   *  ushort size            number of entries in the table
   *  int first_key          first (and lowest) switch case value
   *  int targets[size]      branch targets, relative to switch opcode
   *
   * Total size is (4+size*2) 16-bit code units.
   *
   * Note that the new packed switch data format is the same as the original
   * format, except that targets[] are basic block ids.
   *
   */
  BasicBlock* bb = mir_graph_->GetBasicBlock(mir->bb);
  DCHECK(bb != nullptr);
  // Get the number of entries.
  int entries = table[1];
  const int32_t* as_int32 = reinterpret_cast<const int32_t*>(&table[2]);
  int32_t starting_key = as_int32[0];
  // Create a new table.
  int size = sizeof(uint16_t) * (4 + entries * 2);
  uint16_t* new_table = reinterpret_cast<uint16_t*>(arena_->Alloc(size, kArenaAllocMisc));
  // Copy ident, size, and first_key to the new table.
  memcpy(new_table, table, sizeof(uint16_t) * 4);
  // Get the new targets.
  int32_t* new_targets = reinterpret_cast<int32_t*>(&new_table[4]);
  // Find out targets for each entry.
  int i = 0;
  for (SuccessorBlockInfo* successor_block_info : bb->successor_blocks) {
    DCHECK_EQ(starting_key + i, successor_block_info->key);
    // Save target basic block id.
    new_targets[i++] = successor_block_info->block;
  }
  DCHECK_EQ(i, entries);
  return new_table;
}

/*
 * Code pattern will look something like:
 *
 * mov  r_val, ..
 * call 0
 * pop  r_start_of_method
 * sub  r_start_of_method, ..
 * mov  r_key_reg, r_val
 * sub  r_key_reg, low_key
 * cmp  r_key_reg, size-1  ; bound check
 * ja   done
 * mov  r_disp, [r_start_of_method + r_key_reg * 4 + table_offset]
 * add  r_start_of_method, r_disp
 * jmp  r_start_of_method
 * done:
 */
void X86Mir2Lir::GenLargePackedSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src) {
  const uint16_t* old_table = mir_graph_->GetTable(mir, table_offset);
  const uint16_t* table = ConvertPackedSwitchTable(mir, old_table);
  // Add the table to the list - we'll process it later
  SwitchTable* tab_rec =
      static_cast<SwitchTable*>(arena_->Alloc(sizeof(SwitchTable), kArenaAllocData));
  tab_rec->table = table;
  tab_rec->vaddr = current_dalvik_offset_;
  int size = table[1];
  tab_rec->targets = static_cast<LIR**>(arena_->Alloc(size * sizeof(LIR*),
                                                      kArenaAllocLIR));
  switch_tables_.push_back(tab_rec);

  // Get the switch value
  rl_src = LoadValue(rl_src, kCoreReg);

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
  OpRegImm(kOpCmp, keyReg, size - 1);
  LIR* branch_over = OpCondBranch(kCondHi, NULL);

  RegStorage addr_for_jump;
  if (cu_->target64) {
    RegStorage table_base = AllocTempWide();
    // Load the address of the table into table_base.
    LIR* lea = RawLIR(current_dalvik_offset_, kX86Lea64RM, table_base.GetReg(), kRIPReg,
                      256, 0, WrapPointer(tab_rec));
    lea->flags.fixup = kFixupSwitchTable;
    AppendLIR(lea);

    // Load the offset from the table out of the table.
    addr_for_jump = AllocTempWide();
    NewLIR5(kX86MovsxdRA, addr_for_jump.GetReg(), table_base.GetReg(), keyReg.GetReg(), 2, 0);

    // Add the offset from the table to the table base.
    OpRegReg(kOpAdd, addr_for_jump, table_base);
  } else {
    // Materialize a pointer to the switch table.
    RegStorage start_of_method_reg;
    if (base_of_code_ != nullptr) {
      // We can use the saved value.
      RegLocation rl_method = mir_graph_->GetRegLocation(base_of_code_->s_reg_low);
      rl_method = LoadValue(rl_method, kCoreReg);
      start_of_method_reg = rl_method.reg;
      store_method_addr_used_ = true;
    } else {
      start_of_method_reg = AllocTempRef();
      NewLIR1(kX86StartOfMethod, start_of_method_reg.GetReg());
    }
    // Load the displacement from the switch table.
    addr_for_jump = AllocTemp();
    NewLIR5(kX86PcRelLoadRA, addr_for_jump.GetReg(), start_of_method_reg.GetReg(), keyReg.GetReg(),
            2, WrapPointer(tab_rec));
    // Add displacement to start of method.
    OpRegReg(kOpAdd, addr_for_jump, start_of_method_reg);
  }

  // ..and go!
  tab_rec->anchor = NewLIR1(kX86JmpR, addr_for_jump.GetReg());

  /* branch_over target here */
  LIR* target = NewLIR0(kPseudoTargetLabel);
  branch_over->target = target;
}

void X86Mir2Lir::GenMoveException(RegLocation rl_dest) {
  int ex_offset = cu_->target64 ?
      Thread::ExceptionOffset<8>().Int32Value() :
      Thread::ExceptionOffset<4>().Int32Value();
  RegLocation rl_result = EvalLoc(rl_dest, kRefReg, true);
  NewLIR2(cu_->target64 ? kX86Mov64RT : kX86Mov32RT, rl_result.reg.GetReg(), ex_offset);
  NewLIR2(cu_->target64 ? kX86Mov64TI : kX86Mov32TI, ex_offset, 0);
  StoreValue(rl_dest, rl_result);
}

void X86Mir2Lir::UnconditionallyMarkGCCard(RegStorage tgt_addr_reg) {
  DCHECK_EQ(tgt_addr_reg.Is64Bit(), cu_->target64);
  RegStorage reg_card_base = AllocTempRef();
  RegStorage reg_card_no = AllocTempRef();
  int ct_offset = cu_->target64 ?
      Thread::CardTableOffset<8>().Int32Value() :
      Thread::CardTableOffset<4>().Int32Value();
  NewLIR2(cu_->target64 ? kX86Mov64RT : kX86Mov32RT, reg_card_base.GetReg(), ct_offset);
  OpRegRegImm(kOpLsr, reg_card_no, tgt_addr_reg, gc::accounting::CardTable::kCardShift);
  StoreBaseIndexed(reg_card_base, reg_card_no, reg_card_base, 0, kUnsignedByte);
  FreeTemp(reg_card_base);
  FreeTemp(reg_card_no);
}

void X86Mir2Lir::GenEntrySequence(RegLocation* ArgLocs, RegLocation rl_method) {
  /*
   * On entry, rX86_ARG0, rX86_ARG1, rX86_ARG2 are live.  Let the register
   * allocation mechanism know so it doesn't try to use any of them when
   * expanding the frame or flushing.  This leaves the utility
   * code with no spare temps.
   */
  const RegStorage arg0 = TargetReg32(kArg0);
  const RegStorage arg1 = TargetReg32(kArg1);
  const RegStorage arg2 = TargetReg32(kArg2);
  LockTemp(arg0);
  LockTemp(arg1);
  LockTemp(arg2);

  /*
   * We can safely skip the stack overflow check if we're
   * a leaf *and* our frame size < fudge factor.
   */
  const InstructionSet isa =  cu_->target64 ? kX86_64 : kX86;
  bool skip_overflow_check = mir_graph_->MethodIsLeaf() && !FrameNeedsStackCheck(frame_size_, isa);
  const RegStorage rs_rSP = cu_->target64 ? rs_rX86_SP_64 : rs_rX86_SP_32;

  // If we doing an implicit stack overflow check, perform the load immediately
  // before the stack pointer is decremented and anything is saved.
  if (!skip_overflow_check &&
      cu_->compiler_driver->GetCompilerOptions().GetImplicitStackOverflowChecks()) {
    // Implicit stack overflow check.
    // test eax,[esp + -overflow]
    int overflow = GetStackOverflowReservedBytes(isa);
    NewLIR3(kX86Test32RM, rs_rAX.GetReg(), rs_rSP.GetReg(), -overflow);
    MarkPossibleStackOverflowException();
  }

  /* Build frame, return address already on stack */
  stack_decrement_ = OpRegImm(kOpSub, rs_rSP, frame_size_ -
                              GetInstructionSetPointerSize(cu_->instruction_set));

  NewLIR0(kPseudoMethodEntry);
  /* Spill core callee saves */
  SpillCoreRegs();
  SpillFPRegs();
  if (!skip_overflow_check) {
    class StackOverflowSlowPath : public LIRSlowPath {
     public:
      StackOverflowSlowPath(Mir2Lir* m2l, LIR* branch, size_t sp_displace)
          : LIRSlowPath(m2l, m2l->GetCurrentDexPc(), branch, nullptr), sp_displace_(sp_displace) {
      }
      void Compile() OVERRIDE {
        m2l_->ResetRegPool();
        m2l_->ResetDefTracking();
        GenerateTargetLabel(kPseudoThrowTarget);
        const RegStorage local_rs_rSP = cu_->target64 ? rs_rX86_SP_64 : rs_rX86_SP_32;
        m2l_->OpRegImm(kOpAdd, local_rs_rSP, sp_displace_);
        m2l_->ClobberCallerSave();
        // Assumes codegen and target are in thumb2 mode.
        m2l_->CallHelper(RegStorage::InvalidReg(), kQuickThrowStackOverflow,
                         false /* MarkSafepointPC */, false /* UseLink */);
      }

     private:
      const size_t sp_displace_;
    };
    if (!cu_->compiler_driver->GetCompilerOptions().GetImplicitStackOverflowChecks()) {
      // TODO: for large frames we should do something like:
      // spill ebp
      // lea ebp, [esp + frame_size]
      // cmp ebp, fs:[stack_end_]
      // jcc stack_overflow_exception
      // mov esp, ebp
      // in case a signal comes in that's not using an alternate signal stack and the large frame
      // may have moved us outside of the reserved area at the end of the stack.
      // cmp rs_rX86_SP, fs:[stack_end_]; jcc throw_slowpath
      if (cu_->target64) {
        OpRegThreadMem(kOpCmp, rs_rX86_SP_64, Thread::StackEndOffset<8>());
      } else {
        OpRegThreadMem(kOpCmp, rs_rX86_SP_32, Thread::StackEndOffset<4>());
      }
      LIR* branch = OpCondBranch(kCondUlt, nullptr);
      AddSlowPath(
        new(arena_)StackOverflowSlowPath(this, branch,
                                         frame_size_ -
                                         GetInstructionSetPointerSize(cu_->instruction_set)));
    }
  }

  FlushIns(ArgLocs, rl_method);

  if (base_of_code_ != nullptr) {
    RegStorage method_start = TargetPtrReg(kArg0);
    // We have been asked to save the address of the method start for later use.
    setup_method_address_[0] = NewLIR1(kX86StartOfMethod, method_start.GetReg());
    int displacement = SRegOffset(base_of_code_->s_reg_low);
    // Native pointer - must be natural word size.
    setup_method_address_[1] = StoreBaseDisp(rs_rSP, displacement, method_start,
                                             cu_->target64 ? k64 : k32, kNotVolatile);
  }

  FreeTemp(arg0);
  FreeTemp(arg1);
  FreeTemp(arg2);
}

void X86Mir2Lir::GenExitSequence() {
  /*
   * In the exit path, rX86_RET0/rX86_RET1 are live - make sure they aren't
   * allocated by the register utilities as temps.
   */
  LockTemp(rs_rX86_RET0);
  LockTemp(rs_rX86_RET1);

  NewLIR0(kPseudoMethodExit);
  UnSpillCoreRegs();
  UnSpillFPRegs();
  /* Remove frame except for return address */
  const RegStorage rs_rSP = cu_->target64 ? rs_rX86_SP_64 : rs_rX86_SP_32;
  stack_increment_ = OpRegImm(kOpAdd, rs_rSP,
                              frame_size_ - GetInstructionSetPointerSize(cu_->instruction_set));
  NewLIR0(kX86Ret);
}

void X86Mir2Lir::GenSpecialExitSequence() {
  NewLIR0(kX86Ret);
}

void X86Mir2Lir::GenImplicitNullCheck(RegStorage reg, int opt_flags) {
  if (!(cu_->disable_opt & (1 << kNullCheckElimination)) && (opt_flags & MIR_IGNORE_NULL_CHECK)) {
    return;
  }
  // Implicit null pointer check.
  // test eax,[arg1+0]
  NewLIR3(kX86Test32RM, rs_rAX.GetReg(), reg.GetReg(), 0);
  MarkPossibleNullPointerException(opt_flags);
}

/*
 * Bit of a hack here - in the absence of a real scheduling pass,
 * emit the next instruction in static & direct invoke sequences.
 */
static int X86NextSDCallInsn(CompilationUnit* cu, CallInfo* info,
                             int state, const MethodReference& target_method,
                             uint32_t,
                             uintptr_t direct_code, uintptr_t direct_method,
                             InvokeType type) {
  UNUSED(info, direct_code);
  Mir2Lir* cg = static_cast<Mir2Lir*>(cu->cg.get());
  if (direct_method != 0) {
    switch (state) {
    case 0:  // Get the current Method* [sets kArg0]
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
    RegStorage arg0_ref = cg->TargetReg(kArg0, kRef);
    switch (state) {
    case 0:  // Get the current Method* [sets kArg0]
      // TUNING: we can save a reg copy if Method* has been promoted.
      cg->LoadCurrMethodDirect(arg0_ref);
      break;
    case 1:  // Get method->dex_cache_resolved_methods_
      cg->LoadRefDisp(arg0_ref,
                      mirror::ArtMethod::DexCacheResolvedMethodsOffset().Int32Value(),
                      arg0_ref,
                      kNotVolatile);
      break;
    case 2:  // Grab target method*
      CHECK_EQ(cu->dex_file, target_method.dex_file);
      cg->LoadRefDisp(arg0_ref,
                      mirror::ObjectArray<mirror::Object>::OffsetOfElement(
                          target_method.dex_method_index).Int32Value(),
                      arg0_ref,
                      kNotVolatile);
      break;
    default:
      return -1;
    }
  }
  return state + 1;
}

NextCallInsn X86Mir2Lir::GetNextSDCallInsn() {
  return X86NextSDCallInsn;
}

}  // namespace art
