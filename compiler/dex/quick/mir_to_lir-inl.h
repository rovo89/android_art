/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_COMPILER_DEX_QUICK_MIR_TO_LIR_INL_H_
#define ART_COMPILER_DEX_QUICK_MIR_TO_LIR_INL_H_

#include "mir_to_lir.h"

#include "dex/compiler_internals.h"

namespace art {

/* Mark a temp register as dead.  Does not affect allocation state. */
inline void Mir2Lir::ClobberBody(RegisterInfo* p) {
  DCHECK(p->IsTemp());
  if (!p->IsDead()) {
    DCHECK(!(p->IsLive() && p->IsDirty()))  << "Live & dirty temp in clobber";
    p->MarkDead();
    p->ResetDefBody();
    if (p->IsWide()) {
      p->SetIsWide(false);
      if (p->GetReg() != p->Partner()) {
        // Register pair - deal with the other half.
        p = GetRegInfo(p->Partner());
        p->SetIsWide(false);
        p->MarkDead();
        p->ResetDefBody();
      }
    }
  }
}

inline LIR* Mir2Lir::RawLIR(DexOffset dalvik_offset, int opcode, int op0,
                            int op1, int op2, int op3, int op4, LIR* target) {
  LIR* insn = static_cast<LIR*>(arena_->Alloc(sizeof(LIR), kArenaAllocLIR));
  insn->dalvik_offset = dalvik_offset;
  insn->opcode = opcode;
  insn->operands[0] = op0;
  insn->operands[1] = op1;
  insn->operands[2] = op2;
  insn->operands[3] = op3;
  insn->operands[4] = op4;
  insn->target = target;
  SetupResourceMasks(insn);
  if ((opcode == kPseudoTargetLabel) || (opcode == kPseudoSafepointPC) ||
      (opcode == kPseudoExportedPC)) {
    // Always make labels scheduling barriers
    DCHECK(!insn->flags.use_def_invalid);
    insn->u.m.use_mask = insn->u.m.def_mask = ENCODE_ALL;
  }
  return insn;
}

/*
 * The following are building blocks to construct low-level IRs with 0 - 4
 * operands.
 */
inline LIR* Mir2Lir::NewLIR0(int opcode) {
  DCHECK(IsPseudoLirOp(opcode) || (GetTargetInstFlags(opcode) & NO_OPERAND))
      << GetTargetInstName(opcode) << " " << opcode << " "
      << PrettyMethod(cu_->method_idx, *cu_->dex_file) << " "
      << current_dalvik_offset_;
  LIR* insn = RawLIR(current_dalvik_offset_, opcode);
  AppendLIR(insn);
  return insn;
}

inline LIR* Mir2Lir::NewLIR1(int opcode, int dest) {
  DCHECK(IsPseudoLirOp(opcode) || (GetTargetInstFlags(opcode) & IS_UNARY_OP))
      << GetTargetInstName(opcode) << " " << opcode << " "
      << PrettyMethod(cu_->method_idx, *cu_->dex_file) << " "
      << current_dalvik_offset_;
  LIR* insn = RawLIR(current_dalvik_offset_, opcode, dest);
  AppendLIR(insn);
  return insn;
}

inline LIR* Mir2Lir::NewLIR2(int opcode, int dest, int src1) {
  DCHECK(IsPseudoLirOp(opcode) || (GetTargetInstFlags(opcode) & IS_BINARY_OP))
      << GetTargetInstName(opcode) << " " << opcode << " "
      << PrettyMethod(cu_->method_idx, *cu_->dex_file) << " "
      << current_dalvik_offset_;
  LIR* insn = RawLIR(current_dalvik_offset_, opcode, dest, src1);
  AppendLIR(insn);
  return insn;
}

inline LIR* Mir2Lir::NewLIR2NoDest(int opcode, int src, int info) {
  DCHECK(IsPseudoLirOp(opcode) || (GetTargetInstFlags(opcode) & IS_UNARY_OP))
      << GetTargetInstName(opcode) << " " << opcode << " "
      << PrettyMethod(cu_->method_idx, *cu_->dex_file) << " "
      << current_dalvik_offset_;
  LIR* insn = RawLIR(current_dalvik_offset_, opcode, src, info);
  AppendLIR(insn);
  return insn;
}

inline LIR* Mir2Lir::NewLIR3(int opcode, int dest, int src1, int src2) {
  DCHECK(IsPseudoLirOp(opcode) || (GetTargetInstFlags(opcode) & IS_TERTIARY_OP))
      << GetTargetInstName(opcode) << " " << opcode << " "
      << PrettyMethod(cu_->method_idx, *cu_->dex_file) << " "
      << current_dalvik_offset_;
  LIR* insn = RawLIR(current_dalvik_offset_, opcode, dest, src1, src2);
  AppendLIR(insn);
  return insn;
}

inline LIR* Mir2Lir::NewLIR4(int opcode, int dest, int src1, int src2, int info) {
  DCHECK(IsPseudoLirOp(opcode) || (GetTargetInstFlags(opcode) & IS_QUAD_OP))
      << GetTargetInstName(opcode) << " " << opcode << " "
      << PrettyMethod(cu_->method_idx, *cu_->dex_file) << " "
      << current_dalvik_offset_;
  LIR* insn = RawLIR(current_dalvik_offset_, opcode, dest, src1, src2, info);
  AppendLIR(insn);
  return insn;
}

inline LIR* Mir2Lir::NewLIR5(int opcode, int dest, int src1, int src2, int info1,
                             int info2) {
  DCHECK(IsPseudoLirOp(opcode) || (GetTargetInstFlags(opcode) & IS_QUIN_OP))
      << GetTargetInstName(opcode) << " " << opcode << " "
      << PrettyMethod(cu_->method_idx, *cu_->dex_file) << " "
      << current_dalvik_offset_;
  LIR* insn = RawLIR(current_dalvik_offset_, opcode, dest, src1, src2, info1, info2);
  AppendLIR(insn);
  return insn;
}

/*
 * Mark the corresponding bit(s).
 */
inline void Mir2Lir::SetupRegMask(uint64_t* mask, int reg) {
  DCHECK_EQ((reg & ~RegStorage::kRegValMask), 0);
  DCHECK(reginfo_map_.Get(reg) != nullptr) << "No info for 0x" << reg;
  *mask |= reginfo_map_.Get(reg)->DefUseMask();
}

/*
 * Set up the proper fields in the resource mask
 */
inline void Mir2Lir::SetupResourceMasks(LIR* lir) {
  int opcode = lir->opcode;

  if (IsPseudoLirOp(opcode)) {
    if (opcode != kPseudoBarrier) {
      lir->flags.fixup = kFixupLabel;
    }
    return;
  }

  uint64_t flags = GetTargetInstFlags(opcode);

  if (flags & NEEDS_FIXUP) {
    // Note: target-specific setup may specialize the fixup kind.
    lir->flags.fixup = kFixupLabel;
  }

  /* Get the starting size of the instruction's template */
  lir->flags.size = GetInsnSize(lir);
  estimated_native_code_size_ += lir->flags.size;
  /* Set up the mask for resources that are updated */
  if (flags & (IS_LOAD | IS_STORE)) {
    /* Default to heap - will catch specialized classes later */
    SetMemRefType(lir, flags & IS_LOAD, kHeapRef);
  }

  /*
   * Conservatively assume the branch here will call out a function that in
   * turn will trash everything.
   */
  if (flags & IS_BRANCH) {
    lir->u.m.def_mask = lir->u.m.use_mask = ENCODE_ALL;
    return;
  }

  if (flags & REG_DEF0) {
    SetupRegMask(&lir->u.m.def_mask, lir->operands[0]);
  }

  if (flags & REG_DEF1) {
    SetupRegMask(&lir->u.m.def_mask, lir->operands[1]);
  }

  if (flags & REG_DEF2) {
    SetupRegMask(&lir->u.m.def_mask, lir->operands[2]);
  }

  if (flags & REG_USE0) {
    SetupRegMask(&lir->u.m.use_mask, lir->operands[0]);
  }

  if (flags & REG_USE1) {
    SetupRegMask(&lir->u.m.use_mask, lir->operands[1]);
  }

  if (flags & REG_USE2) {
    SetupRegMask(&lir->u.m.use_mask, lir->operands[2]);
  }

  if (flags & REG_USE3) {
    SetupRegMask(&lir->u.m.use_mask, lir->operands[3]);
  }

  if (flags & REG_USE4) {
    SetupRegMask(&lir->u.m.use_mask, lir->operands[4]);
  }

  if (flags & SETS_CCODES) {
    lir->u.m.def_mask |= ENCODE_CCODE;
  }

  if (flags & USES_CCODES) {
    lir->u.m.use_mask |= ENCODE_CCODE;
  }

  // Handle target-specific actions
  SetupTargetResourceMasks(lir, flags);
}

inline art::Mir2Lir::RegisterInfo* Mir2Lir::GetRegInfo(RegStorage reg) {
  RegisterInfo* res = reg.IsPair() ? reginfo_map_.Get(reg.GetLowReg()) :
      reginfo_map_.Get(reg.GetReg());
  DCHECK(res != nullptr);
  return res;
}

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_MIR_TO_LIR_INL_H_
