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
 * This file contains X86-specific register allocation support.
 */

#include "../../CompilerUtility.h"
#include "../../CompilerIR.h"
#include "../..//Dataflow.h"
#include "X86LIR.h"
#include "Codegen.h"
#include "../Ralloc.h"

namespace art {

void oatAdjustSpillMask(CompilationUnit* cUnit) {
  // Adjustment for LR spilling, x86 has no LR so nothing to do here
}

/*
 * Mark a callee-save fp register as promoted.  Note that
 * vpush/vpop uses contiguous register lists so we must
 * include any holes in the mask.  Associate holes with
 * Dalvik register INVALID_VREG (0xFFFFU).
 */
void oatMarkPreservedSingle(CompilationUnit* cUnit, int sReg, int reg)
{
    UNIMPLEMENTED(WARNING) << "oatMarkPreservedSingle";
#if 0
    LOG(FATAL) << "No support yet for promoted FP regs";
#endif
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
extern void oatClobberCalleeSave(CompilationUnit *cUnit)
{
    UNIMPLEMENTED(WARNING) << "oatClobberCalleeSave";
#if 0
    oatClobber(cUnit, r_ZERO);
    oatClobber(cUnit, r_AT);
    oatClobber(cUnit, r_V0);
    oatClobber(cUnit, r_V1);
    oatClobber(cUnit, r_A0);
    oatClobber(cUnit, r_A1);
    oatClobber(cUnit, r_A2);
    oatClobber(cUnit, r_A3);
    oatClobber(cUnit, r_T0);
    oatClobber(cUnit, r_T1);
    oatClobber(cUnit, r_T2);
    oatClobber(cUnit, r_T3);
    oatClobber(cUnit, r_T4);
    oatClobber(cUnit, r_T5);
    oatClobber(cUnit, r_T6);
    oatClobber(cUnit, r_T7);
    oatClobber(cUnit, r_T8);
    oatClobber(cUnit, r_T9);
    oatClobber(cUnit, r_K0);
    oatClobber(cUnit, r_K1);
    oatClobber(cUnit, r_GP);
    oatClobber(cUnit, r_FP);
    oatClobber(cUnit, r_RA);
    oatClobber(cUnit, r_F0);
    oatClobber(cUnit, r_F1);
    oatClobber(cUnit, r_F2);
    oatClobber(cUnit, r_F3);
    oatClobber(cUnit, r_F4);
    oatClobber(cUnit, r_F5);
    oatClobber(cUnit, r_F6);
    oatClobber(cUnit, r_F7);
    oatClobber(cUnit, r_F8);
    oatClobber(cUnit, r_F9);
    oatClobber(cUnit, r_F10);
    oatClobber(cUnit, r_F11);
    oatClobber(cUnit, r_F12);
    oatClobber(cUnit, r_F13);
    oatClobber(cUnit, r_F14);
    oatClobber(cUnit, r_F15);
#endif
}

extern RegLocation oatGetReturnWideAlt(CompilationUnit* cUnit)
{
    RegLocation res = LOC_C_RETURN_WIDE;
    res.lowReg = rAX;
    res.highReg = rDX;
    oatClobber(cUnit, rAX);
    oatClobber(cUnit, rDX);
    oatMarkInUse(cUnit, rAX);
    oatMarkInUse(cUnit, rDX);
    oatMarkPair(cUnit, res.lowReg, res.highReg);
    return res;
}

extern RegLocation oatGetReturnAlt(CompilationUnit* cUnit)
{
    RegLocation res = LOC_C_RETURN;
    res.lowReg = rDX;
    oatClobber(cUnit, rDX);
    oatMarkInUse(cUnit, rDX);
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
    UNIMPLEMENTED(WARNING) << "oatLockCallTemps";
#if 0
    oatLockTemp(cUnit, rARG0);
    oatLockTemp(cUnit, rARG1);
    oatLockTemp(cUnit, rARG2);
    oatLockTemp(cUnit, rARG3);
#endif
}

/* To be used when explicitly managing register use */
extern void oatFreeCallTemps(CompilationUnit* cUnit)
{
    UNIMPLEMENTED(WARNING) << "oatFreeCallTemps";
#if 0
    oatFreeTemp(cUnit, rARG0);
    oatFreeTemp(cUnit, rARG1);
    oatFreeTemp(cUnit, rARG2);
    oatFreeTemp(cUnit, rARG3);
#endif
}

/* Convert an instruction to a NOP */
void oatNopLIR( LIR* lir)
{
    ((LIR*)lir)->flags.isNop = true;
}

}  // namespace art
