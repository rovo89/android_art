/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include <string>
#include <inttypes.h>

#include "codegen_x86.h"
#include "dex/compiler_internals.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "mirror/array.h"
#include "mirror/string.h"
#include "x86_lir.h"

namespace art {

static constexpr RegStorage core_regs_arr_32[] = {
    rs_rAX, rs_rCX, rs_rDX, rs_rBX, rs_rX86_SP_32, rs_rBP, rs_rSI, rs_rDI,
};
static constexpr RegStorage core_regs_arr_64[] = {
    rs_rAX, rs_rCX, rs_rDX, rs_rBX, rs_rX86_SP_32, rs_rBP, rs_rSI, rs_rDI,
    rs_r8, rs_r9, rs_r10, rs_r11, rs_r12, rs_r13, rs_r14, rs_r15
};
static constexpr RegStorage core_regs_arr_64q[] = {
    rs_r0q, rs_r1q, rs_r2q, rs_r3q, rs_rX86_SP_64, rs_r5q, rs_r6q, rs_r7q,
    rs_r8q, rs_r9q, rs_r10q, rs_r11q, rs_r12q, rs_r13q, rs_r14q, rs_r15q
};
static constexpr RegStorage sp_regs_arr_32[] = {
    rs_fr0, rs_fr1, rs_fr2, rs_fr3, rs_fr4, rs_fr5, rs_fr6, rs_fr7,
};
static constexpr RegStorage sp_regs_arr_64[] = {
    rs_fr0, rs_fr1, rs_fr2, rs_fr3, rs_fr4, rs_fr5, rs_fr6, rs_fr7,
    rs_fr8, rs_fr9, rs_fr10, rs_fr11, rs_fr12, rs_fr13, rs_fr14, rs_fr15
};
static constexpr RegStorage dp_regs_arr_32[] = {
    rs_dr0, rs_dr1, rs_dr2, rs_dr3, rs_dr4, rs_dr5, rs_dr6, rs_dr7,
};
static constexpr RegStorage dp_regs_arr_64[] = {
    rs_dr0, rs_dr1, rs_dr2, rs_dr3, rs_dr4, rs_dr5, rs_dr6, rs_dr7,
    rs_dr8, rs_dr9, rs_dr10, rs_dr11, rs_dr12, rs_dr13, rs_dr14, rs_dr15
};
static constexpr RegStorage reserved_regs_arr_32[] = {rs_rX86_SP_32};
static constexpr RegStorage reserved_regs_arr_64[] = {rs_rX86_SP_32};
static constexpr RegStorage reserved_regs_arr_64q[] = {rs_rX86_SP_64};
static constexpr RegStorage core_temps_arr_32[] = {rs_rAX, rs_rCX, rs_rDX, rs_rBX};
static constexpr RegStorage core_temps_arr_64[] = {
    rs_rAX, rs_rCX, rs_rDX, rs_rSI, rs_rDI,
    rs_r8, rs_r9, rs_r10, rs_r11
};
static constexpr RegStorage core_temps_arr_64q[] = {
    rs_r0q, rs_r1q, rs_r2q, rs_r6q, rs_r7q,
    rs_r8q, rs_r9q, rs_r10q, rs_r11q
};
static constexpr RegStorage sp_temps_arr_32[] = {
    rs_fr0, rs_fr1, rs_fr2, rs_fr3, rs_fr4, rs_fr5, rs_fr6, rs_fr7,
};
static constexpr RegStorage sp_temps_arr_64[] = {
    rs_fr0, rs_fr1, rs_fr2, rs_fr3, rs_fr4, rs_fr5, rs_fr6, rs_fr7,
    rs_fr8, rs_fr9, rs_fr10, rs_fr11, rs_fr12, rs_fr13, rs_fr14, rs_fr15
};
static constexpr RegStorage dp_temps_arr_32[] = {
    rs_dr0, rs_dr1, rs_dr2, rs_dr3, rs_dr4, rs_dr5, rs_dr6, rs_dr7,
};
static constexpr RegStorage dp_temps_arr_64[] = {
    rs_dr0, rs_dr1, rs_dr2, rs_dr3, rs_dr4, rs_dr5, rs_dr6, rs_dr7,
    rs_dr8, rs_dr9, rs_dr10, rs_dr11, rs_dr12, rs_dr13, rs_dr14, rs_dr15
};

static constexpr RegStorage xp_temps_arr_32[] = {
    rs_xr0, rs_xr1, rs_xr2, rs_xr3, rs_xr4, rs_xr5, rs_xr6, rs_xr7,
};
static constexpr RegStorage xp_temps_arr_64[] = {
    rs_xr0, rs_xr1, rs_xr2, rs_xr3, rs_xr4, rs_xr5, rs_xr6, rs_xr7,
    rs_xr8, rs_xr9, rs_xr10, rs_xr11, rs_xr12, rs_xr13, rs_xr14, rs_xr15
};

static constexpr ArrayRef<const RegStorage> empty_pool;
static constexpr ArrayRef<const RegStorage> core_regs_32(core_regs_arr_32);
static constexpr ArrayRef<const RegStorage> core_regs_64(core_regs_arr_64);
static constexpr ArrayRef<const RegStorage> core_regs_64q(core_regs_arr_64q);
static constexpr ArrayRef<const RegStorage> sp_regs_32(sp_regs_arr_32);
static constexpr ArrayRef<const RegStorage> sp_regs_64(sp_regs_arr_64);
static constexpr ArrayRef<const RegStorage> dp_regs_32(dp_regs_arr_32);
static constexpr ArrayRef<const RegStorage> dp_regs_64(dp_regs_arr_64);
static constexpr ArrayRef<const RegStorage> reserved_regs_32(reserved_regs_arr_32);
static constexpr ArrayRef<const RegStorage> reserved_regs_64(reserved_regs_arr_64);
static constexpr ArrayRef<const RegStorage> reserved_regs_64q(reserved_regs_arr_64q);
static constexpr ArrayRef<const RegStorage> core_temps_32(core_temps_arr_32);
static constexpr ArrayRef<const RegStorage> core_temps_64(core_temps_arr_64);
static constexpr ArrayRef<const RegStorage> core_temps_64q(core_temps_arr_64q);
static constexpr ArrayRef<const RegStorage> sp_temps_32(sp_temps_arr_32);
static constexpr ArrayRef<const RegStorage> sp_temps_64(sp_temps_arr_64);
static constexpr ArrayRef<const RegStorage> dp_temps_32(dp_temps_arr_32);
static constexpr ArrayRef<const RegStorage> dp_temps_64(dp_temps_arr_64);

static constexpr ArrayRef<const RegStorage> xp_temps_32(xp_temps_arr_32);
static constexpr ArrayRef<const RegStorage> xp_temps_64(xp_temps_arr_64);

RegStorage rs_rX86_SP;

X86NativeRegisterPool rX86_ARG0;
X86NativeRegisterPool rX86_ARG1;
X86NativeRegisterPool rX86_ARG2;
X86NativeRegisterPool rX86_ARG3;
X86NativeRegisterPool rX86_ARG4;
X86NativeRegisterPool rX86_ARG5;
X86NativeRegisterPool rX86_FARG0;
X86NativeRegisterPool rX86_FARG1;
X86NativeRegisterPool rX86_FARG2;
X86NativeRegisterPool rX86_FARG3;
X86NativeRegisterPool rX86_FARG4;
X86NativeRegisterPool rX86_FARG5;
X86NativeRegisterPool rX86_FARG6;
X86NativeRegisterPool rX86_FARG7;
X86NativeRegisterPool rX86_RET0;
X86NativeRegisterPool rX86_RET1;
X86NativeRegisterPool rX86_INVOKE_TGT;
X86NativeRegisterPool rX86_COUNT;

RegStorage rs_rX86_ARG0;
RegStorage rs_rX86_ARG1;
RegStorage rs_rX86_ARG2;
RegStorage rs_rX86_ARG3;
RegStorage rs_rX86_ARG4;
RegStorage rs_rX86_ARG5;
RegStorage rs_rX86_FARG0;
RegStorage rs_rX86_FARG1;
RegStorage rs_rX86_FARG2;
RegStorage rs_rX86_FARG3;
RegStorage rs_rX86_FARG4;
RegStorage rs_rX86_FARG5;
RegStorage rs_rX86_FARG6;
RegStorage rs_rX86_FARG7;
RegStorage rs_rX86_RET0;
RegStorage rs_rX86_RET1;
RegStorage rs_rX86_INVOKE_TGT;
RegStorage rs_rX86_COUNT;

RegLocation X86Mir2Lir::LocCReturn() {
  return x86_loc_c_return;
}

RegLocation X86Mir2Lir::LocCReturnRef() {
  // FIXME: return x86_loc_c_return_wide for x86_64 when wide refs supported.
  return x86_loc_c_return;
}

RegLocation X86Mir2Lir::LocCReturnWide() {
  return Gen64Bit() ? x86_64_loc_c_return_wide : x86_loc_c_return_wide;
}

RegLocation X86Mir2Lir::LocCReturnFloat() {
  return x86_loc_c_return_float;
}

RegLocation X86Mir2Lir::LocCReturnDouble() {
  return x86_loc_c_return_double;
}

// Return a target-dependent special register.
RegStorage X86Mir2Lir::TargetReg(SpecialTargetRegister reg) {
  RegStorage res_reg = RegStorage::InvalidReg();
  switch (reg) {
    case kSelf: res_reg = RegStorage::InvalidReg(); break;
    case kSuspend: res_reg =  RegStorage::InvalidReg(); break;
    case kLr: res_reg =  RegStorage::InvalidReg(); break;
    case kPc: res_reg =  RegStorage::InvalidReg(); break;
    case kSp: res_reg =  rs_rX86_SP; break;
    case kArg0: res_reg = rs_rX86_ARG0; break;
    case kArg1: res_reg = rs_rX86_ARG1; break;
    case kArg2: res_reg = rs_rX86_ARG2; break;
    case kArg3: res_reg = rs_rX86_ARG3; break;
    case kArg4: res_reg = rs_rX86_ARG4; break;
    case kArg5: res_reg = rs_rX86_ARG5; break;
    case kFArg0: res_reg = rs_rX86_FARG0; break;
    case kFArg1: res_reg = rs_rX86_FARG1; break;
    case kFArg2: res_reg = rs_rX86_FARG2; break;
    case kFArg3: res_reg = rs_rX86_FARG3; break;
    case kFArg4: res_reg = rs_rX86_FARG4; break;
    case kFArg5: res_reg = rs_rX86_FARG5; break;
    case kFArg6: res_reg = rs_rX86_FARG6; break;
    case kFArg7: res_reg = rs_rX86_FARG7; break;
    case kRet0: res_reg = rs_rX86_RET0; break;
    case kRet1: res_reg = rs_rX86_RET1; break;
    case kInvokeTgt: res_reg = rs_rX86_INVOKE_TGT; break;
    case kHiddenArg: res_reg = rs_rAX; break;
    case kHiddenFpArg: DCHECK(!Gen64Bit()); res_reg = rs_fr0; break;
    case kCount: res_reg = rs_rX86_COUNT; break;
    default: res_reg = RegStorage::InvalidReg();
  }
  return res_reg;
}

/*
 * Decode the register id.
 */
ResourceMask X86Mir2Lir::GetRegMaskCommon(const RegStorage& reg) const {
  /* Double registers in x86 are just a single FP register. This is always just a single bit. */
  return ResourceMask::Bit(
      /* FP register starts at bit position 16 */
      ((reg.IsFloat() || reg.StorageSize() > 8) ? kX86FPReg0 : 0) + reg.GetRegNum());
}

ResourceMask X86Mir2Lir::GetPCUseDefEncoding() const {
  /*
   * FIXME: might make sense to use a virtual resource encoding bit for pc.  Might be
   * able to clean up some of the x86/Arm_Mips differences
   */
  LOG(FATAL) << "Unexpected call to GetPCUseDefEncoding for x86";
  return kEncodeNone;
}

void X86Mir2Lir::SetupTargetResourceMasks(LIR* lir, uint64_t flags,
                                          ResourceMask* use_mask, ResourceMask* def_mask) {
  DCHECK(cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64);
  DCHECK(!lir->flags.use_def_invalid);

  // X86-specific resource map setup here.
  if (flags & REG_USE_SP) {
    use_mask->SetBit(kX86RegSP);
  }

  if (flags & REG_DEF_SP) {
    def_mask->SetBit(kX86RegSP);
  }

  if (flags & REG_DEFA) {
    SetupRegMask(def_mask, rs_rAX.GetReg());
  }

  if (flags & REG_DEFD) {
    SetupRegMask(def_mask, rs_rDX.GetReg());
  }
  if (flags & REG_USEA) {
    SetupRegMask(use_mask, rs_rAX.GetReg());
  }

  if (flags & REG_USEC) {
    SetupRegMask(use_mask, rs_rCX.GetReg());
  }

  if (flags & REG_USED) {
    SetupRegMask(use_mask, rs_rDX.GetReg());
  }

  if (flags & REG_USEB) {
    SetupRegMask(use_mask, rs_rBX.GetReg());
  }

  // Fixup hard to describe instruction: Uses rAX, rCX, rDI; sets rDI.
  if (lir->opcode == kX86RepneScasw) {
    SetupRegMask(use_mask, rs_rAX.GetReg());
    SetupRegMask(use_mask, rs_rCX.GetReg());
    SetupRegMask(use_mask, rs_rDI.GetReg());
    SetupRegMask(def_mask, rs_rDI.GetReg());
  }

  if (flags & USE_FP_STACK) {
    use_mask->SetBit(kX86FPStack);
    def_mask->SetBit(kX86FPStack);
  }
}

/* For dumping instructions */
static const char* x86RegName[] = {
  "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
  "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
};

static const char* x86CondName[] = {
  "O",
  "NO",
  "B/NAE/C",
  "NB/AE/NC",
  "Z/EQ",
  "NZ/NE",
  "BE/NA",
  "NBE/A",
  "S",
  "NS",
  "P/PE",
  "NP/PO",
  "L/NGE",
  "NL/GE",
  "LE/NG",
  "NLE/G"
};

/*
 * Interpret a format string and build a string no longer than size
 * See format key in Assemble.cc.
 */
std::string X86Mir2Lir::BuildInsnString(const char *fmt, LIR *lir, unsigned char* base_addr) {
  std::string buf;
  size_t i = 0;
  size_t fmt_len = strlen(fmt);
  while (i < fmt_len) {
    if (fmt[i] != '!') {
      buf += fmt[i];
      i++;
    } else {
      i++;
      DCHECK_LT(i, fmt_len);
      char operand_number_ch = fmt[i];
      i++;
      if (operand_number_ch == '!') {
        buf += "!";
      } else {
        int operand_number = operand_number_ch - '0';
        DCHECK_LT(operand_number, 6);  // Expect upto 6 LIR operands.
        DCHECK_LT(i, fmt_len);
        int operand = lir->operands[operand_number];
        switch (fmt[i]) {
          case 'c':
            DCHECK_LT(static_cast<size_t>(operand), sizeof(x86CondName));
            buf += x86CondName[operand];
            break;
          case 'd':
            buf += StringPrintf("%d", operand);
            break;
          case 'p': {
            EmbeddedData *tab_rec = reinterpret_cast<EmbeddedData*>(UnwrapPointer(operand));
            buf += StringPrintf("0x%08x", tab_rec->offset);
            break;
          }
          case 'r':
            if (RegStorage::IsFloat(operand)) {
              int fp_reg = RegStorage::RegNum(operand);
              buf += StringPrintf("xmm%d", fp_reg);
            } else {
              int reg_num = RegStorage::RegNum(operand);
              DCHECK_LT(static_cast<size_t>(reg_num), sizeof(x86RegName));
              buf += x86RegName[reg_num];
            }
            break;
          case 't':
            buf += StringPrintf("0x%08" PRIxPTR " (L%p)",
                                reinterpret_cast<uintptr_t>(base_addr) + lir->offset + operand,
                                lir->target);
            break;
          default:
            buf += StringPrintf("DecodeError '%c'", fmt[i]);
            break;
        }
        i++;
      }
    }
  }
  return buf;
}

void X86Mir2Lir::DumpResourceMask(LIR *x86LIR, const ResourceMask& mask, const char *prefix) {
  char buf[256];
  buf[0] = 0;

  if (mask.Equals(kEncodeAll)) {
    strcpy(buf, "all");
  } else {
    char num[8];
    int i;

    for (i = 0; i < kX86RegEnd; i++) {
      if (mask.HasBit(i)) {
        snprintf(num, arraysize(num), "%d ", i);
        strcat(buf, num);
      }
    }

    if (mask.HasBit(ResourceMask::kCCode)) {
      strcat(buf, "cc ");
    }
    /* Memory bits */
    if (x86LIR && (mask.HasBit(ResourceMask::kDalvikReg))) {
      snprintf(buf + strlen(buf), arraysize(buf) - strlen(buf), "dr%d%s",
               DECODE_ALIAS_INFO_REG(x86LIR->flags.alias_info),
               (DECODE_ALIAS_INFO_WIDE(x86LIR->flags.alias_info)) ? "(+1)" : "");
    }
    if (mask.HasBit(ResourceMask::kLiteral)) {
      strcat(buf, "lit ");
    }

    if (mask.HasBit(ResourceMask::kHeapRef)) {
      strcat(buf, "heap ");
    }
    if (mask.HasBit(ResourceMask::kMustNotAlias)) {
      strcat(buf, "noalias ");
    }
  }
  if (buf[0]) {
    LOG(INFO) << prefix << ": " <<  buf;
  }
}

void X86Mir2Lir::AdjustSpillMask() {
  // Adjustment for LR spilling, x86 has no LR so nothing to do here
  core_spill_mask_ |= (1 << rs_rRET.GetRegNum());
  num_core_spills_++;
}

/*
 * Mark a callee-save fp register as promoted.  Note that
 * vpush/vpop uses contiguous register lists so we must
 * include any holes in the mask.  Associate holes with
 * Dalvik register INVALID_VREG (0xFFFFU).
 */
void X86Mir2Lir::MarkPreservedSingle(int v_reg, RegStorage reg) {
  UNIMPLEMENTED(FATAL) << "MarkPreservedSingle";
}

void X86Mir2Lir::MarkPreservedDouble(int v_reg, RegStorage reg) {
  UNIMPLEMENTED(FATAL) << "MarkPreservedDouble";
}

RegStorage X86Mir2Lir::AllocateByteRegister() {
  RegStorage reg = AllocTypedTemp(false, kCoreReg);
  if (!Gen64Bit()) {
    DCHECK_LT(reg.GetRegNum(), rs_rX86_SP.GetRegNum());
  }
  return reg;
}

bool X86Mir2Lir::IsByteRegister(RegStorage reg) {
  return Gen64Bit() || reg.GetRegNum() < rs_rX86_SP.GetRegNum();
}

/* Clobber all regs that might be used by an external C call */
void X86Mir2Lir::ClobberCallerSave() {
  Clobber(rs_rAX);
  Clobber(rs_rCX);
  Clobber(rs_rDX);
  Clobber(rs_rBX);
}

RegLocation X86Mir2Lir::GetReturnWideAlt() {
  RegLocation res = LocCReturnWide();
  DCHECK(res.reg.GetLowReg() == rs_rAX.GetReg());
  DCHECK(res.reg.GetHighReg() == rs_rDX.GetReg());
  Clobber(rs_rAX);
  Clobber(rs_rDX);
  MarkInUse(rs_rAX);
  MarkInUse(rs_rDX);
  MarkWide(res.reg);
  return res;
}

RegLocation X86Mir2Lir::GetReturnAlt() {
  RegLocation res = LocCReturn();
  res.reg.SetReg(rs_rDX.GetReg());
  Clobber(rs_rDX);
  MarkInUse(rs_rDX);
  return res;
}

/* To be used when explicitly managing register use */
void X86Mir2Lir::LockCallTemps() {
  LockTemp(rs_rX86_ARG0);
  LockTemp(rs_rX86_ARG1);
  LockTemp(rs_rX86_ARG2);
  LockTemp(rs_rX86_ARG3);
  if (Gen64Bit()) {
    LockTemp(rs_rX86_ARG4);
    LockTemp(rs_rX86_ARG5);
    LockTemp(rs_rX86_FARG0);
    LockTemp(rs_rX86_FARG1);
    LockTemp(rs_rX86_FARG2);
    LockTemp(rs_rX86_FARG3);
    LockTemp(rs_rX86_FARG4);
    LockTemp(rs_rX86_FARG5);
    LockTemp(rs_rX86_FARG6);
    LockTemp(rs_rX86_FARG7);
  }
}

/* To be used when explicitly managing register use */
void X86Mir2Lir::FreeCallTemps() {
  FreeTemp(rs_rX86_ARG0);
  FreeTemp(rs_rX86_ARG1);
  FreeTemp(rs_rX86_ARG2);
  FreeTemp(rs_rX86_ARG3);
  if (Gen64Bit()) {
    FreeTemp(rs_rX86_ARG4);
    FreeTemp(rs_rX86_ARG5);
    FreeTemp(rs_rX86_FARG0);
    FreeTemp(rs_rX86_FARG1);
    FreeTemp(rs_rX86_FARG2);
    FreeTemp(rs_rX86_FARG3);
    FreeTemp(rs_rX86_FARG4);
    FreeTemp(rs_rX86_FARG5);
    FreeTemp(rs_rX86_FARG6);
    FreeTemp(rs_rX86_FARG7);
  }
}

bool X86Mir2Lir::ProvidesFullMemoryBarrier(X86OpCode opcode) {
    switch (opcode) {
      case kX86LockCmpxchgMR:
      case kX86LockCmpxchgAR:
      case kX86LockCmpxchg64M:
      case kX86LockCmpxchg64A:
      case kX86XchgMR:
      case kX86Mfence:
        // Atomic memory instructions provide full barrier.
        return true;
      default:
        break;
    }

    // Conservative if cannot prove it provides full barrier.
    return false;
}

bool X86Mir2Lir::GenMemBarrier(MemBarrierKind barrier_kind) {
#if ANDROID_SMP != 0
  // Start off with using the last LIR as the barrier. If it is not enough, then we will update it.
  LIR* mem_barrier = last_lir_insn_;

  bool ret = false;
  /*
   * According to the JSR-133 Cookbook, for x86 only StoreLoad barriers need memory fence. All other barriers
   * (LoadLoad, LoadStore, StoreStore) are nops due to the x86 memory model. For those cases, all we need
   * to ensure is that there is a scheduling barrier in place.
   */
  if (barrier_kind == kStoreLoad) {
    // If no LIR exists already that can be used a barrier, then generate an mfence.
    if (mem_barrier == nullptr) {
      mem_barrier = NewLIR0(kX86Mfence);
      ret = true;
    }

    // If last instruction does not provide full barrier, then insert an mfence.
    if (ProvidesFullMemoryBarrier(static_cast<X86OpCode>(mem_barrier->opcode)) == false) {
      mem_barrier = NewLIR0(kX86Mfence);
      ret = true;
    }
  }

  // Now ensure that a scheduling barrier is in place.
  if (mem_barrier == nullptr) {
    GenBarrier();
  } else {
    // Mark as a scheduling barrier.
    DCHECK(!mem_barrier->flags.use_def_invalid);
    mem_barrier->u.m.def_mask = &kEncodeAll;
  }
  return ret;
#else
  return false;
#endif
}

void X86Mir2Lir::CompilerInitializeRegAlloc() {
  if (Gen64Bit()) {
    reg_pool_ = new (arena_) RegisterPool(this, arena_, core_regs_64, core_regs_64q, sp_regs_64,
                                          dp_regs_64, reserved_regs_64, reserved_regs_64q,
                                          core_temps_64, core_temps_64q, sp_temps_64, dp_temps_64);
  } else {
    reg_pool_ = new (arena_) RegisterPool(this, arena_, core_regs_32, empty_pool, sp_regs_32,
                                          dp_regs_32, reserved_regs_32, empty_pool,
                                          core_temps_32, empty_pool, sp_temps_32, dp_temps_32);
  }

  // Target-specific adjustments.

  // Add in XMM registers.
  const ArrayRef<const RegStorage> *xp_temps = Gen64Bit() ? &xp_temps_64 : &xp_temps_32;
  for (RegStorage reg : *xp_temps) {
    RegisterInfo* info = new (arena_) RegisterInfo(reg, GetRegMaskCommon(reg));
    reginfo_map_.Put(reg.GetReg(), info);
    info->SetIsTemp(true);
  }

  // Alias single precision xmm to double xmms.
  // TODO: as needed, add larger vector sizes - alias all to the largest.
  GrowableArray<RegisterInfo*>::Iterator it(&reg_pool_->sp_regs_);
  for (RegisterInfo* info = it.Next(); info != nullptr; info = it.Next()) {
    int sp_reg_num = info->GetReg().GetRegNum();
    RegStorage xp_reg = RegStorage::Solo128(sp_reg_num);
    RegisterInfo* xp_reg_info = GetRegInfo(xp_reg);
    // 128-bit xmm vector register's master storage should refer to itself.
    DCHECK_EQ(xp_reg_info, xp_reg_info->Master());

    // Redirect 32-bit vector's master storage to 128-bit vector.
    info->SetMaster(xp_reg_info);

    RegStorage dp_reg = RegStorage::FloatSolo64(sp_reg_num);
    RegisterInfo* dp_reg_info = GetRegInfo(dp_reg);
    // Redirect 64-bit vector's master storage to 128-bit vector.
    dp_reg_info->SetMaster(xp_reg_info);
    // Singles should show a single 32-bit mask bit, at first referring to the low half.
    DCHECK_EQ(info->StorageMask(), 0x1U);
  }

  if (Gen64Bit()) {
    // Alias 32bit W registers to corresponding 64bit X registers.
    GrowableArray<RegisterInfo*>::Iterator w_it(&reg_pool_->core_regs_);
    for (RegisterInfo* info = w_it.Next(); info != nullptr; info = w_it.Next()) {
      int x_reg_num = info->GetReg().GetRegNum();
      RegStorage x_reg = RegStorage::Solo64(x_reg_num);
      RegisterInfo* x_reg_info = GetRegInfo(x_reg);
      // 64bit X register's master storage should refer to itself.
      DCHECK_EQ(x_reg_info, x_reg_info->Master());
      // Redirect 32bit W master storage to 64bit X.
      info->SetMaster(x_reg_info);
      // 32bit W should show a single 32-bit mask bit, at first referring to the low half.
      DCHECK_EQ(info->StorageMask(), 0x1U);
    }
  }

  // Don't start allocating temps at r0/s0/d0 or you may clobber return regs in early-exit methods.
  // TODO: adjust for x86/hard float calling convention.
  reg_pool_->next_core_reg_ = 2;
  reg_pool_->next_sp_reg_ = 2;
  reg_pool_->next_dp_reg_ = 1;
}

void X86Mir2Lir::SpillCoreRegs() {
  if (num_core_spills_ == 0) {
    return;
  }
  // Spill mask not including fake return address register
  uint32_t mask = core_spill_mask_ & ~(1 << rs_rRET.GetRegNum());
  int offset = frame_size_ - (GetInstructionSetPointerSize(cu_->instruction_set) * num_core_spills_);
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      StoreWordDisp(rs_rX86_SP, offset, RegStorage::Solo32(reg));
      offset += GetInstructionSetPointerSize(cu_->instruction_set);
    }
  }
}

