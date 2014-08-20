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
#include "dex/reg_storage_eq.h"
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
static constexpr RegStorage xp_regs_arr_32[] = {
    rs_xr0, rs_xr1, rs_xr2, rs_xr3, rs_xr4, rs_xr5, rs_xr6, rs_xr7,
};
static constexpr RegStorage xp_regs_arr_64[] = {
    rs_xr0, rs_xr1, rs_xr2, rs_xr3, rs_xr4, rs_xr5, rs_xr6, rs_xr7,
    rs_xr8, rs_xr9, rs_xr10, rs_xr11, rs_xr12, rs_xr13, rs_xr14, rs_xr15
};
static constexpr RegStorage reserved_regs_arr_32[] = {rs_rX86_SP_32};
static constexpr RegStorage reserved_regs_arr_64[] = {rs_rX86_SP_32};
static constexpr RegStorage reserved_regs_arr_64q[] = {rs_rX86_SP_64};
static constexpr RegStorage core_temps_arr_32[] = {rs_rAX, rs_rCX, rs_rDX, rs_rBX};
static constexpr RegStorage core_temps_arr_64[] = {
    rs_rAX, rs_rCX, rs_rDX, rs_rSI, rs_rDI,
    rs_r8, rs_r9, rs_r10, rs_r11
};

// How to add register to be available for promotion:
// 1) Remove register from array defining temp
// 2) Update ClobberCallerSave
// 3) Update JNI compiler ABI:
// 3.1) add reg in JniCallingConvention method
// 3.2) update CoreSpillMask/FpSpillMask
// 4) Update entrypoints
// 4.1) Update constants in asm_support_x86_64.h for new frame size
// 4.2) Remove entry in SmashCallerSaves
// 4.3) Update jni_entrypoints to spill/unspill new callee save reg
// 4.4) Update quick_entrypoints to spill/unspill new callee save reg
// 5) Update runtime ABI
// 5.1) Update quick_method_frame_info with new required spills
// 5.2) Update QuickArgumentVisitor with new offsets to gprs and xmms
// Note that you cannot use register corresponding to incoming args
// according to ABI and QCG needs one additional XMM temp for
// bulk copy in preparation to call.
static constexpr RegStorage core_temps_arr_64q[] = {
    rs_r0q, rs_r1q, rs_r2q, rs_r6q, rs_r7q,
    rs_r8q, rs_r9q, rs_r10q, rs_r11q
};
static constexpr RegStorage sp_temps_arr_32[] = {
    rs_fr0, rs_fr1, rs_fr2, rs_fr3, rs_fr4, rs_fr5, rs_fr6, rs_fr7,
};
static constexpr RegStorage sp_temps_arr_64[] = {
    rs_fr0, rs_fr1, rs_fr2, rs_fr3, rs_fr4, rs_fr5, rs_fr6, rs_fr7,
    rs_fr8, rs_fr9, rs_fr10, rs_fr11
};
static constexpr RegStorage dp_temps_arr_32[] = {
    rs_dr0, rs_dr1, rs_dr2, rs_dr3, rs_dr4, rs_dr5, rs_dr6, rs_dr7,
};
static constexpr RegStorage dp_temps_arr_64[] = {
    rs_dr0, rs_dr1, rs_dr2, rs_dr3, rs_dr4, rs_dr5, rs_dr6, rs_dr7,
    rs_dr8, rs_dr9, rs_dr10, rs_dr11
};

static constexpr RegStorage xp_temps_arr_32[] = {
    rs_xr0, rs_xr1, rs_xr2, rs_xr3, rs_xr4, rs_xr5, rs_xr6, rs_xr7,
};
static constexpr RegStorage xp_temps_arr_64[] = {
    rs_xr0, rs_xr1, rs_xr2, rs_xr3, rs_xr4, rs_xr5, rs_xr6, rs_xr7,
    rs_xr8, rs_xr9, rs_xr10, rs_xr11
};

static constexpr ArrayRef<const RegStorage> empty_pool;
static constexpr ArrayRef<const RegStorage> core_regs_32(core_regs_arr_32);
static constexpr ArrayRef<const RegStorage> core_regs_64(core_regs_arr_64);
static constexpr ArrayRef<const RegStorage> core_regs_64q(core_regs_arr_64q);
static constexpr ArrayRef<const RegStorage> sp_regs_32(sp_regs_arr_32);
static constexpr ArrayRef<const RegStorage> sp_regs_64(sp_regs_arr_64);
static constexpr ArrayRef<const RegStorage> dp_regs_32(dp_regs_arr_32);
static constexpr ArrayRef<const RegStorage> dp_regs_64(dp_regs_arr_64);
static constexpr ArrayRef<const RegStorage> xp_regs_32(xp_regs_arr_32);
static constexpr ArrayRef<const RegStorage> xp_regs_64(xp_regs_arr_64);
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
  return cu_->target64 ? x86_64_loc_c_return_ref : x86_loc_c_return_ref;
}

RegLocation X86Mir2Lir::LocCReturnWide() {
  return cu_->target64 ? x86_64_loc_c_return_wide : x86_loc_c_return_wide;
}

RegLocation X86Mir2Lir::LocCReturnFloat() {
  return x86_loc_c_return_float;
}

RegLocation X86Mir2Lir::LocCReturnDouble() {
  return x86_loc_c_return_double;
}

// Return a target-dependent special register for 32-bit.
RegStorage X86Mir2Lir::TargetReg32(SpecialTargetRegister reg) {
  RegStorage res_reg = RegStorage::InvalidReg();
  switch (reg) {
    case kSelf: res_reg = RegStorage::InvalidReg(); break;
    case kSuspend: res_reg =  RegStorage::InvalidReg(); break;
    case kLr: res_reg =  RegStorage::InvalidReg(); break;
    case kPc: res_reg =  RegStorage::InvalidReg(); break;
    case kSp: res_reg =  rs_rX86_SP_32; break;  // This must be the concrete one, as _SP is target-
                                                // specific size.
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
    case kHiddenFpArg: DCHECK(!cu_->target64); res_reg = rs_fr0; break;
    case kCount: res_reg = rs_rX86_COUNT; break;
    default: res_reg = RegStorage::InvalidReg();
  }
  return res_reg;
}

