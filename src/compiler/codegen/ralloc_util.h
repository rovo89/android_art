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

#ifndef ART_SRC_COMPILER_CODEGEN_RALLOCUTIL_H_
#define ART_SRC_COMPILER_CODEGEN_RALLOCUTIL_H_

/*
 * This file contains target independent register alloction support.
 */

#include "../compiler_utility.h"
#include "../compiler_ir.h"
#include "../dataflow.h"

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

void ClobberSReg(CompilationUnit* cUnit, int sReg);
RegLocation EvalLoc(CompilationUnit* cUnit, RegLocation loc,
                              int regClass, bool update);
/* Mark a temp register as dead.  Does not affect allocation state. */
void Clobber(CompilationUnit* cUnit, int reg);
RegLocation UpdateLoc(CompilationUnit* cUnit, RegLocation loc);

/* see comments for updateLoc */
RegLocation UpdateLocWide(CompilationUnit* cUnit, RegLocation loc);

RegLocation UpdateRawLoc(CompilationUnit* cUnit, RegLocation loc);

void MarkLive(CompilationUnit* cUnit, int reg, int sReg);

void MarkTemp(CompilationUnit* cUnit, int reg);

void UnmarkTemp(CompilationUnit* cUnit, int reg);

void MarkDirty(CompilationUnit* cUnit, RegLocation loc);

void MarkPair(CompilationUnit* cUnit, int lowReg, int highReg);

void MarkClean(CompilationUnit* cUnit, RegLocation loc);

void ResetDef(CompilationUnit* cUnit, int reg);

void ResetDefLoc(CompilationUnit* cUnit, RegLocation rl);

/* Set up temp & preserved register pools specialized by target */
void CompilerInitPool(RegisterInfo* regs, int* regNums, int num);

/*
 * Mark the beginning and end LIR of a def sequence.  Note that
 * on entry start points to the LIR prior to the beginning of the
 * sequence.
 */
void MarkDef(CompilationUnit* cUnit, RegLocation rl, LIR* start,
                       LIR* finish);
/*
 * Mark the beginning and end LIR of a def sequence.  Note that
 * on entry start points to the LIR prior to the beginning of the
 * sequence.
 */
void MarkDefWide(CompilationUnit* cUnit, RegLocation rl,
                           LIR* start, LIR* finish);


// Get the LocRecord associated with an SSA name use.
RegLocation GetSrc(CompilationUnit* cUnit, MIR* mir, int num);
RegLocation GetSrcWide(CompilationUnit* cUnit, MIR* mir, int low);
// Non-width checking version
RegLocation GetRawSrc(CompilationUnit* cUnit, MIR* mir, int num);

// Get the LocRecord associated with an SSA name def.
RegLocation GetDest(CompilationUnit* cUnit, MIR* mir);
RegLocation GetDestWide(CompilationUnit* cUnit, MIR* mir);
// Non-width checking version
RegLocation GetRawDest(CompilationUnit* cUnit, MIR* mir);

RegLocation GetReturnWide(CompilationUnit* cUnit, bool isDouble);

/* Clobber all regs that might be used by an external C call */
void ClobberCalleeSave(CompilationUnit* cUnit);

RegisterInfo *IsTemp(CompilationUnit* cUnit, int reg);

RegisterInfo *IsPromoted(CompilationUnit* cUnit, int reg);

bool IsDirty(CompilationUnit* cUnit, int reg);

void MarkInUse(CompilationUnit* cUnit, int reg);

int AllocTemp(CompilationUnit* cUnit);

int AllocTempFloat(CompilationUnit* cUnit);

//REDO: too many assumptions.
int AllocTempDouble(CompilationUnit* cUnit);

void FreeTemp(CompilationUnit* cUnit, int reg);

void ResetDefLocWide(CompilationUnit* cUnit, RegLocation rl);

void ResetDefTracking(CompilationUnit* cUnit);

RegisterInfo *IsLive(CompilationUnit* cUnit, int reg);

/* To be used when explicitly managing register use */
void LockCallTemps(CompilationUnit* cUnit);

void FreeCallTemps(CompilationUnit* cUnit);

void FlushAllRegs(CompilationUnit* cUnit);

RegLocation GetReturnWideAlt(CompilationUnit* cUnit);

RegLocation GetReturn(CompilationUnit* cUnit, bool isFloat);

RegLocation GetReturnAlt(CompilationUnit* cUnit);

/* Clobber any temp associated with an sReg.  Could be in either class */

/* Return a temp if one is available, -1 otherwise */
int AllocFreeTemp(CompilationUnit* cUnit);

/* Attempt to allocate a callee-save register */
/*
 * Similar to AllocTemp(), but forces the allocation of a specific
 * register.  No check is made to see if the register was previously
 * allocated.  Use with caution.
 */
void LockTemp(CompilationUnit* cUnit, int reg);

RegLocation WideToNarrow(CompilationUnit* cUnit, RegLocation rl);

/*
 * Free all allocated temps in the temp pools.  Note that this does
 * not affect the "liveness" of a temp register, which will stay
 * live until it is either explicitly killed or reallocated.
 */
void ResetRegPool(CompilationUnit* cUnit);

void ClobberAllRegs(CompilationUnit* cUnit);

void FlushRegWide(CompilationUnit* cUnit, int reg1, int reg2);

void FlushReg(CompilationUnit* cUnit, int reg);

void DoPromotion(CompilationUnit* cUnit);
int VRegOffset(CompilationUnit* cUnit, int reg);
int SRegOffset(CompilationUnit* cUnit, int reg);
void RecordCorePromotion(CompilationUnit* cUnit, int reg, int sReg);
void RecordFpPromotion(CompilationUnit* cUnit, int reg, int sReg);


/* Architecture-dependent register allocation routines. */
int AllocTypedTempPair(CompilationUnit* cUnit,
                                 bool fpHint, int regClass);

int AllocTypedTemp(CompilationUnit* cUnit, bool fpHint, int regClass);

void oatDumpFPRegPool(CompilationUnit* cUint);
RegisterInfo* GetRegInfo(CompilationUnit* cUnit, int reg);
void NopLIR(LIR* lir);
bool oatIsFPReg(int reg);
uint32_t oatFPRegMask(void);
void AdjustSpillMask(CompilationUnit* cUnit);
void MarkPreservedSingle(CompilationUnit* cUnit, int vReg, int reg);
int ComputeFrameSize(CompilationUnit* cUnit);

}  // namespace art

#endif // ART_SRC_COMPILER_CODEGEN_RALLOCUTIL_H_