void X86Mir2Lir::UnSpillCoreRegs() {
  if (num_core_spills_ == 0) {
    return;
  }
  // Spill mask not including fake return address register
  uint32_t mask = core_spill_mask_ & ~(1 << rs_rRET.GetRegNum());
  int offset = frame_size_ - (GetInstructionSetPointerSize(cu_->instruction_set) * num_core_spills_);
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      LoadWordDisp(rs_rX86_SP, offset, RegStorage::Solo32(reg));
      offset += GetInstructionSetPointerSize(cu_->instruction_set);
    }
  }
}

bool X86Mir2Lir::IsUnconditionalBranch(LIR* lir) {
  return (lir->opcode == kX86Jmp8 || lir->opcode == kX86Jmp32);
}

bool X86Mir2Lir::SupportsVolatileLoadStore(OpSize size) {
  return true;
}

RegisterClass X86Mir2Lir::RegClassForFieldLoadStore(OpSize size, bool is_volatile) {
  // X86_64 can handle any size.
  if (Gen64Bit()) {
    if (size == kReference) {
      return kRefReg;
    }
    return kCoreReg;
  }

  if (UNLIKELY(is_volatile)) {
    // On x86, atomic 64-bit load/store requires an fp register.
    // Smaller aligned load/store is atomic for both core and fp registers.
    if (size == k64 || size == kDouble) {
      return kFPReg;
    }
  }
  return RegClassBySize(size);
}

