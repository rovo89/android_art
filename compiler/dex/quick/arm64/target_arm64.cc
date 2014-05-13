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

#include "codegen_arm64.h"

#include <inttypes.h>

#include <string>

#include "dex/compiler_internals.h"
#include "dex/quick/mir_to_lir-inl.h"

namespace art {

// TODO: rework this when c++11 support allows.
static const RegStorage core_regs_arr[] =
    {rs_x0, rs_x1, rs_x2, rs_x3, rs_x4, rs_x5, rs_x6, rs_x7,
     rs_x8, rs_x9, rs_x10, rs_x11, rs_x12, rs_x13, rs_x14, rs_x15,
     rs_x16, rs_x17, rs_x18, rs_x19, rs_x20, rs_x21, rs_x22, rs_x23,
     rs_x24, rs_x25, rs_x26, rs_x27, rs_x28, rs_x29, rs_x30, rs_x31};
static const RegStorage sp_regs_arr[] =
    {rs_f0, rs_f1, rs_f2, rs_f3, rs_f4, rs_f5, rs_f6, rs_f7,
     rs_f8, rs_f9, rs_f10, rs_f11, rs_f12, rs_f13, rs_f14, rs_f15,
     rs_f16, rs_f17, rs_f18, rs_f19, rs_f20, rs_f21, rs_f22, rs_f23,
     rs_f24, rs_f25, rs_f26, rs_f27, rs_f28, rs_f29, rs_f30, rs_f31};
static const RegStorage dp_regs_arr[] =
    {rs_d0, rs_d1, rs_d2, rs_d3, rs_d4, rs_d5, rs_d6, rs_d7,
     rs_d8, rs_d9, rs_d10, rs_d11, rs_d12, rs_d13, rs_d14, rs_d15};
static const RegStorage reserved_regs_arr[] =
    {rs_rA64_SUSPEND, rs_rA64_SELF, rs_rA64_SP, rs_rA64_LR};
static const RegStorage core_temps_arr[] =
    {rs_x0, rs_x1, rs_x2, rs_x3, rs_x12};
static const RegStorage sp_temps_arr[] =
    {rs_f0, rs_f1, rs_f2, rs_f3, rs_f4, rs_f5, rs_f6, rs_f7,
     rs_f8, rs_f9, rs_f10, rs_f11, rs_f12, rs_f13, rs_f14, rs_f15};
static const RegStorage dp_temps_arr[] =
    {rs_d0, rs_d1, rs_d2, rs_d3, rs_d4, rs_d5, rs_d6, rs_d7};

static const std::vector<RegStorage> core_regs(core_regs_arr,
    core_regs_arr + arraysize(core_regs_arr));
static const std::vector<RegStorage> sp_regs(sp_regs_arr,
    sp_regs_arr + arraysize(sp_regs_arr));
static const std::vector<RegStorage> dp_regs(dp_regs_arr,
    dp_regs_arr + arraysize(dp_regs_arr));
static const std::vector<RegStorage> reserved_regs(reserved_regs_arr,
    reserved_regs_arr + arraysize(reserved_regs_arr));
static const std::vector<RegStorage> core_temps(core_temps_arr,
    core_temps_arr + arraysize(core_temps_arr));
static const std::vector<RegStorage> sp_temps(sp_temps_arr, sp_temps_arr + arraysize(sp_temps_arr));
static const std::vector<RegStorage> dp_temps(dp_temps_arr, dp_temps_arr + arraysize(dp_temps_arr));

RegLocation Arm64Mir2Lir::LocCReturn() {
  return arm_loc_c_return;
}

RegLocation Arm64Mir2Lir::LocCReturnWide() {
  return arm_loc_c_return_wide;
}

RegLocation Arm64Mir2Lir::LocCReturnFloat() {
  return arm_loc_c_return_float;
}

RegLocation Arm64Mir2Lir::LocCReturnDouble() {
  return arm_loc_c_return_double;
}

// Return a target-dependent special register.
RegStorage Arm64Mir2Lir::TargetReg(SpecialTargetRegister reg) {
  // TODO(Arm64): this function doesn't work for hard-float ABI.
  RegStorage res_reg = RegStorage::InvalidReg();
  switch (reg) {
    case kSelf: res_reg = rs_rA64_SELF; break;
    case kSuspend: res_reg = rs_rA64_SUSPEND; break;
    case kLr: res_reg =  rs_rA64_LR; break;
    case kPc: res_reg = RegStorage::InvalidReg(); break;
    case kSp: res_reg =  rs_rA64_SP; break;
    case kArg0: res_reg = rs_x0; break;
    case kArg1: res_reg = rs_x1; break;
    case kArg2: res_reg = rs_x2; break;
    case kArg3: res_reg = rs_x3; break;
    case kFArg0: res_reg = rs_f0; break;
    case kFArg1: res_reg = rs_f1; break;
    case kFArg2: res_reg = rs_f2; break;
    case kFArg3: res_reg = rs_f3; break;
    case kRet0: res_reg = rs_x0; break;
    case kRet1: res_reg = rs_x0; break;
    case kInvokeTgt: res_reg = rs_rA64_LR; break;
    case kHiddenArg: res_reg = rs_x12; break;
    case kHiddenFpArg: res_reg = RegStorage::InvalidReg(); break;
    case kCount: res_reg = RegStorage::InvalidReg(); break;
  }
  return res_reg;
}

RegStorage Arm64Mir2Lir::GetArgMappingToPhysicalReg(int arg_num) {
  return RegStorage::InvalidReg();
}

/*
 * Decode the register id. This routine makes assumptions on the encoding made by RegStorage.
 */
uint64_t Arm64Mir2Lir::GetRegMaskCommon(RegStorage reg) {
  // TODO(Arm64): this function depends too much on the internal RegStorage encoding. Refactor.

  int reg_raw = reg.GetRawBits();
  // Check if the shape mask is zero (i.e. invalid).
  if (UNLIKELY(reg == rs_wzr || reg == rs_xzr)) {
    // The zero register is not a true register. It is just an immediate zero.
    return 0;
  }

  return UINT64_C(1) << (reg_raw & RegStorage::kRegTypeMask);
}

uint64_t Arm64Mir2Lir::GetPCUseDefEncoding() {
  LOG(FATAL) << "Unexpected call to GetPCUseDefEncoding for Arm64";
  return 0ULL;
}

// Arm64 specific setup.  TODO: inline?:
void Arm64Mir2Lir::SetupTargetResourceMasks(LIR* lir, uint64_t flags) {
  DCHECK_EQ(cu_->instruction_set, kArm64);
  DCHECK(!lir->flags.use_def_invalid);

  // These flags are somewhat uncommon - bypass if we can.
  if ((flags & (REG_DEF_SP | REG_USE_SP | REG_DEF_LR)) != 0) {
    if (flags & REG_DEF_SP) {
      lir->u.m.def_mask |= ENCODE_ARM_REG_SP;
    }

    if (flags & REG_USE_SP) {
      lir->u.m.use_mask |= ENCODE_ARM_REG_SP;
    }

    if (flags & REG_DEF_LR) {
      lir->u.m.def_mask |= ENCODE_ARM_REG_LR;
    }
  }
}

ArmConditionCode Arm64Mir2Lir::ArmConditionEncoding(ConditionCode ccode) {
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

static const char *shift_names[4] = {
  "lsl",
  "lsr",
  "asr",
  "ror"
};

static const char* extend_names[8] = {
  "uxtb",
  "uxth",
  "uxtw",
  "uxtx",
  "sxtb",
  "sxth",
  "sxtw",
  "sxtx",
};

/* Decode and print a register extension (e.g. ", uxtb #1") */
static void DecodeRegExtendOrShift(int operand, char *buf, size_t buf_size) {
  if ((operand & (1 << 6)) == 0) {
    const char *shift_name = shift_names[(operand >> 7) & 0x3];
    int amount = operand & 0x3f;
    snprintf(buf, buf_size, ", %s #%d", shift_name, amount);
  } else {
    const char *extend_name = extend_names[(operand >> 3) & 0x7];
    int amount = operand & 0x7;
    if (amount == 0) {
      snprintf(buf, buf_size, ", %s", extend_name);
    } else {
      snprintf(buf, buf_size, ", %s #%d", extend_name, amount);
    }
  }
}

#define BIT_MASK(w) ((UINT64_C(1) << (w)) - UINT64_C(1))

static uint64_t RotateRight(uint64_t value, unsigned rotate, unsigned width) {
  DCHECK_LE(width, 64U);
  rotate &= 63;
  value = value & BIT_MASK(width);
  return ((value & BIT_MASK(rotate)) << (width - rotate)) | (value >> rotate);
}

static uint64_t RepeatBitsAcrossReg(bool is_wide, uint64_t value, unsigned width) {
  unsigned i;
  unsigned reg_size = (is_wide) ? 64 : 32;
  uint64_t result = value & BIT_MASK(width);
  DCHECK_NE(width, reg_size);
  for (i = width; i < reg_size; i *= 2) {
    result |= (result << i);
  }
  DCHECK_EQ(i, reg_size);
  return result;
}

/**
 * @brief Decode an immediate in the form required by logical instructions.
 *
 * @param is_wide Whether @p value encodes a 64-bit (as opposed to 32-bit) immediate.
 * @param value The encoded logical immediates that is to be decoded.
 * @return The decoded logical immediate.
 * @note This is the inverse of Arm64Mir2Lir::EncodeLogicalImmediate().
 */
uint64_t Arm64Mir2Lir::DecodeLogicalImmediate(bool is_wide, int value) {
  unsigned n     = (value >> 12) & 0x01;
  unsigned imm_r = (value >>  6) & 0x3f;
  unsigned imm_s = (value >>  0) & 0x3f;

  // An integer is constructed from the n, imm_s and imm_r bits according to
  // the following table:
  //
  // N   imms immr  size S             R
  // 1 ssssss rrrrrr 64  UInt(ssssss) UInt(rrrrrr)
  // 0 0sssss xrrrrr 32  UInt(sssss)  UInt(rrrrr)
  // 0 10ssss xxrrrr 16  UInt(ssss)   UInt(rrrr)
  // 0 110sss xxxrrr 8   UInt(sss)    UInt(rrr)
  // 0 1110ss xxxxrr 4   UInt(ss)     UInt(rr)
  // 0 11110s xxxxxr 2   UInt(s)      UInt(r)
  // (s bits must not be all set)
  //
  // A pattern is constructed of size bits, where the least significant S+1
  // bits are set. The pattern is rotated right by R, and repeated across a
  // 32 or 64-bit value, depending on destination register width.

  if (n == 1) {
    DCHECK_NE(imm_s, 0x3fU);
    uint64_t bits = BIT_MASK(imm_s + 1);
    return RotateRight(bits, imm_r, 64);
  } else {
    DCHECK_NE((imm_s >> 1), 0x1fU);
    for (unsigned width = 0x20; width >= 0x2; width >>= 1) {
      if ((imm_s & width) == 0) {
        unsigned mask = (unsigned)(width - 1);
        DCHECK_NE((imm_s & mask), mask);
        uint64_t bits = BIT_MASK((imm_s & mask) + 1);
        return RepeatBitsAcrossReg(is_wide, RotateRight(bits, imm_r & mask, width), width);
      }
    }
  }
  return 0;
}

/**
 * @brief Decode an 8-bit single point number encoded with EncodeImmSingle().
 */
static float DecodeImmSingle(uint8_t small_float) {
  int mantissa = (small_float & 0x0f) + 0x10;
  int sign = ((small_float & 0x80) == 0) ? 1 : -1;
  float signed_mantissa = static_cast<float>(sign*mantissa);
  int exponent = (((small_float >> 4) & 0x7) + 4) & 0x7;
  return signed_mantissa*static_cast<float>(1 << exponent)*0.0078125f;
}

static const char* cc_names[] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                                 "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};
/*
 * Interpret a format string and build a string no longer than size
 * See format key in assemble_arm64.cc.
 */
std::string Arm64Mir2Lir::BuildInsnString(const char* fmt, LIR* lir, unsigned char* base_addr) {
  std::string buf;
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
           case 'e':  {
               // Omit ", uxtw #0" in strings like "add w0, w1, w3, uxtw #0" and
               // ", uxtx #0" in strings like "add x0, x1, x3, uxtx #0"
               int omittable = ((IS_WIDE(lir->opcode)) ? EncodeExtend(kA64Uxtw, 0) :
                                EncodeExtend(kA64Uxtw, 0));
               if (LIKELY(operand == omittable)) {
                 strcpy(tbuf, "");
               } else {
                 DecodeRegExtendOrShift(operand, tbuf, arraysize(tbuf));
               }
             }
             break;
           case 'o':
             // Omit ", lsl #0"
             if (LIKELY(operand == EncodeShift(kA64Lsl, 0))) {
               strcpy(tbuf, "");
             } else {
               DecodeRegExtendOrShift(operand, tbuf, arraysize(tbuf));
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
           case 's':
             snprintf(tbuf, arraysize(tbuf), "s%d", operand & ARM_FP_REG_MASK);
             break;
           case 'S':
             snprintf(tbuf, arraysize(tbuf), "d%d", operand & ARM_FP_REG_MASK);
             break;
           case 'f':
             snprintf(tbuf, arraysize(tbuf), "%c%d", (IS_FWIDE(lir->opcode)) ? 'd' : 's',
                      operand & ARM_FP_REG_MASK);
             break;
           case 'l': {
               bool is_wide = IS_WIDE(lir->opcode);
               uint64_t imm = DecodeLogicalImmediate(is_wide, operand);
               snprintf(tbuf, arraysize(tbuf), "%" PRId64 " (%#" PRIx64 ")", imm, imm);
             }
             break;
           case 'I':
             snprintf(tbuf, arraysize(tbuf), "%f", DecodeImmSingle(operand));
             break;
           case 'M':
             if (LIKELY(operand == 0))
               strcpy(tbuf, "");
             else
               snprintf(tbuf, arraysize(tbuf), ", lsl #%d", 16*operand);
             break;
           case 'd':
             snprintf(tbuf, arraysize(tbuf), "%d", operand);
             break;
           case 'w':
             if (LIKELY(operand != rwzr))
               snprintf(tbuf, arraysize(tbuf), "w%d", operand & RegStorage::kRegNumMask);
             else
               strcpy(tbuf, "wzr");
             break;
           case 'W':
             if (LIKELY(operand != rwsp))
               snprintf(tbuf, arraysize(tbuf), "w%d", operand & RegStorage::kRegNumMask);
             else
               strcpy(tbuf, "wsp");
             break;
           case 'x':
             if (LIKELY(operand != rxzr))
               snprintf(tbuf, arraysize(tbuf), "x%d", operand & RegStorage::kRegNumMask);
             else
               strcpy(tbuf, "xzr");
             break;
           case 'X':
             if (LIKELY(operand != rsp))
               snprintf(tbuf, arraysize(tbuf), "x%d", operand & RegStorage::kRegNumMask);
             else
               strcpy(tbuf, "sp");
             break;
           case 'D':
             snprintf(tbuf, arraysize(tbuf), "%d", operand*((IS_WIDE(lir->opcode)) ? 8 : 4));
             break;
           case 'E':
             snprintf(tbuf, arraysize(tbuf), "%d", operand*4);
             break;
           case 'F':
             snprintf(tbuf, arraysize(tbuf), "%d", operand*2);
             break;
           case 'G':
             if (LIKELY(operand == 0))
               strcpy(tbuf, "");
             else
               strcpy(tbuf, (IS_WIDE(lir->opcode)) ? ", lsl #3" : ", lsl #2");
             break;
           case 'c':
             strcpy(tbuf, cc_names[operand]);
             break;
           case 't':
             snprintf(tbuf, arraysize(tbuf), "0x%08" PRIxPTR " (L%p)",
                 reinterpret_cast<uintptr_t>(base_addr) + lir->offset + (operand << 2),
                 lir->target);
             break;
           case 'r': {
               bool is_wide = IS_WIDE(lir->opcode);
               if (LIKELY(operand != rwzr && operand != rxzr)) {
                 snprintf(tbuf, arraysize(tbuf), "%c%d", (is_wide) ? 'x' : 'w',
                          operand & RegStorage::kRegNumMask);
               } else {
                 strcpy(tbuf, (is_wide) ? "xzr" : "wzr");
               }
             }
             break;
           case 'R': {
               bool is_wide = IS_WIDE(lir->opcode);
               if (LIKELY(operand != rwsp || operand != rsp)) {
                 snprintf(tbuf, arraysize(tbuf), "%c%d", (is_wide) ? 'x' : 'w',
                          operand & RegStorage::kRegNumMask);
               } else {
                 strcpy(tbuf, (is_wide) ? "sp" : "wsp");
               }
             }
             break;
           case 'p':
             snprintf(tbuf, arraysize(tbuf), ".+%d (addr %#" PRIxPTR ")", 4*operand,
                      reinterpret_cast<uintptr_t>(base_addr) + lir->offset + 4*operand);
             break;
           case 'T':
             if (LIKELY(operand == 0))
               strcpy(tbuf, "");
             else if (operand == 1)
               strcpy(tbuf, ", lsl #12");
             else
               strcpy(tbuf, ", DecodeError3");
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

void Arm64Mir2Lir::DumpResourceMask(LIR* arm_lir, uint64_t mask, const char* prefix) {
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

bool Arm64Mir2Lir::IsUnconditionalBranch(LIR* lir) {
  return (lir->opcode == kA64B1t);
}

bool Arm64Mir2Lir::SupportsVolatileLoadStore(OpSize size) {
  return true;
}

RegisterClass Arm64Mir2Lir::RegClassForFieldLoadStore(OpSize size, bool is_volatile) {
  if (UNLIKELY(is_volatile)) {
    // On arm64, fp register load/store is atomic only for single bytes.
    if (size != kSignedByte && size != kUnsignedByte) {
      return kCoreReg;
    }
  }
  return RegClassBySize(size);
}

Arm64Mir2Lir::Arm64Mir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena)
    : Mir2Lir(cu, mir_graph, arena) {
  // Sanity check - make sure encoding map lines up.
  for (int i = 0; i < kA64Last; i++) {
    if (UNWIDE(Arm64Mir2Lir::EncodingMap[i].opcode) != i) {
      LOG(FATAL) << "Encoding order for " << Arm64Mir2Lir::EncodingMap[i].name
                 << " is wrong: expecting " << i << ", seeing "
                 << static_cast<int>(Arm64Mir2Lir::EncodingMap[i].opcode);
    }
  }
}

Mir2Lir* Arm64CodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                            ArenaAllocator* const arena) {
  return new Arm64Mir2Lir(cu, mir_graph, arena);
}

// Alloc a pair of core registers, or a double.
RegStorage Arm64Mir2Lir::AllocTypedTempWide(bool fp_hint, int reg_class) {
  if (((reg_class == kAnyReg) && fp_hint) || (reg_class == kFPReg)) {
    return AllocTempDouble();
  } else {
    RegStorage low_reg = AllocTemp();
    RegStorage high_reg = AllocTemp();
    return RegStorage::MakeRegPair(low_reg, high_reg);
  }
}

RegStorage Arm64Mir2Lir::AllocTypedTemp(bool fp_hint, int reg_class) {
  if (((reg_class == kAnyReg) && fp_hint) || (reg_class == kFPReg))
    return AllocTempSingle();
  return AllocTemp();
}

void Arm64Mir2Lir::CompilerInitializeRegAlloc() {
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

  // TODO: re-enable this when we can safely save r4 over the suspension code path.
  bool no_suspend = NO_SUSPEND;  // || !Runtime::Current()->ExplicitSuspendChecks();
  if (no_suspend) {
    GetRegInfo(rs_rA64_SUSPEND)->MarkFree();
  }

  // Don't start allocating temps at r0/s0/d0 or you may clobber return regs in early-exit methods.
  // TODO: adjust when we roll to hard float calling convention.
  reg_pool_->next_core_reg_ = 2;
  reg_pool_->next_sp_reg_ = 0;
  reg_pool_->next_dp_reg_ = 0;
}

void Arm64Mir2Lir::FreeRegLocTemps(RegLocation rl_keep, RegLocation rl_free) {
  LOG(FATAL) << "Unexpected call to FreeRegLocTemps for Arm64";
}

/*
 * TUNING: is true leaf?  Can't just use METHOD_IS_LEAF to determine as some
 * instructions might call out to C/assembly helper functions.  Until
 * machinery is in place, always spill lr.
 */

void Arm64Mir2Lir::AdjustSpillMask() {
  core_spill_mask_ |= (1 << rs_rA64_LR.GetRegNum());
  num_core_spills_++;
}

/*
 * Mark a callee-save fp register as promoted.  Note that
 * vpush/vpop uses contiguous register lists so we must
 * include any holes in the mask.  Associate holes with
 * Dalvik register INVALID_VREG (0xFFFFU).
 */
void Arm64Mir2Lir::MarkPreservedSingle(int v_reg, RegStorage reg) {
  DCHECK_GE(reg.GetRegNum(), ARM_FP_CALLEE_SAVE_BASE);
  int adjusted_reg_num = reg.GetRegNum() - ARM_FP_CALLEE_SAVE_BASE;
  // Ensure fp_vmap_table is large enough
  int table_size = fp_vmap_table_.size();
  for (int i = table_size; i < (adjusted_reg_num + 1); i++) {
    fp_vmap_table_.push_back(INVALID_VREG);
  }
  // Add the current mapping
  fp_vmap_table_[adjusted_reg_num] = v_reg;
  // Size of fp_vmap_table is high-water mark, use to set mask
  num_fp_spills_ = fp_vmap_table_.size();
  fp_spill_mask_ = ((1 << num_fp_spills_) - 1) << ARM_FP_CALLEE_SAVE_BASE;
}

void Arm64Mir2Lir::MarkPreservedDouble(int v_reg, RegStorage reg) {
  // TEMP: perform as 2 singles.
  int reg_num = reg.GetRegNum() << 1;
  RegStorage lo = RegStorage::Solo32(RegStorage::kFloatingPoint | reg_num);
  RegStorage hi = RegStorage::Solo32(RegStorage::kFloatingPoint | reg_num | 1);
  MarkPreservedSingle(v_reg, lo);
  MarkPreservedSingle(v_reg + 1, hi);
}

/* Clobber all regs that might be used by an external C call */
void Arm64Mir2Lir::ClobberCallerSave() {
  // TODO(Arm64): implement this.
  UNIMPLEMENTED(WARNING);

  Clobber(rs_x0);
  Clobber(rs_x1);
  Clobber(rs_x2);
  Clobber(rs_x3);
  Clobber(rs_x12);
  Clobber(rs_x30);
  Clobber(rs_f0);
  Clobber(rs_f1);
  Clobber(rs_f2);
  Clobber(rs_f3);
  Clobber(rs_f4);
  Clobber(rs_f5);
  Clobber(rs_f6);
  Clobber(rs_f7);
  Clobber(rs_f8);
  Clobber(rs_f9);
  Clobber(rs_f10);
  Clobber(rs_f11);
  Clobber(rs_f12);
  Clobber(rs_f13);
  Clobber(rs_f14);
  Clobber(rs_f15);
}

RegLocation Arm64Mir2Lir::GetReturnWideAlt() {
  RegLocation res = LocCReturnWide();
  res.reg.SetReg(rx2);
  res.reg.SetHighReg(rx3);
  Clobber(rs_x2);
  Clobber(rs_x3);
  MarkInUse(rs_x2);
  MarkInUse(rs_x3);
  MarkWide(res.reg);
  return res;
}

RegLocation Arm64Mir2Lir::GetReturnAlt() {
  RegLocation res = LocCReturn();
  res.reg.SetReg(rx1);
  Clobber(rs_x1);
  MarkInUse(rs_x1);
  return res;
}

/* To be used when explicitly managing register use */
void Arm64Mir2Lir::LockCallTemps() {
  LockTemp(rs_x0);
  LockTemp(rs_x1);
  LockTemp(rs_x2);
  LockTemp(rs_x3);
}

/* To be used when explicitly managing register use */
void Arm64Mir2Lir::FreeCallTemps() {
  FreeTemp(rs_x0);
  FreeTemp(rs_x1);
  FreeTemp(rs_x2);
  FreeTemp(rs_x3);
}

RegStorage Arm64Mir2Lir::LoadHelper(ThreadOffset<4> offset) {
  UNIMPLEMENTED(FATAL) << "Should not be called.";
  return RegStorage::InvalidReg();
}

RegStorage Arm64Mir2Lir::LoadHelper(ThreadOffset<8> offset) {
  // TODO(Arm64): use LoadWordDisp instead.
  //   e.g. LoadWordDisp(rs_rA64_SELF, offset.Int32Value(), rs_rA64_LR);
  LoadBaseDisp(rs_rA64_SELF, offset.Int32Value(), rs_rA64_LR, k64);
  return rs_rA64_LR;
}

LIR* Arm64Mir2Lir::CheckSuspendUsingLoad() {
  RegStorage tmp = rs_x0;
  LoadWordDisp(rs_rA64_SELF, Thread::ThreadSuspendTriggerOffset<8>().Int32Value(), tmp);
  LIR* load2 = LoadWordDisp(tmp, 0, tmp);
  return load2;
}

uint64_t Arm64Mir2Lir::GetTargetInstFlags(int opcode) {
  DCHECK(!IsPseudoLirOp(opcode));
  return Arm64Mir2Lir::EncodingMap[UNWIDE(opcode)].flags;
}

const char* Arm64Mir2Lir::GetTargetInstName(int opcode) {
  DCHECK(!IsPseudoLirOp(opcode));
  return Arm64Mir2Lir::EncodingMap[UNWIDE(opcode)].name;
}

const char* Arm64Mir2Lir::GetTargetInstFmt(int opcode) {
  DCHECK(!IsPseudoLirOp(opcode));
  return Arm64Mir2Lir::EncodingMap[UNWIDE(opcode)].fmt;
}

/*
 * Somewhat messy code here.  We want to allocate a pair of contiguous
 * physical single-precision floating point registers starting with
 * an even numbered reg.  It is possible that the paired s_reg (s_reg+1)
 * has already been allocated - try to fit if possible.  Fail to
 * allocate if we can't meet the requirements for the pair of
 * s_reg<=sX[even] & (s_reg+1)<= sX+1.
 */
// TODO: needs rewrite to support non-backed 64-bit float regs.
RegStorage Arm64Mir2Lir::AllocPreservedDouble(int s_reg) {
  RegStorage res;
  int v_reg = mir_graph_->SRegToVReg(s_reg);
  int p_map_idx = SRegToPMap(s_reg);
  if (promotion_map_[p_map_idx+1].fp_location == kLocPhysReg) {
    // Upper reg is already allocated.  Can we fit?
    int high_reg = promotion_map_[p_map_idx+1].FpReg;
    if ((high_reg & 1) == 0) {
      // High reg is even - fail.
      return res;  // Invalid.
    }
    // Is the low reg of the pair free?
    // FIXME: rework.
    RegisterInfo* p = GetRegInfo(RegStorage::FloatSolo32(high_reg - 1));
    if (p->InUse() || p->IsTemp()) {
      // Already allocated or not preserved - fail.
      return res;  // Invalid.
    }
    // OK - good to go.
    res = RegStorage::FloatSolo64(p->GetReg().GetRegNum() >> 1);
    p->MarkInUse();
    MarkPreservedSingle(v_reg, p->GetReg());
  } else {
    /*
     * TODO: until runtime support is in, make sure we avoid promoting the same vreg to
     * different underlying physical registers.
     */
    GrowableArray<RegisterInfo*>::Iterator it(&reg_pool_->dp_regs_);
    for (RegisterInfo* info = it.Next(); info != nullptr; info = it.Next()) {
      if (!info->IsTemp() && !info->InUse()) {
        res = info->GetReg();
        info->MarkInUse();
        MarkPreservedDouble(v_reg, info->GetReg());
        break;
      }
    }
  }
  if (res.Valid()) {
    promotion_map_[p_map_idx].fp_location = kLocPhysReg;
    promotion_map_[p_map_idx].FpReg = res.DoubleToLowSingle().GetReg();
    promotion_map_[p_map_idx+1].fp_location = kLocPhysReg;
    promotion_map_[p_map_idx+1].FpReg = res.DoubleToHighSingle().GetReg();
  }
  return res;
}

// TODO(Arm64): reuse info in QuickArgumentVisitor?
static RegStorage GetArgPhysicalReg(RegLocation* loc, int* num_gpr_used, int* num_fpr_used,
                                    OpSize* op_size) {
  if (loc->fp) {
    int n = *num_fpr_used;
    if (n < 8) {
      *num_fpr_used = n + 1;
      RegStorage::RegStorageKind reg_kind;
      if (loc->wide) {
        *op_size = kDouble;
        reg_kind = RegStorage::k64BitSolo;
      } else {
        *op_size = kSingle;
        reg_kind = RegStorage::k32BitSolo;
      }
      return RegStorage(RegStorage::kValid | reg_kind | RegStorage::kFloatingPoint | n);
    }
  } else {
    int n = *num_gpr_used;
    if (n < 7) {
      *num_gpr_used = n + 1;
      if (loc->wide) {
        *op_size = k64;
        return RegStorage::Solo64(n);
      } else {
        *op_size = k32;
        return RegStorage::Solo32(n);
      }
    }
  }

  return RegStorage::InvalidReg();
}

/*
 * If there are any ins passed in registers that have not been promoted
 * to a callee-save register, flush them to the frame.  Perform initial
 * assignment of promoted arguments.
 *
 * ArgLocs is an array of location records describing the incoming arguments
 * with one location record per word of argument.
 */
void Arm64Mir2Lir::FlushIns(RegLocation* ArgLocs, RegLocation rl_method) {
  int num_gpr_used = 1;
  int num_fpr_used = 0;

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

  // TODO(Arm64): compress the Method pointer?
  StoreValueWide(rl_method, rl_src);

  // If Method* has been promoted, explicitly flush
  if (rl_method.location == kLocPhysReg) {
    StoreWordDisp(TargetReg(kSp), 0, TargetReg(kArg0));
  }

  if (cu_->num_ins == 0) {
    return;
  }

  int start_vreg = cu_->num_dalvik_registers - cu_->num_ins;
  for (int i = 0; i < cu_->num_ins; i++) {
    PromotionMap* v_map = &promotion_map_[start_vreg + i];
    RegLocation* t_loc = &ArgLocs[i];
    OpSize op_size;
    RegStorage reg = GetArgPhysicalReg(t_loc, &num_gpr_used, &num_fpr_used, &op_size);

    if (reg.Valid()) {
      if ((v_map->core_location == kLocPhysReg) && !t_loc->fp) {
        OpRegCopy(RegStorage::Solo32(v_map->core_reg), reg);
      } else if ((v_map->fp_location == kLocPhysReg) && t_loc->fp) {
        OpRegCopy(RegStorage::Solo32(v_map->FpReg), reg);
      } else {
        StoreBaseDisp(TargetReg(kSp), SRegOffset(start_vreg + i), reg, op_size);
        if (reg.Is64Bit()) {
          if (SRegOffset(start_vreg + i) + 4 != SRegOffset(start_vreg + i + 1)) {
            LOG(FATAL) << "64 bit value stored in non-consecutive 4 bytes slots";
          }
          i += 1;
        }
      }
    } else {
      // If arriving in frame & promoted
      if (v_map->core_location == kLocPhysReg) {
        LoadWordDisp(TargetReg(kSp), SRegOffset(start_vreg + i),
                     RegStorage::Solo32(v_map->core_reg));
      }
      if (v_map->fp_location == kLocPhysReg) {
        LoadWordDisp(TargetReg(kSp), SRegOffset(start_vreg + i), RegStorage::Solo32(v_map->FpReg));
      }
    }
  }
}

int Arm64Mir2Lir::LoadArgRegs(CallInfo* info, int call_state,
                              NextCallInsn next_call_insn,
                              const MethodReference& target_method,
                              uint32_t vtable_idx, uintptr_t direct_code,
                              uintptr_t direct_method, InvokeType type, bool skip_this) {
  int last_arg_reg = TargetReg(kArg3).GetReg();
  int next_reg = TargetReg(kArg1).GetReg();
  int next_arg = 0;
  if (skip_this) {
    next_reg++;
    next_arg++;
  }
  for (; (next_reg <= last_arg_reg) && (next_arg < info->num_arg_words); next_reg++) {
    RegLocation rl_arg = info->args[next_arg++];
    rl_arg = UpdateRawLoc(rl_arg);
    if (rl_arg.wide && (next_reg <= TargetReg(kArg2).GetReg())) {
      RegStorage r_tmp(RegStorage::k64BitPair, next_reg, next_reg + 1);
      LoadValueDirectWideFixed(rl_arg, r_tmp);
      next_reg++;
      next_arg++;
    } else {
      if (rl_arg.wide) {
        rl_arg = NarrowRegLoc(rl_arg);
        rl_arg.is_const = false;
      }
      LoadValueDirectFixed(rl_arg, RegStorage::Solo32(next_reg));
    }
    call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                                direct_code, direct_method, type);
  }
  return call_state;
}

}  // namespace art
