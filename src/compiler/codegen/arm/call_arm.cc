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

#include "oat_compilation_unit.h"
#include "oat/runtime/oat_support_entrypoints.h"
#include "arm_lir.h"
#include "codegen_arm.h"
#include "../codegen_util.h"
#include "../ralloc_util.h"

namespace art {


/* Return the position of an ssa name within the argument list */
static int InPosition(CompilationUnit* cu, int s_reg)
{
  int v_reg = SRegToVReg(cu, s_reg);
  return v_reg - cu->num_regs;
}

/*
 * Describe an argument.  If it's already in an arg register, just leave it
 * there.  NOTE: all live arg registers must be locked prior to this call
 * to avoid having them allocated as a temp by downstream utilities.
 */
RegLocation ArmCodegen::ArgLoc(CompilationUnit* cu, RegLocation loc)
{
  int arg_num = InPosition(cu, loc.s_reg_low);
  if (loc.wide) {
    if (arg_num == 2) {
      // Bad case - half in register, half in frame.  Just punt
      loc.location = kLocInvalid;
    } else if (arg_num < 2) {
      loc.low_reg = rARM_ARG1 + arg_num;
      loc.high_reg = loc.low_reg + 1;
      loc.location = kLocPhysReg;
    } else {
      loc.location = kLocDalvikFrame;
    }
  } else {
    if (arg_num < 3) {
      loc.low_reg = rARM_ARG1 + arg_num;
      loc.location = kLocPhysReg;
    } else {
      loc.location = kLocDalvikFrame;
    }
  }
  return loc;
}

/*
 * Load an argument.  If already in a register, just return.  If in
 * the frame, we can't use the normal LoadValue() because it assumed
 * a proper frame - and we're frameless.
 */
static RegLocation LoadArg(CompilationUnit* cu, RegLocation loc)
{
  Codegen* cg = cu->cg.get();
  if (loc.location == kLocDalvikFrame) {
    int start = (InPosition(cu, loc.s_reg_low) + 1) * sizeof(uint32_t);
    loc.low_reg = AllocTemp(cu);
    cg->LoadWordDisp(cu, rARM_SP, start, loc.low_reg);
    if (loc.wide) {
      loc.high_reg = AllocTemp(cu);
      cg->LoadWordDisp(cu, rARM_SP, start + sizeof(uint32_t), loc.high_reg);
    }
    loc.location = kLocPhysReg;
  }
  return loc;
}

/* Lock any referenced arguments that arrive in registers */
static void LockLiveArgs(CompilationUnit* cu, MIR* mir)
{
  int first_in = cu->num_regs;
  const int num_arg_regs = 3;  // TODO: generalize & move to RegUtil.cc
  for (int i = 0; i < mir->ssa_rep->num_uses; i++) {
    int v_reg = SRegToVReg(cu, mir->ssa_rep->uses[i]);
    int InPosition = v_reg - first_in;
    if (InPosition < num_arg_regs) {
      LockTemp(cu, rARM_ARG1 + InPosition);
    }
  }
}

/* Find the next MIR, which may be in a following basic block */
static MIR* GetNextMir(CompilationUnit* cu, BasicBlock** p_bb, MIR* mir)
{
  BasicBlock* bb = *p_bb;
  MIR* orig_mir = mir;
  while (bb != NULL) {
    if (mir != NULL) {
      mir = mir->next;
    }
    if (mir != NULL) {
      return mir;
    } else {
      bb = bb->fall_through;
      *p_bb = bb;
      if (bb) {
         mir = bb->first_mir_insn;
         if (mir != NULL) {
           return mir;
         }
      }
    }
  }
  return orig_mir;
}

/* Used for the "verbose" listing */
//TODO:  move to common code
void ArmCodegen::GenPrintLabel(CompilationUnit *cu, MIR* mir)
{
  /* Mark the beginning of a Dalvik instruction for line tracking */
  char* inst_str = cu->verbose ?
     GetDalvikDisassembly(cu, mir) : NULL;
  MarkBoundary(cu, mir->offset, inst_str);
}

static MIR* SpecialIGet(CompilationUnit* cu, BasicBlock** bb, MIR* mir,
                        OpSize size, bool long_or_double, bool is_object)
{
  Codegen* cg = cu->cg.get();
  int field_offset;
  bool is_volatile;
  uint32_t field_idx = mir->dalvikInsn.vC;
  bool fast_path = FastInstance(cu, field_idx, field_offset, is_volatile, false);
  if (!fast_path || !(mir->optimization_flags & MIR_IGNORE_NULL_CHECK)) {
    return NULL;
  }
  RegLocation rl_obj = GetSrc(cu, mir, 0);
  LockLiveArgs(cu, mir);
  rl_obj = ArmCodegen::ArgLoc(cu, rl_obj);
  RegLocation rl_dest;
  if (long_or_double) {
    rl_dest = GetReturnWide(cu, false);
  } else {
    rl_dest = GetReturn(cu, false);
  }
  // Point of no return - no aborts after this
  ArmCodegen::GenPrintLabel(cu, mir);
  rl_obj = LoadArg(cu, rl_obj);
  cg->GenIGet(cu, field_idx, mir->optimization_flags, size, rl_dest, rl_obj,
              long_or_double, is_object);
  return GetNextMir(cu, bb, mir);
}

static MIR* SpecialIPut(CompilationUnit* cu, BasicBlock** bb, MIR* mir,
                        OpSize size, bool long_or_double, bool is_object)
{
  Codegen* cg = cu->cg.get();
  int field_offset;
  bool is_volatile;
  uint32_t field_idx = mir->dalvikInsn.vC;
  bool fast_path = FastInstance(cu, field_idx, field_offset, is_volatile, false);
  if (!fast_path || !(mir->optimization_flags & MIR_IGNORE_NULL_CHECK)) {
    return NULL;
  }
  RegLocation rl_src;
  RegLocation rl_obj;
  LockLiveArgs(cu, mir);
  if (long_or_double) {
    rl_src = GetSrcWide(cu, mir, 0);
    rl_obj = GetSrc(cu, mir, 2);
  } else {
    rl_src = GetSrc(cu, mir, 0);
    rl_obj = GetSrc(cu, mir, 1);
  }
  rl_src = ArmCodegen::ArgLoc(cu, rl_src);
  rl_obj = ArmCodegen::ArgLoc(cu, rl_obj);
  // Reject if source is split across registers & frame
  if (rl_obj.location == kLocInvalid) {
    ResetRegPool(cu);
    return NULL;
  }
  // Point of no return - no aborts after this
  ArmCodegen::GenPrintLabel(cu, mir);
  rl_obj = LoadArg(cu, rl_obj);
  rl_src = LoadArg(cu, rl_src);
  cg->GenIPut(cu, field_idx, mir->optimization_flags, size, rl_src, rl_obj,
              long_or_double, is_object);
  return GetNextMir(cu, bb, mir);
}

static MIR* SpecialIdentity(CompilationUnit* cu, MIR* mir)
{
  Codegen* cg = cu->cg.get();
  RegLocation rl_src;
  RegLocation rl_dest;
  bool wide = (mir->ssa_rep->num_uses == 2);
  if (wide) {
    rl_src = GetSrcWide(cu, mir, 0);
    rl_dest = GetReturnWide(cu, false);
  } else {
    rl_src = GetSrc(cu, mir, 0);
    rl_dest = GetReturn(cu, false);
  }
  LockLiveArgs(cu, mir);
  rl_src = ArmCodegen::ArgLoc(cu, rl_src);
  if (rl_src.location == kLocInvalid) {
    ResetRegPool(cu);
    return NULL;
  }
  // Point of no return - no aborts after this
  ArmCodegen::GenPrintLabel(cu, mir);
  rl_src = LoadArg(cu, rl_src);
  if (wide) {
    cg->StoreValueWide(cu, rl_dest, rl_src);
  } else {
    cg->StoreValue(cu, rl_dest, rl_src);
  }
  return mir;
}

/*
 * Special-case code genration for simple non-throwing leaf methods.
 */
void ArmCodegen::GenSpecialCase(CompilationUnit* cu, BasicBlock* bb, MIR* mir,
                                SpecialCaseHandler special_case)
{
   cu->current_dalvik_offset = mir->offset;
   MIR* next_mir = NULL;
   switch (special_case) {
     case kNullMethod:
       DCHECK(mir->dalvikInsn.opcode == Instruction::RETURN_VOID);
       next_mir = mir;
       break;
     case kConstFunction:
       ArmCodegen::GenPrintLabel(cu, mir);
       LoadConstant(cu, rARM_RET0, mir->dalvikInsn.vB);
       next_mir = GetNextMir(cu, &bb, mir);
       break;
     case kIGet:
       next_mir = SpecialIGet(cu, &bb, mir, kWord, false, false);
       break;
     case kIGetBoolean:
     case kIGetByte:
       next_mir = SpecialIGet(cu, &bb, mir, kUnsignedByte, false, false);
       break;
     case kIGetObject:
       next_mir = SpecialIGet(cu, &bb, mir, kWord, false, true);
       break;
     case kIGetChar:
       next_mir = SpecialIGet(cu, &bb, mir, kUnsignedHalf, false, false);
       break;
     case kIGetShort:
       next_mir = SpecialIGet(cu, &bb, mir, kSignedHalf, false, false);
       break;
     case kIGetWide:
       next_mir = SpecialIGet(cu, &bb, mir, kLong, true, false);
       break;
     case kIPut:
       next_mir = SpecialIPut(cu, &bb, mir, kWord, false, false);
       break;
     case kIPutBoolean:
     case kIPutByte:
       next_mir = SpecialIPut(cu, &bb, mir, kUnsignedByte, false, false);
       break;
     case kIPutObject:
       next_mir = SpecialIPut(cu, &bb, mir, kWord, false, true);
       break;
     case kIPutChar:
       next_mir = SpecialIPut(cu, &bb, mir, kUnsignedHalf, false, false);
       break;
     case kIPutShort:
       next_mir = SpecialIPut(cu, &bb, mir, kSignedHalf, false, false);
       break;
     case kIPutWide:
       next_mir = SpecialIPut(cu, &bb, mir, kLong, true, false);
       break;
     case kIdentity:
       next_mir = SpecialIdentity(cu, mir);
       break;
     default:
       return;
   }
   if (next_mir != NULL) {
    cu->current_dalvik_offset = next_mir->offset;
    if (special_case != kIdentity) {
      ArmCodegen::GenPrintLabel(cu, next_mir);
    }
    NewLIR1(cu, kThumbBx, rARM_LR);
    cu->core_spill_mask = 0;
    cu->num_core_spills = 0;
    cu->fp_spill_mask = 0;
    cu->num_fp_spills = 0;
    cu->frame_size = 0;
    cu->core_vmap_table.clear();
    cu->fp_vmap_table.clear();
  }
}

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
void ArmCodegen::GenSparseSwitch(CompilationUnit* cu, uint32_t table_offset, RegLocation rl_src)
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
  int size = table[1];
  tab_rec->targets = static_cast<LIR**>(NewMem(cu, size * sizeof(LIR*), true, kAllocLIR));
  InsertGrowableList(cu, &cu->switch_tables, reinterpret_cast<uintptr_t>(tab_rec));