X86Mir2Lir::X86Mir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena, bool gen64bit)
    : Mir2Lir(cu, mir_graph, arena),
      base_of_code_(nullptr), store_method_addr_(false), store_method_addr_used_(false),
      method_address_insns_(arena, 100, kGrowableArrayMisc),
      class_type_address_insns_(arena, 100, kGrowableArrayMisc),
      call_method_insns_(arena, 100, kGrowableArrayMisc),
      stack_decrement_(nullptr), stack_increment_(nullptr), gen64bit_(gen64bit),
      const_vectors_(nullptr) {
  store_method_addr_used_ = false;
  if (kIsDebugBuild) {
    for (int i = 0; i < kX86Last; i++) {
      if (X86Mir2Lir::EncodingMap[i].opcode != i) {
        LOG(FATAL) << "Encoding order for " << X86Mir2Lir::EncodingMap[i].name
                   << " is wrong: expecting " << i << ", seeing "
                   << static_cast<int>(X86Mir2Lir::EncodingMap[i].opcode);
      }
    }
  }
  if (Gen64Bit()) {
    rs_rX86_SP = rs_rX86_SP_64;

    rs_rX86_ARG0 = rs_rDI;
    rs_rX86_ARG1 = rs_rSI;
    rs_rX86_ARG2 = rs_rDX;
    rs_rX86_ARG3 = rs_rCX;
    rs_rX86_ARG4 = rs_r8;
    rs_rX86_ARG5 = rs_r9;
    rs_rX86_FARG0 = rs_fr0;
    rs_rX86_FARG1 = rs_fr1;
    rs_rX86_FARG2 = rs_fr2;
    rs_rX86_FARG3 = rs_fr3;
    rs_rX86_FARG4 = rs_fr4;
    rs_rX86_FARG5 = rs_fr5;
    rs_rX86_FARG6 = rs_fr6;
    rs_rX86_FARG7 = rs_fr7;
    rX86_ARG0 = rDI;
    rX86_ARG1 = rSI;
    rX86_ARG2 = rDX;
    rX86_ARG3 = rCX;
    rX86_ARG4 = r8;
    rX86_ARG5 = r9;
    rX86_FARG0 = fr0;
    rX86_FARG1 = fr1;
    rX86_FARG2 = fr2;
    rX86_FARG3 = fr3;
    rX86_FARG4 = fr4;
    rX86_FARG5 = fr5;
    rX86_FARG6 = fr6;
    rX86_FARG7 = fr7;
    rs_rX86_INVOKE_TGT = rs_rDI;
  } else {
    rs_rX86_SP = rs_rX86_SP_32;

    rs_rX86_ARG0 = rs_rAX;
    rs_rX86_ARG1 = rs_rCX;
    rs_rX86_ARG2 = rs_rDX;
    rs_rX86_ARG3 = rs_rBX;
    rs_rX86_ARG4 = RegStorage::InvalidReg();
    rs_rX86_ARG5 = RegStorage::InvalidReg();
    rs_rX86_FARG0 = rs_rAX;
    rs_rX86_FARG1 = rs_rCX;
    rs_rX86_FARG2 = rs_rDX;
    rs_rX86_FARG3 = rs_rBX;
    rs_rX86_FARG4 = RegStorage::InvalidReg();
    rs_rX86_FARG5 = RegStorage::InvalidReg();
    rs_rX86_FARG6 = RegStorage::InvalidReg();
    rs_rX86_FARG7 = RegStorage::InvalidReg();
    rX86_ARG0 = rAX;
    rX86_ARG1 = rCX;
    rX86_ARG2 = rDX;
    rX86_ARG3 = rBX;
    rX86_FARG0 = rAX;
    rX86_FARG1 = rCX;
    rX86_FARG2 = rDX;
    rX86_FARG3 = rBX;
    rs_rX86_INVOKE_TGT = rs_rAX;
    // TODO(64): Initialize with invalid reg
//    rX86_ARG4 = RegStorage::InvalidReg();
//    rX86_ARG5 = RegStorage::InvalidReg();
  }
  rs_rX86_RET0 = rs_rAX;
  rs_rX86_RET1 = rs_rDX;
  rs_rX86_COUNT = rs_rCX;
  rX86_RET0 = rAX;
  rX86_RET1 = rDX;
  rX86_INVOKE_TGT = rAX;
  rX86_COUNT = rCX;
}

Mir2Lir* X86CodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                          ArenaAllocator* const arena) {
  return new X86Mir2Lir(cu, mir_graph, arena, false);
}

Mir2Lir* X86_64CodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                          ArenaAllocator* const arena) {
  return new X86Mir2Lir(cu, mir_graph, arena, true);
}

// Not used in x86
RegStorage X86Mir2Lir::LoadHelper(ThreadOffset<4> offset) {
  LOG(FATAL) << "Unexpected use of LoadHelper in x86";
  return RegStorage::InvalidReg();
}

// Not used in x86
RegStorage X86Mir2Lir::LoadHelper(ThreadOffset<8> offset) {
  LOG(FATAL) << "Unexpected use of LoadHelper in x86";
  return RegStorage::InvalidReg();
}

LIR* X86Mir2Lir::CheckSuspendUsingLoad() {
  LOG(FATAL) << "Unexpected use of CheckSuspendUsingLoad in x86";
  return nullptr;
}

