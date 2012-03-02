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
 * This file contains codegen for the Mips ISA and is intended to be
 * includes by:
 *
 *        Codegen-$(TARGET_ARCH_VARIANT).c
 *
 */

namespace art {

/*
 * The sparse table in the literal pool is an array of <key,displacement>
 * pairs.  For each set, we'll load them as a pair using ldmia.
 * This means that the register number of the temp we use for the key
 * must be lower than the reg for the displacement.
 *
 * The test loop will look something like:
 *
 *   adr   rBase, <table>
 *   ldr   rVal, [rSP, vRegOff]
 *   mov   rIdx, #tableSize
 * lp:
 *   ldmia rBase!, {rKey, rDisp}
 *   sub   rIdx, #1
 *   cmp   rVal, rKey
 *   ifeq
 *   add   rPC, rDisp   ; This is the branch from which we compute displacement
 *   cbnz  rIdx, lp
 */
void genSparseSwitch(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc)
{
    UNIMPLEMENTED(FATAL) << "Needs Mips sparse switch";
#if 0
    const u2* table = cUnit->insns + mir->offset + mir->dalvikInsn.vB;
    if (cUnit->printMe) {
        dumpSparseSwitchTable(table);
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
    target->defMask = ENCODE_ALL;
    // Load next key/disp
    newLIR2(cUnit, kThumb2LdmiaWB, rBase, (1 << rKey) | (1 << rDisp));
    opRegReg(cUnit, kOpCmp, rKey, rlSrc.lowReg);
    // Go if match. NOTE: No instruction set switch here - must stay Thumb2
    genIT(cUnit, kArmCondEq, "");
    LIR* switchBranch = newLIR1(cUnit, kThumb2AddPCR, rDisp);
    tabRec->bxInst = switchBranch;
    // Needs to use setflags encoding here
    newLIR3(cUnit, kThumb2SubsRRI12, rIdx, rIdx, 1);
    LIR* branch = opCondBranch(cUnit, kCondNe);
    branch->target = (LIR*)target;
#endif
}


void genPackedSwitch(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc)
{
    UNIMPLEMENTED(FATAL) << "Need Mips packed switch";
#if 0
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
    LIR* branchOver = opCondBranch(cUnit, kCondHi);

    // Load the displacement from the switch table
    int dispReg = oatAllocTemp(cUnit);
    loadBaseIndexed(cUnit, tableBase, keyReg, dispReg, 2, kWord);

    // ..and go! NOTE: No instruction set switch here - must stay Thumb2
    LIR* switchBranch = newLIR1(cUnit, kThumb2AddPCR, dispReg);
    tabRec->bxInst = switchBranch;

    /* branchOver target here */
    LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
    target->defMask = ENCODE_ALL;
    branchOver->target = (LIR*)target;
#endif
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
void genFillArrayData(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc)
{
    UNIMPLEMENTED(FATAL) << "Needs Mips FillArrayData";
#if 0
    const u2* table = cUnit->insns + mir->offset + mir->dalvikInsn.vB;
    // Add the table to the list - we'll process it later
    FillArrayData *tabRec = (FillArrayData *)
         oatNew(cUnit, sizeof(FillArrayData), true, kAllocData);
    tabRec->table = table;
    tabRec->vaddr = mir->offset;
    u2 width = tabRec->table[1];
    u4 size = tabRec->table[2] | (((u4)tabRec->table[3]) << 16);
    tabRec->size = (size * width) + 8;

    oatInsertGrowableList(cUnit, &cUnit->fillArrayData, (intptr_t)tabRec);

    // Making a call - use explicit registers
    oatFlushAllRegs(cUnit);   /* Everything to home location */
    loadValueDirectFixed(cUnit, rlSrc, rARG0);
    loadWordDisp(cUnit, rSELF,
                 OFFSETOF_MEMBER(Thread, pHandleFillArrayDataFromCode), rLR);
    // Materialize a pointer to the fill data image
    newLIR3(cUnit, kThumb2Adr, r1, 0, (intptr_t)tabRec);
    callRuntimeHelper(cUnit, rLR);
#endif
}

void genNegFloat(CompilationUnit *cUnit, RegLocation rlDest, RegLocation rlSrc)
{
    RegLocation rlResult;
    rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
    rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    opRegRegImm(cUnit, kOpAdd, rlResult.lowReg,
                rlSrc.lowReg, 0x80000000);
    storeValue(cUnit, rlDest, rlResult);
}

void genNegDouble(CompilationUnit *cUnit, RegLocation rlDest, RegLocation rlSrc)
{
    RegLocation rlResult;
    rlSrc = loadValueWide(cUnit, rlSrc, kCoreReg);
    rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    opRegRegImm(cUnit, kOpAdd, rlResult.highReg, rlSrc.highReg,
                        0x80000000);
    genRegCopy(cUnit, rlResult.lowReg, rlSrc.lowReg);
    storeValueWide(cUnit, rlDest, rlResult);
}

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
    int rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread, pLockObjectFromCode));
    callRuntimeHelper(cUnit, rTgt);
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
    int rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread, pUnlockObjectFromCode));
    callRuntimeHelper(cUnit, rTgt);
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
    rlSrc1 = loadValueWide(cUnit, rlSrc1, kCoreReg);
    rlSrc2 = loadValueWide(cUnit, rlSrc2, kCoreReg);
    int t0 = oatAllocTemp(cUnit);
    int t1 = oatAllocTemp(cUnit);
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    newLIR3(cUnit, kMipsSlt, t0, rlSrc1.highReg, rlSrc2.highReg);
    newLIR3(cUnit, kMipsSlt, t1, rlSrc2.highReg, rlSrc1.highReg);
    newLIR3(cUnit, kMipsSubu, rlResult.lowReg, t1, t0);
    LIR* branch = genCmpImmBranch(cUnit, kCondNe, rlResult.lowReg, 0);
    newLIR3(cUnit, kMipsSltu, t0, rlSrc1.lowReg, rlSrc2.lowReg);
    newLIR3(cUnit, kMipsSltu, t1, rlSrc2.lowReg, rlSrc1.lowReg);
    newLIR3(cUnit, kMipsSubu, rlResult.lowReg, t1, t0);
    oatFreeTemp(cUnit, t0);
    oatFreeTemp(cUnit, t1);
    LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
    target->defMask = ENCODE_ALL;
    branch->target = (LIR*)target;
    storeValue(cUnit, rlDest, rlResult);
}

