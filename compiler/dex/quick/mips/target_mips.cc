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

#include "codegen_mips.h"

#include <inttypes.h>

#include <string>

#include "dex/compiler_internals.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "mips_lir.h"

namespace art {

static const RegStorage core_regs_arr[] =
    {rs_rZERO, rs_rAT, rs_rV0, rs_rV1, rs_rA0, rs_rA1, rs_rA2, rs_rA3, rs_rT0, rs_rT1, rs_rT2,
     rs_rT3, rs_rT4, rs_rT5, rs_rT6, rs_rT7, rs_rS0, rs_rS1, rs_rS2, rs_rS3, rs_rS4, rs_rS5,
     rs_rS6, rs_rS7, rs_rT8, rs_rT9, rs_rK0, rs_rK1, rs_rGP, rs_rSP, rs_rFP, rs_rRA};
static RegStorage sp_regs_arr[] =
    {rs_rF0, rs_rF1, rs_rF2, rs_rF3, rs_rF4, rs_rF5, rs_rF6, rs_rF7, rs_rF8, rs_rF9, rs_rF10,
     rs_rF11, rs_rF12, rs_rF13, rs_rF14, rs_rF15};
static RegStorage dp_regs_arr[] =
    {rs_rD0, rs_rD1, rs_rD2, rs_rD3, rs_rD4, rs_rD5, rs_rD6, rs_rD7};
static const RegStorage reserved_regs_arr[] =
    {rs_rZERO, rs_rAT, rs_rS0, rs_rS1, rs_rK0, rs_rK1, rs_rGP, rs_rSP, rs_rRA};
static RegStorage core_temps_arr[] =
    {rs_rV0, rs_rV1, rs_rA0, rs_rA1, rs_rA2, rs_rA3, rs_rT0, rs_rT1, rs_rT2, rs_rT3, rs_rT4,
     rs_rT5, rs_rT6, rs_rT7, rs_rT8};
static RegStorage sp_temps_arr[] =
    {rs_rF0, rs_rF1, rs_rF2, rs_rF3, rs_rF4, rs_rF5, rs_rF6, rs_rF7, rs_rF8, rs_rF9, rs_rF10,
     rs_rF11, rs_rF12, rs_rF13, rs_rF14, rs_rF15};
static RegStorage dp_temps_arr[] =
    {rs_rD0, rs_rD1, rs_rD2, rs_rD3, rs_rD4, rs_rD5, rs_rD6, rs_rD7};

static const std::vector<RegStorage> core_regs(core_regs_arr,
    core_regs_arr + sizeof(core_regs_arr) / sizeof(core_regs_arr[0]));
static const std::vector<RegStorage> sp_regs(sp_regs_arr,
    sp_regs_arr + sizeof(sp_regs_arr) / sizeof(sp_regs_arr[0]));
static const std::vector<RegStorage> dp_regs(dp_regs_arr,
    dp_regs_arr + sizeof(dp_regs_arr) / sizeof(dp_regs_arr[0]));
static const std::vector<RegStorage> reserved_regs(reserved_regs_arr,
    reserved_regs_arr + sizeof(reserved_regs_arr) / sizeof(reserved_regs_arr[0]));
static const std::vector<RegStorage> core_temps(core_temps_arr,
    core_temps_arr + sizeof(core_temps_arr) / sizeof(core_temps_arr[0]));
static const std::vector<RegStorage> sp_temps(sp_temps_arr,
    sp_temps_arr + sizeof(sp_temps_arr) / sizeof(sp_temps_arr[0]));
static const std::vector<RegStorage> dp_temps(dp_temps_arr,
    dp_temps_arr + sizeof(dp_temps_arr) / sizeof(dp_temps_arr[0]));

RegLocation MipsMir2Lir::LocCReturn() {
  return mips_loc_c_return;
}

RegLocation MipsMir2Lir::LocCReturnWide() {
  return mips_loc_c_return_wide;
}

RegLocation MipsMir2Lir::LocCReturnFloat() {
  return mips_loc_c_return_float;
}

RegLocation MipsMir2Lir::LocCReturnDouble() {
  return mips_loc_c_return_double;
}

// Return a target-dependent special register.
RegStorage MipsMir2Lir::TargetReg(SpecialTargetRegister reg) {
  RegStorage res_reg;
  switch (reg) {
    case kSelf: res_reg = rs_rMIPS_SELF; break;
    case kSuspend: res_reg =  rs_rMIPS_SUSPEND; break;
    case kLr: res_reg =  rs_rMIPS_LR; break;
    case kPc: res_reg =  rs_rMIPS_PC; break;
    case kSp: res_reg =  rs_rMIPS_SP; break;
    case kArg0: res_reg = rs_rMIPS_ARG0; break;
    case kArg1: res_reg = rs_rMIPS_ARG1; break;
    case kArg2: res_reg = rs_rMIPS_ARG2; break;
    case kArg3: res_reg = rs_rMIPS_ARG3; break;
    case kFArg0: res_reg = rs_rMIPS_FARG0; break;
    case kFArg1: res_reg = rs_rMIPS_FARG1; break;
    case kFArg2: res_reg = rs_rMIPS_FARG2; break;
    case kFArg3: res_reg = rs_rMIPS_FARG3; break;
    case kRet0: res_reg = rs_rMIPS_RET0; break;
    case kRet1: res_reg = rs_rMIPS_RET1; break;
    case kInvokeTgt: res_reg = rs_rMIPS_INVOKE_TGT; break;
    case kHiddenArg: res_reg = rs_rT0; break;
    case kHiddenFpArg: res_reg = RegStorage::InvalidReg(); break;
    case kCount: res_reg = rs_rMIPS_COUNT; break;
  }
  return res_reg;
}

RegStorage MipsMir2Lir::GetArgMappingToPhysicalReg(int arg_num) {
  // For the 32-bit internal ABI, the first 3 arguments are passed in registers.
  switch (arg_num) {
    case 0:
      return rs_rMIPS_ARG1;
    case 1:
      return rs_rMIPS_ARG2;
    case 2:
      return rs_rMIPS_ARG3;
    default:
      return RegStorage::InvalidReg();
  }
}

/*
 * Decode the register id.
 */
uint64_t MipsMir2Lir::GetRegMaskCommon(RegStorage reg) {
  uint64_t seed;
  int shift;
  int reg_id = reg.GetRegNum();
  /* Each double register is equal to a pair of single-precision FP registers */
  if (reg.IsDouble()) {
    seed = 0x3;
    reg_id = reg_id << 1;
  } else {
    seed = 1;
  }
  /* FP register starts at bit position 32 */
  shift = reg.IsFloat() ? kMipsFPReg0 : 0;
  /* Expand the double register id into single offset */
  shift += reg_id;
  return (seed << shift);
}

uint64_t MipsMir2Lir::GetPCUseDefEncoding() {
  return ENCODE_MIPS_REG_PC;
}


void MipsMir2Lir::SetupTargetResourceMasks(LIR* lir, uint64_t flags) {
  DCHECK_EQ(cu_->instruction_set, kMips);
  DCHECK(!lir->flags.use_def_invalid);

  // Mips-specific resource map setup here.
  if (flags & REG_DEF_SP) {
    lir->u.m.def_mask |= ENCODE_MIPS_REG_SP;
  }

  if (flags & REG_USE_SP) {
    lir->u.m.use_mask |= ENCODE_MIPS_REG_SP;
  }

  if (flags & REG_DEF_LR) {
    lir->u.m.def_mask |= ENCODE_MIPS_REG_LR;
  }

  if (flags & REG_DEF_HI) {
    lir->u.m.def_mask |= ENCODE_MIPS_REG_HI;
  }

  if (flags & REG_DEF_LO) {
    lir->u.m.def_mask |= ENCODE_MIPS_REG_LO;
  }

  if (flags & REG_USE_HI) {
    lir->u.m.use_mask |= ENCODE_MIPS_REG_HI;
  }

  if (flags & REG_USE_LO) {
    lir->u.m.use_mask |= ENCODE_MIPS_REG_LO;
  }
}

/* For dumping instructions */
#define MIPS_REG_COUNT 32
static const char *mips_reg_name[MIPS_REG_COUNT] = {
  "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
  "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
  "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
  "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

/*
 * Interpret a format string and build a string no longer than size
 * See format key in Assemble.c.
 */
std::string MipsMir2Lir::BuildInsnString(const char *fmt, LIR *lir, unsigned char* base_addr) {
  std::string buf;
  int i;
  const char *fmt_end = &fmt[strlen(fmt)];
  char tbuf[256];
  char nc;
  while (fmt < fmt_end) {
    int operand;
    if (*fmt == '!') {
      fmt++;
      DCHECK_LT(fmt, fmt_end);
      nc = *fmt++;
      if (nc == '!') {
        strcpy(tbuf, "!");
      } else {
         DCHECK_LT(fmt, fmt_end);
         DCHECK_LT(static_cast<unsigned>(nc-'0'), 4u);
         operand = lir->operands[nc-'0'];
         switch (*fmt++) {
           case 'b':
             strcpy(tbuf, "0000");
             for (i = 3; i >= 0; i--) {
               tbuf[i] += operand & 1;
               operand >>= 1;
             }
             break;
           case 's':
             snprintf(tbuf, arraysize(tbuf), "$f%d", RegStorage::RegNum(operand));
             break;
           case 'S':
             DCHECK_EQ(RegStorage::RegNum(operand) & 1, 0);
             snprintf(tbuf, arraysize(tbuf), "$f%d", RegStorage::RegNum(operand));
             break;
           case 'h':
             snprintf(tbuf, arraysize(tbuf), "%04x", operand);
             break;
           case 'M':
           case 'd':
             snprintf(tbuf, arraysize(tbuf), "%d", operand);
             break;
           case 'D':
             snprintf(tbuf, arraysize(tbuf), "%d", operand+1);
             break;
           case 'E':
             snprintf(tbuf, arraysize(tbuf), "%d", operand*4);
             break;
           case 'F':
             snprintf(tbuf, arraysize(tbuf), "%d", operand*2);
             break;
           case 't':
             snprintf(tbuf, arraysize(tbuf), "0x%08" PRIxPTR " (L%p)",
                 reinterpret_cast<uintptr_t>(base_addr) + lir->offset + 4 + (operand << 1),
                 lir->target);
             break;
           case 'T':
             snprintf(tbuf, arraysize(tbuf), "0x%08x", operand << 2);
             break;
           case 'u': {
             int offset_1 = lir->operands[0];
             int offset_2 = NEXT_LIR(lir)->operands[0];
             uintptr_t target =
                 (((reinterpret_cast<uintptr_t>(base_addr) + lir->offset + 4) & ~3) +
                 (offset_1 << 21 >> 9) + (offset_2 << 1)) & 0xfffffffc;
             snprintf(tbuf, arraysize(tbuf), "%p", reinterpret_cast<void*>(target));
             break;
          }

           /* Nothing to print for BLX_2 */
           case 'v':
             strcpy(tbuf, "see above");
             break;
           case 'r':
             DCHECK(operand >= 0 && operand < MIPS_REG_COUNT);
             strcpy(tbuf, mips_reg_name[operand]);
             break;
           case 'N':
             // Placeholder for delay slot handling
             strcpy(tbuf, ";  nop");
             break;
           default:
             strcpy(tbuf, "DecodeError");
             break;
         }
         buf += tbuf;
      }
    } else {
       buf += *fmt++;
    }
  }
  return buf;
}

// FIXME: need to redo resource maps for MIPS - fix this at that time
void MipsMir2Lir::DumpResourceMask(LIR *mips_lir, uint64_t mask, const char *prefix) {
  char buf[256];
  buf[0] = 0;

  if (mask == ENCODE_ALL) {
    strcpy(buf, "all");
  } else {
    char num[8];
    int i;

    for (i = 0; i < kMipsRegEnd; i++) {
      if (mask & (1ULL << i)) {
        snprintf(num, arraysize(num), "%d ", i);
        strcat(buf, num);
      }
    }

    if (mask & ENCODE_CCODE) {
      strcat(buf, "cc ");
    }
    if (mask & ENCODE_FP_STATUS) {
      strcat(buf, "fpcc ");
    }
    /* Memory bits */
    if (mips_lir && (mask & ENCODE_DALVIK_REG)) {
      snprintf(buf + strlen(buf), arraysize(buf) - strlen(buf), "dr%d%s",
               DECODE_ALIAS_INFO_REG(mips_lir->flags.alias_info),
               DECODE_ALIAS_INFO_WIDE(mips_lir->flags.alias_info) ? "(+1)" : "");
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

/*
 * TUNING: is true leaf?  Can't just use METHOD_IS_LEAF to determine as some
 * instructions might call out to C/assembly helper functions.  Until
 * machinery is in place, always spill lr.
 */

void MipsMir2Lir::AdjustSpillMask() {
  core_spill_mask_ |= (1 << rs_rRA.GetRegNum());
  num_core_spills_++;
}

/*
 * Mark a callee-save fp register as promoted.  Note that
 * vpush/vpop uses contiguous register lists so we must
 * include any holes in the mask.  Associate holes with
 * Dalvik register INVALID_VREG (0xFFFFU).
 */
void MipsMir2Lir::MarkPreservedSingle(int s_reg, RegStorage reg) {
  LOG(FATAL) << "No support yet for promoted FP regs";
}

void MipsMir2Lir::MarkPreservedDouble(int s_reg, RegStorage reg) {
  LOG(FATAL) << "No support yet for promoted FP regs";
}

/* Clobber all regs that might be used by an external C call */
void MipsMir2Lir::ClobberCallerSave() {
  Clobber(rs_rZERO);
  Clobber(rs_rAT);
  Clobber(rs_rV0);
  Clobber(rs_rV1);
  Clobber(rs_rA0);
  Clobber(rs_rA1);
  Clobber(rs_rA2);
  Clobber(rs_rA3);
  Clobber(rs_rT0);
  Clobber(rs_rT1);
  Clobber(rs_rT2);
  Clobber(rs_rT3);
  Clobber(rs_rT4);
  Clobber(rs_rT5);
  Clobber(rs_rT6);
  Clobber(rs_rT7);
  Clobber(rs_rT8);
  Clobber(rs_rT9);
  Clobber(rs_rK0);
  Clobber(rs_rK1);
  Clobber(rs_rGP);
  Clobber(rs_rFP);
  Clobber(rs_rRA);
  Clobber(rs_rF0);
  Clobber(rs_rF1);
  Clobber(rs_rF2);
  Clobber(rs_rF3);
  Clobber(rs_rF4);
  Clobber(rs_rF5);
  Clobber(rs_rF6);
  Clobber(rs_rF7);
  Clobber(rs_rF8);
  Clobber(rs_rF9);
  Clobber(rs_rF10);
  Clobber(rs_rF11);
  Clobber(rs_rF12);
  Clobber(rs_rF13);
  Clobber(rs_rF14);
  Clobber(rs_rF15);
  Clobber(rs_rD0);
  Clobber(rs_rD1);
  Clobber(rs_rD2);
  Clobber(rs_rD3);
  Clobber(rs_rD4);
  Clobber(rs_rD5);
  Clobber(rs_rD6);
  Clobber(rs_rD7);
}

RegLocation MipsMir2Lir::GetReturnWideAlt() {
  UNIMPLEMENTED(FATAL) << "No GetReturnWideAlt for MIPS";
  RegLocation res = LocCReturnWide();
  return res;
}

RegLocation MipsMir2Lir::GetReturnAlt() {
  UNIMPLEMENTED(FATAL) << "No GetReturnAlt for MIPS";
  RegLocation res = LocCReturn();
  return res;
}

/* To be used when explicitly managing register use */
void MipsMir2Lir::LockCallTemps() {
  LockTemp(rs_rMIPS_ARG0);
  LockTemp(rs_rMIPS_ARG1);
  LockTemp(rs_rMIPS_ARG2);
  LockTemp(rs_rMIPS_ARG3);
}

/* To be used when explicitly managing register use */
void MipsMir2Lir::FreeCallTemps() {
  FreeTemp(rs_rMIPS_ARG0);
  FreeTemp(rs_rMIPS_ARG1);
  FreeTemp(rs_rMIPS_ARG2);
  FreeTemp(rs_rMIPS_ARG3);
}

bool MipsMir2Lir::GenMemBarrier(MemBarrierKind barrier_kind) {
#if ANDROID_SMP != 0
  NewLIR1(kMipsSync, 0 /* Only stype currently supported */);
  return true;
#else
  return false;
#endif
}

// Alloc a pair of core registers, or a double.
RegStorage MipsMir2Lir::AllocTypedTempWide(bool fp_hint, int reg_class) {
  if (((reg_class == kAnyReg) && fp_hint) || (reg_class == kFPReg)) {
    return AllocTempDouble();
  }

  RegStorage low_reg = AllocTemp();
  RegStorage high_reg = AllocTemp();
  return RegStorage::MakeRegPair(low_reg, high_reg);
}

RegStorage MipsMir2Lir::AllocTypedTemp(bool fp_hint, int reg_class) {
  if (((reg_class == kAnyReg) && fp_hint) || (reg_class == kFPReg)) {
    return AllocTempSingle();
  }
  return AllocTemp();
}

void MipsMir2Lir::CompilerInitializeRegAlloc() {
  reg_pool_ = new (arena_) RegisterPool(this, arena_, core_regs, sp_regs, dp_regs, reserved_regs,
                                        core_temps, sp_temps, dp_temps);

  // Target-specific adjustments.

  // Alias single precision floats to appropriate half of overlapping double.
  GrowableArray<RegisterInfo*>::Iterator it(&reg_pool_->sp_regs_);
  for (RegisterInfo* info = it.Next(); info != nullptr; info = it.Next()) {
    int sp_reg_num = info->GetReg().GetRegNum();
    int dp_reg_num = sp_reg_num >> 1;
    RegStorage dp_reg = RegStorage::Solo64(RegStorage::kFloatingPoint | dp_reg_num);
    RegisterInfo* dp_reg_info = GetRegInfo(dp_reg);
    // Double precision register's master storage should refer to itself.
    DCHECK_EQ(dp_reg_info, dp_reg_info->Master());
    // Redirect single precision's master storage to master.
    info->SetMaster(dp_reg_info);
    // Singles should show a single 32-bit mask bit, at first referring to the low half.
    DCHECK_EQ(info->StorageMask(), 0x1U);
    if (sp_reg_num & 1) {
      // For odd singles, change to user the high word of the backing double.
      info->SetStorageMask(0x2);
    }
  }

  // Don't start allocating temps at r0/s0/d0 or you may clobber return regs in early-exit methods.
  // TODO: adjust when we roll to hard float calling convention.
  reg_pool_->next_core_reg_ = 2;
  reg_pool_->next_sp_reg_ = 2;
  reg_pool_->next_dp_reg_ = 1;
}

void MipsMir2Lir::FreeRegLocTemps(RegLocation rl_keep, RegLocation rl_free) {
  DCHECK(rl_keep.wide);
  DCHECK(rl_free.wide);
  if ((rl_free.reg.GetLowReg() != rl_keep.reg.GetLowReg()) &&
      (rl_free.reg.GetLowReg() != rl_keep.reg.GetHighReg()) &&
      (rl_free.reg.GetHighReg() != rl_keep.reg.GetLowReg()) &&
      (rl_free.reg.GetHighReg() != rl_keep.reg.GetHighReg())) {
    // No overlap, free.
    FreeTemp(rl_free.reg);
  }
}
/*
 * In the Arm code a it is typical to use the link register
 * to hold the target address.  However, for Mips we must
 * ensure that all branch instructions can be restarted if
 * there is a trap in the shadow.  Allocate a temp register.
 */
RegStorage MipsMir2Lir::LoadHelper(ThreadOffset<4> offset) {
  // NOTE: native pointer.
  LoadWordDisp(rs_rMIPS_SELF, offset.Int32Value(), rs_rT9);
  return rs_rT9;
}

RegStorage MipsMir2Lir::LoadHelper(ThreadOffset<8> offset) {
  UNIMPLEMENTED(FATAL) << "Should not be called.";
  return RegStorage::InvalidReg();
}

LIR* MipsMir2Lir::CheckSuspendUsingLoad() {
  RegStorage tmp = AllocTemp();
  // NOTE: native pointer.
  LoadWordDisp(rs_rMIPS_SELF, Thread::ThreadSuspendTriggerOffset<4>().Int32Value(), tmp);
  LIR *inst = LoadWordDisp(tmp, 0, tmp);
  FreeTemp(tmp);
  return inst;
}

void MipsMir2Lir::SpillCoreRegs() {
  if (num_core_spills_ == 0) {
    return;
  }
  uint32_t mask = core_spill_mask_;
  int offset = num_core_spills_ * 4;
  OpRegImm(kOpSub, rs_rSP, offset);
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      offset -= 4;
      Store32Disp(rs_rMIPS_SP, offset, RegStorage::Solo32(reg));
    }
  }
}

void MipsMir2Lir::UnSpillCoreRegs() {
  if (num_core_spills_ == 0) {
    return;
  }
  uint32_t mask = core_spill_mask_;
  int offset = frame_size_;
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      offset -= 4;
      Load32Disp(rs_rMIPS_SP, offset, RegStorage::Solo32(reg));
    }
  }
  OpRegImm(kOpAdd, rs_rSP, frame_size_);
}

bool MipsMir2Lir::IsUnconditionalBranch(LIR* lir) {
  return (lir->opcode == kMipsB);
}

bool MipsMir2Lir::SupportsVolatileLoadStore(OpSize size) {
  // No support for 64-bit atomic load/store on mips.
  return size != k64 && size != kDouble;
}

RegisterClass MipsMir2Lir::RegClassForFieldLoadStore(OpSize size, bool is_volatile) {
  // No support for 64-bit atomic load/store on mips.
  DCHECK(size != k64 && size != kDouble);
  // TODO: Verify that both core and fp registers are suitable for smaller sizes.
  return RegClassBySize(size);
}

MipsMir2Lir::MipsMir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena)
    : Mir2Lir(cu, mir_graph, arena) {
  for (int i = 0; i < kMipsLast; i++) {
    if (MipsMir2Lir::EncodingMap[i].opcode != i) {
      LOG(FATAL) << "Encoding order for " << MipsMir2Lir::EncodingMap[i].name
                 << " is wrong: expecting " << i << ", seeing "
                 << static_cast<int>(MipsMir2Lir::EncodingMap[i].opcode);
    }
  }
}

Mir2Lir* MipsCodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                           ArenaAllocator* const arena) {
  return new MipsMir2Lir(cu, mir_graph, arena);
}

uint64_t MipsMir2Lir::GetTargetInstFlags(int opcode) {
  DCHECK(!IsPseudoLirOp(opcode));
  return MipsMir2Lir::EncodingMap[opcode].flags;
}

const char* MipsMir2Lir::GetTargetInstName(int opcode) {
  DCHECK(!IsPseudoLirOp(opcode));
  return MipsMir2Lir::EncodingMap[opcode].name;
}

const char* MipsMir2Lir::GetTargetInstFmt(int opcode) {
  DCHECK(!IsPseudoLirOp(opcode));
  return MipsMir2Lir::EncodingMap[opcode].fmt;
}

}  // namespace art