RegStorage X86Mir2Lir::TargetReg(SpecialTargetRegister reg) {
  LOG(FATAL) << "Do not use this function!!!";
  return RegStorage::InvalidReg();
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
          case 'q': {
             int64_t value = static_cast<int64_t>(static_cast<int64_t>(operand) << 32 |
                             static_cast<uint32_t>(lir->operands[operand_number+1]));
             buf +=StringPrintf("%" PRId64, value);
          }
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

RegStorage X86Mir2Lir::AllocateByteRegister() {
  RegStorage reg = AllocTypedTemp(false, kCoreReg);
  if (!cu_->target64) {
    DCHECK_LT(reg.GetRegNum(), rs_rX86_SP.GetRegNum());
  }
  return reg;
}

RegStorage X86Mir2Lir::Get128BitRegister(RegStorage reg) {
  return GetRegInfo(reg)->FindMatchingView(RegisterInfo::k128SoloStorageMask)->GetReg();
}

bool X86Mir2Lir::IsByteRegister(RegStorage reg) {
  return cu_->target64 || reg.GetRegNum() < rs_rX86_SP.GetRegNum();
}

/* Clobber all regs that might be used by an external C call */
void X86Mir2Lir::ClobberCallerSave() {
  if (cu_->target64) {
    Clobber(rs_rAX);
    Clobber(rs_rCX);
    Clobber(rs_rDX);
    Clobber(rs_rSI);
    Clobber(rs_rDI);

    Clobber(rs_r8);
    Clobber(rs_r9);
    Clobber(rs_r10);
    Clobber(rs_r11);

    Clobber(rs_fr8);
    Clobber(rs_fr9);
    Clobber(rs_fr10);
    Clobber(rs_fr11);
  } else {
    Clobber(rs_rAX);
    Clobber(rs_rCX);
    Clobber(rs_rDX);
    Clobber(rs_rBX);
  }

  Clobber(rs_fr0);
  Clobber(rs_fr1);
  Clobber(rs_fr2);
  Clobber(rs_fr3);
  Clobber(rs_fr4);
  Clobber(rs_fr5);
  Clobber(rs_fr6);
  Clobber(rs_fr7);
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
  if (cu_->target64) {
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
  if (cu_->target64) {
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
   * According to the JSR-133 Cookbook, for x86 only StoreLoad/AnyAny barriers need memory fence.
   * All other barriers (LoadAny, AnyStore, StoreStore) are nops due to the x86 memory model.
   * For those cases, all we need to ensure is that there is a scheduling barrier in place.
   */
  if (barrier_kind == kAnyAny) {
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
  if (cu_->target64) {
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
  const ArrayRef<const RegStorage> *xp_regs = cu_->target64 ? &xp_regs_64 : &xp_regs_32;
  for (RegStorage reg : *xp_regs) {
    RegisterInfo* info = new (arena_) RegisterInfo(reg, GetRegMaskCommon(reg));
    reginfo_map_.Put(reg.GetReg(), info);
  }
  const ArrayRef<const RegStorage> *xp_temps = cu_->target64 ? &xp_temps_64 : &xp_temps_32;
  for (RegStorage reg : *xp_temps) {
    RegisterInfo* xp_reg_info = GetRegInfo(reg);
    xp_reg_info->SetIsTemp(true);
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

  if (cu_->target64) {
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

int X86Mir2Lir::VectorRegisterSize() {
  return 128;
}

int X86Mir2Lir::NumReservableVectorRegisters(bool fp_used) {
  return fp_used ? 5 : 7;
}

void X86Mir2Lir::SpillCoreRegs() {
  if (num_core_spills_ == 0) {
    return;
  }
  // Spill mask not including fake return address register
  uint32_t mask = core_spill_mask_ & ~(1 << rs_rRET.GetRegNum());
  int offset = frame_size_ - (GetInstructionSetPointerSize(cu_->instruction_set) * num_core_spills_);
  OpSize size = cu_->target64 ? k64 : k32;
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      StoreBaseDisp(rs_rX86_SP, offset, cu_->target64 ? RegStorage::Solo64(reg) :  RegStorage::Solo32(reg),
                   size, kNotVolatile);
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
  OpSize size = cu_->target64 ? k64 : k32;
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      LoadBaseDisp(rs_rX86_SP, offset, cu_->target64 ? RegStorage::Solo64(reg) :  RegStorage::Solo32(reg),
                   size, kNotVolatile);
      offset += GetInstructionSetPointerSize(cu_->instruction_set);
    }
  }
}

void X86Mir2Lir::SpillFPRegs() {
  if (num_fp_spills_ == 0) {
    return;
  }
  uint32_t mask = fp_spill_mask_;
  int offset = frame_size_ - (GetInstructionSetPointerSize(cu_->instruction_set) * (num_fp_spills_ + num_core_spills_));
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      StoreBaseDisp(rs_rX86_SP, offset, RegStorage::FloatSolo64(reg),
                   k64, kNotVolatile);
      offset += sizeof(double);
    }
  }
}
void X86Mir2Lir::UnSpillFPRegs() {
  if (num_fp_spills_ == 0) {
    return;
  }
  uint32_t mask = fp_spill_mask_;
  int offset = frame_size_ - (GetInstructionSetPointerSize(cu_->instruction_set) * (num_fp_spills_ + num_core_spills_));
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      LoadBaseDisp(rs_rX86_SP, offset, RegStorage::FloatSolo64(reg),
                   k64, kNotVolatile);
      offset += sizeof(double);
    }
  }
}


bool X86Mir2Lir::IsUnconditionalBranch(LIR* lir) {
  return (lir->opcode == kX86Jmp8 || lir->opcode == kX86Jmp32);
}

RegisterClass X86Mir2Lir::RegClassForFieldLoadStore(OpSize size, bool is_volatile) {
  // X86_64 can handle any size.
  if (cu_->target64) {
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

X86Mir2Lir::X86Mir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena)
    : Mir2Lir(cu, mir_graph, arena),
      base_of_code_(nullptr), store_method_addr_(false), store_method_addr_used_(false),
      method_address_insns_(arena, 100, kGrowableArrayMisc),
      class_type_address_insns_(arena, 100, kGrowableArrayMisc),
      call_method_insns_(arena, 100, kGrowableArrayMisc),
      stack_decrement_(nullptr), stack_increment_(nullptr),
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
  if (cu_->target64) {
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

  // Initialize the number of reserved vector registers
  num_reserved_vector_regs_ = -1;
}

Mir2Lir* X86CodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                          ArenaAllocator* const arena) {
  return new X86Mir2Lir(cu, mir_graph, arena);
}

// Not used in x86(-64)
RegStorage X86Mir2Lir::LoadHelper(QuickEntrypointEnum trampoline) {
  LOG(FATAL) << "Unexpected use of LoadHelper in x86";
  return RegStorage::InvalidReg();
}

LIR* X86Mir2Lir::CheckSuspendUsingLoad() {
  // First load the pointer in fs:[suspend-trigger] into eax
  // Then use a test instruction to indirect via that address.
  if (cu_->target64) {
    NewLIR2(kX86Mov64RT, rs_rAX.GetReg(),
        Thread::ThreadSuspendTriggerOffset<8>().Int32Value());
  } else {
    NewLIR2(kX86Mov32RT, rs_rAX.GetReg(),
        Thread::ThreadSuspendTriggerOffset<4>().Int32Value());
  }
  return NewLIR3(kX86Test32RM, rs_rAX.GetReg(), rs_rAX.GetReg(), 0);
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
    int r_base = rs_rX86_SP.GetReg();
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
  LIR *move = RawLIR(current_dalvik_offset_, kX86Mov32RI,
                     TargetReg(symbolic_reg, kNotWide).GetReg(),
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
  LIR *move = RawLIR(current_dalvik_offset_, kX86Mov32RI,
                     TargetReg(symbolic_reg, kNotWide).GetReg(),
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

bool X86Mir2Lir::GenInlinedArrayCopyCharArray(CallInfo* info) {
  RegLocation rl_src = info->args[0];
  RegLocation rl_srcPos = info->args[1];
  RegLocation rl_dst = info->args[2];
  RegLocation rl_dstPos = info->args[3];
  RegLocation rl_length = info->args[4];
  if (rl_srcPos.is_const && (mir_graph_->ConstantValue(rl_srcPos) < 0)) {
    return false;
  }
  if (rl_dstPos.is_const && (mir_graph_->ConstantValue(rl_dstPos) < 0)) {
    return false;
  }
  ClobberCallerSave();
  LockCallTemps();  // Using fixed registers.
  RegStorage tmp_reg = cu_->target64 ? rs_r11 : rs_rBX;
  LoadValueDirectFixed(rl_src, rs_rAX);
  LoadValueDirectFixed(rl_dst, rs_rCX);
  LIR* src_dst_same  = OpCmpBranch(kCondEq, rs_rAX, rs_rCX, nullptr);
  LIR* src_null_branch = OpCmpImmBranch(kCondEq, rs_rAX, 0, nullptr);
  LIR* dst_null_branch = OpCmpImmBranch(kCondEq, rs_rCX, 0, nullptr);
  LoadValueDirectFixed(rl_length, rs_rDX);
  // If the length of the copy is > 128 characters (256 bytes) or negative then go slow path.
  LIR* len_too_big  = OpCmpImmBranch(kCondHi, rs_rDX, 128, nullptr);
  LoadValueDirectFixed(rl_src, rs_rAX);
  LoadWordDisp(rs_rAX, mirror::Array::LengthOffset().Int32Value(), rs_rAX);
  LIR* src_bad_len  = nullptr;
  LIR* srcPos_negative  = nullptr;
  if (!rl_srcPos.is_const) {
    LoadValueDirectFixed(rl_srcPos, tmp_reg);
    srcPos_negative  = OpCmpImmBranch(kCondLt, tmp_reg, 0, nullptr);
    OpRegReg(kOpAdd, tmp_reg, rs_rDX);
    src_bad_len  = OpCmpBranch(kCondLt, rs_rAX, tmp_reg, nullptr);
  } else {
    int32_t pos_val = mir_graph_->ConstantValue(rl_srcPos.orig_sreg);
    if (pos_val == 0) {
      src_bad_len  = OpCmpBranch(kCondLt, rs_rAX, rs_rDX, nullptr);
    } else {
      OpRegRegImm(kOpAdd, tmp_reg,  rs_rDX, pos_val);
      src_bad_len  = OpCmpBranch(kCondLt, rs_rAX, tmp_reg, nullptr);
    }
  }
  LIR* dstPos_negative = nullptr;
  LIR* dst_bad_len = nullptr;
  LoadValueDirectFixed(rl_dst, rs_rAX);
  LoadWordDisp(rs_rAX, mirror::Array::LengthOffset().Int32Value(), rs_rAX);
  if (!rl_dstPos.is_const) {
    LoadValueDirectFixed(rl_dstPos, tmp_reg);
    dstPos_negative = OpCmpImmBranch(kCondLt, tmp_reg, 0, nullptr);
    OpRegRegReg(kOpAdd, tmp_reg, tmp_reg, rs_rDX);
    dst_bad_len = OpCmpBranch(kCondLt, rs_rAX, tmp_reg, nullptr);
  } else {
    int32_t pos_val = mir_graph_->ConstantValue(rl_dstPos.orig_sreg);
    if (pos_val == 0) {
      dst_bad_len = OpCmpBranch(kCondLt, rs_rAX, rs_rDX, nullptr);
    } else {
      OpRegRegImm(kOpAdd, tmp_reg,  rs_rDX, pos_val);
      dst_bad_len = OpCmpBranch(kCondLt, rs_rAX, tmp_reg, nullptr);
    }
  }
  // Everything is checked now.
  LoadValueDirectFixed(rl_src, rs_rAX);
  LoadValueDirectFixed(rl_dst, tmp_reg);
  LoadValueDirectFixed(rl_srcPos, rs_rCX);
  NewLIR5(kX86Lea32RA, rs_rAX.GetReg(), rs_rAX.GetReg(),
       rs_rCX.GetReg(), 1, mirror::Array::DataOffset(2).Int32Value());
  // RAX now holds the address of the first src element to be copied.

  LoadValueDirectFixed(rl_dstPos, rs_rCX);
  NewLIR5(kX86Lea32RA, tmp_reg.GetReg(), tmp_reg.GetReg(),
       rs_rCX.GetReg(), 1, mirror::Array::DataOffset(2).Int32Value() );
  // RBX now holds the address of the first dst element to be copied.

  // Check if the number of elements to be copied is odd or even. If odd
  // then copy the first element (so that the remaining number of elements
  // is even).
  LoadValueDirectFixed(rl_length, rs_rCX);
  OpRegImm(kOpAnd, rs_rCX, 1);
  LIR* jmp_to_begin_loop  = OpCmpImmBranch(kCondEq, rs_rCX, 0, nullptr);
  OpRegImm(kOpSub, rs_rDX, 1);
  LoadBaseIndexedDisp(rs_rAX, rs_rDX, 1, 0, rs_rCX, kSignedHalf);
  StoreBaseIndexedDisp(tmp_reg, rs_rDX, 1, 0, rs_rCX, kSignedHalf);

  // Since the remaining number of elements is even, we will copy by
  // two elements at a time.
  LIR* beginLoop = NewLIR0(kPseudoTargetLabel);
  LIR* jmp_to_ret  = OpCmpImmBranch(kCondEq, rs_rDX, 0, nullptr);
  OpRegImm(kOpSub, rs_rDX, 2);
  LoadBaseIndexedDisp(rs_rAX, rs_rDX, 1, 0, rs_rCX, kSingle);
  StoreBaseIndexedDisp(tmp_reg, rs_rDX, 1, 0, rs_rCX, kSingle);
  OpUnconditionalBranch(beginLoop);
  LIR *check_failed = NewLIR0(kPseudoTargetLabel);
  LIR* launchpad_branch  = OpUnconditionalBranch(nullptr);
  LIR *return_point = NewLIR0(kPseudoTargetLabel);
  jmp_to_ret->target = return_point;
  jmp_to_begin_loop->target = beginLoop;
  src_dst_same->target = check_failed;
  len_too_big->target = check_failed;
  src_null_branch->target = check_failed;
  if (srcPos_negative != nullptr)
    srcPos_negative ->target = check_failed;
  if (src_bad_len != nullptr)
    src_bad_len->target = check_failed;
  dst_null_branch->target = check_failed;
  if (dstPos_negative != nullptr)
    dstPos_negative->target = check_failed;
  if (dst_bad_len != nullptr)
    dst_bad_len->target = check_failed;
  AddIntrinsicSlowPath(info, launchpad_branch, return_point);
  return true;
}


/*
 * Fast string.index_of(I) & (II).  Inline check for simple case of char <= 0xffff,
 * otherwise bails to standard library code.
 */
bool X86Mir2Lir::GenInlinedIndexOf(CallInfo* info, bool zero_based) {
  RegLocation rl_obj = info->args[0];
  RegLocation rl_char = info->args[1];
  RegLocation rl_start;  // Note: only present in III flavor or IndexOf.
  // RBX is callee-save register in 64-bit mode.
  RegStorage rs_tmp = cu_->target64 ? rs_r11 : rs_rBX;
  int start_value = -1;

  uint32_t char_value =
    rl_char.is_const ? mir_graph_->ConstantValue(rl_char.orig_sreg) : 0;

  if (char_value > 0xFFFF) {
    // We have to punt to the real String.indexOf.
    return false;
  }

  // Okay, we are commited to inlining this.
  // EAX: 16 bit character being searched.
  // ECX: count: number of words to be searched.
  // EDI: String being searched.
  // EDX: temporary during execution.
  // EBX or R11: temporary during execution (depending on mode).
  // REP SCASW: search instruction.

  FlushReg(rs_rAX);
  Clobber(rs_rAX);
  LockTemp(rs_rAX);
  FlushReg(rs_rCX);
  Clobber(rs_rCX);
  LockTemp(rs_rCX);
  FlushReg(rs_rDX);
  Clobber(rs_rDX);
  LockTemp(rs_rDX);
  FlushReg(rs_tmp);
  Clobber(rs_tmp);
  LockTemp(rs_tmp);
  if (cu_->target64) {
    FlushReg(rs_rDI);
    Clobber(rs_rDI);
    LockTemp(rs_rDI);
  }

  RegLocation rl_return = GetReturn(kCoreReg);
  RegLocation rl_dest = InlineTarget(info);

  // Is the string non-NULL?
  LoadValueDirectFixed(rl_obj, rs_rDX);
  GenNullCheck(rs_rDX, info->opt_flags);
  info->opt_flags |= MIR_IGNORE_NULL_CHECK;  // Record that we've null checked.

  LIR *slowpath_branch = nullptr, *length_compare = nullptr;

  // We need the value in EAX.
  if (rl_char.is_const) {
    LoadConstantNoClobber(rs_rAX, char_value);
  } else {
    // Does the character fit in 16 bits? Compare it at runtime.
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

  // Compute the number of words to search in to rCX.
  Load32Disp(rs_rDX, count_offset, rs_rCX);

  // Possible signal here due to null pointer dereference.
  // Note that the signal handler will expect the top word of
  // the stack to be the ArtMethod*.  If the PUSH edi instruction
  // below is ahead of the load above then this will not be true
  // and the signal handler will not work.
  MarkPossibleNullPointerException(0);

  if (!cu_->target64) {
    // EDI is callee-save register in 32-bit mode.
    NewLIR1(kX86Push32R, rs_rDI.GetReg());
  }

  if (zero_based) {
    // Start index is not present.
    // We have to handle an empty string.  Use special instruction JECXZ.
    length_compare = NewLIR0(kX86Jecxz8);

    // Copy the number of words to search in a temporary register.
    // We will use the register at the end to calculate result.
    OpRegReg(kOpMov, rs_tmp, rs_rCX);
  } else {
    // Start index is present.
    rl_start = info->args[2];

    // We have to offset by the start index.
    if (rl_start.is_const) {
      start_value = mir_graph_->ConstantValue(rl_start.orig_sreg);
      start_value = std::max(start_value, 0);

      // Is the start > count?
      length_compare = OpCmpImmBranch(kCondLe, rs_rCX, start_value, nullptr);
      OpRegImm(kOpMov, rs_rDI, start_value);

      // Copy the number of words to search in a temporary register.
      // We will use the register at the end to calculate result.
      OpRegReg(kOpMov, rs_tmp, rs_rCX);

      if (start_value != 0) {
        // Decrease the number of words to search by the start index.
        OpRegImm(kOpSub, rs_rCX, start_value);
      }
    } else {
      // Handle "start index < 0" case.
      if (!cu_->target64 && rl_start.location != kLocPhysReg) {
        // Load the start index from stack, remembering that we pushed EDI.
        int displacement = SRegOffset(rl_start.s_reg_low) + sizeof(uint32_t);
        ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
        Load32Disp(rs_rX86_SP, displacement, rs_rDI);
        // Dalvik register annotation in LoadBaseIndexedDisp() used wrong offset. Fix it.
        DCHECK(!DECODE_ALIAS_INFO_WIDE(last_lir_insn_->flags.alias_info));
        int reg_id = DECODE_ALIAS_INFO_REG(last_lir_insn_->flags.alias_info) - 1;
        AnnotateDalvikRegAccess(last_lir_insn_, reg_id, true, false);
      } else {
        LoadValueDirectFixed(rl_start, rs_rDI);
      }
      OpRegReg(kOpXor, rs_tmp, rs_tmp);
      OpRegReg(kOpCmp, rs_rDI, rs_tmp);
      OpCondRegReg(kOpCmov, kCondLt, rs_rDI, rs_tmp);

      // The length of the string should be greater than the start index.
      length_compare = OpCmpBranch(kCondLe, rs_rCX, rs_rDI, nullptr);

      // Copy the number of words to search in a temporary register.
      // We will use the register at the end to calculate result.
      OpRegReg(kOpMov, rs_tmp, rs_rCX);

      // Decrease the number of words to search by the start index.
      OpRegReg(kOpSub, rs_rCX, rs_rDI);
    }
  }

  // Load the address of the string into EDI.
  // In case of start index we have to add the address to existing value in EDI.
  // The string starts at VALUE(String) + 2 * OFFSET(String) + DATA_OFFSET.
  if (zero_based || (!zero_based && rl_start.is_const && start_value == 0)) {
    Load32Disp(rs_rDX, offset_offset, rs_rDI);
  } else {
    OpRegMem(kOpAdd, rs_rDI, rs_rDX, offset_offset);
  }
  OpRegImm(kOpLsl, rs_rDI, 1);
  OpRegMem(kOpAdd, rs_rDI, rs_rDX, value_offset);
  OpRegImm(kOpAdd, rs_rDI, data_offset);

  // EDI now contains the start of the string to be searched.
  // We are all prepared to do the search for the character.
  NewLIR0(kX86RepneScasw);

  // Did we find a match?
  LIR* failed_branch = OpCondBranch(kCondNe, nullptr);

  // yes, we matched.  Compute the index of the result.
  OpRegReg(kOpSub, rs_tmp, rs_rCX);
  NewLIR3(kX86Lea32RM, rl_return.reg.GetReg(), rs_tmp.GetReg(), -1);

  LIR *all_done = NewLIR1(kX86Jmp8, 0);

  // Failed to match; return -1.
  LIR *not_found = NewLIR0(kPseudoTargetLabel);
  length_compare->target = not_found;
  failed_branch->target = not_found;
  LoadConstantNoClobber(rl_return.reg, -1);

  // And join up at the end.
  all_done->target = NewLIR0(kPseudoTargetLabel);

  if (!cu_->target64)
    NewLIR1(kX86Pop32R, rs_rDI.GetReg());

  // Out of line code returns here.
  if (slowpath_branch != nullptr) {
    LIR *return_point = NewLIR0(kPseudoTargetLabel);
    AddIntrinsicSlowPath(info, slowpath_branch, return_point);
  }

  StoreValue(rl_dest, rl_return);

  FreeTemp(rs_rAX);
  FreeTemp(rs_rCX);
  FreeTemp(rs_rDX);
  FreeTemp(rs_tmp);
  if (cu_->target64) {
    FreeTemp(rs_rDI);
  }

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


std::vector<uint8_t>* X86CFIInitialization(bool is_x86_64) {
  return X86Mir2Lir::ReturnCommonCallFrameInformation(is_x86_64);
}

static void EncodeUnsignedLeb128(std::vector<uint8_t>& buf, uint32_t value) {
  uint8_t buffer[12];
  uint8_t *ptr = EncodeUnsignedLeb128(buffer, value);
  for (uint8_t *p = buffer; p < ptr; p++) {
    buf.push_back(*p);
  }
}

static void EncodeSignedLeb128(std::vector<uint8_t>& buf, int32_t value) {
  uint8_t buffer[12];
  uint8_t *ptr = EncodeSignedLeb128(buffer, value);
  for (uint8_t *p = buffer; p < ptr; p++) {
    buf.push_back(*p);
  }
}

std::vector<uint8_t>* X86Mir2Lir::ReturnCommonCallFrameInformation(bool is_x86_64) {
  std::vector<uint8_t>*cfi_info = new std::vector<uint8_t>;

  // Length (will be filled in later in this routine).
  PushWord(*cfi_info, 0);

  // CIE id: always 0.
  PushWord(*cfi_info, 0);

  // Version: always 1.
  cfi_info->push_back(0x01);

  // Augmentation: 'zR\0'
  cfi_info->push_back(0x7a);
  cfi_info->push_back(0x52);
  cfi_info->push_back(0x0);

  // Code alignment: 1.
  EncodeUnsignedLeb128(*cfi_info, 1);

  // Data alignment.
  if (is_x86_64) {
    EncodeSignedLeb128(*cfi_info, -8);
  } else {
    EncodeSignedLeb128(*cfi_info, -4);
  }

  // Return address register.
  if (is_x86_64) {
    // R16(RIP)
    cfi_info->push_back(0x10);
  } else {
    // R8(EIP)
    cfi_info->push_back(0x08);
  }

  // Augmentation length: 1.
  cfi_info->push_back(1);

  // Augmentation data: 0x03 ((DW_EH_PE_absptr << 4) | DW_EH_PE_udata4).
  cfi_info->push_back(0x03);

  // Initial instructions.
  if (is_x86_64) {
    // DW_CFA_def_cfa R7(RSP) 8.
    cfi_info->push_back(0x0c);
    cfi_info->push_back(0x07);
    cfi_info->push_back(0x08);

    // DW_CFA_offset R16(RIP) 1 (* -8).
    cfi_info->push_back(0x90);
    cfi_info->push_back(0x01);
  } else {
    // DW_CFA_def_cfa R4(ESP) 4.
    cfi_info->push_back(0x0c);
    cfi_info->push_back(0x04);
    cfi_info->push_back(0x04);

    // DW_CFA_offset R8(EIP) 1 (* -4).
    cfi_info->push_back(0x88);
    cfi_info->push_back(0x01);
  }

  // Padding to a multiple of 4
  while ((cfi_info->size() & 3) != 0) {
    // DW_CFA_nop is encoded as 0.
    cfi_info->push_back(0);
  }

  // Set the length of the CIE inside the generated bytes.
  uint32_t length = cfi_info->size() - 4;
  (*cfi_info)[0] = length;
  (*cfi_info)[1] = length >> 8;
  (*cfi_info)[2] = length >> 16;
  (*cfi_info)[3] = length >> 24;
  return cfi_info;
}

static bool ARTRegIDToDWARFRegID(bool is_x86_64, int art_reg_id, int* dwarf_reg_id) {
  if (is_x86_64) {
    switch (art_reg_id) {
    case 3 : *dwarf_reg_id =  3; return true;  // %rbx
    // This is the only discrepancy between ART & DWARF register numbering.
    case 5 : *dwarf_reg_id =  6; return true;  // %rbp
    case 12: *dwarf_reg_id = 12; return true;  // %r12
    case 13: *dwarf_reg_id = 13; return true;  // %r13
    case 14: *dwarf_reg_id = 14; return true;  // %r14
    case 15: *dwarf_reg_id = 15; return true;  // %r15
    default: return false;  // Should not get here
    }
  } else {
    switch (art_reg_id) {
    case 5: *dwarf_reg_id = 5; return true;  // %ebp
    case 6: *dwarf_reg_id = 6; return true;  // %esi
    case 7: *dwarf_reg_id = 7; return true;  // %edi
    default: return false;  // Should not get here
    }
  }
}

std::vector<uint8_t>* X86Mir2Lir::ReturnCallFrameInformation() {
  std::vector<uint8_t>*cfi_info = new std::vector<uint8_t>;

  // Generate the FDE for the method.
  DCHECK_NE(data_offset_, 0U);

  // Length (will be filled in later in this routine).
  PushWord(*cfi_info, 0);

  // 'CIE_pointer' (filled in by linker).
  PushWord(*cfi_info, 0);

  // 'initial_location' (filled in by linker).
  PushWord(*cfi_info, 0);

  // 'address_range' (number of bytes in the method).
  PushWord(*cfi_info, data_offset_);

  // Augmentation length: 0
  cfi_info->push_back(0);

  // The instructions in the FDE.
  if (stack_decrement_ != nullptr) {
    // Advance LOC to just past the stack decrement.
    uint32_t pc = NEXT_LIR(stack_decrement_)->offset;
    AdvanceLoc(*cfi_info, pc);

    // Now update the offset to the call frame: DW_CFA_def_cfa_offset frame_size.
    cfi_info->push_back(0x0e);
    EncodeUnsignedLeb128(*cfi_info, frame_size_);

    // Handle register spills
    const uint32_t kSpillInstLen = (cu_->target64) ? 5 : 4;
    const int kDataAlignmentFactor = (cu_->target64) ? -8 : -4;
    uint32_t mask = core_spill_mask_ & ~(1 << rs_rRET.GetRegNum());
    int offset = -(GetInstructionSetPointerSize(cu_->instruction_set) * num_core_spills_);
    for (int reg = 0; mask; mask >>= 1, reg++) {
      if (mask & 0x1) {
        pc += kSpillInstLen;

        // Advance LOC to pass this instruction
        AdvanceLoc(*cfi_info, kSpillInstLen);

        int dwarf_reg_id;
        if (ARTRegIDToDWARFRegID(cu_->target64, reg, &dwarf_reg_id)) {
          // DW_CFA_offset_extended_sf reg_no offset
          cfi_info->push_back(0x11);
          EncodeUnsignedLeb128(*cfi_info, dwarf_reg_id);
          EncodeSignedLeb128(*cfi_info, offset / kDataAlignmentFactor);
        }

        offset += GetInstructionSetPointerSize(cu_->instruction_set);
      }
    }

    // We continue with that stack until the epilogue.
    if (stack_increment_ != nullptr) {
      uint32_t new_pc = NEXT_LIR(stack_increment_)->offset;
      AdvanceLoc(*cfi_info, new_pc - pc);

      // We probably have code snippets after the epilogue, so save the
      // current state: DW_CFA_remember_state.
      cfi_info->push_back(0x0a);

      // We have now popped the stack: DW_CFA_def_cfa_offset 4/8.
      // There is only the return PC on the stack now.
      cfi_info->push_back(0x0e);
      EncodeUnsignedLeb128(*cfi_info, GetInstructionSetPointerSize(cu_->instruction_set));

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
    case kMirOpReserveVectorRegisters:
      ReserveVectorRegisters(mir);
      break;
    case kMirOpReturnVectorRegisters:
      ReturnVectorRegisters();
      break;
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

void X86Mir2Lir::ReserveVectorRegisters(MIR* mir) {
  // We should not try to reserve twice without returning the registers
  DCHECK_NE(num_reserved_vector_regs_, -1);

  int num_vector_reg = mir->dalvikInsn.vA;
  for (int i = 0; i < num_vector_reg; i++) {
    RegStorage xp_reg = RegStorage::Solo128(i);
    RegisterInfo *xp_reg_info = GetRegInfo(xp_reg);
    Clobber(xp_reg);

    for (RegisterInfo *info = xp_reg_info->GetAliasChain();
                       info != nullptr;
                       info = info->GetAliasChain()) {
      if (info->GetReg().IsSingle()) {
        reg_pool_->sp_regs_.Delete(info);
      } else {
        reg_pool_->dp_regs_.Delete(info);
      }
    }
  }

  num_reserved_vector_regs_ = num_vector_reg;
}

void X86Mir2Lir::ReturnVectorRegisters() {
  // Return all the reserved registers
  for (int i = 0; i < num_reserved_vector_regs_; i++) {
    RegStorage xp_reg = RegStorage::Solo128(i);
    RegisterInfo *xp_reg_info = GetRegInfo(xp_reg);

    for (RegisterInfo *info = xp_reg_info->GetAliasChain();
                       info != nullptr;
                       info = info->GetAliasChain()) {
      if (info->GetReg().IsSingle()) {
        reg_pool_->sp_regs_.Insert(info);
      } else {
        reg_pool_->dp_regs_.Insert(info);
      }
    }
  }

  // We don't have anymore reserved vector registers
  num_reserved_vector_regs_ = -1;
}

void X86Mir2Lir::GenConst128(BasicBlock* bb, MIR* mir) {
  store_method_addr_used_ = true;
  int type_size = mir->dalvikInsn.vB;
  // We support 128 bit vectors.
  DCHECK_EQ(type_size & 0xFFFF, 128);
  RegStorage rs_dest = RegStorage::Solo128(mir->dalvikInsn.vA);
  uint32_t *args = mir->dalvikInsn.arg;
  int reg = rs_dest.GetReg();
  // Check for all 0 case.
  if (args[0] == 0 && args[1] == 0 && args[2] == 0 && args[3] == 0) {
    NewLIR2(kX86XorpsRR, reg, reg);
    return;
  }

  // Append the mov const vector to reg opcode.
  AppendOpcodeWithConst(kX86MovupsRM, reg, mir);
}

void X86Mir2Lir::AppendOpcodeWithConst(X86OpCode opcode, int reg, MIR* mir) {
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
  LIR *load = NewLIR2(opcode, reg, rl_method.reg.GetReg());
  load->flags.fixup = kFixupLoad;
  load->target = data_target;
}

void X86Mir2Lir::GenMoveVector(BasicBlock *bb, MIR *mir) {
  // We only support 128 bit registers.
  DCHECK_EQ(mir->dalvikInsn.vC & 0xFFFF, 128U);
  RegStorage rs_dest = RegStorage::Solo128(mir->dalvikInsn.vA);
  RegStorage rs_src = RegStorage::Solo128(mir->dalvikInsn.vB);
  NewLIR2(kX86Mova128RR, rs_dest.GetReg(), rs_src.GetReg());
}

void X86Mir2Lir::GenMultiplyVectorSignedByte(BasicBlock *bb, MIR *mir) {
  const int BYTE_SIZE = 8;
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vA);
  RegStorage rs_src2 = RegStorage::Solo128(mir->dalvikInsn.vB);
  RegStorage rs_src1_high_tmp = Get128BitRegister(AllocTempWide());

  /*
   * Emulate the behavior of a kSignedByte by separating out the 16 values in the two XMM
   * and multiplying 8 at a time before recombining back into one XMM register.
   *
   *   let xmm1, xmm2 be real srcs (keep low bits of 16bit lanes)
   *       xmm3 is tmp             (operate on high bits of 16bit lanes)
   *
   *    xmm3 = xmm1
   *    xmm1 = xmm1 .* xmm2
   *    xmm1 = xmm1 & 0x00ff00ff00ff00ff00ff00ff00ff00ff  // xmm1 now has low bits
   *    xmm3 = xmm3 .>> 8
   *    xmm2 = xmm2 & 0xff00ff00ff00ff00ff00ff00ff00ff00
   *    xmm2 = xmm2 .* xmm3                               // xmm2 now has high bits
   *    xmm1 = xmm1 | xmm2                                // combine results
   */

  // Copy xmm1.
  NewLIR2(kX86Mova128RR, rs_src1_high_tmp.GetReg(), rs_dest_src1.GetReg());

  // Multiply low bits.
  NewLIR2(kX86PmullwRR, rs_dest_src1.GetReg(), rs_src2.GetReg());

  // xmm1 now has low bits.
  AndMaskVectorRegister(rs_dest_src1, 0x00FF00FF, 0x00FF00FF, 0x00FF00FF, 0x00FF00FF);

  // Prepare high bits for multiplication.
  NewLIR2(kX86PsrlwRI, rs_src1_high_tmp.GetReg(), BYTE_SIZE);
  AndMaskVectorRegister(rs_src2, 0xFF00FF00, 0xFF00FF00, 0xFF00FF00, 0xFF00FF00);

  // Multiply high bits and xmm2 now has high bits.
  NewLIR2(kX86PmullwRR, rs_src2.GetReg(), rs_src1_high_tmp.GetReg());

  // Combine back into dest XMM register.
  NewLIR2(kX86PorRR, rs_dest_src1.GetReg(), rs_src2.GetReg());
}

void X86Mir2Lir::GenMultiplyVector(BasicBlock *bb, MIR *mir) {
  DCHECK_EQ(mir->dalvikInsn.vC & 0xFFFF, 128U);
  OpSize opsize = static_cast<OpSize>(mir->dalvikInsn.vC >> 16);
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vA);
  RegStorage rs_src2 = RegStorage::Solo128(mir->dalvikInsn.vB);
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
    case kSignedByte:
      // HW doesn't support 16x16 byte multiplication so emulate it.
      GenMultiplyVectorSignedByte(bb, mir);
      return;
    default:
      LOG(FATAL) << "Unsupported vector multiply " << opsize;
      break;
  }
  NewLIR2(opcode, rs_dest_src1.GetReg(), rs_src2.GetReg());
}

void X86Mir2Lir::GenAddVector(BasicBlock *bb, MIR *mir) {
  DCHECK_EQ(mir->dalvikInsn.vC & 0xFFFF, 128U);
  OpSize opsize = static_cast<OpSize>(mir->dalvikInsn.vC >> 16);
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vA);
  RegStorage rs_src2 = RegStorage::Solo128(mir->dalvikInsn.vB);
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
  DCHECK_EQ(mir->dalvikInsn.vC & 0xFFFF, 128U);
  OpSize opsize = static_cast<OpSize>(mir->dalvikInsn.vC >> 16);
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vA);
  RegStorage rs_src2 = RegStorage::Solo128(mir->dalvikInsn.vB);
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

void X86Mir2Lir::GenShiftByteVector(BasicBlock *bb, MIR *mir) {
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vA);
  RegStorage rs_tmp = Get128BitRegister(AllocTempWide());

  int opcode = 0;
  int imm = mir->dalvikInsn.vB;

  switch (static_cast<ExtendedMIROpcode>(mir->dalvikInsn.opcode)) {
    case kMirOpPackedShiftLeft:
      opcode = kX86PsllwRI;
      break;
    case kMirOpPackedSignedShiftRight:
      opcode = kX86PsrawRI;
      break;
    case kMirOpPackedUnsignedShiftRight:
      opcode = kX86PsrlwRI;
      break;
    default:
      LOG(FATAL) << "Unsupported shift operation on byte vector " << opcode;
      break;
  }

  /*
   * xmm1 will have low bits
   * xmm2 will have high bits
   *
   * xmm2 = xmm1
   * xmm1 = xmm1 .<< N
   * xmm2 = xmm2 && 0xFF00FF00FF00FF00FF00FF00FF00FF00
   * xmm2 = xmm2 .<< N
   * xmm1 = xmm1 | xmm2
   */

  // Copy xmm1.
  NewLIR2(kX86Mova128RR, rs_tmp.GetReg(), rs_dest_src1.GetReg());

  // Shift lower values.
  NewLIR2(opcode, rs_dest_src1.GetReg(), imm);

  // Mask bottom bits.
  AndMaskVectorRegister(rs_tmp, 0xFF00FF00, 0xFF00FF00, 0xFF00FF00, 0xFF00FF00);

  // Shift higher values.
  NewLIR2(opcode, rs_tmp.GetReg(), imm);

  // Combine back into dest XMM register.
  NewLIR2(kX86PorRR, rs_dest_src1.GetReg(), rs_tmp.GetReg());
}

void X86Mir2Lir::GenShiftLeftVector(BasicBlock *bb, MIR *mir) {
  DCHECK_EQ(mir->dalvikInsn.vC & 0xFFFF, 128U);
  OpSize opsize = static_cast<OpSize>(mir->dalvikInsn.vC >> 16);
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vA);
  int imm = mir->dalvikInsn.vB;
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
    case kSignedByte:
    case kUnsignedByte:
      GenShiftByteVector(bb, mir);
      return;
    default:
      LOG(FATAL) << "Unsupported vector shift left " << opsize;
      break;
  }
  NewLIR2(opcode, rs_dest_src1.GetReg(), imm);
}

void X86Mir2Lir::GenSignedShiftRightVector(BasicBlock *bb, MIR *mir) {
  DCHECK_EQ(mir->dalvikInsn.vC & 0xFFFF, 128U);
  OpSize opsize = static_cast<OpSize>(mir->dalvikInsn.vC >> 16);
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vA);
  int imm = mir->dalvikInsn.vB;
  int opcode = 0;
  switch (opsize) {
    case k32:
      opcode = kX86PsradRI;
      break;
    case kSignedHalf:
    case kUnsignedHalf:
      opcode = kX86PsrawRI;
      break;
    case kSignedByte:
    case kUnsignedByte:
      GenShiftByteVector(bb, mir);
      return;
    default:
      LOG(FATAL) << "Unsupported vector signed shift right " << opsize;
      break;
  }
  NewLIR2(opcode, rs_dest_src1.GetReg(), imm);
}

void X86Mir2Lir::GenUnsignedShiftRightVector(BasicBlock *bb, MIR *mir) {
  DCHECK_EQ(mir->dalvikInsn.vC & 0xFFFF, 128U);
  OpSize opsize = static_cast<OpSize>(mir->dalvikInsn.vC >> 16);
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vA);
  int imm = mir->dalvikInsn.vB;
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
    case kSignedByte:
    case kUnsignedByte:
      GenShiftByteVector(bb, mir);
      return;
    default:
      LOG(FATAL) << "Unsupported vector unsigned shift right " << opsize;
      break;
  }
  NewLIR2(opcode, rs_dest_src1.GetReg(), imm);
}

