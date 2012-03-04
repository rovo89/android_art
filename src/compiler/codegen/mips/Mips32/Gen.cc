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
void genSparseSwitch(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc)
{
    const u2* table = cUnit->insns + mir->offset + mir->dalvikInsn.vB;
    if (cUnit->printMe) {
        dumpSparseSwitchTable(table);
    }
    // Add the table to the list - we'll process it later
    SwitchTable *tabRec = (SwitchTable *)oatNew(cUnit, sizeof(SwitchTable),
                         true, kAllocData);
    tabRec->table = table;
    tabRec->vaddr = mir->offset;
    int elements = table[1];
    tabRec->targets = (LIR* *)oatNew(cUnit, elements * sizeof(LIR*), true,
                                     kAllocLIR);
    oatInsertGrowableList(cUnit, &cUnit->switchTables, (intptr_t)tabRec);

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
    newLIR4(cUnit, kMipsDelta, rBase, 0, (intptr_t)baseLabel, (intptr_t)tabRec);
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
    newLIR4(cUnit, kMipsDelta, rBase, 0, (intptr_t)baseLabel, (intptr_t)tabRec);

    // Load the displacement from the switch table
    int rDisp = oatAllocTemp(cUnit);
    loadBaseIndexed(cUnit, rBase, rKey, rDisp, 2, kWord);

    // Add to r_AP and go
    opRegRegReg(cUnit, kOpAdd, r_RA, r_RA, rDisp);
    opReg(cUnit, kOpBx, r_RA);

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
void genFillArrayData(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc)
{
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
    oatLockCallTemps(cUnit);
    loadValueDirectFixed(cUnit, rlSrc, rARG0);

    // Must prevent code motion for the curr pc pair
    genBarrier(cUnit);
    newLIR0(cUnit, kMipsCurrPC);  // Really a jal to .+8
    // Now, fill the branch delay slot with the helper load
    int rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread,
                          pHandleFillArrayDataFromCode));
    genBarrier(cUnit);  // Scheduling barrier

    // Construct BaseLabel and set up table base register
    LIR* baseLabel = newLIR0(cUnit, kPseudoTargetLabel);

    // Materialize a pointer to the fill data image
    newLIR4(cUnit, kMipsDelta, rARG1, 0, (intptr_t)baseLabel, (intptr_t)tabRec);

    // And go...
    callRuntimeHelper(cUnit, rTgt);  // ( array*, fill_data* )
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
    opRegCopy(cUnit, rlResult.lowReg, rlSrc.lowReg);
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
    LIR* branch = opCmpImmBranch(cUnit, kCondNe, rlResult.lowReg, 0, NULL);
    newLIR3(cUnit, kMipsSltu, t0, rlSrc1.lowReg, rlSrc2.lowReg);
    newLIR3(cUnit, kMipsSltu, t1, rlSrc2.lowReg, rlSrc1.lowReg);
    newLIR3(cUnit, kMipsSubu, rlResult.lowReg, t1, t0);
    oatFreeTemp(cUnit, t0);
    oatFreeTemp(cUnit, t1);
    LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
    branch->target = (LIR*)target;
    storeValue(cUnit, rlDest, rlResult);
}

LIR* opCmpBranch(CompilationUnit* cUnit, ConditionCode cond, int src1,
                 int src2, LIR* target)
{
    LIR* branch;
    MipsOpCode sltOp;
    MipsOpCode brOp;
    bool cmpZero = false;
    bool swapped = false;
    switch(cond) {
        case kCondEq:
            brOp = kMipsBeq;
            cmpZero = true;
            break;
        case kCondNe:
            brOp = kMipsBne;
            cmpZero = true;
            break;
        case kCondCc:
            sltOp = kMipsSltu;
            brOp = kMipsBnez;
            break;
        case kCondCs:
            sltOp = kMipsSltu;
            brOp = kMipsBeqz;
            break;
        case kCondGe:
            sltOp = kMipsSlt;
            brOp = kMipsBeqz;
            break;
        case kCondGt:
            sltOp = kMipsSlt;
            brOp = kMipsBnez;
            swapped = true;
            break;
        case kCondLe:
            sltOp = kMipsSlt;
            brOp = kMipsBeqz;
            swapped = true;
            break;
        case kCondLt:
            sltOp = kMipsSlt;
            brOp = kMipsBnez;
            break;
        case kCondHi:  // Gtu
            sltOp = kMipsSltu;
            brOp = kMipsBnez;
            swapped = true;
            break;
        default:
            UNIMPLEMENTED(FATAL) << "No support for ConditionCode: "
                                 << (int) cond;
            return NULL;
    }
    if (cmpZero) {
        branch = newLIR2(cUnit, brOp, src1, src2);
    } else {
        int tReg = oatAllocTemp(cUnit);
        if (swapped) {
            newLIR3(cUnit, sltOp, tReg, src2, src1);
        } else {
            newLIR3(cUnit, sltOp, tReg, src1, src2);
        }
        branch = newLIR1(cUnit, brOp, tReg);
        oatFreeTemp(cUnit, tReg);
    }
    branch->target = target;
    return branch;
}

LIR* opCmpImmBranch(CompilationUnit* cUnit, ConditionCode cond, int reg,
                    int checkValue, LIR* target)
{
    LIR* branch;
    if (checkValue != 0) {
        // TUNING: handle s16 & kCondLt/Mi case using slti
        int tReg = oatAllocTemp(cUnit);
        loadConstant(cUnit, tReg, checkValue);
        branch = opCmpBranch(cUnit, cond, reg, tReg, target);
        oatFreeTemp(cUnit, tReg);
        return branch;
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
            // Tuning: use slti when applicable
            int tReg = oatAllocTemp(cUnit);
            loadConstant(cUnit, tReg, checkValue);
            branch = opCmpBranch(cUnit, cond, reg, tReg, target);
            oatFreeTemp(cUnit, tReg);
            return branch;
    }
    branch = newLIR1(cUnit, opc, reg);
    branch->target = target;
    return branch;
}

LIR* opRegCopyNoInsert(CompilationUnit *cUnit, int rDest, int rSrc)
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

LIR* opRegCopy(CompilationUnit *cUnit, int rDest, int rSrc)
{
    LIR *res = opRegCopyNoInsert(cUnit, rDest, rSrc);
    oatAppendLIR(cUnit, (LIR*)res);
    return res;
}

void opRegCopyWide(CompilationUnit *cUnit, int destLo, int destHi,
                    int srcLo, int srcHi)
{
#ifdef __mips_hard_float
    bool destFP = FPREG(destLo) && FPREG(destHi);
    bool srcFP = FPREG(srcLo) && FPREG(srcHi);
    assert(FPREG(srcLo) == FPREG(srcHi));
    assert(FPREG(destLo) == FPREG(destHi));
    if (destFP) {
        if (srcFP) {
            opRegCopy(cUnit, S2D(destLo, destHi), S2D(srcLo, srcHi));
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
                opRegCopy(cUnit, destHi, srcHi);
                opRegCopy(cUnit, destLo, srcLo);
            } else {
                opRegCopy(cUnit, destLo, srcLo);
                opRegCopy(cUnit, destHi, srcHi);
            }
        }
    }
#else
    // Handle overlap
    if (srcHi == destLo) {
        opRegCopy(cUnit, destHi, srcHi);
        opRegCopy(cUnit, destLo, srcLo);
    } else {
        opRegCopy(cUnit, destLo, srcLo);
        opRegCopy(cUnit, destHi, srcHi);
    }
#endif
}

}  // namespace art
