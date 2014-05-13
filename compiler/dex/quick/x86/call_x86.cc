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
#include "x86_lir.h"

namespace art {

/*
 * The sparse table in the literal pool is an array of <key,displacement>
 * pairs.
 */
void X86Mir2Lir::GenSparseSwitch(MIR* mir, DexOffset table_offset,
                                 RegLocation rl_src) {
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
void X86Mir2Lir::GenPackedSwitch(MIR* mir, DexOffset table_offset,
                                 RegLocation rl_src) {
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
    rl_method = LoadValue(rl_method, kCoreReg);
    start_of_method_reg = rl_method.reg;
    store_method_addr_used_ = true;
  } else {
    start_of_method_reg = AllocTemp();
    NewLIR1(kX86StartOfMethod, start_of_method_reg.GetReg());
  }
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
  LIR* branch_over = OpCondBranch(kCondHi, NULL);

  // Load the displacement from the switch table
  RegStorage disp_reg = AllocTemp();
  NewLIR5(kX86PcRelLoadRA, disp_reg.GetReg(), start_of_method_reg.GetReg(), keyReg.GetReg(), 2, WrapPointer(tab_rec));
  // Add displacement to start of method
  OpRegReg(kOpAdd, start_of_method_reg, disp_reg);
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
  LoadValueDirectFixed(rl_src, rs_rX86_ARG0);
  // Materialize a pointer to the fill data image
  if (base_of_code_ != nullptr) {
    // We can use the saved value.
    RegLocation rl_method = mir_graph_->GetRegLocation(base_of_code_->s_reg_low);
    LoadValueDirect(rl_method, rs_rX86_ARG2);
    store_method_addr_used_ = true;
  } else {
    NewLIR1(kX86StartOfMethod, rs_rX86_ARG2.GetReg());
  }
  NewLIR2(kX86PcRelAdr, rs_rX86_ARG1.GetReg(), WrapPointer(tab_rec));
  NewLIR2(kX86Add32RR, rs_rX86_ARG1.GetReg(), rs_rX86_ARG2.GetReg());
  CallRuntimeHelperRegReg(QUICK_ENTRYPOINT_OFFSET(4, pHandleFillArrayData), rs_rX86_ARG0,
                          rs_rX86_ARG1, true);
}

void X86Mir2Lir::GenMoveException(RegLocation rl_dest) {
  int ex_offset = Is64BitInstructionSet(cu_->instruction_set) ?
      Thread::ExceptionOffset<8>().Int32Value() :
      Thread::ExceptionOffset<4>().Int32Value();
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  NewLIR2(kX86Mov32RT, rl_result.reg.GetReg(), ex_offset);
  NewLIR2(kX86Mov32TI, ex_offset, 0);
  StoreValue(rl_dest, rl_result);
}

/*
 * Mark garbage collection card. Skip if the value we're storing is null.
 */
void X86Mir2Lir::MarkGCCard(RegStorage val_reg, RegStorage tgt_addr_reg) {
  RegStorage reg_card_base = AllocTemp();
  RegStorage reg_card_no = AllocTemp();
  LIR* branch_over = OpCmpImmBranch(kCondEq, val_reg, 0, NULL);
  int ct_offset = Is64BitInstructionSet(cu_->instruction_set) ?
      Thread::CardTableOffset<8>().Int32Value() :
      Thread::CardTableOffset<4>().Int32Value();
  NewLIR2(kX86Mov32RT, reg_card_base.GetReg(), ct_offset);
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

  /* Build frame, return address already on stack */
  // TODO: 64 bit.
  stack_decrement_ = OpRegImm(kOpSub, rs_rX86_SP, frame_size_ - 4);

  /*
   * We can safely skip the stack overflow check if we're
   * a leaf *and* our frame size < fudge factor.
   */
  const bool skip_overflow_check = (mir_graph_->MethodIsLeaf() &&
      (static_cast<size_t>(frame_size_) < Thread::kStackOverflowReservedBytes));
  NewLIR0(kPseudoMethodEntry);
  /* Spill core callee saves */
  SpillCoreRegs();
  /* NOTE: promotion of FP regs currently unsupported, thus no FP spill */
  DCHECK_EQ(num_fp_spills_, 0);
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
        if (Is64BitInstructionSet(cu_->instruction_set)) {
          m2l_->CallHelper(RegStorage::InvalidReg(), QUICK_ENTRYPOINT_OFFSET(8, pThrowStackOverflow),
                           false /* MarkSafepointPC */, false /* UseLink */);
        } else {
          m2l_->CallHelper(RegStorage::InvalidReg(), QUICK_ENTRYPOINT_OFFSET(4, pThrowStackOverflow),
                                     false /* MarkSafepointPC */, false /* UseLink */);
        }
      }

     private:
      const size_t sp_displace_;
    };
    // TODO: for large frames we should do something like:
    // spill ebp
    // lea ebp, [esp + frame_size]
    // cmp ebp, fs:[stack_end_]
    // jcc stack_overflow_exception
    // mov esp, ebp
    // in case a signal comes in that's not using an alternate signal stack and the large frame may
    // have moved us outside of the reserved area at the end of the stack.
    // cmp rX86_SP, fs:[stack_end_]; jcc throw_slowpath
    if (Is64BitInstructionSet(cu_->instruction_set)) {
      OpRegThreadMem(kOpCmp, rs_rX86_SP, Thread::StackEndOffset<8>());
    } else {
      OpRegThreadMem(kOpCmp, rs_rX86_SP, Thread::StackEndOffset<4>());
    }
    LIR* branch = OpCondBranch(kCondUlt, nullptr);
    AddSlowPath(new(arena_)StackOverflowSlowPath(this, branch,
                                                 frame_size_ -
                                                 GetInstructionSetPointerSize(cu_->instruction_set)));
  }

  FlushIns(ArgLocs, rl_method);

  if (base_of_code_ != nullptr) {
    // We have been asked to save the address of the method start for later use.
    setup_method_address_[0] = NewLIR1(kX86StartOfMethod, rs_rX86_ARG0.GetReg());
    int displacement = SRegOffset(base_of_code_->s_reg_low);
    // Native pointer - must be natural word size.
    setup_method_address_[1] = StoreWordDisp(rs_rX86_SP, displacement, rs_rX86_ARG0);
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
  /* Remove frame except for return address */
  stack_increment_ = OpRegImm(kOpAdd, rs_rX86_SP, frame_size_ - 4);
  NewLIR0(kX86Ret);
}

void X86Mir2Lir::GenSpecialExitSequence() {
  NewLIR0(kX86Ret);
}

}  // namespace art