uint64_t X86Mir2Lir::GetTargetInstFlags(int opcode) {
  DCHECK(!IsPseudoLirOp(opcode));
  return X86Mir2Lir::EncodingMap[opcode].flags;
}

const char* X86Mir2Lir::GetTargetInstName(int opcode) {
  DCHECK(!IsPseudoLirOp(opcode));
  return X86Mir2Lir::EncodingMap[opcode].name;
}

const char* X86Mir2Lir::GetTargetInstFmt(int opcode) {
  DCHECK(!IsPseudoLirOp(opcode));
  return X86Mir2Lir::EncodingMap[opcode].fmt;
}

void X86Mir2Lir::GenConstWide(RegLocation rl_dest, int64_t value) {
  // Can we do this directly to memory?
  rl_dest = UpdateLocWide(rl_dest);
  if ((rl_dest.location == kLocDalvikFrame) ||
      (rl_dest.location == kLocCompilerTemp)) {
    int32_t val_lo = Low32Bits(value);
    int32_t val_hi = High32Bits(value);
    int r_base = TargetReg(kSp).GetReg();
    int displacement = SRegOffset(rl_dest.s_reg_low);

    ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
    LIR * store = NewLIR3(kX86Mov32MI, r_base, displacement + LOWORD_OFFSET, val_lo);
    AnnotateDalvikRegAccess(store, (displacement + LOWORD_OFFSET) >> 2,
                              false /* is_load */, true /* is64bit */);
    store = NewLIR3(kX86Mov32MI, r_base, displacement + HIWORD_OFFSET, val_hi);
    AnnotateDalvikRegAccess(store, (displacement + HIWORD_OFFSET) >> 2,
                              false /* is_load */, true /* is64bit */);
    return;
  }

  // Just use the standard code to do the generation.
  Mir2Lir::GenConstWide(rl_dest, value);
}

// TODO: Merge with existing RegLocation dumper in vreg_analysis.cc
void X86Mir2Lir::DumpRegLocation(RegLocation loc) {
  LOG(INFO)  << "location: " << loc.location << ','
             << (loc.wide ? " w" : "  ")
             << (loc.defined ? " D" : "  ")
             << (loc.is_const ? " c" : "  ")
             << (loc.fp ? " F" : "  ")
             << (loc.core ? " C" : "  ")
             << (loc.ref ? " r" : "  ")
             << (loc.high_word ? " h" : "  ")
             << (loc.home ? " H" : "  ")
             << ", low: " << static_cast<int>(loc.reg.GetLowReg())
             << ", high: " << static_cast<int>(loc.reg.GetHighReg())
             << ", s_reg: " << loc.s_reg_low
             << ", orig: " << loc.orig_sreg;
}

void X86Mir2Lir::Materialize() {
  // A good place to put the analysis before starting.
  AnalyzeMIR();

  // Now continue with regular code generation.
  Mir2Lir::Materialize();
}

void X86Mir2Lir::LoadMethodAddress(const MethodReference& target_method, InvokeType type,
                                   SpecialTargetRegister symbolic_reg) {
  /*
   * For x86, just generate a 32 bit move immediate instruction, that will be filled
   * in at 'link time'.  For now, put a unique value based on target to ensure that
   * code deduplication works.
   */
  int target_method_idx = target_method.dex_method_index;
  const DexFile* target_dex_file = target_method.dex_file;
  const DexFile::MethodId& target_method_id = target_dex_file->GetMethodId(target_method_idx);
  uintptr_t target_method_id_ptr = reinterpret_cast<uintptr_t>(&target_method_id);

  // Generate the move instruction with the unique pointer and save index, dex_file, and type.
  LIR *move = RawLIR(current_dalvik_offset_, kX86Mov32RI, TargetReg(symbolic_reg).GetReg(),
                     static_cast<int>(target_method_id_ptr), target_method_idx,
                     WrapPointer(const_cast<DexFile*>(target_dex_file)), type);
  AppendLIR(move);
  method_address_insns_.Insert(move);
}

void X86Mir2Lir::LoadClassType(uint32_t type_idx, SpecialTargetRegister symbolic_reg) {
  /*
   * For x86, just generate a 32 bit move immediate instruction, that will be filled
   * in at 'link time'.  For now, put a unique value based on target to ensure that
   * code deduplication works.
   */
  const DexFile::TypeId& id = cu_->dex_file->GetTypeId(type_idx);
  uintptr_t ptr = reinterpret_cast<uintptr_t>(&id);

  // Generate the move instruction with the unique pointer and save index and type.
  LIR *move = RawLIR(current_dalvik_offset_, kX86Mov32RI, TargetReg(symbolic_reg).GetReg(),
                     static_cast<int>(ptr), type_idx);
  AppendLIR(move);
  class_type_address_insns_.Insert(move);
}

LIR *X86Mir2Lir::CallWithLinkerFixup(const MethodReference& target_method, InvokeType type) {
  /*
   * For x86, just generate a 32 bit call relative instruction, that will be filled
   * in at 'link time'.  For now, put a unique value based on target to ensure that
   * code deduplication works.
   */
  int target_method_idx = target_method.dex_method_index;
  const DexFile* target_dex_file = target_method.dex_file;
  const DexFile::MethodId& target_method_id = target_dex_file->GetMethodId(target_method_idx);
  uintptr_t target_method_id_ptr = reinterpret_cast<uintptr_t>(&target_method_id);

  // Generate the call instruction with the unique pointer and save index, dex_file, and type.
  LIR *call = RawLIR(current_dalvik_offset_, kX86CallI, static_cast<int>(target_method_id_ptr),
                     target_method_idx, WrapPointer(const_cast<DexFile*>(target_dex_file)), type);
  AppendLIR(call);
  call_method_insns_.Insert(call);
  return call;
}

/*
 * @brief Enter a 32 bit quantity into a buffer
 * @param buf buffer.
 * @param data Data value.
 */

static void PushWord(std::vector<uint8_t>&buf, int32_t data) {
  buf.push_back(data & 0xff);
  buf.push_back((data >> 8) & 0xff);
  buf.push_back((data >> 16) & 0xff);
  buf.push_back((data >> 24) & 0xff);
}

void X86Mir2Lir::InstallLiteralPools() {
  // These are handled differently for x86.
  DCHECK(code_literal_list_ == nullptr);
  DCHECK(method_literal_list_ == nullptr);
  DCHECK(class_literal_list_ == nullptr);

  // Align to 16 byte boundary.  We have implicit knowledge that the start of the method is
  // on a 4 byte boundary.   How can I check this if it changes (other than aligned loads
  // will fail at runtime)?
  if (const_vectors_ != nullptr) {
    int align_size = (16-4) - (code_buffer_.size() & 0xF);
    if (align_size < 0) {
      align_size += 16;
    }

    while (align_size > 0) {
      code_buffer_.push_back(0);
      align_size--;
    }
    for (LIR *p = const_vectors_; p != nullptr; p = p->next) {
      PushWord(code_buffer_, p->operands[0]);
      PushWord(code_buffer_, p->operands[1]);
      PushWord(code_buffer_, p->operands[2]);
      PushWord(code_buffer_, p->operands[3]);
    }
  }

  // Handle the fixups for methods.
  for (uint32_t i = 0; i < method_address_insns_.Size(); i++) {
      LIR* p = method_address_insns_.Get(i);
      DCHECK_EQ(p->opcode, kX86Mov32RI);
      uint32_t target_method_idx = p->operands[2];
      const DexFile* target_dex_file =
          reinterpret_cast<const DexFile*>(UnwrapPointer(p->operands[3]));

      // The offset to patch is the last 4 bytes of the instruction.
      int patch_offset = p->offset + p->flags.size - 4;
      cu_->compiler_driver->AddMethodPatch(cu_->dex_file, cu_->class_def_idx,
                                           cu_->method_idx, cu_->invoke_type,
                                           target_method_idx, target_dex_file,
                                           static_cast<InvokeType>(p->operands[4]),
                                           patch_offset);
  }

  // Handle the fixups for class types.
  for (uint32_t i = 0; i < class_type_address_insns_.Size(); i++) {
      LIR* p = class_type_address_insns_.Get(i);
      DCHECK_EQ(p->opcode, kX86Mov32RI);
      uint32_t target_method_idx = p->operands[2];

      // The offset to patch is the last 4 bytes of the instruction.
      int patch_offset = p->offset + p->flags.size - 4;
      cu_->compiler_driver->AddClassPatch(cu_->dex_file, cu_->class_def_idx,
                                          cu_->method_idx, target_method_idx, patch_offset);
  }

  // And now the PC-relative calls to methods.
  for (uint32_t i = 0; i < call_method_insns_.Size(); i++) {
      LIR* p = call_method_insns_.Get(i);
      DCHECK_EQ(p->opcode, kX86CallI);
      uint32_t target_method_idx = p->operands[1];
      const DexFile* target_dex_file =
          reinterpret_cast<const DexFile*>(UnwrapPointer(p->operands[2]));

      // The offset to patch is the last 4 bytes of the instruction.
      int patch_offset = p->offset + p->flags.size - 4;
      cu_->compiler_driver->AddRelativeCodePatch(cu_->dex_file, cu_->class_def_idx,
                                                 cu_->method_idx, cu_->invoke_type,
                                                 target_method_idx, target_dex_file,
                                                 static_cast<InvokeType>(p->operands[3]),
                                                 patch_offset, -4 /* offset */);
  }

  // And do the normal processing.
  Mir2Lir::InstallLiteralPools();
}

/*
 * Fast string.index_of(I) & (II).  Inline check for simple case of char <= 0xffff,
 * otherwise bails to standard library code.
 */
