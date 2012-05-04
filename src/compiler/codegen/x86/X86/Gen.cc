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

/*
 * This file contains codegen for the X86 ISA and is intended to be
 * includes by:
 *
 *        Codegen-$(TARGET_ARCH_VARIANT).c
 *
 */

namespace art {

void genSpecialCase(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                    SpecialCaseHandler specialCase)
{
  // TODO
}

/*
 * Perform register memory operation.
 */
LIR* genRegMemCheck(CompilationUnit* cUnit, ConditionCode cCode,
                    int reg1, int base, int offset, MIR* mir, ThrowKind kind)
{
  LIR* tgt = rawLIR(cUnit, 0, kPseudoThrowTarget, kind,
                    mir ? mir->offset : 0, reg1, base, offset);
  opRegMem(cUnit, kOpCmp, reg1, base, offset);
  LIR* branch = opCondBranch(cUnit, cCode, tgt);
  // Remember branch target - will process later
  oatInsertGrowableList(cUnit, &cUnit->throwLaunchpads, (intptr_t)tgt);
  return branch;
}

/*
 * The sparse table in the literal pool is an array of <key,displacement>
 * pairs.
 */
BasicBlock *findBlock(CompilationUnit* cUnit, unsigned int codeOffset,
                      bool split, bool create, BasicBlock** immedPredBlockP);
void genSparseSwitch(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc,
                     LIR* labelList)
{
  const u2* table = cUnit->insns + mir->offset + mir->dalvikInsn.vB;
  if (cUnit->printMe) {
    dumpSparseSwitchTable(table);
  }
  int entries = table[1];
  int* keys = (int*)&table[2];
  int* targets = &keys[entries];
  rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
  for (int i = 0; i < entries; i++) {
    int key = keys[i];
    BasicBlock* case_block = findBlock(cUnit, mir->offset + targets[i],
                                       false, false, NULL);
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
void genPackedSwitch(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc)
{
  const u2* table = cUnit->insns + mir->offset + mir->dalvikInsn.vB;
  if (cUnit->printMe) {
    dumpPackedSwitchTable(table);
  }
  // Add the table to the list - we'll process it later
  SwitchTable *tabRec = (SwitchTable *)oatNew(cUnit, sizeof(SwitchTable),
                                              true, kAllocData);
  tabRec->table = table;
  tabRec->vaddr = mir->offset;
  int size = table[1];
  tabRec->targets = (LIR* *)oatNew(cUnit, size * sizeof(LIR*), true,
                                   kAllocLIR);
  oatInsertGrowableList(cUnit, &cUnit->switchTables, (intptr_t)tabRec);

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
          (intptr_t)tabRec);
  // Add displacement to start of method
  opRegReg(cUnit, kOpAdd, startOfMethodReg, dispReg);
  // ..and go!
  LIR* switchBranch = newLIR1(cUnit, kX86JmpR, startOfMethodReg);
  tabRec->anchor = switchBranch;

  /* branchOver target here */
  LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
  branchOver->target = (LIR*)target;
}

void callRuntimeHelperRegReg(CompilationUnit* cUnit, int helperOffset,
                             int arg0, int arg1);
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
void genFillArrayData(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc)
{
  const u2* table = cUnit->insns + mir->offset + mir->dalvikInsn.vB;
  // Add the table to the list - we'll process it later
  FillArrayData *tabRec = (FillArrayData *)oatNew(cUnit, sizeof(FillArrayData),
      true, kAllocData);
  tabRec->table = table;
  tabRec->vaddr = mir->offset;
  u2 width = tabRec->table[1];
  u4 size = tabRec->table[2] | (((u4)tabRec->table[3]) << 16);
  tabRec->size = (size * width) + 8;

  oatInsertGrowableList(cUnit, &cUnit->fillArrayData, (intptr_t)tabRec);

  // Making a call - use explicit registers
  oatFlushAllRegs(cUnit);   /* Everything to home location */
  loadValueDirectFixed(cUnit, rlSrc, rARG0);
  // Materialize a pointer to the fill data image
  newLIR1(cUnit, kX86StartOfMethod, rARG2);
  newLIR2(cUnit, kX86PcRelAdr, rARG1, (intptr_t)tabRec);
  newLIR2(cUnit, kX86Add32RR, rARG1, rARG2);
  callRuntimeHelperRegReg(cUnit,
                          ENTRYPOINT_OFFSET(pHandleFillArrayDataFromCode),
                          rARG0, rARG1);
}

void genNegFloat(CompilationUnit *cUnit, RegLocation rlDest, RegLocation rlSrc)
{
  UNIMPLEMENTED(WARNING) << "genNegFloat "
                         << PrettyMethod(cUnit->method_idx, *cUnit->dex_file);
  newLIR0(cUnit, kX86Bkpt);
#if 0
  RegLocation rlResult;
  rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
  rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  opRegRegImm(cUnit, kOpAdd, rlResult.lowReg, rlSrc.lowReg, 0x80000000);
  storeValue(cUnit, rlDest, rlResult);
#endif
}

void genNegDouble(CompilationUnit *cUnit, RegLocation rlDest, RegLocation rlSrc)
{
  UNIMPLEMENTED(WARNING) << "genNegDouble"
                         << PrettyMethod(cUnit->method_idx, *cUnit->dex_file);
  newLIR0(cUnit, kX86Bkpt);
#if 0
  RegLocation rlResult;
  rlSrc = loadValueWide(cUnit, rlSrc, kCoreReg);
  rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  opRegRegImm(cUnit, kOpAdd, rlResult.highReg, rlSrc.highReg, 0x80000000);
  opRegCopy(cUnit, rlResult.lowReg, rlSrc.lowReg);
  storeValueWide(cUnit, rlDest, rlResult);
#endif
}

LIR* genNullCheck(CompilationUnit* cUnit, int sReg, int mReg, MIR* mir);
void callRuntimeHelperReg(CompilationUnit* cUnit, int helperOffset, int arg0);

/*
 * TODO: implement fast path to short-circuit thin-lock case
 */
void genMonitorEnter(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc)
{
  oatFlushAllRegs(cUnit);
  loadValueDirectFixed(cUnit, rlSrc, rARG0);  // Get obj
  oatLockCallTemps(cUnit);  // Prepare for explicit register usage
  genNullCheck(cUnit, rlSrc.sRegLow, rARG0, mir);
  // Go expensive route - artLockObjectFromCode(self, obj);
  callRuntimeHelperReg(cUnit, ENTRYPOINT_OFFSET(pLockObjectFromCode), rARG0);
}

/*
 * TODO: implement fast path to short-circuit thin-lock case
 */
void genMonitorExit(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc)
{
  oatFlushAllRegs(cUnit);
  loadValueDirectFixed(cUnit, rlSrc, rARG0);  // Get obj
  oatLockCallTemps(cUnit);  // Prepare for explicit register usage
  genNullCheck(cUnit, rlSrc.sRegLow, rARG0, mir);
  // Go expensive route - UnlockObjectFromCode(obj);
  callRuntimeHelperReg(cUnit, ENTRYPOINT_OFFSET(pUnlockObjectFromCode), rARG0);
}

/*
 * Compare two 64-bit values
 *    x = y     return  0
 *    x < y     return -1
 *    x > y     return  1
 *
 *    slt   t0,  x.hi, y.hi;        # (x.hi < y.hi) ? 1:0
 *    sgt   t1,  x.hi, y.hi;        # (y.hi > x.hi) ? 1:0
 *    subu  res, t0, t1             # res = -1:1:0 for [ < > = ]
 *    bnez  res, finish
 *    sltu  t0, x.lo, y.lo
 *    sgtu  r1, x.lo, y.lo
 *    subu  res, t0, t1
 * finish:
 *
 */
void genCmpLong(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  oatFlushAllRegs(cUnit);
  oatLockCallTemps(cUnit);  // Prepare for explicit register usage
  loadValueDirectWideFixed(cUnit, rlSrc1, r0, r1);
  loadValueDirectWideFixed(cUnit, rlSrc1, r2, r3);
  // Compute (r1:r0) = (r1:r0) - (r2:r3)
  opRegReg(cUnit, kOpSub, r0, r2);  // r0 = r0 - r2
  opRegReg(cUnit, kOpSbc, r1, r3);  // r1 = r1 - r3 - CF
  opRegReg(cUnit, kOpOr, r0, r1);   // r0 = high | low - sets ZF
  newLIR2(cUnit, kX86Set8R, r0, kX86CondNz);  // r0 = (r1:r0) != (r2:r3) ? 1 : 0
  newLIR2(cUnit, kX86Movzx8RR, r0, r0);
  opRegImm(cUnit, kOpAsr, r1, 31);  // r1 = high >> 31
  opRegReg(cUnit, kOpOr, r0, r1);   // r0 holds result
  RegLocation rlResult = LOC_C_RETURN;
  storeValue(cUnit, rlDest, rlResult);
}

X86ConditionCode oatX86ConditionEncoding(ConditionCode cond) {
  switch (cond) {
    case kCondEq: return kX86CondEq;
    case kCondNe: return kX86CondNe;
    case kCondCs: return kX86CondC;
    case kCondCc: return kX86CondNc;
    case kCondMi: return kX86CondS;
    case kCondPl: return kX86CondNs;
    case kCondVs: return kX86CondO;
    case kCondVc: return kX86CondNo;
    case kCondHi: return kX86CondA;
    case kCondLs: return kX86CondBe;
    case kCondGe: return kX86CondGe;
    case kCondLt: return kX86CondL;
    case kCondGt: return kX86CondG;
    case kCondLe: return kX86CondLe;
    case kCondAl:
    case kCondNv: LOG(FATAL) << "Should not reach here";
  }
  return kX86CondO;
}

LIR* opCmpBranch(CompilationUnit* cUnit, ConditionCode cond, int src1,
                 int src2, LIR* target)
{
  newLIR2(cUnit, kX86Cmp32RR, src1, src2);
  X86ConditionCode cc = oatX86ConditionEncoding(cond);
  LIR* branch = newLIR2(cUnit, kX86Jcc8, 0 /* lir operand for Jcc offset */ ,
                        cc);
  branch->target = target;
  return branch;
}

LIR* opCmpImmBranch(CompilationUnit* cUnit, ConditionCode cond, int reg,
                    int checkValue, LIR* target)
{
  if (false && (checkValue == 0) && (cond == kCondEq || cond == kCondNe)) {
    // TODO: when checkValue == 0 and reg is rCX, use the jcxz/nz opcode
    // newLIR2(cUnit, kX86Test32RR, reg, reg);
  } else {
    newLIR2(cUnit, kX86Cmp32RI, reg, checkValue);
  }
  X86ConditionCode cc = oatX86ConditionEncoding(cond);
  LIR* branch = newLIR2(cUnit, kX86Jcc8, 0 /* lir operand for Jcc offset */ , cc);
  branch->target = target;
  return branch;
}

LIR* opRegCopyNoInsert(CompilationUnit *cUnit, int rDest, int rSrc)
{
  if (FPREG(rDest) || FPREG(rSrc))
    return fpRegCopy(cUnit, rDest, rSrc);
  LIR* res = rawLIR(cUnit, cUnit->currentDalvikOffset, kX86Mov32RR,
                    rDest, rSrc);
  if (rDest == rSrc) {
    res->flags.isNop = true;
  }
  return res;
}

LIR* opRegCopy(CompilationUnit *cUnit, int rDest, int rSrc)
{
  LIR *res = opRegCopyNoInsert(cUnit, rDest, rSrc);
  oatAppendLIR(cUnit, res);
  return res;
}

void opRegCopyWide(CompilationUnit *cUnit, int destLo, int destHi,
                   int srcLo, int srcHi)
{
  bool destFP = FPREG(destLo) && FPREG(destHi);
  bool srcFP = FPREG(srcLo) && FPREG(srcHi);
  assert(FPREG(srcLo) == FPREG(srcHi));
  assert(FPREG(destLo) == FPREG(destHi));
  if (destFP) {
    if (srcFP) {
      opRegCopy(cUnit, S2D(destLo, destHi), S2D(srcLo, srcHi));
    } else {
      UNIMPLEMENTED(WARNING);
      newLIR0(cUnit, kX86Bkpt);
    }
  } else {
    if (srcFP) {
      UNIMPLEMENTED(WARNING);
      newLIR0(cUnit, kX86Bkpt);
    } else {
      // Handle overlap
      if (srcHi == destLo) {
        opRegCopy(cUnit, destHi, srcHi);
        opRegCopy(cUnit, destLo, srcLo);
      } else {
        opRegCopy(cUnit, destLo, srcLo);
        opRegCopy(cUnit, destHi, srcHi);
      }
    }
  }
}

}  // namespace art
