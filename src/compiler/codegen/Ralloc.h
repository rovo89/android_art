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

#ifndef ART_SRC_COMPILER_RALLOC_H_
#define ART_SRC_COMPILER_RALLOC_H_
/*
 * This file contains target independent register alloction support.
 */

#include "../CompilerUtility.h"
#include "../CompilerIR.h"
#include "../Dataflow.h"
#include "arm/ArmLIR.h"

/*
 * Return most flexible allowed register class based on size.
 * Bug: 2813841
 * Must use a core register for data types narrower than word (due
 * to possible unaligned load/store.
 */
static inline RegisterClass oatRegClassBySize(OpSize size)
{
    return (size == kUnsignedHalf ||
            size == kSignedHalf ||
            size == kUnsignedByte ||
            size == kSignedByte ) ? kCoreReg : kAnyReg;
}

static inline int oatS2VReg(CompilationUnit* cUnit, int sReg)
{
    assert(sReg != INVALID_SREG);
    return DECODE_REG(oatConvertSSARegToDalvik(cUnit, sReg));
}

/* Reset the tracker to unknown state */
static inline void oatResetNullCheck(CompilationUnit* cUnit)
{
    oatClearAllBits(cUnit->regPool->nullCheckedRegs);
}

/*
 * Get the "real" sreg number associated with an sReg slot.  In general,
 * sReg values passed through codegen are the SSA names created by
 * dataflow analysis and refer to slot numbers in the cUnit->regLocation
 * array.  However, renaming is accomplished by simply replacing RegLocation
 * entries in the cUnit->reglocation[] array.  Therefore, when location
 * records for operands are first created, we need to ask the locRecord
 * identified by the dataflow pass what it's new name is.
 */

static inline int oatSRegHi(int lowSreg) {
    return (lowSreg == INVALID_SREG) ? INVALID_SREG : lowSreg + 1;
}


static inline bool oatLiveOut(CompilationUnit* cUnit, int sReg)
{
    //For now.
    return true;
}

static inline int oatSSASrc(MIR* mir, int num)
{
    assert(mir->ssaRep->numUses > num);
    return mir->ssaRep->uses[num];
}

extern RegLocation oatEvalLoc(CompilationUnit* cUnit, RegLocation loc,
                                      int regClass, bool update);
/* Mark a temp register as dead.  Does not affect allocation state. */
extern void oatClobber(CompilationUnit* cUnit, int reg);

extern RegLocation oatUpdateLoc(CompilationUnit* cUnit,
                                        RegLocation loc);

/* see comments for updateLoc */
extern RegLocation oatUpdateLocWide(CompilationUnit* cUnit,
                                            RegLocation loc);

/* Clobber all of the temps that might be used by a handler. */
extern void oatClobberHandlerRegs(CompilationUnit* cUnit);

extern void oatMarkLive(CompilationUnit* cUnit, int reg, int sReg);

extern void oatMarkTemp(CompilationUnit* cUnit, int reg);

extern void oatMarkDirty(CompilationUnit* cUnit, RegLocation loc);

extern void oatMarkPair(CompilationUnit* cUnit, int lowReg,
                                int highReg);

extern void oatMarkClean(CompilationUnit* cUnit, RegLocation loc);

extern void oatResetDef(CompilationUnit* cUnit, int reg);

extern void oatResetDefLoc(CompilationUnit* cUnit, RegLocation rl);

/* Set up temp & preserved register pools specialized by target */
extern void oatInitPool(RegisterInfo* regs, int* regNums, int num);

/*
 * Mark the beginning and end LIR of a def sequence.  Note that
 * on entry start points to the LIR prior to the beginning of the
 * sequence.
 */
extern void oatMarkDef(CompilationUnit* cUnit, RegLocation rl,
                               LIR* start, LIR* finish);
/*
 * Mark the beginning and end LIR of a def sequence.  Note that
 * on entry start points to the LIR prior to the beginning of the
 * sequence.
 */
extern void oatMarkDefWide(CompilationUnit* cUnit, RegLocation rl,
                                   LIR* start, LIR* finish);

extern RegLocation oatGetSrcWide(CompilationUnit* cUnit, MIR* mir,
                                 int low, int high);

extern RegLocation oatGetDestWide(CompilationUnit* cUnit, MIR* mir,
                                  int low, int high);