  // Get the switch value
  rl_src = LoadValue(cu, rl_src, kCoreReg);
  int rBase = AllocTemp(cu);
  /* Allocate key and disp temps */
  int r_key = AllocTemp(cu);
  int r_disp = AllocTemp(cu);
  // Make sure r_key's register number is less than r_disp's number for ldmia
  if (r_key > r_disp) {
    int tmp = r_disp;
    r_disp = r_key;
    r_key = tmp;
  }
  // Materialize a pointer to the switch table
  NewLIR3(cu, kThumb2Adr, rBase, 0, reinterpret_cast<uintptr_t>(tab_rec));
  // Set up r_idx
  int r_idx = AllocTemp(cu);
  LoadConstant(cu, r_idx, size);
  // Establish loop branch target
  LIR* target = NewLIR0(cu, kPseudoTargetLabel);
  // Load next key/disp
  NewLIR2(cu, kThumb2LdmiaWB, rBase, (1 << r_key) | (1 << r_disp));
  OpRegReg(cu, kOpCmp, r_key, rl_src.low_reg);
  // Go if match. NOTE: No instruction set switch here - must stay Thumb2
  OpIT(cu, kCondEq, "");
  LIR* switch_branch = NewLIR1(cu, kThumb2AddPCR, r_disp);
  tab_rec->anchor = switch_branch;
  // Needs to use setflags encoding here
  NewLIR3(cu, kThumb2SubsRRI12, r_idx, r_idx, 1);
  OpCondBranch(cu, kCondNe, target);
}


