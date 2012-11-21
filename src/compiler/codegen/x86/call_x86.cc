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

#include "x86_lir.h"
#include "../codegen_util.h"
#include "../ralloc_util.h"

namespace art {

void GenSpecialCase(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                    SpecialCaseHandler specialCase)
{
  // TODO
}

/*
 * The sparse table in the literal pool is an array of <key,displacement>
 * pairs.
 */
BasicBlock *FindBlock(CompilationUnit* cUnit, unsigned int codeOffset,
                      bool split, bool create, BasicBlock** immedPredBlockP);
void GenSparseSwitch(CompilationUnit* cUnit, uint32_t tableOffset,
                     RegLocation rlSrc)
{
  const uint16_t* table = cUnit->insns + cUnit->currentDalvikOffset + tableOffset;
  if (cUnit->printMe) {
    DumpSparseSwitchTable(table);
  }
  int entries = table[1];
  const int* keys = reinterpret_cast<const int*>(&table[2]);
  const int* targets = &keys[entries];
  rlSrc = LoadValue(cUnit, rlSrc, kCoreReg);
  for (int i = 0; i < entries; i++) {
    int key = keys[i];
    BasicBlock* case_block = FindBlock(cUnit,
                                       cUnit->currentDalvikOffset + targets[i],
                                       false, false, NULL);
    LIR* labelList = cUnit->blockLabelList;
    OpCmpImmBranch(cUnit, kCondEq, rlSrc.lowReg, key,
                   &labelList[case_block->id]);
  }
}

/*
 * Code pattern will look something like:
 *
 * mov  rVal, ..
 * call 0
 * pop  rStartOfMethod
 * sub  rStartOfMethod, ..
 * mov  rKeyReg, rVal
 * sub  rKeyReg, lowKey
 * cmp  rKeyReg, size-1  ; bound check
 * ja   done
 * mov  rDisp, [rStartOfMethod + rKeyReg * 4 + tableOffset]
 * add  rStartOfMethod, rDisp
 * jmp  rStartOfMethod
 * done:
 */
void GenPackedSwitch(CompilationUnit* cUnit, uint32_t tableOffset,
                     RegLocation rlSrc)
{
  const uint16_t* table = cUnit->insns + cUnit->currentDalvikOffset + tableOffset;
  if (cUnit->printMe) {
    DumpPackedSwitchTable(table);
  }
  // Add the table to the list - we'll process it later
  SwitchTable *tabRec =
      static_cast<SwitchTable *>(NewMem(cUnit, sizeof(SwitchTable), true, kAllocData));
  tabRec->table = table;
  tabRec->vaddr = cUnit->currentDalvikOffset;
  int size = table[1];
  tabRec->targets = static_cast<LIR**>(NewMem(cUnit, size * sizeof(LIR*), true, kAllocLIR));
  InsertGrowableList(cUnit, &cUnit->switchTables, reinterpret_cast<uintptr_t>(tabRec));

  // Get the switch value
  rlSrc = LoadValue(cUnit, rlSrc, kCoreReg);
  int startOfMethodReg = AllocTemp(cUnit);
  // Materialize a pointer to the switch table
  //NewLIR0(cUnit, kX86Bkpt);
  NewLIR1(cUnit, kX86StartOfMethod, startOfMethodReg);
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
  NewLIR5(cUnit, kX86PcRelLoadRA, dispReg, startOfMethodReg, keyReg, 2,
          reinterpret_cast<uintptr_t>(tabRec));
  // Add displacement to start of method
  OpRegReg(cUnit, kOpAdd, startOfMethodReg, dispReg);
  // ..and go!
  LIR* switchBranch = NewLIR1(cUnit, kX86JmpR, startOfMethodReg);
  tabRec->anchor = switchBranch;

  /* branchOver target here */
  LIR* target = NewLIR0(cUnit, kPseudoTargetLabel);
  branchOver->target = target;
}

void CallRuntimeHelperRegReg(CompilationUnit* cUnit, int helperOffset,
                             int arg0, int arg1, bool safepointPC);
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
void GenFillArrayData(CompilationUnit* cUnit, uint32_t tableOffset,
                      RegLocation rlSrc)
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
  LoadValueDirectFixed(cUnit, rlSrc, rX86_ARG0);
  // Materialize a pointer to the fill data image
  NewLIR1(cUnit, kX86StartOfMethod, rX86_ARG2);
  NewLIR2(cUnit, kX86PcRelAdr, rX86_ARG1, reinterpret_cast<uintptr_t>(tabRec));
  NewLIR2(cUnit, kX86Add32RR, rX86_ARG1, rX86_ARG2);
  CallRuntimeHelperRegReg(cUnit, ENTRYPOINT_OFFSET(pHandleFillArrayDataFromCode), rX86_ARG0,
                          rX86_ARG1, true);
}

void GenMonitorEnter(CompilationUnit* cUnit, int optFlags, RegLocation rlSrc)
{
  FlushAllRegs(cUnit);
  LoadValueDirectFixed(cUnit, rlSrc, rCX);  // Get obj
  LockCallTemps(cUnit);  // Prepare for explicit register usage
  GenNullCheck(cUnit, rlSrc.sRegLow, rCX, optFlags);
  // If lock is unheld, try to grab it quickly with compare and exchange
  // TODO: copy and clear hash state?
  NewLIR2(cUnit, kX86Mov32RT, rDX, Thread::ThinLockIdOffset().Int32Value());
  NewLIR2(cUnit, kX86Sal32RI, rDX, LW_LOCK_OWNER_SHIFT);
  NewLIR2(cUnit, kX86Xor32RR, rAX, rAX);
  NewLIR3(cUnit, kX86LockCmpxchgMR, rCX, Object::MonitorOffset().Int32Value(), rDX);
  LIR* branch = NewLIR2(cUnit, kX86Jcc8, 0, kX86CondEq);
  // If lock is held, go the expensive route - artLockObjectFromCode(self, obj);
  CallRuntimeHelperReg(cUnit, ENTRYPOINT_OFFSET(pLockObjectFromCode), rCX, true);
  branch->target = NewLIR0(cUnit, kPseudoTargetLabel);
}

