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

static const RegStorage core_regs_arr_32[] = {
    rs_rAX, rs_rCX, rs_rDX, rs_rBX, rs_rX86_SP_32, rs_rBP, rs_rSI, rs_rDI,
};
static const RegStorage core_regs_arr_64[] = {
    rs_rAX, rs_rCX, rs_rDX, rs_rBX, rs_rX86_SP_64, rs_rBP, rs_rSI, rs_rDI,
#ifdef TARGET_REX_SUPPORT
    rs_r8, rs_r9, rs_r10, rs_r11, rs_r12, rs_r13, rs_r14, rs_r15
#endif
};
static const RegStorage sp_regs_arr_32[] = {
    rs_fr0, rs_fr1, rs_fr2, rs_fr3, rs_fr4, rs_fr5, rs_fr6, rs_fr7,
};
static const RegStorage sp_regs_arr_64[] = {
    rs_fr0, rs_fr1, rs_fr2, rs_fr3, rs_fr4, rs_fr5, rs_fr6, rs_fr7,
#ifdef TARGET_REX_SUPPORT
    rs_fr8, rs_fr9, rs_fr10, rs_fr11, rs_fr12, rs_fr13, rs_fr14, rs_fr15
#endif
};
static const RegStorage dp_regs_arr_32[] = {
    rs_dr0, rs_dr1, rs_dr2, rs_dr3, rs_dr4, rs_dr5, rs_dr6, rs_dr7,
};
static const RegStorage dp_regs_arr_64[] = {
    rs_dr0, rs_dr1, rs_dr2, rs_dr3, rs_dr4, rs_dr5, rs_dr6, rs_dr7,
#ifdef TARGET_REX_SUPPORT
    rs_dr8, rs_dr9, rs_dr10, rs_dr11, rs_dr12, rs_dr13, rs_dr14, rs_dr15
#endif
};
static const RegStorage reserved_regs_arr_32[] = {rs_rX86_SP_32};
static const RegStorage reserved_regs_arr_64[] = {rs_rX86_SP_64};
static const RegStorage core_temps_arr_32[] = {rs_rAX, rs_rCX, rs_rDX, rs_rBX};
static const RegStorage core_temps_arr_64[] = {
    rs_rAX, rs_rCX, rs_rDX, rs_rSI, rs_rDI,
#ifdef TARGET_REX_SUPPORT
    rs_r8, rs_r9, rs_r10, rs_r11
#endif
};
static const RegStorage sp_temps_arr_32[] = {
    rs_fr0, rs_fr1, rs_fr2, rs_fr3, rs_fr4, rs_fr5, rs_fr6, rs_fr7,
};
static const RegStorage sp_temps_arr_64[] = {
    rs_fr0, rs_fr1, rs_fr2, rs_fr3, rs_fr4, rs_fr5, rs_fr6, rs_fr7,
#ifdef TARGET_REX_SUPPORT
    rs_fr8, rs_fr9, rs_fr10, rs_fr11, rs_fr12, rs_fr13, rs_fr14, rs_fr15
#endif
};
static const RegStorage dp_temps_arr_32[] = {
    rs_dr0, rs_dr1, rs_dr2, rs_dr3, rs_dr4, rs_dr5, rs_dr6, rs_dr7,
};
static const RegStorage dp_temps_arr_64[] = {
    rs_dr0, rs_dr1, rs_dr2, rs_dr3, rs_dr4, rs_dr5, rs_dr6, rs_dr7,
#ifdef TARGET_REX_SUPPORT
    rs_dr8, rs_dr9, rs_dr10, rs_dr11, rs_dr12, rs_dr13, rs_dr14, rs_dr15
#endif
};

static const std::vector<RegStorage> core_regs_32(core_regs_arr_32,
    core_regs_arr_32 + sizeof(core_regs_arr_32) / sizeof(core_regs_arr_32[0]));