void ArmCodegen::GenPackedSwitch(CompilationUnit* cu, uint32_t table_offset, RegLocation rl_src)
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
  int table_base = AllocTemp(cu);
  // Materialize a pointer to the switch table
  NewLIR3(cu, kThumb2Adr, table_base, 0, reinterpret_cast<uintptr_t>(tab_rec));
  int low_key = s4FromSwitchData(&table[2]);
  int keyReg;
  // Remove the bias, if necessary
  if (low_key == 0) {
    keyReg = rl_src.low_reg;
  } else {
    keyReg = AllocTemp(cu);
    OpRegRegImm(cu, kOpSub, keyReg, rl_src.low_reg, low_key);
  }
  // Bounds check - if < 0 or >= size continue following switch
  OpRegImm(cu, kOpCmp, keyReg, size-1);
  LIR* branch_over = OpCondBranch(cu, kCondHi, NULL);

  // Load the displacement from the switch table
  int disp_reg = AllocTemp(cu);
  LoadBaseIndexed(cu, table_base, keyReg, disp_reg, 2, kWord);

  // ..and go! NOTE: No instruction set switch here - must stay Thumb2
  LIR* switch_branch = NewLIR1(cu, kThumb2AddPCR, disp_reg);
  tab_rec->anchor = switch_branch;

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
void ArmCodegen::GenFillArrayData(CompilationUnit* cu, uint32_t table_offset, RegLocation rl_src)
{
  const uint16_t* table = cu->insns + cu->current_dalvik_offset + table_offset;
  // Add the table to the list - we'll process it later
  FillArrayData *tab_rec =
      static_cast<FillArrayData*>(NewMem(cu, sizeof(FillArrayData), true, kAllocData));
  tab_rec->table = table;
  tab_rec->vaddr = cu->current_dalvik_offset;
  uint16_t width = tab_rec->table[1];
  uint32_t size = tab_rec->table[2] | ((static_cast<uint32_t>(tab_rec->table[3])) << 16);
  tab_rec->size = (size * width) + 8;

  InsertGrowableList(cu, &cu->fill_array_data, reinterpret_cast<uintptr_t>(tab_rec));

  // Making a call - use explicit registers
  FlushAllRegs(cu);   /* Everything to home location */
  LoadValueDirectFixed(cu, rl_src, r0);
  LoadWordDisp(cu, rARM_SELF, ENTRYPOINT_OFFSET(pHandleFillArrayDataFromCode),
               rARM_LR);
  // Materialize a pointer to the fill data image
  NewLIR3(cu, kThumb2Adr, r1, 0, reinterpret_cast<uintptr_t>(tab_rec));
  ClobberCalleeSave(cu);
  LIR* call_inst = OpReg(cu, kOpBlx, rARM_LR);
  MarkSafepointPC(cu, call_inst);
}

