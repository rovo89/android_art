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

namespace art {

/*
 * This file contains codegen for the X86 ISA and is intended to be
 * includes by:
 *
 *        Codegen-$(TARGET_ARCH_VARIANT).c
 *
 */

/*
 * Alloc a pair of core registers, or a double.  Low reg in low byte,
 * high reg in next byte.
 */
int oatAllocTypedTempPair(CompilationUnit *cUnit, bool fpHint,
                          int regClass)
{
  int highReg;
  int lowReg;
  int res = 0;

  if (((regClass == kAnyReg) && fpHint) || (regClass == kFPReg)) {
    lowReg = oatAllocTempDouble(cUnit);
    highReg = lowReg + 1;
    res = (lowReg & 0xff) | ((highReg & 0xff) << 8);
    return res;
  }

  lowReg = oatAllocTemp(cUnit);
  highReg = oatAllocTemp(cUnit);
  res = (lowReg & 0xff) | ((highReg & 0xff) << 8);
  return res;
}

int oatAllocTypedTemp(CompilationUnit *cUnit, bool fpHint, int regClass) {
  if (((regClass == kAnyReg) && fpHint) || (regClass == kFPReg)) {
    return oatAllocTempFloat(cUnit);
  }
  return oatAllocTemp(cUnit);
}

void oatInitializeRegAlloc(CompilationUnit* cUnit) {
  int numRegs = sizeof(coreRegs)/sizeof(*coreRegs);
  int numReserved = sizeof(reservedRegs)/sizeof(*reservedRegs);
  int numTemps = sizeof(coreTemps)/sizeof(*coreTemps);
  int numFPRegs = sizeof(fpRegs)/sizeof(*fpRegs);
  int numFPTemps = sizeof(fpTemps)/sizeof(*fpTemps);
  RegisterPool *pool = (RegisterPool *)oatNew(cUnit, sizeof(*pool), true,
                                              kAllocRegAlloc);
  cUnit->regPool = pool;
  pool->numCoreRegs = numRegs;
  pool->coreRegs = (RegisterInfo *)
      oatNew(cUnit, numRegs * sizeof(*cUnit->regPool->coreRegs), true,
             kAllocRegAlloc);
  pool->numFPRegs = numFPRegs;
  pool->FPRegs = (RegisterInfo *)
      oatNew(cUnit, numFPRegs * sizeof(*cUnit->regPool->FPRegs), true,
             kAllocRegAlloc);
  oatInitPool(pool->coreRegs, coreRegs, pool->numCoreRegs);
  oatInitPool(pool->FPRegs, fpRegs, pool->numFPRegs);
  // Keep special registers from being allocated
  for (int i = 0; i < numReserved; i++) {
    oatMarkInUse(cUnit, reservedRegs[i]);
  }
  // Mark temp regs - all others not in use can be used for promotion
  for (int i = 0; i < numTemps; i++) {
    oatMarkTemp(cUnit, coreTemps[i]);
  }
  for (int i = 0; i < numFPTemps; i++) {
    oatMarkTemp(cUnit, fpTemps[i]);
  }
  // Construct the alias map.
  cUnit->phiAliasMap = (int*)oatNew(cUnit, cUnit->numSSARegs *
                                    sizeof(cUnit->phiAliasMap[0]), false,
                                    kAllocDFInfo);
  for (int i = 0; i < cUnit->numSSARegs; i++) {
    cUnit->phiAliasMap[i] = i;
  }
  for (MIR* phi = cUnit->phiList; phi; phi = phi->meta.phiNext) {
    int defReg = phi->ssaRep->defs[0];
    for (int i = 0; i < phi->ssaRep->numUses; i++) {
      for (int j = 0; j < cUnit->numSSARegs; j++) {
        if (cUnit->phiAliasMap[j] == phi->ssaRep->uses[i]) {
          cUnit->phiAliasMap[j] = defReg;
        }
      }
    }
  }
}

void freeRegLocTemps(CompilationUnit* cUnit, RegLocation rlKeep,
                     RegLocation rlFree)
{
  if ((rlFree.lowReg != rlKeep.lowReg) && (rlFree.lowReg != rlKeep.highReg) &&
      (rlFree.highReg != rlKeep.lowReg) && (rlFree.highReg != rlKeep.highReg)) {
    // No overlap, free both
    oatFreeTemp(cUnit, rlFree.lowReg);
    oatFreeTemp(cUnit, rlFree.highReg);
  }
}


}  // namespace art
