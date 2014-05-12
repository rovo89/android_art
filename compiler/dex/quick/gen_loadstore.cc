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

#include "dex/compiler_ir.h"
#include "dex/compiler_internals.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "invoke_type.h"

namespace art {

/* This file contains target-independent codegen and support. */

/*
 * Load an immediate value into a fixed or temp register.  Target
 * register is clobbered, and marked in_use.
 */
LIR* Mir2Lir::LoadConstant(RegStorage r_dest, int value) {
  if (IsTemp(r_dest)) {
    Clobber(r_dest);
    MarkInUse(r_dest);
  }
  return LoadConstantNoClobber(r_dest, value);
}

/*
 * Temporary workaround for Issue 7250540.  If we're loading a constant zero into a
 * promoted floating point register, also copy a zero into the int/ref identity of
 * that sreg.
 */
void Mir2Lir::Workaround7250540(RegLocation rl_dest, RegStorage zero_reg) {
  if (rl_dest.fp) {
    int pmap_index = SRegToPMap(rl_dest.s_reg_low);
    if (promotion_map_[pmap_index].fp_location == kLocPhysReg) {
      // Now, determine if this vreg is ever used as a reference.  If not, we're done.
      bool used_as_reference = false;
      int base_vreg = mir_graph_->SRegToVReg(rl_dest.s_reg_low);
      for (int i = 0; !used_as_reference && (i < mir_graph_->GetNumSSARegs()); i++) {
        if (mir_graph_->SRegToVReg(mir_graph_->reg_location_[i].s_reg_low) == base_vreg) {
          used_as_reference |= mir_graph_->reg_location_[i].ref;
        }
      }
      if (!used_as_reference) {
        return;
      }
      RegStorage temp_reg = zero_reg;
      if (!temp_reg.Valid()) {
        temp_reg = AllocTemp();
        LoadConstant(temp_reg, 0);
      }
      if (promotion_map_[pmap_index].core_location == kLocPhysReg) {
        // Promoted - just copy in a zero
        OpRegCopy(RegStorage::Solo32(promotion_map_[pmap_index].core_reg), temp_reg);
      } else {
        // Lives in the frame, need to store.
        StoreBaseDisp(TargetReg(kSp), SRegOffset(rl_dest.s_reg_low), temp_reg, k32);
      }
      if (!zero_reg.Valid()) {
        FreeTemp(temp_reg);
      }
    }
  }
}

/*
 * Load a Dalvik register into a physical register.  Take care when
 * using this routine, as it doesn't perform any bookkeeping regarding
 * register liveness.  That is the responsibility of the caller.
 */
void Mir2Lir::LoadValueDirect(RegLocation rl_src, RegStorage r_dest) {
  rl_src = UpdateLoc(rl_src);
  if (rl_src.location == kLocPhysReg) {
    OpRegCopy(r_dest, rl_src.reg);
  } else if (IsInexpensiveConstant(rl_src)) {
    // On 64-bit targets, will sign extend.  Make sure constant reference is always NULL.
    DCHECK(!rl_src.ref || (mir_graph_->ConstantValue(rl_src) == 0));
    LoadConstantNoClobber(r_dest, mir_graph_->ConstantValue(rl_src));
  } else {
    DCHECK((rl_src.location == kLocDalvikFrame) ||
           (rl_src.location == kLocCompilerTemp));
    if (rl_src.ref) {
      LoadRefDisp(TargetReg(kSp), SRegOffset(rl_src.s_reg_low), r_dest);
    } else {
      Load32Disp(TargetReg(kSp), SRegOffset(rl_src.s_reg_low), r_dest);
    }
  }
}

/*
 * Similar to LoadValueDirect, but clobbers and allocates the target
 * register.  Should be used when loading to a fixed register (for example,
 * loading arguments to an out of line call.
 */
void Mir2Lir::LoadValueDirectFixed(RegLocation rl_src, RegStorage r_dest) {
  Clobber(r_dest);
  MarkInUse(r_dest);
  LoadValueDirect(rl_src, r_dest);
}

/*
 * Load a Dalvik register pair into a physical register[s].  Take care when
 * using this routine, as it doesn't perform any bookkeeping regarding
 * register liveness.  That is the responsibility of the caller.
 */
void Mir2Lir::LoadValueDirectWide(RegLocation rl_src, RegStorage r_dest) {
  rl_src = UpdateLocWide(rl_src);
  if (rl_src.location == kLocPhysReg) {
    OpRegCopyWide(r_dest, rl_src.reg);
  } else if (IsInexpensiveConstant(rl_src)) {
    LoadConstantWide(r_dest, mir_graph_->ConstantValueWide(rl_src));
  } else {
    DCHECK((rl_src.location == kLocDalvikFrame) ||
           (rl_src.location == kLocCompilerTemp));
    LoadBaseDisp(TargetReg(kSp), SRegOffset(rl_src.s_reg_low), r_dest, k64);
  }
}

/*
 * Similar to LoadValueDirect, but clobbers and allocates the target
 * registers.  Should be used when loading to a fixed registers (for example,
 * loading arguments to an out of line call.
 */
void Mir2Lir::LoadValueDirectWideFixed(RegLocation rl_src, RegStorage r_dest) {
  Clobber(r_dest);
  MarkInUse(r_dest);
  LoadValueDirectWide(rl_src, r_dest);
}

RegLocation Mir2Lir::LoadValue(RegLocation rl_src, RegisterClass op_kind) {
  rl_src = UpdateLoc(rl_src);
  if (rl_src.location == kLocPhysReg) {
    if (!RegClassMatches(op_kind, rl_src.reg)) {
      // Wrong register class, realloc, copy and transfer ownership.
      RegStorage new_reg = AllocTypedTemp(rl_src.fp, op_kind);
      OpRegCopy(new_reg, rl_src.reg);
      // Associate the old sreg with the new register and clobber the old register.
      GetRegInfo(new_reg)->SetSReg(GetRegInfo(rl_src.reg)->SReg());
      Clobber(rl_src.reg);
      rl_src.reg = new_reg;
    }
    return rl_src;
  }

  DCHECK_NE(rl_src.s_reg_low, INVALID_SREG);
  rl_src.reg = AllocTypedTemp(rl_src.fp, op_kind);
  LoadValueDirect(rl_src, rl_src.reg);
  rl_src.location = kLocPhysReg;
  MarkLive(rl_src);
  return rl_src;
}

void Mir2Lir::StoreValue(RegLocation rl_dest, RegLocation rl_src) {
  /*
   * Sanity checking - should never try to store to the same
   * ssa name during the compilation of a single instruction
   * without an intervening ClobberSReg().
   */
  if (kIsDebugBuild) {
    DCHECK((live_sreg_ == INVALID_SREG) ||
           (rl_dest.s_reg_low != live_sreg_));
    live_sreg_ = rl_dest.s_reg_low;
  }
  LIR* def_start;
  LIR* def_end;
  DCHECK(!rl_dest.wide);
  DCHECK(!rl_src.wide);
  rl_src = UpdateLoc(rl_src);
  rl_dest = UpdateLoc(rl_dest);
  if (rl_src.location == kLocPhysReg) {
    if (IsLive(rl_src.reg) ||
      IsPromoted(rl_src.reg) ||
      (rl_dest.location == kLocPhysReg)) {
      // Src is live/promoted or Dest has assigned reg.
      rl_dest = EvalLoc(rl_dest, kAnyReg, false);
      OpRegCopy(rl_dest.reg, rl_src.reg);
    } else {
      // Just re-assign the registers.  Dest gets Src's regs
      rl_dest.reg = rl_src.reg;
      Clobber(rl_src.reg);
    }
  } else {
    // Load Src either into promoted Dest or temps allocated for Dest
    rl_dest = EvalLoc(rl_dest, kAnyReg, false);
    LoadValueDirect(rl_src, rl_dest.reg);
  }

  // Dest is now live and dirty (until/if we flush it to home location)
  MarkLive(rl_dest);
  MarkDirty(rl_dest);


  ResetDefLoc(rl_dest);
  if (IsDirty(rl_dest.reg) && LiveOut(rl_dest.s_reg_low)) {
    def_start = last_lir_insn_;
    Store32Disp(TargetReg(kSp), SRegOffset(rl_dest.s_reg_low), rl_dest.reg);
    MarkClean(rl_dest);
    def_end = last_lir_insn_;
    if (!rl_dest.ref) {
      // Exclude references from store elimination
      MarkDef(rl_dest, def_start, def_end);
    }
  }
}

RegLocation Mir2Lir::LoadValueWide(RegLocation rl_src, RegisterClass op_kind) {
  DCHECK(rl_src.wide);
  rl_src = UpdateLocWide(rl_src);
  if (rl_src.location == kLocPhysReg) {
    if (!RegClassMatches(op_kind, rl_src.reg)) {
      // Wrong register class, realloc, copy and transfer ownership.
      RegStorage new_regs = AllocTypedTempWide(rl_src.fp, op_kind);
      OpRegCopyWide(new_regs, rl_src.reg);
      // Associate the old sreg with the new register and clobber the old register.
      GetRegInfo(new_regs)->SetSReg(GetRegInfo(rl_src.reg)->SReg());
      Clobber(rl_src.reg);
      rl_src.reg = new_regs;
    }
    return rl_src;
  }

  DCHECK_NE(rl_src.s_reg_low, INVALID_SREG);
  DCHECK_NE(GetSRegHi(rl_src.s_reg_low), INVALID_SREG);
  rl_src.reg = AllocTypedTempWide(rl_src.fp, op_kind);
  LoadValueDirectWide(rl_src, rl_src.reg);
  rl_src.location = kLocPhysReg;
  MarkLive(rl_src);
  return rl_src;
}

void Mir2Lir::StoreValueWide(RegLocation rl_dest, RegLocation rl_src) {
  /*
   * Sanity checking - should never try to store to the same
   * ssa name during the compilation of a single instruction
   * without an intervening ClobberSReg().
   */
  if (kIsDebugBuild) {
    DCHECK((live_sreg_ == INVALID_SREG) ||
           (rl_dest.s_reg_low != live_sreg_));
    live_sreg_ = rl_dest.s_reg_low;
  }
  LIR* def_start;
  LIR* def_end;
  DCHECK(rl_dest.wide);
  DCHECK(rl_src.wide);
  rl_src = UpdateLocWide(rl_src);
  rl_dest = UpdateLocWide(rl_dest);
  if (rl_src.location == kLocPhysReg) {
    if (IsLive(rl_src.reg) ||
        IsPromoted(rl_src.reg) ||
        (rl_dest.location == kLocPhysReg)) {
      /*
       * If src reg[s] are tied to the original Dalvik vreg via liveness or promotion, we
       * can't repurpose them.  Similarly, if the dest reg[s] are tied to Dalvik vregs via
       * promotion, we can't just re-assign.  In these cases, we have to copy.
       */
      rl_dest = EvalLoc(rl_dest, kAnyReg, false);
      OpRegCopyWide(rl_dest.reg, rl_src.reg);
    } else {
      // Just re-assign the registers.  Dest gets Src's regs
      rl_dest.reg = rl_src.reg;
      Clobber(rl_src.reg);
    }
  } else {
    // Load Src either into promoted Dest or temps allocated for Dest
    rl_dest = EvalLoc(rl_dest, kAnyReg, false);
    LoadValueDirectWide(rl_src, rl_dest.reg);
  }

  // Dest is now live and dirty (until/if we flush it to home location)
  MarkLive(rl_dest);
  MarkWide(rl_dest.reg);
  MarkDirty(rl_dest);

  ResetDefLocWide(rl_dest);
  if (IsDirty(rl_dest.reg) && (LiveOut(rl_dest.s_reg_low) ||
      LiveOut(GetSRegHi(rl_dest.s_reg_low)))) {
    def_start = last_lir_insn_;
    DCHECK_EQ((mir_graph_->SRegToVReg(rl_dest.s_reg_low)+1),
              mir_graph_->SRegToVReg(GetSRegHi(rl_dest.s_reg_low)));
    StoreBaseDisp(TargetReg(kSp), SRegOffset(rl_dest.s_reg_low), rl_dest.reg, k64);
    MarkClean(rl_dest);
    def_end = last_lir_insn_;
    MarkDefWide(rl_dest, def_start, def_end);
  }
}

void Mir2Lir::StoreFinalValue(RegLocation rl_dest, RegLocation rl_src) {
  DCHECK_EQ(rl_src.location, kLocPhysReg);

  if (rl_dest.location == kLocPhysReg) {
    OpRegCopy(rl_dest.reg, rl_src.reg);
  } else {
    // Just re-assign the register.  Dest gets Src's reg.
    rl_dest.location = kLocPhysReg;
    rl_dest.reg = rl_src.reg;
    Clobber(rl_src.reg);
  }

  // Dest is now live and dirty (until/if we flush it to home location)
  MarkLive(rl_dest);
  MarkDirty(rl_dest);


  ResetDefLoc(rl_dest);
  if (IsDirty(rl_dest.reg) && LiveOut(rl_dest.s_reg_low)) {
    LIR *def_start = last_lir_insn_;
    Store32Disp(TargetReg(kSp), SRegOffset(rl_dest.s_reg_low), rl_dest.reg);
    MarkClean(rl_dest);
    LIR *def_end = last_lir_insn_;
    if (!rl_dest.ref) {
      // Exclude references from store elimination
      MarkDef(rl_dest, def_start, def_end);
    }
  }
}

void Mir2Lir::StoreFinalValueWide(RegLocation rl_dest, RegLocation rl_src) {
  DCHECK(rl_dest.wide);
  DCHECK(rl_src.wide);
  DCHECK_EQ(rl_src.location, kLocPhysReg);

  if (rl_dest.location == kLocPhysReg) {
    OpRegCopyWide(rl_dest.reg, rl_src.reg);
  } else {
    // Just re-assign the registers.  Dest gets Src's regs.
    rl_dest.location = kLocPhysReg;
    rl_dest.reg = rl_src.reg;
    Clobber(rl_src.reg);
  }

  // Dest is now live and dirty (until/if we flush it to home location).
  MarkLive(rl_dest);
  MarkWide(rl_dest.reg);
  MarkDirty(rl_dest);

  ResetDefLocWide(rl_dest);
  if (IsDirty(rl_dest.reg) && (LiveOut(rl_dest.s_reg_low) ||
      LiveOut(GetSRegHi(rl_dest.s_reg_low)))) {
    LIR *def_start = last_lir_insn_;
    DCHECK_EQ((mir_graph_->SRegToVReg(rl_dest.s_reg_low)+1),
              mir_graph_->SRegToVReg(GetSRegHi(rl_dest.s_reg_low)));
    StoreBaseDisp(TargetReg(kSp), SRegOffset(rl_dest.s_reg_low), rl_dest.reg, k64);
    MarkClean(rl_dest);
    LIR *def_end = last_lir_insn_;
    MarkDefWide(rl_dest, def_start, def_end);
  }
}

/* Utilities to load the current Method* */
void Mir2Lir::LoadCurrMethodDirect(RegStorage r_tgt) {
  LoadValueDirectFixed(mir_graph_->GetMethodLoc(), r_tgt);
}

RegLocation Mir2Lir::LoadCurrMethod() {
  return LoadValue(mir_graph_->GetMethodLoc(), kCoreReg);
}

RegLocation Mir2Lir::ForceTemp(RegLocation loc) {
  DCHECK(!loc.wide);
  DCHECK(loc.location == kLocPhysReg);
  DCHECK(!loc.reg.IsFloat());
  if (IsTemp(loc.reg)) {
    Clobber(loc.reg);
  } else {
    RegStorage temp_low = AllocTemp();
    OpRegCopy(temp_low, loc.reg);
    loc.reg = temp_low;
  }

  // Ensure that this doesn't represent the original SR any more.
  loc.s_reg_low = INVALID_SREG;
  return loc;
}

// FIXME: will need an update for 64-bit core regs.
RegLocation Mir2Lir::ForceTempWide(RegLocation loc) {
  DCHECK(loc.wide);
  DCHECK(loc.location == kLocPhysReg);
  DCHECK(!loc.reg.IsFloat());
  if (IsTemp(loc.reg.GetLow())) {
    Clobber(loc.reg.GetLow());
  } else {
    RegStorage temp_low = AllocTemp();
    OpRegCopy(temp_low, loc.reg.GetLow());
    loc.reg.SetLowReg(temp_low.GetReg());
  }
  if (IsTemp(loc.reg.GetHigh())) {
    Clobber(loc.reg.GetHigh());
  } else {
    RegStorage temp_high = AllocTemp();
    OpRegCopy(temp_high, loc.reg.GetHigh());
    loc.reg.SetHighReg(temp_high.GetReg());
  }

  // Ensure that this doesn't represent the original SR any more.
  loc.s_reg_low = INVALID_SREG;
  return loc;
}

}  // namespace art
