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

// FIXME: restore "static" when usage uncovered
/*static*/ int core_regs[] = {
  rAX, rCX, rDX, rBX, rX86_SP, rBP, rSI, rDI
#ifdef TARGET_REX_SUPPORT
  r8, r9, r10, r11, r12, r13, r14, 15
#endif
};
/*static*/ int ReservedRegs[] = {rX86_SP};
/*static*/ int core_temps[] = {rAX, rCX, rDX, rBX};
/*static*/ int FpRegs[] = {
  fr0, fr1, fr2, fr3, fr4, fr5, fr6, fr7,
#ifdef TARGET_REX_SUPPORT
  fr8, fr9, fr10, fr11, fr12, fr13, fr14, fr15
#endif
};
/*static*/ int fp_temps[] = {
  fr0, fr1, fr2, fr3, fr4, fr5, fr6, fr7,
#ifdef TARGET_REX_SUPPORT
  fr8, fr9, fr10, fr11, fr12, fr13, fr14, fr15
#endif
};

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
  int res_reg = RegStorage::kInvalidRegVal;
  switch (reg) {
    case kSelf: res_reg = rX86_SELF; break;
    case kSuspend: res_reg =  rX86_SUSPEND; break;
    case kLr: res_reg =  rX86_LR; break;
    case kPc: res_reg =  rX86_PC; break;
    case kSp: res_reg =  rX86_SP; break;
    case kArg0: res_reg = rX86_ARG0; break;
    case kArg1: res_reg = rX86_ARG1; break;
    case kArg2: res_reg = rX86_ARG2; break;
    case kArg3: res_reg = rX86_ARG3; break;
    case kFArg0: res_reg = rX86_FARG0; break;
    case kFArg1: res_reg = rX86_FARG1; break;
    case kFArg2: res_reg = rX86_FARG2; break;
    case kFArg3: res_reg = rX86_FARG3; break;
    case kRet0: res_reg = rX86_RET0; break;
    case kRet1: res_reg = rX86_RET1; break;
    case kInvokeTgt: res_reg = rX86_INVOKE_TGT; break;
    case kHiddenArg: res_reg = rAX; break;
    case kHiddenFpArg: res_reg = fr0; break;
    case kCount: res_reg = rX86_COUNT; break;
  }
  return RegStorage::Solo32(res_reg);
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

// Create a double from a pair of singles.
int X86Mir2Lir::S2d(int low_reg, int high_reg) {
  return X86_S2D(low_reg, high_reg);
}

// Return mask to strip off fp reg flags and bias.
uint32_t X86Mir2Lir::FpRegMask() {
  return X86_FP_REG_MASK;
}

// True if both regs single, both core or both double.
bool X86Mir2Lir::SameRegType(int reg1, int reg2) {
  return (X86_REGTYPE(reg1) == X86_REGTYPE(reg2));
}

/*
 * Decode the register id.
 */
