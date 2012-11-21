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
#include "../codegen_util.h"
#include "../ralloc_util.h"

namespace art {


/* Return the position of an ssa name within the argument list */
int InPosition(CompilationUnit* cUnit, int sReg)
{
  int vReg = SRegToVReg(cUnit, sReg);
  return vReg - cUnit->numRegs;
}

/*
 * Describe an argument.  If it's already in an arg register, just leave it
 * there.  NOTE: all live arg registers must be locked prior to this call
 * to avoid having them allocated as a temp by downstream utilities.
 */
RegLocation ArgLoc(CompilationUnit* cUnit, RegLocation loc)
{
  int argNum = InPosition(cUnit, loc.sRegLow);
  if (loc.wide) {
    if (argNum == 2) {
      // Bad case - half in register, half in frame.  Just punt
      loc.location = kLocInvalid;
    } else if (argNum < 2) {
      loc.lowReg = rARM_ARG1 + argNum;
      loc.highReg = loc.lowReg + 1;
      loc.location = kLocPhysReg;
    } else {
      loc.location = kLocDalvikFrame;
    }
  } else {
    if (argNum < 3) {
      loc.lowReg = rARM_ARG1 + argNum;
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
RegLocation LoadArg(CompilationUnit* cUnit, RegLocation loc)
{
  if (loc.location == kLocDalvikFrame) {
    int start = (InPosition(cUnit, loc.sRegLow) + 1) * sizeof(uint32_t);
    loc.lowReg = AllocTemp(cUnit);
    LoadWordDisp(cUnit, rARM_SP, start, loc.lowReg);
    if (loc.wide) {
      loc.highReg = AllocTemp(cUnit);
      LoadWordDisp(cUnit, rARM_SP, start + sizeof(uint32_t), loc.highReg);
    }
    loc.location = kLocPhysReg;
  }
  return loc;
}

/* Lock any referenced arguments that arrive in registers */
void LockLiveArgs(CompilationUnit* cUnit, MIR* mir)
{
  int firstIn = cUnit->numRegs;
  const int numArgRegs = 3;  // TODO: generalize & move to RegUtil.cc
  for (int i = 0; i < mir->ssaRep->numUses; i++) {
    int vReg = SRegToVReg(cUnit, mir->ssaRep->uses[i]);
    int InPosition = vReg - firstIn;
    if (InPosition < numArgRegs) {
      LockTemp(cUnit, rARM_ARG1 + InPosition);
    }
  }
}

/* Find the next MIR, which may be in a following basic block */
MIR* GetNextMir(CompilationUnit* cUnit, BasicBlock** pBb, MIR* mir)
{
  BasicBlock* bb = *pBb;
  MIR* origMir = mir;
  while (bb != NULL) {
    if (mir != NULL) {
      mir = mir->next;
    }
    if (mir != NULL) {
      return mir;
    } else {
      bb = bb->fallThrough;
      *pBb = bb;
      if (bb) {
         mir = bb->firstMIRInsn;
         if (mir != NULL) {
           return mir;
         }
      }
    }
  }
  return origMir;
}

/* Used for the "printMe" listing */
void GenPrintLabel(CompilationUnit *cUnit, MIR* mir)
{
  /* Mark the beginning of a Dalvik instruction for line tracking */
  char* instStr = cUnit->printMe ?
     GetDalvikDisassembly(cUnit, mir->dalvikInsn, "") : NULL;
  MarkBoundary(cUnit, mir->offset, instStr);
  /* Don't generate the SSA annotation unless verbose mode is on */
  if (cUnit->printMe && mir->ssaRep) {
    char* ssaString = GetSSAString(cUnit, mir->ssaRep);
    NewLIR1(cUnit, kPseudoSSARep, reinterpret_cast<uintptr_t>(ssaString));
  }
}

MIR* SpecialIGet(CompilationUnit* cUnit, BasicBlock** bb, MIR* mir,
                 OpSize size, bool longOrDouble, bool isObject)
{
  int fieldOffset;
  bool isVolatile;
  uint32_t fieldIdx = mir->dalvikInsn.vC;
  bool fastPath = FastInstance(cUnit, fieldIdx, fieldOffset, isVolatile, false);
  if (!fastPath || !(mir->optimizationFlags & MIR_IGNORE_NULL_CHECK)) {
    return NULL;
  }
  RegLocation rlObj = GetSrc(cUnit, mir, 0);
  LockLiveArgs(cUnit, mir);
  rlObj = ArgLoc(cUnit, rlObj);
  RegLocation rlDest;
  if (longOrDouble) {
    rlDest = GetReturnWide(cUnit, false);
  } else {
    rlDest = GetReturn(cUnit, false);
  }
  // Point of no return - no aborts after this
  GenPrintLabel(cUnit, mir);
  rlObj = LoadArg(cUnit, rlObj);
  GenIGet(cUnit, fieldIdx, mir->optimizationFlags, size, rlDest, rlObj,
          longOrDouble, isObject);
  return GetNextMir(cUnit, bb, mir);
}

MIR* SpecialIPut(CompilationUnit* cUnit, BasicBlock** bb, MIR* mir,
                 OpSize size, bool longOrDouble, bool isObject)
{
  int fieldOffset;
  bool isVolatile;
  uint32_t fieldIdx = mir->dalvikInsn.vC;
  bool fastPath = FastInstance(cUnit, fieldIdx, fieldOffset, isVolatile, false);
  if (!fastPath || !(mir->optimizationFlags & MIR_IGNORE_NULL_CHECK)) {
    return NULL;
  }
  RegLocation rlSrc;
  RegLocation rlObj;
  LockLiveArgs(cUnit, mir);
  if (longOrDouble) {
    rlSrc = GetSrcWide(cUnit, mir, 0);
    rlObj = GetSrc(cUnit, mir, 2);
  } else {
    rlSrc = GetSrc(cUnit, mir, 0);
    rlObj = GetSrc(cUnit, mir, 1);
  }
  rlSrc = ArgLoc(cUnit, rlSrc);
  rlObj = ArgLoc(cUnit, rlObj);
  // Reject if source is split across registers & frame
  if (rlObj.location == kLocInvalid) {
    ResetRegPool(cUnit);
    return NULL;
  }
  // Point of no return - no aborts after this
  GenPrintLabel(cUnit, mir);
  rlObj = LoadArg(cUnit, rlObj);
  rlSrc = LoadArg(cUnit, rlSrc);
  GenIPut(cUnit, fieldIdx, mir->optimizationFlags, size, rlSrc, rlObj,
          longOrDouble, isObject);
  return GetNextMir(cUnit, bb, mir);
}

MIR* SpecialIdentity(CompilationUnit* cUnit, MIR* mir)
{
  RegLocation rlSrc;
  RegLocation rlDest;
  bool wide = (mir->ssaRep->numUses == 2);
  if (wide) {
    rlSrc = GetSrcWide(cUnit, mir, 0);
    rlDest = GetReturnWide(cUnit, false);
  } else {
    rlSrc = GetSrc(cUnit, mir, 0);
    rlDest = GetReturn(cUnit, false);
  }
  LockLiveArgs(cUnit, mir);
  rlSrc = ArgLoc(cUnit, rlSrc);
  if (rlSrc.location == kLocInvalid) {
    ResetRegPool(cUnit);
    return NULL;
  }
  // Point of no return - no aborts after this
  GenPrintLabel(cUnit, mir);
  rlSrc = LoadArg(cUnit, rlSrc);
  if (wide) {
    StoreValueWide(cUnit, rlDest, rlSrc);
  } else {
    StoreValue(cUnit, rlDest, rlSrc);
  }
  return mir;
}

/*
 * Special-case code genration for simple non-throwing leaf methods.
 */
void GenSpecialCase(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
          SpecialCaseHandler specialCase)
{
   cUnit->currentDalvikOffset = mir->offset;
   MIR* nextMir = NULL;
   switch (specialCase) {
     case kNullMethod:
       DCHECK(mir->dalvikInsn.opcode == Instruction::RETURN_VOID);
       nextMir = mir;
       break;
     case kConstFunction:
       GenPrintLabel(cUnit, mir);
       LoadConstant(cUnit, rARM_RET0, mir->dalvikInsn.vB);
       nextMir = GetNextMir(cUnit, &bb, mir);
       break;
     case kIGet:
       nextMir = SpecialIGet(cUnit, &bb, mir, kWord, false, false);
       break;
     case kIGetBoolean:
     case kIGetByte:
       nextMir = SpecialIGet(cUnit, &bb, mir, kUnsignedByte, false, false);
       break;
     case kIGetObject:
       nextMir = SpecialIGet(cUnit, &bb, mir, kWord, false, true);
       break;
     case kIGetChar:
       nextMir = SpecialIGet(cUnit, &bb, mir, kUnsignedHalf, false, false);
       break;
     case kIGetShort:
       nextMir = SpecialIGet(cUnit, &bb, mir, kSignedHalf, false, false);
       break;
     case kIGetWide:
       nextMir = SpecialIGet(cUnit, &bb, mir, kLong, true, false);
       break;
     case kIPut:
       nextMir = SpecialIPut(cUnit, &bb, mir, kWord, false, false);
       break;
     case kIPutBoolean:
     case kIPutByte:
       nextMir = SpecialIPut(cUnit, &bb, mir, kUnsignedByte, false, false);
       break;
     case kIPutObject:
       nextMir = SpecialIPut(cUnit, &bb, mir, kWord, false, true);
       break;
     case kIPutChar:
       nextMir = SpecialIPut(cUnit, &bb, mir, kUnsignedHalf, false, false);
       break;
     case kIPutShort:
       nextMir = SpecialIPut(cUnit, &bb, mir, kSignedHalf, false, false);
       break;
     case kIPutWide:
       nextMir = SpecialIPut(cUnit, &bb, mir, kLong, true, false);
       break;
     case kIdentity:
       nextMir = SpecialIdentity(cUnit, mir);
       break;
     default:
       return;
   }
   if (nextMir != NULL) {
    cUnit->currentDalvikOffset = nextMir->offset;
    if (specialCase != kIdentity) {
      GenPrintLabel(cUnit, nextMir);
    }
    NewLIR1(cUnit, kThumbBx, rARM_LR);
    cUnit->coreSpillMask = 0;
    cUnit->numCoreSpills = 0;
    cUnit->fpSpillMask = 0;
    cUnit->numFPSpills = 0;
    cUnit->frameSize = 0;
    cUnit->coreVmapTable.clear();
    cUnit->fpVmapTable.clear();
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
 *   ldr   rVal, [rARM_SP, vRegOff]
 *   mov   rIdx, #tableSize
 * lp:
 *   ldmia rBase!, {rKey, rDisp}
 *   sub   rIdx, #1
 *   cmp   rVal, rKey
 *   ifeq
 *   add   rARM_PC, rDisp   ; This is the branch from which we compute displacement
 *   cbnz  rIdx, lp
 */
void GenSparseSwitch(CompilationUnit* cUnit, uint32_t tableOffset,
                     RegLocation rlSrc)
{
  const uint16_t* table = cUnit->insns + cUnit->currentDalvikOffset + tableOffset;
  if (cUnit->printMe) {
    DumpSparseSwitchTable(table);
  }
  // Add the table to the list - we'll process it later
  SwitchTable *tabRec =
      static_cast<SwitchTable*>(NewMem(cUnit, sizeof(SwitchTable), true, kAllocData));
  tabRec->table = table;
  tabRec->vaddr = cUnit->currentDalvikOffset;
  int size = table[1];
  tabRec->targets = static_cast<LIR**>(NewMem(cUnit, size * sizeof(LIR*), true, kAllocLIR));
  InsertGrowableList(cUnit, &cUnit->switchTables, reinterpret_cast<uintptr_t>(tabRec));

  // Get the switch value
  rlSrc = LoadValue(cUnit, rlSrc, kCoreReg);
  int rBase = AllocTemp(cUnit);
  /* Allocate key and disp temps */
  int rKey = AllocTemp(cUnit);
  int rDisp = AllocTemp(cUnit);
  // Make sure rKey's register number is less than rDisp's number for ldmia
  if (rKey > rDisp) {
    int tmp = rDisp;
    rDisp = rKey;
    rKey = tmp;
  }
  // Materialize a pointer to the switch table
  NewLIR3(cUnit, kThumb2Adr, rBase, 0, reinterpret_cast<uintptr_t>(tabRec));
  // Set up rIdx
  int rIdx = AllocTemp(cUnit);
  LoadConstant(cUnit, rIdx, size);
  // Establish loop branch target
  LIR* target = NewLIR0(cUnit, kPseudoTargetLabel);
  // Load next key/disp
  NewLIR2(cUnit, kThumb2LdmiaWB, rBase, (1 << rKey) | (1 << rDisp));
  OpRegReg(cUnit, kOpCmp, rKey, rlSrc.lowReg);
  // Go if match. NOTE: No instruction set switch here - must stay Thumb2
  OpIT(cUnit, kArmCondEq, "");
  LIR* switchBranch = NewLIR1(cUnit, kThumb2AddPCR, rDisp);
  tabRec->anchor = switchBranch;
  // Needs to use setflags encoding here
  NewLIR3(cUnit, kThumb2SubsRRI12, rIdx, rIdx, 1);
  OpCondBranch(cUnit, kCondNe, target);
}


void GenPackedSwitch(CompilationUnit* cUnit, uint32_t tableOffset,
                     RegLocation rlSrc)
{
  const uint16_t* table = cUnit->insns + cUnit->currentDalvikOffset + tableOffset;
  if (cUnit->printMe) {
    DumpPackedSwitchTable(table);
  }
  // Add the table to the list - we'll process it later
  SwitchTable *tabRec =
      static_cast<SwitchTable*>(NewMem(cUnit, sizeof(SwitchTable), true, kAllocData));
  tabRec->table = table;
  tabRec->vaddr = cUnit->currentDalvikOffset;
  int size = table[1];
  tabRec->targets = static_cast<LIR**>(NewMem(cUnit, size * sizeof(LIR*), true, kAllocLIR));
  InsertGrowableList(cUnit, &cUnit->switchTables, reinterpret_cast<uintptr_t>(tabRec));

  // Get the switch value
  rlSrc = LoadValue(cUnit, rlSrc, kCoreReg);
  int tableBase = AllocTemp(cUnit);
  // Materialize a pointer to the switch table
  NewLIR3(cUnit, kThumb2Adr, tableBase, 0, reinterpret_cast<uintptr_t>(tabRec));
  int lowKey = s4FromSwitchData(&table[2]);
  int keyReg;
  // Remove the bias, if necessary
  if (lowKey == 0) {
    keyReg = rlSrc.lowReg;
  } else {
    keyReg = AllocTemp(cUnit);
    OpRegRegImm(cUnit, kOpSub, keyReg, rlSrc.lowReg, lowKey);
  }
  // Bounds check - if < 0 or >= size continue following switch
  OpRegImm(cUnit, kOpCmp, keyReg, size-1);
  LIR* branchOver = OpCondBranch(cUnit, kCondHi, NULL);

  // Load the displacement from the switch table
  int dispReg = AllocTemp(cUnit);
  LoadBaseIndexed(cUnit, tableBase, keyReg, dispReg, 2, kWord);

  // ..and go! NOTE: No instruction set switch here - must stay Thumb2
  LIR* switchBranch = NewLIR1(cUnit, kThumb2AddPCR, dispReg);
  tabRec->anchor = switchBranch;

  /* branchOver target here */
  LIR* target = NewLIR0(cUnit, kPseudoTargetLabel);
  branchOver->target = target;
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
void GenFillArrayData(CompilationUnit* cUnit, uint32_t tableOffset, RegLocation rlSrc)
{
  const uint16_t* table = cUnit->insns + cUnit->currentDalvikOffset + tableOffset;
  // Add the table to the list - we'll process it later
  FillArrayData *tabRec =
      static_cast<FillArrayData*>(NewMem(cUnit, sizeof(FillArrayData), true, kAllocData));
  tabRec->table = table;
  tabRec->vaddr = cUnit->currentDalvikOffset;
  uint16_t width = tabRec->table[1];
  uint32_t size = tabRec->table[2] | ((static_cast<uint32_t>(tabRec->table[3])) << 16);
  tabRec->size = (size * width) + 8;

  InsertGrowableList(cUnit, &cUnit->fillArrayData, reinterpret_cast<uintptr_t>(tabRec));

  // Making a call - use explicit registers
  FlushAllRegs(cUnit);   /* Everything to home location */
  LoadValueDirectFixed(cUnit, rlSrc, r0);
  LoadWordDisp(cUnit, rARM_SELF, ENTRYPOINT_OFFSET(pHandleFillArrayDataFromCode),
               rARM_LR);
  // Materialize a pointer to the fill data image
  NewLIR3(cUnit, kThumb2Adr, r1, 0, reinterpret_cast<uintptr_t>(tabRec));
  ClobberCalleeSave(cUnit);
  LIR* callInst = OpReg(cUnit, kOpBlx, rARM_LR);
  MarkSafepointPC(cUnit, callInst);
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
 * r3 -> self->threadId
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
void GenMonitorEnter(CompilationUnit* cUnit, int optFlags, RegLocation rlSrc)
{
  FlushAllRegs(cUnit);
  DCHECK_EQ(LW_SHAPE_THIN, 0);
  LoadValueDirectFixed(cUnit, rlSrc, r0);  // Get obj
  LockCallTemps(cUnit);  // Prepare for explicit register usage
  GenNullCheck(cUnit, rlSrc.sRegLow, r0, optFlags);
  LoadWordDisp(cUnit, rARM_SELF, Thread::ThinLockIdOffset().Int32Value(), r2);
  NewLIR3(cUnit, kThumb2Ldrex, r1, r0,
          Object::MonitorOffset().Int32Value() >> 2); // Get object->lock
  // Align owner
  OpRegImm(cUnit, kOpLsl, r2, LW_LOCK_OWNER_SHIFT);
  // Is lock unheld on lock or held by us (==threadId) on unlock?
  NewLIR4(cUnit, kThumb2Bfi, r2, r1, 0, LW_LOCK_OWNER_SHIFT - 1);
  NewLIR3(cUnit, kThumb2Bfc, r1, LW_HASH_STATE_SHIFT, LW_LOCK_OWNER_SHIFT - 1);
  OpRegImm(cUnit, kOpCmp, r1, 0);
  OpIT(cUnit, kArmCondEq, "");
  NewLIR4(cUnit, kThumb2Strex, r1, r2, r0,
          Object::MonitorOffset().Int32Value() >> 2);
  OpRegImm(cUnit, kOpCmp, r1, 0);
  OpIT(cUnit, kArmCondNe, "T");
  // Go expensive route - artLockObjectFromCode(self, obj);
  LoadWordDisp(cUnit, rARM_SELF, ENTRYPOINT_OFFSET(pLockObjectFromCode), rARM_LR);
  ClobberCalleeSave(cUnit);
  LIR* callInst = OpReg(cUnit, kOpBlx, rARM_LR);
  MarkSafepointPC(cUnit, callInst);
  GenMemBarrier(cUnit, kLoadLoad);
}

/*
 * For monitor unlock, we don't have to use ldrex/strex.  Once
 * we've determined that the lock is thin and that we own it with
 * a zero recursion count, it's safe to punch it back to the
 * initial, unlock thin state with a store word.
 */
void GenMonitorExit(CompilationUnit* cUnit, int optFlags, RegLocation rlSrc)
{
  DCHECK_EQ(LW_SHAPE_THIN, 0);
  FlushAllRegs(cUnit);
  LoadValueDirectFixed(cUnit, rlSrc, r0);  // Get obj
  LockCallTemps(cUnit);  // Prepare for explicit register usage
  GenNullCheck(cUnit, rlSrc.sRegLow, r0, optFlags);
  LoadWordDisp(cUnit, r0, Object::MonitorOffset().Int32Value(), r1); // Get lock
  LoadWordDisp(cUnit, rARM_SELF, Thread::ThinLockIdOffset().Int32Value(), r2);
  // Is lock unheld on lock or held by us (==threadId) on unlock?
  OpRegRegImm(cUnit, kOpAnd, r3, r1,
              (LW_HASH_STATE_MASK << LW_HASH_STATE_SHIFT));
  // Align owner
  OpRegImm(cUnit, kOpLsl, r2, LW_LOCK_OWNER_SHIFT);
  NewLIR3(cUnit, kThumb2Bfc, r1, LW_HASH_STATE_SHIFT, LW_LOCK_OWNER_SHIFT - 1);
  OpRegReg(cUnit, kOpSub, r1, r2);
  OpIT(cUnit, kArmCondEq, "EE");
  StoreWordDisp(cUnit, r0, Object::MonitorOffset().Int32Value(), r3);
  // Go expensive route - UnlockObjectFromCode(obj);
  LoadWordDisp(cUnit, rARM_SELF, ENTRYPOINT_OFFSET(pUnlockObjectFromCode), rARM_LR);
  ClobberCalleeSave(cUnit);
  LIR* callInst = OpReg(cUnit, kOpBlx, rARM_LR);
  MarkSafepointPC(cUnit, callInst);
  GenMemBarrier(cUnit, kStoreLoad);
}

/*
 * Mark garbage collection card. Skip if the value we're storing is null.
 */
void MarkGCCard(CompilationUnit* cUnit, int valReg, int tgtAddrReg)
{
  int regCardBase = AllocTemp(cUnit);
  int regCardNo = AllocTemp(cUnit);
  LIR* branchOver = OpCmpImmBranch(cUnit, kCondEq, valReg, 0, NULL);
  LoadWordDisp(cUnit, rARM_SELF, Thread::CardTableOffset().Int32Value(), regCardBase);
  OpRegRegImm(cUnit, kOpLsr, regCardNo, tgtAddrReg, CardTable::kCardShift);
  StoreBaseIndexed(cUnit, regCardBase, regCardNo, regCardBase, 0,
                   kUnsignedByte);
  LIR* target = NewLIR0(cUnit, kPseudoTargetLabel);
  branchOver->target = target;
  FreeTemp(cUnit, regCardBase);
  FreeTemp(cUnit, regCardNo);
}

void GenEntrySequence(CompilationUnit* cUnit, RegLocation* ArgLocs,
                      RegLocation rlMethod)
{
  int spillCount = cUnit->numCoreSpills + cUnit->numFPSpills;
  /*
   * On entry, r0, r1, r2 & r3 are live.  Let the register allocation
   * mechanism know so it doesn't try to use any of them when
   * expanding the frame or flushing.  This leaves the utility
   * code with a single temp: r12.  This should be enough.
   */
  LockTemp(cUnit, r0);
  LockTemp(cUnit, r1);
  LockTemp(cUnit, r2);
  LockTemp(cUnit, r3);

  /*
   * We can safely skip the stack overflow check if we're
   * a leaf *and* our frame size < fudge factor.
   */
  bool skipOverflowCheck = ((cUnit->attrs & METHOD_IS_LEAF) &&
                            (static_cast<size_t>(cUnit->frameSize) <
                            Thread::kStackOverflowReservedBytes));
  NewLIR0(cUnit, kPseudoMethodEntry);
  if (!skipOverflowCheck) {
    /* Load stack limit */
    LoadWordDisp(cUnit, rARM_SELF, Thread::StackEndOffset().Int32Value(), r12);
  }
  /* Spill core callee saves */
  NewLIR1(cUnit, kThumb2Push, cUnit->coreSpillMask);
  /* Need to spill any FP regs? */
  if (cUnit->numFPSpills) {
    /*
     * NOTE: fp spills are a little different from core spills in that
     * they are pushed as a contiguous block.  When promoting from
     * the fp set, we must allocate all singles from s16..highest-promoted
     */
    NewLIR1(cUnit, kThumb2VPushCS, cUnit->numFPSpills);
  }
  if (!skipOverflowCheck) {
    OpRegRegImm(cUnit, kOpSub, rARM_LR, rARM_SP, cUnit->frameSize - (spillCount * 4));
    GenRegRegCheck(cUnit, kCondCc, rARM_LR, r12, kThrowStackOverflow);
    OpRegCopy(cUnit, rARM_SP, rARM_LR);     // Establish stack
  } else {
    OpRegImm(cUnit, kOpSub, rARM_SP, cUnit->frameSize - (spillCount * 4));
  }

  FlushIns(cUnit, ArgLocs, rlMethod);

  FreeTemp(cUnit, r0);
  FreeTemp(cUnit, r1);
  FreeTemp(cUnit, r2);
  FreeTemp(cUnit, r3);
}

void GenExitSequence(CompilationUnit* cUnit)
{
  int spillCount = cUnit->numCoreSpills + cUnit->numFPSpills;
  /*
   * In the exit path, r0/r1 are live - make sure they aren't
   * allocated by the register utilities as temps.
   */
  LockTemp(cUnit, r0);
  LockTemp(cUnit, r1);

  NewLIR0(cUnit, kPseudoMethodExit);
  OpRegImm(cUnit, kOpAdd, rARM_SP, cUnit->frameSize - (spillCount * 4));
  /* Need to restore any FP callee saves? */
  if (cUnit->numFPSpills) {
    NewLIR1(cUnit, kThumb2VPopCS, cUnit->numFPSpills);
  }
  if (cUnit->coreSpillMask & (1 << rARM_LR)) {
    /* Unspill rARM_LR to rARM_PC */
    cUnit->coreSpillMask &= ~(1 << rARM_LR);
    cUnit->coreSpillMask |= (1 << rARM_PC);
  }
  NewLIR1(cUnit, kThumb2Pop, cUnit->coreSpillMask);
  if (!(cUnit->coreSpillMask & (1 << rARM_PC))) {
    /* We didn't pop to rARM_PC, so must do a bv rARM_LR */
    NewLIR1(cUnit, kThumbBx, rARM_LR);
  }
}

}  // namespace art