bool X86Mir2Lir::GenInlinedIndexOf(CallInfo* info, bool zero_based) {
  ClobberCallerSave();
  LockCallTemps();  // Using fixed registers

  // EAX: 16 bit character being searched.
  // ECX: count: number of words to be searched.
  // EDI: String being searched.
  // EDX: temporary during execution.
  // EBX: temporary during execution.

  RegLocation rl_obj = info->args[0];
  RegLocation rl_char = info->args[1];
  RegLocation rl_start;  // Note: only present in III flavor or IndexOf.

  uint32_t char_value =
    rl_char.is_const ? mir_graph_->ConstantValue(rl_char.orig_sreg) : 0;

  if (char_value > 0xFFFF) {
    // We have to punt to the real String.indexOf.
    return false;
  }

  // Okay, we are commited to inlining this.
  RegLocation rl_return = GetReturn(kCoreReg);
  RegLocation rl_dest = InlineTarget(info);

  // Is the string non-NULL?
  LoadValueDirectFixed(rl_obj, rs_rDX);
  GenNullCheck(rs_rDX, info->opt_flags);
  info->opt_flags |= MIR_IGNORE_NULL_CHECK;  // Record that we've null checked.

  // Does the character fit in 16 bits?
  LIR* slowpath_branch = nullptr;
  if (rl_char.is_const) {
    // We need the value in EAX.
    LoadConstantNoClobber(rs_rAX, char_value);
  } else {
    // Character is not a constant; compare at runtime.
    LoadValueDirectFixed(rl_char, rs_rAX);
    slowpath_branch = OpCmpImmBranch(kCondGt, rs_rAX, 0xFFFF, nullptr);
  }

  // From here down, we know that we are looking for a char that fits in 16 bits.
  // Location of reference to data array within the String object.
  int value_offset = mirror::String::ValueOffset().Int32Value();
  // Location of count within the String object.
  int count_offset = mirror::String::CountOffset().Int32Value();
  // Starting offset within data array.
  int offset_offset = mirror::String::OffsetOffset().Int32Value();
  // Start of char data with array_.
  int data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Int32Value();

  // Character is in EAX.
  // Object pointer is in EDX.

  // We need to preserve EDI, but have no spare registers, so push it on the stack.
  // We have to remember that all stack addresses after this are offset by sizeof(EDI).
  NewLIR1(kX86Push32R, rs_rDI.GetReg());

  // Compute the number of words to search in to rCX.
  Load32Disp(rs_rDX, count_offset, rs_rCX);
  LIR *length_compare = nullptr;
  int start_value = 0;
  bool is_index_on_stack = false;
  if (zero_based) {
    // We have to handle an empty string.  Use special instruction JECXZ.
    length_compare = NewLIR0(kX86Jecxz8);
  } else {
    rl_start = info->args[2];
    // We have to offset by the start index.
    if (rl_start.is_const) {
      start_value = mir_graph_->ConstantValue(rl_start.orig_sreg);
      start_value = std::max(start_value, 0);

      // Is the start > count?
      length_compare = OpCmpImmBranch(kCondLe, rs_rCX, start_value, nullptr);

      if (start_value != 0) {
        OpRegImm(kOpSub, rs_rCX, start_value);
      }
    } else {
      // Runtime start index.
      rl_start = UpdateLocTyped(rl_start, kCoreReg);
      if (rl_start.location == kLocPhysReg) {
        // Handle "start index < 0" case.
        OpRegReg(kOpXor, rs_rBX, rs_rBX);
        OpRegReg(kOpCmp, rl_start.reg, rs_rBX);
        OpCondRegReg(kOpCmov, kCondLt, rl_start.reg, rs_rBX);

        // The length of the string should be greater than the start index.
        length_compare = OpCmpBranch(kCondLe, rs_rCX, rl_start.reg, nullptr);
        OpRegReg(kOpSub, rs_rCX, rl_start.reg);
        if (rl_start.reg == rs_rDI) {
          // The special case. We will use EDI further, so lets put start index to stack.
          NewLIR1(kX86Push32R, rs_rDI.GetReg());
          is_index_on_stack = true;
        }
      } else {
        // Load the start index from stack, remembering that we pushed EDI.
        int displacement = SRegOffset(rl_start.s_reg_low) + sizeof(uint32_t);
        {
          ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
          Load32Disp(rs_rX86_SP, displacement, rs_rBX);
        }
        OpRegReg(kOpXor, rs_rDI, rs_rDI);
        OpRegReg(kOpCmp, rs_rBX, rs_rDI);
        OpCondRegReg(kOpCmov, kCondLt, rs_rBX, rs_rDI);

        length_compare = OpCmpBranch(kCondLe, rs_rCX, rs_rBX, nullptr);
        OpRegReg(kOpSub, rs_rCX, rs_rBX);
        // Put the start index to stack.
        NewLIR1(kX86Push32R, rs_rBX.GetReg());
        is_index_on_stack = true;
      }
    }
  }
  DCHECK(length_compare != nullptr);

  // ECX now contains the count in words to be searched.

  // Load the address of the string into EBX.
  // The string starts at VALUE(String) + 2 * OFFSET(String) + DATA_OFFSET.
  Load32Disp(rs_rDX, value_offset, rs_rDI);
  Load32Disp(rs_rDX, offset_offset, rs_rBX);
  OpLea(rs_rBX, rs_rDI, rs_rBX, 1, data_offset);

  // Now compute into EDI where the search will start.
  if (zero_based || rl_start.is_const) {
    if (start_value == 0) {
      OpRegCopy(rs_rDI, rs_rBX);
    } else {
      NewLIR3(kX86Lea32RM, rs_rDI.GetReg(), rs_rBX.GetReg(), 2 * start_value);
    }
  } else {
    if (is_index_on_stack == true) {
      // Load the start index from stack.
      NewLIR1(kX86Pop32R, rs_rDX.GetReg());
      OpLea(rs_rDI, rs_rBX, rs_rDX, 1, 0);
    } else {
      OpLea(rs_rDI, rs_rBX, rl_start.reg, 1, 0);
    }
  }

  // EDI now contains the start of the string to be searched.
  // We are all prepared to do the search for the character.
  NewLIR0(kX86RepneScasw);

  // Did we find a match?
  LIR* failed_branch = OpCondBranch(kCondNe, nullptr);

  // yes, we matched.  Compute the index of the result.
  // index = ((curr_ptr - orig_ptr) / 2) - 1.
  OpRegReg(kOpSub, rs_rDI, rs_rBX);
  OpRegImm(kOpAsr, rs_rDI, 1);
  NewLIR3(kX86Lea32RM, rl_return.reg.GetReg(), rs_rDI.GetReg(), -1);
  LIR *all_done = NewLIR1(kX86Jmp8, 0);

  // Failed to match; return -1.
  LIR *not_found = NewLIR0(kPseudoTargetLabel);
  length_compare->target = not_found;
  failed_branch->target = not_found;
  LoadConstantNoClobber(rl_return.reg, -1);

  // And join up at the end.
  all_done->target = NewLIR0(kPseudoTargetLabel);
  // Restore EDI from the stack.
  NewLIR1(kX86Pop32R, rs_rDI.GetReg());

  // Out of line code returns here.
  if (slowpath_branch != nullptr) {
    LIR *return_point = NewLIR0(kPseudoTargetLabel);
    AddIntrinsicSlowPath(info, slowpath_branch, return_point);
  }

  StoreValue(rl_dest, rl_return);
  return true;
}

/*
 * @brief Enter an 'advance LOC' into the FDE buffer
 * @param buf FDE buffer.
 * @param increment Amount by which to increase the current location.
 */
static void AdvanceLoc(std::vector<uint8_t>&buf, uint32_t increment) {
  if (increment < 64) {
    // Encoding in opcode.
    buf.push_back(0x1 << 6 | increment);
  } else if (increment < 256) {
    // Single byte delta.
    buf.push_back(0x02);
    buf.push_back(increment);
  } else if (increment < 256 * 256) {
    // Two byte delta.
    buf.push_back(0x03);
    buf.push_back(increment & 0xff);
    buf.push_back((increment >> 8) & 0xff);
  } else {
    // Four byte delta.
    buf.push_back(0x04);
    PushWord(buf, increment);
  }
}


std::vector<uint8_t>* X86CFIInitialization() {
  return X86Mir2Lir::ReturnCommonCallFrameInformation();
}

std::vector<uint8_t>* X86Mir2Lir::ReturnCommonCallFrameInformation() {
  std::vector<uint8_t>*cfi_info = new std::vector<uint8_t>;

  // Length of the CIE (except for this field).
  PushWord(*cfi_info, 16);

  // CIE id.
  PushWord(*cfi_info, 0xFFFFFFFFU);

  // Version: 3.
  cfi_info->push_back(0x03);

  // Augmentation: empty string.
  cfi_info->push_back(0x0);

  // Code alignment: 1.
  cfi_info->push_back(0x01);

  // Data alignment: -4.
  cfi_info->push_back(0x7C);

  // Return address register (R8).
  cfi_info->push_back(0x08);

  // Initial return PC is 4(ESP): DW_CFA_def_cfa R4 4.
  cfi_info->push_back(0x0C);
  cfi_info->push_back(0x04);
  cfi_info->push_back(0x04);

  // Return address location: 0(SP): DW_CFA_offset R8 1 (* -4);.
  cfi_info->push_back(0x2 << 6 | 0x08);
  cfi_info->push_back(0x01);

  // And 2 Noops to align to 4 byte boundary.
  cfi_info->push_back(0x0);
  cfi_info->push_back(0x0);

  DCHECK_EQ(cfi_info->size() & 3, 0U);
  return cfi_info;
}

static void EncodeUnsignedLeb128(std::vector<uint8_t>& buf, uint32_t value) {
  uint8_t buffer[12];
  uint8_t *ptr = EncodeUnsignedLeb128(buffer, value);
  for (uint8_t *p = buffer; p < ptr; p++) {
    buf.push_back(*p);
  }
}

std::vector<uint8_t>* X86Mir2Lir::ReturnCallFrameInformation() {
  std::vector<uint8_t>*cfi_info = new std::vector<uint8_t>;

  // Generate the FDE for the method.
  DCHECK_NE(data_offset_, 0U);

  // Length (will be filled in later in this routine).
  PushWord(*cfi_info, 0);

  // CIE_pointer (can be filled in by linker); might be left at 0 if there is only
  // one CIE for the whole debug_frame section.
  PushWord(*cfi_info, 0);

  // 'initial_location' (filled in by linker).
  PushWord(*cfi_info, 0);

  // 'address_range' (number of bytes in the method).
  PushWord(*cfi_info, data_offset_);

  // The instructions in the FDE.
  if (stack_decrement_ != nullptr) {
    // Advance LOC to just past the stack decrement.
    uint32_t pc = NEXT_LIR(stack_decrement_)->offset;
    AdvanceLoc(*cfi_info, pc);

    // Now update the offset to the call frame: DW_CFA_def_cfa_offset frame_size.
    cfi_info->push_back(0x0e);
    EncodeUnsignedLeb128(*cfi_info, frame_size_);

    // We continue with that stack until the epilogue.
    if (stack_increment_ != nullptr) {
      uint32_t new_pc = NEXT_LIR(stack_increment_)->offset;
      AdvanceLoc(*cfi_info, new_pc - pc);

      // We probably have code snippets after the epilogue, so save the
      // current state: DW_CFA_remember_state.
      cfi_info->push_back(0x0a);

      // We have now popped the stack: DW_CFA_def_cfa_offset 4.  There is only the return
      // PC on the stack now.
      cfi_info->push_back(0x0e);
      EncodeUnsignedLeb128(*cfi_info, 4);

      // Everything after that is the same as before the epilogue.
      // Stack bump was followed by RET instruction.
      LIR *post_ret_insn = NEXT_LIR(NEXT_LIR(stack_increment_));
      if (post_ret_insn != nullptr) {
        pc = new_pc;
        new_pc = post_ret_insn->offset;
        AdvanceLoc(*cfi_info, new_pc - pc);
        // Restore the state: DW_CFA_restore_state.
        cfi_info->push_back(0x0b);
      }
    }
  }

  // Padding to a multiple of 4
  while ((cfi_info->size() & 3) != 0) {
    // DW_CFA_nop is encoded as 0.
    cfi_info->push_back(0);
  }

  // Set the length of the FDE inside the generated bytes.
  uint32_t length = cfi_info->size() - 4;
  (*cfi_info)[0] = length;
  (*cfi_info)[1] = length >> 8;
  (*cfi_info)[2] = length >> 16;
  (*cfi_info)[3] = length >> 24;
  return cfi_info;
}