void X86Mir2Lir::GenAndVector(BasicBlock *bb, MIR *mir) {
  // We only support 128 bit registers.
  DCHECK_EQ(mir->dalvikInsn.vC & 0xFFFF, 128U);
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vA);
  RegStorage rs_src2 = RegStorage::Solo128(mir->dalvikInsn.vB);
  NewLIR2(kX86PandRR, rs_dest_src1.GetReg(), rs_src2.GetReg());
}

void X86Mir2Lir::GenOrVector(BasicBlock *bb, MIR *mir) {
  // We only support 128 bit registers.
  DCHECK_EQ(mir->dalvikInsn.vC & 0xFFFF, 128U);
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vA);
  RegStorage rs_src2 = RegStorage::Solo128(mir->dalvikInsn.vB);
  NewLIR2(kX86PorRR, rs_dest_src1.GetReg(), rs_src2.GetReg());
}

void X86Mir2Lir::GenXorVector(BasicBlock *bb, MIR *mir) {
  // We only support 128 bit registers.
  DCHECK_EQ(mir->dalvikInsn.vC & 0xFFFF, 128U);
  RegStorage rs_dest_src1 = RegStorage::Solo128(mir->dalvikInsn.vA);
  RegStorage rs_src2 = RegStorage::Solo128(mir->dalvikInsn.vB);
  NewLIR2(kX86PxorRR, rs_dest_src1.GetReg(), rs_src2.GetReg());
}

