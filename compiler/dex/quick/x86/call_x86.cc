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
#include "dex/quick/mir_to_lir-inl.h"
#include "gc/accounting/card_table.h"
#include "x86_lir.h"

namespace art {

/*
 * The sparse table in the literal pool is an array of <key,displacement>
 * pairs.
 */
void X86Mir2Lir::GenLargeSparseSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src) {
  const uint16_t* table = cu_->insns + current_dalvik_offset_ + table_offset;
  if (cu_->verbose) {
    DumpSparseSwitchTable(table);
  }
  int entries = table[1];
  const int32_t* keys = reinterpret_cast<const int32_t*>(&table[2]);
  const int32_t* targets = &keys[entries];
  rl_src = LoadValue(rl_src, kCoreReg);
  for (int i = 0; i < entries; i++) {
    int key = keys[i];
    BasicBlock* case_block =
        mir_graph_->FindBlock(current_dalvik_offset_ + targets[i]);
    OpCmpImmBranch(kCondEq, rl_src.reg, key, &block_label_list_[case_block->id]);
  }
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
  const uint16_t* table = cu_->insns + current_dalvik_offset_ + table_offset;
  if (cu_->verbose) {
    DumpPackedSwitchTable(table);
  }
  // Add the table to the list - we'll process it later
  SwitchTable* tab_rec =
      static_cast<SwitchTable*>(arena_->Alloc(sizeof(SwitchTable), kArenaAllocData));
  tab_rec->table = table;
  tab_rec->vaddr = current_dalvik_offset_;
  int size = table[1];
  tab_rec->targets = static_cast<LIR**>(arena_->Alloc(size * sizeof(LIR*),
                                                      kArenaAllocLIR));
  switch_tables_.Insert(tab_rec);

  // Get the switch value
  rl_src = LoadValue(rl_src, kCoreReg);
  // NewLIR0(kX86Bkpt);

  // Materialize a pointer to the switch table
  RegStorage start_of_method_reg;
  if (base_of_code_ != nullptr) {
    // We can use the saved value.
    RegLocation rl_method = mir_graph_->GetRegLocation(base_of_code_->s_reg_low);
    if (rl_method.wide) {
      rl_method = LoadValueWide(rl_method, kCoreReg);
    } else {
      rl_method = LoadValue(rl_method, kCoreReg);
    }
    start_of_method_reg = rl_method.reg;
    store_method_addr_used_ = true;
  } else {
    start_of_method_reg = AllocTempRef();
    NewLIR1(kX86StartOfMethod, start_of_method_reg.GetReg());
  }
  DCHECK_EQ(start_of_method_reg.Is64Bit(), cu_->target64);
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

  // Load the displacement from the switch table
  RegStorage disp_reg = AllocTemp();
  NewLIR5(kX86PcRelLoadRA, disp_reg.GetReg(), start_of_method_reg.GetReg(), keyReg.GetReg(),
          2, WrapPointer(tab_rec));
  // Add displacement to start of method
  OpRegReg(kOpAdd, start_of_method_reg, cu_->target64 ? As64BitReg(disp_reg) : disp_reg);
  // ..and go!
  LIR* switch_branch = NewLIR1(kX86JmpR, start_of_method_reg.GetReg());
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
void X86Mir2Lir::GenFillArrayData(DexOffset table_offset, RegLocation rl_src) {
  const uint16_t* table = cu_->insns + current_dalvik_offset_ + table_offset;
  // Add the table to the list - we'll process it later
  FillArrayData* tab_rec =
      static_cast<FillArrayData*>(arena_->Alloc(sizeof(FillArrayData), kArenaAllocData));
  tab_rec->table = table;
  tab_rec->vaddr = current_dalvik_offset_;
  uint16_t width = tab_rec->table[1];
  uint32_t size = tab_rec->table[2] | ((static_cast<uint32_t>(tab_rec->table[3])) << 16);
  tab_rec->size = (size * width) + 8;

  fill_array_data_.Insert(tab_rec);

  // Making a call - use explicit registers
  FlushAllRegs();   /* Everything to home location */
  RegStorage array_ptr = TargetReg(kArg0, kRef);
  RegStorage payload = TargetPtrReg(kArg1);
  RegStorage method_start = TargetPtrReg(kArg2);

  LoadValueDirectFixed(rl_src, array_ptr);
  // Materialize a pointer to the fill data image
  if (base_of_code_ != nullptr) {
    // We can use the saved value.
    RegLocation rl_method = mir_graph_->GetRegLocation(base_of_code_->s_reg_low);
    if (rl_method.wide) {
      LoadValueDirectWide(rl_method, method_start);
    } else {
      LoadValueDirect(rl_method, method_start);
    }
    store_method_addr_used_ = true;
  } else {
    NewLIR1(kX86StartOfMethod, method_start.GetReg());
  }
  NewLIR2(kX86PcRelAdr, payload.GetReg(), WrapPointer(tab_rec));
  OpRegReg(kOpAdd, payload, method_start);
  CallRuntimeHelperRegReg(kQuickHandleFillArrayData, array_ptr, payload, true);
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

/*
 * Mark garbage collection card. Skip if the value we're storing is null.
 */
void X86Mir2Lir::MarkGCCard(RegStorage val_reg, RegStorage tgt_addr_reg) {
  DCHECK_EQ(tgt_addr_reg.Is64Bit(), cu_->target64);
  DCHECK_EQ(val_reg.Is64Bit(), cu_->target64);
  RegStorage reg_card_base = AllocTempRef();
  RegStorage reg_card_no = AllocTempRef();
  LIR* branch_over = OpCmpImmBranch(kCondEq, val_reg, 0, NULL);
  int ct_offset = cu_->target64 ?
      Thread::CardTableOffset<8>().Int32Value() :
      Thread::CardTableOffset<4>().Int32Value();
  NewLIR2(cu_->target64 ? kX86Mov64RT : kX86Mov32RT, reg_card_base.GetReg(), ct_offset);
  OpRegRegImm(kOpLsr, reg_card_no, tgt_addr_reg, gc::accounting::CardTable::kCardShift);
  StoreBaseIndexed(reg_card_base, reg_card_no, reg_card_base, 0, kUnsignedByte);
  LIR* target = NewLIR0(kPseudoTargetLabel);
  branch_over->target = target;
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
  LockTemp(rs_rX86_ARG0);
  LockTemp(rs_rX86_ARG1);
  LockTemp(rs_rX86_ARG2);

  /*
   * We can safely skip the stack overflow check if we're
   * a leaf *and* our frame size < fudge factor.
   */
  InstructionSet isa =  cu_->target64 ? kX86_64 : kX86;
  bool skip_overflow_check = mir_graph_->MethodIsLeaf() && !FrameNeedsStackCheck(frame_size_, isa);

  // If we doing an implicit stack overflow check, perform the load immediately
  // before the stack pointer is decremented and anything is saved.
  if (!skip_overflow_check &&
      cu_->compiler_driver->GetCompilerOptions().GetImplicitStackOverflowChecks()) {
    // Implicit stack overflow check.
    // test eax,[esp + -overflow]
    int overflow = GetStackOverflowReservedBytes(isa);
    NewLIR3(kX86Test32RM, rs_rAX.GetReg(), rs_rX86_SP.GetReg(), -overflow);
    MarkPossibleStackOverflowException();
  }

  /* Build frame, return address already on stack */
  stack_decrement_ = OpRegImm(kOpSub, rs_rX86_SP, frame_size_ -
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
        m2l_->OpRegImm(kOpAdd, rs_rX86_SP, sp_displace_);
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
        OpRegThreadMem(kOpCmp, rs_rX86_SP, Thread::StackEndOffset<8>());
      } else {
        OpRegThreadMem(kOpCmp, rs_rX86_SP, Thread::StackEndOffset<4>());
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
    setup_method_address_[1] = StoreBaseDisp(rs_rX86_SP, displacement, method_start,
                                             cu_->target64 ? k64 : k32, kNotVolatile);
  }

  FreeTemp(rs_rX86_ARG0);
  FreeTemp(rs_rX86_ARG1);
  FreeTemp(rs_rX86_ARG2);
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
  stack_increment_ = OpRegImm(kOpAdd, rs_rX86_SP, frame_size_ - GetInstructionSetPointerSize(cu_->instruction_set));
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

}  // namespace art
