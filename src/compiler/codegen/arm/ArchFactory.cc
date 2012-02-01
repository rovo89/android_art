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
 * This file contains arm-specific codegen factory support.
 * It is included by
 *
 *        Codegen-$(TARGET_ARCH_VARIANT).c
 *
 */

namespace art {

STATIC ArmLIR* genUnconditionalBranch(CompilationUnit*, ArmLIR*);
STATIC ArmLIR* genConditionalBranch(CompilationUnit*, ArmConditionCode,
                                    ArmLIR*);

/*
 * Utiltiy to load the current Method*.  Broken out
 * to allow easy change between placing the current Method* in a
 * dedicated register or its home location in the frame.
 */
STATIC void loadCurrMethodDirect(CompilationUnit *cUnit, int rTgt)
{
#if defined(METHOD_IN_REG)
    genRegCopy(cUnit, rTgt, rMETHOD);
#else
    loadWordDisp(cUnit, rSP, 0, rTgt);
#endif
}

STATIC int loadCurrMethod(CompilationUnit *cUnit)
{
#if defined(METHOD_IN_REG)
    return rMETHOD;
#else
    int mReg = oatAllocTemp(cUnit);
    loadCurrMethodDirect(cUnit, mReg);
    return mReg;
#endif
}

STATIC ArmLIR* genCheck(CompilationUnit* cUnit, ArmConditionCode cCode,
                        MIR* mir, ArmThrowKind kind)
{
    ArmLIR* tgt = (ArmLIR*)oatNew(sizeof(ArmLIR), true, kAllocLIR);
    tgt->opcode = kArmPseudoThrowTarget;
    tgt->operands[0] = kind;
    tgt->operands[1] = mir ? mir->offset : 0;
    ArmLIR* branch = genConditionalBranch(cUnit, cCode, tgt);
    // Remember branch target - will process later
    oatInsertGrowableList(&cUnit->throwLaunchpads, (intptr_t)tgt);
    return branch;
}

STATIC ArmLIR* genImmedCheck(CompilationUnit* cUnit, ArmConditionCode cCode,
                             int reg, int immVal, MIR* mir, ArmThrowKind kind)
{
    ArmLIR* tgt = (ArmLIR*)oatNew(sizeof(ArmLIR), true, kAllocLIR);
    tgt->opcode = kArmPseudoThrowTarget;
    tgt->operands[0] = kind;
    tgt->operands[1] = mir->offset;
    ArmLIR* branch;
    if (cCode == kArmCondAl) {
        branch = genUnconditionalBranch(cUnit, tgt);
    } else {
        branch = genCmpImmBranch(cUnit, cCode, reg, immVal);
        branch->generic.target = (LIR*)tgt;
    }
    // Remember branch target - will process later
    oatInsertGrowableList(&cUnit->throwLaunchpads, (intptr_t)tgt);
    return branch;
}

/* Perform null-check on a register.  */
STATIC ArmLIR* genNullCheck(CompilationUnit* cUnit, int sReg, int mReg,
                             MIR* mir)
{
    if (!(cUnit->disableOpt & (1 << kNullCheckElimination)) &&
        mir->optimizationFlags & MIR_IGNORE_NULL_CHECK) {
        return NULL;
    }
    return genImmedCheck(cUnit, kArmCondEq, mReg, 0, mir, kArmThrowNullPointer);
}

/* Perform check on two registers */
STATIC TGT_LIR* genRegRegCheck(CompilationUnit* cUnit, ArmConditionCode cCode,
                               int reg1, int reg2, MIR* mir, ArmThrowKind kind)
{
    ArmLIR* tgt = (ArmLIR*)oatNew(sizeof(ArmLIR), true, kAllocLIR);
    tgt->opcode = kArmPseudoThrowTarget;
    tgt->operands[0] = kind;
    tgt->operands[1] = mir ? mir->offset : 0;
    tgt->operands[2] = reg1;
    tgt->operands[3] = reg2;
    opRegReg(cUnit, kOpCmp, reg1, reg2);
    ArmLIR* branch = genConditionalBranch(cUnit, cCode, tgt);
    // Remember branch target - will process later
    oatInsertGrowableList(&cUnit->throwLaunchpads, (intptr_t)tgt);
    return branch;
}

}  // namespace art
