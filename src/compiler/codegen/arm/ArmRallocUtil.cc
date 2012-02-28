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
 * This file contains Arm-specific register allocation support.
 */

#include "../../CompilerUtility.h"
#include "../../CompilerIR.h"
#include "../..//Dataflow.h"
#include "ArmLIR.h"
#include "Codegen.h"
#include "../Ralloc.h"

namespace art {

/*
 * TUNING: is leaf?  Can't just use "hasInvoke" to determine as some
 * instructions might call out to C/assembly helper functions.  Until
 * machinery is in place, always spill lr.
 */

void oatAdjustSpillMask(CompilationUnit* cUnit)
{
    cUnit->coreSpillMask |= (1 << rLR);
    cUnit->numCoreSpills++;
}

/*
 * Mark a callee-save fp register as promoted.  Note that
 * vpush/vpop uses contiguous register lists so we must
 * include any holes in the mask.  Associate holes with
 * Dalvik register INVALID_VREG (0xFFFFU).
 */
void oatMarkPreservedSingle(CompilationUnit* cUnit, int sReg, int reg)
{
    DCHECK_GE(reg, FP_REG_MASK + FP_CALLEE_SAVE_BASE);
    reg = (reg & FP_REG_MASK) - FP_CALLEE_SAVE_BASE;
    // Ensure fpVmapTable is large enough
    int tableSize = cUnit->fpVmapTable.size();
    for (int i = tableSize; i < (reg + 1); i++) {
        cUnit->fpVmapTable.push_back(INVALID_VREG);
    }
    // Add the current mapping
    cUnit->fpVmapTable[reg] = sReg;
    // Size of fpVmapTable is high-water mark, use to set mask
    cUnit->numFPSpills = cUnit->fpVmapTable.size();
    cUnit->fpSpillMask = ((1 << cUnit->numFPSpills) - 1) << FP_CALLEE_SAVE_BASE;
}

void oatFlushRegWide(CompilationUnit* cUnit, int reg1, int reg2)
{
    RegisterInfo* info1 = oatGetRegInfo(cUnit, reg1);
    RegisterInfo* info2 = oatGetRegInfo(cUnit, reg2);
    DCHECK(info1 && info2 && info1->pair && info2->pair &&
           (info1->partner == info2->reg) &&
           (info2->partner == info1->reg));
    if ((info1->live && info1->dirty) || (info2->live && info2->dirty)) {
        if (!(info1->isTemp && info2->isTemp)) {
            /* Should not happen.  If it does, there's a problem in evalLoc */
            LOG(FATAL) << "Long half-temp, half-promoted";
        }

        info1->dirty = false;
        info2->dirty = false;
        if (oatS2VReg(cUnit, info2->sReg) <
            oatS2VReg(cUnit, info1->sReg))
            info1 = info2;
        int vReg = oatS2VReg(cUnit, info1->sReg);
        oatFlushRegWideImpl(cUnit, rSP,
                                    oatVRegOffset(cUnit, vReg),
                                    info1->reg, info1->partner);
    }
}

void oatFlushReg(CompilationUnit* cUnit, int reg)
{
    RegisterInfo* info = oatGetRegInfo(cUnit, reg);
    if (info->live && info->dirty) {
        info->dirty = false;
        int vReg = oatS2VReg(cUnit, info->sReg);
        oatFlushRegImpl(cUnit, rSP,
                                oatVRegOffset(cUnit, vReg),
                                reg, kWord);
    }
}

/* Give access to the target-dependent FP register encoding to common code */
bool oatIsFpReg(int reg) {
    return FPREG(reg);
}

uint32_t oatFpRegMask() {
    return FP_REG_MASK;
}

/* Clobber all regs that might be used by an external C call */
void oatClobberCalleeSave(CompilationUnit *cUnit)
{
    oatClobber(cUnit, r0);
    oatClobber(cUnit, r1);
    oatClobber(cUnit, r2);
    oatClobber(cUnit, r3);
    oatClobber(cUnit, r12);
    oatClobber(cUnit, r14lr);
}

extern RegLocation oatGetReturnWide(CompilationUnit* cUnit)
{
    RegLocation res = LOC_C_RETURN_WIDE;
    oatClobber(cUnit, r0);
    oatClobber(cUnit, r1);
    oatMarkInUse(cUnit, r0);
    oatMarkInUse(cUnit, r1);
    oatMarkPair(cUnit, res.lowReg, res.highReg);
    return res;
}

extern RegLocation oatGetReturnWideAlt(CompilationUnit* cUnit)
{
    RegLocation res = LOC_C_RETURN_WIDE;
    res.lowReg = r2;
    res.highReg = r3;
    oatClobber(cUnit, r2);
    oatClobber(cUnit, r3);
    oatMarkInUse(cUnit, r2);
    oatMarkInUse(cUnit, r3);
    oatMarkPair(cUnit, res.lowReg, res.highReg);
    return res;
}

extern RegLocation oatGetReturn(CompilationUnit* cUnit)
{
    RegLocation res = LOC_C_RETURN;
    oatClobber(cUnit, r0);
    oatMarkInUse(cUnit, r0);
    return res;
}

extern RegLocation oatGetReturnAlt(CompilationUnit* cUnit)
{
    RegLocation res = LOC_C_RETURN;
    res.lowReg = r1;
    oatClobber(cUnit, r1);
    oatMarkInUse(cUnit, r1);
    return res;
}

extern RegisterInfo* oatGetRegInfo(CompilationUnit* cUnit, int reg)
{
    return FPREG(reg) ? &cUnit->regPool->FPRegs[reg & FP_REG_MASK]
                      : &cUnit->regPool->coreRegs[reg];
}

/* To be used when explicitly managing register use */
extern void oatLockCallTemps(CompilationUnit* cUnit)
{
    oatLockTemp(cUnit, r0);
    oatLockTemp(cUnit, r1);
    oatLockTemp(cUnit, r2);
    oatLockTemp(cUnit, r3);
}

/* To be used when explicitly managing register use */
extern void oatFreeCallTemps(CompilationUnit* cUnit)
{
    oatFreeTemp(cUnit, r0);
    oatFreeTemp(cUnit, r1);
    oatFreeTemp(cUnit, r2);
    oatFreeTemp(cUnit, r3);
}

/* Convert an instruction to a NOP */
void oatNopLIR( LIR* lir)
{
    ((LIR*)lir)->flags.isNop = true;
}

}  // namespace art