// Get the LocRecord associated with an SSA name use.
extern RegLocation oatGetSrc(CompilationUnit* cUnit, MIR* mir, int num);

// Get the LocRecord associated with an SSA name def.
extern RegLocation oatGetDest(CompilationUnit* cUnit, MIR* mir, int num);

extern RegLocation oatGetReturnWide(CompilationUnit* cUnit);

/* Clobber all regs that might be used by an external C call */
extern void oatClobberCallRegs(CompilationUnit* cUnit);

extern RegisterInfo *oatIsTemp(CompilationUnit* cUnit, int reg);

extern bool oatIsDirty(CompilationUnit* cUnit, int reg);

extern void oatMarkInUse(CompilationUnit* cUnit, int reg);

extern int oatAllocTemp(CompilationUnit* cUnit);

extern int oatAllocTempFloat(CompilationUnit* cUnit);

//REDO: too many assumptions.
extern int oatAllocTempDouble(CompilationUnit* cUnit);

extern void oatFreeTemp(CompilationUnit* cUnit, int reg);

extern void oatResetDefLocWide(CompilationUnit* cUnit, RegLocation rl);

extern void oatResetDefTracking(CompilationUnit* cUnit);

/* Kill the corresponding bit in the null-checked register list */
extern void oatKillNullCheckedLoc(CompilationUnit* cUnit,
                                  RegLocation loc);

//FIXME - this needs to also check the preserved pool.
extern RegisterInfo *oatIsLive(CompilationUnit* cUnit, int reg);

/* To be used when explicitly managing register use */
extern void oatLockAllTemps(CompilationUnit* cUnit);

extern void oatFlushAllRegs(CompilationUnit* cUnit);

extern RegLocation oatGetReturnWideAlt(CompilationUnit* cUnit);

extern RegLocation oatGetReturn(CompilationUnit* cUnit);

extern RegLocation oatGetReturnAlt(CompilationUnit* cUnit);

/* Clobber any temp associated with an sReg.  Could be in either class */
extern void oatClobberSReg(CompilationUnit* cUnit, int sReg);

/* Return a temp if one is available, -1 otherwise */
extern int oatAllocFreeTemp(CompilationUnit* cUnit);

/* Attempt to allocate a callee-save register */
extern int oatAllocPreservedCoreReg(CompilationUnit* cUnit, int sreg);
extern int oatAllocPreservedFPReg(CompilationUnit* cUnit, int sReg,
                                  bool doubleStart);

/*
 * Similar to oatAllocTemp(), but forces the allocation of a specific
 * register.  No check is made to see if the register was previously
 * allocated.  Use with caution.
 */
extern void oatLockTemp(CompilationUnit* cUnit, int reg);

extern RegLocation oatWideToNarrow(CompilationUnit* cUnit,
                                   RegLocation rl);

/*
 * Free all allocated temps in the temp pools.  Note that this does
 * not affect the "liveness" of a temp register, which will stay
 * live until it is either explicitly killed or reallocated.
 */
extern void oatResetRegPool(CompilationUnit* cUnit);

extern void oatClobberAllRegs(CompilationUnit* cUnit);

extern void oatFlushRegWide(CompilationUnit* cUnit, int reg1, int reg2);

extern void oatFlushReg(CompilationUnit* cUnit, int reg);

/*
 * Architecture-dependent register allocation routines implemented in
 * ${TARGET_ARCH}/${TARGET_ARCH_VARIANT}/Ralloc.c
 */
extern int oatAllocTypedTempPair(CompilationUnit* cUnit,
                                 bool fpHint, int regClass);

extern int oatAllocTypedTemp(CompilationUnit* cUnit, bool fpHint,
                             int regClass);

extern ArmLIR* oatRegCopy(CompilationUnit* cUnit, int rDest, int rSrc);

extern void oatRegCopyWide(CompilationUnit* cUnit, int destLo,
                           int destHi, int srcLo, int srcHi);

extern void oatFlushRegImpl(CompilationUnit* cUnit, int rBase,
                            int displacement, int rSrc, OpSize size);

extern void oatFlushRegWideImpl(CompilationUnit* cUnit, int rBase,
                                int displacement, int rSrcLo, int rSrcHi);

extern void oatDoPromotion(CompilationUnit* cUnit);
extern int oatVRegOffset(CompilationUnit* cUnit, int reg);
#endif // ART_SRC_COMPILER_RALLOC_H_