void X86Mir2Lir::GenMachineSpecificExtendedMethodMIR(BasicBlock* bb, MIR* mir) {
  switch (static_cast<ExtendedMIROpcode>(mir->dalvikInsn.opcode)) {
    case kMirOpConstVector:
      GenConst128(bb, mir);
      break;
    case kMirOpMoveVector:
      GenMoveVector(bb, mir);
      break;
    case kMirOpPackedMultiply:
      GenMultiplyVector(bb, mir);
      break;
    case kMirOpPackedAddition:
      GenAddVector(bb, mir);
      break;
    case kMirOpPackedSubtract:
      GenSubtractVector(bb, mir);
      break;
    case kMirOpPackedShiftLeft:
      GenShiftLeftVector(bb, mir);
      break;
    case kMirOpPackedSignedShiftRight:
      GenSignedShiftRightVector(bb, mir);
      break;
    case kMirOpPackedUnsignedShiftRight:
      GenUnsignedShiftRightVector(bb, mir);
      break;
    case kMirOpPackedAnd:
      GenAndVector(bb, mir);
      break;
    case kMirOpPackedOr:
      GenOrVector(bb, mir);
      break;
    case kMirOpPackedXor:
      GenXorVector(bb, mir);
      break;
    case kMirOpPackedAddReduce:
      GenAddReduceVector(bb, mir);
      break;
    case kMirOpPackedReduce:
      GenReduceVector(bb, mir);
      break;
    case kMirOpPackedSet:
      GenSetVector(bb, mir);
      break;
    default:
      break;
  }
}

void X86Mir2Lir::GenConst128(BasicBlock* bb, MIR* mir) {
  int type_size = mir->dalvikInsn.vA;
  // We support 128 bit vectors.
  DCHECK_EQ(type_size & 0xFFFF, 128);
  RegStorage rs_dest = RegStorage::Solo128(mir->dalvikInsn.vB);
  uint32_t *args = mir->dalvikInsn.arg;
  int reg = rs_dest.GetReg();
  // Check for all 0 case.
  if (args[0] == 0 && args[1] == 0 && args[2] == 0 && args[3] == 0) {
    NewLIR2(kX86XorpsRR, reg, reg);
    return;
  }
  // Okay, load it from the constant vector area.
  LIR *data_target = ScanVectorLiteral(mir);
  if (data_target == nullptr) {
    data_target = AddVectorLiteral(mir);
  }

  // Address the start of the method.
  RegLocation rl_method = mir_graph_->GetRegLocation(base_of_code_->s_reg_low);
  if (rl_method.wide) {
    rl_method = LoadValueWide(rl_method, kCoreReg);
  } else {
    rl_method = LoadValue(rl_method, kCoreReg);
  }

  // Load the proper value from the literal area.
  // We don't know the proper offset for the value, so pick one that will force
  // 4 byte offset.  We will fix this up in the assembler later to have the right
  // value.
  ScopedMemRefType mem_ref_type(this, ResourceMask::kLiteral);
  LIR *load = NewLIR3(kX86Mova128RM, reg, rl_method.reg.GetReg(),  256 /* bogus */);
  load->flags.fixup = kFixupLoad;
  load->target = data_target;
}

void X86Mir2Lir::GenMoveVector(BasicBlock *bb, MIR *mir) {
  // We only support 128 bit registers.
  DCHECK_EQ(mir->dalvikInsn.vA & 0xFFFF, 128U);
  RegStorage rs_dest = RegStorage::Solo128(mir->dalvikInsn.vB);
  RegStorage rs_src = RegStorage::Solo128(mir->dalvikInsn.vC);
  NewLIR2(kX86Mova128RR, rs_dest.GetReg(), rs_src.GetReg());
}

void X86Mir2Lir::GenMultiplyVector(BasicBlock *bb, MIR *mir) {
  DCHECK_EQ(mir->dalvikInsn.vA & 0xFFFF, 128U);
  OpSize opsize = static_cast<OpSize>(mir->dalvikInsn.vA >> 16);
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vB);
  RegStorage rs_src2 = RegStorage::Solo128(mir->dalvikInsn.vC);
  int opcode = 0;
  switch (opsize) {
    case k32:
      opcode = kX86PmulldRR;
      break;
    case kSignedHalf:
      opcode = kX86PmullwRR;
      break;
    case kSingle:
      opcode = kX86MulpsRR;
      break;
    case kDouble:
      opcode = kX86MulpdRR;
      break;
    default:
      LOG(FATAL) << "Unsupported vector multiply " << opsize;
      break;
  }
  NewLIR2(opcode, rs_dest_src1.GetReg(), rs_src2.GetReg());
}

void X86Mir2Lir::GenAddVector(BasicBlock *bb, MIR *mir) {
  DCHECK_EQ(mir->dalvikInsn.vA & 0xFFFF, 128U);
  OpSize opsize = static_cast<OpSize>(mir->dalvikInsn.vA >> 16);
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vB);
  RegStorage rs_src2 = RegStorage::Solo128(mir->dalvikInsn.vC);
  int opcode = 0;
  switch (opsize) {
    case k32:
      opcode = kX86PadddRR;
      break;
    case kSignedHalf:
    case kUnsignedHalf:
      opcode = kX86PaddwRR;
      break;
    case kUnsignedByte:
    case kSignedByte:
      opcode = kX86PaddbRR;
      break;
    case kSingle:
      opcode = kX86AddpsRR;
      break;
    case kDouble:
      opcode = kX86AddpdRR;
      break;
    default:
      LOG(FATAL) << "Unsupported vector addition " << opsize;
      break;
  }
  NewLIR2(opcode, rs_dest_src1.GetReg(), rs_src2.GetReg());
}

void X86Mir2Lir::GenSubtractVector(BasicBlock *bb, MIR *mir) {
  DCHECK_EQ(mir->dalvikInsn.vA & 0xFFFF, 128U);
  OpSize opsize = static_cast<OpSize>(mir->dalvikInsn.vA >> 16);
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vB);
  RegStorage rs_src2 = RegStorage::Solo128(mir->dalvikInsn.vC);
  int opcode = 0;
  switch (opsize) {
    case k32:
      opcode = kX86PsubdRR;
      break;
    case kSignedHalf:
    case kUnsignedHalf:
      opcode = kX86PsubwRR;
      break;
    case kUnsignedByte:
    case kSignedByte:
      opcode = kX86PsubbRR;
      break;
    case kSingle:
      opcode = kX86SubpsRR;
      break;
    case kDouble:
      opcode = kX86SubpdRR;
      break;
    default:
      LOG(FATAL) << "Unsupported vector subtraction " << opsize;
      break;
  }
  NewLIR2(opcode, rs_dest_src1.GetReg(), rs_src2.GetReg());
}

void X86Mir2Lir::GenShiftLeftVector(BasicBlock *bb, MIR *mir) {
  DCHECK_EQ(mir->dalvikInsn.vA & 0xFFFF, 128U);
  OpSize opsize = static_cast<OpSize>(mir->dalvikInsn.vA >> 16);
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vB);
  int imm = mir->dalvikInsn.vC;
  int opcode = 0;
  switch (opsize) {
    case k32:
      opcode = kX86PslldRI;
      break;
    case k64:
      opcode = kX86PsllqRI;
      break;
    case kSignedHalf:
    case kUnsignedHalf:
      opcode = kX86PsllwRI;
      break;
    default:
      LOG(FATAL) << "Unsupported vector shift left " << opsize;
      break;
  }
  NewLIR2(opcode, rs_dest_src1.GetReg(), imm);
}

void X86Mir2Lir::GenSignedShiftRightVector(BasicBlock *bb, MIR *mir) {
  DCHECK_EQ(mir->dalvikInsn.vA & 0xFFFF, 128U);
  OpSize opsize = static_cast<OpSize>(mir->dalvikInsn.vA >> 16);
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vB);
  int imm = mir->dalvikInsn.vC;
  int opcode = 0;
  switch (opsize) {
    case k32:
      opcode = kX86PsradRI;
      break;
    case kSignedHalf:
    case kUnsignedHalf:
      opcode = kX86PsrawRI;
      break;
    default:
      LOG(FATAL) << "Unsupported vector signed shift right " << opsize;
      break;
  }
  NewLIR2(opcode, rs_dest_src1.GetReg(), imm);
}

void X86Mir2Lir::GenUnsignedShiftRightVector(BasicBlock *bb, MIR *mir) {
  DCHECK_EQ(mir->dalvikInsn.vA & 0xFFFF, 128U);
  OpSize opsize = static_cast<OpSize>(mir->dalvikInsn.vA >> 16);
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vB);
  int imm = mir->dalvikInsn.vC;
  int opcode = 0;
  switch (opsize) {
    case k32:
      opcode = kX86PsrldRI;
      break;
    case k64:
      opcode = kX86PsrlqRI;
      break;
    case kSignedHalf:
    case kUnsignedHalf:
      opcode = kX86PsrlwRI;
      break;
    default:
      LOG(FATAL) << "Unsupported vector unsigned shift right " << opsize;
      break;
  }
  NewLIR2(opcode, rs_dest_src1.GetReg(), imm);
}

void X86Mir2Lir::GenAndVector(BasicBlock *bb, MIR *mir) {
  // We only support 128 bit registers.
  DCHECK_EQ(mir->dalvikInsn.vA & 0xFFFF, 128U);
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vB);
  RegStorage rs_src2 = RegStorage::Solo128(mir->dalvikInsn.vC);
  NewLIR2(kX86PandRR, rs_dest_src1.GetReg(), rs_src2.GetReg());
}

void X86Mir2Lir::GenOrVector(BasicBlock *bb, MIR *mir) {
  // We only support 128 bit registers.
  DCHECK_EQ(mir->dalvikInsn.vA & 0xFFFF, 128U);
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vB);
  RegStorage rs_src2 = RegStorage::Solo128(mir->dalvikInsn.vC);
  NewLIR2(kX86PorRR, rs_dest_src1.GetReg(), rs_src2.GetReg());
}

void X86Mir2Lir::GenXorVector(BasicBlock *bb, MIR *mir) {
  // We only support 128 bit registers.
  DCHECK_EQ(mir->dalvikInsn.vA & 0xFFFF, 128U);
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vB);
  RegStorage rs_src2 = RegStorage::Solo128(mir->dalvikInsn.vC);
  NewLIR2(kX86PxorRR, rs_dest_src1.GetReg(), rs_src2.GetReg());
}

void X86Mir2Lir::GenAddReduceVector(BasicBlock *bb, MIR *mir) {
  DCHECK_EQ(mir->dalvikInsn.vA & 0xFFFF, 128U);
  OpSize opsize = static_cast<OpSize>(mir->dalvikInsn.vA >> 16);
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vB);
  int imm = mir->dalvikInsn.vC;
  int opcode = 0;
  switch (opsize) {
    case k32:
      opcode = kX86PhadddRR;
      break;
    case kSignedHalf:
    case kUnsignedHalf:
      opcode = kX86PhaddwRR;
      break;
    default:
      LOG(FATAL) << "Unsupported vector add reduce " << opsize;
      break;
  }
  NewLIR2(opcode, rs_dest_src1.GetReg(), imm);
}

void X86Mir2Lir::GenReduceVector(BasicBlock *bb, MIR *mir) {
  DCHECK_EQ(mir->dalvikInsn.vA & 0xFFFF, 128U);
  OpSize opsize = static_cast<OpSize>(mir->dalvikInsn.vA >> 16);
  RegStorage rs_src = RegStorage::Solo128(mir->dalvikInsn.vB);
  int index = mir->dalvikInsn.arg[0];
  int opcode = 0;
  switch (opsize) {
    case k32:
      opcode = kX86PextrdRRI;
      break;
    case kSignedHalf:
    case kUnsignedHalf:
      opcode = kX86PextrwRRI;
      break;
    case kUnsignedByte:
    case kSignedByte:
      opcode = kX86PextrbRRI;
      break;
    default:
      LOG(FATAL) << "Unsupported vector reduce " << opsize;
      break;
  }
  // We need to extract to a GPR.
  RegStorage temp = AllocTemp();
  NewLIR3(opcode, temp.GetReg(), rs_src.GetReg(), index);

  // Assume that the destination VR is in the def for the mir.
  RegLocation rl_dest = mir_graph_->GetDest(mir);
  RegLocation rl_temp =
    {kLocPhysReg, 0, 0, 0, 0, 0, 0, 0, 1, temp, INVALID_SREG, INVALID_SREG};
  StoreValue(rl_dest, rl_temp);
}

