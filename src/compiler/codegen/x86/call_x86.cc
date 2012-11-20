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

namespace art {

void genSpecialCase(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                    SpecialCaseHandler specialCase)
{
  // TODO
}

/*
 * The sparse table in the literal pool is an array of <key,displacement>
 * pairs.
 */
BasicBlock *findBlock(CompilationUnit* cUnit, unsigned int codeOffset,
                      bool split, bool create, BasicBlock** immedPredBlockP);
void genSparseSwitch(CompilationUnit* cUnit, uint32_t tableOffset,
                     RegLocation rlSrc)
{
  const uint16_t* table = cUnit->insns + cUnit->currentDalvikOffset + tableOffset;
  if (cUnit->printMe) {
    dumpSparseSwitchTable(table);
  }
  int entries = table[1];
  const int* keys = reinterpret_cast<const int*>(&table[2]);
  const int* targets = &keys[entries];
  rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
  for (int i = 0; i < entries; i++) {
    int key = keys[i];
    BasicBlock* case_block = findBlock(cUnit,
                                       cUnit->currentDalvikOffset + targets[i],
                                       false, false, NULL);
    LIR* labelList = cUnit->blockLabelList;
    opCmpImmBranch(cUnit, kCondEq, rlSrc.lowReg, key,
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
void genPackedSwitch(CompilationUnit* cUnit, uint32_t tableOffset,
                     RegLocation rlSrc)
{
  const uint16_t* table = cUnit->insns + cUnit->currentDalvikOffset + tableOffset;
  if (cUnit->printMe) {
    dumpPackedSwitchTable(table);
  }
  // Add the table to the list - we'll process it later
  SwitchTable *tabRec =
      static_cast<SwitchTable *>(oatNew(cUnit, sizeof(SwitchTable), true, kAllocData));
  tabRec->table = table;
  tabRec->vaddr = cUnit->currentDalvikOffset;
  int size = table[1];
  tabRec->targets = static_cast<LIR**>(oatNew(cUnit, size * sizeof(LIR*), true, kAllocLIR));
  oatInsertGrowableList(cUnit, &cUnit->switchTables, reinterpret_cast<uintptr_t>(tabRec));

  // Get the switch value
  rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
  int startOfMethodReg = oatAllocTemp(cUnit);
  // Materialize a pointer to the switch table
  //newLIR0(cUnit, kX86Bkpt);
  newLIR1(cUnit, kX86StartOfMethod, startOfMethodReg);
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
  newLIR5(cUnit, kX86PcRelLoadRA, dispReg, startOfMethodReg, keyReg, 2,
          reinterpret_cast<uintptr_t>(tabRec));
  // Add displacement to start of method
  opRegReg(cUnit, kOpAdd, startOfMethodReg, dispReg);
  // ..and go!
  LIR* switchBranch = newLIR1(cUnit, kX86JmpR, startOfMethodReg);
  tabRec->anchor = switchBranch;

  /* branchOver target here */
  LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
  branchOver->target = target;
}

void callRuntimeHelperRegReg(CompilationUnit* cUnit, int helperOffset,
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
void genFillArrayData(CompilationUnit* cUnit, uint32_t tableOffset,
                      RegLocation rlSrc)
{
  const uint16_t* table = cUnit->insns + cUnit->currentDalvikOffset + tableOffset;
  // Add the table to the list - we'll process it later
  FillArrayData *tabRec =
      static_cast<FillArrayData*>(oatNew(cUnit, sizeof(FillArrayData), true, kAllocData));
  tabRec->table = table;
  tabRec->vaddr = cUnit->currentDalvikOffset;
  uint16_t width = tabRec->table[1];
  uint32_t size = tabRec->table[2] | ((static_cast<uint32_t>(tabRec->table[3])) << 16);
  tabRec->size = (size * width) + 8;

  oatInsertGrowableList(cUnit, &cUnit->fillArrayData, reinterpret_cast<uintptr_t>(tabRec));

  // Making a call - use explicit registers
  oatFlushAllRegs(cUnit);   /* Everything to home location */
  loadValueDirectFixed(cUnit, rlSrc, rX86_ARG0);
  // Materialize a pointer to the fill data image
  newLIR1(cUnit, kX86StartOfMethod, rX86_ARG2);
  newLIR2(cUnit, kX86PcRelAdr, rX86_ARG1, reinterpret_cast<uintptr_t>(tabRec));
  newLIR2(cUnit, kX86Add32RR, rX86_ARG1, rX86_ARG2);
  callRuntimeHelperRegReg(cUnit, ENTRYPOINT_OFFSET(pHandleFillArrayDataFromCode), rX86_ARG0,
                          rX86_ARG1, true);
}

void genMonitorEnter(CompilationUnit* cUnit, int optFlags, RegLocation rlSrc)
{
  oatFlushAllRegs(cUnit);
  loadValueDirectFixed(cUnit, rlSrc, rCX);  // Get obj
  oatLockCallTemps(cUnit);  // Prepare for explicit register usage
  genNullCheck(cUnit, rlSrc.sRegLow, rCX, optFlags);
  // If lock is unheld, try to grab it quickly with compare and exchange
  // TODO: copy and clear hash state?
  newLIR2(cUnit, kX86Mov32RT, rDX, Thread::ThinLockIdOffset().Int32Value());
  newLIR2(cUnit, kX86Sal32RI, rDX, LW_LOCK_OWNER_SHIFT);
  newLIR2(cUnit, kX86Xor32RR, rAX, rAX);
  newLIR3(cUnit, kX86LockCmpxchgMR, rCX, Object::MonitorOffset().Int32Value(), rDX);
  LIR* branch = newLIR2(cUnit, kX86Jcc8, 0, kX86CondEq);
  // If lock is held, go the expensive route - artLockObjectFromCode(self, obj);
  callRuntimeHelperReg(cUnit, ENTRYPOINT_OFFSET(pLockObjectFromCode), rCX, true);
  branch->target = newLIR0(cUnit, kPseudoTargetLabel);
}

void genMonitorExit(CompilationUnit* cUnit, int optFlags, RegLocation rlSrc)
{
  oatFlushAllRegs(cUnit);
  loadValueDirectFixed(cUnit, rlSrc, rAX);  // Get obj
  oatLockCallTemps(cUnit);  // Prepare for explicit register usage
  genNullCheck(cUnit, rlSrc.sRegLow, rAX, optFlags);
  // If lock is held by the current thread, clear it to quickly release it
  // TODO: clear hash state?
  newLIR2(cUnit, kX86Mov32RT, rDX, Thread::ThinLockIdOffset().Int32Value());
  newLIR2(cUnit, kX86Sal32RI, rDX, LW_LOCK_OWNER_SHIFT);
  newLIR3(cUnit, kX86Mov32RM, rCX, rAX, Object::MonitorOffset().Int32Value());
  opRegReg(cUnit, kOpSub, rCX, rDX);
  LIR* branch = newLIR2(cUnit, kX86Jcc8, 0, kX86CondNe);
  newLIR3(cUnit, kX86Mov32MR, rAX, Object::MonitorOffset().Int32Value(), rCX);
  LIR* branch2 = newLIR1(cUnit, kX86Jmp8, 0);
  branch->target = newLIR0(cUnit, kPseudoTargetLabel);
  // Otherwise, go the expensive route - UnlockObjectFromCode(obj);
  callRuntimeHelperReg(cUnit, ENTRYPOINT_OFFSET(pUnlockObjectFromCode), rAX, true);
  branch2->target = newLIR0(cUnit, kPseudoTargetLabel);
}

/*
 * Mark garbage collection card. Skip if the value we're storing is null.
 */
void markGCCard(CompilationUnit* cUnit, int valReg, int tgtAddrReg)
{
  int regCardBase = oatAllocTemp(cUnit);
  int regCardNo = oatAllocTemp(cUnit);
  LIR* branchOver = opCmpImmBranch(cUnit, kCondEq, valReg, 0, NULL);
  newLIR2(cUnit, kX86Mov32RT, regCardBase, Thread::CardTableOffset().Int32Value());
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
  /*
   * On entry, rX86_ARG0, rX86_ARG1, rX86_ARG2 are live.  Let the register
   * allocation mechanism know so it doesn't try to use any of them when
   * expanding the frame or flushing.  This leaves the utility
   * code with no spare temps.
   */
  oatLockTemp(cUnit, rX86_ARG0);
  oatLockTemp(cUnit, rX86_ARG1);
  oatLockTemp(cUnit, rX86_ARG2);

  /* Build frame, return address already on stack */
  opRegImm(cUnit, kOpSub, rX86_SP, cUnit->frameSize - 4);

  /*
   * We can safely skip the stack overflow check if we're
   * a leaf *and* our frame size < fudge factor.
   */
  bool skipOverflowCheck = ((cUnit->attrs & METHOD_IS_LEAF) &&
                (static_cast<size_t>(cUnit->frameSize) <
                Thread::kStackOverflowReservedBytes));
  newLIR0(cUnit, kPseudoMethodEntry);
  /* Spill core callee saves */
  spillCoreRegs(cUnit);
  /* NOTE: promotion of FP regs currently unsupported, thus no FP spill */
  DCHECK_EQ(cUnit->numFPSpills, 0);
  if (!skipOverflowCheck) {
    // cmp rX86_SP, fs:[stack_end_]; jcc throw_launchpad
    LIR* tgt = rawLIR(cUnit, 0, kPseudoThrowTarget, kThrowStackOverflow, 0, 0, 0, 0);
    opRegThreadMem(cUnit, kOpCmp, rX86_SP, Thread::StackEndOffset().Int32Value());
    opCondBranch(cUnit, kCondUlt, tgt);
    // Remember branch target - will process later
    oatInsertGrowableList(cUnit, &cUnit->throwLaunchpads, reinterpret_cast<uintptr_t>(tgt));
  }

  flushIns(cUnit, argLocs, rlMethod);

  oatFreeTemp(cUnit, rX86_ARG0);
  oatFreeTemp(cUnit, rX86_ARG1);
  oatFreeTemp(cUnit, rX86_ARG2);
}

void genExitSequence(CompilationUnit* cUnit) {
  /*
   * In the exit path, rX86_RET0/rX86_RET1 are live - make sure they aren't
   * allocated by the register utilities as temps.
   */
  oatLockTemp(cUnit, rX86_RET0);
  oatLockTemp(cUnit, rX86_RET1);

  newLIR0(cUnit, kPseudoMethodExit);
  unSpillCoreRegs(cUnit);
  /* Remove frame except for return address */
  opRegImm(cUnit, kOpAdd, rX86_SP, cUnit->frameSize - 4);
  newLIR0(cUnit, kX86Ret);
}

}  // namespace art