void X86Mir2Lir::AndMaskVectorRegister(RegStorage rs_src1, uint32_t m1, uint32_t m2, uint32_t m3, uint32_t m4) {
  MaskVectorRegister(kX86PandRM, rs_src1, m1, m2, m3, m4);
}

void X86Mir2Lir::MaskVectorRegister(X86OpCode opcode, RegStorage rs_src1, uint32_t m0, uint32_t m1, uint32_t m2, uint32_t m3) {
  // Create temporary MIR as container for 128-bit binary mask.
  MIR const_mir;
  MIR* const_mirp = &const_mir;
  const_mirp->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpConstVector);
  const_mirp->dalvikInsn.arg[0] = m0;
  const_mirp->dalvikInsn.arg[1] = m1;
  const_mirp->dalvikInsn.arg[2] = m2;
  const_mirp->dalvikInsn.arg[3] = m3;

  // Mask vector with const from literal pool.
  AppendOpcodeWithConst(opcode, rs_src1.GetReg(), const_mirp);
}

void X86Mir2Lir::GenAddReduceVector(BasicBlock *bb, MIR *mir) {
  OpSize opsize = static_cast<OpSize>(mir->dalvikInsn.vC >> 16);
  RegStorage rs_src1 = RegStorage::Solo128(mir->dalvikInsn.vB);
  RegLocation rl_dest = mir_graph_->GetDest(mir);
  RegStorage rs_tmp;

  int vec_bytes = (mir->dalvikInsn.vC & 0xFFFF) / 8;
  int vec_unit_size = 0;
  int opcode = 0;
  int extr_opcode = 0;
  RegLocation rl_result;

  switch (opsize) {
    case k32:
      extr_opcode = kX86PextrdRRI;
      opcode = kX86PhadddRR;
      vec_unit_size = 4;
      break;
    case kSignedByte:
    case kUnsignedByte:
      extr_opcode = kX86PextrbRRI;
      opcode = kX86PhaddwRR;
      vec_unit_size = 2;
      break;
    case kSignedHalf:
    case kUnsignedHalf:
      extr_opcode = kX86PextrwRRI;
      opcode = kX86PhaddwRR;
      vec_unit_size = 2;
      break;
    case kSingle:
      rl_result = EvalLoc(rl_dest, kFPReg, true);
      vec_unit_size = 4;
      for (int i = 0; i < 3; i++) {
        NewLIR2(kX86AddssRR, rl_result.reg.GetReg(), rs_src1.GetReg());
        NewLIR3(kX86ShufpsRRI, rs_src1.GetReg(), rs_src1.GetReg(), 0x39);
      }
      NewLIR2(kX86AddssRR, rl_result.reg.GetReg(), rs_src1.GetReg());
      StoreValue(rl_dest, rl_result);

      // For single-precision floats, we are done here
      return;
    default:
      LOG(FATAL) << "Unsupported vector add reduce " << opsize;
      break;
  }

  int elems = vec_bytes / vec_unit_size;

  // Emulate horizontal add instruction by reducing 2 vectors with 8 values before adding them again
  // TODO is overflow handled correctly?
  if (opsize == kSignedByte || opsize == kUnsignedByte) {
    rs_tmp = Get128BitRegister(AllocTempWide());

    // tmp = xmm1 .>> 8.
    NewLIR2(kX86Mova128RR, rs_tmp.GetReg(), rs_src1.GetReg());
    NewLIR2(kX86PsrlwRI, rs_tmp.GetReg(), 8);

    // Zero extend low bits in xmm1.
    AndMaskVectorRegister(rs_src1, 0x00FF00FF, 0x00FF00FF, 0x00FF00FF, 0x00FF00FF);
  }

  while (elems > 1) {
    if (opsize == kSignedByte || opsize == kUnsignedByte) {
      NewLIR2(opcode, rs_tmp.GetReg(), rs_tmp.GetReg());
    }
    NewLIR2(opcode, rs_src1.GetReg(), rs_src1.GetReg());
    elems >>= 1;
  }

  // Combine the results if we separated them.
  if (opsize == kSignedByte || opsize == kUnsignedByte) {
    NewLIR2(kX86PaddbRR, rs_src1.GetReg(), rs_tmp.GetReg());
  }

  // We need to extract to a GPR.
  RegStorage temp = AllocTemp();
  NewLIR3(extr_opcode, temp.GetReg(), rs_src1.GetReg(), 0);

  // Can we do this directly into memory?
  rl_result = UpdateLocTyped(rl_dest, kCoreReg);
  if (rl_result.location == kLocPhysReg) {
    // Ensure res is in a core reg
    rl_result = EvalLoc(rl_dest, kCoreReg, true);
    OpRegReg(kOpAdd, rl_result.reg, temp);
    StoreFinalValue(rl_dest, rl_result);
  } else {
    OpMemReg(kOpAdd, rl_result, temp.GetReg());
  }

  FreeTemp(temp);
}

