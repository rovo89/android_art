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

/*
 * This file contains codegen for the Thumb2 ISA and is intended to be
 * includes by:
 *
 *        Codegen-$(TARGET_ARCH_VARIANT).c
 *
 */

#include "oat_compilation_unit.h"

namespace art {


/*
 * Generate a Thumb2 IT instruction, which can nullify up to
 * four subsequent instructions based on a condition and its
 * inverse.  The condition applies to the first instruction, which
 * is executed if the condition is met.  The string "guide" consists
 * of 0 to 3 chars, and applies to the 2nd through 4th instruction.
 * A "T" means the instruction is executed if the condition is
 * met, and an "E" means the instruction is executed if the condition
 * is not met.
 */
LIR* opIT(CompilationUnit* cUnit, ArmConditionCode code, const char* guide)
{
    int mask;
    int condBit = code & 1;
    int altBit = condBit ^ 1;
    int mask3 = 0;
    int mask2 = 0;
    int mask1 = 0;

    //Note: case fallthroughs intentional
    switch(strlen(guide)) {
        case 3:
            mask1 = (guide[2] == 'T') ? condBit : altBit;
        case 2:
            mask2 = (guide[1] == 'T') ? condBit : altBit;
        case 1:
            mask3 = (guide[0] == 'T') ? condBit : altBit;
            break;
        case 0:
            break;
        default:
            LOG(FATAL) << "OAT: bad case in opIT";
    }
    mask = (mask3 << 3) | (mask2 << 2) | (mask1 << 1) |
           (1 << (3 - strlen(guide)));
    return newLIR2(cUnit, kThumb2It, code, mask);
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
    loadValueDirectFixed(cUnit, rlSrc, r0);
    loadWordDisp(cUnit, rSELF,
                 OFFSETOF_MEMBER(Thread, pHandleFillArrayDataFromCode), rLR);
    // Materialize a pointer to the fill data image
    newLIR3(cUnit, kThumb2Adr, r1, 0, (intptr_t)tabRec);
    oatClobberCalleeSave(cUnit);
    opReg(cUnit, kOpBlx, rLR);
}

void genNegFloat(CompilationUnit* cUnit, RegLocation rlDest, RegLocation rlSrc)
{
    RegLocation rlResult;
    rlSrc = loadValue(cUnit, rlSrc, kFPReg);
    rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
    newLIR2(cUnit, kThumb2Vnegs, rlResult.lowReg, rlSrc.lowReg);
    storeValue(cUnit, rlDest, rlResult);
}

void genNegDouble(CompilationUnit* cUnit, RegLocation rlDest, RegLocation rlSrc)
{
    RegLocation rlResult;
    rlSrc = loadValueWide(cUnit, rlSrc, kFPReg);
    rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
    newLIR2(cUnit, kThumb2Vnegd, S2D(rlResult.lowReg, rlResult.highReg),
            S2D(rlSrc.lowReg, rlSrc.highReg));
    storeValueWide(cUnit, rlDest, rlResult);
}

/*
 * Handle simple case (thin lock) inline.  If it's complicated, bail
 * out to the heavyweight lock/unlock routines.  We'll use dedicated
 * registers here in order to be in the right position in case we
 * to bail to dvm[Lock/Unlock]Object(self, object)
 *
 * r0 -> self pointer [arg0 for dvm[Lock/Unlock]Object
 * r1 -> object [arg1 for dvm[Lock/Unlock]Object
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
void genMonitorEnter(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc)
{
    oatFlushAllRegs(cUnit);
    DCHECK_EQ(LW_SHAPE_THIN, 0);
    loadValueDirectFixed(cUnit, rlSrc, r0);  // Get obj
    oatLockCallTemps(cUnit);  // Prepare for explicit register usage
    genNullCheck(cUnit, rlSrc.sRegLow, r0, mir);
    loadWordDisp(cUnit, rSELF, Thread::ThinLockIdOffset().Int32Value(), r2);
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
    loadWordDisp(cUnit, rSELF, OFFSETOF_MEMBER(Thread, pLockObjectFromCode),
                 rLR);
    oatClobberCalleeSave(cUnit);
    opReg(cUnit, kOpBlx, rLR);
    oatGenMemBarrier(cUnit, kSY);
}

/*
 * For monitor unlock, we don't have to use ldrex/strex.  Once
 * we've determined that the lock is thin and that we own it with
 * a zero recursion count, it's safe to punch it back to the
 * initial, unlock thin state with a store word.
 */
void genMonitorExit(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc)
{
    DCHECK_EQ(LW_SHAPE_THIN, 0);
    oatFlushAllRegs(cUnit);
    loadValueDirectFixed(cUnit, rlSrc, r0);  // Get obj
    oatLockCallTemps(cUnit);  // Prepare for explicit register usage
    genNullCheck(cUnit, rlSrc.sRegLow, r0, mir);
    loadWordDisp(cUnit, r0, Object::MonitorOffset().Int32Value(), r1); // Get lock
    loadWordDisp(cUnit, rSELF, Thread::ThinLockIdOffset().Int32Value(), r2);
    // Is lock unheld on lock or held by us (==threadId) on unlock?
    opRegRegImm(cUnit, kOpAnd, r3, r1, (LW_HASH_STATE_MASK << LW_HASH_STATE_SHIFT));
    // Align owner
    opRegImm(cUnit, kOpLsl, r2, LW_LOCK_OWNER_SHIFT);
    newLIR3(cUnit, kThumb2Bfc, r1, LW_HASH_STATE_SHIFT, LW_LOCK_OWNER_SHIFT - 1);
    opRegReg(cUnit, kOpSub, r1, r2);
    opIT(cUnit, kArmCondEq, "EE");
    storeWordDisp(cUnit, r0, Object::MonitorOffset().Int32Value(), r3);
    // Go expensive route - UnlockObjectFromCode(obj);
    loadWordDisp(cUnit, rSELF, OFFSETOF_MEMBER(Thread, pUnlockObjectFromCode),
                 rLR);
    oatClobberCalleeSave(cUnit);
    opReg(cUnit, kOpBlx, rLR);
    oatGenMemBarrier(cUnit, kSY);
}

/*
 * 64-bit 3way compare function.
 *     mov   rX, #-1
 *     cmp   op1hi, op2hi
 *     blt   done
 *     bgt   flip
 *     sub   rX, op1lo, op2lo (treat as unsigned)
 *     beq   done
 *     ite   hi
 *     mov(hi)   rX, #-1
 *     mov(!hi)  rX, #1
 * flip:
 *     neg   rX
 * done:
 */
void genCmpLong(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
    LIR* target1;
    LIR* target2;
    rlSrc1 = loadValueWide(cUnit, rlSrc1, kCoreReg);
    rlSrc2 = loadValueWide(cUnit, rlSrc2, kCoreReg);
    int tReg = oatAllocTemp(cUnit);
    loadConstant(cUnit, tReg, -1);
    opRegReg(cUnit, kOpCmp, rlSrc1.highReg, rlSrc2.highReg);
    LIR* branch1 = opCondBranch(cUnit, kCondLt, NULL);
    LIR* branch2 = opCondBranch(cUnit, kCondGt, NULL);
    opRegRegReg(cUnit, kOpSub, tReg, rlSrc1.lowReg, rlSrc2.lowReg);
    LIR* branch3 = opCondBranch(cUnit, kCondEq, NULL);

    opIT(cUnit, kArmCondHi, "E");
    newLIR2(cUnit, kThumb2MovImmShift, tReg, modifiedImmediate(-1));
    loadConstant(cUnit, tReg, 1);
    genBarrier(cUnit);

    target2 = newLIR0(cUnit, kPseudoTargetLabel);
    opRegReg(cUnit, kOpNeg, tReg, tReg);

    target1 = newLIR0(cUnit, kPseudoTargetLabel);

    RegLocation rlTemp = LOC_C_RETURN; // Just using as template, will change
    rlTemp.lowReg = tReg;
    storeValue(cUnit, rlDest, rlTemp);
    oatFreeTemp(cUnit, tReg);

    branch1->target = (LIR*)target1;
    branch2->target = (LIR*)target2;
    branch3->target = branch1->target;
}

/*
 * Generate a register comparison to an immediate and branch.  Caller
 * is responsible for setting branch target field.
 */
LIR* opCmpImmBranch(CompilationUnit* cUnit, ConditionCode cond, int reg,
                    int checkValue, LIR* target)
{
    LIR* branch;
    int modImm;
    ArmConditionCode armCond = oatArmConditionEncoding(cond);
    if ((LOWREG(reg)) && (checkValue == 0) &&
       ((armCond == kArmCondEq) || (armCond == kArmCondNe))) {
        branch = newLIR2(cUnit,
                         (armCond == kArmCondEq) ? kThumb2Cbz : kThumb2Cbnz,
                         reg, 0);
    } else {
        modImm = modifiedImmediate(checkValue);
        if (LOWREG(reg) && ((checkValue & 0xff) == checkValue)) {
            newLIR2(cUnit, kThumbCmpRI8, reg, checkValue);
        } else if (modImm >= 0) {
            newLIR2(cUnit, kThumb2CmpRI8, reg, modImm);
        } else {
            int tReg = oatAllocTemp(cUnit);
            loadConstant(cUnit, tReg, checkValue);
            opRegReg(cUnit, kOpCmp, reg, tReg);
        }
        branch = newLIR2(cUnit, kThumbBCond, 0, armCond);
    }
    branch->target = target;
    return branch;
}
LIR* opRegCopyNoInsert(CompilationUnit* cUnit, int rDest, int rSrc)
{
    LIR* res;
    ArmOpcode opcode;
    if (FPREG(rDest) || FPREG(rSrc))
        return fpRegCopy(cUnit, rDest, rSrc);
    if (LOWREG(rDest) && LOWREG(rSrc))
        opcode = kThumbMovRR;
    else if (!LOWREG(rDest) && !LOWREG(rSrc))
         opcode = kThumbMovRR_H2H;
    else if (LOWREG(rDest))
         opcode = kThumbMovRR_H2L;
    else
         opcode = kThumbMovRR_L2H;
    res = rawLIR(cUnit, cUnit->currentDalvikOffset, opcode, rDest, rSrc);
    if (!(cUnit->disableOpt & (1 << kSafeOptimizations)) && rDest == rSrc) {
        res->flags.isNop = true;
    }
    return res;
}

LIR* opRegCopy(CompilationUnit* cUnit, int rDest, int rSrc)
{
    LIR* res = opRegCopyNoInsert(cUnit, rDest, rSrc);
    oatAppendLIR(cUnit, (LIR*)res);
    return res;
}

void opRegCopyWide(CompilationUnit* cUnit, int destLo, int destHi,
                           int srcLo, int srcHi)
{
    bool destFP = FPREG(destLo) && FPREG(destHi);
    bool srcFP = FPREG(srcLo) && FPREG(srcHi);
    DCHECK_EQ(FPREG(srcLo), FPREG(srcHi));
    DCHECK_EQ(FPREG(destLo), FPREG(destHi));
    if (destFP) {
        if (srcFP) {
            opRegCopy(cUnit, S2D(destLo, destHi), S2D(srcLo, srcHi));
        } else {
            newLIR3(cUnit, kThumb2Fmdrr, S2D(destLo, destHi), srcLo, srcHi);
        }
    } else {
        if (srcFP) {
            newLIR3(cUnit, kThumb2Fmrrd, destLo, destHi, S2D(srcLo, srcHi));
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