/*
 * Handle simple case (thin lock) inline.  If it's complicated, bail
 * out to the heavyweight lock/unlock routines.  We'll use dedicated
 * registers here in order to be in the right position in case we
 * to bail to oat[Lock/Unlock]Object(self, object)
 *
 * r0 -> self pointer [arg0 for oat[Lock/Unlock]Object
 * r1 -> object [arg1 for oat[Lock/Unlock]Object
 * r2 -> intial contents of object->lock, later result of strex
 * r3 -> self->thread_id
 * r12 -> allow to be used by utilities as general temp
 *
 * The result of the strex is 0 if we acquire the lock.
 *
 * See comments in Sync.c for the layout of the lock word.
 * Of particular interest to this code is the test for the
 * simple case - which we handle inline.  For monitor enter, the
 * simple case is thin lock, held by no-one.  For monitor exit,
 * the simple case is thin lock, held by the unlocking thread with
 * a recurse count of 0.
 *
 * A minor complication is that there is a field in the lock word
 * unrelated to locking: the hash state.  This field must be ignored, but
 * preserved.
 *
 */
void ArmCodegen::GenMonitorEnter(CompilationUnit* cu, int opt_flags, RegLocation rl_src)
{
  FlushAllRegs(cu);
  DCHECK_EQ(LW_SHAPE_THIN, 0);
  LoadValueDirectFixed(cu, rl_src, r0);  // Get obj
  LockCallTemps(cu);  // Prepare for explicit register usage
  GenNullCheck(cu, rl_src.s_reg_low, r0, opt_flags);
  LoadWordDisp(cu, rARM_SELF, Thread::ThinLockIdOffset().Int32Value(), r2);
  NewLIR3(cu, kThumb2Ldrex, r1, r0,
          Object::MonitorOffset().Int32Value() >> 2); // Get object->lock
  // Align owner
  OpRegImm(cu, kOpLsl, r2, LW_LOCK_OWNER_SHIFT);
  // Is lock unheld on lock or held by us (==thread_id) on unlock?
  NewLIR4(cu, kThumb2Bfi, r2, r1, 0, LW_LOCK_OWNER_SHIFT - 1);
  NewLIR3(cu, kThumb2Bfc, r1, LW_HASH_STATE_SHIFT, LW_LOCK_OWNER_SHIFT - 1);
  OpRegImm(cu, kOpCmp, r1, 0);
  OpIT(cu, kCondEq, "");
  NewLIR4(cu, kThumb2Strex, r1, r2, r0,
          Object::MonitorOffset().Int32Value() >> 2);
  OpRegImm(cu, kOpCmp, r1, 0);
  OpIT(cu, kCondNe, "T");
  // Go expensive route - artLockObjectFromCode(self, obj);
  LoadWordDisp(cu, rARM_SELF, ENTRYPOINT_OFFSET(pLockObjectFromCode), rARM_LR);
  ClobberCalleeSave(cu);
  LIR* call_inst = OpReg(cu, kOpBlx, rARM_LR);
  MarkSafepointPC(cu, call_inst);
  GenMemBarrier(cu, kLoadLoad);
}