void X86Mir2Lir::GenSetVector(BasicBlock *bb, MIR *mir) {
  DCHECK_EQ(mir->dalvikInsn.vA & 0xFFFF, 128U);
  OpSize opsize = static_cast<OpSize>(mir->dalvikInsn.vA >> 16);
  RegStorage rs_dest = RegStorage::Solo128(mir->dalvikInsn.vB);
  int op_low = 0, op_high = 0;
  switch (opsize) {
    case k32:
      op_low = kX86PshufdRRI;
      break;
    case kSignedHalf:
    case kUnsignedHalf:
      // Handles low quadword.
      op_low = kX86PshuflwRRI;
      // Handles upper quadword.
      op_high = kX86PshufdRRI;
      break;
    default:
      LOG(FATAL) << "Unsupported vector set " << opsize;
      break;
  }

  // Load the value from the VR into a GPR.
  RegLocation rl_src = mir_graph_->GetSrc(mir, 0);
  rl_src = LoadValue(rl_src, kCoreReg);

  // Load the value into the XMM register.
  NewLIR2(kX86MovdxrRR, rs_dest.GetReg(), rl_src.reg.GetReg());

  // Now shuffle the value across the destination.
  NewLIR3(op_low, rs_dest.GetReg(), rs_dest.GetReg(), 0);

  // And then repeat as needed.
  if (op_high != 0) {
    NewLIR3(op_high, rs_dest.GetReg(), rs_dest.GetReg(), 0);
  }
}


LIR *X86Mir2Lir::ScanVectorLiteral(MIR *mir) {
  int *args = reinterpret_cast<int*>(mir->dalvikInsn.arg);
  for (LIR *p = const_vectors_; p != nullptr; p = p->next) {
    if (args[0] == p->operands[0] && args[1] == p->operands[1] &&
        args[2] == p->operands[2] && args[3] == p->operands[3]) {
      return p;
    }
  }
  return nullptr;
}

LIR *X86Mir2Lir::AddVectorLiteral(MIR *mir) {
  LIR* new_value = static_cast<LIR*>(arena_->Alloc(sizeof(LIR), kArenaAllocData));
  int *args = reinterpret_cast<int*>(mir->dalvikInsn.arg);
  new_value->operands[0] = args[0];
  new_value->operands[1] = args[1];
  new_value->operands[2] = args[2];
  new_value->operands[3] = args[3];
  new_value->next = const_vectors_;
  if (const_vectors_ == nullptr) {
    estimated_native_code_size_ += 12;  // Amount needed to align to 16 byte boundary.
  }
  estimated_native_code_size_ += 16;  // Space for one vector.
  const_vectors_ = new_value;
  return new_value;
}

// ------------ ABI support: mapping of args to physical registers -------------
RegStorage X86Mir2Lir::InToRegStorageX86_64Mapper::GetNextReg(bool is_double_or_float, bool is_wide) {
  const RegStorage coreArgMappingToPhysicalReg[] = {rs_rX86_ARG1, rs_rX86_ARG2, rs_rX86_ARG3, rs_rX86_ARG4, rs_rX86_ARG5};
  const int coreArgMappingToPhysicalRegSize = sizeof(coreArgMappingToPhysicalReg) / sizeof(RegStorage);
  const RegStorage fpArgMappingToPhysicalReg[] = {rs_rX86_FARG0, rs_rX86_FARG1, rs_rX86_FARG2, rs_rX86_FARG3,
                                                  rs_rX86_FARG4, rs_rX86_FARG5, rs_rX86_FARG6, rs_rX86_FARG7};
  const int fpArgMappingToPhysicalRegSize = sizeof(fpArgMappingToPhysicalReg) / sizeof(RegStorage);

  RegStorage result = RegStorage::InvalidReg();
  if (is_double_or_float) {
    if (cur_fp_reg_ < fpArgMappingToPhysicalRegSize) {
      result = fpArgMappingToPhysicalReg[cur_fp_reg_++];
      if (result.Valid()) {
        result = is_wide ? RegStorage::FloatSolo64(result.GetReg()) : RegStorage::FloatSolo32(result.GetReg());
      }
    }
  } else {
    if (cur_core_reg_ < coreArgMappingToPhysicalRegSize) {
      result = coreArgMappingToPhysicalReg[cur_core_reg_++];
      if (result.Valid()) {
        result = is_wide ? RegStorage::Solo64(result.GetReg()) : RegStorage::Solo32(result.GetReg());
      }
    }
  }
  return result;
}

RegStorage X86Mir2Lir::InToRegStorageMapping::Get(int in_position) {
  DCHECK(IsInitialized());
  auto res = mapping_.find(in_position);
  return res != mapping_.end() ? res->second : RegStorage::InvalidReg();
}

void X86Mir2Lir::InToRegStorageMapping::Initialize(RegLocation* arg_locs, int count, InToRegStorageMapper* mapper) {
  DCHECK(mapper != nullptr);
  max_mapped_in_ = -1;
  is_there_stack_mapped_ = false;
  for (int in_position = 0; in_position < count; in_position++) {
     RegStorage reg = mapper->GetNextReg(arg_locs[in_position].fp, arg_locs[in_position].wide);
     if (reg.Valid()) {
       mapping_[in_position] = reg;
       max_mapped_in_ = std::max(max_mapped_in_, in_position);
       if (reg.Is64BitSolo()) {
         // We covered 2 args, so skip the next one
         in_position++;
       }
     } else {
       is_there_stack_mapped_ = true;
     }
  }
  initialized_ = true;
}

RegStorage X86Mir2Lir::GetArgMappingToPhysicalReg(int arg_num) {
  if (!Gen64Bit()) {
    return GetCoreArgMappingToPhysicalReg(arg_num);
  }

  if (!in_to_reg_storage_mapping_.IsInitialized()) {
    int start_vreg = cu_->num_dalvik_registers - cu_->num_ins;
    RegLocation* arg_locs = &mir_graph_->reg_location_[start_vreg];

    InToRegStorageX86_64Mapper mapper;
    in_to_reg_storage_mapping_.Initialize(arg_locs, cu_->num_ins, &mapper);
  }
  return in_to_reg_storage_mapping_.Get(arg_num);
}

RegStorage X86Mir2Lir::GetCoreArgMappingToPhysicalReg(int core_arg_num) {
  // For the 32-bit internal ABI, the first 3 arguments are passed in registers.
  // Not used for 64-bit, TODO: Move X86_32 to the same framework
  switch (core_arg_num) {
    case 0:
      return rs_rX86_ARG1;
    case 1:
      return rs_rX86_ARG2;
    case 2:
      return rs_rX86_ARG3;
    default:
      return RegStorage::InvalidReg();
  }
}

// ---------End of ABI support: mapping of args to physical registers -------------

/*
 * If there are any ins passed in registers that have not been promoted
 * to a callee-save register, flush them to the frame.  Perform initial
 * assignment of promoted arguments.
 *
 * ArgLocs is an array of location records describing the incoming arguments
 * with one location record per word of argument.
 */
void X86Mir2Lir::FlushIns(RegLocation* ArgLocs, RegLocation rl_method) {
  if (!Gen64Bit()) return Mir2Lir::FlushIns(ArgLocs, rl_method);
  /*
   * Dummy up a RegLocation for the incoming Method*
   * It will attempt to keep kArg0 live (or copy it to home location
   * if promoted).
   */

  RegLocation rl_src = rl_method;
  rl_src.location = kLocPhysReg;
  rl_src.reg = TargetReg(kArg0);
  rl_src.home = false;
  MarkLive(rl_src);
  StoreValue(rl_method, rl_src);
  // If Method* has been promoted, explicitly flush
  if (rl_method.location == kLocPhysReg) {
    StoreRefDisp(TargetReg(kSp), 0, TargetReg(kArg0));
  }

  if (cu_->num_ins == 0) {
    return;
  }

  int start_vreg = cu_->num_dalvik_registers - cu_->num_ins;
  /*
   * Copy incoming arguments to their proper home locations.
   * NOTE: an older version of dx had an issue in which
   * it would reuse static method argument registers.
   * This could result in the same Dalvik virtual register
   * being promoted to both core and fp regs. To account for this,
   * we only copy to the corresponding promoted physical register
   * if it matches the type of the SSA name for the incoming
   * argument.  It is also possible that long and double arguments
   * end up half-promoted.  In those cases, we must flush the promoted
   * half to memory as well.
   */
  ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
  for (int i = 0; i < cu_->num_ins; i++) {
    PromotionMap* v_map = &promotion_map_[start_vreg + i];
    RegStorage reg = RegStorage::InvalidReg();
    // get reg corresponding to input
    reg = GetArgMappingToPhysicalReg(i);

    if (reg.Valid()) {
      // If arriving in register
      bool need_flush = true;
      RegLocation* t_loc = &ArgLocs[i];
      if ((v_map->core_location == kLocPhysReg) && !t_loc->fp) {
        OpRegCopy(RegStorage::Solo32(v_map->core_reg), reg);
        need_flush = false;
      } else if ((v_map->fp_location == kLocPhysReg) && t_loc->fp) {
        OpRegCopy(RegStorage::Solo32(v_map->FpReg), reg);
        need_flush = false;
      } else {
        need_flush = true;
      }

      // For wide args, force flush if not fully promoted
      if (t_loc->wide) {
        PromotionMap* p_map = v_map + (t_loc->high_word ? -1 : +1);
        // Is only half promoted?
        need_flush |= (p_map->core_location != v_map->core_location) ||
            (p_map->fp_location != v_map->fp_location);
      }
      if (need_flush) {
        if (t_loc->wide && t_loc->fp) {
          StoreBaseDisp(TargetReg(kSp), SRegOffset(start_vreg + i), reg, k64);
          // Increment i to skip the next one
          i++;
        } else if (t_loc->wide && !t_loc->fp) {
          StoreBaseDisp(TargetReg(kSp), SRegOffset(start_vreg + i), reg, k64);
          // Increment i to skip the next one
          i++;
        } else {
          Store32Disp(TargetReg(kSp), SRegOffset(start_vreg + i), reg);
        }
      }
    } else {
      // If arriving in frame & promoted
      if (v_map->core_location == kLocPhysReg) {
        Load32Disp(TargetReg(kSp), SRegOffset(start_vreg + i), RegStorage::Solo32(v_map->core_reg));
      }
      if (v_map->fp_location == kLocPhysReg) {
        Load32Disp(TargetReg(kSp), SRegOffset(start_vreg + i), RegStorage::Solo32(v_map->FpReg));
      }
    }
  }
}

/*
 * Load up to 5 arguments, the first three of which will be in
 * kArg1 .. kArg3.  On entry kArg0 contains the current method pointer,
 * and as part of the load sequence, it must be replaced with
 * the target method pointer.  Note, this may also be called
 * for "range" variants if the number of arguments is 5 or fewer.
 */