void GenMonitorExit(CompilationUnit* cUnit, int optFlags, RegLocation rlSrc)
{
  FlushAllRegs(cUnit);
  LoadValueDirectFixed(cUnit, rlSrc, rAX);  // Get obj
  LockCallTemps(cUnit);  // Prepare for explicit register usage
  GenNullCheck(cUnit, rlSrc.sRegLow, rAX, optFlags);
  // If lock is held by the current thread, clear it to quickly release it
  // TODO: clear hash state?
  NewLIR2(cUnit, kX86Mov32RT, rDX, Thread::ThinLockIdOffset().Int32Value());
  NewLIR2(cUnit, kX86Sal32RI, rDX, LW_LOCK_OWNER_SHIFT);
  NewLIR3(cUnit, kX86Mov32RM, rCX, rAX, Object::MonitorOffset().Int32Value());
  OpRegReg(cUnit, kOpSub, rCX, rDX);
  LIR* branch = NewLIR2(cUnit, kX86Jcc8, 0, kX86CondNe);
  NewLIR3(cUnit, kX86Mov32MR, rAX, Object::MonitorOffset().Int32Value(), rCX);
  LIR* branch2 = NewLIR1(cUnit, kX86Jmp8, 0);
  branch->target = NewLIR0(cUnit, kPseudoTargetLabel);
  // Otherwise, go the expensive route - UnlockObjectFromCode(obj);
  CallRuntimeHelperReg(cUnit, ENTRYPOINT_OFFSET(pUnlockObjectFromCode), rAX, true);
  branch2->target = NewLIR0(cUnit, kPseudoTargetLabel);
}

/*
 * Mark garbage collection card. Skip if the value we're storing is null.
 */
void MarkGCCard(CompilationUnit* cUnit, int valReg, int tgtAddrReg)
{
  int regCardBase = AllocTemp(cUnit);
  int regCardNo = AllocTemp(cUnit);
  LIR* branchOver = OpCmpImmBranch(cUnit, kCondEq, valReg, 0, NULL);
  NewLIR2(cUnit, kX86Mov32RT, regCardBase, Thread::CardTableOffset().Int32Value());
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
  /*
   * On entry, rX86_ARG0, rX86_ARG1, rX86_ARG2 are live.  Let the register
   * allocation mechanism know so it doesn't try to use any of them when
   * expanding the frame or flushing.  This leaves the utility
   * code with no spare temps.
   */
  LockTemp(cUnit, rX86_ARG0);
  LockTemp(cUnit, rX86_ARG1);
  LockTemp(cUnit, rX86_ARG2);

  /* Build frame, return address already on stack */
  OpRegImm(cUnit, kOpSub, rX86_SP, cUnit->frameSize - 4);

  /*
   * We can safely skip the stack overflow check if we're
   * a leaf *and* our frame size < fudge factor.
   */
  bool skipOverflowCheck = ((cUnit->attrs & METHOD_IS_LEAF) &&
                (static_cast<size_t>(cUnit->frameSize) <
                Thread::kStackOverflowReservedBytes));
  NewLIR0(cUnit, kPseudoMethodEntry);
  /* Spill core callee saves */
  SpillCoreRegs(cUnit);
  /* NOTE: promotion of FP regs currently unsupported, thus no FP spill */
  DCHECK_EQ(cUnit->numFPSpills, 0);
  if (!skipOverflowCheck) {
    // cmp rX86_SP, fs:[stack_end_]; jcc throw_launchpad
    LIR* tgt = RawLIR(cUnit, 0, kPseudoThrowTarget, kThrowStackOverflow, 0, 0, 0, 0);
    OpRegThreadMem(cUnit, kOpCmp, rX86_SP, Thread::StackEndOffset().Int32Value());
    OpCondBranch(cUnit, kCondUlt, tgt);
    // Remember branch target - will process later
    InsertGrowableList(cUnit, &cUnit->throwLaunchpads, reinterpret_cast<uintptr_t>(tgt));
  }

  FlushIns(cUnit, ArgLocs, rlMethod);

  FreeTemp(cUnit, rX86_ARG0);
  FreeTemp(cUnit, rX86_ARG1);
  FreeTemp(cUnit, rX86_ARG2);
}

void GenExitSequence(CompilationUnit* cUnit) {
  /*
   * In the exit path, rX86_RET0/rX86_RET1 are live - make sure they aren't
   * allocated by the register utilities as temps.
   */
  LockTemp(cUnit, rX86_RET0);
  LockTemp(cUnit, rX86_RET1);

  NewLIR0(cUnit, kPseudoMethodExit);
  UnSpillCoreRegs(cUnit);
  /* Remove frame except for return address */
  OpRegImm(cUnit, kOpAdd, rX86_SP, cUnit->frameSize - 4);
  NewLIR0(cUnit, kX86Ret);
}

}  // namespace art
