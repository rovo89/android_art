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
 * This file contains Arm-specific register alloction support.
 */

#include "../../CompilerUtility.h"
#include "../../CompilerIR.h"
#include "../..//Dataflow.h"
#include "ArmLIR.h"
#include "Codegen.h"
#include "../Ralloc.h"

/*
 * Placeholder routine until we do proper register allocation.
 */

typedef struct RefCounts {
    int count;
    int sReg;
    bool doubleStart;   // Starting vReg for a double
} RefCounts;

/* USE SSA names to count references of base Dalvik vRegs. */
STATIC void countRefs(CompilationUnit *cUnit, BasicBlock* bb,
                      RefCounts* counts, bool fp)
{
    MIR* mir;
    if (bb->blockType != kDalvikByteCode && bb->blockType != kEntryBlock &&
        bb->blockType != kExitBlock)
        return;

    for (mir = bb->firstMIRInsn; mir; mir = mir->next) {
        SSARepresentation *ssaRep = mir->ssaRep;
        if (ssaRep) {
            int i;
            int attrs = oatDataFlowAttributes[mir->dalvikInsn.opcode];
            if (fp) {
                // Mark 1st reg of double pairs
                int first = 0;
                int sReg;
                if ((attrs & (DF_DA_WIDE|DF_FP_A)) == (DF_DA_WIDE|DF_FP_A)) {
                    sReg = DECODE_REG(
                        oatConvertSSARegToDalvik(cUnit, ssaRep->defs[0]));
                    counts[sReg].doubleStart = true;
                }
                if ((attrs & (DF_UA_WIDE|DF_FP_A)) == (DF_UA_WIDE|DF_FP_A)) {
                    sReg = DECODE_REG(
                        oatConvertSSARegToDalvik(cUnit, ssaRep->uses[first]));
                    counts[sReg].doubleStart = true;
                }
                if (attrs & DF_UA_WIDE) {
                    first += 2;
                }
                if ((attrs & (DF_UB_WIDE|DF_FP_B)) == (DF_UB_WIDE|DF_FP_B)) {
                    sReg = DECODE_REG(
                        oatConvertSSARegToDalvik(cUnit, ssaRep->uses[first]));
                    counts[sReg].doubleStart = true;
                }
                if (attrs & DF_UB_WIDE) {
                    first += 2;
                }
                if ((attrs & (DF_UC_WIDE|DF_FP_C)) == (DF_UC_WIDE|DF_FP_C)) {
                    sReg = DECODE_REG(
                        oatConvertSSARegToDalvik(cUnit, ssaRep->uses[first]));
                    counts[sReg].doubleStart = true;
                }
            }
            for (i=0; i< ssaRep->numUses; i++) {
                int origSreg = DECODE_REG(
                    oatConvertSSARegToDalvik(cUnit, ssaRep->uses[i]));
                DCHECK_LT(origSreg, cUnit->method->NumRegisters());
                bool fpUse = ssaRep->fpUse ? ssaRep->fpUse[i] : false;
                if (fp == fpUse) {
                    counts[origSreg].count++;
                }
            }
            for (i=0; i< ssaRep->numDefs; i++) {
                if (attrs & DF_SETS_CONST) {
                    // CONST opcodes are untyped - don't pollute the counts
                    continue;
                }
                int origSreg = DECODE_REG(
                    oatConvertSSARegToDalvik(cUnit, ssaRep->defs[i]));
                DCHECK_LT(origSreg, cUnit->method->NumRegisters());
                bool fpDef = ssaRep->fpDef ? ssaRep->fpDef[i] : false;
                if (fp == fpDef) {
                    counts[origSreg].count++;
                }
            }
        }
    }
}

/* qsort callback function, sort descending */
STATIC int sortCounts(const void *val1, const void *val2)
{
    const RefCounts* op1 = (const RefCounts*)val1;
    const RefCounts* op2 = (const RefCounts*)val2;
    return (op1->count == op2->count) ? 0 : (op1->count < op2->count ? 1 : -1);
}