int X86Mir2Lir::GenDalvikArgsNoRange(CallInfo* info,
                                  int call_state, LIR** pcrLabel, NextCallInsn next_call_insn,
                                  const MethodReference& target_method,
                                  uint32_t vtable_idx, uintptr_t direct_code,
                                  uintptr_t direct_method, InvokeType type, bool skip_this) {
  if (!Gen64Bit()) {
    return Mir2Lir::GenDalvikArgsNoRange(info,
                                  call_state, pcrLabel, next_call_insn,
                                  target_method,
                                  vtable_idx, direct_code,
                                  direct_method, type, skip_this);
  }
  return GenDalvikArgsRange(info,
                       call_state, pcrLabel, next_call_insn,
                       target_method,
                       vtable_idx, direct_code,
                       direct_method, type, skip_this);
}

/*
 * May have 0+ arguments (also used for jumbo).  Note that
 * source virtual registers may be in physical registers, so may
 * need to be flushed to home location before copying.  This
 * applies to arg3 and above (see below).
 *
 * Two general strategies:
 *    If < 20 arguments
 *       Pass args 3-18 using vldm/vstm block copy
 *       Pass arg0, arg1 & arg2 in kArg1-kArg3
 *    If 20+ arguments
 *       Pass args arg19+ using memcpy block copy
 *       Pass arg0, arg1 & arg2 in kArg1-kArg3
 *
 */
int X86Mir2Lir::GenDalvikArgsRange(CallInfo* info, int call_state,
                                LIR** pcrLabel, NextCallInsn next_call_insn,
                                const MethodReference& target_method,
                                uint32_t vtable_idx, uintptr_t direct_code, uintptr_t direct_method,
                                InvokeType type, bool skip_this) {
  if (!Gen64Bit()) {
    return Mir2Lir::GenDalvikArgsRange(info, call_state,
                                pcrLabel, next_call_insn,
                                target_method,
                                vtable_idx, direct_code, direct_method,
                                type, skip_this);
  }

  /* If no arguments, just return */
  if (info->num_arg_words == 0)
    return call_state;

  const int start_index = skip_this ? 1 : 0;

  InToRegStorageX86_64Mapper mapper;
  InToRegStorageMapping in_to_reg_storage_mapping;
  in_to_reg_storage_mapping.Initialize(info->args, info->num_arg_words, &mapper);
  const int last_mapped_in = in_to_reg_storage_mapping.GetMaxMappedIn();
  const int size_of_the_last_mapped = last_mapped_in == -1 ? 1 :
          in_to_reg_storage_mapping.Get(last_mapped_in).Is64BitSolo() ? 2 : 1;
  int regs_left_to_pass_via_stack = info->num_arg_words - (last_mapped_in + size_of_the_last_mapped);

  // Fisrt of all, check whether it make sense to use bulk copying
  // Optimization is aplicable only for range case
  // TODO: make a constant instead of 2
  if (info->is_range && regs_left_to_pass_via_stack >= 2) {
    // Scan the rest of the args - if in phys_reg flush to memory
    for (int next_arg = last_mapped_in + size_of_the_last_mapped; next_arg < info->num_arg_words;) {
      RegLocation loc = info->args[next_arg];
      if (loc.wide) {
        loc = UpdateLocWide(loc);
        if (loc.location == kLocPhysReg) {
          ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
          StoreBaseDisp(TargetReg(kSp), SRegOffset(loc.s_reg_low), loc.reg, k64);
        }
        next_arg += 2;
      } else {
        loc = UpdateLoc(loc);
        if (loc.location == kLocPhysReg) {
          ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
          StoreBaseDisp(TargetReg(kSp), SRegOffset(loc.s_reg_low), loc.reg, k32);
        }
        next_arg++;
      }
    }

    // Logic below assumes that Method pointer is at offset zero from SP.
    DCHECK_EQ(VRegOffset(static_cast<int>(kVRegMethodPtrBaseReg)), 0);

    // The rest can be copied together
    int start_offset = SRegOffset(info->args[last_mapped_in + size_of_the_last_mapped].s_reg_low);
    int outs_offset = StackVisitor::GetOutVROffset(last_mapped_in + size_of_the_last_mapped, cu_->instruction_set);

    int current_src_offset = start_offset;
    int current_dest_offset = outs_offset;

    // Only davik regs are accessed in this loop; no next_call_insn() calls.
    ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
    while (regs_left_to_pass_via_stack > 0) {
      // This is based on the knowledge that the stack itself is 16-byte aligned.
      bool src_is_16b_aligned = (current_src_offset & 0xF) == 0;
      bool dest_is_16b_aligned = (current_dest_offset & 0xF) == 0;
      size_t bytes_to_move;

      /*
       * The amount to move defaults to 32-bit. If there are 4 registers left to move, then do a
       * a 128-bit move because we won't get the chance to try to aligned. If there are more than
       * 4 registers left to move, consider doing a 128-bit only if either src or dest are aligned.
       * We do this because we could potentially do a smaller move to align.
       */
      if (regs_left_to_pass_via_stack == 4 ||
          (regs_left_to_pass_via_stack > 4 && (src_is_16b_aligned || dest_is_16b_aligned))) {
        // Moving 128-bits via xmm register.
        bytes_to_move = sizeof(uint32_t) * 4;

        // Allocate a free xmm temp. Since we are working through the calling sequence,
        // we expect to have an xmm temporary available.  AllocTempDouble will abort if
        // there are no free registers.
        RegStorage temp = AllocTempDouble();

        LIR* ld1 = nullptr;
        LIR* ld2 = nullptr;
        LIR* st1 = nullptr;
        LIR* st2 = nullptr;

        /*
         * The logic is similar for both loads and stores. If we have 16-byte alignment,
         * do an aligned move. If we have 8-byte alignment, then do the move in two
         * parts. This approach prevents possible cache line splits. Finally, fall back
         * to doing an unaligned move. In most cases we likely won't split the cache
         * line but we cannot prove it and thus take a conservative approach.
         */
        bool src_is_8b_aligned = (current_src_offset & 0x7) == 0;
        bool dest_is_8b_aligned = (current_dest_offset & 0x7) == 0;

        ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
        if (src_is_16b_aligned) {
          ld1 = OpMovRegMem(temp, TargetReg(kSp), current_src_offset, kMovA128FP);
        } else if (src_is_8b_aligned) {
          ld1 = OpMovRegMem(temp, TargetReg(kSp), current_src_offset, kMovLo128FP);
          ld2 = OpMovRegMem(temp, TargetReg(kSp), current_src_offset + (bytes_to_move >> 1),
                            kMovHi128FP);
        } else {
          ld1 = OpMovRegMem(temp, TargetReg(kSp), current_src_offset, kMovU128FP);
        }

        if (dest_is_16b_aligned) {
          st1 = OpMovMemReg(TargetReg(kSp), current_dest_offset, temp, kMovA128FP);
        } else if (dest_is_8b_aligned) {
          st1 = OpMovMemReg(TargetReg(kSp), current_dest_offset, temp, kMovLo128FP);
          st2 = OpMovMemReg(TargetReg(kSp), current_dest_offset + (bytes_to_move >> 1),
                            temp, kMovHi128FP);
        } else {
          st1 = OpMovMemReg(TargetReg(kSp), current_dest_offset, temp, kMovU128FP);
        }

        // TODO If we could keep track of aliasing information for memory accesses that are wider
        // than 64-bit, we wouldn't need to set up a barrier.
        if (ld1 != nullptr) {
          if (ld2 != nullptr) {
            // For 64-bit load we can actually set up the aliasing information.
            AnnotateDalvikRegAccess(ld1, current_src_offset >> 2, true, true);
            AnnotateDalvikRegAccess(ld2, (current_src_offset + (bytes_to_move >> 1)) >> 2, true, true);
          } else {
            // Set barrier for 128-bit load.
            ld1->u.m.def_mask = &kEncodeAll;
          }
        }
        if (st1 != nullptr) {
          if (st2 != nullptr) {
            // For 64-bit store we can actually set up the aliasing information.
            AnnotateDalvikRegAccess(st1, current_dest_offset >> 2, false, true);
            AnnotateDalvikRegAccess(st2, (current_dest_offset + (bytes_to_move >> 1)) >> 2, false, true);
          } else {
            // Set barrier for 128-bit store.
            st1->u.m.def_mask = &kEncodeAll;
          }
        }

        // Free the temporary used for the data movement.
        FreeTemp(temp);
      } else {
        // Moving 32-bits via general purpose register.
        bytes_to_move = sizeof(uint32_t);

        // Instead of allocating a new temp, simply reuse one of the registers being used
        // for argument passing.
        RegStorage temp = TargetReg(kArg3);

        // Now load the argument VR and store to the outs.
        Load32Disp(TargetReg(kSp), current_src_offset, temp);
        Store32Disp(TargetReg(kSp), current_dest_offset, temp);
      }

      current_src_offset += bytes_to_move;
      current_dest_offset += bytes_to_move;
      regs_left_to_pass_via_stack -= (bytes_to_move >> 2);
    }
    DCHECK_EQ(regs_left_to_pass_via_stack, 0);
  }

  // Now handle rest not registers if they are
  if (in_to_reg_storage_mapping.IsThereStackMapped()) {
    RegStorage regSingle = TargetReg(kArg2);
    RegStorage regWide = RegStorage::Solo64(TargetReg(kArg3).GetReg());
    for (int i = start_index; i <= last_mapped_in + regs_left_to_pass_via_stack; i++) {
      RegLocation rl_arg = info->args[i];
      rl_arg = UpdateRawLoc(rl_arg);
      RegStorage reg = in_to_reg_storage_mapping.Get(i);
      if (!reg.Valid()) {
        int out_offset = StackVisitor::GetOutVROffset(i, cu_->instruction_set);

        {
          ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
          if (rl_arg.wide) {
            if (rl_arg.location == kLocPhysReg) {
              StoreBaseDisp(TargetReg(kSp), out_offset, rl_arg.reg, k64);
            } else {
              LoadValueDirectWideFixed(rl_arg, regWide);
              StoreBaseDisp(TargetReg(kSp), out_offset, regWide, k64);
            }
            i++;
          } else {
            if (rl_arg.location == kLocPhysReg) {
              StoreBaseDisp(TargetReg(kSp), out_offset, rl_arg.reg, k32);
            } else {
              LoadValueDirectFixed(rl_arg, regSingle);
              StoreBaseDisp(TargetReg(kSp), out_offset, regSingle, k32);
            }
          }
        }
        call_state = next_call_insn(cu_, info, call_state, target_method,
                                    vtable_idx, direct_code, direct_method, type);
      }
    }
  }

  // Finish with mapped registers
  for (int i = start_index; i <= last_mapped_in; i++) {
    RegLocation rl_arg = info->args[i];
    rl_arg = UpdateRawLoc(rl_arg);
    RegStorage reg = in_to_reg_storage_mapping.Get(i);
    if (reg.Valid()) {
      if (rl_arg.wide) {
        LoadValueDirectWideFixed(rl_arg, reg);
        i++;
      } else {
        LoadValueDirectFixed(rl_arg, reg);
      }
      call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                               direct_code, direct_method, type);
    }
  }

  call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                           direct_code, direct_method, type);
  if (pcrLabel) {
    if (Runtime::Current()->ExplicitNullChecks()) {
      *pcrLabel = GenExplicitNullCheck(TargetReg(kArg1), info->opt_flags);
    } else {
      *pcrLabel = nullptr;
      // In lieu of generating a check for kArg1 being null, we need to
      // perform a load when doing implicit checks.
      RegStorage tmp = AllocTemp();
      Load32Disp(TargetReg(kArg1), 0, tmp);
      MarkPossibleNullPointerException(info->opt_flags);
      FreeTemp(tmp);
    }
  }
  return call_state;
}

}  // namespace art

