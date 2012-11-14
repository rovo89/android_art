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
  cUnit->coreSpillMask |= (1 << rARM_LR);
  cUnit->numCoreSpills++;
}

/*
 * Mark a callee-save fp register as promoted.  Note that
 * vpush/vpop uses contiguous register lists so we must
 * include any holes in the mask.  Associate holes with
 * Dalvik register INVALID_VREG (0xFFFFU).
 */
void oatMarkPreservedSingle(CompilationUnit* cUnit, int vReg, int reg)
{
  DCHECK_GE(reg, ARM_FP_REG_MASK + ARM_FP_CALLEE_SAVE_BASE);
  reg = (reg & ARM_FP_REG_MASK) - ARM_FP_CALLEE_SAVE_BASE;
  // Ensure fpVmapTable is large enough
  int tableSize = cUnit->fpVmapTable.size();
  for (int i = tableSize; i < (reg + 1); i++) {
    cUnit->fpVmapTable.push_back(INVALID_VREG);
  }
  // Add the current mapping
  cUnit->fpVmapTable[reg] = vReg;
  // Size of fpVmapTable is high-water mark, use to set mask
  cUnit->numFPSpills = cUnit->fpVmapTable.size();
  cUnit->fpSpillMask = ((1 << cUnit->numFPSpills) - 1) << ARM_FP_CALLEE_SAVE_BASE;
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
    if (SRegToVReg(cUnit, info2->sReg) <
      SRegToVReg(cUnit, info1->sReg))
      info1 = info2;
    int vReg = SRegToVReg(cUnit, info1->sReg);
    oatFlushRegWideImpl(cUnit, rARM_SP, oatVRegOffset(cUnit, vReg),
                        info1->reg, info1->partner);
  }
}

void oatFlushReg(CompilationUnit* cUnit, int reg)
{
  RegisterInfo* info = oatGetRegInfo(cUnit, reg);
  if (info->live && info->dirty) {
    info->dirty = false;
    int vReg = SRegToVReg(cUnit, info->sReg);
    oatFlushRegImpl(cUnit, rARM_SP, oatVRegOffset(cUnit, vReg), reg, kWord);
  }
}

/* Give access to the target-dependent FP register encoding to common code */
bool oatIsFpReg(int reg) {
  return ARM_FPREG(reg);
}

uint32_t oatFpRegMask() {
  return ARM_FP_REG_MASK;
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
  oatClobber(cUnit, fr0);
  oatClobber(cUnit, fr1);
  oatClobber(cUnit, fr2);
  oatClobber(cUnit, fr3);
  oatClobber(cUnit, fr4);
  oatClobber(cUnit, fr5);
  oatClobber(cUnit, fr6);
  oatClobber(cUnit, fr7);
  oatClobber(cUnit, fr8);
  oatClobber(cUnit, fr9);
  oatClobber(cUnit, fr10);
  oatClobber(cUnit, fr11);
  oatClobber(cUnit, fr12);
  oatClobber(cUnit, fr13);
  oatClobber(cUnit, fr14);
  oatClobber(cUnit, fr15);
}

extern RegLocation oatGetReturnWideAlt(CompilationUnit* cUnit)
{
  RegLocation res = locCReturnWide();
  res.lowReg = r2;
  res.highReg = r3;
  oatClobber(cUnit, r2);
  oatClobber(cUnit, r3);
  oatMarkInUse(cUnit, r2);
  oatMarkInUse(cUnit, r3);
  oatMarkPair(cUnit, res.lowReg, res.highReg);
  return res;
}

extern RegLocation oatGetReturnAlt(CompilationUnit* cUnit)
{
  RegLocation res = locCReturn();
  res.lowReg = r1;
  oatClobber(cUnit, r1);
  oatMarkInUse(cUnit, r1);
  return res;
}

extern RegisterInfo* oatGetRegInfo(CompilationUnit* cUnit, int reg)
{
  return ARM_FPREG(reg) ? &cUnit->regPool->FPRegs[reg & ARM_FP_REG_MASK]
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