LIR* genCompareBranch(CompilationUnit* cUnit, ConditionCode cond, int src1,
                      int src2)
{
    if (cond == kCondEq) {
        return newLIR2(cUnit, kMipsBeq, src1, src2);
    } else if (cond == kCondNe) {
        return newLIR2(cUnit, kMipsBne, src1, src2);
    }
    //int rRes = oatAllocTemp(cUnit);
    switch(cond) {
        case kCondEq: return newLIR2(cUnit, kMipsBeq, src1, src2);
        case kCondNe: return newLIR2(cUnit, kMipsBne, src1, src2);
        default:
            UNIMPLEMENTED(FATAL) << "Need to flesh out genCompareBranch";
            return NULL;
    }
}

LIR* genCmpImmBranch(CompilationUnit* cUnit, ConditionCode cond, int reg,
                     int checkValue)
{
    if (checkValue != 0) {
        // TUNING: handle s16 & kCondLt/Mi case using slti
        int tReg = oatAllocTemp(cUnit);
        loadConstant(cUnit, tReg, checkValue);
        return genCompareBranch(cUnit, cond, reg, tReg);
    }
    MipsOpCode opc;
    switch(cond) {
        case kCondEq: opc = kMipsBeqz; break;
        case kCondGe: opc = kMipsBgez; break;
        case kCondGt: opc = kMipsBgtz; break;
        case kCondLe: opc = kMipsBlez; break;
        //case KCondMi:
        case kCondLt: opc = kMipsBltz; break;
        case kCondNe: opc = kMipsBnez; break;
        default:
            int tReg = oatAllocTemp(cUnit);
            loadConstant(cUnit, tReg, checkValue);
            return genCompareBranch(cUnit, cond, reg, tReg);
    }
    return newLIR1(cUnit, opc, reg);
}

LIR* genRegCopyNoInsert(CompilationUnit *cUnit, int rDest, int rSrc)
{
    LIR* res;
    MipsOpCode opcode;
#ifdef __mips_hard_float
    if (FPREG(rDest) || FPREG(rSrc))
        return fpRegCopy(cUnit, rDest, rSrc);
#endif
    res = (LIR *) oatNew(cUnit, sizeof(LIR), true, kAllocLIR);
    opcode = kMipsMove;
    assert(LOWREG(rDest) && LOWREG(rSrc));
    res->operands[0] = rDest;
    res->operands[1] = rSrc;
    res->opcode = opcode;
    setupResourceMasks(res);
    if (rDest == rSrc) {
        res->flags.isNop = true;
    }
    return res;
}

LIR* genRegCopy(CompilationUnit *cUnit, int rDest, int rSrc)
{
    LIR *res = genRegCopyNoInsert(cUnit, rDest, rSrc);
    oatAppendLIR(cUnit, (LIR*)res);
    return res;
}

void genRegCopyWide(CompilationUnit *cUnit, int destLo, int destHi,
                    int srcLo, int srcHi)
{
#ifdef __mips_hard_float
    bool destFP = FPREG(destLo) && FPREG(destHi);
    bool srcFP = FPREG(srcLo) && FPREG(srcHi);
    assert(FPREG(srcLo) == FPREG(srcHi));
    assert(FPREG(destLo) == FPREG(destHi));
    if (destFP) {
        if (srcFP) {
            genRegCopy(cUnit, S2D(destLo, destHi), S2D(srcLo, srcHi));
        } else {
           /* note the operands are swapped for the mtc1 instr */
            newLIR2(cUnit, kMipsMtc1, srcLo, destLo);
            newLIR2(cUnit, kMipsMtc1, srcHi, destHi);
        }
    } else {
        if (srcFP) {
            newLIR2(cUnit, kMipsMfc1, destLo, srcLo);
            newLIR2(cUnit, kMipsMfc1, destHi, srcHi);
        } else {
            // Handle overlap
            if (srcHi == destLo) {
                genRegCopy(cUnit, destHi, srcHi);
                genRegCopy(cUnit, destLo, srcLo);
            } else {
                genRegCopy(cUnit, destLo, srcLo);
                genRegCopy(cUnit, destHi, srcHi);
            }
        }
    }
#else
    // Handle overlap
    if (srcHi == destLo) {
        genRegCopy(cUnit, destHi, srcHi);
        genRegCopy(cUnit, destLo, srcLo);
    } else {
        genRegCopy(cUnit, destLo, srcLo);
        genRegCopy(cUnit, destHi, srcHi);
    }
#endif
}

}  // namespace art
