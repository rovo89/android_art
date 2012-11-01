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

namespace art {

/* Static register use counts */
struct RefCounts {
  int count;
  int sReg;
  bool doubleStart;   // Starting vReg for a double
};


/*
 * Get the "real" sreg number associated with an sReg slot.  In general,
 * sReg values passed through codegen are the SSA names created by
 * dataflow analysis and refer to slot numbers in the cUnit->regLocation
 * array.  However, renaming is accomplished by simply replacing RegLocation
 * entries in the cUnit->reglocation[] array.  Therefore, when location
 * records for operands are first created, we need to ask the locRecord
 * identified by the dataflow pass what it's new name is.
 */

inline int oatSRegHi(int lowSreg) {
  return (lowSreg == INVALID_SREG) ? INVALID_SREG : lowSreg + 1;
}


inline bool oatLiveOut(CompilationUnit* cUnit, int sReg) {
  //For now.
  return true;
}

inline int oatSSASrc(MIR* mir, int num) {
  DCHECK_GT(mir->ssaRep->numUses, num);
  return mir->ssaRep->uses[num];
}

extern RegLocation oatEvalLoc(CompilationUnit* cUnit, RegLocation loc,
                              int regClass, bool update);
/* Mark a temp register as dead.  Does not affect allocation state. */
extern void oatClobber(CompilationUnit* cUnit, int reg);
extern RegLocation oatUpdateLoc(CompilationUnit* cUnit, RegLocation loc);

/* see comments for updateLoc */
extern RegLocation oatUpdateLocWide(CompilationUnit* cUnit, RegLocation loc);

extern RegLocation oatUpdateRawLoc(CompilationUnit* cUnit, RegLocation loc);

extern void oatMarkLive(CompilationUnit* cUnit, int reg, int sReg);

extern void oatMarkTemp(CompilationUnit* cUnit, int reg);

extern void oatUnmarkTemp(CompilationUnit* cUnit, int reg);

extern void oatMarkDirty(CompilationUnit* cUnit, RegLocation loc);

extern void oatMarkPair(CompilationUnit* cUnit, int lowReg, int highReg);

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
extern void oatMarkDef(CompilationUnit* cUnit, RegLocation rl, LIR* start,
                       LIR* finish);
/*
 * Mark the beginning and end LIR of a def sequence.  Note that
 * on entry start points to the LIR prior to the beginning of the
 * sequence.
 */
extern void oatMarkDefWide(CompilationUnit* cUnit, RegLocation rl,
                           LIR* start, LIR* finish);


// Get the LocRecord associated with an SSA name use.
extern RegLocation oatGetSrc(CompilationUnit* cUnit, MIR* mir, int num);
extern RegLocation oatGetSrcWide(CompilationUnit* cUnit, MIR* mir, int low);
// Non-width checking version
extern RegLocation oatGetRawSrc(CompilationUnit* cUnit, MIR* mir, int num);

// Get the LocRecord associated with an SSA name def.
extern RegLocation oatGetDest(CompilationUnit* cUnit, MIR* mir);
extern RegLocation oatGetDestWide(CompilationUnit* cUnit, MIR* mir);
// Non-width checking version
extern RegLocation oatGetRawDest(CompilationUnit* cUnit, MIR* mir);

extern RegLocation oatGetReturnWide(CompilationUnit* cUnit, bool isDouble);

/* Clobber all regs that might be used by an external C call */
extern void oatClobberCalleeSave(CompilationUnit* cUnit);

extern RegisterInfo *oatIsTemp(CompilationUnit* cUnit, int reg);

extern RegisterInfo *oatIsPromoted(CompilationUnit* cUnit, int reg);

extern bool oatIsDirty(CompilationUnit* cUnit, int reg);

extern void oatMarkInUse(CompilationUnit* cUnit, int reg);

extern int oatAllocTemp(CompilationUnit* cUnit);

extern int oatAllocTempFloat(CompilationUnit* cUnit);

//REDO: too many assumptions.
extern int oatAllocTempDouble(CompilationUnit* cUnit);

extern void oatFreeTemp(CompilationUnit* cUnit, int reg);

extern void oatResetDefLocWide(CompilationUnit* cUnit, RegLocation rl);

extern void oatResetDefTracking(CompilationUnit* cUnit);

extern RegisterInfo *oatIsLive(CompilationUnit* cUnit, int reg);

/* To be used when explicitly managing register use */
extern void oatLockCallTemps(CompilationUnit* cUnit);

extern void oatFreeCallTemps(CompilationUnit* cUnit);

extern void oatFlushAllRegs(CompilationUnit* cUnit);

extern RegLocation oatGetReturnWideAlt(CompilationUnit* cUnit);

extern RegLocation oatGetReturn(CompilationUnit* cUnit, bool isFloat);

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

extern RegLocation oatWideToNarrow(CompilationUnit* cUnit, RegLocation rl);

/*
 * Free all allocated temps in the temp pools.  Note that this does
 * not affect the "liveness" of a temp register, which will stay
 * live until it is either explicitly killed or reallocated.
 */
extern void oatResetRegPool(CompilationUnit* cUnit);

extern void oatClobberAllRegs(CompilationUnit* cUnit);

extern void oatFlushRegWide(CompilationUnit* cUnit, int reg1, int reg2);

extern void oatFlushReg(CompilationUnit* cUnit, int reg);

extern void oatDoPromotion(CompilationUnit* cUnit);
extern int oatVRegOffset(CompilationUnit* cUnit, int reg);
extern int oatSRegOffset(CompilationUnit* cUnit, int reg);
extern void oatCountRefs(CompilationUnit*, BasicBlock*, RefCounts*, RefCounts*);
extern int oatSortCounts(const void *val1, const void *val2);
extern void oatDumpCounts(const RefCounts* arr, int size, const char* msg);
extern void oatRecordCorePromotion(CompilationUnit* cUnit, int reg, int sReg);
extern void oatRecordFpPromotion(CompilationUnit* cUnit, int reg, int sReg);


/* Architecture-dependent register allocation routines. */
extern int oatAllocTypedTempPair(CompilationUnit* cUnit,
                                 bool fpHint, int regClass);

extern int oatAllocTypedTemp(CompilationUnit* cUnit, bool fpHint, int regClass);

extern void oatRegCopyWide(CompilationUnit* cUnit, int destLo,
                           int destHi, int srcLo, int srcHi);

extern void oatFlushRegImpl(CompilationUnit* cUnit, int rBase,
                            int displacement, int rSrc, OpSize size);

extern void oatFlushRegWideImpl(CompilationUnit* cUnit, int rBase,
                                int displacement, int rSrcLo, int rSrcHi);

extern void oatDumpCoreRegPool(CompilationUnit* cUint);
extern void oatDumpFPRegPool(CompilationUnit* cUint);
extern bool oatCheckCorePoolSanity(CompilationUnit* cUnit);
extern RegisterInfo* oatGetRegInfo(CompilationUnit* cUnit, int reg);
extern void oatNopLIR(LIR* lir);
extern bool oatIsFPReg(int reg);
extern uint32_t oatFPRegMask(void);
extern void oatAdjustSpillMask(CompilationUnit* cUnit);
void oatMarkPreservedSingle(CompilationUnit* cUnit, int vReg, int reg);
void oatRegCopy(CompilationUnit* cUnit, int rDest, int rSrc);
int oatComputeFrameSize(CompilationUnit* cUnit);

}  // namespace art

#endif // ART_SRC_COMPILER_RALLOC_H_
