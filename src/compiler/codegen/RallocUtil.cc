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

 /* Set up temp & preserved register pools specialized by target */
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

static void dumpRegPool(RegisterInfo* p, int numRegs)
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

/* Get info for a reg. */
static RegisterInfo* getRegInfo(CompilationUnit* cUnit, int reg)
{
    int numRegs = cUnit->regPool->numCoreRegs;
    RegisterInfo* p = cUnit->regPool->coreRegs;
    int i;
    for (i=0; i< numRegs; i++) {
        if (p[i].reg == reg) {
            return &p[i];
        }
    }
    p = cUnit->regPool->FPRegs;
    numRegs = cUnit->regPool->numFPRegs;
    for (i=0; i< numRegs; i++) {
        if (p[i].reg == reg) {
            return &p[i];
        }
    }
    LOG(FATAL) << "Tried to get info on a non-existant reg :r" << reg;
    return NULL; // Quiet gcc
}

void oatFlushRegWide(CompilationUnit* cUnit, int reg1, int reg2)
{
    RegisterInfo* info1 = getRegInfo(cUnit, reg1);
    RegisterInfo* info2 = getRegInfo(cUnit, reg2);
    assert(info1 && info2 && info1->pair && info2->pair &&
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
    RegisterInfo* info = getRegInfo(cUnit, reg);
    if (info->live && info->dirty) {
        info->dirty = false;
        int vReg = oatS2VReg(cUnit, info->sReg);
        oatFlushRegImpl(cUnit, rSP,
                                oatVRegOffset(cUnit, vReg),
                                reg, kWord);
    }
}

/* return true if found reg to clobber */
static bool clobberRegBody(CompilationUnit* cUnit, RegisterInfo* p,
                           int numRegs, int reg)
{
    int i;
    for (i=0; i< numRegs; i++) {
        if (p[i].reg == reg) {
            if (p[i].isTemp) {
                if (p[i].isTemp && p[i].live && p[i].dirty) {
                    if (p[i].pair) {
                        oatFlushRegWide(cUnit, p[i].reg, p[i].partner);
                    } else {
                        oatFlushReg(cUnit, p[i].reg);
                    }
                }
                p[i].live = false;
                p[i].sReg = INVALID_SREG;
            }
            p[i].defStart = NULL;
            p[i].defEnd = NULL;
            if (p[i].pair) {
                p[i].pair = false;
                /* partners should be in same pool */
                clobberRegBody(cUnit, p, numRegs, p[i].partner);
            }
            return true;
        }
    }
    return false;
}

/* Mark a temp register as dead.  Does not affect allocation state. */
void oatClobber(CompilationUnit* cUnit, int reg)
{
    if (!clobberRegBody(cUnit, cUnit->regPool->coreRegs,
                        cUnit->regPool->numCoreRegs, reg)) {
        clobberRegBody(cUnit, cUnit->regPool->FPRegs,
                       cUnit->regPool->numFPRegs, reg);
    }
}

static void clobberSRegBody(RegisterInfo* p, int numRegs, int sReg)
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
            cUnit->numSpills++;
            cUnit->regLocation[sReg].location = kLocPhysReg;
            cUnit->regLocation[sReg].lowReg = res;
            cUnit->regLocation[sReg].home = true;
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
static int allocPreservedSingle(CompilationUnit* cUnit, int sReg, bool even)
{
    int res = -1;
    RegisterInfo* FPRegs = cUnit->regPool->FPRegs;
    for (int i = 0; i < cUnit->regPool->numFPRegs; i++) {
        if (!FPRegs[i].isTemp && !FPRegs[i].inUse &&
            ((FPRegs[i].reg & 0x1) == 0) == even) {
            res = FPRegs[i].reg;
            FPRegs[i].inUse = true;
            cUnit->fpSpillMask |= (1 << (res & FP_REG_MASK));
            cUnit->fpVmapTable.push_back(sReg);
            cUnit->numSpills++;
            cUnit->numFPSpills++;
            cUnit->regLocation[sReg].fpLocation = kLocPhysReg;
            cUnit->regLocation[sReg].fpLowReg = res;
            cUnit->regLocation[sReg].home = true;
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
static int allocPreservedDouble(CompilationUnit* cUnit, int sReg)
{
    int res = -1; // Assume failure
    if (cUnit->regLocation[sReg+1].fpLocation == kLocPhysReg) {
        // Upper reg is already allocated.  Can we fit?
        int highReg = cUnit->regLocation[sReg+1].fpLowReg;
        if ((highReg & 1) == 0) {
            // High reg is even - fail.
            return res;
        }
        // Is the low reg of the pair free?
        RegisterInfo* p = getRegInfo(cUnit, highReg-1);
        if (p->inUse || p->isTemp) {
            // Already allocated or not preserved - fail.
            return res;
        }
        // OK - good to go.
        res = p->reg;
        p->inUse = true;
        assert((res & 1) == 0);
        cUnit->fpSpillMask |= (1 << (res & FP_REG_MASK));
        cUnit->numSpills++;
        cUnit->numFPSpills ++;
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
                cUnit->fpSpillMask |= (1 << (res & FP_REG_MASK));
                FPRegs[i+1].inUse = true;
                cUnit->fpSpillMask |= (1 << ((res+1) & FP_REG_MASK));
                cUnit->numSpills += 2;
                cUnit->numFPSpills += 2;
                break;
            }
        }
    }
    if (res != -1) {
        cUnit->regLocation[sReg].fpLocation = kLocPhysReg;
        cUnit->regLocation[sReg].fpLowReg = res;
        cUnit->regLocation[sReg].home = true;
        cUnit->regLocation[sReg+1].fpLocation = kLocPhysReg;
        cUnit->regLocation[sReg+1].fpLowReg = res + 1;
        cUnit->regLocation[sReg+1].home = true;
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
    } else {
    }
    if (res == -1) {
        res = allocPreservedSingle(cUnit, sReg, false /* try odd # */);
    }
    if (res == -1)
        res = allocPreservedSingle(cUnit, sReg, true /* try even # */);
    return res;
}

static int allocTempBody(CompilationUnit* cUnit, RegisterInfo* p, int numRegs,
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
    int next = cUnit->regPool->nextFPReg;
    int i;

    for (i=0; i < numRegs; i+=2) {
        /* Cleanup - not all targets need aligned regs */
        if (next & 1)
            next++;
        if (next >= numRegs)
            next = 0;
        if ((p[next].isTemp && !p[next].inUse && !p[next].live) &&
            (p[next+1].isTemp && !p[next+1].inUse && !p[next+1].live)) {
            oatClobber(cUnit, p[next].reg);
            oatClobber(cUnit, p[next+1].reg);
            p[next].inUse = true;
            p[next+1].inUse = true;
            assert((p[next].reg+1) == p[next+1].reg);
            assert((p[next].reg & 0x1) == 0);
            cUnit->regPool->nextFPReg += 2;
            return p[next].reg;
        }
        next += 2;
    }
    next = cUnit->regPool->nextFPReg;
    for (i=0; i < numRegs; i+=2) {
        if (next >= numRegs)
            next = 0;
        if (p[next].isTemp && !p[next].inUse && p[next+1].isTemp &&
            !p[next+1].inUse) {
            oatClobber(cUnit, p[next].reg);
            oatClobber(cUnit, p[next+1].reg);
            p[next].inUse = true;
            p[next+1].inUse = true;
            assert((p[next].reg+1) == p[next+1].reg);
            assert((p[next].reg & 0x1) == 0);
            cUnit->regPool->nextFPReg += 2;
            return p[next].reg;
        }
        next += 2;
    }
    LOG(FATAL) << "No free temp registers";
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

static RegisterInfo* allocLiveBody(RegisterInfo* p, int numRegs, int sReg)
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

static RegisterInfo* allocLive(CompilationUnit* cUnit, int sReg,
                               int regClass)
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
    RegisterInfo* p = getRegInfo(cUnit, reg);
    return (p->isTemp) ? p : NULL;
}

extern bool oatIsDirty(CompilationUnit* cUnit, int reg)
{
    RegisterInfo* p = getRegInfo(cUnit, reg);
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
            assert(p[i].isTemp);
            p[i].inUse = true;
            p[i].live = false;
            return;
        }
    }
    p = cUnit->regPool->FPRegs;
    numRegs = cUnit->regPool->numFPRegs;
    for (i=0; i< numRegs; i++) {
        if (p[i].reg == reg) {
            assert(p[i].isTemp);
            p[i].inUse = true;
            p[i].live = false;
            return;
        }
    }
    LOG(FATAL) << "Tried to lock a non-existant temp: r" << reg;
}

