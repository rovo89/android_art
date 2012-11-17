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

namespace art {


/* Return the position of an ssa name within the argument list */
int inPosition(CompilationUnit* cUnit, int sReg)
{
  int vReg = SRegToVReg(cUnit, sReg);
  return vReg - cUnit->numRegs;
}

/*
 * Describe an argument.  If it's already in an arg register, just leave it
 * there.  NOTE: all live arg registers must be locked prior to this call
 * to avoid having them allocated as a temp by downstream utilities.
 */
RegLocation argLoc(CompilationUnit* cUnit, RegLocation loc)
{
  int argNum = inPosition(cUnit, loc.sRegLow);
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
 * the frame, we can't use the normal loadValue() because it assumed
 * a proper frame - and we're frameless.
 */
RegLocation loadArg(CompilationUnit* cUnit, RegLocation loc)
{
  if (loc.location == kLocDalvikFrame) {
    int start = (inPosition(cUnit, loc.sRegLow) + 1) * sizeof(uint32_t);
    loc.lowReg = oatAllocTemp(cUnit);
    loadWordDisp(cUnit, rARM_SP, start, loc.lowReg);
    if (loc.wide) {
      loc.highReg = oatAllocTemp(cUnit);
      loadWordDisp(cUnit, rARM_SP, start + sizeof(uint32_t), loc.highReg);
    }
    loc.location = kLocPhysReg;
  }
  return loc;
}

/* Lock any referenced arguments that arrive in registers */
void lockLiveArgs(CompilationUnit* cUnit, MIR* mir)
{
  int firstIn = cUnit->numRegs;
  const int numArgRegs = 3;  // TODO: generalize & move to RegUtil.cc
  for (int i = 0; i < mir->ssaRep->numUses; i++) {
    int vReg = SRegToVReg(cUnit, mir->ssaRep->uses[i]);
    int inPosition = vReg - firstIn;
    if (inPosition < numArgRegs) {
      oatLockTemp(cUnit, rARM_ARG1 + inPosition);
    }
  }
}

/* Find the next MIR, which may be in a following basic block */
MIR* getNextMir(CompilationUnit* cUnit, BasicBlock** pBb, MIR* mir)
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
void genPrintLabel(CompilationUnit *cUnit, MIR* mir)
{
  /* Mark the beginning of a Dalvik instruction for line tracking */
  char* instStr = cUnit->printMe ?
     oatGetDalvikDisassembly(cUnit, mir->dalvikInsn, "") : NULL;
  markBoundary(cUnit, mir->offset, instStr);
  /* Don't generate the SSA annotation unless verbose mode is on */
  if (cUnit->printMe && mir->ssaRep) {
    char* ssaString = oatGetSSAString(cUnit, mir->ssaRep);
    newLIR1(cUnit, kPseudoSSARep, (int) ssaString);
  }
}

MIR* specialIGet(CompilationUnit* cUnit, BasicBlock** bb, MIR* mir,
                 OpSize size, bool longOrDouble, bool isObject)
{
  int fieldOffset;
  bool isVolatile;
  uint32_t fieldIdx = mir->dalvikInsn.vC;
  bool fastPath = fastInstance(cUnit, fieldIdx, fieldOffset, isVolatile, false);
  if (!fastPath || !(mir->optimizationFlags & MIR_IGNORE_NULL_CHECK)) {
    return NULL;
  }
  RegLocation rlObj = oatGetSrc(cUnit, mir, 0);
  lockLiveArgs(cUnit, mir);
  rlObj = argLoc(cUnit, rlObj);
  RegLocation rlDest;
  if (longOrDouble) {
    rlDest = oatGetReturnWide(cUnit, false);
  } else {
    rlDest = oatGetReturn(cUnit, false);
  }
  // Point of no return - no aborts after this
  genPrintLabel(cUnit, mir);
  rlObj = loadArg(cUnit, rlObj);
  genIGet(cUnit, fieldIdx, mir->optimizationFlags, size, rlDest, rlObj,
          longOrDouble, isObject);
  return getNextMir(cUnit, bb, mir);
}

MIR* specialIPut(CompilationUnit* cUnit, BasicBlock** bb, MIR* mir,
                 OpSize size, bool longOrDouble, bool isObject)
{
  int fieldOffset;
  bool isVolatile;
  uint32_t fieldIdx = mir->dalvikInsn.vC;
  bool fastPath = fastInstance(cUnit, fieldIdx, fieldOffset, isVolatile, false);
  if (!fastPath || !(mir->optimizationFlags & MIR_IGNORE_NULL_CHECK)) {
    return NULL;
  }
  RegLocation rlSrc;
  RegLocation rlObj;
  lockLiveArgs(cUnit, mir);
  if (longOrDouble) {
    rlSrc = oatGetSrcWide(cUnit, mir, 0);
    rlObj = oatGetSrc(cUnit, mir, 2);
  } else {
    rlSrc = oatGetSrc(cUnit, mir, 0);
    rlObj = oatGetSrc(cUnit, mir, 1);
  }
  rlSrc = argLoc(cUnit, rlSrc);
  rlObj = argLoc(cUnit, rlObj);
  // Reject if source is split across registers & frame
  if (rlObj.location == kLocInvalid) {
    oatResetRegPool(cUnit);
    return NULL;
  }
  // Point of no return - no aborts after this
  genPrintLabel(cUnit, mir);
  rlObj = loadArg(cUnit, rlObj);
  rlSrc = loadArg(cUnit, rlSrc);
  genIPut(cUnit, fieldIdx, mir->optimizationFlags, size, rlSrc, rlObj,
          longOrDouble, isObject);
  return getNextMir(cUnit, bb, mir);
}

MIR* specialIdentity(CompilationUnit* cUnit, MIR* mir)
{
  RegLocation rlSrc;
  RegLocation rlDest;
  bool wide = (mir->ssaRep->numUses == 2);
  if (wide) {
    rlSrc = oatGetSrcWide(cUnit, mir, 0);
    rlDest = oatGetReturnWide(cUnit, false);
  } else {
    rlSrc = oatGetSrc(cUnit, mir, 0);
    rlDest = oatGetReturn(cUnit, false);
  }
  lockLiveArgs(cUnit, mir);
  rlSrc = argLoc(cUnit, rlSrc);
  if (rlSrc.location == kLocInvalid) {
    oatResetRegPool(cUnit);
    return NULL;
  }
  // Point of no return - no aborts after this
  genPrintLabel(cUnit, mir);
  rlSrc = loadArg(cUnit, rlSrc);
  if (wide) {
    storeValueWide(cUnit, rlDest, rlSrc);
  } else {
    storeValue(cUnit, rlDest, rlSrc);
  }
  return mir;
}

/*
 * Special-case code genration for simple non-throwing leaf methods.
 */
void genSpecialCase(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
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
       genPrintLabel(cUnit, mir);
       loadConstant(cUnit, rARM_RET0, mir->dalvikInsn.vB);
       nextMir = getNextMir(cUnit, &bb, mir);
       break;
     case kIGet:
       nextMir = specialIGet(cUnit, &bb, mir, kWord, false, false);
       break;
     case kIGetBoolean:
     case kIGetByte:
       nextMir = specialIGet(cUnit, &bb, mir, kUnsignedByte, false, false);
       break;
     case kIGetObject:
       nextMir = specialIGet(cUnit, &bb, mir, kWord, false, true);
       break;
     case kIGetChar:
       nextMir = specialIGet(cUnit, &bb, mir, kUnsignedHalf, false, false);
       break;
     case kIGetShort:
       nextMir = specialIGet(cUnit, &bb, mir, kSignedHalf, false, false);
       break;
     case kIGetWide:
       nextMir = specialIGet(cUnit, &bb, mir, kLong, true, false);
       break;
     case kIPut:
       nextMir = specialIPut(cUnit, &bb, mir, kWord, false, false);
       break;
     case kIPutBoolean:
     case kIPutByte:
       nextMir = specialIPut(cUnit, &bb, mir, kUnsignedByte, false, false);
       break;
     case kIPutObject:
       nextMir = specialIPut(cUnit, &bb, mir, kWord, false, true);
       break;
     case kIPutChar:
       nextMir = specialIPut(cUnit, &bb, mir, kUnsignedHalf, false, false);
       break;
     case kIPutShort:
       nextMir = specialIPut(cUnit, &bb, mir, kSignedHalf, false, false);
       break;
     case kIPutWide:
       nextMir = specialIPut(cUnit, &bb, mir, kLong, true, false);
       break;
     case kIdentity:
       nextMir = specialIdentity(cUnit, mir);
       break;
     default:
       return;
   }
   if (nextMir != NULL) {
    cUnit->currentDalvikOffset = nextMir->offset;
    if (specialCase != kIdentity) {
      genPrintLabel(cUnit, nextMir);
    }
    newLIR1(cUnit, kThumbBx, rARM_LR);
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
void genSparseSwitch(CompilationUnit* cUnit, uint32_t tableOffset,
                     RegLocation rlSrc)
{
  const uint16_t* table = cUnit->insns + cUnit->currentDalvikOffset + tableOffset;
  if (cUnit->printMe) {
    dumpSparseSwitchTable(table);
  }
  // Add the table to the list - we'll process it later
  SwitchTable *tabRec = (SwitchTable *)oatNew(cUnit, sizeof(SwitchTable),
                                              true, kAllocData);
  tabRec->table = table;
  tabRec->vaddr = cUnit->currentDalvikOffset;
  int size = table[1];
  tabRec->targets = (LIR* *)oatNew(cUnit, size * sizeof(LIR*), true, kAllocLIR);
  oatInsertGrowableList(cUnit, &cUnit->switchTables, (intptr_t)tabRec);

  // Get the switch value
  rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
  int rBase = oatAllocTemp(cUnit);
  /* Allocate key and disp temps */
  int rKey = oatAllocTemp(cUnit);
  int rDisp = oatAllocTemp(cUnit);
  // Make sure rKey's register number is less than rDisp's number for ldmia
  if (rKey > rDisp) {
    int tmp = rDisp;
    rDisp = rKey;
    rKey = tmp;
  }
  // Materialize a pointer to the switch table
  newLIR3(cUnit, kThumb2Adr, rBase, 0, (intptr_t)tabRec);
  // Set up rIdx
  int rIdx = oatAllocTemp(cUnit);
  loadConstant(cUnit, rIdx, size);
  // Establish loop branch target
  LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
  // Load next key/disp
  newLIR2(cUnit, kThumb2LdmiaWB, rBase, (1 << rKey) | (1 << rDisp));
  opRegReg(cUnit, kOpCmp, rKey, rlSrc.lowReg);
  // Go if match. NOTE: No instruction set switch here - must stay Thumb2
  opIT(cUnit, kArmCondEq, "");
  LIR* switchBranch = newLIR1(cUnit, kThumb2AddPCR, rDisp);
  tabRec->anchor = switchBranch;
  // Needs to use setflags encoding here
  newLIR3(cUnit, kThumb2SubsRRI12, rIdx, rIdx, 1);
  opCondBranch(cUnit, kCondNe, target);
}


void genPackedSwitch(CompilationUnit* cUnit, uint32_t tableOffset,
                     RegLocation rlSrc)
{
  const uint16_t* table = cUnit->insns + cUnit->currentDalvikOffset + tableOffset;
  if (cUnit->printMe) {
    dumpPackedSwitchTable(table);
  }
  // Add the table to the list - we'll process it later
  SwitchTable *tabRec = (SwitchTable *)oatNew(cUnit, sizeof(SwitchTable),
                                              true, kAllocData);
  tabRec->table = table;
  tabRec->vaddr = cUnit->currentDalvikOffset;
  int size = table[1];
  tabRec->targets = (LIR* *)oatNew(cUnit, size * sizeof(LIR*), true, kAllocLIR);
  oatInsertGrowableList(cUnit, &cUnit->switchTables, (intptr_t)tabRec);

  // Get the switch value
  rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
  int tableBase = oatAllocTemp(cUnit);
  // Materialize a pointer to the switch table
  newLIR3(cUnit, kThumb2Adr, tableBase, 0, (intptr_t)tabRec);
  int lowKey = s4FromSwitchData(&table[2]);
  int keyReg;
  // Remove the bias, if necessary
  if (lowKey == 0) {
    keyReg = rlSrc.lowReg;
  } else {
    keyReg = oatAllocTemp(cUnit);
    opRegRegImm(cUnit, kOpSub, keyReg, rlSrc.lowReg, lowKey);
  }
  // Bounds check - if < 0 or >= size continue following switch
  opRegImm(cUnit, kOpCmp, keyReg, size-1);
  LIR* branchOver = opCondBranch(cUnit, kCondHi, NULL);

  // Load the displacement from the switch table
  int dispReg = oatAllocTemp(cUnit);
  loadBaseIndexed(cUnit, tableBase, keyReg, dispReg, 2, kWord);

  // ..and go! NOTE: No instruction set switch here - must stay Thumb2
  LIR* switchBranch = newLIR1(cUnit, kThumb2AddPCR, dispReg);
  tabRec->anchor = switchBranch;

  /* branchOver target here */
  LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
  branchOver->target = (LIR*)target;
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
void genFillArrayData(CompilationUnit* cUnit, uint32_t tableOffset, RegLocation rlSrc)
{
  const uint16_t* table = cUnit->insns + cUnit->currentDalvikOffset + tableOffset;
  // Add the table to the list - we'll process it later
  FillArrayData *tabRec = (FillArrayData *)
     oatNew(cUnit, sizeof(FillArrayData), true, kAllocData);
  tabRec->table = table;
  tabRec->vaddr = cUnit->currentDalvikOffset;
  uint16_t width = tabRec->table[1];
  uint32_t size = tabRec->table[2] | ((static_cast<uint32_t>(tabRec->table[3])) << 16);
  tabRec->size = (size * width) + 8;

  oatInsertGrowableList(cUnit, &cUnit->fillArrayData, (intptr_t)tabRec);

  // Making a call - use explicit registers
  oatFlushAllRegs(cUnit);   /* Everything to home location */
  loadValueDirectFixed(cUnit, rlSrc, r0);
  loadWordDisp(cUnit, rARM_SELF, ENTRYPOINT_OFFSET(pHandleFillArrayDataFromCode),
               rARM_LR);
  // Materialize a pointer to the fill data image
  newLIR3(cUnit, kThumb2Adr, r1, 0, (intptr_t)tabRec);
  oatClobberCalleeSave(cUnit);
  LIR* callInst = opReg(cUnit, kOpBlx, rARM_LR);
  markSafepointPC(cUnit, callInst);
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
void genMonitorEnter(CompilationUnit* cUnit, int optFlags, RegLocation rlSrc)
{
  oatFlushAllRegs(cUnit);
  DCHECK_EQ(LW_SHAPE_THIN, 0);
  loadValueDirectFixed(cUnit, rlSrc, r0);  // Get obj
  oatLockCallTemps(cUnit);  // Prepare for explicit register usage
  genNullCheck(cUnit, rlSrc.sRegLow, r0, optFlags);
  loadWordDisp(cUnit, rARM_SELF, Thread::ThinLockIdOffset().Int32Value(), r2);
  newLIR3(cUnit, kThumb2Ldrex, r1, r0,
          Object::MonitorOffset().Int32Value() >> 2); // Get object->lock
  // Align owner
  opRegImm(cUnit, kOpLsl, r2, LW_LOCK_OWNER_SHIFT);
  // Is lock unheld on lock or held by us (==threadId) on unlock?
  newLIR4(cUnit, kThumb2Bfi, r2, r1, 0, LW_LOCK_OWNER_SHIFT - 1);
  newLIR3(cUnit, kThumb2Bfc, r1, LW_HASH_STATE_SHIFT, LW_LOCK_OWNER_SHIFT - 1);
  opRegImm(cUnit, kOpCmp, r1, 0);
  opIT(cUnit, kArmCondEq, "");
  newLIR4(cUnit, kThumb2Strex, r1, r2, r0,
          Object::MonitorOffset().Int32Value() >> 2);
  opRegImm(cUnit, kOpCmp, r1, 0);
  opIT(cUnit, kArmCondNe, "T");
  // Go expensive route - artLockObjectFromCode(self, obj);
  loadWordDisp(cUnit, rARM_SELF, ENTRYPOINT_OFFSET(pLockObjectFromCode), rARM_LR);
  oatClobberCalleeSave(cUnit);
  LIR* callInst = opReg(cUnit, kOpBlx, rARM_LR);
  markSafepointPC(cUnit, callInst);
  oatGenMemBarrier(cUnit, kSY);
}

/*
 * For monitor unlock, we don't have to use ldrex/strex.  Once
 * we've determined that the lock is thin and that we own it with
 * a zero recursion count, it's safe to punch it back to the
 * initial, unlock thin state with a store word.
 */
void genMonitorExit(CompilationUnit* cUnit, int optFlags, RegLocation rlSrc)
{
  DCHECK_EQ(LW_SHAPE_THIN, 0);
  oatFlushAllRegs(cUnit);
  loadValueDirectFixed(cUnit, rlSrc, r0);  // Get obj
  oatLockCallTemps(cUnit);  // Prepare for explicit register usage
  genNullCheck(cUnit, rlSrc.sRegLow, r0, optFlags);
  loadWordDisp(cUnit, r0, Object::MonitorOffset().Int32Value(), r1); // Get lock
  loadWordDisp(cUnit, rARM_SELF, Thread::ThinLockIdOffset().Int32Value(), r2);
  // Is lock unheld on lock or held by us (==threadId) on unlock?
  opRegRegImm(cUnit, kOpAnd, r3, r1,
              (LW_HASH_STATE_MASK << LW_HASH_STATE_SHIFT));
  // Align owner
  opRegImm(cUnit, kOpLsl, r2, LW_LOCK_OWNER_SHIFT);
  newLIR3(cUnit, kThumb2Bfc, r1, LW_HASH_STATE_SHIFT, LW_LOCK_OWNER_SHIFT - 1);
  opRegReg(cUnit, kOpSub, r1, r2);
  opIT(cUnit, kArmCondEq, "EE");
  storeWordDisp(cUnit, r0, Object::MonitorOffset().Int32Value(), r3);
  // Go expensive route - UnlockObjectFromCode(obj);
  loadWordDisp(cUnit, rARM_SELF, ENTRYPOINT_OFFSET(pUnlockObjectFromCode), rARM_LR);
  oatClobberCalleeSave(cUnit);
  LIR* callInst = opReg(cUnit, kOpBlx, rARM_LR);
  markSafepointPC(cUnit, callInst);
  oatGenMemBarrier(cUnit, kSY);
}

/*
 * Mark garbage collection card. Skip if the value we're storing is null.
 */
void markGCCard(CompilationUnit* cUnit, int valReg, int tgtAddrReg)
{
  int regCardBase = oatAllocTemp(cUnit);
  int regCardNo = oatAllocTemp(cUnit);
  LIR* branchOver = opCmpImmBranch(cUnit, kCondEq, valReg, 0, NULL);
  loadWordDisp(cUnit, rARM_SELF, Thread::CardTableOffset().Int32Value(), regCardBase);
  opRegRegImm(cUnit, kOpLsr, regCardNo, tgtAddrReg, CardTable::kCardShift);
  storeBaseIndexed(cUnit, regCardBase, regCardNo, regCardBase, 0,
                   kUnsignedByte);
  LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
  branchOver->target = (LIR*)target;
  oatFreeTemp(cUnit, regCardBase);
  oatFreeTemp(cUnit, regCardNo);
}

void genEntrySequence(CompilationUnit* cUnit, RegLocation* argLocs,
                      RegLocation rlMethod)
{
  int spillCount = cUnit->numCoreSpills + cUnit->numFPSpills;
  /*
   * On entry, r0, r1, r2 & r3 are live.  Let the register allocation
   * mechanism know so it doesn't try to use any of them when
   * expanding the frame or flushing.  This leaves the utility
   * code with a single temp: r12.  This should be enough.
   */
  oatLockTemp(cUnit, r0);
  oatLockTemp(cUnit, r1);
  oatLockTemp(cUnit, r2);
  oatLockTemp(cUnit, r3);

  /*
   * We can safely skip the stack overflow check if we're
   * a leaf *and* our frame size < fudge factor.
   */
  bool skipOverflowCheck = ((cUnit->attrs & METHOD_IS_LEAF) &&
                            ((size_t)cUnit->frameSize <
                            Thread::kStackOverflowReservedBytes));
  newLIR0(cUnit, kPseudoMethodEntry);
  if (!skipOverflowCheck) {
    /* Load stack limit */
    loadWordDisp(cUnit, rARM_SELF, Thread::StackEndOffset().Int32Value(), r12);
  }
  /* Spill core callee saves */
  newLIR1(cUnit, kThumb2Push, cUnit->coreSpillMask);
  /* Need to spill any FP regs? */
  if (cUnit->numFPSpills) {
    /*
     * NOTE: fp spills are a little different from core spills in that
     * they are pushed as a contiguous block.  When promoting from
     * the fp set, we must allocate all singles from s16..highest-promoted
     */
    newLIR1(cUnit, kThumb2VPushCS, cUnit->numFPSpills);
  }
  if (!skipOverflowCheck) {
    opRegRegImm(cUnit, kOpSub, rARM_LR, rARM_SP, cUnit->frameSize - (spillCount * 4));
    genRegRegCheck(cUnit, kCondCc, rARM_LR, r12, kThrowStackOverflow);
    opRegCopy(cUnit, rARM_SP, rARM_LR);     // Establish stack
  } else {
    opRegImm(cUnit, kOpSub, rARM_SP, cUnit->frameSize - (spillCount * 4));
  }

  flushIns(cUnit, argLocs, rlMethod);

  oatFreeTemp(cUnit, r0);
  oatFreeTemp(cUnit, r1);
  oatFreeTemp(cUnit, r2);
  oatFreeTemp(cUnit, r3);
}

void genExitSequence(CompilationUnit* cUnit)
{
  int spillCount = cUnit->numCoreSpills + cUnit->numFPSpills;
  /*
   * In the exit path, r0/r1 are live - make sure they aren't
   * allocated by the register utilities as temps.
   */
  oatLockTemp(cUnit, r0);
  oatLockTemp(cUnit, r1);

  newLIR0(cUnit, kPseudoMethodExit);
  opRegImm(cUnit, kOpAdd, rARM_SP, cUnit->frameSize - (spillCount * 4));
  /* Need to restore any FP callee saves? */
  if (cUnit->numFPSpills) {
    newLIR1(cUnit, kThumb2VPopCS, cUnit->numFPSpills);
  }
  if (cUnit->coreSpillMask & (1 << rARM_LR)) {
    /* Unspill rARM_LR to rARM_PC */
    cUnit->coreSpillMask &= ~(1 << rARM_LR);
    cUnit->coreSpillMask |= (1 << rARM_PC);
  }
  newLIR1(cUnit, kThumb2Pop, cUnit->coreSpillMask);
  if (!(cUnit->coreSpillMask & (1 << rARM_PC))) {
    /* We didn't pop to rARM_PC, so must do a bv rARM_LR */
    newLIR1(cUnit, kThumbBx, rARM_LR);
  }
}

}  // namespace art
