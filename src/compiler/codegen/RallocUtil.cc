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
 * This file contains register alloction support and is intended to be
 * included by:
 *
 *        Codegen-$(TARGET_ARCH_VARIANT).c
 *
 */

#include "../CompilerUtility.h"
#include "../CompilerIR.h"
#include "../Dataflow.h"
#include "Ralloc.h"

namespace art {

#define SREG(c, s) ((c)->regLocation[(s)].sRegLow)
/*
 * Get the "real" sreg number associated with an sReg slot.  In general,
 * sReg values passed through codegen are the SSA names created by
 * dataflow analysis and refer to slot numbers in the cUnit->regLocation
 * array.  However, renaming is accomplished by simply replacing RegLocation
 * entries in the cUnit->reglocation[] array.  Therefore, when location
 * records for operands are first created, we need to ask the locRecord
 * identified by the dataflow pass what it's new name is.
 */

/*
 * Free all allocated temps in the temp pools.  Note that this does
 * not affect the "liveness" of a temp register, which will stay
 * live until it is either explicitly killed or reallocated.
 */
extern void oatResetRegPool(CompilationUnit* cUnit)
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
extern void oatInitPool(RegisterInfo* regs, int* regNums, int num)
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

void dumpRegPool(RegisterInfo* p, int numRegs)
{
    int i;
    LOG(INFO) << "================================================";
    for (i=0; i < numRegs; i++ ){
        char buf[100];
        snprintf(buf, 100,
            "R[%d]: T:%d, U:%d, P:%d, p:%d, LV:%d, D:%d, SR:%d, ST:%x, EN:%x",
            p[i].reg, p[i].isTemp, p[i].inUse, p[i].pair, p[i].partner,
            p[i].live, p[i].dirty, p[i].sReg,(int)p[i].defStart,
            (int)p[i].defEnd);
    LOG(INFO) << buf;
    }
    LOG(INFO) << "================================================";
}

void oatDumpCoreRegPool(CompilationUnit* cUnit)
{
    dumpRegPool(cUnit->regPool->coreRegs, cUnit->regPool->numCoreRegs);
}

void oatDumpFpRegPool(CompilationUnit* cUnit)
{
    dumpRegPool(cUnit->regPool->FPRegs, cUnit->regPool->numFPRegs);
}

/* Mark a temp register as dead.  Does not affect allocation state. */
static inline void clobberBody(CompilationUnit *cUnit, RegisterInfo* p)
{
    if (p->isTemp) {
        DCHECK(!(p->live && p->dirty))  << "Live & dirty temp in clobber";
        p->live = false;
        p->sReg = INVALID_SREG;
        p->defStart = NULL;
        p->defEnd = NULL;
        if (p->pair) {
            p->pair = false;
            oatClobber(cUnit, p->partner);
        }
    }
}

/* Mark a temp register as dead.  Does not affect allocation state. */
void oatClobber(CompilationUnit* cUnit, int reg)
{
    clobberBody(cUnit, oatGetRegInfo(cUnit, reg));
}

void clobberSRegBody(RegisterInfo* p, int numRegs, int sReg)
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
extern void oatClobberSReg(CompilationUnit* cUnit, int sReg)
{
    clobberSRegBody(cUnit->regPool->coreRegs, cUnit->regPool->numCoreRegs,
                    sReg);
    clobberSRegBody(cUnit->regPool->FPRegs, cUnit->regPool->numFPRegs,
                    sReg);
}

/* Reserve a callee-save register.  Return -1 if none available */
extern int oatAllocPreservedCoreReg(CompilationUnit* cUnit, int sReg)
{
    int res = -1;
    RegisterInfo* coreRegs = cUnit->regPool->coreRegs;
    for (int i = 0; i < cUnit->regPool->numCoreRegs; i++) {
        if (!coreRegs[i].isTemp && !coreRegs[i].inUse) {
            res = coreRegs[i].reg;
            coreRegs[i].inUse = true;
            cUnit->coreSpillMask |= (1 << res);
            cUnit->coreVmapTable.push_back(sReg);
            cUnit->numCoreSpills++;
            //  Should be promoting based on initial sReg set
            DCHECK_EQ(sReg, oatS2VReg(cUnit, sReg));
            cUnit->promotionMap[sReg].coreLocation = kLocPhysReg;
            cUnit->promotionMap[sReg].coreReg = res;
            break;
        }
    }
    return res;
}

/*
 * Reserve a callee-save fp single register.  Try to fullfill request for
 * even/odd  allocation, but go ahead and allocate anything if not
 * available.  If nothing's available, return -1.
 */
int allocPreservedSingle(CompilationUnit* cUnit, int sReg, bool even)
{
    int res = -1;
    RegisterInfo* FPRegs = cUnit->regPool->FPRegs;
    for (int i = 0; i < cUnit->regPool->numFPRegs; i++) {
        if (!FPRegs[i].isTemp && !FPRegs[i].inUse &&
            ((FPRegs[i].reg & 0x1) == 0) == even) {
            res = FPRegs[i].reg;
            FPRegs[i].inUse = true;
            //  Should be promoting based on initial sReg set
            DCHECK_EQ(sReg, oatS2VReg(cUnit, sReg));
            oatMarkPreservedSingle(cUnit, sReg, res);
            cUnit->promotionMap[sReg].fpLocation = kLocPhysReg;
            cUnit->promotionMap[sReg].fpReg = res;
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
int allocPreservedDouble(CompilationUnit* cUnit, int sReg)
{
    int res = -1; // Assume failure
    //  Should be promoting based on initial sReg set
    DCHECK_EQ(sReg, oatS2VReg(cUnit, sReg));
    if (cUnit->promotionMap[sReg+1].fpLocation == kLocPhysReg) {
        // Upper reg is already allocated.  Can we fit?
        int highReg = cUnit->promotionMap[sReg+1].fpReg;
        if ((highReg & 1) == 0) {
            // High reg is even - fail.
            return res;
        }
        // Is the low reg of the pair free?
        RegisterInfo* p = oatGetRegInfo(cUnit, highReg-1);
        if (p->inUse || p->isTemp) {
            // Already allocated or not preserved - fail.
            return res;
        }
        // OK - good to go.
        res = p->reg;
        p->inUse = true;
        DCHECK_EQ((res & 1), 0);
        oatMarkPreservedSingle(cUnit, sReg, res);
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
                oatMarkPreservedSingle(cUnit, sReg, res);
                FPRegs[i+1].inUse = true;
                DCHECK_EQ(res + 1, FPRegs[i+1].reg);
                oatMarkPreservedSingle(cUnit, sReg+1, res+1);
                break;
            }
        }
    }
    if (res != -1) {
        cUnit->promotionMap[sReg].fpLocation = kLocPhysReg;
        cUnit->promotionMap[sReg].fpReg = res;
        cUnit->promotionMap[sReg+1].fpLocation = kLocPhysReg;
        cUnit->promotionMap[sReg+1].fpReg = res + 1;
    }
    return res;
}


/*
 * Reserve a callee-save fp register.   If this register can be used
 * as the first of a double, attempt to allocate an even pair of fp
 * single regs (but if can't still attempt to allocate a single, preferring
 * first to allocate an odd register.
 */
extern int oatAllocPreservedFPReg(CompilationUnit* cUnit, int sReg,
                                  bool doubleStart)
{
    int res = -1;
    if (doubleStart) {
        res = allocPreservedDouble(cUnit, sReg);
    }
    if (res == -1) {
        res = allocPreservedSingle(cUnit, sReg, false /* try odd # */);
    }
    if (res == -1)
        res = allocPreservedSingle(cUnit, sReg, true /* try even # */);
    return res;
}

int allocTempBody(CompilationUnit* cUnit, RegisterInfo* p, int numRegs,
                  int* nextTemp, bool required)
{
    int i;
    int next = *nextTemp;
    for (i=0; i< numRegs; i++) {
        if (next >= numRegs)
            next = 0;
        if (p[next].isTemp && !p[next].inUse && !p[next].live) {
            oatClobber(cUnit, p[next].reg);
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
            oatClobber(cUnit, p[next].reg);
            p[next].inUse = true;
            p[next].pair = false;
            *nextTemp = next + 1;
            return p[next].reg;
        }
        next++;
    }
    if (required) {
        oatCodegenDump(cUnit);
        dumpRegPool(cUnit->regPool->coreRegs,
                    cUnit->regPool->numCoreRegs);
        LOG(FATAL) << "No free temp registers";
    }
    return -1;  // No register available
}

//REDO: too many assumptions.
extern int oatAllocTempDouble(CompilationUnit* cUnit)
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
            oatClobber(cUnit, p[next].reg);
            oatClobber(cUnit, p[next+1].reg);
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
            oatClobber(cUnit, p[next].reg);
            oatClobber(cUnit, p[next+1].reg);
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
extern int oatAllocFreeTemp(CompilationUnit* cUnit)
{
    return allocTempBody(cUnit, cUnit->regPool->coreRegs,
                         cUnit->regPool->numCoreRegs,
                         &cUnit->regPool->nextCoreReg, true);
}

extern int oatAllocTemp(CompilationUnit* cUnit)
{
    return allocTempBody(cUnit, cUnit->regPool->coreRegs,
                         cUnit->regPool->numCoreRegs,
                         &cUnit->regPool->nextCoreReg, true);
}

extern int oatAllocTempFloat(CompilationUnit* cUnit)
{
    return allocTempBody(cUnit, cUnit->regPool->FPRegs,
                         cUnit->regPool->numFPRegs,
                         &cUnit->regPool->nextFPReg, true);
}

RegisterInfo* allocLiveBody(RegisterInfo* p, int numRegs, int sReg)
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

RegisterInfo* allocLive(CompilationUnit* cUnit, int sReg, int regClass)
{
    RegisterInfo* res = NULL;
    switch(regClass) {
        case kAnyReg:
            res = allocLiveBody(cUnit->regPool->FPRegs,
                                cUnit->regPool->numFPRegs, sReg);
            if (res)
                break;
            /* Intentional fallthrough */
        case kCoreReg:
            res = allocLiveBody(cUnit->regPool->coreRegs,
                                cUnit->regPool->numCoreRegs, sReg);
            break;
        case kFPReg:
            res = allocLiveBody(cUnit->regPool->FPRegs,
                                cUnit->regPool->numFPRegs, sReg);
            break;
        default:
            LOG(FATAL) << "Invalid register type";
    }
    return res;
}

extern void oatFreeTemp(CompilationUnit* cUnit, int reg)
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

extern RegisterInfo* oatIsLive(CompilationUnit* cUnit, int reg)
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

extern RegisterInfo* oatIsTemp(CompilationUnit* cUnit, int reg)
{
    RegisterInfo* p = oatGetRegInfo(cUnit, reg);
    return (p->isTemp) ? p : NULL;
}

extern RegisterInfo* oatIsPromoted(CompilationUnit* cUnit, int reg)
{
    RegisterInfo* p = oatGetRegInfo(cUnit, reg);
    return (p->isTemp) ? NULL : p;
}

extern bool oatIsDirty(CompilationUnit* cUnit, int reg)
{
    RegisterInfo* p = oatGetRegInfo(cUnit, reg);
    return p->dirty;
}

/*
 * Similar to oatAllocTemp(), but forces the allocation of a specific
 * register.  No check is made to see if the register was previously
 * allocated.  Use with caution.
 */
extern void oatLockTemp(CompilationUnit* cUnit, int reg)
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

static inline void resetDefBody(RegisterInfo* p)
{
    p->defStart = NULL;
    p->defEnd = NULL;
}

extern void oatResetDef(CompilationUnit* cUnit, int reg)
{
    resetDefBody(oatGetRegInfo(cUnit, reg));
}

void nullifyRange(CompilationUnit* cUnit, LIR *start, LIR *finish,
                         int sReg1, int sReg2)
{
    if (start && finish) {
        LIR *p;
        DCHECK_EQ(sReg1, sReg2);
        for (p = start; ;p = p->next) {
            oatNopLIR(p);
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
extern void oatMarkDef(CompilationUnit* cUnit, RegLocation rl,
                       LIR *start, LIR *finish)
{
    DCHECK(!rl.wide);
    DCHECK(start && start->next);
    DCHECK(finish);
    RegisterInfo* p = oatGetRegInfo(cUnit, rl.lowReg);
    p->defStart = start->next;
    p->defEnd = finish;
}

/*
 * Mark the beginning and end LIR of a def sequence.  Note that
 * on entry start points to the LIR prior to the beginning of the
 * sequence.
 */
extern void oatMarkDefWide(CompilationUnit* cUnit, RegLocation rl,
                           LIR *start, LIR *finish)
{
    DCHECK(rl.wide);
    DCHECK(start && start->next);
    DCHECK(finish);
    RegisterInfo* p = oatGetRegInfo(cUnit, rl.lowReg);
    oatResetDef(cUnit, rl.highReg);  // Only track low of pair
    p->defStart = start->next;
    p->defEnd = finish;
}

extern RegLocation oatWideToNarrow(CompilationUnit* cUnit, RegLocation rl)
{
    DCHECK(rl.wide);
    if (rl.location == kLocPhysReg) {
        RegisterInfo* infoLo = oatGetRegInfo(cUnit, rl.lowReg);
        RegisterInfo* infoHi = oatGetRegInfo(cUnit, rl.highReg);
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

extern void oatResetDefLoc(CompilationUnit* cUnit, RegLocation rl)
{
    DCHECK(!rl.wide);
    RegisterInfo* p = oatIsTemp(cUnit, rl.lowReg);
    if (p && !(cUnit->disableOpt & (1 << kSuppressLoads))) {
        DCHECK(!p->pair);
        nullifyRange(cUnit, p->defStart, p->defEnd,
                     p->sReg, rl.sRegLow);
    }
    oatResetDef(cUnit, rl.lowReg);
}

extern void oatResetDefLocWide(CompilationUnit* cUnit, RegLocation rl)
{
    DCHECK(rl.wide);
    RegisterInfo* pLow = oatIsTemp(cUnit, rl.lowReg);
    RegisterInfo* pHigh = oatIsTemp(cUnit, rl.highReg);
    if (pLow && !(cUnit->disableOpt & (1 << kSuppressLoads))) {
        DCHECK(pLow->pair);
        nullifyRange(cUnit, pLow->defStart, pLow->defEnd,
                     pLow->sReg, rl.sRegLow);
    }
    if (pHigh && !(cUnit->disableOpt & (1 << kSuppressLoads))) {
        DCHECK(pHigh->pair);
    }
    oatResetDef(cUnit, rl.lowReg);
    oatResetDef(cUnit, rl.highReg);
}

extern void oatResetDefTracking(CompilationUnit* cUnit)
{
    int i;
    for (i=0; i< cUnit->regPool->numCoreRegs; i++) {
        resetDefBody(&cUnit->regPool->coreRegs[i]);
    }
    for (i=0; i< cUnit->regPool->numFPRegs; i++) {
        resetDefBody(&cUnit->regPool->FPRegs[i]);
    }
}

extern void oatClobberAllRegs(CompilationUnit* cUnit)
{
    int i;
    for (i=0; i< cUnit->regPool->numCoreRegs; i++) {
        clobberBody(cUnit, &cUnit->regPool->coreRegs[i]);
    }
    for (i=0; i< cUnit->regPool->numFPRegs; i++) {
        clobberBody(cUnit, &cUnit->regPool->FPRegs[i]);
    }
}

// Make sure nothing is live and dirty
void flushAllRegsBody(CompilationUnit* cUnit, RegisterInfo* info,
                             int numRegs)
{
    int i;
    for (i=0; i < numRegs; i++) {
        if (info[i].live && info[i].dirty) {
            if (info[i].pair) {
                oatFlushRegWide(cUnit, info[i].reg, info[i].partner);
            } else {
                oatFlushReg(cUnit, info[i].reg);
            }
        }
    }
}

extern void oatFlushAllRegs(CompilationUnit* cUnit)
{
    flushAllRegsBody(cUnit, cUnit->regPool->coreRegs,
                     cUnit->regPool->numCoreRegs);
    flushAllRegsBody(cUnit, cUnit->regPool->FPRegs,
                     cUnit->regPool->numFPRegs);
    oatClobberAllRegs(cUnit);
}


//TUNING: rewrite all of this reg stuff.  Probably use an attribute table
bool regClassMatches(int regClass, int reg)
{
    if (regClass == kAnyReg) {
        return true;
    } else if (regClass == kCoreReg) {
        return !oatIsFpReg(reg);
    } else {
        return oatIsFpReg(reg);
    }
}

extern void oatMarkLive(CompilationUnit* cUnit, int reg, int sReg)
{
    RegisterInfo* info = oatGetRegInfo(cUnit, reg);
    if ((info->reg == reg) && (info->sReg == sReg) && info->live) {
        return;  /* already live */
    } else if (sReg != INVALID_SREG) {
        oatClobberSReg(cUnit, sReg);
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

extern void oatMarkTemp(CompilationUnit* cUnit, int reg)
{
    RegisterInfo* info = oatGetRegInfo(cUnit, reg);
    info->isTemp = true;
}

extern void oatUnmarkTemp(CompilationUnit* cUnit, int reg)
{
    RegisterInfo* info = oatGetRegInfo(cUnit, reg);
    info->isTemp = false;
}

extern void oatMarkPair(CompilationUnit* cUnit, int lowReg, int highReg)
{
    RegisterInfo* infoLo = oatGetRegInfo(cUnit, lowReg);
    RegisterInfo* infoHi = oatGetRegInfo(cUnit, highReg);
    infoLo->pair = infoHi->pair = true;
    infoLo->partner = highReg;
    infoHi->partner = lowReg;
}

extern void oatMarkClean(CompilationUnit* cUnit, RegLocation loc)
{
    RegisterInfo* info = oatGetRegInfo(cUnit, loc.lowReg);
    info->dirty = false;
    if (loc.wide) {
        info = oatGetRegInfo(cUnit, loc.highReg);
        info->dirty = false;
    }
}

extern void oatMarkDirty(CompilationUnit* cUnit, RegLocation loc)
{
    if (loc.home) {
        // If already home, can't be dirty
        return;
    }
    RegisterInfo* info = oatGetRegInfo(cUnit, loc.lowReg);
    info->dirty = true;
    if (loc.wide) {
        info = oatGetRegInfo(cUnit, loc.highReg);
        info->dirty = true;
    }
}

extern void oatMarkInUse(CompilationUnit* cUnit, int reg)
{
      RegisterInfo* info = oatGetRegInfo(cUnit, reg);
      info->inUse = true;
}

void copyRegInfo(CompilationUnit* cUnit, int newReg, int oldReg)
{
    RegisterInfo* newInfo = oatGetRegInfo(cUnit, newReg);
    RegisterInfo* oldInfo = oatGetRegInfo(cUnit, oldReg);
    // Target temp status must not change
    bool isTemp = newInfo->isTemp;
    *newInfo = *oldInfo;
    // Restore target's temp status
    newInfo->isTemp = isTemp;
    newInfo->reg = newReg;
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

extern RegLocation oatUpdateLoc(CompilationUnit* cUnit, RegLocation loc)
{
    DCHECK(!loc.wide);
    DCHECK(oatCheckCorePoolSanity(cUnit));
    if (loc.location == kLocDalvikFrame) {
        RegisterInfo* infoLo = allocLive(cUnit, loc.sRegLow, kAnyReg);
        if (infoLo) {
            if (infoLo->pair) {
                oatClobber(cUnit, infoLo->reg);
                oatClobber(cUnit, infoLo->partner);
                oatFreeTemp(cUnit, infoLo->reg);
            } else {
                loc.lowReg = infoLo->reg;
                loc.location = kLocPhysReg;
            }
        }
    }

    return loc;
}

bool oatCheckCorePoolSanity(CompilationUnit* cUnit)
{
   for (static int i = 0; i < cUnit->regPool->numCoreRegs; i++) {
       if (cUnit->regPool->coreRegs[i].pair) {
           static int myReg = cUnit->regPool->coreRegs[i].reg;
           static int mySreg = cUnit->regPool->coreRegs[i].sReg;
           static int partnerReg = cUnit->regPool->coreRegs[i].partner;
           static RegisterInfo* partner = oatGetRegInfo(cUnit, partnerReg);
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

/* see comments for updateLoc */
extern RegLocation oatUpdateLocWide(CompilationUnit* cUnit, RegLocation loc)
{
    DCHECK(loc.wide);
    DCHECK(oatCheckCorePoolSanity(cUnit));
    if (loc.location == kLocDalvikFrame) {
        // Are the dalvik regs already live in physical registers?
        RegisterInfo* infoLo = allocLive(cUnit, loc.sRegLow, kAnyReg);
        RegisterInfo* infoHi = allocLive(cUnit,
              oatSRegHi(loc.sRegLow), kAnyReg);
        bool match = true;
        match = match && (infoLo != NULL);
        match = match && (infoHi != NULL);
        // Are they both core or both FP?
        match = match && (oatIsFpReg(infoLo->reg) == oatIsFpReg(infoHi->reg));
        // If a pair of floating point singles, are they properly aligned?
        if (match && oatIsFpReg(infoLo->reg)) {
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
            oatMarkPair(cUnit, loc.lowReg, loc.highReg);
            DCHECK(!oatIsFpReg(loc.lowReg) || ((loc.lowReg & 0x1) == 0));
            return loc;
        }
        // Can't easily reuse - clobber and free any overlaps
        if (infoLo) {
            oatClobber(cUnit, infoLo->reg);
            oatFreeTemp(cUnit, infoLo->reg);
            if (infoLo->pair)
                oatClobber(cUnit, infoLo->partner);
        }
        if (infoHi) {
            oatClobber(cUnit, infoHi->reg);
            oatFreeTemp(cUnit, infoHi->reg);
            if (infoHi->pair)
                oatClobber(cUnit, infoHi->partner);
        }
    }
    return loc;
}


/* For use in cases we don't know (or care) width */
extern RegLocation oatUpdateRawLoc(CompilationUnit* cUnit, RegLocation loc)
{
    if (loc.wide)
        return oatUpdateLocWide(cUnit, loc);
    else
        return oatUpdateLoc(cUnit, loc);
}

RegLocation evalLocWide(CompilationUnit* cUnit, RegLocation loc,
                        int regClass, bool update)
{
    DCHECK(loc.wide);
    int newRegs;
    int lowReg;
    int highReg;

    loc = oatUpdateLocWide(cUnit, loc);

    /* If already in registers, we can assume proper form.  Right reg class? */
    if (loc.location == kLocPhysReg) {
        DCHECK_EQ(oatIsFpReg(loc.lowReg), oatIsFpReg(loc.highReg));
        DCHECK(!oatIsFpReg(loc.lowReg) || ((loc.lowReg & 0x1) == 0));
        if (!regClassMatches(regClass, loc.lowReg)) {
            /* Wrong register class.  Reallocate and copy */
            newRegs = oatAllocTypedTempPair(cUnit, loc.fp, regClass);
            lowReg = newRegs & 0xff;
            highReg = (newRegs >> 8) & 0xff;
            oatRegCopyWide(cUnit, lowReg, highReg, loc.lowReg,
                                   loc.highReg);
            copyRegInfo(cUnit, lowReg, loc.lowReg);
            copyRegInfo(cUnit, highReg, loc.highReg);
            oatClobber(cUnit, loc.lowReg);
            oatClobber(cUnit, loc.highReg);
            loc.lowReg = lowReg;
            loc.highReg = highReg;
            oatMarkPair(cUnit, loc.lowReg, loc.highReg);
            DCHECK(!oatIsFpReg(loc.lowReg) || ((loc.lowReg & 0x1) == 0));
        }
        return loc;
    }

    DCHECK_NE(loc.sRegLow, INVALID_SREG);
    DCHECK_NE(oatSRegHi(loc.sRegLow), INVALID_SREG);

    newRegs = oatAllocTypedTempPair(cUnit, loc.fp, regClass);
    loc.lowReg = newRegs & 0xff;
    loc.highReg = (newRegs >> 8) & 0xff;

    oatMarkPair(cUnit, loc.lowReg, loc.highReg);
    if (update) {
        loc.location = kLocPhysReg;
        oatMarkLive(cUnit, loc.lowReg, loc.sRegLow);
        oatMarkLive(cUnit, loc.highReg, oatSRegHi(loc.sRegLow));
    }
    DCHECK(!oatIsFpReg(loc.lowReg) || ((loc.lowReg & 0x1) == 0));
    return loc;
}

extern RegLocation oatEvalLoc(CompilationUnit* cUnit, RegLocation loc,
                              int regClass, bool update)
{
    int newReg;

    if (loc.wide)
        return evalLocWide(cUnit, loc, regClass, update);

    loc = oatUpdateLoc(cUnit, loc);

    if (loc.location == kLocPhysReg) {
        if (!regClassMatches(regClass, loc.lowReg)) {
            /* Wrong register class.  Realloc, copy and transfer ownership */
            newReg = oatAllocTypedTemp(cUnit, loc.fp, regClass);
            oatRegCopy(cUnit, newReg, loc.lowReg);
            copyRegInfo(cUnit, newReg, loc.lowReg);
            oatClobber(cUnit, loc.lowReg);
            loc.lowReg = newReg;
        }
        return loc;
    }

    DCHECK_NE(loc.sRegLow, INVALID_SREG);

    newReg = oatAllocTypedTemp(cUnit, loc.fp, regClass);
    loc.lowReg = newReg;

    if (update) {
        loc.location = kLocPhysReg;
        oatMarkLive(cUnit, loc.lowReg, loc.sRegLow);
    }
    return loc;
}

extern RegLocation oatGetDest(CompilationUnit* cUnit, MIR* mir, int num)
{
    RegLocation res = cUnit->regLocation[mir->ssaRep->defs[num]];
    DCHECK(!res.wide);
    return res;
}
extern RegLocation oatGetSrc(CompilationUnit* cUnit, MIR* mir, int num)
{
    RegLocation res = cUnit->regLocation[mir->ssaRep->uses[num]];
    DCHECK(!res.wide);
    return res;
}
extern RegLocation oatGetRawSrc(CompilationUnit* cUnit, MIR* mir, int num)
{
    RegLocation res = cUnit->regLocation[mir->ssaRep->uses[num]];
    return res;
}
extern RegLocation oatGetDestWide(CompilationUnit* cUnit, MIR* mir,
                                  int low, int high)
{
    RegLocation res = cUnit->regLocation[mir->ssaRep->defs[low]];
    DCHECK(res.wide);
    return res;
}

extern RegLocation oatGetSrcWide(CompilationUnit* cUnit, MIR* mir,
                                 int low, int high)
{
    RegLocation res = cUnit->regLocation[mir->ssaRep->uses[low]];
    DCHECK(res.wide);
    return res;
}

/* USE SSA names to count references of base Dalvik vRegs. */
void oatCountRefs(CompilationUnit *cUnit, BasicBlock* bb,
                  RefCounts* coreCounts, RefCounts* fpCounts)
{
    MIR* mir;
    if (bb->blockType != kDalvikByteCode && bb->blockType != kEntryBlock &&
        bb->blockType != kExitBlock)
        return;

    for (mir = bb->firstMIRInsn; mir; mir = mir->next) {
        SSARepresentation *ssaRep = mir->ssaRep;
        if (ssaRep) {
            for (int i = 0; i < ssaRep->numDefs;) {
                RegLocation loc = cUnit->regLocation[ssaRep->defs[i]];
                RefCounts* counts = loc.fp ? fpCounts : coreCounts;
                int vReg = oatS2VReg(cUnit, ssaRep->defs[i]);
                if (loc.defined) {
                    counts[vReg].count++;
                }
                if (loc.wide) {
                    if (loc.defined) {
                        if (loc.fp) {
                            counts[vReg].doubleStart = true;
                        }
                        counts[vReg+1].count++;
                    }
                    i += 2;
                } else {
                    i++;
                }
            }
            for (int i = 0; i < ssaRep->numUses;) {
                RegLocation loc = cUnit->regLocation[ssaRep->uses[i]];
                RefCounts* counts = loc.fp ? fpCounts : coreCounts;
                int vReg = oatS2VReg(cUnit, ssaRep->uses[i]);
                if (loc.defined) {
                    counts[vReg].count++;
                }
                if (loc.wide) {
                    if (loc.defined) {
                        if (loc.fp) {
                            counts[vReg].doubleStart = true;
                        }
                        counts[vReg+1].count++;
                    }
                    i += 2;
                } else {
                    i++;
                }
            }
        }
    }
}

/* qsort callback function, sort descending */
int oatSortCounts(const void *val1, const void *val2)
{
    const RefCounts* op1 = (const RefCounts*)val1;
    const RefCounts* op2 = (const RefCounts*)val2;
    return (op1->count == op2->count) ? 0 : (op1->count < op2->count ? 1 : -1);
}

void oatDumpCounts(const RefCounts* arr, int size, const char* msg)
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
extern void oatDoPromotion(CompilationUnit* cUnit)
{
    int numRegs = cUnit->numDalvikRegisters;

    // Allow target code to add any special registers
    oatAdjustSpillMask(cUnit);

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
    RefCounts *coreRegs = (RefCounts *)
          oatNew(cUnit, sizeof(RefCounts) * numRegs, true, kAllocRegAlloc);
    RefCounts *fpRegs = (RefCounts *)
          oatNew(cUnit, sizeof(RefCounts) * numRegs, true, kAllocRegAlloc);
    for (int i = 0; i < numRegs; i++) {
        coreRegs[i].sReg = fpRegs[i].sReg = i;
    }
    GrowableListIterator iterator;
    oatGrowableListIteratorInit(&cUnit->blockList, &iterator);
    while (true) {
        BasicBlock* bb;
        bb = (BasicBlock*)oatGrowableListIteratorNext(&iterator);
        if (bb == NULL) break;
        oatCountRefs(cUnit, bb, coreRegs, fpRegs);
    }

    /*
     * Ideally, we'd allocate doubles starting with an even-numbered
     * register.  Bias the counts to try to allocate any vreg that's
     * used as the start of a pair first.
     */
    for (int i = 0; i < numRegs; i++) {
        if (fpRegs[i].doubleStart) {
            fpRegs[i].count *= 2;
        }
    }

    // Sort the count arrays
    qsort(coreRegs, numRegs, sizeof(RefCounts), oatSortCounts);
    qsort(fpRegs, numRegs, sizeof(RefCounts), oatSortCounts);

    if (cUnit->printMe) {
        oatDumpCounts(coreRegs, numRegs, "Core regs after sort");
        oatDumpCounts(fpRegs, numRegs, "Fp regs after sort");
    }

    if (!(cUnit->disableOpt & (1 << kPromoteRegs))) {
        // Promote fpRegs
        for (int i = 0; (fpRegs[i].count > 0) && (i < numRegs); i++) {
            if (cUnit->promotionMap[fpRegs[i].sReg].fpLocation != kLocPhysReg) {
                if (fpRegs[i].sReg >= cUnit->numRegs) {
                    // don't promote arg regs
                    continue;
                }
                int reg = oatAllocPreservedFPReg(cUnit, fpRegs[i].sReg,
                    fpRegs[i].doubleStart);
                if (reg < 0) {
                    break;  // No more left
                }
            }
        }

        // Promote core regs
        for (int i = 0; (coreRegs[i].count > 0) && i < numRegs; i++) {
            if (cUnit->promotionMap[coreRegs[i].sReg].coreLocation !=
                    kLocPhysReg) {
                if (coreRegs[i].sReg >= cUnit->numRegs) {
                    // don't promote arg regs
                    continue;
                }
                int reg = oatAllocPreservedCoreReg(cUnit, coreRegs[i].sReg);
                if (reg < 0) {
                   break;  // No more left
                }
            }
        }
    }

    // Now, update SSA names to new home locations
    for (int i = 0; i < cUnit->numSSARegs; i++) {
        RegLocation *curr = &cUnit->regLocation[i];
        int baseVReg = oatS2VReg(cUnit, curr->sRegLow);
        if (!curr->wide) {
            if (curr->fp) {
                if (cUnit->promotionMap[baseVReg].fpLocation == kLocPhysReg) {
                    curr->location = kLocPhysReg;
                    curr->lowReg = cUnit->promotionMap[baseVReg].fpReg;
                    curr->home = true;
                }
            } else {
                if (cUnit->promotionMap[baseVReg].coreLocation == kLocPhysReg) {
                    curr->location = kLocPhysReg;
                    curr->lowReg = cUnit->promotionMap[baseVReg].coreReg;
                    curr->home = true;
                }
            }
            curr->highReg = INVALID_REG;
        } else {
            if (curr->highWord) {
                continue;
            }
            if (curr->fp) {
                if ((cUnit->promotionMap[baseVReg].fpLocation == kLocPhysReg) &&
                    (cUnit->promotionMap[baseVReg+1].fpLocation ==
                    kLocPhysReg)) {
                    int lowReg = cUnit->promotionMap[baseVReg].fpReg;
                    int highReg = cUnit->promotionMap[baseVReg+1].fpReg;
                    // Doubles require pair of singles starting at even reg
                    if (((lowReg & 0x1) == 0) && ((lowReg + 1) == highReg)) {
                        curr->location = kLocPhysReg;
                        curr->lowReg = lowReg;
                        curr->highReg = highReg;
                        curr->home = true;
                    }
                }
            } else {
                if ((cUnit->promotionMap[baseVReg].coreLocation == kLocPhysReg)
                     && (cUnit->promotionMap[baseVReg+1].coreLocation ==
                     kLocPhysReg)) {
                    curr->location = kLocPhysReg;
                    curr->lowReg = cUnit->promotionMap[baseVReg].coreReg;
                    curr->highReg = cUnit->promotionMap[baseVReg+1].coreReg;
                    curr->home = true;
                }
            }
        }
    }
}

/* Returns sp-relative offset in bytes for a VReg */
extern int oatVRegOffset(CompilationUnit* cUnit, int vReg)
{
    return (vReg < cUnit->numRegs) ? cUnit->regsOffset + (vReg << 2) :
            cUnit->insOffset + ((vReg - cUnit->numRegs) << 2);
}

/* Returns sp-relative offset in bytes for a SReg */
extern int oatSRegOffset(CompilationUnit* cUnit, int sReg)
{
    return oatVRegOffset(cUnit, oatS2VReg(cUnit, sReg));
}


/* Return sp-relative offset in bytes using Method* */
extern int oatVRegOffset(const DexFile::CodeItem* code_item,
                         uint32_t core_spills, uint32_t fp_spills,
                         size_t frame_size, int reg)
{
    int numIns = code_item->ins_size_;
    int numRegs = code_item->registers_size_ - numIns;
    int numOuts = code_item->outs_size_;
    int numSpills = __builtin_popcount(core_spills) +
                    __builtin_popcount(fp_spills);
    int numPadding = (STACK_ALIGN_WORDS -
        (numSpills + numRegs + numOuts + 2)) & (STACK_ALIGN_WORDS-1);
    int regsOffset = (numOuts + numPadding + 1) * 4;
    int insOffset = frame_size + 4;
    return (reg < numRegs) ? regsOffset + (reg << 2) :
           insOffset + ((reg - numRegs) << 2);
}

}  // namespace art
