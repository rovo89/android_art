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
  int s_reg;
  bool double_start;   // Starting v_reg for a double
};


/*
 * Get the "real" sreg number associated with an s_reg slot.  In general,
 * s_reg values passed through codegen are the SSA names created by
 * dataflow analysis and refer to slot numbers in the cu->reg_location
 * array.  However, renaming is accomplished by simply replacing RegLocation
 * entries in the cu->reglocation[] array.  Therefore, when location
 * records for operands are first created, we need to ask the locRecord
 * identified by the dataflow pass what it's new name is.
 */

inline int GetSRegHi(int lowSreg) {
  return (lowSreg == INVALID_SREG) ? INVALID_SREG : lowSreg + 1;
}


inline bool oat_live_out(CompilationUnit* cu, int s_reg) {
  //For now.
  return true;
}

inline int oatSSASrc(MIR* mir, int num) {
  DCHECK_GT(mir->ssa_rep->num_uses, num);
  return mir->ssa_rep->uses[num];
}

void ClobberSReg(CompilationUnit* cu, int s_reg);
RegLocation EvalLoc(CompilationUnit* cu, RegLocation loc,
                              int reg_class, bool update);
/* Mark a temp register as dead.  Does not affect allocation state. */
void Clobber(CompilationUnit* cu, int reg);
RegLocation UpdateLoc(CompilationUnit* cu, RegLocation loc);

/* see comments for update_loc */
RegLocation UpdateLocWide(CompilationUnit* cu, RegLocation loc);

RegLocation UpdateRawLoc(CompilationUnit* cu, RegLocation loc);

void MarkLive(CompilationUnit* cu, int reg, int s_reg);

void MarkTemp(CompilationUnit* cu, int reg);

void UnmarkTemp(CompilationUnit* cu, int reg);

void MarkDirty(CompilationUnit* cu, RegLocation loc);

void MarkPair(CompilationUnit* cu, int low_reg, int high_reg);

void MarkClean(CompilationUnit* cu, RegLocation loc);

void ResetDef(CompilationUnit* cu, int reg);

void ResetDefLoc(CompilationUnit* cu, RegLocation rl);

/* Set up temp & preserved register pools specialized by target */
void CompilerInitPool(RegisterInfo* regs, int* reg_nums, int num);

/*
 * Mark the beginning and end LIR of a def sequence.  Note that
 * on entry start points to the LIR prior to the beginning of the
 * sequence.
 */
void MarkDef(CompilationUnit* cu, RegLocation rl, LIR* start,
                       LIR* finish);
/*
 * Mark the beginning and end LIR of a def sequence.  Note that
 * on entry start points to the LIR prior to the beginning of the
 * sequence.
 */
void MarkDefWide(CompilationUnit* cu, RegLocation rl,
                           LIR* start, LIR* finish);


// Get the LocRecord associated with an SSA name use.
RegLocation GetSrc(CompilationUnit* cu, MIR* mir, int num);
RegLocation GetSrcWide(CompilationUnit* cu, MIR* mir, int low);
// Non-width checking version
RegLocation GetRawSrc(CompilationUnit* cu, MIR* mir, int num);

// Get the LocRecord associated with an SSA name def.
RegLocation GetDest(CompilationUnit* cu, MIR* mir);
RegLocation GetDestWide(CompilationUnit* cu, MIR* mir);
// Non-width checking version
RegLocation GetRawDest(CompilationUnit* cu, MIR* mir);

RegLocation GetReturnWide(CompilationUnit* cu, bool is_double);

/* Clobber all regs that might be used by an external C call */
void ClobberCalleeSave(CompilationUnit* cu);

RegisterInfo *IsTemp(CompilationUnit* cu, int reg);

RegisterInfo *IsPromoted(CompilationUnit* cu, int reg);

bool IsDirty(CompilationUnit* cu, int reg);

void MarkInUse(CompilationUnit* cu, int reg);

int AllocTemp(CompilationUnit* cu);

int AllocTempFloat(CompilationUnit* cu);

//REDO: too many assumptions.
int AllocTempDouble(CompilationUnit* cu);

void FreeTemp(CompilationUnit* cu, int reg);

void ResetDefLocWide(CompilationUnit* cu, RegLocation rl);

void ResetDefTracking(CompilationUnit* cu);

RegisterInfo *IsLive(CompilationUnit* cu, int reg);

/* To be used when explicitly managing register use */
void LockCallTemps(CompilationUnit* cu);

void FreeCallTemps(CompilationUnit* cu);

void FlushAllRegs(CompilationUnit* cu);

RegLocation GetReturnWideAlt(CompilationUnit* cu);

RegLocation GetReturn(CompilationUnit* cu, bool is_float);

RegLocation GetReturnAlt(CompilationUnit* cu);

/* Clobber any temp associated with an s_reg.  Could be in either class */

/* Return a temp if one is available, -1 otherwise */
int AllocFreeTemp(CompilationUnit* cu);

/* Attempt to allocate a callee-save register */
/*
 * Similar to AllocTemp(), but forces the allocation of a specific
 * register.  No check is made to see if the register was previously
 * allocated.  Use with caution.
 */
void LockTemp(CompilationUnit* cu, int reg);

RegLocation WideToNarrow(CompilationUnit* cu, RegLocation rl);

/*
 * Free all allocated temps in the temp pools.  Note that this does
 * not affect the "liveness" of a temp register, which will stay
 * live until it is either explicitly killed or reallocated.
 */
void ResetRegPool(CompilationUnit* cu);

void ClobberAllRegs(CompilationUnit* cu);

void FlushRegWide(CompilationUnit* cu, int reg1, int reg2);

void FlushReg(CompilationUnit* cu, int reg);

void DoPromotion(CompilationUnit* cu);
int VRegOffset(CompilationUnit* cu, int reg);
int SRegOffset(CompilationUnit* cu, int reg);
void RecordCorePromotion(CompilationUnit* cu, int reg, int s_reg);
void RecordFpPromotion(CompilationUnit* cu, int reg, int s_reg);


/* Architecture-dependent register allocation routines. */
int AllocTypedTempPair(CompilationUnit* cu,
                                 bool fp_hint, int reg_class);

int AllocTypedTemp(CompilationUnit* cu, bool fp_hint, int reg_class);

void oatDumpFPRegPool(CompilationUnit* cUint);
RegisterInfo* GetRegInfo(CompilationUnit* cu, int reg);
void NopLIR(LIR* lir);
bool oatIsFPReg(int reg);
uint32_t oatFPRegMask(void);
void AdjustSpillMask(CompilationUnit* cu);
void MarkPreservedSingle(CompilationUnit* cu, int v_reg, int reg);
int ComputeFrameSize(CompilationUnit* cu);

}  // namespace art

#endif // ART_SRC_COMPILER_CODEGEN_RALLOCUTIL_H_