void X86Mir2Lir::GenReduceVector(BasicBlock *bb, MIR *mir) {
  OpSize opsize = static_cast<OpSize>(mir->dalvikInsn.vC >> 16);
  RegLocation rl_dest = mir_graph_->GetDest(mir);
  RegStorage rs_src1 = RegStorage::Solo128(mir->dalvikInsn.vB);
  int extract_index = mir->dalvikInsn.arg[0];
  int extr_opcode = 0;
  RegLocation rl_result;
  bool is_wide = false;

  switch (opsize) {
    case k32:
      rl_result = UpdateLocTyped(rl_dest, kCoreReg);
      extr_opcode = (rl_result.location == kLocPhysReg) ? kX86PextrdMRI : kX86PextrdRRI;
      break;
    case kSignedHalf:
    case kUnsignedHalf:
      rl_result= UpdateLocTyped(rl_dest, kCoreReg);
      extr_opcode = (rl_result.location == kLocPhysReg) ? kX86PextrwMRI : kX86PextrwRRI;
      break;
    default:
      LOG(FATAL) << "Unsupported vector add reduce " << opsize;
      return;
      break;
  }

  if (rl_result.location == kLocPhysReg) {
    NewLIR3(extr_opcode, rl_result.reg.GetReg(), rs_src1.GetReg(), extract_index);
    if (is_wide == true) {
      StoreFinalValue(rl_dest, rl_result);
    } else {
      StoreFinalValueWide(rl_dest, rl_result);
    }
  } else {
    int displacement = SRegOffset(rl_result.s_reg_low);
    LIR *l = NewLIR3(extr_opcode, rs_rX86_SP.GetReg(), displacement, rs_src1.GetReg());
    AnnotateDalvikRegAccess(l, displacement >> 2, true /* is_load */, is_wide /* is_64bit */);
    AnnotateDalvikRegAccess(l, displacement >> 2, false /* is_load */, is_wide /* is_64bit */);
  }
}