extern void oatResetDef(CompilationUnit* cUnit, int reg)
{
    RegisterInfo* p = getRegInfo(cUnit, reg);
    p->defStart = NULL;
    p->defEnd = NULL;
}

static void nullifyRange(CompilationUnit* cUnit, LIR *start, LIR *finish,
                         int sReg1, int sReg2)
{
    if (start && finish) {
        LIR *p;
        assert(sReg1 == sReg2);
        for (p = start; ;p = p->next) {
            ((ArmLIR *)p)->flags.isNop = true;
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
    assert(!rl.wide);
    assert(start && start->next);
    assert(finish);
    RegisterInfo* p = getRegInfo(cUnit, rl.lowReg);
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
    assert(rl.wide);
    assert(start && start->next);
    assert(finish);
    RegisterInfo* p = getRegInfo(cUnit, rl.lowReg);
    oatResetDef(cUnit, rl.highReg);  // Only track low of pair
    p->defStart = start->next;
    p->defEnd = finish;
}

extern RegLocation oatWideToNarrow(CompilationUnit* cUnit,
                                           RegLocation rl)
{
    assert(rl.wide);
    if (rl.location == kLocPhysReg) {
        RegisterInfo* infoLo = getRegInfo(cUnit, rl.lowReg);
        RegisterInfo* infoHi = getRegInfo(cUnit, rl.highReg);
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
    assert(!rl.wide);
    if (!(cUnit->disableOpt & (1 << kSuppressLoads))) {
        RegisterInfo* p = getRegInfo(cUnit, rl.lowReg);
        assert(!p->pair);
        nullifyRange(cUnit, p->defStart, p->defEnd,
                     p->sReg, rl.sRegLow);
    }
    oatResetDef(cUnit, rl.lowReg);
}

extern void oatResetDefLocWide(CompilationUnit* cUnit, RegLocation rl)
{
    assert(rl.wide);
    if (!(cUnit->disableOpt & (1 << kSuppressLoads))) {
        RegisterInfo* p = getRegInfo(cUnit, rl.lowReg);
        assert(p->pair);
        nullifyRange(cUnit, p->defStart, p->defEnd,
                     p->sReg, rl.sRegLow);
    }
    oatResetDef(cUnit, rl.lowReg);
    oatResetDef(cUnit, rl.highReg);
}

extern void oatResetDefTracking(CompilationUnit* cUnit)
{
    int i;
    for (i=0; i< cUnit->regPool->numCoreRegs; i++) {
        oatResetDef(cUnit, cUnit->regPool->coreRegs[i].reg);
    }
    for (i=0; i< cUnit->regPool->numFPRegs; i++) {
        oatResetDef(cUnit, cUnit->regPool->FPRegs[i].reg);
    }
}

extern void oatClobberAllRegs(CompilationUnit* cUnit)
{
    int i;
    for (i=0; i< cUnit->regPool->numCoreRegs; i++) {
        oatClobber(cUnit, cUnit->regPool->coreRegs[i].reg);
    }
    for (i=0; i< cUnit->regPool->numFPRegs; i++) {
        oatClobber(cUnit, cUnit->regPool->FPRegs[i].reg);
    }
}

/* To be used when explicitly managing register use */
extern void oatLockCallTemps(CompilationUnit* cUnit)
{
    //TODO: Arm specific - move to target dependent code
    oatLockTemp(cUnit, r0);
    oatLockTemp(cUnit, r1);
    oatLockTemp(cUnit, r2);
    oatLockTemp(cUnit, r3);
}

/* To be used when explicitly managing register use */
extern void oatFreeCallTemps(CompilationUnit* cUnit)
{
    //TODO: Arm specific - move to target dependent code
    oatFreeTemp(cUnit, r0);
    oatFreeTemp(cUnit, r1);
    oatFreeTemp(cUnit, r2);
    oatFreeTemp(cUnit, r3);
}

// Make sure nothing is live and dirty
static void flushAllRegsBody(CompilationUnit* cUnit, RegisterInfo* info,
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
static bool regClassMatches(int regClass, int reg)
{
    if (regClass == kAnyReg) {
        return true;
    } else if (regClass == kCoreReg) {
        return !FPREG(reg);
    } else {
        return FPREG(reg);
    }
}

extern void oatMarkLive(CompilationUnit* cUnit, int reg, int sReg)
{
    RegisterInfo* info = getRegInfo(cUnit, reg);
    if ((info->reg == reg) && (info->sReg == sReg) && info->live) {
        return;  /* already live */
    } else if (sReg != INVALID_SREG) {
        oatClobberSReg(cUnit, sReg);
        if (info->isTemp) {
            info->live = true;
        }
    } else {
        /* Can't be live if no associated sReg */
        assert(info->isTemp);
        info->live = false;
    }
    info->sReg = sReg;
}

extern void oatMarkTemp(CompilationUnit* cUnit, int reg)
{
    RegisterInfo* info = getRegInfo(cUnit, reg);
    info->isTemp = true;
}

extern void oatUnmarkTemp(CompilationUnit* cUnit, int reg)
{
    RegisterInfo* info = getRegInfo(cUnit, reg);
    info->isTemp = false;
}

extern void oatMarkPair(CompilationUnit* cUnit, int lowReg, int highReg)
{
    RegisterInfo* infoLo = getRegInfo(cUnit, lowReg);
    RegisterInfo* infoHi = getRegInfo(cUnit, highReg);
    infoLo->pair = infoHi->pair = true;
    infoLo->partner = highReg;
    infoHi->partner = lowReg;
}

extern void oatMarkClean(CompilationUnit* cUnit, RegLocation loc)
{
    RegisterInfo* info = getRegInfo(cUnit, loc.lowReg);
    info->dirty = false;
    if (loc.wide) {
        info = getRegInfo(cUnit, loc.highReg);
        info->dirty = false;
    }
}

extern void oatMarkDirty(CompilationUnit* cUnit, RegLocation loc)
{
    if (loc.home) {
        // If already home, can't be dirty
        return;
    }
    RegisterInfo* info = getRegInfo(cUnit, loc.lowReg);
    info->dirty = true;
    if (loc.wide) {
        info = getRegInfo(cUnit, loc.highReg);
        info->dirty = true;
    }
}

extern void oatMarkInUse(CompilationUnit* cUnit, int reg)
{
      RegisterInfo* info = getRegInfo(cUnit, reg);
      info->inUse = true;
}

static void copyRegInfo(CompilationUnit* cUnit, int newReg, int oldReg)
{
    RegisterInfo* newInfo = getRegInfo(cUnit, newReg);
    RegisterInfo* oldInfo = getRegInfo(cUnit, oldReg);
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
 * is generated.  The the live value is part of an older pair,
 * clobber both low and high.
 * TUNING: clobbering both is a bit heavy-handed, but the alternative
 * is a bit complex when dealing with FP regs.  Examine code to see
 * if it's worthwhile trying to be more clever here.
 */

extern RegLocation oatUpdateLoc(CompilationUnit* cUnit, RegLocation loc)
{
    assert(!loc.wide);
    if (loc.location == kLocDalvikFrame) {
        RegisterInfo* infoLo = allocLive(cUnit, loc.sRegLow, kAnyReg);
        if (infoLo) {
            if (infoLo->pair) {
                oatClobber(cUnit, infoLo->reg);
                oatClobber(cUnit, infoLo->partner);
            } else {
                loc.lowReg = infoLo->reg;
                loc.location = kLocPhysReg;
            }
        }
    }

    return loc;
}

/* see comments for updateLoc */
extern RegLocation oatUpdateLocWide(CompilationUnit* cUnit,
                                    RegLocation loc)
{
    assert(loc.wide);
    if (loc.location == kLocDalvikFrame) {
        // Are the dalvik regs already live in physical registers?
        RegisterInfo* infoLo = allocLive(cUnit, loc.sRegLow, kAnyReg);
        RegisterInfo* infoHi = allocLive(cUnit,
              oatSRegHi(loc.sRegLow), kAnyReg);
        bool match = true;
        match = match && (infoLo != NULL);
        match = match && (infoHi != NULL);
        // Are they both core or both FP?
        match = match && (FPREG(infoLo->reg) == FPREG(infoHi->reg));
        // If a pair of floating point singles, are they properly aligned?
        if (match && FPREG(infoLo->reg)) {
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
            assert(!FPREG(loc.lowReg) || ((loc.lowReg & 0x1) == 0));
            return loc;
        }
        // Can't easily reuse - clobber any overlaps
        if (infoLo) {
            oatClobber(cUnit, infoLo->reg);
            if (infoLo->pair)
                oatClobber(cUnit, infoLo->partner);
        }
        if (infoHi) {
            oatClobber(cUnit, infoHi->reg);
            if (infoHi->pair)
                oatClobber(cUnit, infoHi->partner);
        }
    }
    return loc;
}

static RegLocation evalLocWide(CompilationUnit* cUnit, RegLocation loc,
                               int regClass, bool update)
{
    assert(loc.wide);
    int newRegs;
    int lowReg;
    int highReg;

    loc = oatUpdateLocWide(cUnit, loc);

    /* If already in registers, we can assume proper form.  Right reg class? */
    if (loc.location == kLocPhysReg) {
        assert(FPREG(loc.lowReg) == FPREG(loc.highReg));
        assert(!FPREG(loc.lowReg) || ((loc.lowReg & 0x1) == 0));
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
            assert(!FPREG(loc.lowReg) || ((loc.lowReg & 0x1) == 0));
        }
        return loc;
    }

    assert(loc.sRegLow != INVALID_SREG);
    assert(oatSRegHi(loc.sRegLow) != INVALID_SREG);

    newRegs = oatAllocTypedTempPair(cUnit, loc.fp, regClass);
    loc.lowReg = newRegs & 0xff;
    loc.highReg = (newRegs >> 8) & 0xff;

    oatMarkPair(cUnit, loc.lowReg, loc.highReg);
    if (update) {
        loc.location = kLocPhysReg;
        oatMarkLive(cUnit, loc.lowReg, loc.sRegLow);
        oatMarkLive(cUnit, loc.highReg, oatSRegHi(loc.sRegLow));
    }
    assert(!FPREG(loc.lowReg) || ((loc.lowReg & 0x1) == 0));
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

    assert(loc.sRegLow != INVALID_SREG);

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
    assert(!res.wide);
    return res;
}
extern RegLocation oatGetSrc(CompilationUnit* cUnit, MIR* mir, int num)
{
    RegLocation res = cUnit->regLocation[mir->ssaRep->uses[num]];
    assert(!res.wide);
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
    assert(res.wide);
    return res;
}

extern RegLocation oatGetSrcWide(CompilationUnit* cUnit, MIR* mir,
                                 int low, int high)
{
    RegLocation res = cUnit->regLocation[mir->ssaRep->uses[low]];
    assert(res.wide);
    return res;
}
