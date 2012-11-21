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

#include "../compiler_ir.h"
#include "ralloc_util.h"
#include "codegen_util.h"

namespace art {

/* This file contains target-independent codegen and support. */

/*
 * Load an immediate value into a fixed or temp register.  Target
 * register is clobbered, and marked inUse.
 */
LIR* LoadConstant(CompilationUnit* cUnit, int rDest, int value)
{
  if (IsTemp(cUnit, rDest)) {
    Clobber(cUnit, rDest);
    MarkInUse(cUnit, rDest);
  }
  return LoadConstantNoClobber(cUnit, rDest, value);
}

/* Load a word at base + displacement.  Displacement must be word multiple */
LIR* LoadWordDisp(CompilationUnit* cUnit, int rBase, int displacement,
                  int rDest)
{
  return LoadBaseDisp(cUnit, rBase, displacement, rDest, kWord,
                      INVALID_SREG);
}

LIR* StoreWordDisp(CompilationUnit* cUnit, int rBase, int displacement,
                   int rSrc)
{
  return StoreBaseDisp(cUnit, rBase, displacement, rSrc, kWord);
}

/*
 * Load a Dalvik register into a physical register.  Take care when
 * using this routine, as it doesn't perform any bookkeeping regarding
 * register liveness.  That is the responsibility of the caller.
 */
void LoadValueDirect(CompilationUnit* cUnit, RegLocation rlSrc, int rDest)
{
  rlSrc = UpdateLoc(cUnit, rlSrc);
  if (rlSrc.location == kLocPhysReg) {
    OpRegCopy(cUnit, rDest, rlSrc.lowReg);
  } else {
    DCHECK((rlSrc.location == kLocDalvikFrame) ||
           (rlSrc.location == kLocCompilerTemp));
    LoadWordDisp(cUnit, TargetReg(kSp), SRegOffset(cUnit, rlSrc.sRegLow), rDest);
  }
}

/*
 * Similar to LoadValueDirect, but clobbers and allocates the target
 * register.  Should be used when loading to a fixed register (for example,
 * loading arguments to an out of line call.
 */
void LoadValueDirectFixed(CompilationUnit* cUnit, RegLocation rlSrc, int rDest)
{
  Clobber(cUnit, rDest);
  MarkInUse(cUnit, rDest);
  LoadValueDirect(cUnit, rlSrc, rDest);
}

/*
 * Load a Dalvik register pair into a physical register[s].  Take care when
 * using this routine, as it doesn't perform any bookkeeping regarding
 * register liveness.  That is the responsibility of the caller.
 */
void LoadValueDirectWide(CompilationUnit* cUnit, RegLocation rlSrc, int regLo,
             int regHi)
{
  rlSrc = UpdateLocWide(cUnit, rlSrc);
  if (rlSrc.location == kLocPhysReg) {
    OpRegCopyWide(cUnit, regLo, regHi, rlSrc.lowReg, rlSrc.highReg);
  } else {
    DCHECK((rlSrc.location == kLocDalvikFrame) ||
           (rlSrc.location == kLocCompilerTemp));
    LoadBaseDispWide(cUnit, TargetReg(kSp), SRegOffset(cUnit, rlSrc.sRegLow),
                     regLo, regHi, INVALID_SREG);
  }
}

/*
 * Similar to LoadValueDirect, but clobbers and allocates the target
 * registers.  Should be used when loading to a fixed registers (for example,
 * loading arguments to an out of line call.
 */
void LoadValueDirectWideFixed(CompilationUnit* cUnit, RegLocation rlSrc,
                              int regLo, int regHi)
{
  Clobber(cUnit, regLo);
  Clobber(cUnit, regHi);
  MarkInUse(cUnit, regLo);
  MarkInUse(cUnit, regHi);
  LoadValueDirectWide(cUnit, rlSrc, regLo, regHi);
}

RegLocation LoadValue(CompilationUnit* cUnit, RegLocation rlSrc,
                      RegisterClass opKind)
{
  rlSrc = EvalLoc(cUnit, rlSrc, opKind, false);
  if (rlSrc.location != kLocPhysReg) {
    DCHECK((rlSrc.location == kLocDalvikFrame) ||
           (rlSrc.location == kLocCompilerTemp));
    LoadValueDirect(cUnit, rlSrc, rlSrc.lowReg);
    rlSrc.location = kLocPhysReg;
    MarkLive(cUnit, rlSrc.lowReg, rlSrc.sRegLow);
  }
  return rlSrc;
}

void StoreValue(CompilationUnit* cUnit, RegLocation rlDest, RegLocation rlSrc)
{
#ifndef NDEBUG
  /*
   * Sanity checking - should never try to store to the same
   * ssa name during the compilation of a single instruction
   * without an intervening ClobberSReg().
   */
  DCHECK((cUnit->liveSReg == INVALID_SREG) ||
         (rlDest.sRegLow != cUnit->liveSReg));
  cUnit->liveSReg = rlDest.sRegLow;
#endif
  LIR* defStart;
  LIR* defEnd;
  DCHECK(!rlDest.wide);
  DCHECK(!rlSrc.wide);
  rlSrc = UpdateLoc(cUnit, rlSrc);
  rlDest = UpdateLoc(cUnit, rlDest);
  if (rlSrc.location == kLocPhysReg) {
    if (IsLive(cUnit, rlSrc.lowReg) ||
      IsPromoted(cUnit, rlSrc.lowReg) ||
      (rlDest.location == kLocPhysReg)) {
      // Src is live/promoted or Dest has assigned reg.
      rlDest = EvalLoc(cUnit, rlDest, kAnyReg, false);
      OpRegCopy(cUnit, rlDest.lowReg, rlSrc.lowReg);
    } else {
      // Just re-assign the registers.  Dest gets Src's regs
      rlDest.lowReg = rlSrc.lowReg;
      Clobber(cUnit, rlSrc.lowReg);
    }
  } else {
    // Load Src either into promoted Dest or temps allocated for Dest
    rlDest = EvalLoc(cUnit, rlDest, kAnyReg, false);
    LoadValueDirect(cUnit, rlSrc, rlDest.lowReg);
  }

  // Dest is now live and dirty (until/if we flush it to home location)
  MarkLive(cUnit, rlDest.lowReg, rlDest.sRegLow);
  MarkDirty(cUnit, rlDest);


  ResetDefLoc(cUnit, rlDest);
  if (IsDirty(cUnit, rlDest.lowReg) &&
      oatLiveOut(cUnit, rlDest.sRegLow)) {
    defStart = cUnit->lastLIRInsn;
    StoreBaseDisp(cUnit, TargetReg(kSp), SRegOffset(cUnit, rlDest.sRegLow),
                  rlDest.lowReg, kWord);
    MarkClean(cUnit, rlDest);
    defEnd = cUnit->lastLIRInsn;
    MarkDef(cUnit, rlDest, defStart, defEnd);
  }
}

RegLocation LoadValueWide(CompilationUnit* cUnit, RegLocation rlSrc,
              RegisterClass opKind)
{
  DCHECK(rlSrc.wide);
  rlSrc = EvalLoc(cUnit, rlSrc, opKind, false);
  if (rlSrc.location != kLocPhysReg) {
    DCHECK((rlSrc.location == kLocDalvikFrame) ||
        (rlSrc.location == kLocCompilerTemp));
    LoadValueDirectWide(cUnit, rlSrc, rlSrc.lowReg, rlSrc.highReg);
    rlSrc.location = kLocPhysReg;
    MarkLive(cUnit, rlSrc.lowReg, rlSrc.sRegLow);
    MarkLive(cUnit, rlSrc.highReg,
                oatSRegHi(rlSrc.sRegLow));
  }
  return rlSrc;
}

void StoreValueWide(CompilationUnit* cUnit, RegLocation rlDest,
          RegLocation rlSrc)
{
#ifndef NDEBUG
  /*
   * Sanity checking - should never try to store to the same
   * ssa name during the compilation of a single instruction
   * without an intervening ClobberSReg().
   */
  DCHECK((cUnit->liveSReg == INVALID_SREG) ||
      (rlDest.sRegLow != cUnit->liveSReg));
  cUnit->liveSReg = rlDest.sRegLow;
#endif
  LIR* defStart;
  LIR* defEnd;
  DCHECK_EQ(FpReg(rlSrc.lowReg), FpReg(rlSrc.highReg));
  DCHECK(rlDest.wide);
  DCHECK(rlSrc.wide);
  if (rlSrc.location == kLocPhysReg) {
    if (IsLive(cUnit, rlSrc.lowReg) ||
        IsLive(cUnit, rlSrc.highReg) ||
        IsPromoted(cUnit, rlSrc.lowReg) ||
        IsPromoted(cUnit, rlSrc.highReg) ||
        (rlDest.location == kLocPhysReg)) {
      // Src is live or promoted or Dest has assigned reg.
      rlDest = EvalLoc(cUnit, rlDest, kAnyReg, false);
      OpRegCopyWide(cUnit, rlDest.lowReg, rlDest.highReg,
                    rlSrc.lowReg, rlSrc.highReg);
    } else {
      // Just re-assign the registers.  Dest gets Src's regs
      rlDest.lowReg = rlSrc.lowReg;
      rlDest.highReg = rlSrc.highReg;
      Clobber(cUnit, rlSrc.lowReg);
      Clobber(cUnit, rlSrc.highReg);
    }
  } else {
    // Load Src either into promoted Dest or temps allocated for Dest
    rlDest = EvalLoc(cUnit, rlDest, kAnyReg, false);
    LoadValueDirectWide(cUnit, rlSrc, rlDest.lowReg, rlDest.highReg);
  }

  // Dest is now live and dirty (until/if we flush it to home location)
  MarkLive(cUnit, rlDest.lowReg, rlDest.sRegLow);
  MarkLive(cUnit, rlDest.highReg, oatSRegHi(rlDest.sRegLow));
  MarkDirty(cUnit, rlDest);
  MarkPair(cUnit, rlDest.lowReg, rlDest.highReg);


  ResetDefLocWide(cUnit, rlDest);
  if ((IsDirty(cUnit, rlDest.lowReg) ||
      IsDirty(cUnit, rlDest.highReg)) &&
      (oatLiveOut(cUnit, rlDest.sRegLow) ||
      oatLiveOut(cUnit, oatSRegHi(rlDest.sRegLow)))) {
    defStart = cUnit->lastLIRInsn;
    DCHECK_EQ((SRegToVReg(cUnit, rlDest.sRegLow)+1),
              SRegToVReg(cUnit, oatSRegHi(rlDest.sRegLow)));
    StoreBaseDispWide(cUnit, TargetReg(kSp), SRegOffset(cUnit, rlDest.sRegLow),
                      rlDest.lowReg, rlDest.highReg);
    MarkClean(cUnit, rlDest);
    defEnd = cUnit->lastLIRInsn;
    MarkDefWide(cUnit, rlDest, defStart, defEnd);
  }
}

/* Utilities to load the current Method* */
void LoadCurrMethodDirect(CompilationUnit *cUnit, int rTgt)
{
  LoadValueDirectFixed(cUnit, cUnit->methodLoc, rTgt);
}

RegLocation LoadCurrMethod(CompilationUnit *cUnit)
{
  return LoadValue(cUnit, cUnit->methodLoc, kCoreReg);
}

bool MethodStarInReg(CompilationUnit* cUnit)
{
   return (cUnit->methodLoc.location == kLocPhysReg);
}


}  // namespace art