void X86Mir2Lir::GenSetVector(BasicBlock *bb, MIR *mir) {
  DCHECK_EQ(mir->dalvikInsn.vC & 0xFFFF, 128U);
  OpSize opsize = static_cast<OpSize>(mir->dalvikInsn.vC >> 16);
  RegStorage rs_dest = RegStorage::Solo128(mir->dalvikInsn.vA);
  int op_low = 0, op_high = 0, imm = 0, op_mov = kX86MovdxrRR;
  RegisterClass reg_type = kCoreReg;

  switch (opsize) {
    case k32:
      op_low = kX86PshufdRRI;
      break;
    case kSingle:
      op_low = kX86PshufdRRI;
      op_mov = kX86Mova128RR;
      reg_type = kFPReg;
      break;
    case k64:
      op_low = kX86PshufdRRI;
      imm = 0x44;
      break;
    case kDouble:
      op_low = kX86PshufdRRI;
      op_mov = kX86Mova128RR;
      reg_type = kFPReg;
      imm = 0x44;
      break;
    case kSignedByte:
    case kUnsignedByte:
      // Shuffle 8 bit value into 16 bit word.
      // We set val = val + (val << 8) below and use 16 bit shuffle.
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

  RegLocation rl_src = mir_graph_->GetSrc(mir, 0);

  // Load the value from the VR into the reg.
  if (rl_src.wide == 0) {
    rl_src = LoadValue(rl_src, reg_type);
  } else {
    rl_src = LoadValueWide(rl_src, reg_type);
  }

  // If opsize is 8 bits wide then double value and use 16 bit shuffle instead.
  if (opsize == kSignedByte || opsize == kUnsignedByte) {
    RegStorage temp = AllocTemp();
    // val = val + (val << 8).
    NewLIR2(kX86Mov32RR, temp.GetReg(), rl_src.reg.GetReg());
    NewLIR2(kX86Sal32RI, temp.GetReg(), 8);
    NewLIR2(kX86Or32RR, rl_src.reg.GetReg(), temp.GetReg());
    FreeTemp(temp);
  }

  // Load the value into the XMM register.
  NewLIR2(op_mov, rs_dest.GetReg(), rl_src.reg.GetReg());

  // Now shuffle the value across the destination.
  NewLIR3(op_low, rs_dest.GetReg(), rs_dest.GetReg(), imm);

  // And then repeat as needed.
  if (op_high != 0) {
    NewLIR3(op_high, rs_dest.GetReg(), rs_dest.GetReg(), imm);
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
RegStorage X86Mir2Lir::InToRegStorageX86_64Mapper::GetNextReg(bool is_double_or_float, bool is_wide,
                                                              bool is_ref) {
  const SpecialTargetRegister coreArgMappingToPhysicalReg[] = {kArg1, kArg2, kArg3, kArg4, kArg5};
  const int coreArgMappingToPhysicalRegSize = sizeof(coreArgMappingToPhysicalReg) /
      sizeof(SpecialTargetRegister);
  const SpecialTargetRegister fpArgMappingToPhysicalReg[] = {kFArg0, kFArg1, kFArg2, kFArg3,
                                                             kFArg4, kFArg5, kFArg6, kFArg7};
  const int fpArgMappingToPhysicalRegSize = sizeof(fpArgMappingToPhysicalReg) /
      sizeof(SpecialTargetRegister);

  if (is_double_or_float) {
    if (cur_fp_reg_ < fpArgMappingToPhysicalRegSize) {
      return ml_->TargetReg(fpArgMappingToPhysicalReg[cur_fp_reg_++], is_wide ? kWide : kNotWide);
    }
  } else {
    if (cur_core_reg_ < coreArgMappingToPhysicalRegSize) {
      return ml_->TargetReg(coreArgMappingToPhysicalReg[cur_core_reg_++],
                            is_ref ? kRef : (is_wide ? kWide : kNotWide));
    }
  }
  return RegStorage::InvalidReg();
}

RegStorage X86Mir2Lir::InToRegStorageMapping::Get(int in_position) {
  DCHECK(IsInitialized());
  auto res = mapping_.find(in_position);
  return res != mapping_.end() ? res->second : RegStorage::InvalidReg();
}

void X86Mir2Lir::InToRegStorageMapping::Initialize(RegLocation* arg_locs, int count,
                                                   InToRegStorageMapper* mapper) {
  DCHECK(mapper != nullptr);
  max_mapped_in_ = -1;
  is_there_stack_mapped_ = false;
  for (int in_position = 0; in_position < count; in_position++) {
     RegStorage reg = mapper->GetNextReg(arg_locs[in_position].fp,
             arg_locs[in_position].wide, arg_locs[in_position].ref);
     if (reg.Valid()) {
       mapping_[in_position] = reg;
       max_mapped_in_ = std::max(max_mapped_in_, in_position);
       if (arg_locs[in_position].wide) {
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
  if (!cu_->target64) {
    return GetCoreArgMappingToPhysicalReg(arg_num);
  }

  if (!in_to_reg_storage_mapping_.IsInitialized()) {
    int start_vreg = cu_->num_dalvik_registers - cu_->num_ins;
    RegLocation* arg_locs = &mir_graph_->reg_location_[start_vreg];

    InToRegStorageX86_64Mapper mapper(this);
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
  if (!cu_->target64) return Mir2Lir::FlushIns(ArgLocs, rl_method);
  /*
   * Dummy up a RegLocation for the incoming Method*
   * It will attempt to keep kArg0 live (or copy it to home location
   * if promoted).
   */

  RegLocation rl_src = rl_method;
  rl_src.location = kLocPhysReg;
  rl_src.reg = TargetReg(kArg0, kRef);
  rl_src.home = false;
  MarkLive(rl_src);
  StoreValue(rl_method, rl_src);
  // If Method* has been promoted, explicitly flush
  if (rl_method.location == kLocPhysReg) {
    StoreRefDisp(rs_rX86_SP, 0, As32BitReg(TargetReg(kArg0, kRef)), kNotVolatile);
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
    // get reg corresponding to input
    RegStorage reg = GetArgMappingToPhysicalReg(i);

    RegLocation* t_loc = &ArgLocs[i];
    if (reg.Valid()) {
      // If arriving in register.

      // We have already updated the arg location with promoted info
      // so we can be based on it.
      if (t_loc->location == kLocPhysReg) {
        // Just copy it.
        OpRegCopy(t_loc->reg, reg);
      } else {
        // Needs flush.
        if (t_loc->ref) {
          StoreRefDisp(rs_rX86_SP, SRegOffset(start_vreg + i), reg, kNotVolatile);
        } else {
          StoreBaseDisp(rs_rX86_SP, SRegOffset(start_vreg + i), reg, t_loc->wide ? k64 : k32,
                        kNotVolatile);
        }
      }
    } else {
      // If arriving in frame & promoted.
      if (t_loc->location == kLocPhysReg) {
        if (t_loc->ref) {
          LoadRefDisp(rs_rX86_SP, SRegOffset(start_vreg + i), t_loc->reg, kNotVolatile);
        } else {
          LoadBaseDisp(rs_rX86_SP, SRegOffset(start_vreg + i), t_loc->reg,
                       t_loc->wide ? k64 : k32, kNotVolatile);
        }
      }
    }
    if (t_loc->wide) {
      // Increment i to skip the next one.
      i++;
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
  if (!cu_->target64) {
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
  if (!cu_->target64) {
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

  InToRegStorageX86_64Mapper mapper(this);
  InToRegStorageMapping in_to_reg_storage_mapping;
  in_to_reg_storage_mapping.Initialize(info->args, info->num_arg_words, &mapper);
  const int last_mapped_in = in_to_reg_storage_mapping.GetMaxMappedIn();
  const int size_of_the_last_mapped = last_mapped_in == -1 ? 1 :
          info->args[last_mapped_in].wide ? 2 : 1;
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
          StoreBaseDisp(rs_rX86_SP, SRegOffset(loc.s_reg_low), loc.reg, k64, kNotVolatile);
        }
        next_arg += 2;
      } else {
        loc = UpdateLoc(loc);
        if (loc.location == kLocPhysReg) {
          ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
          StoreBaseDisp(rs_rX86_SP, SRegOffset(loc.s_reg_low), loc.reg, k32, kNotVolatile);
        }
        next_arg++;
      }
    }

    // Logic below assumes that Method pointer is at offset zero from SP.
    DCHECK_EQ(VRegOffset(static_cast<int>(kVRegMethodPtrBaseReg)), 0);

    // The rest can be copied together
    int start_offset = SRegOffset(info->args[last_mapped_in + size_of_the_last_mapped].s_reg_low);
    int outs_offset = StackVisitor::GetOutVROffset(last_mapped_in + size_of_the_last_mapped,
                                                   cu_->instruction_set);

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
          ld1 = OpMovRegMem(temp, rs_rX86_SP, current_src_offset, kMovA128FP);
        } else if (src_is_8b_aligned) {
          ld1 = OpMovRegMem(temp, rs_rX86_SP, current_src_offset, kMovLo128FP);
          ld2 = OpMovRegMem(temp, rs_rX86_SP, current_src_offset + (bytes_to_move >> 1),
                            kMovHi128FP);
        } else {
          ld1 = OpMovRegMem(temp, rs_rX86_SP, current_src_offset, kMovU128FP);
        }

        if (dest_is_16b_aligned) {
          st1 = OpMovMemReg(rs_rX86_SP, current_dest_offset, temp, kMovA128FP);
        } else if (dest_is_8b_aligned) {
          st1 = OpMovMemReg(rs_rX86_SP, current_dest_offset, temp, kMovLo128FP);
          st2 = OpMovMemReg(rs_rX86_SP, current_dest_offset + (bytes_to_move >> 1),
                            temp, kMovHi128FP);
        } else {
          st1 = OpMovMemReg(rs_rX86_SP, current_dest_offset, temp, kMovU128FP);
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
        RegStorage temp = TargetReg(kArg3, kNotWide);

        // Now load the argument VR and store to the outs.
        Load32Disp(rs_rX86_SP, current_src_offset, temp);
        Store32Disp(rs_rX86_SP, current_dest_offset, temp);
      }

      current_src_offset += bytes_to_move;
      current_dest_offset += bytes_to_move;
      regs_left_to_pass_via_stack -= (bytes_to_move >> 2);
    }
    DCHECK_EQ(regs_left_to_pass_via_stack, 0);
  }

  // Now handle rest not registers if they are
  if (in_to_reg_storage_mapping.IsThereStackMapped()) {
    RegStorage regSingle = TargetReg(kArg2, kNotWide);
    RegStorage regWide = TargetReg(kArg3, kWide);
    for (int i = start_index;
         i < last_mapped_in + size_of_the_last_mapped + regs_left_to_pass_via_stack; i++) {
      RegLocation rl_arg = info->args[i];
      rl_arg = UpdateRawLoc(rl_arg);
      RegStorage reg = in_to_reg_storage_mapping.Get(i);
      if (!reg.Valid()) {
        int out_offset = StackVisitor::GetOutVROffset(i, cu_->instruction_set);

        {
          ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
          if (rl_arg.wide) {
            if (rl_arg.location == kLocPhysReg) {
              StoreBaseDisp(rs_rX86_SP, out_offset, rl_arg.reg, k64, kNotVolatile);
            } else {
              LoadValueDirectWideFixed(rl_arg, regWide);
              StoreBaseDisp(rs_rX86_SP, out_offset, regWide, k64, kNotVolatile);
            }
          } else {
            if (rl_arg.location == kLocPhysReg) {
              StoreBaseDisp(rs_rX86_SP, out_offset, rl_arg.reg, k32, kNotVolatile);
            } else {
              LoadValueDirectFixed(rl_arg, regSingle);
              StoreBaseDisp(rs_rX86_SP, out_offset, regSingle, k32, kNotVolatile);
            }
          }
        }
        call_state = next_call_insn(cu_, info, call_state, target_method,
                                    vtable_idx, direct_code, direct_method, type);
      }
      if (rl_arg.wide) {
        i++;
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
      } else {
        LoadValueDirectFixed(rl_arg, reg);
      }
      call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                               direct_code, direct_method, type);
    }
    if (rl_arg.wide) {
      i++;
    }
  }

  call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                           direct_code, direct_method, type);
  if (pcrLabel) {
    if (!cu_->compiler_driver->GetCompilerOptions().GetImplicitNullChecks()) {
      *pcrLabel = GenExplicitNullCheck(TargetReg(kArg1, kRef), info->opt_flags);
    } else {
      *pcrLabel = nullptr;
      // In lieu of generating a check for kArg1 being null, we need to
      // perform a load when doing implicit checks.
      RegStorage tmp = AllocTemp();
      Load32Disp(TargetReg(kArg1, kRef), 0, tmp);
      MarkPossibleNullPointerException(info->opt_flags);
      FreeTemp(tmp);
    }
  }
  return call_state;
}

bool X86Mir2Lir::GenInlinedCharAt(CallInfo* info) {
  // Location of reference to data array
  int value_offset = mirror::String::ValueOffset().Int32Value();
  // Location of count
  int count_offset = mirror::String::CountOffset().Int32Value();
  // Starting offset within data array
  int offset_offset = mirror::String::OffsetOffset().Int32Value();
  // Start of char data with array_
  int data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Int32Value();

  RegLocation rl_obj = info->args[0];
  RegLocation rl_idx = info->args[1];
  rl_obj = LoadValue(rl_obj, kRefReg);
  // X86 wants to avoid putting a constant index into a register.
  if (!rl_idx.is_const) {
    rl_idx = LoadValue(rl_idx, kCoreReg);
  }
  RegStorage reg_max;
  GenNullCheck(rl_obj.reg, info->opt_flags);
  bool range_check = (!(info->opt_flags & MIR_IGNORE_RANGE_CHECK));
  LIR* range_check_branch = nullptr;
  RegStorage reg_off;
  RegStorage reg_ptr;
  if (range_check) {
    // On x86, we can compare to memory directly
    // Set up a launch pad to allow retry in case of bounds violation */
    if (rl_idx.is_const) {
      LIR* comparison;
      range_check_branch = OpCmpMemImmBranch(
          kCondUlt, RegStorage::InvalidReg(), rl_obj.reg, count_offset,
          mir_graph_->ConstantValue(rl_idx.orig_sreg), nullptr, &comparison);
      MarkPossibleNullPointerExceptionAfter(0, comparison);
    } else {
      OpRegMem(kOpCmp, rl_idx.reg, rl_obj.reg, count_offset);
      MarkPossibleNullPointerException(0);
      range_check_branch = OpCondBranch(kCondUge, nullptr);
    }
  }
  reg_off = AllocTemp();
  reg_ptr = AllocTempRef();
  Load32Disp(rl_obj.reg, offset_offset, reg_off);
  LoadRefDisp(rl_obj.reg, value_offset, reg_ptr, kNotVolatile);
  if (rl_idx.is_const) {
    OpRegImm(kOpAdd, reg_off, mir_graph_->ConstantValue(rl_idx.orig_sreg));
  } else {
    OpRegReg(kOpAdd, reg_off, rl_idx.reg);
  }
  FreeTemp(rl_obj.reg);
  if (rl_idx.location == kLocPhysReg) {
    FreeTemp(rl_idx.reg);
  }
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  LoadBaseIndexedDisp(reg_ptr, reg_off, 1, data_offset, rl_result.reg, kUnsignedHalf);
  FreeTemp(reg_off);
  FreeTemp(reg_ptr);
  StoreValue(rl_dest, rl_result);
  if (range_check) {
    DCHECK(range_check_branch != nullptr);
    info->opt_flags |= MIR_IGNORE_NULL_CHECK;  // Record that we've already null checked.
    AddIntrinsicSlowPath(info, range_check_branch);
  }
  return true;
}

bool X86Mir2Lir::GenInlinedCurrentThread(CallInfo* info) {
  RegLocation rl_dest = InlineTarget(info);

  // Early exit if the result is unused.
  if (rl_dest.orig_sreg < 0) {
    return true;
  }

  RegLocation rl_result = EvalLoc(rl_dest, kRefReg, true);

  if (cu_->target64) {
    OpRegThreadMem(kOpMov, rl_result.reg, Thread::PeerOffset<8>());
  } else {
    OpRegThreadMem(kOpMov, rl_result.reg, Thread::PeerOffset<4>());
  }

  StoreValue(rl_dest, rl_result);
  return true;
}

}  // namespace art