/*
 * For monitor unlock, we don't have to use ldrex/strex.  Once
 * we've determined that the lock is thin and that we own it with
 * a zero recursion count, it's safe to punch it back to the
 * initial, unlock thin state with a store word.
 */
void ArmCodegen::GenMonitorExit(CompilationUnit* cu, int opt_flags, RegLocation rl_src)
{
  DCHECK_EQ(LW_SHAPE_THIN, 0);
  FlushAllRegs(cu);
  LoadValueDirectFixed(cu, rl_src, r0);  // Get obj
  LockCallTemps(cu);  // Prepare for explicit register usage
  GenNullCheck(cu, rl_src.s_reg_low, r0, opt_flags);
  LoadWordDisp(cu, r0, Object::MonitorOffset().Int32Value(), r1); // Get lock
  LoadWordDisp(cu, rARM_SELF, Thread::ThinLockIdOffset().Int32Value(), r2);
  // Is lock unheld on lock or held by us (==thread_id) on unlock?
  OpRegRegImm(cu, kOpAnd, r3, r1,
              (LW_HASH_STATE_MASK << LW_HASH_STATE_SHIFT));
  // Align owner
  OpRegImm(cu, kOpLsl, r2, LW_LOCK_OWNER_SHIFT);
  NewLIR3(cu, kThumb2Bfc, r1, LW_HASH_STATE_SHIFT, LW_LOCK_OWNER_SHIFT - 1);
  OpRegReg(cu, kOpSub, r1, r2);
  OpIT(cu, kCondEq, "EE");
  StoreWordDisp(cu, r0, Object::MonitorOffset().Int32Value(), r3);
  // Go expensive route - UnlockObjectFromCode(obj);
  LoadWordDisp(cu, rARM_SELF, ENTRYPOINT_OFFSET(pUnlockObjectFromCode), rARM_LR);
  ClobberCalleeSave(cu);
  LIR* call_inst = OpReg(cu, kOpBlx, rARM_LR);
  MarkSafepointPC(cu, call_inst);
  GenMemBarrier(cu, kStoreLoad);
}

/*
 * Mark garbage collection card. Skip if the value we're storing is null.
 */
