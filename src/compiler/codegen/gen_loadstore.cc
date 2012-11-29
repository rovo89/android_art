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
 * register is clobbered, and marked in_use.
 */
LIR* Codegen::LoadConstant(CompilationUnit* cu, int r_dest, int value)
{
  if (IsTemp(cu, r_dest)) {
    Clobber(cu, r_dest);
    MarkInUse(cu, r_dest);
  }
  return LoadConstantNoClobber(cu, r_dest, value);
}

/*
 * Temporary workaround for Issue 7250540.  If we're loading a constant zero into a
 * promoted floating point register, also copy a zero into the int/ref identity of
 * that sreg.
 */
void Codegen::Workaround7250540(CompilationUnit* cu, RegLocation rl_dest, int value)
{
  if (rl_dest.fp && (value == 0)) {
    int pmap_index = SRegToPMap(cu, rl_dest.s_reg_low);
    if (cu->promotion_map[pmap_index].fp_location == kLocPhysReg) {
      if (cu->promotion_map[pmap_index].core_location == kLocPhysReg) {
        // Promoted - just copy in a zero
        LoadConstant(cu, cu->promotion_map[pmap_index].core_reg, 0);
      } else {
        // Lives in the frame, need to store.
        int temp_reg = AllocTemp(cu);
        LoadConstant(cu, temp_reg, 0);
        StoreBaseDisp(cu, TargetReg(kSp), SRegOffset(cu, rl_dest.s_reg_low), temp_reg, kWord);
      }
    }
  }
}

/* Load a word at base + displacement.  Displacement must be word multiple */
LIR* Codegen::LoadWordDisp(CompilationUnit* cu, int rBase, int displacement, int r_dest)
{
  return LoadBaseDisp(cu, rBase, displacement, r_dest, kWord,
                      INVALID_SREG);
}

LIR* Codegen::StoreWordDisp(CompilationUnit* cu, int rBase, int displacement, int r_src)
{
  return StoreBaseDisp(cu, rBase, displacement, r_src, kWord);
}

/*
 * Load a Dalvik register into a physical register.  Take care when
 * using this routine, as it doesn't perform any bookkeeping regarding
 * register liveness.  That is the responsibility of the caller.
 */
void Codegen::LoadValueDirect(CompilationUnit* cu, RegLocation rl_src, int r_dest)
{
  rl_src = UpdateLoc(cu, rl_src);
  if (rl_src.location == kLocPhysReg) {
    OpRegCopy(cu, r_dest, rl_src.low_reg);
  } else {
    DCHECK((rl_src.location == kLocDalvikFrame) ||
           (rl_src.location == kLocCompilerTemp));
    LoadWordDisp(cu, TargetReg(kSp), SRegOffset(cu, rl_src.s_reg_low), r_dest);
  }
}

/*
 * Similar to LoadValueDirect, but clobbers and allocates the target
 * register.  Should be used when loading to a fixed register (for example,
 * loading arguments to an out of line call.
 */
void Codegen::LoadValueDirectFixed(CompilationUnit* cu, RegLocation rl_src, int r_dest)
{
  Clobber(cu, r_dest);
  MarkInUse(cu, r_dest);
  LoadValueDirect(cu, rl_src, r_dest);
}

/*
 * Load a Dalvik register pair into a physical register[s].  Take care when
 * using this routine, as it doesn't perform any bookkeeping regarding
 * register liveness.  That is the responsibility of the caller.
 */
void Codegen::LoadValueDirectWide(CompilationUnit* cu, RegLocation rl_src, int reg_lo,
             int reg_hi)
{
  rl_src = UpdateLocWide(cu, rl_src);
  if (rl_src.location == kLocPhysReg) {
    OpRegCopyWide(cu, reg_lo, reg_hi, rl_src.low_reg, rl_src.high_reg);
  } else {
    DCHECK((rl_src.location == kLocDalvikFrame) ||
           (rl_src.location == kLocCompilerTemp));
    LoadBaseDispWide(cu, TargetReg(kSp), SRegOffset(cu, rl_src.s_reg_low),
                     reg_lo, reg_hi, INVALID_SREG);
  }
}

