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
#include "compiler/dex/quick/codegen_util.h"
#include "compiler/dex/quick/ralloc_util.h"
#include "mips_lir.h"
#include "oat/runtime/oat_support_entrypoints.h"

namespace art {

void MipsCodegen::GenSpecialCase(CompilationUnit* cu, BasicBlock* bb, MIR* mir,
                                 SpecialCaseHandler special_case)
{
    // TODO
}

/*
 * The lack of pc-relative loads on Mips presents somewhat of a challenge
 * for our PIC switch table strategy.  To materialize the current location
 * we'll do a dummy JAL and reference our tables using r_RA as the
 * base register.  Note that r_RA will be used both as the base to
 * locate the switch table data and as the reference base for the switch
 * target offsets stored in the table.  We'll use a special pseudo-instruction
 * to represent the jal and trigger the construction of the
 * switch table offsets (which will happen after final assembly and all
 * labels are fixed).
 *
 * The test loop will look something like:
 *
 *   ori   rEnd, r_ZERO, #table_size  ; size in bytes
 *   jal   BaseLabel         ; stores "return address" (BaseLabel) in r_RA
 *   nop                     ; opportunistically fill
 * BaseLabel:
 *   addiu rBase, r_RA, <table> - <BaseLabel>  ; table relative to BaseLabel
     addu  rEnd, rEnd, rBase                   ; end of table
 *   lw    r_val, [rSP, v_reg_off]                ; Test Value
 * loop:
 *   beq   rBase, rEnd, done
 *   lw    r_key, 0(rBase)
 *   addu  rBase, 8
 *   bne   r_val, r_key, loop
 *   lw    r_disp, -4(rBase)
 *   addu  r_RA, r_disp
 *   jr    r_RA
 * done:
 *
 */
void MipsCodegen::GenSparseSwitch(CompilationUnit* cu, uint32_t table_offset, RegLocation rl_src)
{
  const uint16_t* table = cu->insns + cu->current_dalvik_offset + table_offset;
  if (cu->verbose) {
    DumpSparseSwitchTable(table);
  }
  // Add the table to the list - we'll process it later
  SwitchTable *tab_rec =
      static_cast<SwitchTable*>(NewMem(cu, sizeof(SwitchTable), true, kAllocData));
  tab_rec->table = table;
  tab_rec->vaddr = cu->current_dalvik_offset;
  int elements = table[1];
  tab_rec->targets =
      static_cast<LIR**>(NewMem(cu, elements * sizeof(LIR*), true, kAllocLIR));
  InsertGrowableList(cu, &cu->switch_tables, reinterpret_cast<uintptr_t>(tab_rec));

  // The table is composed of 8-byte key/disp pairs
  int byte_size = elements * 8;

  int size_hi = byte_size >> 16;
  int size_lo = byte_size & 0xffff;

  int rEnd = AllocTemp(cu);
  if (size_hi) {
    NewLIR2(cu, kMipsLui, rEnd, size_hi);
  }
  // Must prevent code motion for the curr pc pair
  GenBarrier(cu);  // Scheduling barrier
  NewLIR0(cu, kMipsCurrPC);  // Really a jal to .+8
  // Now, fill the branch delay slot
  if (size_hi) {
    NewLIR3(cu, kMipsOri, rEnd, rEnd, size_lo);
  } else {
    NewLIR3(cu, kMipsOri, rEnd, r_ZERO, size_lo);
  }
  GenBarrier(cu);  // Scheduling barrier

  // Construct BaseLabel and set up table base register
  LIR* base_label = NewLIR0(cu, kPseudoTargetLabel);
  // Remember base label so offsets can be computed later
  tab_rec->anchor = base_label;
  int rBase = AllocTemp(cu);
  NewLIR4(cu, kMipsDelta, rBase, 0, reinterpret_cast<uintptr_t>(base_label),
          reinterpret_cast<uintptr_t>(tab_rec));
  OpRegRegReg(cu, kOpAdd, rEnd, rEnd, rBase);

  // Grab switch test value
  rl_src = LoadValue(cu, rl_src, kCoreReg);

  // Test loop
  int r_key = AllocTemp(cu);
  LIR* loop_label = NewLIR0(cu, kPseudoTargetLabel);
  LIR* exit_branch = OpCmpBranch(cu , kCondEq, rBase, rEnd, NULL);
  LoadWordDisp(cu, rBase, 0, r_key);
  OpRegImm(cu, kOpAdd, rBase, 8);
  OpCmpBranch(cu, kCondNe, rl_src.low_reg, r_key, loop_label);
  int r_disp = AllocTemp(cu);
  LoadWordDisp(cu, rBase, -4, r_disp);
  OpRegRegReg(cu, kOpAdd, r_RA, r_RA, r_disp);
  OpReg(cu, kOpBx, r_RA);

  // Loop exit
  LIR* exit_label = NewLIR0(cu, kPseudoTargetLabel);
  exit_branch->target = exit_label;
}

/*
 * Code pattern will look something like:
 *
 *   lw    r_val
 *   jal   BaseLabel         ; stores "return address" (BaseLabel) in r_RA
 *   nop                     ; opportunistically fill
 *   [subiu r_val, bias]      ; Remove bias if low_val != 0
 *   bound check -> done
 *   lw    r_disp, [r_RA, r_val]
 *   addu  r_RA, r_disp
 *   jr    r_RA
 * done:
 */
void MipsCodegen::GenPackedSwitch(CompilationUnit* cu, uint32_t table_offset, RegLocation rl_src)
{
  const uint16_t* table = cu->insns + cu->current_dalvik_offset + table_offset;
  if (cu->verbose) {
    DumpPackedSwitchTable(table);
  }
  // Add the table to the list - we'll process it later
  SwitchTable *tab_rec =
      static_cast<SwitchTable*>(NewMem(cu, sizeof(SwitchTable), true, kAllocData));
  tab_rec->table = table;
  tab_rec->vaddr = cu->current_dalvik_offset;
  int size = table[1];
  tab_rec->targets = static_cast<LIR**>(NewMem(cu, size * sizeof(LIR*), true, kAllocLIR));
  InsertGrowableList(cu, &cu->switch_tables, reinterpret_cast<uintptr_t>(tab_rec));

  // Get the switch value
  rl_src = LoadValue(cu, rl_src, kCoreReg);

  // Prepare the bias.  If too big, handle 1st stage here
  int low_key = s4FromSwitchData(&table[2]);
  bool large_bias = false;
  int r_key;
  if (low_key == 0) {
    r_key = rl_src.low_reg;
  } else if ((low_key & 0xffff) != low_key) {
    r_key = AllocTemp(cu);
    LoadConstant(cu, r_key, low_key);
    large_bias = true;
  } else {
    r_key = AllocTemp(cu);
  }

  // Must prevent code motion for the curr pc pair
  GenBarrier(cu);
  NewLIR0(cu, kMipsCurrPC);  // Really a jal to .+8
  // Now, fill the branch delay slot with bias strip
  if (low_key == 0) {
    NewLIR0(cu, kMipsNop);
  } else {
    if (large_bias) {
      OpRegRegReg(cu, kOpSub, r_key, rl_src.low_reg, r_key);
    } else {
      OpRegRegImm(cu, kOpSub, r_key, rl_src.low_reg, low_key);
    }
  }
  GenBarrier(cu);  // Scheduling barrier

  // Construct BaseLabel and set up table base register
  LIR* base_label = NewLIR0(cu, kPseudoTargetLabel);
  // Remember base label so offsets can be computed later
  tab_rec->anchor = base_label;

  // Bounds check - if < 0 or >= size continue following switch
  LIR* branch_over = OpCmpImmBranch(cu, kCondHi, r_key, size-1, NULL);

  // Materialize the table base pointer
  int rBase = AllocTemp(cu);
  NewLIR4(cu, kMipsDelta, rBase, 0, reinterpret_cast<uintptr_t>(base_label),
          reinterpret_cast<uintptr_t>(tab_rec));

  // Load the displacement from the switch table
  int r_disp = AllocTemp(cu);
  LoadBaseIndexed(cu, rBase, r_key, r_disp, 2, kWord);

  // Add to r_AP and go
  OpRegRegReg(cu, kOpAdd, r_RA, r_RA, r_disp);
  OpReg(cu, kOpBx, r_RA);

  /* branch_over target here */
  LIR* target = NewLIR0(cu, kPseudoTargetLabel);
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
void MipsCodegen::GenFillArrayData(CompilationUnit* cu, uint32_t table_offset, RegLocation rl_src)
{
  const uint16_t* table = cu->insns + cu->current_dalvik_offset + table_offset;
  // Add the table to the list - we'll process it later
  FillArrayData *tab_rec =
      reinterpret_cast<FillArrayData*>(NewMem(cu, sizeof(FillArrayData), true, kAllocData));
  tab_rec->table = table;
  tab_rec->vaddr = cu->current_dalvik_offset;
  uint16_t width = tab_rec->table[1];
  uint32_t size = tab_rec->table[2] | ((static_cast<uint32_t>(tab_rec->table[3])) << 16);
  tab_rec->size = (size * width) + 8;

  InsertGrowableList(cu, &cu->fill_array_data, reinterpret_cast<uintptr_t>(tab_rec));

  // Making a call - use explicit registers
  FlushAllRegs(cu);   /* Everything to home location */
  LockCallTemps(cu);
  LoadValueDirectFixed(cu, rl_src, rMIPS_ARG0);

  // Must prevent code motion for the curr pc pair
  GenBarrier(cu);
  NewLIR0(cu, kMipsCurrPC);  // Really a jal to .+8
  // Now, fill the branch delay slot with the helper load
  int r_tgt = LoadHelper(cu, ENTRYPOINT_OFFSET(pHandleFillArrayDataFromCode));
  GenBarrier(cu);  // Scheduling barrier

  // Construct BaseLabel and set up table base register
  LIR* base_label = NewLIR0(cu, kPseudoTargetLabel);

  // Materialize a pointer to the fill data image
  NewLIR4(cu, kMipsDelta, rMIPS_ARG1, 0, reinterpret_cast<uintptr_t>(base_label),
          reinterpret_cast<uintptr_t>(tab_rec));

  // And go...
  ClobberCalleeSave(cu);
  LIR* call_inst = OpReg(cu, kOpBlx, r_tgt); // ( array*, fill_data* )
  MarkSafepointPC(cu, call_inst);
}

/*
 * TODO: implement fast path to short-circuit thin-lock case
 */
void MipsCodegen::GenMonitorEnter(CompilationUnit* cu, int opt_flags, RegLocation rl_src)
{
  FlushAllRegs(cu);
  LoadValueDirectFixed(cu, rl_src, rMIPS_ARG0);  // Get obj
  LockCallTemps(cu);  // Prepare for explicit register usage
  GenNullCheck(cu, rl_src.s_reg_low, rMIPS_ARG0, opt_flags);
  // Go expensive route - artLockObjectFromCode(self, obj);
  int r_tgt = LoadHelper(cu, ENTRYPOINT_OFFSET(pLockObjectFromCode));
  ClobberCalleeSave(cu);
  LIR* call_inst = OpReg(cu, kOpBlx, r_tgt);
  MarkSafepointPC(cu, call_inst);
}

/*
 * TODO: implement fast path to short-circuit thin-lock case
 */
void MipsCodegen::GenMonitorExit(CompilationUnit* cu, int opt_flags, RegLocation rl_src)
{
  FlushAllRegs(cu);
  LoadValueDirectFixed(cu, rl_src, rMIPS_ARG0);  // Get obj
  LockCallTemps(cu);  // Prepare for explicit register usage
  GenNullCheck(cu, rl_src.s_reg_low, rMIPS_ARG0, opt_flags);
  // Go expensive route - UnlockObjectFromCode(obj);
  int r_tgt = LoadHelper(cu, ENTRYPOINT_OFFSET(pUnlockObjectFromCode));
  ClobberCalleeSave(cu);
  LIR* call_inst = OpReg(cu, kOpBlx, r_tgt);
  MarkSafepointPC(cu, call_inst);
}

void MipsCodegen::GenMoveException(CompilationUnit* cu, RegLocation rl_dest)
{
  int ex_offset = Thread::ExceptionOffset().Int32Value();
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  int reset_reg = AllocTemp(cu);
  LoadWordDisp(cu, rMIPS_SELF, ex_offset, rl_result.low_reg);
  LoadConstant(cu, reset_reg, 0);
  StoreWordDisp(cu, rMIPS_SELF, ex_offset, reset_reg);
  FreeTemp(cu, reset_reg);
  StoreValue(cu, rl_dest, rl_result);
}

/*
 * Mark garbage collection card. Skip if the value we're storing is null.
 */
void MipsCodegen::MarkGCCard(CompilationUnit* cu, int val_reg, int tgt_addr_reg)
{
  int reg_card_base = AllocTemp(cu);
  int reg_card_no = AllocTemp(cu);
  LIR* branch_over = OpCmpImmBranch(cu, kCondEq, val_reg, 0, NULL);
  LoadWordDisp(cu, rMIPS_SELF, Thread::CardTableOffset().Int32Value(), reg_card_base);
  OpRegRegImm(cu, kOpLsr, reg_card_no, tgt_addr_reg, CardTable::kCardShift);
  StoreBaseIndexed(cu, reg_card_base, reg_card_no, reg_card_base, 0,
                   kUnsignedByte);
  LIR* target = NewLIR0(cu, kPseudoTargetLabel);
  branch_over->target = target;
  FreeTemp(cu, reg_card_base);
  FreeTemp(cu, reg_card_no);
}
void MipsCodegen::GenEntrySequence(CompilationUnit* cu, RegLocation* ArgLocs, RegLocation rl_method)
{
  int spill_count = cu->num_core_spills + cu->num_fp_spills;
  /*
   * On entry, rMIPS_ARG0, rMIPS_ARG1, rMIPS_ARG2 & rMIPS_ARG3 are live.  Let the register
   * allocation mechanism know so it doesn't try to use any of them when
   * expanding the frame or flushing.  This leaves the utility
   * code with a single temp: r12.  This should be enough.
   */
  LockTemp(cu, rMIPS_ARG0);
  LockTemp(cu, rMIPS_ARG1);
  LockTemp(cu, rMIPS_ARG2);
  LockTemp(cu, rMIPS_ARG3);

  /*
   * We can safely skip the stack overflow check if we're
   * a leaf *and* our frame size < fudge factor.
   */
  bool skip_overflow_check = ((cu->attrs & METHOD_IS_LEAF) &&
      (static_cast<size_t>(cu->frame_size) < Thread::kStackOverflowReservedBytes));
  NewLIR0(cu, kPseudoMethodEntry);
  int check_reg = AllocTemp(cu);
  int new_sp = AllocTemp(cu);
  if (!skip_overflow_check) {
    /* Load stack limit */
    LoadWordDisp(cu, rMIPS_SELF, Thread::StackEndOffset().Int32Value(), check_reg);
  }
  /* Spill core callee saves */
  SpillCoreRegs(cu);
  /* NOTE: promotion of FP regs currently unsupported, thus no FP spill */
  DCHECK_EQ(cu->num_fp_spills, 0);
  if (!skip_overflow_check) {
    OpRegRegImm(cu, kOpSub, new_sp, rMIPS_SP, cu->frame_size - (spill_count * 4));
    GenRegRegCheck(cu, kCondCc, new_sp, check_reg, kThrowStackOverflow);
    OpRegCopy(cu, rMIPS_SP, new_sp);     // Establish stack
  } else {
    OpRegImm(cu, kOpSub, rMIPS_SP, cu->frame_size - (spill_count * 4));
  }

  FlushIns(cu, ArgLocs, rl_method);

  FreeTemp(cu, rMIPS_ARG0);
  FreeTemp(cu, rMIPS_ARG1);
  FreeTemp(cu, rMIPS_ARG2);
  FreeTemp(cu, rMIPS_ARG3);
}

void MipsCodegen::GenExitSequence(CompilationUnit* cu)
{
  /*
   * In the exit path, rMIPS_RET0/rMIPS_RET1 are live - make sure they aren't
   * allocated by the register utilities as temps.
   */
  LockTemp(cu, rMIPS_RET0);
  LockTemp(cu, rMIPS_RET1);

  NewLIR0(cu, kPseudoMethodExit);
  UnSpillCoreRegs(cu);
  OpReg(cu, kOpBx, r_RA);
}

}  // namespace art
