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

#include "../../compiler_internals.h"
#include "mips_lir.h"
#include "codegen_mips.h"
#include "../ralloc_util.h"
#include "../codegen_util.h"

#include <string>

namespace art {

static int core_regs[] = {r_ZERO, r_AT, r_V0, r_V1, r_A0, r_A1, r_A2, r_A3,
                          r_T0, r_T1, r_T2, r_T3, r_T4, r_T5, r_T6, r_T7,
                          r_S0, r_S1, r_S2, r_S3, r_S4, r_S5, r_S6, r_S7, r_T8,
                          r_T9, r_K0, r_K1, r_GP, r_SP, r_FP, r_RA};
static int ReservedRegs[] = {r_ZERO, r_AT, r_S0, r_S1, r_K0, r_K1, r_GP, r_SP,
                             r_RA};
static int core_temps[] = {r_V0, r_V1, r_A0, r_A1, r_A2, r_A3, r_T0, r_T1, r_T2,
                           r_T3, r_T4, r_T5, r_T6, r_T7, r_T8};
static int FpRegs[] = {r_F0, r_F1, r_F2, r_F3, r_F4, r_F5, r_F6, r_F7,
                       r_F8, r_F9, r_F10, r_F11, r_F12, r_F13, r_F14, r_F15};
static int fp_temps[] = {r_F0, r_F1, r_F2, r_F3, r_F4, r_F5, r_F6, r_F7,
                         r_F8, r_F9, r_F10, r_F11, r_F12, r_F13, r_F14, r_F15};

RegLocation MipsCodegen::LocCReturn()
{
  RegLocation res = MIPS_LOC_C_RETURN;
  return res;
}

RegLocation MipsCodegen::LocCReturnWide()
{
  RegLocation res = MIPS_LOC_C_RETURN_WIDE;
  return res;
}

RegLocation MipsCodegen::LocCReturnFloat()
{
  RegLocation res = MIPS_LOC_C_RETURN_FLOAT;
  return res;
}

RegLocation MipsCodegen::LocCReturnDouble()
{
  RegLocation res = MIPS_LOC_C_RETURN_DOUBLE;
  return res;
}

// Return a target-dependent special register.
int MipsCodegen::TargetReg(SpecialTargetRegister reg) {
  int res = INVALID_REG;
  switch (reg) {
    case kSelf: res = rMIPS_SELF; break;
    case kSuspend: res =  rMIPS_SUSPEND; break;
    case kLr: res =  rMIPS_LR; break;
    case kPc: res =  rMIPS_PC; break;
    case kSp: res =  rMIPS_SP; break;
    case kArg0: res = rMIPS_ARG0; break;
    case kArg1: res = rMIPS_ARG1; break;
    case kArg2: res = rMIPS_ARG2; break;
    case kArg3: res = rMIPS_ARG3; break;
    case kFArg0: res = rMIPS_FARG0; break;
    case kFArg1: res = rMIPS_FARG1; break;
    case kFArg2: res = rMIPS_FARG2; break;
    case kFArg3: res = rMIPS_FARG3; break;
    case kRet0: res = rMIPS_RET0; break;
    case kRet1: res = rMIPS_RET1; break;
    case kInvokeTgt: res = rMIPS_INVOKE_TGT; break;
    case kCount: res = rMIPS_COUNT; break;
  }
  return res;
}

// Create a double from a pair of singles.
int MipsCodegen::S2d(int low_reg, int high_reg)
{
  return MIPS_S2D(low_reg, high_reg);
}

// Return mask to strip off fp reg flags and bias.
uint32_t MipsCodegen::FpRegMask()
{
  return MIPS_FP_REG_MASK;
}

// True if both regs single, both core or both double.
bool MipsCodegen::SameRegType(int reg1, int reg2)
{
  return (MIPS_REGTYPE(reg1) == MIPS_REGTYPE(reg2));
}

/*
 * Decode the register id.
 */
uint64_t MipsCodegen::GetRegMaskCommon(CompilationUnit* cu, int reg)
{
  uint64_t seed;
  int shift;
  int reg_id;


  reg_id = reg & 0x1f;
  /* Each double register is equal to a pair of single-precision FP registers */
  seed = MIPS_DOUBLEREG(reg) ? 3 : 1;
  /* FP register starts at bit position 16 */
  shift = MIPS_FPREG(reg) ? kMipsFPReg0 : 0;
  /* Expand the double register id into single offset */
  shift += reg_id;
  return (seed << shift);
}

uint64_t MipsCodegen::GetPCUseDefEncoding()
{
  return ENCODE_MIPS_REG_PC;
}


void MipsCodegen::SetupTargetResourceMasks(CompilationUnit* cu, LIR* lir)
{
  DCHECK_EQ(cu->instruction_set, kMips);

  // Mips-specific resource map setup here.
  uint64_t flags = MipsCodegen::EncodingMap[lir->opcode].flags;

  if (flags & REG_DEF_SP) {
    lir->def_mask |= ENCODE_MIPS_REG_SP;
  }

  if (flags & REG_USE_SP) {
    lir->use_mask |= ENCODE_MIPS_REG_SP;
  }

  if (flags & REG_DEF_LR) {
    lir->def_mask |= ENCODE_MIPS_REG_LR;
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
std::string MipsCodegen::BuildInsnString(const char *fmt, LIR *lir, unsigned char* base_addr)
{
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
      if (nc=='!') {
        strcpy(tbuf, "!");
      } else {
         DCHECK_LT(fmt, fmt_end);
         DCHECK_LT(static_cast<unsigned>(nc-'0'), 4u);
         operand = lir->operands[nc-'0'];
         switch (*fmt++) {
           case 'b':
             strcpy(tbuf,"0000");
             for (i=3; i>= 0; i--) {
               tbuf[i] += operand & 1;
               operand >>= 1;
             }
             break;
           case 's':
             sprintf(tbuf,"$f%d",operand & MIPS_FP_REG_MASK);
             break;
           case 'S':
             DCHECK_EQ(((operand & MIPS_FP_REG_MASK) & 1), 0);
             sprintf(tbuf,"$f%d",operand & MIPS_FP_REG_MASK);
             break;
           case 'h':
             sprintf(tbuf,"%04x", operand);
             break;
           case 'M':
           case 'd':
             sprintf(tbuf,"%d", operand);
             break;
           case 'D':
             sprintf(tbuf,"%d", operand+1);
             break;
           case 'E':
             sprintf(tbuf,"%d", operand*4);
             break;
           case 'F':
             sprintf(tbuf,"%d", operand*2);
             break;
           case 't':
             sprintf(tbuf,"0x%08x (L%p)", reinterpret_cast<uintptr_t>(base_addr) + lir->offset + 4 +
                     (operand << 2), lir->target);
             break;
           case 'T':
             sprintf(tbuf,"0x%08x", operand << 2);
             break;
           case 'u': {
             int offset_1 = lir->operands[0];
             int offset_2 = NEXT_LIR(lir)->operands[0];
             uintptr_t target =
                 (((reinterpret_cast<uintptr_t>(base_addr) + lir->offset + 4) & ~3) +
                 (offset_1 << 21 >> 9) + (offset_2 << 1)) & 0xfffffffc;
             sprintf(tbuf, "%p", reinterpret_cast<void*>(target));
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
             strcpy(tbuf,"DecodeError");
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
void MipsCodegen::DumpResourceMask(LIR *mips_lir, uint64_t mask, const char *prefix)
{
  char buf[256];
  buf[0] = 0;

  if (mask == ENCODE_ALL) {
    strcpy(buf, "all");
  } else {
    char num[8];
    int i;

    for (i = 0; i < kMipsRegEnd; i++) {
      if (mask & (1ULL << i)) {
        sprintf(num, "%d ", i);
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
      sprintf(buf + strlen(buf), "dr%d%s", mips_lir->alias_info & 0xffff,
              (mips_lir->alias_info & 0x80000000) ? "(+1)" : "");
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
 * TUNING: is leaf?  Can't just use "has_invoke" to determine as some
 * instructions might call out to C/assembly helper functions.  Until
 * machinery is in place, always spill lr.
 */

void MipsCodegen::AdjustSpillMask(CompilationUnit* cu)
{
  cu->core_spill_mask |= (1 << r_RA);
  cu->num_core_spills++;
}

/*
 * Mark a callee-save fp register as promoted.  Note that
 * vpush/vpop uses contiguous register lists so we must
 * include any holes in the mask.  Associate holes with
 * Dalvik register INVALID_VREG (0xFFFFU).
 */
void MipsCodegen::MarkPreservedSingle(CompilationUnit* cu, int s_reg, int reg)
{
  LOG(FATAL) << "No support yet for promoted FP regs";
}

void MipsCodegen::FlushRegWide(CompilationUnit* cu, int reg1, int reg2)
{
  RegisterInfo* info1 = GetRegInfo(cu, reg1);
  RegisterInfo* info2 = GetRegInfo(cu, reg2);
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
    if (SRegToVReg(cu, info2->s_reg) < SRegToVReg(cu, info1->s_reg))
      info1 = info2;
    int v_reg = SRegToVReg(cu, info1->s_reg);
    StoreBaseDispWide(cu, rMIPS_SP, VRegOffset(cu, v_reg), info1->reg, info1->partner);
  }
}

void MipsCodegen::FlushReg(CompilationUnit* cu, int reg)
{
  RegisterInfo* info = GetRegInfo(cu, reg);
  if (info->live && info->dirty) {
    info->dirty = false;
    int v_reg = SRegToVReg(cu, info->s_reg);
    StoreBaseDisp(cu, rMIPS_SP, VRegOffset(cu, v_reg), reg, kWord);
  }
}

/* Give access to the target-dependent FP register encoding to common code */
bool MipsCodegen::IsFpReg(int reg) {
  return MIPS_FPREG(reg);
}

/* Clobber all regs that might be used by an external C call */
void MipsCodegen::ClobberCalleeSave(CompilationUnit *cu)
{
  Clobber(cu, r_ZERO);
  Clobber(cu, r_AT);
  Clobber(cu, r_V0);
  Clobber(cu, r_V1);
  Clobber(cu, r_A0);
  Clobber(cu, r_A1);
  Clobber(cu, r_A2);
  Clobber(cu, r_A3);
  Clobber(cu, r_T0);
  Clobber(cu, r_T1);
  Clobber(cu, r_T2);
  Clobber(cu, r_T3);
  Clobber(cu, r_T4);
  Clobber(cu, r_T5);
  Clobber(cu, r_T6);
  Clobber(cu, r_T7);
  Clobber(cu, r_T8);
  Clobber(cu, r_T9);
  Clobber(cu, r_K0);
  Clobber(cu, r_K1);
  Clobber(cu, r_GP);
  Clobber(cu, r_FP);
  Clobber(cu, r_RA);
  Clobber(cu, r_F0);
  Clobber(cu, r_F1);
  Clobber(cu, r_F2);
  Clobber(cu, r_F3);
  Clobber(cu, r_F4);
  Clobber(cu, r_F5);
  Clobber(cu, r_F6);
  Clobber(cu, r_F7);
  Clobber(cu, r_F8);
  Clobber(cu, r_F9);
  Clobber(cu, r_F10);
  Clobber(cu, r_F11);
  Clobber(cu, r_F12);
  Clobber(cu, r_F13);
  Clobber(cu, r_F14);
  Clobber(cu, r_F15);
}

RegLocation MipsCodegen::GetReturnWideAlt(CompilationUnit* cu)
{
  UNIMPLEMENTED(FATAL) << "No GetReturnWideAlt for MIPS";
  RegLocation res = LocCReturnWide();
  return res;
}

RegLocation MipsCodegen::GetReturnAlt(CompilationUnit* cu)
{
  UNIMPLEMENTED(FATAL) << "No GetReturnAlt for MIPS";
  RegLocation res = LocCReturn();
  return res;
}

RegisterInfo* MipsCodegen::GetRegInfo(CompilationUnit* cu, int reg)
{
  return MIPS_FPREG(reg) ? &cu->reg_pool->FPRegs[reg & MIPS_FP_REG_MASK]
            : &cu->reg_pool->core_regs[reg];
}

/* To be used when explicitly managing register use */
void MipsCodegen::LockCallTemps(CompilationUnit* cu)
{
  LockTemp(cu, rMIPS_ARG0);
  LockTemp(cu, rMIPS_ARG1);
  LockTemp(cu, rMIPS_ARG2);
  LockTemp(cu, rMIPS_ARG3);
}

/* To be used when explicitly managing register use */
void MipsCodegen::FreeCallTemps(CompilationUnit* cu)
{
  FreeTemp(cu, rMIPS_ARG0);
  FreeTemp(cu, rMIPS_ARG1);
  FreeTemp(cu, rMIPS_ARG2);
  FreeTemp(cu, rMIPS_ARG3);
}

void MipsCodegen::GenMemBarrier(CompilationUnit *cu, MemBarrierKind barrier_kind)
{
#if ANDROID_SMP != 0
  NewLIR1(cu, kMipsSync, 0 /* Only stype currently supported */);
#endif
}

/*
 * Alloc a pair of core registers, or a double.  Low reg in low byte,
 * high reg in next byte.
 */
int MipsCodegen::AllocTypedTempPair(CompilationUnit *cu, bool fp_hint,
                  int reg_class)
{
  int high_reg;
  int low_reg;
  int res = 0;

  if (((reg_class == kAnyReg) && fp_hint) || (reg_class == kFPReg)) {
    low_reg = AllocTempDouble(cu);
    high_reg = low_reg + 1;
    res = (low_reg & 0xff) | ((high_reg & 0xff) << 8);
    return res;
  }

  low_reg = AllocTemp(cu);
  high_reg = AllocTemp(cu);
  res = (low_reg & 0xff) | ((high_reg & 0xff) << 8);
  return res;
}

int MipsCodegen::AllocTypedTemp(CompilationUnit *cu, bool fp_hint, int reg_class)
{
  if (((reg_class == kAnyReg) && fp_hint) || (reg_class == kFPReg))
{
    return AllocTempFloat(cu);
}
  return AllocTemp(cu);
}

void MipsCodegen::CompilerInitializeRegAlloc(CompilationUnit* cu)
{
  int num_regs = sizeof(core_regs)/sizeof(*core_regs);
  int num_reserved = sizeof(ReservedRegs)/sizeof(*ReservedRegs);
  int num_temps = sizeof(core_temps)/sizeof(*core_temps);
  int num_fp_regs = sizeof(FpRegs)/sizeof(*FpRegs);
  int num_fp_temps = sizeof(fp_temps)/sizeof(*fp_temps);
  RegisterPool *pool =
      static_cast<RegisterPool*>(NewMem(cu, sizeof(*pool), true, kAllocRegAlloc));
  cu->reg_pool = pool;
  pool->num_core_regs = num_regs;
  pool->core_regs = static_cast<RegisterInfo*>
     (NewMem(cu, num_regs * sizeof(*cu->reg_pool->core_regs), true, kAllocRegAlloc));
  pool->num_fp_regs = num_fp_regs;
  pool->FPRegs = static_cast<RegisterInfo*>
      (NewMem(cu, num_fp_regs * sizeof(*cu->reg_pool->FPRegs), true, kAllocRegAlloc));
  CompilerInitPool(pool->core_regs, core_regs, pool->num_core_regs);
  CompilerInitPool(pool->FPRegs, FpRegs, pool->num_fp_regs);
  // Keep special registers from being allocated
  for (int i = 0; i < num_reserved; i++) {
    if (NO_SUSPEND && (ReservedRegs[i] == rMIPS_SUSPEND)) {
      //To measure cost of suspend check
      continue;
    }
    MarkInUse(cu, ReservedRegs[i]);
  }
  // Mark temp regs - all others not in use can be used for promotion
  for (int i = 0; i < num_temps; i++) {
    MarkTemp(cu, core_temps[i]);
  }
  for (int i = 0; i < num_fp_temps; i++) {
    MarkTemp(cu, fp_temps[i]);
  }
  // Construct the alias map.
  cu->phi_alias_map = static_cast<int*>
      (NewMem(cu, cu->num_ssa_regs * sizeof(cu->phi_alias_map[0]), false, kAllocDFInfo));
  for (int i = 0; i < cu->num_ssa_regs; i++) {
    cu->phi_alias_map[i] = i;
  }
  for (MIR* phi = cu->phi_list; phi; phi = phi->meta.phi_next) {
    int def_reg = phi->ssa_rep->defs[0];
    for (int i = 0; i < phi->ssa_rep->num_uses; i++) {
       for (int j = 0; j < cu->num_ssa_regs; j++) {
         if (cu->phi_alias_map[j] == phi->ssa_rep->uses[i]) {
           cu->phi_alias_map[j] = def_reg;
         }
       }
    }
  }
}

void MipsCodegen::FreeRegLocTemps(CompilationUnit* cu, RegLocation rl_keep, RegLocation rl_free)
{
  if ((rl_free.low_reg != rl_keep.low_reg) && (rl_free.low_reg != rl_keep.high_reg) &&
    (rl_free.high_reg != rl_keep.low_reg) && (rl_free.high_reg != rl_keep.high_reg)) {
    // No overlap, free both
    FreeTemp(cu, rl_free.low_reg);
    FreeTemp(cu, rl_free.high_reg);
  }
}
/*
 * In the Arm code a it is typical to use the link register
 * to hold the target address.  However, for Mips we must
 * ensure that all branch instructions can be restarted if
 * there is a trap in the shadow.  Allocate a temp register.
 */
int MipsCodegen::LoadHelper(CompilationUnit* cu, int offset)
{
  LoadWordDisp(cu, rMIPS_SELF, offset, r_T9);
  return r_T9;
}

void MipsCodegen::SpillCoreRegs(CompilationUnit* cu)
{
  if (cu->num_core_spills == 0) {
    return;
  }
  uint32_t mask = cu->core_spill_mask;
  int offset = cu->num_core_spills * 4;
  OpRegImm(cu, kOpSub, rMIPS_SP, offset);
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      offset -= 4;
      StoreWordDisp(cu, rMIPS_SP, offset, reg);
    }
  }
}

void MipsCodegen::UnSpillCoreRegs(CompilationUnit* cu)
{
  if (cu->num_core_spills == 0) {
    return;
  }
  uint32_t mask = cu->core_spill_mask;
  int offset = cu->frame_size;
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      offset -= 4;
      LoadWordDisp(cu, rMIPS_SP, offset, reg);
    }
  }
  OpRegImm(cu, kOpAdd, rMIPS_SP, cu->frame_size);
}

bool MipsCodegen::IsUnconditionalBranch(LIR* lir)
{
  return (lir->opcode == kMipsB);
}

/* Common initialization routine for an architecture family */
bool InitMipsCodegen(CompilationUnit* cu)
{
  cu->cg.reset(new MipsCodegen());
  for (int i = 0; i < kMipsLast; i++) {
    if (MipsCodegen::EncodingMap[i].opcode != i) {
      LOG(FATAL) << "Encoding order for " << MipsCodegen::EncodingMap[i].name
                 << " is wrong: expecting " << i << ", seeing "
                 << static_cast<int>(MipsCodegen::EncodingMap[i].opcode);
    }
  }
  return true;
}

uint64_t MipsCodegen::GetTargetInstFlags(int opcode)
{
  return MipsCodegen::EncodingMap[opcode].flags;
}

const char* MipsCodegen::GetTargetInstName(int opcode)
{
  return MipsCodegen::EncodingMap[opcode].name;
}

const char* MipsCodegen::GetTargetInstFmt(int opcode)
{
  return MipsCodegen::EncodingMap[opcode].fmt;
}

} // namespace art