/*
 * Similar to LoadValueDirect, but clobbers and allocates the target
 * registers.  Should be used when loading to a fixed registers (for example,
 * loading arguments to an out of line call.
 */
void Codegen::LoadValueDirectWideFixed(CompilationUnit* cu, RegLocation rl_src, int reg_lo,
                                       int reg_hi)
{
  Clobber(cu, reg_lo);
  Clobber(cu, reg_hi);
  MarkInUse(cu, reg_lo);
  MarkInUse(cu, reg_hi);
  LoadValueDirectWide(cu, rl_src, reg_lo, reg_hi);
}

RegLocation Codegen::LoadValue(CompilationUnit* cu, RegLocation rl_src, RegisterClass op_kind)
{
  rl_src = EvalLoc(cu, rl_src, op_kind, false);
  if (rl_src.location != kLocPhysReg) {
    DCHECK((rl_src.location == kLocDalvikFrame) ||
           (rl_src.location == kLocCompilerTemp));
    LoadValueDirect(cu, rl_src, rl_src.low_reg);
    rl_src.location = kLocPhysReg;
    MarkLive(cu, rl_src.low_reg, rl_src.s_reg_low);
  }
  return rl_src;
}

void Codegen::StoreValue(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
{
#ifndef NDEBUG
  /*
   * Sanity checking - should never try to store to the same
   * ssa name during the compilation of a single instruction
   * without an intervening ClobberSReg().
   */
  DCHECK((cu->live_sreg == INVALID_SREG) ||
         (rl_dest.s_reg_low != cu->live_sreg));
  cu->live_sreg = rl_dest.s_reg_low;
#endif
  LIR* def_start;
  LIR* def_end;
  DCHECK(!rl_dest.wide);
  DCHECK(!rl_src.wide);
  rl_src = UpdateLoc(cu, rl_src);
  rl_dest = UpdateLoc(cu, rl_dest);
  if (rl_src.location == kLocPhysReg) {
    if (IsLive(cu, rl_src.low_reg) ||
      IsPromoted(cu, rl_src.low_reg) ||
      (rl_dest.location == kLocPhysReg)) {
      // Src is live/promoted or Dest has assigned reg.
      rl_dest = EvalLoc(cu, rl_dest, kAnyReg, false);
      OpRegCopy(cu, rl_dest.low_reg, rl_src.low_reg);
    } else {
      // Just re-assign the registers.  Dest gets Src's regs
      rl_dest.low_reg = rl_src.low_reg;
      Clobber(cu, rl_src.low_reg);
    }
  } else {
    // Load Src either into promoted Dest or temps allocated for Dest
    rl_dest = EvalLoc(cu, rl_dest, kAnyReg, false);
    LoadValueDirect(cu, rl_src, rl_dest.low_reg);
  }

  // Dest is now live and dirty (until/if we flush it to home location)
  MarkLive(cu, rl_dest.low_reg, rl_dest.s_reg_low);
  MarkDirty(cu, rl_dest);


  ResetDefLoc(cu, rl_dest);
  if (IsDirty(cu, rl_dest.low_reg) &&
      oat_live_out(cu, rl_dest.s_reg_low)) {
    def_start = cu->last_lir_insn;
    StoreBaseDisp(cu, TargetReg(kSp), SRegOffset(cu, rl_dest.s_reg_low),
                  rl_dest.low_reg, kWord);
    MarkClean(cu, rl_dest);
    def_end = cu->last_lir_insn;
    if (!rl_dest.ref) {
      // Exclude references from store elimination
      MarkDef(cu, rl_dest, def_start, def_end);
    }
  }
}

RegLocation Codegen::LoadValueWide(CompilationUnit* cu, RegLocation rl_src, RegisterClass op_kind)
{
  DCHECK(rl_src.wide);
  rl_src = EvalLoc(cu, rl_src, op_kind, false);
  if (rl_src.location != kLocPhysReg) {
    DCHECK((rl_src.location == kLocDalvikFrame) ||
        (rl_src.location == kLocCompilerTemp));
    LoadValueDirectWide(cu, rl_src, rl_src.low_reg, rl_src.high_reg);
    rl_src.location = kLocPhysReg;
    MarkLive(cu, rl_src.low_reg, rl_src.s_reg_low);
    MarkLive(cu, rl_src.high_reg,
                GetSRegHi(rl_src.s_reg_low));
  }
  return rl_src;
}

void Codegen::StoreValueWide(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
{
#ifndef NDEBUG
  /*
   * Sanity checking - should never try to store to the same
   * ssa name during the compilation of a single instruction
   * without an intervening ClobberSReg().
   */
  DCHECK((cu->live_sreg == INVALID_SREG) ||
      (rl_dest.s_reg_low != cu->live_sreg));
  cu->live_sreg = rl_dest.s_reg_low;
#endif
  LIR* def_start;
  LIR* def_end;
  DCHECK_EQ(IsFpReg(rl_src.low_reg), IsFpReg(rl_src.high_reg));
  DCHECK(rl_dest.wide);
  DCHECK(rl_src.wide);
  if (rl_src.location == kLocPhysReg) {
    if (IsLive(cu, rl_src.low_reg) ||
        IsLive(cu, rl_src.high_reg) ||
        IsPromoted(cu, rl_src.low_reg) ||
        IsPromoted(cu, rl_src.high_reg) ||
        (rl_dest.location == kLocPhysReg)) {
      // Src is live or promoted or Dest has assigned reg.
      rl_dest = EvalLoc(cu, rl_dest, kAnyReg, false);
      OpRegCopyWide(cu, rl_dest.low_reg, rl_dest.high_reg,
                    rl_src.low_reg, rl_src.high_reg);
    } else {
      // Just re-assign the registers.  Dest gets Src's regs
      rl_dest.low_reg = rl_src.low_reg;
      rl_dest.high_reg = rl_src.high_reg;
      Clobber(cu, rl_src.low_reg);
      Clobber(cu, rl_src.high_reg);
    }
  } else {
    // Load Src either into promoted Dest or temps allocated for Dest
    rl_dest = EvalLoc(cu, rl_dest, kAnyReg, false);
    LoadValueDirectWide(cu, rl_src, rl_dest.low_reg, rl_dest.high_reg);
  }

  // Dest is now live and dirty (until/if we flush it to home location)
  MarkLive(cu, rl_dest.low_reg, rl_dest.s_reg_low);
  MarkLive(cu, rl_dest.high_reg, GetSRegHi(rl_dest.s_reg_low));
  MarkDirty(cu, rl_dest);
  MarkPair(cu, rl_dest.low_reg, rl_dest.high_reg);


  ResetDefLocWide(cu, rl_dest);
  if ((IsDirty(cu, rl_dest.low_reg) ||
      IsDirty(cu, rl_dest.high_reg)) &&
      (oat_live_out(cu, rl_dest.s_reg_low) ||
      oat_live_out(cu, GetSRegHi(rl_dest.s_reg_low)))) {
    def_start = cu->last_lir_insn;
    DCHECK_EQ((SRegToVReg(cu, rl_dest.s_reg_low)+1),
              SRegToVReg(cu, GetSRegHi(rl_dest.s_reg_low)));
    StoreBaseDispWide(cu, TargetReg(kSp), SRegOffset(cu, rl_dest.s_reg_low),
                      rl_dest.low_reg, rl_dest.high_reg);
    MarkClean(cu, rl_dest);
    def_end = cu->last_lir_insn;
    MarkDefWide(cu, rl_dest, def_start, def_end);
  }
}

/* Utilities to load the current Method* */
void Codegen::LoadCurrMethodDirect(CompilationUnit *cu, int r_tgt)
{
  LoadValueDirectFixed(cu, cu->method_loc, r_tgt);
}

RegLocation Codegen::LoadCurrMethod(CompilationUnit *cu)
{
  return LoadValue(cu, cu->method_loc, kCoreReg);
}

}  // namespace art