STATIC void dumpCounts(const RefCounts* arr, int size, const char* msg)
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
    int numRegs = cUnit->method->NumRegisters();

    /*
     * TUNING: is leaf?  Can't just use "hasInvoke" to determine as some
     * instructions might call out to C/assembly helper functions.  Until
     * machinery is in place, always spill lr.
     */
    cUnit->coreSpillMask |= (1 << rLR);
    cUnit->numCoreSpills++;
    /*
     * Simple hack for testing register allocation.  Just do a static
     * count of the uses of Dalvik registers.  Note that we examine
     * the SSA names, but count based on original Dalvik register name.
     * Count refs separately based on type in order to give allocation
     * preference to fp doubles - which must be allocated sequential
     * physical single fp registers started with an even-numbered
     * reg.
     */
    RefCounts *coreRegs = (RefCounts *)
          oatNew(sizeof(RefCounts) * numRegs, true);
    RefCounts *fpRegs = (RefCounts *)
          oatNew(sizeof(RefCounts) * numRegs, true);
    for (int i = 0; i < numRegs; i++) {
        coreRegs[i].sReg = fpRegs[i].sReg = i;
    }
    GrowableListIterator iterator;
    oatGrowableListIteratorInit(&cUnit->blockList, &iterator);
    while (true) {
        BasicBlock* bb;
        bb = (BasicBlock*)oatGrowableListIteratorNext(&iterator);
        if (bb == NULL) break;
        countRefs(cUnit, bb, coreRegs, false);
        countRefs(cUnit, bb, fpRegs, true);
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
    qsort(coreRegs, numRegs, sizeof(RefCounts), sortCounts);
    qsort(fpRegs, numRegs, sizeof(RefCounts), sortCounts);

    if (cUnit->printMeVerbose) {
        dumpCounts(coreRegs, numRegs, "coreRegs");
        dumpCounts(fpRegs, numRegs, "fpRegs");
    }

    if (!(cUnit->disableOpt & (1 << kPromoteRegs))) {
        // Promote fpRegs
        for (int i = 0; (fpRegs[i].count > 0) && (i < numRegs); i++) {
            if (cUnit->regLocation[fpRegs[i].sReg].fpLocation != kLocPhysReg) {
                int reg = oatAllocPreservedFPReg(cUnit, fpRegs[i].sReg,
                    fpRegs[i].doubleStart);
                if (reg < 0) {
                   break;  // No more left
                }
            }
        }

        // Promote core regs
        for (int i = 0; (coreRegs[i].count > 0) && i < numRegs; i++) {
            if (cUnit->regLocation[i].location != kLocPhysReg) {
                int reg = oatAllocPreservedCoreReg(cUnit, coreRegs[i].sReg);
                if (reg < 0) {
                   break;  // No more left
                }
            }
        }
    }

    // Now, update SSA names to new home locations
    for (int i = 0; i < cUnit->numSSARegs; i++) {
        int baseSreg = cUnit->regLocation[i].sRegLow;
        RegLocation *base = &cUnit->regLocation[baseSreg];
        RegLocation *baseNext = &cUnit->regLocation[baseSreg+1];
        RegLocation *curr = &cUnit->regLocation[i];
        if (curr->fp) {
            /* Single or double, check fpLocation of base */
            if (base->fpLocation == kLocPhysReg) {
                if (curr->wide) {
                    /* TUNING: consider alignment during allocation */
                    if ((base->fpLowReg & 1) ||
                        (baseNext->fpLocation != kLocPhysReg)) {
                        /* Half-promoted or bad alignment - demote */
                        curr->location = kLocDalvikFrame;
                        curr->lowReg = INVALID_REG;
                        curr->highReg = INVALID_REG;
                        continue;
                    }
                    curr->highReg = baseNext->fpLowReg;
                }
                curr->location = kLocPhysReg;
                curr->lowReg = base->fpLowReg;
                curr->home = true;
            }
        } else {
            /* Core or wide */
            if (base->location == kLocPhysReg) {
                if (curr->wide) {
                    /* Make sure upper half is also in reg or skip */
                    if (baseNext->location != kLocPhysReg) {
                        /* Only half promoted; demote to frame */
                        curr->location = kLocDalvikFrame;
                        curr->lowReg = INVALID_REG;
                        curr->highReg = INVALID_REG;
                        continue;
                    }
                    curr->highReg = baseNext->lowReg;
                }
                curr->location = kLocPhysReg;
                curr->lowReg = base->lowReg;
                curr->home = true;
            }
        }
    }
}

/* Returns sp-relative offset in bytes */
extern int oatVRegOffset(CompilationUnit* cUnit, int reg)
{
    return (reg < cUnit->numRegs) ? cUnit->regsOffset + (reg << 2) :
            cUnit->insOffset + ((reg - cUnit->numRegs) << 2);
}

/* Return sp-relative offset in bytes using Method* */
extern int oatVRegOffsetFromMethod(Method* method, int reg)
{
    int numIns = method->NumIns();
    int numRegs = method->NumRegisters() - numIns;
    int numOuts = method->NumOuts();
    int numSpills = __builtin_popcount(method->GetCoreSpillMask()) +
                    __builtin_popcount(method->GetFpSpillMask());
    int numPadding = (STACK_ALIGN_WORDS -
        (numSpills + numRegs + numOuts + 2)) & (STACK_ALIGN_WORDS-1);
    int regsOffset = (numOuts + numPadding + 1) * 4;
    int insOffset = method->GetFrameSizeInBytes() + 4;
    return (reg < numRegs) ? regsOffset + (reg << 2) :
           insOffset + ((reg - numRegs) << 2);
}

/* Clobber all regs that might be used by an external C call */
extern void oatClobberCalleeSave(CompilationUnit *cUnit)
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