uint64_t X86Mir2Lir::GetRegMaskCommon(int reg) {
  uint64_t seed;
  int shift;
  int reg_id;

  reg_id = reg & 0xf;
  /* Double registers in x86 are just a single FP register */
  seed = 1;
  /* FP register starts at bit position 16 */
  shift = X86_FPREG(reg) ? kX86FPReg0 : 0;
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
  DCHECK_EQ(cu_->instruction_set, kX86);
  DCHECK(!lir->flags.use_def_invalid);

  // X86-specific resource map setup here.
  if (flags & REG_USE_SP) {
    lir->u.m.use_mask |= ENCODE_X86_REG_SP;
  }

  if (flags & REG_DEF_SP) {
    lir->u.m.def_mask |= ENCODE_X86_REG_SP;
  }

  if (flags & REG_DEFA) {
    SetupRegMask(&lir->u.m.def_mask, rAX);
  }

  if (flags & REG_DEFD) {
    SetupRegMask(&lir->u.m.def_mask, rDX);
  }
  if (flags & REG_USEA) {
    SetupRegMask(&lir->u.m.use_mask, rAX);
  }

  if (flags & REG_USEC) {
    SetupRegMask(&lir->u.m.use_mask, rCX);
  }

  if (flags & REG_USED) {
    SetupRegMask(&lir->u.m.use_mask, rDX);
  }

  if (flags & REG_USEB) {
    SetupRegMask(&lir->u.m.use_mask, rBX);
  }

  // Fixup hard to describe instruction: Uses rAX, rCX, rDI; sets rDI.
  if (lir->opcode == kX86RepneScasw) {
    SetupRegMask(&lir->u.m.use_mask, rAX);
    SetupRegMask(&lir->u.m.use_mask, rCX);
    SetupRegMask(&lir->u.m.use_mask, rDI);
    SetupRegMask(&lir->u.m.def_mask, rDI);
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
            if (X86_FPREG(operand) || X86_DOUBLEREG(operand)) {
              int fp_reg = operand & X86_FP_REG_MASK;
              buf += StringPrintf("xmm%d", fp_reg);
            } else {
              DCHECK_LT(static_cast<size_t>(operand), sizeof(x86RegName));
              buf += x86RegName[operand];
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
  core_spill_mask_ |= (1 << rRET);
  num_core_spills_++;
}

/*
 * Mark a callee-save fp register as promoted.  Note that
 * vpush/vpop uses contiguous register lists so we must
 * include any holes in the mask.  Associate holes with
 * Dalvik register INVALID_VREG (0xFFFFU).
 */
void X86Mir2Lir::MarkPreservedSingle(int v_reg, int reg) {
  UNIMPLEMENTED(WARNING) << "MarkPreservedSingle";
#if 0
  LOG(FATAL) << "No support yet for promoted FP regs";
#endif
}

void X86Mir2Lir::FlushRegWide(RegStorage reg) {
  RegisterInfo* info1 = GetRegInfo(reg.GetLowReg());
  RegisterInfo* info2 = GetRegInfo(reg.GetHighReg());
  DCHECK(info1 && info2 && info1->pair && info2->pair &&
         (info1->partner == info2->reg) &&
         (info2->partner == info1->reg));
  if ((info1->live && info1->dirty) || (info2->live && info2->dirty)) {
    if (!(info1->is_temp && info2->is_temp)) {
      /* Should not happen.  If it does, there's a problem in eval_loc */
      LOG(FATAL) << "Long half-temp, half-promoted";
    }

    info1->dirty = false;
    info2->dirty = false;
    if (mir_graph_->SRegToVReg(info2->s_reg) < mir_graph_->SRegToVReg(info1->s_reg))
      info1 = info2;
    int v_reg = mir_graph_->SRegToVReg(info1->s_reg);
    StoreBaseDispWide(rs_rX86_SP, VRegOffset(v_reg),
                      RegStorage(RegStorage::k64BitPair, info1->reg, info1->partner));
  }
}

void X86Mir2Lir::FlushReg(RegStorage reg) {
  DCHECK(!reg.IsPair());
  RegisterInfo* info = GetRegInfo(reg.GetReg());
  if (info->live && info->dirty) {
    info->dirty = false;
    int v_reg = mir_graph_->SRegToVReg(info->s_reg);
    StoreBaseDisp(rs_rX86_SP, VRegOffset(v_reg), reg, kWord);
  }
}

/* Give access to the target-dependent FP register encoding to common code */
bool X86Mir2Lir::IsFpReg(int reg) {
  return X86_FPREG(reg);
}

bool X86Mir2Lir::IsFpReg(RegStorage reg) {
  return IsFpReg(reg.IsPair() ? reg.GetLowReg() : reg.GetReg());
}

/* Clobber all regs that might be used by an external C call */
void X86Mir2Lir::ClobberCallerSave() {
  Clobber(rAX);
  Clobber(rCX);
  Clobber(rDX);
  Clobber(rBX);
}

RegLocation X86Mir2Lir::GetReturnWideAlt() {
  RegLocation res = LocCReturnWide();
  CHECK(res.reg.GetLowReg() == rAX);
  CHECK(res.reg.GetHighReg() == rDX);
  Clobber(rAX);
  Clobber(rDX);
  MarkInUse(rAX);
  MarkInUse(rDX);
  MarkPair(res.reg.GetLowReg(), res.reg.GetHighReg());
  return res;
}

RegLocation X86Mir2Lir::GetReturnAlt() {
  RegLocation res = LocCReturn();
  res.reg.SetReg(rDX);
  Clobber(rDX);
  MarkInUse(rDX);
  return res;
}

/* To be used when explicitly managing register use */
void X86Mir2Lir::LockCallTemps() {
  LockTemp(rX86_ARG0);
  LockTemp(rX86_ARG1);
  LockTemp(rX86_ARG2);
  LockTemp(rX86_ARG3);
}

/* To be used when explicitly managing register use */
void X86Mir2Lir::FreeCallTemps() {
  FreeTemp(rX86_ARG0);
  FreeTemp(rX86_ARG1);
  FreeTemp(rX86_ARG2);
  FreeTemp(rX86_ARG3);
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

void X86Mir2Lir::GenMemBarrier(MemBarrierKind barrier_kind) {
#if ANDROID_SMP != 0
  // Start off with using the last LIR as the barrier. If it is not enough, then we will update it.
  LIR* mem_barrier = last_lir_insn_;

  /*
   * According to the JSR-133 Cookbook, for x86 only StoreLoad barriers need memory fence. All other barriers
   * (LoadLoad, LoadStore, StoreStore) are nops due to the x86 memory model. For those cases, all we need
   * to ensure is that there is a scheduling barrier in place.
   */
  if (barrier_kind == kStoreLoad) {
    // If no LIR exists already that can be used a barrier, then generate an mfence.
    if (mem_barrier == nullptr) {
      mem_barrier = NewLIR0(kX86Mfence);
    }

    // If last instruction does not provide full barrier, then insert an mfence.
    if (ProvidesFullMemoryBarrier(static_cast<X86OpCode>(mem_barrier->opcode)) == false) {
      mem_barrier = NewLIR0(kX86Mfence);
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
    return AllocTempFloat();
  }
  return AllocTemp();
}

void X86Mir2Lir::CompilerInitializeRegAlloc() {
  int num_regs = sizeof(core_regs)/sizeof(*core_regs);
  int num_reserved = sizeof(ReservedRegs)/sizeof(*ReservedRegs);
  int num_temps = sizeof(core_temps)/sizeof(*core_temps);
  int num_fp_regs = sizeof(FpRegs)/sizeof(*FpRegs);
  int num_fp_temps = sizeof(fp_temps)/sizeof(*fp_temps);
  reg_pool_ = static_cast<RegisterPool*>(arena_->Alloc(sizeof(*reg_pool_),
                                                       kArenaAllocRegAlloc));
  reg_pool_->num_core_regs = num_regs;
  reg_pool_->core_regs =
      static_cast<RegisterInfo*>(arena_->Alloc(num_regs * sizeof(*reg_pool_->core_regs),
                                               kArenaAllocRegAlloc));
  reg_pool_->num_fp_regs = num_fp_regs;
  reg_pool_->FPRegs =
      static_cast<RegisterInfo *>(arena_->Alloc(num_fp_regs * sizeof(*reg_pool_->FPRegs),
                                                kArenaAllocRegAlloc));
  CompilerInitPool(reg_pool_->core_regs, core_regs, reg_pool_->num_core_regs);
  CompilerInitPool(reg_pool_->FPRegs, FpRegs, reg_pool_->num_fp_regs);
  // Keep special registers from being allocated
  for (int i = 0; i < num_reserved; i++) {
    MarkInUse(ReservedRegs[i]);
  }
  // Mark temp regs - all others not in use can be used for promotion
  for (int i = 0; i < num_temps; i++) {
    MarkTemp(core_temps[i]);
  }
  for (int i = 0; i < num_fp_temps; i++) {
    MarkTemp(fp_temps[i]);
  }
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
    FreeTemp(free_low);
    FreeTemp(free_high);
  }
}

void X86Mir2Lir::SpillCoreRegs() {
  if (num_core_spills_ == 0) {
    return;
  }
  // Spill mask not including fake return address register
  uint32_t mask = core_spill_mask_ & ~(1 << rRET);
  int offset = frame_size_ - (4 * num_core_spills_);
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      StoreWordDisp(rs_rX86_SP, offset, RegStorage::Solo32(reg));
      offset += 4;
    }
  }
}

void X86Mir2Lir::UnSpillCoreRegs() {
  if (num_core_spills_ == 0) {
    return;
  }
  // Spill mask not including fake return address register
  uint32_t mask = core_spill_mask_ & ~(1 << rRET);
  int offset = frame_size_ - (4 * num_core_spills_);
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      LoadWordDisp(rs_rX86_SP, offset, RegStorage::Solo32(reg));
      offset += 4;
    }
  }
}

