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

#include "oat/runtime/oat_support_entrypoints.h"
#include "mips_lir.h"
#include "../codegen_util.h"
#include "../ralloc_util.h"

namespace art {

void genSpecialCase(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                    SpecialCaseHandler specialCase)
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
 *   ori   rEnd, r_ZERO, #tableSize  ; size in bytes
 *   jal   BaseLabel         ; stores "return address" (BaseLabel) in r_RA
 *   nop                     ; opportunistically fill
 * BaseLabel:
 *   addiu rBase, r_RA, <table> - <BaseLabel>  ; table relative to BaseLabel
     addu  rEnd, rEnd, rBase                   ; end of table
 *   lw    rVal, [rSP, vRegOff]                ; Test Value
 * loop:
 *   beq   rBase, rEnd, done
 *   lw    rKey, 0(rBase)
 *   addu  rBase, 8
 *   bne   rVal, rKey, loop
 *   lw    rDisp, -4(rBase)
 *   addu  r_RA, rDisp
 *   jr    r_RA
 * done:
 *
 */
void genSparseSwitch(CompilationUnit* cUnit, uint32_t tableOffset,
                     RegLocation rlSrc)
{
  const uint16_t* table = cUnit->insns + cUnit->currentDalvikOffset + tableOffset;
  if (cUnit->printMe) {
    dumpSparseSwitchTable(table);
  }
  // Add the table to the list - we'll process it later
  SwitchTable *tabRec =
      static_cast<SwitchTable*>(oatNew(cUnit, sizeof(SwitchTable), true, kAllocData));
  tabRec->table = table;
  tabRec->vaddr = cUnit->currentDalvikOffset;
  int elements = table[1];
  tabRec->targets =
      static_cast<LIR**>(oatNew(cUnit, elements * sizeof(LIR*), true, kAllocLIR));
  oatInsertGrowableList(cUnit, &cUnit->switchTables, reinterpret_cast<uintptr_t>(tabRec));

  // The table is composed of 8-byte key/disp pairs
  int byteSize = elements * 8;

  int sizeHi = byteSize >> 16;
  int sizeLo = byteSize & 0xffff;

  int rEnd = oatAllocTemp(cUnit);
  if (sizeHi) {
    newLIR2(cUnit, kMipsLui, rEnd, sizeHi);
  }
  // Must prevent code motion for the curr pc pair
  genBarrier(cUnit);  // Scheduling barrier
  newLIR0(cUnit, kMipsCurrPC);  // Really a jal to .+8
  // Now, fill the branch delay slot
  if (sizeHi) {
    newLIR3(cUnit, kMipsOri, rEnd, rEnd, sizeLo);
  } else {
    newLIR3(cUnit, kMipsOri, rEnd, r_ZERO, sizeLo);
  }
  genBarrier(cUnit);  // Scheduling barrier

  // Construct BaseLabel and set up table base register
  LIR* baseLabel = newLIR0(cUnit, kPseudoTargetLabel);
  // Remember base label so offsets can be computed later
  tabRec->anchor = baseLabel;
  int rBase = oatAllocTemp(cUnit);
  newLIR4(cUnit, kMipsDelta, rBase, 0, reinterpret_cast<uintptr_t>(baseLabel),
          reinterpret_cast<uintptr_t>(tabRec));
  opRegRegReg(cUnit, kOpAdd, rEnd, rEnd, rBase);

  // Grab switch test value
  rlSrc = loadValue(cUnit, rlSrc, kCoreReg);

  // Test loop
  int rKey = oatAllocTemp(cUnit);
  LIR* loopLabel = newLIR0(cUnit, kPseudoTargetLabel);
  LIR* exitBranch = opCmpBranch(cUnit , kCondEq, rBase, rEnd, NULL);
  loadWordDisp(cUnit, rBase, 0, rKey);
  opRegImm(cUnit, kOpAdd, rBase, 8);
  opCmpBranch(cUnit, kCondNe, rlSrc.lowReg, rKey, loopLabel);
  int rDisp = oatAllocTemp(cUnit);
  loadWordDisp(cUnit, rBase, -4, rDisp);
  opRegRegReg(cUnit, kOpAdd, r_RA, r_RA, rDisp);
  opReg(cUnit, kOpBx, r_RA);

  // Loop exit
  LIR* exitLabel = newLIR0(cUnit, kPseudoTargetLabel);
  exitBranch->target = exitLabel;
}

/*
 * Code pattern will look something like:
 *
 *   lw    rVal
 *   jal   BaseLabel         ; stores "return address" (BaseLabel) in r_RA
 *   nop                     ; opportunistically fill
 *   [subiu rVal, bias]      ; Remove bias if lowVal != 0
 *   bound check -> done
 *   lw    rDisp, [r_RA, rVal]
 *   addu  r_RA, rDisp
 *   jr    r_RA
 * done:
 */
void genPackedSwitch(CompilationUnit* cUnit, uint32_t tableOffset,
                     RegLocation rlSrc)
{
  const uint16_t* table = cUnit->insns + cUnit->currentDalvikOffset + tableOffset;
  if (cUnit->printMe) {
    dumpPackedSwitchTable(table);
  }
  // Add the table to the list - we'll process it later
  SwitchTable *tabRec =
      static_cast<SwitchTable*>(oatNew(cUnit, sizeof(SwitchTable), true, kAllocData));
  tabRec->table = table;
  tabRec->vaddr = cUnit->currentDalvikOffset;
  int size = table[1];
  tabRec->targets = static_cast<LIR**>(oatNew(cUnit, size * sizeof(LIR*), true, kAllocLIR));
  oatInsertGrowableList(cUnit, &cUnit->switchTables, reinterpret_cast<uintptr_t>(tabRec));

  // Get the switch value
  rlSrc = loadValue(cUnit, rlSrc, kCoreReg);

  // Prepare the bias.  If too big, handle 1st stage here
  int lowKey = s4FromSwitchData(&table[2]);
  bool largeBias = false;
  int rKey;
  if (lowKey == 0) {
    rKey = rlSrc.lowReg;
  } else if ((lowKey & 0xffff) != lowKey) {
    rKey = oatAllocTemp(cUnit);
    loadConstant(cUnit, rKey, lowKey);
    largeBias = true;
  } else {
    rKey = oatAllocTemp(cUnit);
  }

  // Must prevent code motion for the curr pc pair
  genBarrier(cUnit);
  newLIR0(cUnit, kMipsCurrPC);  // Really a jal to .+8
  // Now, fill the branch delay slot with bias strip
  if (lowKey == 0) {
    newLIR0(cUnit, kMipsNop);
  } else {
    if (largeBias) {
      opRegRegReg(cUnit, kOpSub, rKey, rlSrc.lowReg, rKey);
    } else {
      opRegRegImm(cUnit, kOpSub, rKey, rlSrc.lowReg, lowKey);
    }
  }
  genBarrier(cUnit);  // Scheduling barrier

  // Construct BaseLabel and set up table base register
  LIR* baseLabel = newLIR0(cUnit, kPseudoTargetLabel);
  // Remember base label so offsets can be computed later
  tabRec->anchor = baseLabel;

  // Bounds check - if < 0 or >= size continue following switch
  LIR* branchOver = opCmpImmBranch(cUnit, kCondHi, rKey, size-1, NULL);

  // Materialize the table base pointer
  int rBase = oatAllocTemp(cUnit);
  newLIR4(cUnit, kMipsDelta, rBase, 0, reinterpret_cast<uintptr_t>(baseLabel),
          reinterpret_cast<uintptr_t>(tabRec));

  // Load the displacement from the switch table
  int rDisp = oatAllocTemp(cUnit);
  loadBaseIndexed(cUnit, rBase, rKey, rDisp, 2, kWord);

  // Add to r_AP and go
  opRegRegReg(cUnit, kOpAdd, r_RA, r_RA, rDisp);
  opReg(cUnit, kOpBx, r_RA);

  /* branchOver target here */
  LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
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
void genFillArrayData(CompilationUnit* cUnit, uint32_t tableOffset,
                      RegLocation rlSrc)
{
  const uint16_t* table = cUnit->insns + cUnit->currentDalvikOffset + tableOffset;
  // Add the table to the list - we'll process it later
  FillArrayData *tabRec =
      reinterpret_cast<FillArrayData*>(oatNew(cUnit, sizeof(FillArrayData), true, kAllocData));
  tabRec->table = table;
  tabRec->vaddr = cUnit->currentDalvikOffset;
  uint16_t width = tabRec->table[1];
  uint32_t size = tabRec->table[2] | ((static_cast<uint32_t>(tabRec->table[3])) << 16);
  tabRec->size = (size * width) + 8;

  oatInsertGrowableList(cUnit, &cUnit->fillArrayData, reinterpret_cast<uintptr_t>(tabRec));

  // Making a call - use explicit registers
  oatFlushAllRegs(cUnit);   /* Everything to home location */
  oatLockCallTemps(cUnit);
  loadValueDirectFixed(cUnit, rlSrc, rMIPS_ARG0);

  // Must prevent code motion for the curr pc pair
  genBarrier(cUnit);
  newLIR0(cUnit, kMipsCurrPC);  // Really a jal to .+8
  // Now, fill the branch delay slot with the helper load
  int rTgt = loadHelper(cUnit, ENTRYPOINT_OFFSET(pHandleFillArrayDataFromCode));
  genBarrier(cUnit);  // Scheduling barrier

  // Construct BaseLabel and set up table base register
  LIR* baseLabel = newLIR0(cUnit, kPseudoTargetLabel);

  // Materialize a pointer to the fill data image
  newLIR4(cUnit, kMipsDelta, rMIPS_ARG1, 0, reinterpret_cast<uintptr_t>(baseLabel),
          reinterpret_cast<uintptr_t>(tabRec));

  // And go...
  oatClobberCalleeSave(cUnit);
  LIR* callInst = opReg(cUnit, kOpBlx, rTgt); // ( array*, fill_data* )
  markSafepointPC(cUnit, callInst);
}

/*
 * TODO: implement fast path to short-circuit thin-lock case
 */
void genMonitorEnter(CompilationUnit* cUnit, int optFlags, RegLocation rlSrc)
{
  oatFlushAllRegs(cUnit);
  loadValueDirectFixed(cUnit, rlSrc, rMIPS_ARG0);  // Get obj
  oatLockCallTemps(cUnit);  // Prepare for explicit register usage
  genNullCheck(cUnit, rlSrc.sRegLow, rMIPS_ARG0, optFlags);
  // Go expensive route - artLockObjectFromCode(self, obj);
  int rTgt = loadHelper(cUnit, ENTRYPOINT_OFFSET(pLockObjectFromCode));
  oatClobberCalleeSave(cUnit);
  LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
  markSafepointPC(cUnit, callInst);
}

/*
 * TODO: implement fast path to short-circuit thin-lock case
 */
void genMonitorExit(CompilationUnit* cUnit, int optFlags, RegLocation rlSrc)
{
  oatFlushAllRegs(cUnit);
  loadValueDirectFixed(cUnit, rlSrc, rMIPS_ARG0);  // Get obj
  oatLockCallTemps(cUnit);  // Prepare for explicit register usage
  genNullCheck(cUnit, rlSrc.sRegLow, rMIPS_ARG0, optFlags);
  // Go expensive route - UnlockObjectFromCode(obj);
  int rTgt = loadHelper(cUnit, ENTRYPOINT_OFFSET(pUnlockObjectFromCode));
  oatClobberCalleeSave(cUnit);
  LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
  markSafepointPC(cUnit, callInst);
}

/*
 * Mark garbage collection card. Skip if the value we're storing is null.
 */
void markGCCard(CompilationUnit* cUnit, int valReg, int tgtAddrReg)
{
  int regCardBase = oatAllocTemp(cUnit);
  int regCardNo = oatAllocTemp(cUnit);
  LIR* branchOver = opCmpImmBranch(cUnit, kCondEq, valReg, 0, NULL);
  loadWordDisp(cUnit, rMIPS_SELF, Thread::CardTableOffset().Int32Value(), regCardBase);
  opRegRegImm(cUnit, kOpLsr, regCardNo, tgtAddrReg, CardTable::kCardShift);
  storeBaseIndexed(cUnit, regCardBase, regCardNo, regCardBase, 0,
                   kUnsignedByte);
  LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
  branchOver->target = target;
  oatFreeTemp(cUnit, regCardBase);
  oatFreeTemp(cUnit, regCardNo);
}
void genEntrySequence(CompilationUnit* cUnit, RegLocation* argLocs,
                      RegLocation rlMethod)
{
  int spillCount = cUnit->numCoreSpills + cUnit->numFPSpills;
  /*
   * On entry, rMIPS_ARG0, rMIPS_ARG1, rMIPS_ARG2 & rMIPS_ARG3 are live.  Let the register
   * allocation mechanism know so it doesn't try to use any of them when
   * expanding the frame or flushing.  This leaves the utility
   * code with a single temp: r12.  This should be enough.
   */
  oatLockTemp(cUnit, rMIPS_ARG0);
  oatLockTemp(cUnit, rMIPS_ARG1);
  oatLockTemp(cUnit, rMIPS_ARG2);
  oatLockTemp(cUnit, rMIPS_ARG3);

  /*
   * We can safely skip the stack overflow check if we're
   * a leaf *and* our frame size < fudge factor.
   */
  bool skipOverflowCheck = ((cUnit->attrs & METHOD_IS_LEAF) &&
      (static_cast<size_t>(cUnit->frameSize) < Thread::kStackOverflowReservedBytes));
  newLIR0(cUnit, kPseudoMethodEntry);
  int checkReg = oatAllocTemp(cUnit);
  int newSP = oatAllocTemp(cUnit);
  if (!skipOverflowCheck) {
    /* Load stack limit */
    loadWordDisp(cUnit, rMIPS_SELF, Thread::StackEndOffset().Int32Value(), checkReg);
  }
  /* Spill core callee saves */
  spillCoreRegs(cUnit);
  /* NOTE: promotion of FP regs currently unsupported, thus no FP spill */
  DCHECK_EQ(cUnit->numFPSpills, 0);
  if (!skipOverflowCheck) {
    opRegRegImm(cUnit, kOpSub, newSP, rMIPS_SP, cUnit->frameSize - (spillCount * 4));
    genRegRegCheck(cUnit, kCondCc, newSP, checkReg, kThrowStackOverflow);
    opRegCopy(cUnit, rMIPS_SP, newSP);     // Establish stack
  } else {
    opRegImm(cUnit, kOpSub, rMIPS_SP, cUnit->frameSize - (spillCount * 4));
  }

  flushIns(cUnit, argLocs, rlMethod);

  oatFreeTemp(cUnit, rMIPS_ARG0);
  oatFreeTemp(cUnit, rMIPS_ARG1);
  oatFreeTemp(cUnit, rMIPS_ARG2);
  oatFreeTemp(cUnit, rMIPS_ARG3);
}

void genExitSequence(CompilationUnit* cUnit)
{
  /*
   * In the exit path, rMIPS_RET0/rMIPS_RET1 are live - make sure they aren't
   * allocated by the register utilities as temps.
   */
  oatLockTemp(cUnit, rMIPS_RET0);
  oatLockTemp(cUnit, rMIPS_RET1);

  newLIR0(cUnit, kPseudoMethodExit);
  unSpillCoreRegs(cUnit);
  opReg(cUnit, kOpBx, r_RA);
}

}  // namespace art