static const std::vector<RegStorage> core_regs_64(core_regs_arr_64,
    core_regs_arr_64 + sizeof(core_regs_arr_64) / sizeof(core_regs_arr_64[0]));
static const std::vector<RegStorage> sp_regs_32(sp_regs_arr_32,
    sp_regs_arr_32 + sizeof(sp_regs_arr_32) / sizeof(sp_regs_arr_32[0]));
static const std::vector<RegStorage> sp_regs_64(sp_regs_arr_64,
    sp_regs_arr_64 + sizeof(sp_regs_arr_64) / sizeof(sp_regs_arr_64[0]));
static const std::vector<RegStorage> dp_regs_32(dp_regs_arr_32,
    dp_regs_arr_32 + sizeof(dp_regs_arr_32) / sizeof(dp_regs_arr_32[0]));
static const std::vector<RegStorage> dp_regs_64(dp_regs_arr_64,
    dp_regs_arr_64 + sizeof(dp_regs_arr_64) / sizeof(dp_regs_arr_64[0]));
static const std::vector<RegStorage> reserved_regs_32(reserved_regs_arr_32,
    reserved_regs_arr_32 + sizeof(reserved_regs_arr_32) / sizeof(reserved_regs_arr_32[0]));
static const std::vector<RegStorage> reserved_regs_64(reserved_regs_arr_64,
    reserved_regs_arr_64 + sizeof(reserved_regs_arr_64) / sizeof(reserved_regs_arr_64[0]));
static const std::vector<RegStorage> core_temps_32(core_temps_arr_32,
    core_temps_arr_32 + sizeof(core_temps_arr_32) / sizeof(core_temps_arr_32[0]));
static const std::vector<RegStorage> core_temps_64(core_temps_arr_64,
    core_temps_arr_64 + sizeof(core_temps_arr_64) / sizeof(core_temps_arr_64[0]));
static const std::vector<RegStorage> sp_temps_32(sp_temps_arr_32,
    sp_temps_arr_32 + sizeof(sp_temps_arr_32) / sizeof(sp_temps_arr_32[0]));
static const std::vector<RegStorage> sp_temps_64(sp_temps_arr_64,
    sp_temps_arr_64 + sizeof(sp_temps_arr_64) / sizeof(sp_temps_arr_64[0]));
static const std::vector<RegStorage> dp_temps_32(dp_temps_arr_32,
    dp_temps_arr_32 + sizeof(dp_temps_arr_32) / sizeof(dp_temps_arr_32[0]));
static const std::vector<RegStorage> dp_temps_64(dp_temps_arr_64,
    dp_temps_arr_64 + sizeof(dp_temps_arr_64) / sizeof(dp_temps_arr_64[0]));

RegStorage rs_rX86_SP;

X86NativeRegisterPool rX86_ARG0;
X86NativeRegisterPool rX86_ARG1;
X86NativeRegisterPool rX86_ARG2;
X86NativeRegisterPool rX86_ARG3;
X86NativeRegisterPool rX86_FARG0;
X86NativeRegisterPool rX86_FARG1;
X86NativeRegisterPool rX86_FARG2;
X86NativeRegisterPool rX86_FARG3;
X86NativeRegisterPool rX86_RET0;
X86NativeRegisterPool rX86_RET1;
X86NativeRegisterPool rX86_INVOKE_TGT;
X86NativeRegisterPool rX86_COUNT;

RegStorage rs_rX86_ARG0;
RegStorage rs_rX86_ARG1;
RegStorage rs_rX86_ARG2;
RegStorage rs_rX86_ARG3;
RegStorage rs_rX86_FARG0;
RegStorage rs_rX86_FARG1;
RegStorage rs_rX86_FARG2;
RegStorage rs_rX86_FARG3;
RegStorage rs_rX86_RET0;
RegStorage rs_rX86_RET1;
RegStorage rs_rX86_INVOKE_TGT;
RegStorage rs_rX86_COUNT;