bool X86Mir2Lir::IsUnconditionalBranch(LIR* lir) {
  return (lir->opcode == kX86Jmp8 || lir->opcode == kX86Jmp32);
}

X86Mir2Lir::X86Mir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena)
    : Mir2Lir(cu, mir_graph, arena),
      method_address_insns_(arena, 100, kGrowableArrayMisc),
      class_type_address_insns_(arena, 100, kGrowableArrayMisc),
      call_method_insns_(arena, 100, kGrowableArrayMisc),
      stack_decrement_(nullptr), stack_increment_(nullptr) {
  store_method_addr_used_ = false;
  for (int i = 0; i < kX86Last; i++) {
    if (X86Mir2Lir::EncodingMap[i].opcode != i) {
      LOG(FATAL) << "Encoding order for " << X86Mir2Lir::EncodingMap[i].name
                 << " is wrong: expecting " << i << ", seeing "
                 << static_cast<int>(X86Mir2Lir::EncodingMap[i].opcode);
    }
  }
}

Mir2Lir* X86CodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                          ArenaAllocator* const arena) {
  return new X86Mir2Lir(cu, mir_graph, arena);
}

// Not used in x86
RegStorage X86Mir2Lir::LoadHelper(ThreadOffset offset) {
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

/*
 * Return an updated location record with current in-register status.
 * If the value lives in live temps, reflect that fact.  No code
 * is generated.  If the live value is part of an older pair,
 * clobber both low and high.
 */
// TODO: Reunify with common code after 'pair mess' has been fixed
RegLocation X86Mir2Lir::UpdateLocWide(RegLocation loc) {
  DCHECK(loc.wide);
  DCHECK(CheckCorePoolSanity());
  if (loc.location != kLocPhysReg) {
    DCHECK((loc.location == kLocDalvikFrame) ||
         (loc.location == kLocCompilerTemp));
    // Are the dalvik regs already live in physical registers?
    RegisterInfo* info_lo = AllocLive(loc.s_reg_low, kAnyReg);

    // Handle FP registers specially on x86.
    if (info_lo && IsFpReg(info_lo->reg)) {
      bool match = true;

      // We can't match a FP register with a pair of Core registers.
      match = match && (info_lo->pair == 0);

      if (match) {
        // We can reuse;update the register usage info.
        loc.location = kLocPhysReg;
        loc.vec_len = kVectorLength8;
        // TODO: use k64BitVector
        loc.reg = RegStorage(RegStorage::k64BitPair, info_lo->reg, info_lo->reg);
        DCHECK(IsFpReg(loc.reg.GetLowReg()));
        return loc;
      }
      // We can't easily reuse; clobber and free any overlaps.
      if (info_lo) {
        Clobber(info_lo->reg);
        FreeTemp(info_lo->reg);
        if (info_lo->pair)
          Clobber(info_lo->partner);
      }
    } else {
      RegisterInfo* info_hi = AllocLive(GetSRegHi(loc.s_reg_low), kAnyReg);
      bool match = true;
      match = match && (info_lo != NULL);
      match = match && (info_hi != NULL);
      // Are they both core or both FP?
      match = match && (IsFpReg(info_lo->reg) == IsFpReg(info_hi->reg));
      // If a pair of floating point singles, are they properly aligned?
      if (match && IsFpReg(info_lo->reg)) {
        match &= ((info_lo->reg & 0x1) == 0);
        match &= ((info_hi->reg - info_lo->reg) == 1);
      }
      // If previously used as a pair, it is the same pair?
      if (match && (info_lo->pair || info_hi->pair)) {
        match = (info_lo->pair == info_hi->pair);
        match &= ((info_lo->reg == info_hi->partner) &&
              (info_hi->reg == info_lo->partner));
      }
      if (match) {
        // Can reuse - update the register usage info
        loc.reg = RegStorage(RegStorage::k64BitPair, info_lo->reg, info_hi->reg);
        loc.location = kLocPhysReg;
        MarkPair(loc.reg.GetLowReg(), loc.reg.GetHighReg());
        DCHECK(!IsFpReg(loc.reg.GetLowReg()) || ((loc.reg.GetLowReg() & 0x1) == 0));
        return loc;
      }
      // Can't easily reuse - clobber and free any overlaps
      if (info_lo) {
        Clobber(info_lo->reg);
        FreeTemp(info_lo->reg);
        if (info_lo->pair)
          Clobber(info_lo->partner);
      }
      if (info_hi) {
        Clobber(info_hi->reg);
        FreeTemp(info_hi->reg);
        if (info_hi->pair)
          Clobber(info_hi->partner);
      }
    }
  }
  return loc;
}

// TODO: Reunify with common code after 'pair mess' has been fixed
RegLocation X86Mir2Lir::EvalLocWide(RegLocation loc, int reg_class, bool update) {
  DCHECK(loc.wide);

  loc = UpdateLocWide(loc);

  /* If it is already in a register, we can assume proper form.  Is it the right reg class? */
  if (loc.location == kLocPhysReg) {
    DCHECK_EQ(IsFpReg(loc.reg.GetLowReg()), loc.IsVectorScalar());
    if (!RegClassMatches(reg_class, loc.reg)) {
      /* It is the wrong register class.  Reallocate and copy. */
      if (!IsFpReg(loc.reg.GetLowReg())) {
        // We want this in a FP reg, and it is in core registers.
        DCHECK(reg_class != kCoreReg);
        // Allocate this into any FP reg, and mark it with the right size.
        int32_t low_reg = AllocTypedTemp(true, reg_class).GetReg();
        OpVectorRegCopyWide(low_reg, loc.reg.GetLowReg(), loc.reg.GetHighReg());
        CopyRegInfo(low_reg, loc.reg.GetLowReg());
        Clobber(loc.reg);
        loc.reg.SetReg(low_reg);
        loc.reg.SetHighReg(low_reg);  // Play nice with existing code.
        loc.vec_len = kVectorLength8;
      } else {
        // The value is in a FP register, and we want it in a pair of core registers.
        DCHECK_EQ(reg_class, kCoreReg);
        DCHECK_EQ(loc.reg.GetLowReg(), loc.reg.GetHighReg());
        RegStorage new_regs = AllocTypedTempWide(false, kCoreReg);  // Force to core registers.
        OpRegCopyWide(new_regs, loc.reg);
        CopyRegInfo(new_regs.GetLowReg(), loc.reg.GetLowReg());
        CopyRegInfo(new_regs.GetHighReg(), loc.reg.GetHighReg());
        Clobber(loc.reg);
        loc.reg = new_regs;
        MarkPair(loc.reg.GetLowReg(), loc.reg.GetHighReg());
        DCHECK(!IsFpReg(loc.reg.GetLowReg()) || ((loc.reg.GetLowReg() & 0x1) == 0));
      }
    }
    return loc;
  }

  DCHECK_NE(loc.s_reg_low, INVALID_SREG);
  DCHECK_NE(GetSRegHi(loc.s_reg_low), INVALID_SREG);

  loc.reg = AllocTypedTempWide(loc.fp, reg_class);

  // FIXME: take advantage of RegStorage notation.
  if (loc.reg.GetLowReg() == loc.reg.GetHighReg()) {
    DCHECK(IsFpReg(loc.reg.GetLowReg()));
    loc.vec_len = kVectorLength8;
  } else {
    MarkPair(loc.reg.GetLowReg(), loc.reg.GetHighReg());
  }
  if (update) {
    loc.location = kLocPhysReg;
    MarkLive(loc.reg.GetLow(), loc.s_reg_low);
    if (loc.reg.GetLowReg() != loc.reg.GetHighReg()) {
      MarkLive(loc.reg.GetHigh(), GetSRegHi(loc.s_reg_low));
    }
  }
  return loc;
}

// TODO: Reunify with common code after 'pair mess' has been fixed
RegLocation X86Mir2Lir::EvalLoc(RegLocation loc, int reg_class, bool update) {
  if (loc.wide)
    return EvalLocWide(loc, reg_class, update);

  loc = UpdateLoc(loc);

  if (loc.location == kLocPhysReg) {
    if (!RegClassMatches(reg_class, loc.reg)) {
      /* Wrong register class.  Realloc, copy and transfer ownership. */
      RegStorage new_reg = AllocTypedTemp(loc.fp, reg_class);
      OpRegCopy(new_reg, loc.reg);
      CopyRegInfo(new_reg, loc.reg);
      Clobber(loc.reg);
      loc.reg = new_reg;
      if (IsFpReg(loc.reg.GetReg()) && reg_class != kCoreReg)
        loc.vec_len = kVectorLength4;
    }
    return loc;
  }

  DCHECK_NE(loc.s_reg_low, INVALID_SREG);

  loc.reg = AllocTypedTemp(loc.fp, reg_class);
  if (IsFpReg(loc.reg.GetReg()) && reg_class != kCoreReg)
    loc.vec_len = kVectorLength4;

  if (update) {
    loc.location = kLocPhysReg;
    MarkLive(loc.reg, loc.s_reg_low);
  }
  return loc;
}

RegStorage X86Mir2Lir::AllocTempDouble() {
  // We really don't need a pair of registers.
  // FIXME - update to double
  int reg = AllocTempFloat().GetReg();
  return RegStorage(RegStorage::k64BitPair, reg, reg);
}

// TODO: Reunify with common code after 'pair mess' has been fixed
void X86Mir2Lir::ResetDefLocWide(RegLocation rl) {
  DCHECK(rl.wide);
  RegisterInfo* p_low = IsTemp(rl.reg.GetLowReg());
  if (IsFpReg(rl.reg.GetLowReg())) {
    // We are using only the low register.
    if (p_low && !(cu_->disable_opt & (1 << kSuppressLoads))) {
      NullifyRange(p_low->def_start, p_low->def_end, p_low->s_reg, rl.s_reg_low);
    }
    ResetDef(rl.reg.GetLowReg());
  } else {
    RegisterInfo* p_high = IsTemp(rl.reg.GetHighReg());
    if (p_low && !(cu_->disable_opt & (1 << kSuppressLoads))) {
      DCHECK(p_low->pair);
      NullifyRange(p_low->def_start, p_low->def_end, p_low->s_reg, rl.s_reg_low);
    }
    if (p_high && !(cu_->disable_opt & (1 << kSuppressLoads))) {
      DCHECK(p_high->pair);
    }
    ResetDef(rl.reg.GetLowReg());
    ResetDef(rl.reg.GetHighReg());
  }
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
             << " vec_len: " << loc.vec_len
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

void X86Mir2Lir::InstallLiteralPools() {
  // These are handled differently for x86.
  DCHECK(code_literal_list_ == nullptr);
  DCHECK(method_literal_list_ == nullptr);
  DCHECK(class_literal_list_ == nullptr);

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
  LIR* launchpad_branch = nullptr;
  if (rl_char.is_const) {
    // We need the value in EAX.
    LoadConstantNoClobber(rs_rAX, char_value);
  } else {
    // Character is not a constant; compare at runtime.
    LoadValueDirectFixed(rl_char, rs_rAX);
    launchpad_branch = OpCmpImmBranch(kCondGt, rs_rAX, 0xFFFF, nullptr);
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
  NewLIR1(kX86Push32R, rDI);

  // Compute the number of words to search in to rCX.
  LoadWordDisp(rs_rDX, count_offset, rs_rCX);
  LIR *length_compare = nullptr;
  int start_value = 0;
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
      rl_start = UpdateLoc(rl_start);
      if (rl_start.location == kLocPhysReg) {
        length_compare = OpCmpBranch(kCondLe, rs_rCX, rl_start.reg, nullptr);
        OpRegReg(kOpSub, rs_rCX, rl_start.reg);
      } else {
        // Compare to memory to avoid a register load.  Handle pushed EDI.
        int displacement = SRegOffset(rl_start.s_reg_low) + sizeof(uint32_t);
        OpRegMem(kOpCmp, rs_rCX, rs_rX86_SP, displacement);
        length_compare = NewLIR2(kX86Jcc8, 0, kX86CondLe);
        OpRegMem(kOpSub, rs_rCX, rs_rX86_SP, displacement);
      }
    }
  }
  DCHECK(length_compare != nullptr);

  // ECX now contains the count in words to be searched.

  // Load the address of the string into EBX.
  // The string starts at VALUE(String) + 2 * OFFSET(String) + DATA_OFFSET.
  LoadWordDisp(rs_rDX, value_offset, rs_rDI);
  LoadWordDisp(rs_rDX, offset_offset, rs_rBX);
  OpLea(rs_rBX, rs_rDI, rs_rBX, 1, data_offset);

  // Now compute into EDI where the search will start.
  if (zero_based || rl_start.is_const) {
    if (start_value == 0) {
      OpRegCopy(rs_rDI, rs_rBX);
    } else {
      NewLIR3(kX86Lea32RM, rDI, rBX, 2 * start_value);
    }
  } else {
    if (rl_start.location == kLocPhysReg) {
      if (rl_start.reg.GetReg() == rDI) {
        // We have a slight problem here.  We are already using RDI!
        // Grab the value from the stack.
        LoadWordDisp(rs_rX86_SP, 0, rs_rDX);
        OpLea(rs_rDI, rs_rBX, rs_rDX, 1, 0);
      } else {
        OpLea(rs_rDI, rs_rBX, rl_start.reg, 1, 0);
      }
    } else {
      OpRegCopy(rs_rDI, rs_rBX);
      // Load the start index from stack, remembering that we pushed EDI.
      int displacement = SRegOffset(rl_start.s_reg_low) + sizeof(uint32_t);
      LoadWordDisp(rs_rX86_SP, displacement, rs_rDX);
      OpLea(rs_rDI, rs_rBX, rs_rDX, 1, 0);
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
  NewLIR3(kX86Lea32RM, rl_return.reg.GetReg(), rDI, -1);
  LIR *all_done = NewLIR1(kX86Jmp8, 0);

  // Failed to match; return -1.
  LIR *not_found = NewLIR0(kPseudoTargetLabel);
  length_compare->target = not_found;
  failed_branch->target = not_found;
  LoadConstantNoClobber(rl_return.reg, -1);

  // And join up at the end.
  all_done->target = NewLIR0(kPseudoTargetLabel);
  // Restore EDI from the stack.
  NewLIR1(kX86Pop32R, rDI);

  // Out of line code returns here.
  if (launchpad_branch != nullptr) {
    LIR *return_point = NewLIR0(kPseudoTargetLabel);
    AddIntrinsicLaunchpad(info, launchpad_branch, return_point);
  }

  StoreValue(rl_dest, rl_return);
  return true;
}

/*
 * @brief Enter a 32 bit quantity into the FDE buffer
 * @param buf FDE buffer.
 * @param data Data value.
 */
static void PushWord(std::vector<uint8_t>&buf, int data) {
  buf.push_back(data & 0xff);
  buf.push_back((data >> 8) & 0xff);
  buf.push_back((data >> 16) & 0xff);
  buf.push_back((data >> 24) & 0xff);
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

}  // namespace art
