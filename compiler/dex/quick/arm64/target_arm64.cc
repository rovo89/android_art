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
#include <sstream>

#include "backend_arm64.h"
#include "base/logging.h"
#include "dex/mir_graph.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "dex/reg_storage_eq.h"

namespace art {

static constexpr RegStorage core_regs_arr[] =
    {rs_w0, rs_w1, rs_w2, rs_w3, rs_w4, rs_w5, rs_w6, rs_w7,
     rs_w8, rs_w9, rs_w10, rs_w11, rs_w12, rs_w13, rs_w14, rs_w15,
     rs_w16, rs_w17, rs_w18, rs_w19, rs_w20, rs_w21, rs_w22, rs_w23,
     rs_w24, rs_w25, rs_w26, rs_w27, rs_w28, rs_w29, rs_w30, rs_w31,
     rs_wzr};
static constexpr RegStorage core64_regs_arr[] =
    {rs_x0, rs_x1, rs_x2, rs_x3, rs_x4, rs_x5, rs_x6, rs_x7,
     rs_x8, rs_x9, rs_x10, rs_x11, rs_x12, rs_x13, rs_x14, rs_x15,
     rs_x16, rs_x17, rs_x18, rs_x19, rs_x20, rs_x21, rs_x22, rs_x23,
     rs_x24, rs_x25, rs_x26, rs_x27, rs_x28, rs_x29, rs_x30, rs_x31,
     rs_xzr};
static constexpr RegStorage sp_regs_arr[] =
    {rs_f0, rs_f1, rs_f2, rs_f3, rs_f4, rs_f5, rs_f6, rs_f7,
     rs_f8, rs_f9, rs_f10, rs_f11, rs_f12, rs_f13, rs_f14, rs_f15,
     rs_f16, rs_f17, rs_f18, rs_f19, rs_f20, rs_f21, rs_f22, rs_f23,
     rs_f24, rs_f25, rs_f26, rs_f27, rs_f28, rs_f29, rs_f30, rs_f31};
static constexpr RegStorage dp_regs_arr[] =
    {rs_d0, rs_d1, rs_d2, rs_d3, rs_d4, rs_d5, rs_d6, rs_d7,
     rs_d8, rs_d9, rs_d10, rs_d11, rs_d12, rs_d13, rs_d14, rs_d15,
     rs_d16, rs_d17, rs_d18, rs_d19, rs_d20, rs_d21, rs_d22, rs_d23,
     rs_d24, rs_d25, rs_d26, rs_d27, rs_d28, rs_d29, rs_d30, rs_d31};
// Note: we are not able to call to C function since rs_xSELF is a special register need to be
// preserved but would be scratched by native functions follow aapcs64.
static constexpr RegStorage reserved_regs_arr[] = {rs_wSELF, rs_wsp, rs_wLR, rs_wzr};
static constexpr RegStorage reserved64_regs_arr[] = {rs_xSELF, rs_sp, rs_xLR, rs_xzr};

static constexpr RegStorage core_temps_arr[] =
    {rs_w0, rs_w1, rs_w2, rs_w3, rs_w4, rs_w5, rs_w6, rs_w7,
     rs_w8, rs_w9, rs_w10, rs_w11, rs_w12, rs_w13, rs_w14, rs_w15, rs_w16,
     rs_w17};
static constexpr RegStorage core64_temps_arr[] =
    {rs_x0, rs_x1, rs_x2, rs_x3, rs_x4, rs_x5, rs_x6, rs_x7,
     rs_x8, rs_x9, rs_x10, rs_x11, rs_x12, rs_x13, rs_x14, rs_x15, rs_x16,
     rs_x17};
static constexpr RegStorage sp_temps_arr[] =
    {rs_f0, rs_f1, rs_f2, rs_f3, rs_f4, rs_f5, rs_f6, rs_f7,
     rs_f16, rs_f17, rs_f18, rs_f19, rs_f20, rs_f21, rs_f22, rs_f23,
     rs_f24, rs_f25, rs_f26, rs_f27, rs_f28, rs_f29, rs_f30, rs_f31};
static constexpr RegStorage dp_temps_arr[] =
    {rs_d0, rs_d1, rs_d2, rs_d3, rs_d4, rs_d5, rs_d6, rs_d7,
     rs_d16, rs_d17, rs_d18, rs_d19, rs_d20, rs_d21, rs_d22, rs_d23,
     rs_d24, rs_d25, rs_d26, rs_d27, rs_d28, rs_d29, rs_d30, rs_d31};

static constexpr ArrayRef<const RegStorage> core_regs(core_regs_arr);
static constexpr ArrayRef<const RegStorage> core64_regs(core64_regs_arr);
static constexpr ArrayRef<const RegStorage> sp_regs(sp_regs_arr);
static constexpr ArrayRef<const RegStorage> dp_regs(dp_regs_arr);
static constexpr ArrayRef<const RegStorage> reserved_regs(reserved_regs_arr);
static constexpr ArrayRef<const RegStorage> reserved64_regs(reserved64_regs_arr);
static constexpr ArrayRef<const RegStorage> core_temps(core_temps_arr);
static constexpr ArrayRef<const RegStorage> core64_temps(core64_temps_arr);
static constexpr ArrayRef<const RegStorage> sp_temps(sp_temps_arr);
static constexpr ArrayRef<const RegStorage> dp_temps(dp_temps_arr);

RegLocation Arm64Mir2Lir::LocCReturn() {
  return a64_loc_c_return;
}

RegLocation Arm64Mir2Lir::LocCReturnRef() {
  return a64_loc_c_return_ref;
}

RegLocation Arm64Mir2Lir::LocCReturnWide() {
  return a64_loc_c_return_wide;
}

RegLocation Arm64Mir2Lir::LocCReturnFloat() {
  return a64_loc_c_return_float;
}

RegLocation Arm64Mir2Lir::LocCReturnDouble() {
  return a64_loc_c_return_double;
}

// Return a target-dependent special register.
RegStorage Arm64Mir2Lir::TargetReg(SpecialTargetRegister reg) {
  RegStorage res_reg = RegStorage::InvalidReg();
  switch (reg) {
    case kSelf: res_reg = rs_wSELF; break;
    case kSuspend: res_reg = RegStorage::InvalidReg(); break;
    case kLr: res_reg =  rs_wLR; break;
    case kPc: res_reg = RegStorage::InvalidReg(); break;
    case kSp: res_reg =  rs_wsp; break;
    case kArg0: res_reg = rs_w0; break;
    case kArg1: res_reg = rs_w1; break;
    case kArg2: res_reg = rs_w2; break;
    case kArg3: res_reg = rs_w3; break;
    case kArg4: res_reg = rs_w4; break;
    case kArg5: res_reg = rs_w5; break;
    case kArg6: res_reg = rs_w6; break;
    case kArg7: res_reg = rs_w7; break;
    case kFArg0: res_reg = rs_f0; break;
    case kFArg1: res_reg = rs_f1; break;
    case kFArg2: res_reg = rs_f2; break;
    case kFArg3: res_reg = rs_f3; break;
    case kFArg4: res_reg = rs_f4; break;
    case kFArg5: res_reg = rs_f5; break;
    case kFArg6: res_reg = rs_f6; break;
    case kFArg7: res_reg = rs_f7; break;
    case kRet0: res_reg = rs_w0; break;
    case kRet1: res_reg = rs_w1; break;
    case kInvokeTgt: res_reg = rs_wLR; break;
    case kHiddenArg: res_reg = rs_wIP1; break;
    case kHiddenFpArg: res_reg = RegStorage::InvalidReg(); break;
    case kCount: res_reg = RegStorage::InvalidReg(); break;
    default: res_reg = RegStorage::InvalidReg();
  }
  return res_reg;
}

/*
 * Decode the register id. This routine makes assumptions on the encoding made by RegStorage.
 */
ResourceMask Arm64Mir2Lir::GetRegMaskCommon(const RegStorage& reg) const {
  // TODO(Arm64): this function depends too much on the internal RegStorage encoding. Refactor.

  // Check if the shape mask is zero (i.e. invalid).
  if (UNLIKELY(reg == rs_wzr || reg == rs_xzr)) {
    // The zero register is not a true register. It is just an immediate zero.
    return kEncodeNone;
  }

  return ResourceMask::Bit(
      // FP register starts at bit position 32.
      (reg.IsFloat() ? kA64FPReg0 : 0) + reg.GetRegNum());
}

ResourceMask Arm64Mir2Lir::GetPCUseDefEncoding() const {
  // Note: On arm64, we are not able to set pc except branch instructions, which is regarded as a
  //       kind of barrier. All other instructions only use pc, which has no dependency between any
  //       of them. So it is fine to just return kEncodeNone here.
  return kEncodeNone;
}

// Arm64 specific setup.  TODO: inline?:
void Arm64Mir2Lir::SetupTargetResourceMasks(LIR* lir, uint64_t flags,
                                            ResourceMask* use_mask, ResourceMask* def_mask) {
  DCHECK_EQ(cu_->instruction_set, kArm64);
  DCHECK(!lir->flags.use_def_invalid);

  // Note: REG_USE_PC is ignored, the reason is the same with what we do in GetPCUseDefEncoding().
  // These flags are somewhat uncommon - bypass if we can.
  if ((flags & (REG_DEF_SP | REG_USE_SP | REG_DEF_LR)) != 0) {
    if (flags & REG_DEF_SP) {
      def_mask->SetBit(kA64RegSP);
    }

    if (flags & REG_USE_SP) {
      use_mask->SetBit(kA64RegSP);
    }

    if (flags & REG_DEF_LR) {
      def_mask->SetBit(kA64RegLR);
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

static uint64_t bit_mask(unsigned width) {
  DCHECK_LE(width, 64U);
  return (width == 64) ? static_cast<uint64_t>(-1) : ((UINT64_C(1) << (width)) - UINT64_C(1));
}

static uint64_t RotateRight(uint64_t value, unsigned rotate, unsigned width) {
  DCHECK_LE(width, 64U);
  rotate &= 63;
  value = value & bit_mask(width);
  return ((value & bit_mask(rotate)) << (width - rotate)) | (value >> rotate);
}

static uint64_t RepeatBitsAcrossReg(bool is_wide, uint64_t value, unsigned width) {
  unsigned i;
  unsigned reg_size = (is_wide) ? 64 : 32;
  uint64_t result = value & bit_mask(width);
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
    uint64_t bits = bit_mask(imm_s + 1);
    return RotateRight(bits, imm_r, 64);
  } else {
    DCHECK_NE((imm_s >> 1), 0x1fU);
    for (unsigned width = 0x20; width >= 0x2; width >>= 1) {
      if ((imm_s & width) == 0) {
        unsigned mask = (unsigned)(width - 1);
        DCHECK_NE((imm_s & mask), mask);
        uint64_t bits = bit_mask((imm_s & mask) + 1);
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
             snprintf(tbuf, arraysize(tbuf), "s%d", operand & RegStorage::kRegNumMask);
             break;
           case 'S':
             snprintf(tbuf, arraysize(tbuf), "d%d", operand & RegStorage::kRegNumMask);
             break;
           case 'f':
             snprintf(tbuf, arraysize(tbuf), "%c%d", (IS_WIDE(lir->opcode)) ? 'd' : 's',
                      operand & RegStorage::kRegNumMask);
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
               if (LIKELY(operand != rwsp && operand != rsp)) {
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
           case 'h':
             snprintf(tbuf, arraysize(tbuf), "%d", operand);
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
  // Dump thread offset.
  std::string fmt_str = GetTargetInstFmt(lir->opcode);
  if (std::string::npos != fmt_str.find(", [!1X, #!2") && rxSELF == lir->operands[1] &&
      std::string::npos != buf.find(", [")) {
    int offset = lir->operands[2];
    if (std::string::npos != fmt_str.find("#!2d")) {
    } else if (std::string::npos != fmt_str.find("#!2D")) {
      offset *= (IS_WIDE(lir->opcode)) ? 8 : 4;
    } else if (std::string::npos != fmt_str.find("#!2F")) {
      offset *= 2;
    } else {
      LOG(FATAL) << "Should not reach here";
    }
    std::ostringstream tmp_stream;
    Thread::DumpThreadOffset<8>(tmp_stream, offset);
    buf += "  ; ";
    buf += tmp_stream.str();
  }
  return buf;
}

void Arm64Mir2Lir::DumpResourceMask(LIR* arm_lir, const ResourceMask& mask, const char* prefix) {
  char buf[256];
  buf[0] = 0;

  if (mask.Equals(kEncodeAll)) {
    strcpy(buf, "all");
  } else {
    char num[8];
    int i;

    for (i = 0; i < kA64RegEnd; i++) {
      if (mask.HasBit(i)) {
        snprintf(num, arraysize(num), "%d ", i);
        strcat(buf, num);
      }
    }

    if (mask.HasBit(ResourceMask::kCCode)) {
      strcat(buf, "cc ");
    }
    if (mask.HasBit(ResourceMask::kFPStatus)) {
      strcat(buf, "fpcc ");
    }

    /* Memory bits */
    if (arm_lir && (mask.HasBit(ResourceMask::kDalvikReg))) {
      snprintf(buf + strlen(buf), arraysize(buf) - strlen(buf), "dr%d%s",
               DECODE_ALIAS_INFO_REG(arm_lir->flags.alias_info),
               DECODE_ALIAS_INFO_WIDE(arm_lir->flags.alias_info) ? "(+1)" : "");
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
    LOG(INFO) << prefix << ": " << buf;
  }
}

bool Arm64Mir2Lir::IsUnconditionalBranch(LIR* lir) {
  return (lir->opcode == kA64B1t);
}

RegisterClass Arm64Mir2Lir::RegClassForFieldLoadStore(OpSize size, bool is_volatile) {
  if (UNLIKELY(is_volatile)) {
    // On arm64, fp register load/store is atomic only for single bytes.
    if (size != kSignedByte && size != kUnsignedByte) {
      return (size == kReference) ? kRefReg : kCoreReg;
    }
  }
  return RegClassBySize(size);
}

Arm64Mir2Lir::Arm64Mir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena)
    : Mir2Lir(cu, mir_graph, arena),
      call_method_insns_(arena->Adapter()),
      dex_cache_access_insns_(arena->Adapter()) {
  // Sanity check - make sure encoding map lines up.
  for (int i = 0; i < kA64Last; i++) {
    DCHECK_EQ(UNWIDE(Arm64Mir2Lir::EncodingMap[i].opcode), i)
        << "Encoding order for " << Arm64Mir2Lir::EncodingMap[i].name
        << " is wrong: expecting " << i << ", seeing "
        << static_cast<int>(Arm64Mir2Lir::EncodingMap[i].opcode);
  }
}

Mir2Lir* Arm64CodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                            ArenaAllocator* const arena) {
  return new Arm64Mir2Lir(cu, mir_graph, arena);
}

void Arm64Mir2Lir::CompilerInitializeRegAlloc() {
  reg_pool_.reset(new (arena_) RegisterPool(this, arena_, core_regs, core64_regs, sp_regs, dp_regs,
                                            reserved_regs, reserved64_regs,
                                            core_temps, core64_temps, sp_temps, dp_temps));

  // Target-specific adjustments.
  // Alias single precision float registers to corresponding double registers.
  for (RegisterInfo* info : reg_pool_->sp_regs_) {
    int fp_reg_num = info->GetReg().GetRegNum();
    RegStorage dp_reg = RegStorage::FloatSolo64(fp_reg_num);
    RegisterInfo* dp_reg_info = GetRegInfo(dp_reg);
    // Double precision register's master storage should refer to itself.
    DCHECK_EQ(dp_reg_info, dp_reg_info->Master());
    // Redirect single precision's master storage to master.
    info->SetMaster(dp_reg_info);
    // Singles should show a single 32-bit mask bit, at first referring to the low half.
    DCHECK_EQ(info->StorageMask(), 0x1U);
  }

  // Alias 32bit W registers to corresponding 64bit X registers.
  for (RegisterInfo* info : reg_pool_->core_regs_) {
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

  // Don't start allocating temps at r0/s0/d0 or you may clobber return regs in early-exit methods.
  // TODO: adjust when we roll to hard float calling convention.
  reg_pool_->next_core_reg_ = 2;
  reg_pool_->next_sp_reg_ = 0;
  reg_pool_->next_dp_reg_ = 0;
}

/*
 * TUNING: is true leaf?  Can't just use METHOD_IS_LEAF to determine as some
 * instructions might call out to C/assembly helper functions.  Until
 * machinery is in place, always spill lr.
 */

void Arm64Mir2Lir::AdjustSpillMask() {
  core_spill_mask_ |= (1 << rs_xLR.GetRegNum());
  num_core_spills_++;
}

/* Clobber all regs that might be used by an external C call */
void Arm64Mir2Lir::ClobberCallerSave() {
  Clobber(rs_x0);
  Clobber(rs_x1);
  Clobber(rs_x2);
  Clobber(rs_x3);
  Clobber(rs_x4);
  Clobber(rs_x5);
  Clobber(rs_x6);
  Clobber(rs_x7);
  Clobber(rs_x8);
  Clobber(rs_x9);
  Clobber(rs_x10);
  Clobber(rs_x11);
  Clobber(rs_x12);
  Clobber(rs_x13);
  Clobber(rs_x14);
  Clobber(rs_x15);
  Clobber(rs_x16);
  Clobber(rs_x17);
  Clobber(rs_x30);

  Clobber(rs_f0);
  Clobber(rs_f1);
  Clobber(rs_f2);
  Clobber(rs_f3);
  Clobber(rs_f4);
  Clobber(rs_f5);
  Clobber(rs_f6);
  Clobber(rs_f7);
  Clobber(rs_f16);
  Clobber(rs_f17);
  Clobber(rs_f18);
  Clobber(rs_f19);
  Clobber(rs_f20);
  Clobber(rs_f21);
  Clobber(rs_f22);
  Clobber(rs_f23);
  Clobber(rs_f24);
  Clobber(rs_f25);
  Clobber(rs_f26);
  Clobber(rs_f27);
  Clobber(rs_f28);
  Clobber(rs_f29);
  Clobber(rs_f30);
  Clobber(rs_f31);
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
  // TODO: needs cleanup.
  LockTemp(rs_x0);
  LockTemp(rs_x1);
  LockTemp(rs_x2);
  LockTemp(rs_x3);
  LockTemp(rs_x4);
  LockTemp(rs_x5);
  LockTemp(rs_x6);
  LockTemp(rs_x7);
  LockTemp(rs_f0);
  LockTemp(rs_f1);
  LockTemp(rs_f2);
  LockTemp(rs_f3);
  LockTemp(rs_f4);
  LockTemp(rs_f5);
  LockTemp(rs_f6);
  LockTemp(rs_f7);
}

/* To be used when explicitly managing register use */
void Arm64Mir2Lir::FreeCallTemps() {
  // TODO: needs cleanup.
  FreeTemp(rs_x0);
  FreeTemp(rs_x1);
  FreeTemp(rs_x2);
  FreeTemp(rs_x3);
  FreeTemp(rs_x4);
  FreeTemp(rs_x5);
  FreeTemp(rs_x6);
  FreeTemp(rs_x7);
  FreeTemp(rs_f0);
  FreeTemp(rs_f1);
  FreeTemp(rs_f2);
  FreeTemp(rs_f3);
  FreeTemp(rs_f4);
  FreeTemp(rs_f5);
  FreeTemp(rs_f6);
  FreeTemp(rs_f7);
  FreeTemp(TargetReg(kHiddenArg));
}

RegStorage Arm64Mir2Lir::LoadHelper(QuickEntrypointEnum trampoline) {
  // TODO(Arm64): use LoadWordDisp instead.
  //   e.g. LoadWordDisp(rs_rA64_SELF, offset.Int32Value(), rs_rA64_LR);
  LoadBaseDisp(rs_xSELF, GetThreadOffset<8>(trampoline).Int32Value(), rs_xLR, k64, kNotVolatile);
  return rs_xLR;
}

LIR* Arm64Mir2Lir::CheckSuspendUsingLoad() {
  RegStorage tmp = rs_x0;
  LoadWordDisp(rs_xSELF, Thread::ThreadSuspendTriggerOffset<8>().Int32Value(), tmp);
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

RegStorage Arm64Mir2Lir::InToRegStorageArm64Mapper::GetNextReg(ShortyArg arg) {
  const RegStorage coreArgMappingToPhysicalReg[] =
      {rs_x1, rs_x2, rs_x3, rs_x4, rs_x5, rs_x6, rs_x7};
  const size_t coreArgMappingToPhysicalRegSize = arraysize(coreArgMappingToPhysicalReg);
  const RegStorage fpArgMappingToPhysicalReg[] =
      {rs_f0, rs_f1, rs_f2, rs_f3, rs_f4, rs_f5, rs_f6, rs_f7};
  const size_t fpArgMappingToPhysicalRegSize = arraysize(fpArgMappingToPhysicalReg);

  RegStorage result = RegStorage::InvalidReg();
  if (arg.IsFP()) {
    if (cur_fp_reg_ < fpArgMappingToPhysicalRegSize) {
      DCHECK(!arg.IsRef());
      result = fpArgMappingToPhysicalReg[cur_fp_reg_++];
      if (result.Valid()) {
        // TODO: switching between widths remains a bit ugly.  Better way?
        int res_reg = result.GetReg();
        result = arg.IsWide() ? RegStorage::FloatSolo64(res_reg) : RegStorage::FloatSolo32(res_reg);
      }
    }
  } else {
    if (cur_core_reg_ < coreArgMappingToPhysicalRegSize) {
      result = coreArgMappingToPhysicalReg[cur_core_reg_++];
      if (result.Valid()) {
        // TODO: switching between widths remains a bit ugly.  Better way?
        int res_reg = result.GetReg();
        DCHECK(!(arg.IsWide() && arg.IsRef()));
        result = (arg.IsWide() || arg.IsRef()) ?
                 RegStorage::Solo64(res_reg) : RegStorage::Solo32(res_reg);
      }
    }
  }
  return result;
}

void Arm64Mir2Lir::InstallLiteralPools() {
  patches_.reserve(call_method_insns_.size() + dex_cache_access_insns_.size());

  // PC-relative calls to methods.
  for (LIR* p : call_method_insns_) {
      DCHECK_EQ(p->opcode, kA64Bl1t);
      uint32_t target_method_idx = p->operands[1];
      const DexFile* target_dex_file = UnwrapPointer<DexFile>(p->operands[2]);
      patches_.push_back(LinkerPatch::RelativeCodePatch(p->offset,
                                                        target_dex_file, target_method_idx));
  }

  // PC-relative references to dex cache arrays.
  for (LIR* p : dex_cache_access_insns_) {
    auto non_wide = UNWIDE(p->opcode);  // May be a wide load for ArtMethod*.
    DCHECK(non_wide == kA64Adrp2xd || non_wide == kA64Ldr3rXD) << p->opcode << " " << non_wide;
    const LIR* adrp = UnwrapPointer<LIR>(p->operands[4]);
    DCHECK_EQ(adrp->opcode, kA64Adrp2xd);
    const DexFile* dex_file = UnwrapPointer<DexFile>(adrp->operands[2]);
    uint32_t offset = adrp->operands[3];
    DCHECK(!p->flags.is_nop);
    DCHECK(!adrp->flags.is_nop);
    patches_.push_back(LinkerPatch::DexCacheArrayPatch(p->offset, dex_file, adrp->offset, offset));
  }

  // And do the normal processing.
  Mir2Lir::InstallLiteralPools();
}

int Arm64Mir2Lir::GenDalvikArgsBulkCopy(CallInfo* /*info*/, int /*first*/, int count) {
  /*
   * TODO: Improve by adding block copy for large number of arguments.  For now, just
   * copy a Dalvik vreg at a time.
   */
  return count;
}

void Arm64Mir2Lir::GenMachineSpecificExtendedMethodMIR(BasicBlock* bb, MIR* mir) {
  UNUSED(bb);
  DCHECK(MIR::DecodedInstruction::IsPseudoMirOp(mir->dalvikInsn.opcode));
  RegLocation rl_src[3];
  RegLocation rl_dest = mir_graph_->GetBadLoc();
  rl_src[0] = rl_src[1] = rl_src[2] = mir_graph_->GetBadLoc();
  ExtendedMIROpcode opcode = static_cast<ExtendedMIROpcode>(mir->dalvikInsn.opcode);
  switch (opcode) {
    case kMirOpMaddInt:
    case kMirOpMsubInt:
      rl_dest = mir_graph_->GetDest(mir);
      rl_src[0] = mir_graph_->GetSrc(mir, 0);
      rl_src[1] = mir_graph_->GetSrc(mir, 1);
      rl_src[2]= mir_graph_->GetSrc(mir, 2);
      GenMaddMsubInt(rl_dest, rl_src[0], rl_src[1], rl_src[2], opcode == kMirOpMsubInt);
      break;
    case kMirOpMaddLong:
    case kMirOpMsubLong:
      rl_dest = mir_graph_->GetDestWide(mir);
      rl_src[0] = mir_graph_->GetSrcWide(mir, 0);
      rl_src[1] = mir_graph_->GetSrcWide(mir, 2);
      rl_src[2] = mir_graph_->GetSrcWide(mir, 4);
      GenMaddMsubLong(rl_dest, rl_src[0], rl_src[1], rl_src[2], opcode == kMirOpMsubLong);
      break;
    default:
      LOG(FATAL) << "Unexpected opcode: " << static_cast<int>(opcode);
  }
}

}  // namespace art