void ArmCodegen::MarkGCCard(CompilationUnit* cu, int val_reg, int tgt_addr_reg)
{
  int reg_card_base = AllocTemp(cu);
  int reg_card_no = AllocTemp(cu);
  LIR* branch_over = OpCmpImmBranch(cu, kCondEq, val_reg, 0, NULL);
  LoadWordDisp(cu, rARM_SELF, Thread::CardTableOffset().Int32Value(), reg_card_base);
  OpRegRegImm(cu, kOpLsr, reg_card_no, tgt_addr_reg, CardTable::kCardShift);
  StoreBaseIndexed(cu, reg_card_base, reg_card_no, reg_card_base, 0,
                   kUnsignedByte);
  LIR* target = NewLIR0(cu, kPseudoTargetLabel);
  branch_over->target = target;
  FreeTemp(cu, reg_card_base);
  FreeTemp(cu, reg_card_no);
}

void ArmCodegen::GenEntrySequence(CompilationUnit* cu, RegLocation* ArgLocs, RegLocation rl_method)
{
  int spill_count = cu->num_core_spills + cu->num_fp_spills;
  /*
   * On entry, r0, r1, r2 & r3 are live.  Let the register allocation
   * mechanism know so it doesn't try to use any of them when
   * expanding the frame or flushing.  This leaves the utility
   * code with a single temp: r12.  This should be enough.
   */
  LockTemp(cu, r0);
  LockTemp(cu, r1);
  LockTemp(cu, r2);
  LockTemp(cu, r3);

  /*
   * We can safely skip the stack overflow check if we're
   * a leaf *and* our frame size < fudge factor.
   */
  bool skip_overflow_check = ((cu->attrs & METHOD_IS_LEAF) &&
                            (static_cast<size_t>(cu->frame_size) <
                            Thread::kStackOverflowReservedBytes));
  NewLIR0(cu, kPseudoMethodEntry);
  if (!skip_overflow_check) {
    /* Load stack limit */
    LoadWordDisp(cu, rARM_SELF, Thread::StackEndOffset().Int32Value(), r12);
  }
  /* Spill core callee saves */
  NewLIR1(cu, kThumb2Push, cu->core_spill_mask);
  /* Need to spill any FP regs? */
  if (cu->num_fp_spills) {
    /*
     * NOTE: fp spills are a little different from core spills in that
     * they are pushed as a contiguous block.  When promoting from
     * the fp set, we must allocate all singles from s16..highest-promoted
     */
    NewLIR1(cu, kThumb2VPushCS, cu->num_fp_spills);
  }
  if (!skip_overflow_check) {
    OpRegRegImm(cu, kOpSub, rARM_LR, rARM_SP, cu->frame_size - (spill_count * 4));
    GenRegRegCheck(cu, kCondCc, rARM_LR, r12, kThrowStackOverflow);
    OpRegCopy(cu, rARM_SP, rARM_LR);     // Establish stack
  } else {
    OpRegImm(cu, kOpSub, rARM_SP, cu->frame_size - (spill_count * 4));
  }

  FlushIns(cu, ArgLocs, rl_method);

  FreeTemp(cu, r0);
  FreeTemp(cu, r1);
  FreeTemp(cu, r2);
  FreeTemp(cu, r3);
}

void ArmCodegen::GenExitSequence(CompilationUnit* cu)
{
  int spill_count = cu->num_core_spills + cu->num_fp_spills;
  /*
   * In the exit path, r0/r1 are live - make sure they aren't
   * allocated by the register utilities as temps.
   */
  LockTemp(cu, r0);
  LockTemp(cu, r1);

  NewLIR0(cu, kPseudoMethodExit);
  OpRegImm(cu, kOpAdd, rARM_SP, cu->frame_size - (spill_count * 4));
  /* Need to restore any FP callee saves? */
  if (cu->num_fp_spills) {
    NewLIR1(cu, kThumb2VPopCS, cu->num_fp_spills);
  }
  if (cu->core_spill_mask & (1 << rARM_LR)) {
    /* Unspill rARM_LR to rARM_PC */
    cu->core_spill_mask &= ~(1 << rARM_LR);
    cu->core_spill_mask |= (1 << rARM_PC);
  }
  NewLIR1(cu, kThumb2Pop, cu->core_spill_mask);
  if (!(cu->core_spill_mask & (1 << rARM_PC))) {
    /* We didn't pop to rARM_PC, so must do a bv rARM_LR */
    NewLIR1(cu, kThumbBx, rARM_LR);
  }
}

}  // namespace art
