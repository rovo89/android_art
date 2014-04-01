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

#include "codegen_arm.h"

#include <inttypes.h>

#include <string>

#include "dex/compiler_internals.h"
#include "dex/quick/mir_to_lir-inl.h"

namespace art {

static int core_regs[] = {r0, r1, r2, r3, rARM_SUSPEND, r5, r6, r7, r8, rARM_SELF, r10,
                         r11, r12, rARM_SP, rARM_LR, rARM_PC};
static int ReservedRegs[] = {rARM_SUSPEND, rARM_SELF, rARM_SP, rARM_LR, rARM_PC};
static int FpRegs[] = {fr0, fr1, fr2, fr3, fr4, fr5, fr6, fr7,
                       fr8, fr9, fr10, fr11, fr12, fr13, fr14, fr15,
                       fr16, fr17, fr18, fr19, fr20, fr21, fr22, fr23,
                       fr24, fr25, fr26, fr27, fr28, fr29, fr30, fr31};
static int core_temps[] = {r0, r1, r2, r3, r12};
static int fp_temps[] = {fr0, fr1, fr2, fr3, fr4, fr5, fr6, fr7,
                        fr8, fr9, fr10, fr11, fr12, fr13, fr14, fr15};

RegLocation ArmMir2Lir::LocCReturn() {
  return arm_loc_c_return;
}

RegLocation ArmMir2Lir::LocCReturnWide() {
  return arm_loc_c_return_wide;
}

RegLocation ArmMir2Lir::LocCReturnFloat() {
  return arm_loc_c_return_float;
}

RegLocation ArmMir2Lir::LocCReturnDouble() {
  return arm_loc_c_return_double;
}

// Return a target-dependent special register.
RegStorage ArmMir2Lir::TargetReg(SpecialTargetRegister reg) {
  int res_reg = RegStorage::kInvalidRegVal;
  switch (reg) {
    case kSelf: res_reg = rARM_SELF; break;
    case kSuspend: res_reg =  rARM_SUSPEND; break;
    case kLr: res_reg =  rARM_LR; break;
    case kPc: res_reg =  rARM_PC; break;
    case kSp: res_reg =  rARM_SP; break;
    case kArg0: res_reg = rARM_ARG0; break;
    case kArg1: res_reg = rARM_ARG1; break;
    case kArg2: res_reg = rARM_ARG2; break;
    case kArg3: res_reg = rARM_ARG3; break;
    case kFArg0: res_reg = rARM_FARG0; break;
    case kFArg1: res_reg = rARM_FARG1; break;
    case kFArg2: res_reg = rARM_FARG2; break;
    case kFArg3: res_reg = rARM_FARG3; break;
    case kRet0: res_reg = rARM_RET0; break;
    case kRet1: res_reg = rARM_RET1; break;
    case kInvokeTgt: res_reg = rARM_INVOKE_TGT; break;
    case kHiddenArg: res_reg = r12; break;
    case kHiddenFpArg: res_reg = RegStorage::kInvalidRegVal; break;
    case kCount: res_reg = rARM_COUNT; break;
  }
  return RegStorage::Solo32(res_reg);
}

RegStorage ArmMir2Lir::GetArgMappingToPhysicalReg(int arg_num) {
  // For the 32-bit internal ABI, the first 3 arguments are passed in registers.
  switch (arg_num) {
    case 0:
      return rs_rARM_ARG1;
    case 1:
      return rs_rARM_ARG2;
    case 2:
      return rs_rARM_ARG3;
    default:
      return RegStorage::InvalidReg();
  }
}

// Create a double from a pair of singles.
int ArmMir2Lir::S2d(int low_reg, int high_reg) {
  return ARM_S2D(low_reg, high_reg);
}

// Return mask to strip off fp reg flags and bias.
uint32_t ArmMir2Lir::FpRegMask() {
  return ARM_FP_REG_MASK;
}

// True if both regs single, both core or both double.
bool ArmMir2Lir::SameRegType(int reg1, int reg2) {
  return (ARM_REGTYPE(reg1) == ARM_REGTYPE(reg2));
}

/*
 * Decode the register id.
 */
uint64_t ArmMir2Lir::GetRegMaskCommon(int reg) {
  uint64_t seed;
  int shift;
  int reg_id;


  reg_id = reg & 0x1f;
  /* Each double register is equal to a pair of single-precision FP registers */
  seed = ARM_DOUBLEREG(reg) ? 3 : 1;
  /* FP register starts at bit position 16 */
  shift = ARM_FPREG(reg) ? kArmFPReg0 : 0;
  /* Expand the double register id into single offset */
  shift += reg_id;
  return (seed << shift);
}

uint64_t ArmMir2Lir::GetPCUseDefEncoding() {
  return ENCODE_ARM_REG_PC;
}

// Thumb2 specific setup.  TODO: inline?:
void ArmMir2Lir::SetupTargetResourceMasks(LIR* lir, uint64_t flags) {
  DCHECK_EQ(cu_->instruction_set, kThumb2);
  DCHECK(!lir->flags.use_def_invalid);

  int opcode = lir->opcode;

  // These flags are somewhat uncommon - bypass if we can.
  if ((flags & (REG_DEF_SP | REG_USE_SP | REG_DEF_LIST0 | REG_DEF_LIST1 |
                REG_DEF_FPCS_LIST0 | REG_DEF_FPCS_LIST2 | REG_USE_PC | IS_IT | REG_USE_LIST0 |
                REG_USE_LIST1 | REG_USE_FPCS_LIST0 | REG_USE_FPCS_LIST2 | REG_DEF_LR)) != 0) {
    if (flags & REG_DEF_SP) {
      lir->u.m.def_mask |= ENCODE_ARM_REG_SP;
    }

    if (flags & REG_USE_SP) {
      lir->u.m.use_mask |= ENCODE_ARM_REG_SP;
    }

    if (flags & REG_DEF_LIST0) {
      lir->u.m.def_mask |= ENCODE_ARM_REG_LIST(lir->operands[0]);
    }

    if (flags & REG_DEF_LIST1) {
      lir->u.m.def_mask |= ENCODE_ARM_REG_LIST(lir->operands[1]);
    }

    if (flags & REG_DEF_FPCS_LIST0) {
      lir->u.m.def_mask |= ENCODE_ARM_REG_FPCS_LIST(lir->operands[0]);
    }

    if (flags & REG_DEF_FPCS_LIST2) {
      for (int i = 0; i < lir->operands[2]; i++) {
        SetupRegMask(&lir->u.m.def_mask, lir->operands[1] + i);
      }
    }

    if (flags & REG_USE_PC) {
      lir->u.m.use_mask |= ENCODE_ARM_REG_PC;
    }

    /* Conservatively treat the IT block */
    if (flags & IS_IT) {
      lir->u.m.def_mask = ENCODE_ALL;
    }

    if (flags & REG_USE_LIST0) {
      lir->u.m.use_mask |= ENCODE_ARM_REG_LIST(lir->operands[0]);
    }

    if (flags & REG_USE_LIST1) {
      lir->u.m.use_mask |= ENCODE_ARM_REG_LIST(lir->operands[1]);
    }

    if (flags & REG_USE_FPCS_LIST0) {
      lir->u.m.use_mask |= ENCODE_ARM_REG_FPCS_LIST(lir->operands[0]);
    }

    if (flags & REG_USE_FPCS_LIST2) {
      for (int i = 0; i < lir->operands[2]; i++) {
        SetupRegMask(&lir->u.m.use_mask, lir->operands[1] + i);
      }
    }
    /* Fixup for kThumbPush/lr and kThumbPop/pc */
    if (opcode == kThumbPush || opcode == kThumbPop) {
      uint64_t r8Mask = GetRegMaskCommon(r8);
      if ((opcode == kThumbPush) && (lir->u.m.use_mask & r8Mask)) {
        lir->u.m.use_mask &= ~r8Mask;
        lir->u.m.use_mask |= ENCODE_ARM_REG_LR;
      } else if ((opcode == kThumbPop) && (lir->u.m.def_mask & r8Mask)) {
        lir->u.m.def_mask &= ~r8Mask;
        lir->u.m.def_mask |= ENCODE_ARM_REG_PC;
      }
    }
    if (flags & REG_DEF_LR) {
      lir->u.m.def_mask |= ENCODE_ARM_REG_LR;
    }
  }
}

ArmConditionCode ArmMir2Lir::ArmConditionEncoding(ConditionCode ccode) {
  ArmConditionCode res;
  switch (ccode) {
    case kCondEq: res = kArmCondEq; break;
    case kCondNe: res = kArmCondNe; break;
    case kCondCs: res = kArmCondCs; break;
    case kCondCc: res = kArmCondCc; break;
    case kCondUlt: res = kArmCondCc; break;
    case kCondUge: res = kArmCondCs; break;
    case kCondMi: res = kArmCondMi; break;
    case kCondPl: res = kArmCondPl; break;
    case kCondVs: res = kArmCondVs; break;
    case kCondVc: res = kArmCondVc; break;
    case kCondHi: res = kArmCondHi; break;
    case kCondLs: res = kArmCondLs; break;
    case kCondGe: res = kArmCondGe; break;
    case kCondLt: res = kArmCondLt; break;
    case kCondGt: res = kArmCondGt; break;
    case kCondLe: res = kArmCondLe; break;
    case kCondAl: res = kArmCondAl; break;
    case kCondNv: res = kArmCondNv; break;
    default:
      LOG(FATAL) << "Bad condition code " << ccode;
      res = static_cast<ArmConditionCode>(0);  // Quiet gcc
  }
  return res;
}

static const char* core_reg_names[16] = {
  "r0",
  "r1",
  "r2",
  "r3",
  "r4",
  "r5",
  "r6",
  "r7",
  "r8",
  "rSELF",
  "r10",
  "r11",
  "r12",
  "sp",
  "lr",
  "pc",
};


static const char* shift_names[4] = {
  "lsl",
  "lsr",
  "asr",
  "ror"};

/* Decode and print a ARM register name */
static char* DecodeRegList(int opcode, int vector, char* buf, size_t buf_size) {
  int i;
  bool printed = false;
  buf[0] = 0;
  for (i = 0; i < 16; i++, vector >>= 1) {
    if (vector & 0x1) {
      int reg_id = i;
      if (opcode == kThumbPush && i == 8) {
        reg_id = r14lr;
      } else if (opcode == kThumbPop && i == 8) {
        reg_id = r15pc;
      }
      if (printed) {
        snprintf(buf + strlen(buf), buf_size - strlen(buf), ", r%d", reg_id);
      } else {
        printed = true;
        snprintf(buf, buf_size, "r%d", reg_id);
      }
    }
  }
  return buf;
}

static char*  DecodeFPCSRegList(int count, int base, char* buf, size_t buf_size) {
  snprintf(buf, buf_size, "s%d", base);
  for (int i = 1; i < count; i++) {
    snprintf(buf + strlen(buf), buf_size - strlen(buf), ", s%d", base + i);
  }
  return buf;
}

static int32_t ExpandImmediate(int value) {
  int32_t mode = (value & 0xf00) >> 8;
  uint32_t bits = value & 0xff;
  switch (mode) {
    case 0:
      return bits;
     case 1:
      return (bits << 16) | bits;
     case 2:
      return (bits << 24) | (bits << 8);
     case 3:
      return (bits << 24) | (bits << 16) | (bits << 8) | bits;
    default:
      break;
  }
  bits = (bits | 0x80) << 24;
  return bits >> (((value & 0xf80) >> 7) - 8);
}

const char* cc_names[] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                         "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};
/*
 * Interpret a format string and build a string no longer than size
 * See format key in Assemble.c.
 */
std::string ArmMir2Lir::BuildInsnString(const char* fmt, LIR* lir, unsigned char* base_addr) {
  std::string buf;
  int i;
  const char* fmt_end = &fmt[strlen(fmt)];
  char tbuf[256];
  const char* name;
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
         DCHECK_LT(static_cast<unsigned>(nc-'0'), 4U);
         operand = lir->operands[nc-'0'];
         switch (*fmt++) {
           case 'H':
             if (operand != 0) {
               snprintf(tbuf, arraysize(tbuf), ", %s %d", shift_names[operand & 0x3], operand >> 2);
             } else {
               strcpy(tbuf, "");
             }
             break;
           case 'B':
             switch (operand) {
               case kSY:
                 name = "sy";
                 break;
               case kST:
                 name = "st";
                 break;
               case kISH:
                 name = "ish";
                 break;
               case kISHST:
                 name = "ishst";
                 break;
               case kNSH:
                 name = "nsh";
                 break;
               case kNSHST:
                 name = "shst";
                 break;
               default:
                 name = "DecodeError2";
                 break;
             }
             strcpy(tbuf, name);
             break;
           case 'b':
             strcpy(tbuf, "0000");
             for (i = 3; i >= 0; i--) {
               tbuf[i] += operand & 1;
               operand >>= 1;
             }
             break;
           case 'n':
             operand = ~ExpandImmediate(operand);
             snprintf(tbuf, arraysize(tbuf), "%d [%#x]", operand, operand);
             break;
           case 'm':
             operand = ExpandImmediate(operand);
             snprintf(tbuf, arraysize(tbuf), "%d [%#x]", operand, operand);
             break;
           case 's':
             snprintf(tbuf, arraysize(tbuf), "s%d", operand & ARM_FP_REG_MASK);
             break;
           case 'S':
             snprintf(tbuf, arraysize(tbuf), "d%d", (operand & ARM_FP_REG_MASK) >> 1);
             break;
           case 'h':
             snprintf(tbuf, arraysize(tbuf), "%04x", operand);
             break;
           case 'M':
           case 'd':
             snprintf(tbuf, arraysize(tbuf), "%d", operand);
             break;
           case 'C':
             DCHECK_LT(operand, static_cast<int>(
                 sizeof(core_reg_names)/sizeof(core_reg_names[0])));
             snprintf(tbuf, arraysize(tbuf), "%s", core_reg_names[operand]);
             break;
           case 'E':
             snprintf(tbuf, arraysize(tbuf), "%d", operand*4);
             break;
           case 'F':
             snprintf(tbuf, arraysize(tbuf), "%d", operand*2);
             break;
           case 'c':
             strcpy(tbuf, cc_names[operand]);
             break;
           case 't':
             snprintf(tbuf, arraysize(tbuf), "0x%08" PRIxPTR " (L%p)",
                 reinterpret_cast<uintptr_t>(base_addr) + lir->offset + 4 + (operand << 1),
                 lir->target);
             break;
           case 'u': {
             int offset_1 = lir->operands[0];
             int offset_2 = NEXT_LIR(lir)->operands[0];
             uintptr_t target =
                 (((reinterpret_cast<uintptr_t>(base_addr) + lir->offset + 4) &
                 ~3) + (offset_1 << 21 >> 9) + (offset_2 << 1)) &
                 0xfffffffc;
             snprintf(tbuf, arraysize(tbuf), "%p", reinterpret_cast<void *>(target));
             break;
          }

           /* Nothing to print for BLX_2 */
           case 'v':
             strcpy(tbuf, "see above");
             break;
           case 'R':
             DecodeRegList(lir->opcode, operand, tbuf, arraysize(tbuf));
             break;
           case 'P':
             DecodeFPCSRegList(operand, 16, tbuf, arraysize(tbuf));
             break;
           case 'Q':
             DecodeFPCSRegList(operand, 0, tbuf, arraysize(tbuf));
             break;
           default:
             strcpy(tbuf, "DecodeError1");
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

void ArmMir2Lir::DumpResourceMask(LIR* arm_lir, uint64_t mask, const char* prefix) {
  char buf[256];
  buf[0] = 0;

  if (mask == ENCODE_ALL) {
    strcpy(buf, "all");
  } else {
    char num[8];
    int i;

    for (i = 0; i < kArmRegEnd; i++) {
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
    if (arm_lir && (mask & ENCODE_DALVIK_REG)) {
      snprintf(buf + strlen(buf), arraysize(buf) - strlen(buf), "dr%d%s",
               DECODE_ALIAS_INFO_REG(arm_lir->flags.alias_info),
               DECODE_ALIAS_INFO_WIDE(arm_lir->flags.alias_info) ? "(+1)" : "");
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
    LOG(INFO) << prefix << ": " << buf;
  }
}

bool ArmMir2Lir::IsUnconditionalBranch(LIR* lir) {
  return ((lir->opcode == kThumbBUncond) || (lir->opcode == kThumb2BUncond));
}

ArmMir2Lir::ArmMir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena)
    : Mir2Lir(cu, mir_graph, arena) {
  // Sanity check - make sure encoding map lines up.
  for (int i = 0; i < kArmLast; i++) {
    if (ArmMir2Lir::EncodingMap[i].opcode != i) {
      LOG(FATAL) << "Encoding order for " << ArmMir2Lir::EncodingMap[i].name
                 << " is wrong: expecting " << i << ", seeing "
                 << static_cast<int>(ArmMir2Lir::EncodingMap[i].opcode);
    }
  }
}

Mir2Lir* ArmCodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                          ArenaAllocator* const arena) {
  return new ArmMir2Lir(cu, mir_graph, arena);
}

// Alloc a pair of core registers, or a double.
RegStorage ArmMir2Lir::AllocTypedTempWide(bool fp_hint, int reg_class) {
  if (((reg_class == kAnyReg) && fp_hint) || (reg_class == kFPReg)) {
    return AllocTempDouble();
  } else {
    RegStorage low_reg = AllocTemp();
    RegStorage high_reg = AllocTemp();
    return RegStorage::MakeRegPair(low_reg, high_reg);
  }
}

RegStorage ArmMir2Lir::AllocTypedTemp(bool fp_hint, int reg_class) {
  if (((reg_class == kAnyReg) && fp_hint) || (reg_class == kFPReg))
    return AllocTempFloat();
  return AllocTemp();
}

void ArmMir2Lir::CompilerInitializeRegAlloc() {
  int num_regs = sizeof(core_regs)/sizeof(*core_regs);
  int num_reserved = sizeof(ReservedRegs)/sizeof(*ReservedRegs);
  int num_temps = sizeof(core_temps)/sizeof(*core_temps);
  int num_fp_regs = sizeof(FpRegs)/sizeof(*FpRegs);
  int num_fp_temps = sizeof(fp_temps)/sizeof(*fp_temps);
  reg_pool_ = static_cast<RegisterPool*>(arena_->Alloc(sizeof(*reg_pool_),
                                                       kArenaAllocRegAlloc));
  reg_pool_->num_core_regs = num_regs;
  reg_pool_->core_regs = reinterpret_cast<RegisterInfo*>
      (arena_->Alloc(num_regs * sizeof(*reg_pool_->core_regs), kArenaAllocRegAlloc));
  reg_pool_->num_fp_regs = num_fp_regs;
  reg_pool_->FPRegs = static_cast<RegisterInfo*>
      (arena_->Alloc(num_fp_regs * sizeof(*reg_pool_->FPRegs), kArenaAllocRegAlloc));
  CompilerInitPool(reg_pool_->core_regs, core_regs, reg_pool_->num_core_regs);
  CompilerInitPool(reg_pool_->FPRegs, FpRegs, reg_pool_->num_fp_regs);
  // Keep special registers from being allocated
  for (int i = 0; i < num_reserved; i++) {
    if (NO_SUSPEND && (ReservedRegs[i] == rARM_SUSPEND)) {
      // To measure cost of suspend check
      continue;
    }
    MarkInUse(ReservedRegs[i]);
  }
  // Mark temp regs - all others not in use can be used for promotion
  for (int i = 0; i < num_temps; i++) {
    MarkTemp(core_temps[i]);
  }
  for (int i = 0; i < num_fp_temps; i++) {
    MarkTemp(fp_temps[i]);
  }

  // Start allocation at r2 in an attempt to avoid clobbering return values
  reg_pool_->next_core_reg = r2;
}

void ArmMir2Lir::FreeRegLocTemps(RegLocation rl_keep, RegLocation rl_free) {
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
 * TUNING: is true leaf?  Can't just use METHOD_IS_LEAF to determine as some
 * instructions might call out to C/assembly helper functions.  Until
 * machinery is in place, always spill lr.
 */

void ArmMir2Lir::AdjustSpillMask() {
  core_spill_mask_ |= (1 << rARM_LR);
  num_core_spills_++;
}

/*
 * Mark a callee-save fp register as promoted.  Note that
 * vpush/vpop uses contiguous register lists so we must
 * include any holes in the mask.  Associate holes with
 * Dalvik register INVALID_VREG (0xFFFFU).
 */
void ArmMir2Lir::MarkPreservedSingle(int v_reg, int reg) {
  DCHECK_GE(reg, ARM_FP_REG_MASK + ARM_FP_CALLEE_SAVE_BASE);
  reg = (reg & ARM_FP_REG_MASK) - ARM_FP_CALLEE_SAVE_BASE;
  // Ensure fp_vmap_table is large enough
  int table_size = fp_vmap_table_.size();
  for (int i = table_size; i < (reg + 1); i++) {
    fp_vmap_table_.push_back(INVALID_VREG);
  }
  // Add the current mapping
  fp_vmap_table_[reg] = v_reg;
  // Size of fp_vmap_table is high-water mark, use to set mask
  num_fp_spills_ = fp_vmap_table_.size();
  fp_spill_mask_ = ((1 << num_fp_spills_) - 1) << ARM_FP_CALLEE_SAVE_BASE;
}

void ArmMir2Lir::FlushRegWide(RegStorage reg) {
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
    if (mir_graph_->SRegToVReg(info2->s_reg) <
      mir_graph_->SRegToVReg(info1->s_reg))
      info1 = info2;
    int v_reg = mir_graph_->SRegToVReg(info1->s_reg);
    StoreBaseDispWide(rs_rARM_SP, VRegOffset(v_reg),
                      RegStorage(RegStorage::k64BitPair, info1->reg, info1->partner));
  }
}

void ArmMir2Lir::FlushReg(RegStorage reg) {
  DCHECK(!reg.IsPair());
  RegisterInfo* info = GetRegInfo(reg.GetReg());
  if (info->live && info->dirty) {
    info->dirty = false;
    int v_reg = mir_graph_->SRegToVReg(info->s_reg);
    StoreBaseDisp(rs_rARM_SP, VRegOffset(v_reg), reg, kWord);
  }
}

/* Give access to the target-dependent FP register encoding to common code */
bool ArmMir2Lir::IsFpReg(int reg) {
  return ARM_FPREG(reg);
}

bool ArmMir2Lir::IsFpReg(RegStorage reg) {
  return IsFpReg(reg.IsPair() ? reg.GetLowReg() : reg.GetReg());
}

/* Clobber all regs that might be used by an external C call */
void ArmMir2Lir::ClobberCallerSave() {
  Clobber(r0);
  Clobber(r1);
  Clobber(r2);
  Clobber(r3);
  Clobber(r12);
  Clobber(r14lr);
  Clobber(fr0);
  Clobber(fr1);
  Clobber(fr2);
  Clobber(fr3);
  Clobber(fr4);
  Clobber(fr5);
  Clobber(fr6);
  Clobber(fr7);
  Clobber(fr8);
  Clobber(fr9);
  Clobber(fr10);
  Clobber(fr11);
  Clobber(fr12);
  Clobber(fr13);
  Clobber(fr14);
  Clobber(fr15);
}

RegLocation ArmMir2Lir::GetReturnWideAlt() {
  RegLocation res = LocCReturnWide();
  res.reg.SetReg(r2);
  res.reg.SetHighReg(r3);
  Clobber(r2);
  Clobber(r3);
  MarkInUse(r2);
  MarkInUse(r3);
  MarkPair(res.reg.GetLowReg(), res.reg.GetHighReg());
  return res;
}

RegLocation ArmMir2Lir::GetReturnAlt() {
  RegLocation res = LocCReturn();
  res.reg.SetReg(r1);
  Clobber(r1);
  MarkInUse(r1);
  return res;
}

/* To be used when explicitly managing register use */
void ArmMir2Lir::LockCallTemps() {
  LockTemp(r0);
  LockTemp(r1);
  LockTemp(r2);
  LockTemp(r3);
}

/* To be used when explicitly managing register use */
void ArmMir2Lir::FreeCallTemps() {
  FreeTemp(r0);
  FreeTemp(r1);
  FreeTemp(r2);
  FreeTemp(r3);
}

RegStorage ArmMir2Lir::LoadHelper(ThreadOffset<4> offset) {
  LoadWordDisp(rs_rARM_SELF, offset.Int32Value(), rs_rARM_LR);
  return rs_rARM_LR;
}

LIR* ArmMir2Lir::CheckSuspendUsingLoad() {
  RegStorage tmp = rs_r0;
  LoadWordDisp(rs_rARM_SELF, Thread::ThreadSuspendTriggerOffset<4>().Int32Value(), tmp);
  LIR* load2 = LoadWordDisp(tmp, 0, tmp);
  return load2;
}

uint64_t ArmMir2Lir::GetTargetInstFlags(int opcode) {
  DCHECK(!IsPseudoLirOp(opcode));
  return ArmMir2Lir::EncodingMap[opcode].flags;
}

const char* ArmMir2Lir::GetTargetInstName(int opcode) {
  DCHECK(!IsPseudoLirOp(opcode));
  return ArmMir2Lir::EncodingMap[opcode].name;
}

const char* ArmMir2Lir::GetTargetInstFmt(int opcode) {
  DCHECK(!IsPseudoLirOp(opcode));
  return ArmMir2Lir::EncodingMap[opcode].fmt;
}

}  // namespace art
