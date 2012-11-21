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

/* This file contains register alloction support. */

#include "../compiler_utility.h"
#include "../compiler_ir.h"
#include "../dataflow.h"
#include "ralloc_util.h"
#include "codegen_util.h"

namespace art {

/*
 * Free all allocated temps in the temp pools.  Note that this does
 * not affect the "liveness" of a temp register, which will stay
 * live until it is either explicitly killed or reallocated.
 */
void ResetRegPool(CompilationUnit* cUnit)
{
  int i;
  for (i=0; i < cUnit->regPool->numCoreRegs; i++) {
    if (cUnit->regPool->coreRegs[i].isTemp)
      cUnit->regPool->coreRegs[i].inUse = false;
  }
  for (i=0; i < cUnit->regPool->numFPRegs; i++) {
    if (cUnit->regPool->FPRegs[i].isTemp)
      cUnit->regPool->FPRegs[i].inUse = false;
  }
}

 /*
  * Set up temp & preserved register pools specialized by target.
  * Note: numRegs may be zero.
  */
void CompilerInitPool(RegisterInfo* regs, int* regNums, int num)
{
  int i;
  for (i=0; i < num; i++) {
    regs[i].reg = regNums[i];
    regs[i].inUse = false;
    regs[i].isTemp = false;
    regs[i].pair = false;
    regs[i].live = false;
    regs[i].dirty = false;
    regs[i].sReg = INVALID_SREG;
  }
}

static void DumpRegPool(RegisterInfo* p, int numRegs)
{
  LOG(INFO) << "================================================";
  for (int i = 0; i < numRegs; i++) {
    LOG(INFO) << StringPrintf(
        "R[%d]: T:%d, U:%d, P:%d, p:%d, LV:%d, D:%d, SR:%d, ST:%x, EN:%x",
        p[i].reg, p[i].isTemp, p[i].inUse, p[i].pair, p[i].partner,
        p[i].live, p[i].dirty, p[i].sReg, reinterpret_cast<uintptr_t>(p[i].defStart),
        reinterpret_cast<uintptr_t>(p[i].defEnd));
  }
  LOG(INFO) << "================================================";
}

void DumpCoreRegPool(CompilationUnit* cUnit)
{
  DumpRegPool(cUnit->regPool->coreRegs, cUnit->regPool->numCoreRegs);
}

void DumpFpRegPool(CompilationUnit* cUnit)
{
  DumpRegPool(cUnit->regPool->FPRegs, cUnit->regPool->numFPRegs);
}

/* Mark a temp register as dead.  Does not affect allocation state. */
static void ClobberBody(CompilationUnit *cUnit, RegisterInfo* p)
{
  if (p->isTemp) {
    DCHECK(!(p->live && p->dirty))  << "Live & dirty temp in clobber";
    p->live = false;
    p->sReg = INVALID_SREG;
    p->defStart = NULL;
    p->defEnd = NULL;
    if (p->pair) {
      p->pair = false;
      Clobber(cUnit, p->partner);
    }
  }
}

/* Mark a temp register as dead.  Does not affect allocation state. */
void Clobber(CompilationUnit* cUnit, int reg)
{
  ClobberBody(cUnit, GetRegInfo(cUnit, reg));
}

static void ClobberSRegBody(RegisterInfo* p, int numRegs, int sReg)
{
  int i;
  for (i=0; i< numRegs; i++) {
    if (p[i].sReg == sReg) {
      if (p[i].isTemp) {
        p[i].live = false;
      }
      p[i].defStart = NULL;
      p[i].defEnd = NULL;
    }
  }
}

/* Clobber any temp associated with an sReg.  Could be in either class */
void ClobberSReg(CompilationUnit* cUnit, int sReg)
{
#ifndef NDEBUG
  /* Reset live temp tracking sanity checker */
  if (sReg == cUnit->liveSReg) {
    cUnit->liveSReg = INVALID_SREG;
  }
#endif
  ClobberSRegBody(cUnit->regPool->coreRegs, cUnit->regPool->numCoreRegs, sReg);
  ClobberSRegBody(cUnit->regPool->FPRegs, cUnit->regPool->numFPRegs, sReg);
}

/*
 * SSA names associated with the initial definitions of Dalvik
 * registers are the same as the Dalvik register number (and
 * thus take the same position in the promotionMap.  However,
 * the special Method* and compiler temp resisters use negative
 * vReg numbers to distinguish them and can have an arbitrary
 * ssa name (above the last original Dalvik register).  This function
 * maps SSA names to positions in the promotionMap array.
 */
static int SRegToPMap(CompilationUnit* cUnit, int sReg)
{
  DCHECK_LT(sReg, cUnit->numSSARegs);
  DCHECK_GE(sReg, 0);
  int vReg = SRegToVReg(cUnit, sReg);
  if (vReg >= 0) {
    DCHECK_LT(vReg, cUnit->numDalvikRegisters);
    return vReg;
  } else {
    int pos = std::abs(vReg) - std::abs(SSA_METHOD_BASEREG);
    DCHECK_LE(pos, cUnit->numCompilerTemps);
    return cUnit->numDalvikRegisters + pos;
  }
}

void RecordCorePromotion(CompilationUnit* cUnit, int reg, int sReg)
{
  int pMapIdx = SRegToPMap(cUnit, sReg);
  int vReg = SRegToVReg(cUnit, sReg);
  GetRegInfo(cUnit, reg)->inUse = true;
  cUnit->coreSpillMask |= (1 << reg);
  // Include reg for later sort
  cUnit->coreVmapTable.push_back(reg << VREG_NUM_WIDTH |
                                 (vReg & ((1 << VREG_NUM_WIDTH) - 1)));
  cUnit->numCoreSpills++;
  cUnit->promotionMap[pMapIdx].coreLocation = kLocPhysReg;
  cUnit->promotionMap[pMapIdx].coreReg = reg;
}

/* Reserve a callee-save register.  Return -1 if none available */
static int AllocPreservedCoreReg(CompilationUnit* cUnit, int sReg)
{
  int res = -1;
  RegisterInfo* coreRegs = cUnit->regPool->coreRegs;
  for (int i = 0; i < cUnit->regPool->numCoreRegs; i++) {
    if (!coreRegs[i].isTemp && !coreRegs[i].inUse) {
      res = coreRegs[i].reg;
      RecordCorePromotion(cUnit, res, sReg);
      break;
    }
  }
  return res;
}

void RecordFpPromotion(CompilationUnit* cUnit, int reg, int sReg)
{
  int pMapIdx = SRegToPMap(cUnit, sReg);
  int vReg = SRegToVReg(cUnit, sReg);
  GetRegInfo(cUnit, reg)->inUse = true;
  MarkPreservedSingle(cUnit, vReg, reg);
  cUnit->promotionMap[pMapIdx].fpLocation = kLocPhysReg;
  cUnit->promotionMap[pMapIdx].FpReg = reg;
}

/*
 * Reserve a callee-save fp single register.  Try to fullfill request for
 * even/odd  allocation, but go ahead and allocate anything if not
 * available.  If nothing's available, return -1.
 */
static int AllocPreservedSingle(CompilationUnit* cUnit, int sReg, bool even)
{
  int res = -1;
  RegisterInfo* FPRegs = cUnit->regPool->FPRegs;
  for (int i = 0; i < cUnit->regPool->numFPRegs; i++) {
    if (!FPRegs[i].isTemp && !FPRegs[i].inUse &&
      ((FPRegs[i].reg & 0x1) == 0) == even) {
      res = FPRegs[i].reg;
      RecordFpPromotion(cUnit, res, sReg);
      break;
    }
  }
  return res;
}

/*
 * Somewhat messy code here.  We want to allocate a pair of contiguous
 * physical single-precision floating point registers starting with
 * an even numbered reg.  It is possible that the paired sReg (sReg+1)
 * has already been allocated - try to fit if possible.  Fail to
 * allocate if we can't meet the requirements for the pair of
 * sReg<=sX[even] & (sReg+1)<= sX+1.
 */
static int AllocPreservedDouble(CompilationUnit* cUnit, int sReg)
{
  int res = -1; // Assume failure
  int vReg = SRegToVReg(cUnit, sReg);
  int pMapIdx = SRegToPMap(cUnit, sReg);
  if (cUnit->promotionMap[pMapIdx+1].fpLocation == kLocPhysReg) {
    // Upper reg is already allocated.  Can we fit?
    int highReg = cUnit->promotionMap[pMapIdx+1].FpReg;
    if ((highReg & 1) == 0) {
      // High reg is even - fail.
      return res;
    }
    // Is the low reg of the pair free?
    RegisterInfo* p = GetRegInfo(cUnit, highReg-1);
    if (p->inUse || p->isTemp) {
      // Already allocated or not preserved - fail.
      return res;
    }
    // OK - good to go.
    res = p->reg;
    p->inUse = true;
    DCHECK_EQ((res & 1), 0);
    MarkPreservedSingle(cUnit, vReg, res);
  } else {
    RegisterInfo* FPRegs = cUnit->regPool->FPRegs;
    for (int i = 0; i < cUnit->regPool->numFPRegs; i++) {
      if (!FPRegs[i].isTemp && !FPRegs[i].inUse &&
        ((FPRegs[i].reg & 0x1) == 0x0) &&
        !FPRegs[i+1].isTemp && !FPRegs[i+1].inUse &&
        ((FPRegs[i+1].reg & 0x1) == 0x1) &&
        (FPRegs[i].reg + 1) == FPRegs[i+1].reg) {
        res = FPRegs[i].reg;
        FPRegs[i].inUse = true;
        MarkPreservedSingle(cUnit, vReg, res);
        FPRegs[i+1].inUse = true;
        DCHECK_EQ(res + 1, FPRegs[i+1].reg);
        MarkPreservedSingle(cUnit, vReg+1, res+1);
        break;
      }
    }
  }
  if (res != -1) {
    cUnit->promotionMap[pMapIdx].fpLocation = kLocPhysReg;
    cUnit->promotionMap[pMapIdx].FpReg = res;
    cUnit->promotionMap[pMapIdx+1].fpLocation = kLocPhysReg;
    cUnit->promotionMap[pMapIdx+1].FpReg = res + 1;
  }
  return res;
}


/*
 * Reserve a callee-save fp register.   If this register can be used
 * as the first of a double, attempt to allocate an even pair of fp
 * single regs (but if can't still attempt to allocate a single, preferring
 * first to allocate an odd register.
 */
static int AllocPreservedFPReg(CompilationUnit* cUnit, int sReg, bool doubleStart)
{
  int res = -1;
  if (doubleStart) {
    res = AllocPreservedDouble(cUnit, sReg);
  }
  if (res == -1) {
    res = AllocPreservedSingle(cUnit, sReg, false /* try odd # */);
  }
  if (res == -1)
    res = AllocPreservedSingle(cUnit, sReg, true /* try even # */);
  return res;
}

static int AllocTempBody(CompilationUnit* cUnit, RegisterInfo* p, int numRegs, int* nextTemp,
                          bool required)
{
  int i;
  int next = *nextTemp;
  for (i=0; i< numRegs; i++) {
    if (next >= numRegs)
      next = 0;
    if (p[next].isTemp && !p[next].inUse && !p[next].live) {
      Clobber(cUnit, p[next].reg);
      p[next].inUse = true;
      p[next].pair = false;
      *nextTemp = next + 1;
      return p[next].reg;
    }
    next++;
  }
  next = *nextTemp;
  for (i=0; i< numRegs; i++) {
    if (next >= numRegs)
      next = 0;
    if (p[next].isTemp && !p[next].inUse) {
      Clobber(cUnit, p[next].reg);
      p[next].inUse = true;
      p[next].pair = false;
      *nextTemp = next + 1;
      return p[next].reg;
    }
    next++;
  }
  if (required) {
    CodegenDump(cUnit);
    DumpRegPool(cUnit->regPool->coreRegs,
          cUnit->regPool->numCoreRegs);
    LOG(FATAL) << "No free temp registers";
  }
  return -1;  // No register available
}

//REDO: too many assumptions.
int AllocTempDouble(CompilationUnit* cUnit)
{
  RegisterInfo* p = cUnit->regPool->FPRegs;
  int numRegs = cUnit->regPool->numFPRegs;
  /* Start looking at an even reg */
  int next = cUnit->regPool->nextFPReg & ~0x1;

  // First try to avoid allocating live registers
  for (int i=0; i < numRegs; i+=2) {
    if (next >= numRegs)
      next = 0;
    if ((p[next].isTemp && !p[next].inUse && !p[next].live) &&
      (p[next+1].isTemp && !p[next+1].inUse && !p[next+1].live)) {
      Clobber(cUnit, p[next].reg);
      Clobber(cUnit, p[next+1].reg);
      p[next].inUse = true;
      p[next+1].inUse = true;
      DCHECK_EQ((p[next].reg+1), p[next+1].reg);
      DCHECK_EQ((p[next].reg & 0x1), 0);
      cUnit->regPool->nextFPReg = next + 2;
      if (cUnit->regPool->nextFPReg >= numRegs) {
        cUnit->regPool->nextFPReg = 0;
      }
      return p[next].reg;
    }
    next += 2;
  }
  next = cUnit->regPool->nextFPReg & ~0x1;

  // No choice - find a pair and kill it.
  for (int i=0; i < numRegs; i+=2) {
    if (next >= numRegs)
      next = 0;
    if (p[next].isTemp && !p[next].inUse && p[next+1].isTemp &&
      !p[next+1].inUse) {
      Clobber(cUnit, p[next].reg);
      Clobber(cUnit, p[next+1].reg);
      p[next].inUse = true;
      p[next+1].inUse = true;
      DCHECK_EQ((p[next].reg+1), p[next+1].reg);
      DCHECK_EQ((p[next].reg & 0x1), 0);
      cUnit->regPool->nextFPReg = next + 2;
      if (cUnit->regPool->nextFPReg >= numRegs) {
        cUnit->regPool->nextFPReg = 0;
      }
      return p[next].reg;
    }
    next += 2;
  }
  LOG(FATAL) << "No free temp registers (pair)";
  return -1;
}

/* Return a temp if one is available, -1 otherwise */
int AllocFreeTemp(CompilationUnit* cUnit)
{
  return AllocTempBody(cUnit, cUnit->regPool->coreRegs,
             cUnit->regPool->numCoreRegs,
             &cUnit->regPool->nextCoreReg, true);
}

int AllocTemp(CompilationUnit* cUnit)
{
  return AllocTempBody(cUnit, cUnit->regPool->coreRegs,
             cUnit->regPool->numCoreRegs,
             &cUnit->regPool->nextCoreReg, true);
}

int AllocTempFloat(CompilationUnit* cUnit)
{
  return AllocTempBody(cUnit, cUnit->regPool->FPRegs,
             cUnit->regPool->numFPRegs,
             &cUnit->regPool->nextFPReg, true);
}

static RegisterInfo* AllocLiveBody(RegisterInfo* p, int numRegs, int sReg)
{
  int i;
  if (sReg == -1)
    return NULL;
  for (i=0; i < numRegs; i++) {
    if (p[i].live && (p[i].sReg == sReg)) {
      if (p[i].isTemp)
        p[i].inUse = true;
      return &p[i];
    }
  }
  return NULL;
}

RegisterInfo* AllocLive(CompilationUnit* cUnit, int sReg, int regClass)
{
  RegisterInfo* res = NULL;
  switch (regClass) {
    case kAnyReg:
      res = AllocLiveBody(cUnit->regPool->FPRegs,
                cUnit->regPool->numFPRegs, sReg);
      if (res)
        break;
      /* Intentional fallthrough */
    case kCoreReg:
      res = AllocLiveBody(cUnit->regPool->coreRegs,
                cUnit->regPool->numCoreRegs, sReg);
      break;
    case kFPReg:
      res = AllocLiveBody(cUnit->regPool->FPRegs,
                cUnit->regPool->numFPRegs, sReg);
      break;
    default:
      LOG(FATAL) << "Invalid register type";
  }
  return res;
}

void FreeTemp(CompilationUnit* cUnit, int reg)
{
  RegisterInfo* p = cUnit->regPool->coreRegs;
  int numRegs = cUnit->regPool->numCoreRegs;
  int i;
  for (i=0; i< numRegs; i++) {
    if (p[i].reg == reg) {
      if (p[i].isTemp) {
        p[i].inUse = false;
      }
      p[i].pair = false;
      return;
    }
  }
  p = cUnit->regPool->FPRegs;
  numRegs = cUnit->regPool->numFPRegs;
  for (i=0; i< numRegs; i++) {
    if (p[i].reg == reg) {
      if (p[i].isTemp) {
        p[i].inUse = false;
      }
      p[i].pair = false;
      return;
    }
  }
  LOG(FATAL) << "Tried to free a non-existant temp: r" << reg;
}

RegisterInfo* IsLive(CompilationUnit* cUnit, int reg)
{
  RegisterInfo* p = cUnit->regPool->coreRegs;
  int numRegs = cUnit->regPool->numCoreRegs;
  int i;
  for (i=0; i< numRegs; i++) {
    if (p[i].reg == reg) {
      return p[i].live ? &p[i] : NULL;
    }
  }
  p = cUnit->regPool->FPRegs;
  numRegs = cUnit->regPool->numFPRegs;
  for (i=0; i< numRegs; i++) {
    if (p[i].reg == reg) {
      return p[i].live ? &p[i] : NULL;
    }
  }
  return NULL;
}

RegisterInfo* IsTemp(CompilationUnit* cUnit, int reg)
{
  RegisterInfo* p = GetRegInfo(cUnit, reg);
  return (p->isTemp) ? p : NULL;
}

RegisterInfo* IsPromoted(CompilationUnit* cUnit, int reg)
{
  RegisterInfo* p = GetRegInfo(cUnit, reg);
  return (p->isTemp) ? NULL : p;
}

bool IsDirty(CompilationUnit* cUnit, int reg)
{
  RegisterInfo* p = GetRegInfo(cUnit, reg);
  return p->dirty;
}

/*
 * Similar to AllocTemp(), but forces the allocation of a specific
 * register.  No check is made to see if the register was previously
 * allocated.  Use with caution.
 */
void LockTemp(CompilationUnit* cUnit, int reg)
{
  RegisterInfo* p = cUnit->regPool->coreRegs;
  int numRegs = cUnit->regPool->numCoreRegs;
  int i;
  for (i=0; i< numRegs; i++) {
    if (p[i].reg == reg) {
      DCHECK(p[i].isTemp);
      p[i].inUse = true;
      p[i].live = false;
      return;
    }
  }
  p = cUnit->regPool->FPRegs;
  numRegs = cUnit->regPool->numFPRegs;
  for (i=0; i< numRegs; i++) {
    if (p[i].reg == reg) {
      DCHECK(p[i].isTemp);
      p[i].inUse = true;
      p[i].live = false;
      return;
    }
  }
  LOG(FATAL) << "Tried to lock a non-existant temp: r" << reg;
}

static void ResetDefBody(RegisterInfo* p)
{
  p->defStart = NULL;
  p->defEnd = NULL;
}

void ResetDef(CompilationUnit* cUnit, int reg)
{
  ResetDefBody(GetRegInfo(cUnit, reg));
}

static void NullifyRange(CompilationUnit* cUnit, LIR *start, LIR *finish, int sReg1, int sReg2)
{
  if (start && finish) {
    LIR *p;
    DCHECK_EQ(sReg1, sReg2);
    for (p = start; ;p = p->next) {
      NopLIR(p);
      if (p == finish)
        break;
    }
  }
}

/*
 * Mark the beginning and end LIR of a def sequence.  Note that
 * on entry start points to the LIR prior to the beginning of the
 * sequence.
 */
void MarkDef(CompilationUnit* cUnit, RegLocation rl,
             LIR *start, LIR *finish)
{
  DCHECK(!rl.wide);
  DCHECK(start && start->next);
  DCHECK(finish);
  RegisterInfo* p = GetRegInfo(cUnit, rl.lowReg);
  p->defStart = start->next;
  p->defEnd = finish;
}

/*
 * Mark the beginning and end LIR of a def sequence.  Note that
 * on entry start points to the LIR prior to the beginning of the
 * sequence.
 */
void MarkDefWide(CompilationUnit* cUnit, RegLocation rl,
               LIR *start, LIR *finish)
{
  DCHECK(rl.wide);
  DCHECK(start && start->next);
  DCHECK(finish);
  RegisterInfo* p = GetRegInfo(cUnit, rl.lowReg);
  ResetDef(cUnit, rl.highReg);  // Only track low of pair
  p->defStart = start->next;
  p->defEnd = finish;
}

RegLocation WideToNarrow(CompilationUnit* cUnit, RegLocation rl)
{
  DCHECK(rl.wide);
  if (rl.location == kLocPhysReg) {
    RegisterInfo* infoLo = GetRegInfo(cUnit, rl.lowReg);
    RegisterInfo* infoHi = GetRegInfo(cUnit, rl.highReg);
    if (infoLo->isTemp) {
      infoLo->pair = false;
      infoLo->defStart = NULL;
      infoLo->defEnd = NULL;
    }
    if (infoHi->isTemp) {
      infoHi->pair = false;
      infoHi->defStart = NULL;
      infoHi->defEnd = NULL;
    }
  }
  rl.wide = false;
  return rl;
}

void ResetDefLoc(CompilationUnit* cUnit, RegLocation rl)
{
  DCHECK(!rl.wide);
  RegisterInfo* p = IsTemp(cUnit, rl.lowReg);
  if (p && !(cUnit->disableOpt & (1 << kSuppressLoads))) {
    DCHECK(!p->pair);
    NullifyRange(cUnit, p->defStart, p->defEnd, p->sReg, rl.sRegLow);
  }
  ResetDef(cUnit, rl.lowReg);
}

void ResetDefLocWide(CompilationUnit* cUnit, RegLocation rl)
{
  DCHECK(rl.wide);
  RegisterInfo* pLow = IsTemp(cUnit, rl.lowReg);
  RegisterInfo* pHigh = IsTemp(cUnit, rl.highReg);
  if (pLow && !(cUnit->disableOpt & (1 << kSuppressLoads))) {
    DCHECK(pLow->pair);
    NullifyRange(cUnit, pLow->defStart, pLow->defEnd, pLow->sReg, rl.sRegLow);
  }
  if (pHigh && !(cUnit->disableOpt & (1 << kSuppressLoads))) {
    DCHECK(pHigh->pair);
  }
  ResetDef(cUnit, rl.lowReg);
  ResetDef(cUnit, rl.highReg);
}

void ResetDefTracking(CompilationUnit* cUnit)
{
  int i;
  for (i=0; i< cUnit->regPool->numCoreRegs; i++) {
    ResetDefBody(&cUnit->regPool->coreRegs[i]);
  }
  for (i=0; i< cUnit->regPool->numFPRegs; i++) {
    ResetDefBody(&cUnit->regPool->FPRegs[i]);
  }
}

void ClobberAllRegs(CompilationUnit* cUnit)
{
  int i;
  for (i=0; i< cUnit->regPool->numCoreRegs; i++) {
    ClobberBody(cUnit, &cUnit->regPool->coreRegs[i]);
  }
  for (i=0; i< cUnit->regPool->numFPRegs; i++) {
    ClobberBody(cUnit, &cUnit->regPool->FPRegs[i]);
  }
}

// Make sure nothing is live and dirty
static void FlushAllRegsBody(CompilationUnit* cUnit, RegisterInfo* info, int numRegs)
{
  int i;
  for (i=0; i < numRegs; i++) {
    if (info[i].live && info[i].dirty) {
      if (info[i].pair) {
        FlushRegWide(cUnit, info[i].reg, info[i].partner);
      } else {
        FlushReg(cUnit, info[i].reg);
      }
    }
  }
}

void FlushAllRegs(CompilationUnit* cUnit)
{
  FlushAllRegsBody(cUnit, cUnit->regPool->coreRegs,
           cUnit->regPool->numCoreRegs);
  FlushAllRegsBody(cUnit, cUnit->regPool->FPRegs,
           cUnit->regPool->numFPRegs);
  ClobberAllRegs(cUnit);
}


//TUNING: rewrite all of this reg stuff.  Probably use an attribute table
static bool RegClassMatches(int regClass, int reg)
{
  if (regClass == kAnyReg) {
    return true;
  } else if (regClass == kCoreReg) {
    return !IsFpReg(reg);
  } else {
    return IsFpReg(reg);
  }
}

void MarkLive(CompilationUnit* cUnit, int reg, int sReg)
{
  RegisterInfo* info = GetRegInfo(cUnit, reg);
  if ((info->reg == reg) && (info->sReg == sReg) && info->live) {
    return;  /* already live */
  } else if (sReg != INVALID_SREG) {
    ClobberSReg(cUnit, sReg);
    if (info->isTemp) {
      info->live = true;
    }
  } else {
    /* Can't be live if no associated sReg */
    DCHECK(info->isTemp);
    info->live = false;
  }
  info->sReg = sReg;
}

void MarkTemp(CompilationUnit* cUnit, int reg)
{
  RegisterInfo* info = GetRegInfo(cUnit, reg);
  info->isTemp = true;
}

void UnmarkTemp(CompilationUnit* cUnit, int reg)
{
  RegisterInfo* info = GetRegInfo(cUnit, reg);
  info->isTemp = false;
}

void MarkPair(CompilationUnit* cUnit, int lowReg, int highReg)
{
  RegisterInfo* infoLo = GetRegInfo(cUnit, lowReg);
  RegisterInfo* infoHi = GetRegInfo(cUnit, highReg);
  infoLo->pair = infoHi->pair = true;
  infoLo->partner = highReg;
  infoHi->partner = lowReg;
}

void MarkClean(CompilationUnit* cUnit, RegLocation loc)
{
  RegisterInfo* info = GetRegInfo(cUnit, loc.lowReg);
  info->dirty = false;
  if (loc.wide) {
    info = GetRegInfo(cUnit, loc.highReg);
    info->dirty = false;
  }
}

void MarkDirty(CompilationUnit* cUnit, RegLocation loc)
{
  if (loc.home) {
    // If already home, can't be dirty
    return;
  }
  RegisterInfo* info = GetRegInfo(cUnit, loc.lowReg);
  info->dirty = true;
  if (loc.wide) {
    info = GetRegInfo(cUnit, loc.highReg);
    info->dirty = true;
  }
}

void MarkInUse(CompilationUnit* cUnit, int reg)
{
    RegisterInfo* info = GetRegInfo(cUnit, reg);
    info->inUse = true;
}

static void CopyRegInfo(CompilationUnit* cUnit, int newReg, int oldReg)
{
  RegisterInfo* newInfo = GetRegInfo(cUnit, newReg);
  RegisterInfo* oldInfo = GetRegInfo(cUnit, oldReg);
  // Target temp status must not change
  bool isTemp = newInfo->isTemp;
  *newInfo = *oldInfo;
  // Restore target's temp status
  newInfo->isTemp = isTemp;
  newInfo->reg = newReg;
}

static bool CheckCorePoolSanity(CompilationUnit* cUnit)
{
   for (static int i = 0; i < cUnit->regPool->numCoreRegs; i++) {
     if (cUnit->regPool->coreRegs[i].pair) {
       static int myReg = cUnit->regPool->coreRegs[i].reg;
       static int mySreg = cUnit->regPool->coreRegs[i].sReg;
       static int partnerReg = cUnit->regPool->coreRegs[i].partner;
       static RegisterInfo* partner = GetRegInfo(cUnit, partnerReg);
       DCHECK(partner != NULL);
       DCHECK(partner->pair);
       DCHECK_EQ(myReg, partner->partner);
       static int partnerSreg = partner->sReg;
       if (mySreg == INVALID_SREG) {
         DCHECK_EQ(partnerSreg, INVALID_SREG);
       } else {
         int diff = mySreg - partnerSreg;
         DCHECK((diff == -1) || (diff == 1));
       }
     }
     if (!cUnit->regPool->coreRegs[i].live) {
       DCHECK(cUnit->regPool->coreRegs[i].defStart == NULL);
       DCHECK(cUnit->regPool->coreRegs[i].defEnd == NULL);
     }
   }
   return true;
}

/*
 * Return an updated location record with current in-register status.
 * If the value lives in live temps, reflect that fact.  No code
 * is generated.  If the live value is part of an older pair,
 * clobber both low and high.
 * TUNING: clobbering both is a bit heavy-handed, but the alternative
 * is a bit complex when dealing with FP regs.  Examine code to see
 * if it's worthwhile trying to be more clever here.
 */

RegLocation UpdateLoc(CompilationUnit* cUnit, RegLocation loc)
{
  DCHECK(!loc.wide);
  DCHECK(CheckCorePoolSanity(cUnit));
  if (loc.location != kLocPhysReg) {
    DCHECK((loc.location == kLocDalvikFrame) ||
         (loc.location == kLocCompilerTemp));
    RegisterInfo* infoLo = AllocLive(cUnit, loc.sRegLow, kAnyReg);
    if (infoLo) {
      if (infoLo->pair) {
        Clobber(cUnit, infoLo->reg);
        Clobber(cUnit, infoLo->partner);
        FreeTemp(cUnit, infoLo->reg);
      } else {
        loc.lowReg = infoLo->reg;
        loc.location = kLocPhysReg;
      }
    }
  }

  return loc;
}

/* see comments for updateLoc */
RegLocation UpdateLocWide(CompilationUnit* cUnit, RegLocation loc)
{
  DCHECK(loc.wide);
  DCHECK(CheckCorePoolSanity(cUnit));
  if (loc.location != kLocPhysReg) {
    DCHECK((loc.location == kLocDalvikFrame) ||
         (loc.location == kLocCompilerTemp));
    // Are the dalvik regs already live in physical registers?
    RegisterInfo* infoLo = AllocLive(cUnit, loc.sRegLow, kAnyReg);
    RegisterInfo* infoHi = AllocLive(cUnit,
        oatSRegHi(loc.sRegLow), kAnyReg);
    bool match = true;
    match = match && (infoLo != NULL);
    match = match && (infoHi != NULL);
    // Are they both core or both FP?
    match = match && (IsFpReg(infoLo->reg) == IsFpReg(infoHi->reg));
    // If a pair of floating point singles, are they properly aligned?
    if (match && IsFpReg(infoLo->reg)) {
      match &= ((infoLo->reg & 0x1) == 0);
      match &= ((infoHi->reg - infoLo->reg) == 1);
    }
    // If previously used as a pair, it is the same pair?
    if (match && (infoLo->pair || infoHi->pair)) {
      match = (infoLo->pair == infoHi->pair);
      match &= ((infoLo->reg == infoHi->partner) &&
            (infoHi->reg == infoLo->partner));
    }
    if (match) {
      // Can reuse - update the register usage info
      loc.lowReg = infoLo->reg;
      loc.highReg = infoHi->reg;
      loc.location = kLocPhysReg;
      MarkPair(cUnit, loc.lowReg, loc.highReg);
      DCHECK(!IsFpReg(loc.lowReg) || ((loc.lowReg & 0x1) == 0));
      return loc;
    }
    // Can't easily reuse - clobber and free any overlaps
    if (infoLo) {
      Clobber(cUnit, infoLo->reg);
      FreeTemp(cUnit, infoLo->reg);
      if (infoLo->pair)
        Clobber(cUnit, infoLo->partner);
    }
    if (infoHi) {
      Clobber(cUnit, infoHi->reg);
      FreeTemp(cUnit, infoHi->reg);
      if (infoHi->pair)
        Clobber(cUnit, infoHi->partner);
    }
  }
  return loc;
}


/* For use in cases we don't know (or care) width */
RegLocation UpdateRawLoc(CompilationUnit* cUnit, RegLocation loc)
{
  if (loc.wide)
    return UpdateLocWide(cUnit, loc);
  else
    return UpdateLoc(cUnit, loc);
}

RegLocation EvalLocWide(CompilationUnit* cUnit, RegLocation loc, int regClass, bool update)
{
  DCHECK(loc.wide);
  int newRegs;
  int lowReg;
  int highReg;

  loc = UpdateLocWide(cUnit, loc);

  /* If already in registers, we can assume proper form.  Right reg class? */
  if (loc.location == kLocPhysReg) {
    DCHECK_EQ(IsFpReg(loc.lowReg), IsFpReg(loc.highReg));
    DCHECK(!IsFpReg(loc.lowReg) || ((loc.lowReg & 0x1) == 0));
    if (!RegClassMatches(regClass, loc.lowReg)) {
      /* Wrong register class.  Reallocate and copy */
      newRegs = AllocTypedTempPair(cUnit, loc.fp, regClass);
      lowReg = newRegs & 0xff;
      highReg = (newRegs >> 8) & 0xff;
      OpRegCopyWide(cUnit, lowReg, highReg, loc.lowReg,
                    loc.highReg);
      CopyRegInfo(cUnit, lowReg, loc.lowReg);
      CopyRegInfo(cUnit, highReg, loc.highReg);
      Clobber(cUnit, loc.lowReg);
      Clobber(cUnit, loc.highReg);
      loc.lowReg = lowReg;
      loc.highReg = highReg;
      MarkPair(cUnit, loc.lowReg, loc.highReg);
      DCHECK(!IsFpReg(loc.lowReg) || ((loc.lowReg & 0x1) == 0));
    }
    return loc;
  }

  DCHECK_NE(loc.sRegLow, INVALID_SREG);
  DCHECK_NE(oatSRegHi(loc.sRegLow), INVALID_SREG);

  newRegs = AllocTypedTempPair(cUnit, loc.fp, regClass);
  loc.lowReg = newRegs & 0xff;
  loc.highReg = (newRegs >> 8) & 0xff;

  MarkPair(cUnit, loc.lowReg, loc.highReg);
  if (update) {
    loc.location = kLocPhysReg;
    MarkLive(cUnit, loc.lowReg, loc.sRegLow);
    MarkLive(cUnit, loc.highReg, oatSRegHi(loc.sRegLow));
  }
  DCHECK(!IsFpReg(loc.lowReg) || ((loc.lowReg & 0x1) == 0));
  return loc;
}

RegLocation EvalLoc(CompilationUnit* cUnit, RegLocation loc,
                int regClass, bool update)
{
  int newReg;

  if (loc.wide)
    return EvalLocWide(cUnit, loc, regClass, update);

  loc = UpdateLoc(cUnit, loc);

  if (loc.location == kLocPhysReg) {
    if (!RegClassMatches(regClass, loc.lowReg)) {
      /* Wrong register class.  Realloc, copy and transfer ownership */
      newReg = AllocTypedTemp(cUnit, loc.fp, regClass);
      OpRegCopy(cUnit, newReg, loc.lowReg);
      CopyRegInfo(cUnit, newReg, loc.lowReg);
      Clobber(cUnit, loc.lowReg);
      loc.lowReg = newReg;
    }
    return loc;
  }

  DCHECK_NE(loc.sRegLow, INVALID_SREG);

  newReg = AllocTypedTemp(cUnit, loc.fp, regClass);
  loc.lowReg = newReg;

  if (update) {
    loc.location = kLocPhysReg;
    MarkLive(cUnit, loc.lowReg, loc.sRegLow);
  }
  return loc;
}

RegLocation GetRawSrc(CompilationUnit* cUnit, MIR* mir, int num)
{
  DCHECK(num < mir->ssaRep->numUses);
  RegLocation res = cUnit->regLocation[mir->ssaRep->uses[num]];
  return res;
}

RegLocation GetRawDest(CompilationUnit* cUnit, MIR* mir)
{
  DCHECK_GT(mir->ssaRep->numDefs, 0);
  RegLocation res = cUnit->regLocation[mir->ssaRep->defs[0]];
  return res;
}

RegLocation GetDest(CompilationUnit* cUnit, MIR* mir)
{
  RegLocation res = GetRawDest(cUnit, mir);
  DCHECK(!res.wide);
  return res;
}

RegLocation GetSrc(CompilationUnit* cUnit, MIR* mir, int num)
{
  RegLocation res = GetRawSrc(cUnit, mir, num);
  DCHECK(!res.wide);
  return res;
}

RegLocation GetDestWide(CompilationUnit* cUnit, MIR* mir)
{
  RegLocation res = GetRawDest(cUnit, mir);
  DCHECK(res.wide);
  return res;
}

RegLocation GetSrcWide(CompilationUnit* cUnit, MIR* mir,
                 int low)
{
  RegLocation res = GetRawSrc(cUnit, mir, low);
  DCHECK(res.wide);
  return res;
}

/* USE SSA names to count references of base Dalvik vRegs. */
static void CountRefs(CompilationUnit *cUnit, BasicBlock* bb, RefCounts* coreCounts,
                      RefCounts* fpCounts)
{
  if ((cUnit->disableOpt & (1 << kPromoteRegs)) ||
    !((bb->blockType == kEntryBlock) || (bb->blockType == kExitBlock) ||
      (bb->blockType == kDalvikByteCode))) {
    return;
  }
  for (int i = 0; i < cUnit->numSSARegs;) {
    RegLocation loc = cUnit->regLocation[i];
    RefCounts* counts = loc.fp ? fpCounts : coreCounts;
    int pMapIdx = SRegToPMap(cUnit, loc.sRegLow);
    if (loc.defined) {
      counts[pMapIdx].count += cUnit->useCounts.elemList[i];
    }
    if (loc.wide) {
      if (loc.defined) {
        if (loc.fp) {
          counts[pMapIdx].doubleStart = true;
          counts[pMapIdx+1].count += cUnit->useCounts.elemList[i+1];
        }
      }
      i += 2;
    } else {
      i++;
    }
  }
}

/* qsort callback function, sort descending */
static int SortCounts(const void *val1, const void *val2)
{
  const RefCounts* op1 = reinterpret_cast<const RefCounts*>(val1);
  const RefCounts* op2 = reinterpret_cast<const RefCounts*>(val2);
  return (op1->count == op2->count) ? 0 : (op1->count < op2->count ? 1 : -1);
}

static void DumpCounts(const RefCounts* arr, int size, const char* msg)
{
  LOG(INFO) << msg;
  for (int i = 0; i < size; i++) {
    LOG(INFO) << "sReg[" << arr[i].sReg << "]: " << arr[i].count;
  }
}

/*
 * Note: some portions of this code required even if the kPromoteRegs
 * optimization is disabled.
 */
void DoPromotion(CompilationUnit* cUnit)
{
  int regBias = cUnit->numCompilerTemps + 1;
  int dalvikRegs = cUnit->numDalvikRegisters;
  int numRegs = dalvikRegs + regBias;
  const int promotionThreshold = 2;

  // Allow target code to add any special registers
  AdjustSpillMask(cUnit);

  /*
   * Simple register promotion. Just do a static count of the uses
   * of Dalvik registers.  Note that we examine the SSA names, but
   * count based on original Dalvik register name.  Count refs
   * separately based on type in order to give allocation
   * preference to fp doubles - which must be allocated sequential
   * physical single fp registers started with an even-numbered
   * reg.
   * TUNING: replace with linear scan once we have the ability
   * to describe register live ranges for GC.
   */
  RefCounts *coreRegs = static_cast<RefCounts*>(NewMem(cUnit, sizeof(RefCounts) * numRegs,
                                                       true, kAllocRegAlloc));
  RefCounts *FpRegs = static_cast<RefCounts *>(NewMem(cUnit, sizeof(RefCounts) * numRegs,
                                                      true, kAllocRegAlloc));
  // Set ssa names for original Dalvik registers
  for (int i = 0; i < dalvikRegs; i++) {
    coreRegs[i].sReg = FpRegs[i].sReg = i;
  }
  // Set ssa name for Method*
  coreRegs[dalvikRegs].sReg = cUnit->methodSReg;
  FpRegs[dalvikRegs].sReg = cUnit->methodSReg;  // For consistecy
  // Set ssa names for compilerTemps
  for (int i = 1; i <= cUnit->numCompilerTemps; i++) {
    CompilerTemp* ct = reinterpret_cast<CompilerTemp*>(cUnit->compilerTemps.elemList[i]);
    coreRegs[dalvikRegs + i].sReg = ct->sReg;
    FpRegs[dalvikRegs + i].sReg = ct->sReg;
  }

  GrowableListIterator iterator;
  GrowableListIteratorInit(&cUnit->blockList, &iterator);
  while (true) {
    BasicBlock* bb;
    bb = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iterator));
    if (bb == NULL) break;
    CountRefs(cUnit, bb, coreRegs, FpRegs);
  }

  /*
   * Ideally, we'd allocate doubles starting with an even-numbered
   * register.  Bias the counts to try to allocate any vreg that's
   * used as the start of a pair first.
   */
  for (int i = 0; i < numRegs; i++) {
    if (FpRegs[i].doubleStart) {
      FpRegs[i].count *= 2;
    }
  }

  // Sort the count arrays
  qsort(coreRegs, numRegs, sizeof(RefCounts), SortCounts);
  qsort(FpRegs, numRegs, sizeof(RefCounts), SortCounts);

  if (cUnit->printMe) {
    DumpCounts(coreRegs, numRegs, "Core regs after sort");
    DumpCounts(FpRegs, numRegs, "Fp regs after sort");
  }

  if (!(cUnit->disableOpt & (1 << kPromoteRegs))) {
    // Promote FpRegs
    for (int i = 0; (i < numRegs) &&
            (FpRegs[i].count >= promotionThreshold ); i++) {
      int pMapIdx = SRegToPMap(cUnit, FpRegs[i].sReg);
      if (cUnit->promotionMap[pMapIdx].fpLocation != kLocPhysReg) {
        int reg = AllocPreservedFPReg(cUnit, FpRegs[i].sReg,
          FpRegs[i].doubleStart);
        if (reg < 0) {
          break;  // No more left
        }
      }
    }

    // Promote core regs
    for (int i = 0; (i < numRegs) &&
            (coreRegs[i].count > promotionThreshold); i++) {
      int pMapIdx = SRegToPMap(cUnit, coreRegs[i].sReg);
      if (cUnit->promotionMap[pMapIdx].coreLocation !=
          kLocPhysReg) {
        int reg = AllocPreservedCoreReg(cUnit, coreRegs[i].sReg);
        if (reg < 0) {
           break;  // No more left
        }
      }
    }
  } else if (cUnit->qdMode) {
    AllocPreservedCoreReg(cUnit, cUnit->methodSReg);
    for (int i = 0; i < numRegs; i++) {
      int reg = AllocPreservedCoreReg(cUnit, i);
      if (reg < 0) {
         break;  // No more left
      }
    }
  }


  // Now, update SSA names to new home locations
  for (int i = 0; i < cUnit->numSSARegs; i++) {
    RegLocation *curr = &cUnit->regLocation[i];
    int pMapIdx = SRegToPMap(cUnit, curr->sRegLow);
    if (!curr->wide) {
      if (curr->fp) {
        if (cUnit->promotionMap[pMapIdx].fpLocation == kLocPhysReg) {
          curr->location = kLocPhysReg;
          curr->lowReg = cUnit->promotionMap[pMapIdx].FpReg;
          curr->home = true;
        }
      } else {
        if (cUnit->promotionMap[pMapIdx].coreLocation == kLocPhysReg) {
          curr->location = kLocPhysReg;
          curr->lowReg = cUnit->promotionMap[pMapIdx].coreReg;
          curr->home = true;
        }
      }
      curr->highReg = INVALID_REG;
    } else {
      if (curr->highWord) {
        continue;
      }
      if (curr->fp) {
        if ((cUnit->promotionMap[pMapIdx].fpLocation == kLocPhysReg) &&
          (cUnit->promotionMap[pMapIdx+1].fpLocation ==
          kLocPhysReg)) {
          int lowReg = cUnit->promotionMap[pMapIdx].FpReg;
          int highReg = cUnit->promotionMap[pMapIdx+1].FpReg;
          // Doubles require pair of singles starting at even reg
          if (((lowReg & 0x1) == 0) && ((lowReg + 1) == highReg)) {
            curr->location = kLocPhysReg;
            curr->lowReg = lowReg;
            curr->highReg = highReg;
            curr->home = true;
          }
        }
      } else {
        if ((cUnit->promotionMap[pMapIdx].coreLocation == kLocPhysReg)
           && (cUnit->promotionMap[pMapIdx+1].coreLocation ==
           kLocPhysReg)) {
          curr->location = kLocPhysReg;
          curr->lowReg = cUnit->promotionMap[pMapIdx].coreReg;
          curr->highReg = cUnit->promotionMap[pMapIdx+1].coreReg;
          curr->home = true;
        }
      }
    }
  }
  if (cUnit->printMe) {
    DumpPromotionMap(cUnit);
  }
}

/* Returns sp-relative offset in bytes for a VReg */
int VRegOffset(CompilationUnit* cUnit, int vReg)
{
  return StackVisitor::GetVRegOffset(cUnit->code_item, cUnit->coreSpillMask,
                                     cUnit->fpSpillMask, cUnit->frameSize, vReg);
}

/* Returns sp-relative offset in bytes for a SReg */
int SRegOffset(CompilationUnit* cUnit, int sReg)
{
  return VRegOffset(cUnit, SRegToVReg(cUnit, sReg));
}

}  // namespace art