RegLocation X86Mir2Lir::LocCReturn() {
  return x86_loc_c_return;
}

RegLocation X86Mir2Lir::LocCReturnWide() {
  return x86_loc_c_return_wide;
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
    case kFArg0: res_reg = rs_rX86_FARG0; break;
    case kFArg1: res_reg = rs_rX86_FARG1; break;
    case kFArg2: res_reg = rs_rX86_FARG2; break;
    case kFArg3: res_reg = rs_rX86_FARG3; break;
    case kRet0: res_reg = rs_rX86_RET0; break;
    case kRet1: res_reg = rs_rX86_RET1; break;
    case kInvokeTgt: res_reg = rs_rX86_INVOKE_TGT; break;
    case kHiddenArg: res_reg = rs_rAX; break;
    case kHiddenFpArg: res_reg = rs_fr0; break;
    case kCount: res_reg = rs_rX86_COUNT; break;
  }
  return res_reg;
}

RegStorage X86Mir2Lir::GetArgMappingToPhysicalReg(int arg_num) {
  // For the 32-bit internal ABI, the first 3 arguments are passed in registers.
  // TODO: This is not 64-bit compliant and depends on new internal ABI.
  switch (arg_num) {
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

/*
 * Decode the register id.
 */
uint64_t X86Mir2Lir::GetRegMaskCommon(RegStorage reg) {
  uint64_t seed;
  int shift;
  int reg_id;

  reg_id = reg.GetRegNum();
  /* Double registers in x86 are just a single FP register */
  seed = 1;
  /* FP register starts at bit position 16 */
  shift = reg.IsFloat() ? kX86FPReg0 : 0;
  /* Expand the double register id into single offset */
  shift += reg_id;
  return (seed << shift);
}

uint64_t X86Mir2Lir::GetPCUseDefEncoding() {
  /*
   * FIXME: might make sense to use a virtual resource encoding bit for pc.  Might be
   * able to clean up some of the x86/Arm_Mips differences
   */
  LOG(FATAL) << "Unexpected call to GetPCUseDefEncoding for x86";
  return 0ULL;
}

void X86Mir2Lir::SetupTargetResourceMasks(LIR* lir, uint64_t flags) {
  DCHECK(cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64);
  DCHECK(!lir->flags.use_def_invalid);

  // X86-specific resource map setup here.
  if (flags & REG_USE_SP) {
    lir->u.m.use_mask |= ENCODE_X86_REG_SP;
  }

  if (flags & REG_DEF_SP) {
    lir->u.m.def_mask |= ENCODE_X86_REG_SP;
  }

  if (flags & REG_DEFA) {
    SetupRegMask(&lir->u.m.def_mask, rs_rAX.GetReg());
  }

  if (flags & REG_DEFD) {
    SetupRegMask(&lir->u.m.def_mask, rs_rDX.GetReg());
  }
  if (flags & REG_USEA) {
    SetupRegMask(&lir->u.m.use_mask, rs_rAX.GetReg());
  }

  if (flags & REG_USEC) {
    SetupRegMask(&lir->u.m.use_mask, rs_rCX.GetReg());
  }

  if (flags & REG_USED) {
    SetupRegMask(&lir->u.m.use_mask, rs_rDX.GetReg());
  }

  if (flags & REG_USEB) {
    SetupRegMask(&lir->u.m.use_mask, rs_rBX.GetReg());
  }

  // Fixup hard to describe instruction: Uses rAX, rCX, rDI; sets rDI.
  if (lir->opcode == kX86RepneScasw) {
    SetupRegMask(&lir->u.m.use_mask, rs_rAX.GetReg());
    SetupRegMask(&lir->u.m.use_mask, rs_rCX.GetReg());
    SetupRegMask(&lir->u.m.use_mask, rs_rDI.GetReg());
    SetupRegMask(&lir->u.m.def_mask, rs_rDI.GetReg());
  }

  if (flags & USE_FP_STACK) {
    lir->u.m.use_mask |= ENCODE_X86_FP_STACK;
    lir->u.m.def_mask |= ENCODE_X86_FP_STACK;
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

void X86Mir2Lir::DumpResourceMask(LIR *x86LIR, uint64_t mask, const char *prefix) {
  char buf[256];
  buf[0] = 0;

  if (mask == ENCODE_ALL) {
    strcpy(buf, "all");
  } else {
    char num[8];
    int i;

    for (i = 0; i < kX86RegEnd; i++) {
      if (mask & (1ULL << i)) {
        snprintf(num, arraysize(num), "%d ", i);
        strcat(buf, num);
      }
    }

    if (mask & ENCODE_CCODE) {
      strcat(buf, "cc ");
    }
    /* Memory bits */
    if (x86LIR && (mask & ENCODE_DALVIK_REG)) {
      snprintf(buf + strlen(buf), arraysize(buf) - strlen(buf), "dr%d%s",
               DECODE_ALIAS_INFO_REG(x86LIR->flags.alias_info),
               (DECODE_ALIAS_INFO_WIDE(x86LIR->flags.alias_info)) ? "(+1)" : "");
    }
    if (mask & ENCODE_LITERAL) {
      strcat(buf, "lit ");
    }

    if (mask & ENCODE_HEAP_REF) {
      strcat(buf, "heap ");
    }
    if (mask & ENCODE_MUST_NOT_ALIAS) {
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
}

/* To be used when explicitly managing register use */
void X86Mir2Lir::FreeCallTemps() {
  FreeTemp(rs_rX86_ARG0);
  FreeTemp(rs_rX86_ARG1);
  FreeTemp(rs_rX86_ARG2);
  FreeTemp(rs_rX86_ARG3);
}

bool X86Mir2Lir::ProvidesFullMemoryBarrier(X86OpCode opcode) {
    switch (opcode) {
      case kX86LockCmpxchgMR:
      case kX86LockCmpxchgAR:
      case kX86LockCmpxchg8bM:
      case kX86LockCmpxchg8bA:
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
    mem_barrier->u.m.def_mask = ENCODE_ALL;
  }
  return ret;
#else
  return false;
#endif
}

// Alloc a pair of core registers, or a double.
RegStorage X86Mir2Lir::AllocTypedTempWide(bool fp_hint, int reg_class) {
  if (((reg_class == kAnyReg) && fp_hint) || (reg_class == kFPReg)) {
    return AllocTempDouble();
  }
  RegStorage low_reg = AllocTemp();
  RegStorage high_reg = AllocTemp();
  return RegStorage::MakeRegPair(low_reg, high_reg);
}

RegStorage X86Mir2Lir::AllocTypedTemp(bool fp_hint, int reg_class) {
  if (((reg_class == kAnyReg) && fp_hint) || (reg_class == kFPReg)) {
    return AllocTempSingle();
  }
  return AllocTemp();
}

void X86Mir2Lir::CompilerInitializeRegAlloc() {
  if (Gen64Bit()) {
    reg_pool_ = new (arena_) RegisterPool(this, arena_, core_regs_64, sp_regs_64, dp_regs_64, reserved_regs_64,
                                        core_temps_64, sp_temps_64, dp_temps_64);
  } else {
    reg_pool_ = new (arena_) RegisterPool(this, arena_, core_regs_32, sp_regs_32, dp_regs_32, reserved_regs_32,
                                        core_temps_32, sp_temps_32, dp_temps_32);
  }

  // Target-specific adjustments.

  // Alias single precision xmm to double xmms.
  // TODO: as needed, add larger vector sizes - alias all to the largest.
  GrowableArray<RegisterInfo*>::Iterator it(&reg_pool_->sp_regs_);
  for (RegisterInfo* info = it.Next(); info != nullptr; info = it.Next()) {
    int sp_reg_num = info->GetReg().GetRegNum();
    RegStorage dp_reg = RegStorage::Solo64(RegStorage::kFloatingPoint | sp_reg_num);
    RegisterInfo* dp_reg_info = GetRegInfo(dp_reg);
    // 64-bit xmm vector register's master storage should refer to itself.
    DCHECK_EQ(dp_reg_info, dp_reg_info->Master());
    // Redirect 32-bit vector's master storage to 64-bit vector.
    info->SetMaster(dp_reg_info);
  }

  // Don't start allocating temps at r0/s0/d0 or you may clobber return regs in early-exit methods.
  // TODO: adjust for x86/hard float calling convention.
  reg_pool_->next_core_reg_ = 2;
  reg_pool_->next_sp_reg_ = 2;
  reg_pool_->next_dp_reg_ = 1;
}

void X86Mir2Lir::FreeRegLocTemps(RegLocation rl_keep, RegLocation rl_free) {
  DCHECK(rl_keep.wide);
  DCHECK(rl_free.wide);
  int free_low = rl_free.reg.GetLowReg();
  int free_high = rl_free.reg.GetHighReg();
  int keep_low = rl_keep.reg.GetLowReg();
  int keep_high = rl_keep.reg.GetHighReg();
  if ((free_low != keep_low) && (free_low != keep_high) &&
      (free_high != keep_low) && (free_high != keep_high)) {
    // No overlap, free both
    FreeTemp(rl_free.reg);
  }
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
    rX86_ARG0 = rDI;
    rX86_ARG1 = rSI;
    rX86_ARG2 = rDX;
    rX86_ARG3 = rCX;
    // TODO: ARG4(r8), ARG5(r9), floating point args.
  } else {
    rs_rX86_SP = rs_rX86_SP_32;

    rs_rX86_ARG0 = rs_rAX;
    rs_rX86_ARG1 = rs_rCX;
    rs_rX86_ARG2 = rs_rDX;
    rs_rX86_ARG3 = rs_rBX;
    rX86_ARG0 = rAX;
    rX86_ARG1 = rCX;
    rX86_ARG2 = rDX;
    rX86_ARG3 = rBX;
  }
  rs_rX86_FARG0 = rs_rAX;
  rs_rX86_FARG1 = rs_rCX;
  rs_rX86_FARG2 = rs_rDX;
  rs_rX86_FARG3 = rs_rBX;
  rs_rX86_RET0 = rs_rAX;
  rs_rX86_RET1 = rs_rDX;
  rs_rX86_INVOKE_TGT = rs_rAX;
  rs_rX86_COUNT = rs_rCX;
  rX86_FARG0 = rAX;
  rX86_FARG1 = rCX;
  rX86_FARG2 = rDX;
  rX86_FARG3 = rBX;
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
  RegLocation rl_return = GetReturn(false);
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
        Load32Disp(rs_rX86_SP, displacement, rs_rBX);
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
    default:
      break;
  }
}

void X86Mir2Lir::GenConst128(BasicBlock* bb, MIR* mir) {
  int type_size = mir->dalvikInsn.vA;
  // We support 128 bit vectors.
  DCHECK_EQ(type_size & 0xFFFF, 128);
  int reg = mir->dalvikInsn.vB;
  DCHECK_LT(reg, 8);
  uint32_t *args = mir->dalvikInsn.arg;
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
  rl_method = LoadValue(rl_method, kCoreReg);

  // Load the proper value from the literal area.
  // We don't know the proper offset for the value, so pick one that will force
  // 4 byte offset.  We will fix this up in the assembler later to have the right
  // value.
  LIR *load = NewLIR3(kX86Mova128RM, reg, rl_method.reg.GetReg(),  256 /* bogus */);
  load->flags.fixup = kFixupLoad;
  load->target = data_target;
  SetMemRefType(load, true, kLiteral);
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

}  // namespace art
