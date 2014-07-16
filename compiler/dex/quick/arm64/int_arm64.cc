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

/* This file contains codegen for the Thumb2 ISA. */

#include "arm64_lir.h"
#include "codegen_arm64.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "dex/reg_storage_eq.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "mirror/array.h"

namespace art {

LIR* Arm64Mir2Lir::OpCmpBranch(ConditionCode cond, RegStorage src1, RegStorage src2, LIR* target) {
  OpRegReg(kOpCmp, src1, src2);
  return OpCondBranch(cond, target);
}

LIR* Arm64Mir2Lir::OpIT(ConditionCode ccode, const char* guide) {
  LOG(FATAL) << "Unexpected use of OpIT for Arm64";
  return NULL;
}

void Arm64Mir2Lir::OpEndIT(LIR* it) {
  LOG(FATAL) << "Unexpected use of OpEndIT for Arm64";
}

/*
 * 64-bit 3way compare function.
 *     cmp   xA, xB
 *     csinc wC, wzr, wzr, eq  // wC = (xA == xB) ? 0 : 1
 *     csneg wC, wC, wC, ge    // wC = (xA >= xB) ? wC : -wC
 */
void Arm64Mir2Lir::GenCmpLong(RegLocation rl_dest, RegLocation rl_src1,
                              RegLocation rl_src2) {
  RegLocation rl_result;
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  rl_result = EvalLoc(rl_dest, kCoreReg, true);

  OpRegReg(kOpCmp, rl_src1.reg, rl_src2.reg);
  NewLIR4(kA64Csinc4rrrc, rl_result.reg.GetReg(), rwzr, rwzr, kArmCondEq);
  NewLIR4(kA64Csneg4rrrc, rl_result.reg.GetReg(), rl_result.reg.GetReg(),
          rl_result.reg.GetReg(), kArmCondGe);
  StoreValue(rl_dest, rl_result);
}

void Arm64Mir2Lir::GenShiftOpLong(Instruction::Code opcode, RegLocation rl_dest,
                             RegLocation rl_src1, RegLocation rl_shift) {
  OpKind op = kOpBkpt;
  switch (opcode) {
  case Instruction::SHL_LONG:
  case Instruction::SHL_LONG_2ADDR:
    op = kOpLsl;
    break;
  case Instruction::SHR_LONG:
  case Instruction::SHR_LONG_2ADDR:
    op = kOpAsr;
    break;
  case Instruction::USHR_LONG:
  case Instruction::USHR_LONG_2ADDR:
    op = kOpLsr;
    break;
  default:
    LOG(FATAL) << "Unexpected case: " << opcode;
  }
  rl_shift = LoadValue(rl_shift, kCoreReg);
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  OpRegRegReg(op, rl_result.reg, rl_src1.reg, As64BitReg(rl_shift.reg));
  StoreValueWide(rl_dest, rl_result);
}

static constexpr bool kUseDeltaEncodingInGenSelect = false;

void Arm64Mir2Lir::GenSelect(int32_t true_val, int32_t false_val, ConditionCode ccode,
                             RegStorage rs_dest, int result_reg_class) {
  if (false_val == 0 ||               // 0 is better as first operand.
      true_val == 1 ||                // Potentially Csinc.
      true_val == -1 ||               // Potentially Csinv.
      true_val == false_val + 1) {    // Potentially Csinc.
    ccode = NegateComparison(ccode);
    std::swap(true_val, false_val);
  }

  ArmConditionCode code = ArmConditionEncoding(ccode);

  int opcode;                                      // The opcode.
  RegStorage left_op = RegStorage::InvalidReg();   // The operands.
  RegStorage right_op = RegStorage::InvalidReg();  // The operands.

  bool is_wide = rs_dest.Is64Bit();

  RegStorage zero_reg = is_wide ? rs_xzr : rs_wzr;

  if (true_val == 0) {
    left_op = zero_reg;
  } else {
    left_op = rs_dest;
    LoadConstantNoClobber(rs_dest, true_val);
  }
  if (false_val == 1) {
    right_op = zero_reg;
    opcode = kA64Csinc4rrrc;
  } else if (false_val == -1) {
    right_op = zero_reg;
    opcode = kA64Csinv4rrrc;
  } else if (false_val == true_val + 1) {
    right_op = left_op;
    opcode = kA64Csinc4rrrc;
  } else if (false_val == -true_val) {
    right_op = left_op;
    opcode = kA64Csneg4rrrc;
  } else if (false_val == ~true_val) {
    right_op = left_op;
    opcode = kA64Csinv4rrrc;
  } else if (true_val == 0) {
    // left_op is zero_reg.
    right_op = rs_dest;
    LoadConstantNoClobber(rs_dest, false_val);
    opcode = kA64Csel4rrrc;
  } else {
    // Generic case.
    RegStorage t_reg2 = AllocTypedTemp(false, result_reg_class);
    if (is_wide) {
      if (t_reg2.Is32Bit()) {
        t_reg2 = As64BitReg(t_reg2);
      }
    } else {
      if (t_reg2.Is64Bit()) {
        t_reg2 = As32BitReg(t_reg2);
      }
    }

    if (kUseDeltaEncodingInGenSelect) {
      int32_t delta = false_val - true_val;
      uint32_t abs_val = delta < 0 ? -delta : delta;

      if (abs_val < 0x1000) {  // TODO: Replace with InexpensiveConstant with opcode.
        // Can encode as immediate to an add.
        right_op = t_reg2;
        OpRegRegImm(kOpAdd, t_reg2, left_op, delta);
      }
    }

    // Load as constant.
    if (!right_op.Valid()) {
      LoadConstantNoClobber(t_reg2, false_val);
      right_op = t_reg2;
    }

    opcode = kA64Csel4rrrc;
  }

  DCHECK(left_op.Valid() && right_op.Valid());
  NewLIR4(is_wide ? WIDE(opcode) : opcode, rs_dest.GetReg(), left_op.GetReg(), right_op.GetReg(),
      code);
}

void Arm64Mir2Lir::GenSelectConst32(RegStorage left_op, RegStorage right_op, ConditionCode code,
                                    int32_t true_val, int32_t false_val, RegStorage rs_dest,
                                    int dest_reg_class) {
  DCHECK(rs_dest.Valid());
  OpRegReg(kOpCmp, left_op, right_op);
  GenSelect(true_val, false_val, code, rs_dest, dest_reg_class);
}

void Arm64Mir2Lir::GenSelect(BasicBlock* bb, MIR* mir) {
  RegLocation rl_src = mir_graph_->GetSrc(mir, 0);
  rl_src = LoadValue(rl_src, rl_src.ref ? kRefReg : kCoreReg);
  // rl_src may be aliased with rl_result/rl_dest, so do compare early.
  OpRegImm(kOpCmp, rl_src.reg, 0);

  RegLocation rl_dest = mir_graph_->GetDest(mir);

  // The kMirOpSelect has two variants, one for constants and one for moves.
  if (mir->ssa_rep->num_uses == 1) {
    RegLocation rl_result = EvalLoc(rl_dest, rl_dest.ref ? kRefReg : kCoreReg, true);
    GenSelect(mir->dalvikInsn.vB, mir->dalvikInsn.vC, mir->meta.ccode, rl_result.reg,
              rl_dest.ref ? kRefReg : kCoreReg);
    StoreValue(rl_dest, rl_result);
  } else {
    RegLocation rl_true = mir_graph_->reg_location_[mir->ssa_rep->uses[1]];
    RegLocation rl_false = mir_graph_->reg_location_[mir->ssa_rep->uses[2]];

    RegisterClass result_reg_class = rl_dest.ref ? kRefReg : kCoreReg;
    rl_true = LoadValue(rl_true, result_reg_class);
    rl_false = LoadValue(rl_false, result_reg_class);
    RegLocation rl_result = EvalLoc(rl_dest, result_reg_class, true);

    bool is_wide = rl_dest.ref || rl_dest.wide;
    int opcode = is_wide ? WIDE(kA64Csel4rrrc) : kA64Csel4rrrc;
    NewLIR4(opcode, rl_result.reg.GetReg(),
            rl_true.reg.GetReg(), rl_false.reg.GetReg(), ArmConditionEncoding(mir->meta.ccode));
    StoreValue(rl_dest, rl_result);
  }
}

void Arm64Mir2Lir::GenFusedLongCmpBranch(BasicBlock* bb, MIR* mir) {
  RegLocation rl_src1 = mir_graph_->GetSrcWide(mir, 0);
  RegLocation rl_src2 = mir_graph_->GetSrcWide(mir, 2);
  LIR* taken = &block_label_list_[bb->taken];
  LIR* not_taken = &block_label_list_[bb->fall_through];
  // Normalize such that if either operand is constant, src2 will be constant.
  ConditionCode ccode = mir->meta.ccode;
  if (rl_src1.is_const) {
    std::swap(rl_src1, rl_src2);
    ccode = FlipComparisonOrder(ccode);
  }

  rl_src1 = LoadValueWide(rl_src1, kCoreReg);

  if (rl_src2.is_const) {
    // TODO: Optimize for rl_src1.is_const? (Does happen in the boot image at the moment.)

    int64_t val = mir_graph_->ConstantValueWide(rl_src2);
    // Special handling using cbz & cbnz.
    if (val == 0 && (ccode == kCondEq || ccode == kCondNe)) {
      OpCmpImmBranch(ccode, rl_src1.reg, 0, taken);
      OpCmpImmBranch(NegateComparison(ccode), rl_src1.reg, 0, not_taken);
      return;
    }

    // Only handle Imm if src2 is not already in a register.
    rl_src2 = UpdateLocWide(rl_src2);
    if (rl_src2.location != kLocPhysReg) {
      OpRegImm64(kOpCmp, rl_src1.reg, val);
      OpCondBranch(ccode, taken);
      OpCondBranch(NegateComparison(ccode), not_taken);
      return;
    }
  }

  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  OpRegReg(kOpCmp, rl_src1.reg, rl_src2.reg);
  OpCondBranch(ccode, taken);
  OpCondBranch(NegateComparison(ccode), not_taken);
}

/*
 * Generate a register comparison to an immediate and branch.  Caller
 * is responsible for setting branch target field.
 */
LIR* Arm64Mir2Lir::OpCmpImmBranch(ConditionCode cond, RegStorage reg, int check_value,
                                  LIR* target) {
  LIR* branch;
  ArmConditionCode arm_cond = ArmConditionEncoding(cond);
  if (check_value == 0 && (arm_cond == kArmCondEq || arm_cond == kArmCondNe)) {
    ArmOpcode opcode = (arm_cond == kArmCondEq) ? kA64Cbz2rt : kA64Cbnz2rt;
    ArmOpcode wide = reg.Is64Bit() ? WIDE(0) : UNWIDE(0);
    branch = NewLIR2(opcode | wide, reg.GetReg(), 0);
  } else {
    OpRegImm(kOpCmp, reg, check_value);
    branch = NewLIR2(kA64B2ct, arm_cond, 0);
  }
  branch->target = target;
  return branch;
}

LIR* Arm64Mir2Lir::OpCmpMemImmBranch(ConditionCode cond, RegStorage temp_reg,
                                     RegStorage base_reg, int offset, int check_value,
                                     LIR* target, LIR** compare) {
  DCHECK(compare == nullptr);
  // It is possible that temp register is 64-bit. (ArgReg or RefReg)
  // Always compare 32-bit value no matter what temp_reg is.
  if (temp_reg.Is64Bit()) {
    temp_reg = As32BitReg(temp_reg);
  }
  Load32Disp(base_reg, offset, temp_reg);
  LIR* branch = OpCmpImmBranch(cond, temp_reg, check_value, target);
  return branch;
}

LIR* Arm64Mir2Lir::OpRegCopyNoInsert(RegStorage r_dest, RegStorage r_src) {
  bool dest_is_fp = r_dest.IsFloat();
  bool src_is_fp = r_src.IsFloat();
  ArmOpcode opcode = kA64Brk1d;
  LIR* res;

  if (LIKELY(dest_is_fp == src_is_fp)) {
    if (LIKELY(!dest_is_fp)) {
      DCHECK_EQ(r_dest.Is64Bit(), r_src.Is64Bit());

      // Core/core copy.
      // Copies involving the sp register require a different instruction.
      opcode = UNLIKELY(A64_REG_IS_SP(r_dest.GetReg())) ? kA64Add4RRdT : kA64Mov2rr;

      // TODO(Arm64): kA64Add4RRdT formally has 4 args, but is used as a 2 args instruction.
      //   This currently works because the other arguments are set to 0 by default. We should
      //   rather introduce an alias kA64Mov2RR.

      // core/core copy. Do a x/x copy only if both registers are x.
      if (r_dest.Is64Bit() && r_src.Is64Bit()) {
        opcode = WIDE(opcode);
      }
    } else {
      // Float/float copy.
      bool dest_is_double = r_dest.IsDouble();
      bool src_is_double = r_src.IsDouble();

      // We do not do float/double or double/float casts here.
      DCHECK_EQ(dest_is_double, src_is_double);

      // Homogeneous float/float copy.
      opcode = (dest_is_double) ? FWIDE(kA64Fmov2ff) : kA64Fmov2ff;
    }
  } else {
    // Inhomogeneous register copy.
    if (dest_is_fp) {
      if (r_dest.IsDouble()) {
        opcode = kA64Fmov2Sx;
      } else {
        r_src = Check32BitReg(r_src);
        opcode = kA64Fmov2sw;
      }
    } else {
      if (r_src.IsDouble()) {
        opcode = kA64Fmov2xS;
      } else {
        r_dest = Check32BitReg(r_dest);
        opcode = kA64Fmov2ws;
      }
    }
  }

  res = RawLIR(current_dalvik_offset_, opcode, r_dest.GetReg(), r_src.GetReg());

  if (!(cu_->disable_opt & (1 << kSafeOptimizations)) && r_dest == r_src) {
    res->flags.is_nop = true;
  }

  return res;
}

void Arm64Mir2Lir::OpRegCopy(RegStorage r_dest, RegStorage r_src) {
  if (r_dest != r_src) {
    LIR* res = OpRegCopyNoInsert(r_dest, r_src);
    AppendLIR(res);
  }
}

void Arm64Mir2Lir::OpRegCopyWide(RegStorage r_dest, RegStorage r_src) {
  OpRegCopy(r_dest, r_src);
}

// Table of magic divisors
struct MagicTable {
  int magic64_base;
  int magic64_eor;
  uint64_t magic64;
  uint32_t magic32;
  uint32_t shift;
  DividePattern pattern;
};

static const MagicTable magic_table[] = {
  {   0,      0,                  0,          0, 0, DivideNone},  // 0
  {   0,      0,                  0,          0, 0, DivideNone},  // 1
  {   0,      0,                  0,          0, 0, DivideNone},  // 2
  {0x3c,     -1, 0x5555555555555556, 0x55555556, 0, Divide3},     // 3
  {   0,      0,                  0,          0, 0, DivideNone},  // 4
  {0xf9,     -1, 0x6666666666666667, 0x66666667, 1, Divide5},     // 5
  {0x7c, 0x1041, 0x2AAAAAAAAAAAAAAB, 0x2AAAAAAB, 0, Divide3},     // 6
  {  -1,     -1, 0x924924924924924A, 0x92492493, 2, Divide7},     // 7
  {   0,      0,                  0,          0, 0, DivideNone},  // 8
  {  -1,     -1, 0x38E38E38E38E38E4, 0x38E38E39, 1, Divide5},     // 9
  {0xf9,     -1, 0x6666666666666667, 0x66666667, 2, Divide5},     // 10
  {  -1,     -1, 0x2E8BA2E8BA2E8BA3, 0x2E8BA2E9, 1, Divide5},     // 11
  {0x7c, 0x1041, 0x2AAAAAAAAAAAAAAB, 0x2AAAAAAB, 1, Divide5},     // 12
  {  -1,     -1, 0x4EC4EC4EC4EC4EC5, 0x4EC4EC4F, 2, Divide5},     // 13
  {  -1,     -1, 0x924924924924924A, 0x92492493, 3, Divide7},     // 14
  {0x78,     -1, 0x8888888888888889, 0x88888889, 3, Divide7},     // 15
};

// Integer division by constant via reciprocal multiply (Hacker's Delight, 10-4)
bool Arm64Mir2Lir::SmallLiteralDivRem(Instruction::Code dalvik_opcode, bool is_div,
                                      RegLocation rl_src, RegLocation rl_dest, int lit) {
  if ((lit < 0) || (lit >= static_cast<int>(arraysize(magic_table)))) {
    return false;
  }
  DividePattern pattern = magic_table[lit].pattern;
  if (pattern == DivideNone) {
    return false;
  }
  // Tuning: add rem patterns
  if (!is_div) {
    return false;
  }

  RegStorage r_magic = AllocTemp();
  LoadConstant(r_magic, magic_table[lit].magic32);
  rl_src = LoadValue(rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  RegStorage r_long_mul = AllocTemp();
  NewLIR4(kA64Smaddl4xwwx, As64BitReg(r_long_mul).GetReg(),
          r_magic.GetReg(), rl_src.reg.GetReg(), rxzr);
  switch (pattern) {
    case Divide3:
      OpRegRegImm(kOpLsr, As64BitReg(r_long_mul), As64BitReg(r_long_mul), 32);
      OpRegRegRegShift(kOpSub, rl_result.reg, r_long_mul, rl_src.reg, EncodeShift(kA64Asr, 31));
      break;
    case Divide5:
      OpRegRegImm(kOpAsr, As64BitReg(r_long_mul), As64BitReg(r_long_mul),
                  32 + magic_table[lit].shift);
      OpRegRegRegShift(kOpSub, rl_result.reg, r_long_mul, rl_src.reg, EncodeShift(kA64Asr, 31));
      break;
    case Divide7:
      OpRegRegRegShift(kOpAdd, As64BitReg(r_long_mul), As64BitReg(rl_src.reg),
                       As64BitReg(r_long_mul), EncodeShift(kA64Lsr, 32));
      OpRegRegImm(kOpAsr, r_long_mul, r_long_mul, magic_table[lit].shift);
      OpRegRegRegShift(kOpSub, rl_result.reg, r_long_mul, rl_src.reg, EncodeShift(kA64Asr, 31));
      break;
    default:
      LOG(FATAL) << "Unexpected pattern: " << pattern;
  }
  StoreValue(rl_dest, rl_result);
  return true;
}

bool Arm64Mir2Lir::SmallLiteralDivRem64(Instruction::Code dalvik_opcode, bool is_div,
                                        RegLocation rl_src, RegLocation rl_dest, int64_t lit) {
  if ((lit < 0) || (lit >= static_cast<int>(arraysize(magic_table)))) {
    return false;
  }
  DividePattern pattern = magic_table[lit].pattern;
  if (pattern == DivideNone) {
    return false;
  }
  // Tuning: add rem patterns
  if (!is_div) {
    return false;
  }

  RegStorage r_magic = AllocTempWide();
  rl_src = LoadValueWide(rl_src, kCoreReg);
  RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  RegStorage r_long_mul = AllocTempWide();

  if (magic_table[lit].magic64_base >= 0) {
    // Check that the entry in the table is correct.
    if (kIsDebugBuild) {
      uint64_t reconstructed_imm;
      uint64_t base = DecodeLogicalImmediate(/*is_wide*/true, magic_table[lit].magic64_base);
      if (magic_table[lit].magic64_eor >= 0) {
        uint64_t eor = DecodeLogicalImmediate(/*is_wide*/true, magic_table[lit].magic64_eor);
        reconstructed_imm = base ^ eor;
      } else {
        reconstructed_imm = base + 1;
      }
      DCHECK_EQ(reconstructed_imm, magic_table[lit].magic64) << " for literal " << lit;
    }

    // Load the magic constant in two instructions.
    NewLIR3(WIDE(kA64Orr3Rrl), r_magic.GetReg(), rxzr, magic_table[lit].magic64_base);
    if (magic_table[lit].magic64_eor >= 0) {
      NewLIR3(WIDE(kA64Eor3Rrl), r_magic.GetReg(), r_magic.GetReg(),
              magic_table[lit].magic64_eor);
    } else {
      NewLIR4(WIDE(kA64Add4RRdT), r_magic.GetReg(), r_magic.GetReg(), 1, 0);
    }
  } else {
    LoadConstantWide(r_magic, magic_table[lit].magic64);
  }

  NewLIR3(kA64Smulh3xxx, r_long_mul.GetReg(), r_magic.GetReg(), rl_src.reg.GetReg());
  switch (pattern) {
    case Divide3:
      OpRegRegRegShift(kOpSub, rl_result.reg, r_long_mul, rl_src.reg, EncodeShift(kA64Asr, 63));
      break;
    case Divide5:
      OpRegRegImm(kOpAsr, r_long_mul, r_long_mul, magic_table[lit].shift);
      OpRegRegRegShift(kOpSub, rl_result.reg, r_long_mul, rl_src.reg, EncodeShift(kA64Asr, 63));
      break;
    case Divide7:
      OpRegRegReg(kOpAdd, r_long_mul, rl_src.reg, r_long_mul);
      OpRegRegImm(kOpAsr, r_long_mul, r_long_mul, magic_table[lit].shift);
      OpRegRegRegShift(kOpSub, rl_result.reg, r_long_mul, rl_src.reg, EncodeShift(kA64Asr, 63));
      break;
    default:
      LOG(FATAL) << "Unexpected pattern: " << pattern;
  }
  StoreValueWide(rl_dest, rl_result);
  return true;
}

// Returns true if it added instructions to 'cu' to divide 'rl_src' by 'lit'
// and store the result in 'rl_dest'.
bool Arm64Mir2Lir::HandleEasyDivRem(Instruction::Code dalvik_opcode, bool is_div,
                                    RegLocation rl_src, RegLocation rl_dest, int lit) {
  return HandleEasyDivRem64(dalvik_opcode, is_div, rl_src, rl_dest, static_cast<int>(lit));
}

// Returns true if it added instructions to 'cu' to divide 'rl_src' by 'lit'
// and store the result in 'rl_dest'.
bool Arm64Mir2Lir::HandleEasyDivRem64(Instruction::Code dalvik_opcode, bool is_div,
                                      RegLocation rl_src, RegLocation rl_dest, int64_t lit) {
  const bool is_64bit = rl_dest.wide;
  const int nbits = (is_64bit) ? 64 : 32;

  if (lit < 2) {
    return false;
  }
  if (!IsPowerOfTwo(lit)) {
    if (is_64bit) {
      return SmallLiteralDivRem64(dalvik_opcode, is_div, rl_src, rl_dest, lit);
    } else {
      return SmallLiteralDivRem(dalvik_opcode, is_div, rl_src, rl_dest, static_cast<int32_t>(lit));
    }
  }
  int k = LowestSetBit(lit);
  if (k >= nbits - 2) {
    // Avoid special cases.
    return false;
  }

  RegLocation rl_result;
  RegStorage t_reg;
  if (is_64bit) {
    rl_src = LoadValueWide(rl_src, kCoreReg);
    rl_result = EvalLocWide(rl_dest, kCoreReg, true);
    t_reg = AllocTempWide();
  } else {
    rl_src = LoadValue(rl_src, kCoreReg);
    rl_result = EvalLoc(rl_dest, kCoreReg, true);
    t_reg = AllocTemp();
  }

  int shift = EncodeShift(kA64Lsr, nbits - k);
  if (is_div) {
    if (lit == 2) {
      // Division by 2 is by far the most common division by constant.
      OpRegRegRegShift(kOpAdd, t_reg, rl_src.reg, rl_src.reg, shift);
      OpRegRegImm(kOpAsr, rl_result.reg, t_reg, k);
    } else {
      OpRegRegImm(kOpAsr, t_reg, rl_src.reg, nbits - 1);
      OpRegRegRegShift(kOpAdd, t_reg, rl_src.reg, t_reg, shift);
      OpRegRegImm(kOpAsr, rl_result.reg, t_reg, k);
    }
  } else {
    if (lit == 2) {
      OpRegRegRegShift(kOpAdd, t_reg, rl_src.reg, rl_src.reg, shift);
      OpRegRegImm64(kOpAnd, t_reg, t_reg, lit - 1);
      OpRegRegRegShift(kOpSub, rl_result.reg, t_reg, rl_src.reg, shift);
    } else {
      RegStorage t_reg2 = (is_64bit) ? AllocTempWide() : AllocTemp();
      OpRegRegImm(kOpAsr, t_reg, rl_src.reg, nbits - 1);
      OpRegRegRegShift(kOpAdd, t_reg2, rl_src.reg, t_reg, shift);
      OpRegRegImm64(kOpAnd, t_reg2, t_reg2, lit - 1);
      OpRegRegRegShift(kOpSub, rl_result.reg, t_reg2, t_reg, shift);
    }
  }

  if (is_64bit) {
    StoreValueWide(rl_dest, rl_result);
  } else {
    StoreValue(rl_dest, rl_result);
  }
  return true;
}

bool Arm64Mir2Lir::EasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit) {
  LOG(FATAL) << "Unexpected use of EasyMultiply for Arm64";
  return false;
}

RegLocation Arm64Mir2Lir::GenDivRemLit(RegLocation rl_dest, RegLocation rl_src1, int lit, bool is_div) {
  LOG(FATAL) << "Unexpected use of GenDivRemLit for Arm64";
  return rl_dest;
}

RegLocation Arm64Mir2Lir::GenDivRemLit(RegLocation rl_dest, RegStorage reg1, int lit, bool is_div) {
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);

  // Put the literal in a temp.
  RegStorage lit_temp = AllocTemp();
  LoadConstant(lit_temp, lit);
  // Use the generic case for div/rem with arg2 in a register.
  // TODO: The literal temp can be freed earlier during a modulus to reduce reg pressure.
  rl_result = GenDivRem(rl_result, reg1, lit_temp, is_div);
  FreeTemp(lit_temp);

  return rl_result;
}

RegLocation Arm64Mir2Lir::GenDivRem(RegLocation rl_dest, RegLocation rl_src1,
                                    RegLocation rl_src2, bool is_div, bool check_zero) {
  LOG(FATAL) << "Unexpected use of GenDivRem for Arm64";
  return rl_dest;
}

RegLocation Arm64Mir2Lir::GenDivRem(RegLocation rl_dest, RegStorage r_src1, RegStorage r_src2,
                                    bool is_div) {
  CHECK_EQ(r_src1.Is64Bit(), r_src2.Is64Bit());

  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (is_div) {
    OpRegRegReg(kOpDiv, rl_result.reg, r_src1, r_src2);
  } else {
    // temp = r_src1 / r_src2
    // dest = r_src1 - temp * r_src2
    RegStorage temp;
    ArmOpcode wide;
    if (rl_result.reg.Is64Bit()) {
      temp = AllocTempWide();
      wide = WIDE(0);
    } else {
      temp = AllocTemp();
      wide = UNWIDE(0);
    }
    OpRegRegReg(kOpDiv, temp, r_src1, r_src2);
    NewLIR4(kA64Msub4rrrr | wide, rl_result.reg.GetReg(), temp.GetReg(),
            r_src1.GetReg(), r_src2.GetReg());
    FreeTemp(temp);
  }
  return rl_result;
}

bool Arm64Mir2Lir::GenInlinedAbsLong(CallInfo* info) {
  RegLocation rl_src = info->args[0];
  rl_src = LoadValueWide(rl_src, kCoreReg);
  RegLocation rl_dest = InlineTargetWide(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  RegStorage sign_reg = AllocTempWide();
  // abs(x) = y<=x>>63, (x+y)^y.
  OpRegRegImm(kOpAsr, sign_reg, rl_src.reg, 63);
  OpRegRegReg(kOpAdd, rl_result.reg, rl_src.reg, sign_reg);
  OpRegReg(kOpXor, rl_result.reg, sign_reg);
  StoreValueWide(rl_dest, rl_result);
  return true;
}

bool Arm64Mir2Lir::GenInlinedMinMax(CallInfo* info, bool is_min, bool is_long) {
  DCHECK_EQ(cu_->instruction_set, kArm64);
  RegLocation rl_src1 = info->args[0];
  RegLocation rl_src2 = (is_long) ? info->args[2] : info->args[1];
  rl_src1 = (is_long) ? LoadValueWide(rl_src1, kCoreReg) : LoadValue(rl_src1, kCoreReg);
  rl_src2 = (is_long) ? LoadValueWide(rl_src2, kCoreReg) : LoadValue(rl_src2, kCoreReg);
  RegLocation rl_dest = (is_long) ? InlineTargetWide(info) : InlineTarget(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  OpRegReg(kOpCmp, rl_src1.reg, rl_src2.reg);
  NewLIR4((is_long) ? WIDE(kA64Csel4rrrc) : kA64Csel4rrrc, rl_result.reg.GetReg(),
          rl_src1.reg.GetReg(), rl_src2.reg.GetReg(), (is_min) ? kArmCondLt : kArmCondGt);
  (is_long) ?  StoreValueWide(rl_dest, rl_result) :StoreValue(rl_dest, rl_result);
  return true;
}

bool Arm64Mir2Lir::GenInlinedPeek(CallInfo* info, OpSize size) {
  RegLocation rl_src_address = info->args[0];  // long address
  RegLocation rl_dest = (size == k64) ? InlineTargetWide(info) : InlineTarget(info);
  RegLocation rl_address = LoadValueWide(rl_src_address, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);

  LoadBaseDisp(rl_address.reg, 0, rl_result.reg, size, kNotVolatile);
  if (size == k64) {
    StoreValueWide(rl_dest, rl_result);
  } else {
    DCHECK(size == kSignedByte || size == kSignedHalf || size == k32);
    StoreValue(rl_dest, rl_result);
  }
  return true;
}

bool Arm64Mir2Lir::GenInlinedPoke(CallInfo* info, OpSize size) {
  RegLocation rl_src_address = info->args[0];  // long address
  RegLocation rl_src_value = info->args[2];  // [size] value
  RegLocation rl_address = LoadValueWide(rl_src_address, kCoreReg);

  RegLocation rl_value;
  if (size == k64) {
    rl_value = LoadValueWide(rl_src_value, kCoreReg);
  } else {
    DCHECK(size == kSignedByte || size == kSignedHalf || size == k32);
    rl_value = LoadValue(rl_src_value, kCoreReg);
  }
  StoreBaseDisp(rl_address.reg, 0, rl_value.reg, size, kNotVolatile);
  return true;
}

void Arm64Mir2Lir::OpLea(RegStorage r_base, RegStorage reg1, RegStorage reg2, int scale, int offset) {
  LOG(FATAL) << "Unexpected use of OpLea for Arm64";
}

void Arm64Mir2Lir::OpTlsCmp(ThreadOffset<4> offset, int val) {
  UNIMPLEMENTED(FATAL) << "Should not be used.";
}

void Arm64Mir2Lir::OpTlsCmp(ThreadOffset<8> offset, int val) {
  LOG(FATAL) << "Unexpected use of OpTlsCmp for Arm64";
}

bool Arm64Mir2Lir::GenInlinedCas(CallInfo* info, bool is_long, bool is_object) {
  DCHECK_EQ(cu_->instruction_set, kArm64);
  // Unused - RegLocation rl_src_unsafe = info->args[0];
  RegLocation rl_src_obj = info->args[1];  // Object - known non-null
  RegLocation rl_src_offset = info->args[2];  // long low
  RegLocation rl_src_expected = info->args[4];  // int, long or Object
  // If is_long, high half is in info->args[5]
  RegLocation rl_src_new_value = info->args[is_long ? 6 : 5];  // int, long or Object
  // If is_long, high half is in info->args[7]
  RegLocation rl_dest = InlineTarget(info);  // boolean place for result

  // Load Object and offset
  RegLocation rl_object = LoadValue(rl_src_obj, kRefReg);
  RegLocation rl_offset = LoadValueWide(rl_src_offset, kCoreReg);

  RegLocation rl_new_value;
  RegLocation rl_expected;
  if (is_long) {
    rl_new_value = LoadValueWide(rl_src_new_value, kCoreReg);
    rl_expected = LoadValueWide(rl_src_expected, kCoreReg);
  } else {
    rl_new_value = LoadValue(rl_src_new_value, is_object ? kRefReg : kCoreReg);
    rl_expected = LoadValue(rl_src_expected, is_object ? kRefReg : kCoreReg);
  }

  if (is_object && !mir_graph_->IsConstantNullRef(rl_new_value)) {
    // Mark card for object assuming new value is stored.
    MarkGCCard(rl_new_value.reg, rl_object.reg);
  }

  RegStorage r_ptr = AllocTempRef();
  OpRegRegReg(kOpAdd, r_ptr, rl_object.reg, rl_offset.reg);

  // Free now unneeded rl_object and rl_offset to give more temps.
  ClobberSReg(rl_object.s_reg_low);
  FreeTemp(rl_object.reg);
  ClobberSReg(rl_offset.s_reg_low);
  FreeTemp(rl_offset.reg);

  // do {
  //   tmp = [r_ptr] - expected;
  // } while (tmp == 0 && failure([r_ptr] <- r_new_value));
  // result = tmp != 0;

  RegStorage r_tmp;
  RegStorage r_tmp_stored;
  RegStorage rl_new_value_stored = rl_new_value.reg;
  ArmOpcode wide = UNWIDE(0);
  if (is_long) {
    r_tmp_stored = r_tmp = AllocTempWide();
    wide = WIDE(0);
  } else if (is_object) {
    // References use 64-bit registers, but are stored as compressed 32-bit values.
    // This means r_tmp_stored != r_tmp.
    r_tmp = AllocTempRef();
    r_tmp_stored = As32BitReg(r_tmp);
    rl_new_value_stored = As32BitReg(rl_new_value_stored);
  } else {
    r_tmp_stored = r_tmp = AllocTemp();
  }

  RegStorage r_tmp32 = (r_tmp.Is32Bit()) ? r_tmp : As32BitReg(r_tmp);
  LIR* loop = NewLIR0(kPseudoTargetLabel);
  NewLIR2(kA64Ldaxr2rX | wide, r_tmp_stored.GetReg(), r_ptr.GetReg());
  OpRegReg(kOpCmp, r_tmp, rl_expected.reg);
  DCHECK(last_lir_insn_->u.m.def_mask->HasBit(ResourceMask::kCCode));
  LIR* early_exit = OpCondBranch(kCondNe, NULL);
  NewLIR3(kA64Stlxr3wrX | wide, r_tmp32.GetReg(), rl_new_value_stored.GetReg(), r_ptr.GetReg());
  NewLIR3(kA64Cmp3RdT, r_tmp32.GetReg(), 0, ENCODE_NO_SHIFT);
  DCHECK(last_lir_insn_->u.m.def_mask->HasBit(ResourceMask::kCCode));
  OpCondBranch(kCondNe, loop);

  LIR* exit_loop = NewLIR0(kPseudoTargetLabel);
  early_exit->target = exit_loop;

  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  NewLIR4(kA64Csinc4rrrc, rl_result.reg.GetReg(), rwzr, rwzr, kArmCondNe);

  FreeTemp(r_tmp);  // Now unneeded.
  FreeTemp(r_ptr);  // Now unneeded.

  StoreValue(rl_dest, rl_result);

  return true;
}

LIR* Arm64Mir2Lir::OpPcRelLoad(RegStorage reg, LIR* target) {
  return RawLIR(current_dalvik_offset_, WIDE(kA64Ldr2rp), reg.GetReg(), 0, 0, 0, 0, target);
}

LIR* Arm64Mir2Lir::OpVldm(RegStorage r_base, int count) {
  LOG(FATAL) << "Unexpected use of OpVldm for Arm64";
  return NULL;
}

LIR* Arm64Mir2Lir::OpVstm(RegStorage r_base, int count) {
  LOG(FATAL) << "Unexpected use of OpVstm for Arm64";
  return NULL;
}

void Arm64Mir2Lir::GenMultiplyByTwoBitMultiplier(RegLocation rl_src,
                                               RegLocation rl_result, int lit,
                                               int first_bit, int second_bit) {
  OpRegRegRegShift(kOpAdd, rl_result.reg, rl_src.reg, rl_src.reg, EncodeShift(kA64Lsl, second_bit - first_bit));
  if (first_bit != 0) {
    OpRegRegImm(kOpLsl, rl_result.reg, rl_result.reg, first_bit);
  }
}

void Arm64Mir2Lir::GenDivZeroCheckWide(RegStorage reg) {
  LOG(FATAL) << "Unexpected use of GenDivZero for Arm64";
}

// Test suspend flag, return target of taken suspend branch
LIR* Arm64Mir2Lir::OpTestSuspend(LIR* target) {
  NewLIR3(kA64Subs3rRd, rwSUSPEND, rwSUSPEND, 1);
  return OpCondBranch((target == NULL) ? kCondEq : kCondNe, target);
}

// Decrement register and branch on condition
LIR* Arm64Mir2Lir::OpDecAndBranch(ConditionCode c_code, RegStorage reg, LIR* target) {
  // Combine sub & test using sub setflags encoding here.  We need to make sure a
  // subtract form that sets carry is used, so generate explicitly.
  // TODO: might be best to add a new op, kOpSubs, and handle it generically.
  ArmOpcode opcode = reg.Is64Bit() ? WIDE(kA64Subs3rRd) : UNWIDE(kA64Subs3rRd);
  NewLIR3(opcode, reg.GetReg(), reg.GetReg(), 1);  // For value == 1, this should set flags.
  DCHECK(last_lir_insn_->u.m.def_mask->HasBit(ResourceMask::kCCode));
  return OpCondBranch(c_code, target);
}

bool Arm64Mir2Lir::GenMemBarrier(MemBarrierKind barrier_kind) {
#if ANDROID_SMP != 0
  // Start off with using the last LIR as the barrier. If it is not enough, then we will generate one.
  LIR* barrier = last_lir_insn_;

  int dmb_flavor;
  // TODO: revisit Arm barrier kinds
  switch (barrier_kind) {
    case kAnyStore: dmb_flavor = kISH; break;
    case kLoadAny: dmb_flavor = kISH; break;
        // We conjecture that kISHLD is insufficient.  It is documented
        // to provide LoadLoad | StoreStore ordering.  But if this were used
        // to implement volatile loads, we suspect that the lack of store
        // atomicity on ARM would cause us to allow incorrect results for
        // the canonical IRIW example.  But we're not sure.
        // We should be using acquire loads instead.
    case kStoreStore: dmb_flavor = kISHST; break;
    case kAnyAny: dmb_flavor = kISH; break;
    default:
      LOG(FATAL) << "Unexpected MemBarrierKind: " << barrier_kind;
      dmb_flavor = kSY;  // quiet gcc.
      break;
  }

  bool ret = false;

  // If the same barrier already exists, don't generate another.
  if (barrier == nullptr
      || (barrier->opcode != kA64Dmb1B || barrier->operands[0] != dmb_flavor)) {
    barrier = NewLIR1(kA64Dmb1B, dmb_flavor);
    ret = true;
  }

  // At this point we must have a memory barrier. Mark it as a scheduling barrier as well.
  DCHECK(!barrier->flags.use_def_invalid);
  barrier->u.m.def_mask = &kEncodeAll;
  return ret;
#else
  return false;
#endif
}

void Arm64Mir2Lir::GenIntToLong(RegLocation rl_dest, RegLocation rl_src) {
  RegLocation rl_result;

  rl_src = LoadValue(rl_src, kCoreReg);
  rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  NewLIR4(WIDE(kA64Sbfm4rrdd), rl_result.reg.GetReg(), As64BitReg(rl_src.reg).GetReg(), 0, 31);
  StoreValueWide(rl_dest, rl_result);
}

void Arm64Mir2Lir::GenDivRemLong(Instruction::Code opcode, RegLocation rl_dest,
                                 RegLocation rl_src1, RegLocation rl_src2, bool is_div) {
  if (rl_src2.is_const) {
    DCHECK(rl_src2.wide);
    int64_t lit = mir_graph_->ConstantValueWide(rl_src2);
    if (HandleEasyDivRem64(opcode, is_div, rl_src1, rl_dest, lit)) {
      return;
    }
  }

  RegLocation rl_result;
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  GenDivZeroCheck(rl_src2.reg);
  rl_result = GenDivRem(rl_dest, rl_src1.reg, rl_src2.reg, is_div);
  StoreValueWide(rl_dest, rl_result);
}

void Arm64Mir2Lir::GenLongOp(OpKind op, RegLocation rl_dest, RegLocation rl_src1,
                             RegLocation rl_src2) {
  RegLocation rl_result;

  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  OpRegRegRegShift(op, rl_result.reg, rl_src1.reg, rl_src2.reg, ENCODE_NO_SHIFT);
  StoreValueWide(rl_dest, rl_result);
}

void Arm64Mir2Lir::GenNegLong(RegLocation rl_dest, RegLocation rl_src) {
  RegLocation rl_result;

  rl_src = LoadValueWide(rl_src, kCoreReg);
  rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  OpRegRegShift(kOpNeg, rl_result.reg, rl_src.reg, ENCODE_NO_SHIFT);
  StoreValueWide(rl_dest, rl_result);
}

void Arm64Mir2Lir::GenNotLong(RegLocation rl_dest, RegLocation rl_src) {
  RegLocation rl_result;

  rl_src = LoadValueWide(rl_src, kCoreReg);
  rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  OpRegRegShift(kOpMvn, rl_result.reg, rl_src.reg, ENCODE_NO_SHIFT);
  StoreValueWide(rl_dest, rl_result);
}

void Arm64Mir2Lir::GenMulLong(Instruction::Code opcode, RegLocation rl_dest,
                              RegLocation rl_src1, RegLocation rl_src2) {
  GenLongOp(kOpMul, rl_dest, rl_src1, rl_src2);
}

void Arm64Mir2Lir::GenAddLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                              RegLocation rl_src2) {
  GenLongOp(kOpAdd, rl_dest, rl_src1, rl_src2);
}

void Arm64Mir2Lir::GenSubLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) {
  GenLongOp(kOpSub, rl_dest, rl_src1, rl_src2);
}

void Arm64Mir2Lir::GenAndLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) {
  GenLongOp(kOpAnd, rl_dest, rl_src1, rl_src2);
}

void Arm64Mir2Lir::GenOrLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                           RegLocation rl_src2) {
  GenLongOp(kOpOr, rl_dest, rl_src1, rl_src2);
}

void Arm64Mir2Lir::GenXorLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) {
  GenLongOp(kOpXor, rl_dest, rl_src1, rl_src2);
}

/*
 * Generate array load
 */
void Arm64Mir2Lir::GenArrayGet(int opt_flags, OpSize size, RegLocation rl_array,
                             RegLocation rl_index, RegLocation rl_dest, int scale) {
  RegisterClass reg_class = RegClassBySize(size);
  int len_offset = mirror::Array::LengthOffset().Int32Value();
  int data_offset;
  RegLocation rl_result;
  bool constant_index = rl_index.is_const;
  rl_array = LoadValue(rl_array, kRefReg);
  if (!constant_index) {
    rl_index = LoadValue(rl_index, kCoreReg);
  }

  if (rl_dest.wide) {
    data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Int32Value();
  } else {
    data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Int32Value();
  }

  // If index is constant, just fold it into the data offset
  if (constant_index) {
    data_offset += mir_graph_->ConstantValue(rl_index) << scale;
  }

  /* null object? */
  GenNullCheck(rl_array.reg, opt_flags);

  bool needs_range_check = (!(opt_flags & MIR_IGNORE_RANGE_CHECK));
  RegStorage reg_len;
  if (needs_range_check) {
    reg_len = AllocTemp();
    /* Get len */
    Load32Disp(rl_array.reg, len_offset, reg_len);
    MarkPossibleNullPointerException(opt_flags);
  } else {
    ForceImplicitNullCheck(rl_array.reg, opt_flags);
  }
  if (rl_dest.wide || rl_dest.fp || constant_index) {
    RegStorage reg_ptr;
    if (constant_index) {
      reg_ptr = rl_array.reg;  // NOTE: must not alter reg_ptr in constant case.
    } else {
      // No special indexed operation, lea + load w/ displacement
      reg_ptr = AllocTempRef();
      OpRegRegRegShift(kOpAdd, reg_ptr, rl_array.reg, As64BitReg(rl_index.reg),
                       EncodeShift(kA64Lsl, scale));
      FreeTemp(rl_index.reg);
    }
    rl_result = EvalLoc(rl_dest, reg_class, true);

    if (needs_range_check) {
      if (constant_index) {
        GenArrayBoundsCheck(mir_graph_->ConstantValue(rl_index), reg_len);
      } else {
        GenArrayBoundsCheck(rl_index.reg, reg_len);
      }
      FreeTemp(reg_len);
    }
    if (rl_result.ref) {
      LoadRefDisp(reg_ptr, data_offset, rl_result.reg, kNotVolatile);
    } else {
      LoadBaseDisp(reg_ptr, data_offset, rl_result.reg, size, kNotVolatile);
    }
    MarkPossibleNullPointerException(opt_flags);
    if (!constant_index) {
      FreeTemp(reg_ptr);
    }
    if (rl_dest.wide) {
      StoreValueWide(rl_dest, rl_result);
    } else {
      StoreValue(rl_dest, rl_result);
    }
  } else {
    // Offset base, then use indexed load
    RegStorage reg_ptr = AllocTempRef();
    OpRegRegImm(kOpAdd, reg_ptr, rl_array.reg, data_offset);
    FreeTemp(rl_array.reg);
    rl_result = EvalLoc(rl_dest, reg_class, true);

    if (needs_range_check) {
      GenArrayBoundsCheck(rl_index.reg, reg_len);
      FreeTemp(reg_len);
    }
    if (rl_result.ref) {
      LoadRefIndexed(reg_ptr, As64BitReg(rl_index.reg), rl_result.reg, scale);
    } else {
      LoadBaseIndexed(reg_ptr, As64BitReg(rl_index.reg), rl_result.reg, scale, size);
    }
    MarkPossibleNullPointerException(opt_flags);
    FreeTemp(reg_ptr);
    StoreValue(rl_dest, rl_result);
  }
}

/*
 * Generate array store
 *
 */
void Arm64Mir2Lir::GenArrayPut(int opt_flags, OpSize size, RegLocation rl_array,
                             RegLocation rl_index, RegLocation rl_src, int scale, bool card_mark) {
  RegisterClass reg_class = RegClassBySize(size);
  int len_offset = mirror::Array::LengthOffset().Int32Value();
  bool constant_index = rl_index.is_const;

  int data_offset;
  if (size == k64 || size == kDouble) {
    data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Int32Value();
  } else {
    data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Int32Value();
  }

  // If index is constant, just fold it into the data offset.
  if (constant_index) {
    data_offset += mir_graph_->ConstantValue(rl_index) << scale;
  }

  rl_array = LoadValue(rl_array, kRefReg);
  if (!constant_index) {
    rl_index = LoadValue(rl_index, kCoreReg);
  }

  RegStorage reg_ptr;
  bool allocated_reg_ptr_temp = false;
  if (constant_index) {
    reg_ptr = rl_array.reg;
  } else if (IsTemp(rl_array.reg) && !card_mark) {
    Clobber(rl_array.reg);
    reg_ptr = rl_array.reg;
  } else {
    allocated_reg_ptr_temp = true;
    reg_ptr = AllocTempRef();
  }

  /* null object? */
  GenNullCheck(rl_array.reg, opt_flags);

  bool needs_range_check = (!(opt_flags & MIR_IGNORE_RANGE_CHECK));
  RegStorage reg_len;
  if (needs_range_check) {
    reg_len = AllocTemp();
    // NOTE: max live temps(4) here.
    /* Get len */
    Load32Disp(rl_array.reg, len_offset, reg_len);
    MarkPossibleNullPointerException(opt_flags);
  } else {
    ForceImplicitNullCheck(rl_array.reg, opt_flags);
  }
  /* at this point, reg_ptr points to array, 2 live temps */
  if (rl_src.wide || rl_src.fp || constant_index) {
    if (rl_src.wide) {
      rl_src = LoadValueWide(rl_src, reg_class);
    } else {
      rl_src = LoadValue(rl_src, reg_class);
    }
    if (!constant_index) {
      OpRegRegRegShift(kOpAdd, reg_ptr, rl_array.reg, As64BitReg(rl_index.reg),
                       EncodeShift(kA64Lsl, scale));
    }
    if (needs_range_check) {
      if (constant_index) {
        GenArrayBoundsCheck(mir_graph_->ConstantValue(rl_index), reg_len);
      } else {
        GenArrayBoundsCheck(rl_index.reg, reg_len);
      }
      FreeTemp(reg_len);
    }
    if (rl_src.ref) {
      StoreRefDisp(reg_ptr, data_offset, rl_src.reg, kNotVolatile);
    } else {
      StoreBaseDisp(reg_ptr, data_offset, rl_src.reg, size, kNotVolatile);
    }
    MarkPossibleNullPointerException(opt_flags);
  } else {
    /* reg_ptr -> array data */
    OpRegRegImm(kOpAdd, reg_ptr, rl_array.reg, data_offset);
    rl_src = LoadValue(rl_src, reg_class);
    if (needs_range_check) {
      GenArrayBoundsCheck(rl_index.reg, reg_len);
      FreeTemp(reg_len);
    }
    if (rl_src.ref) {
      StoreRefIndexed(reg_ptr, As64BitReg(rl_index.reg), rl_src.reg, scale);
    } else {
      StoreBaseIndexed(reg_ptr, As64BitReg(rl_index.reg), rl_src.reg, scale, size);
    }
    MarkPossibleNullPointerException(opt_flags);
  }
  if (allocated_reg_ptr_temp) {
    FreeTemp(reg_ptr);
  }
  if (card_mark) {
    MarkGCCard(rl_src.reg, rl_array.reg);
  }
}

void Arm64Mir2Lir::GenShiftImmOpLong(Instruction::Code opcode,
                                     RegLocation rl_dest, RegLocation rl_src, RegLocation rl_shift) {
  OpKind op = kOpBkpt;
  // Per spec, we only care about low 6 bits of shift amount.
  int shift_amount = mir_graph_->ConstantValue(rl_shift) & 0x3f;
  rl_src = LoadValueWide(rl_src, kCoreReg);
  if (shift_amount == 0) {
    StoreValueWide(rl_dest, rl_src);
    return;
  }

  RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  switch (opcode) {
    case Instruction::SHL_LONG:
    case Instruction::SHL_LONG_2ADDR:
      op = kOpLsl;
      break;
    case Instruction::SHR_LONG:
    case Instruction::SHR_LONG_2ADDR:
      op = kOpAsr;
      break;
    case Instruction::USHR_LONG:
    case Instruction::USHR_LONG_2ADDR:
      op = kOpLsr;
      break;
    default:
      LOG(FATAL) << "Unexpected case";
  }
  OpRegRegImm(op, rl_result.reg, rl_src.reg, shift_amount);
  StoreValueWide(rl_dest, rl_result);
}

void Arm64Mir2Lir::GenArithImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                     RegLocation rl_src1, RegLocation rl_src2) {
  if ((opcode == Instruction::SUB_LONG) || (opcode == Instruction::SUB_LONG_2ADDR)) {
    if (!rl_src2.is_const) {
      return GenArithOpLong(opcode, rl_dest, rl_src1, rl_src2);
    }
  } else {
    // Associativity.
    if (!rl_src2.is_const) {
      DCHECK(rl_src1.is_const);
      std::swap(rl_src1, rl_src2);
    }
  }
  DCHECK(rl_src2.is_const);

  OpKind op = kOpBkpt;
  int64_t val = mir_graph_->ConstantValueWide(rl_src2);

  switch (opcode) {
    case Instruction::ADD_LONG:
    case Instruction::ADD_LONG_2ADDR:
      op = kOpAdd;
      break;
    case Instruction::SUB_LONG:
    case Instruction::SUB_LONG_2ADDR:
      op = kOpSub;
      break;
    case Instruction::AND_LONG:
    case Instruction::AND_LONG_2ADDR:
      op = kOpAnd;
      break;
    case Instruction::OR_LONG:
    case Instruction::OR_LONG_2ADDR:
      op = kOpOr;
      break;
    case Instruction::XOR_LONG:
    case Instruction::XOR_LONG_2ADDR:
      op = kOpXor;
      break;
    default:
      LOG(FATAL) << "Unexpected opcode";
  }

  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  OpRegRegImm64(op, rl_result.reg, rl_src1.reg, val);
  StoreValueWide(rl_dest, rl_result);
}

/**
 * @brief Split a register list in pairs or registers.
 *
 * Given a list of registers in @p reg_mask, split the list in pairs. Use as follows:
 * @code
 *   int reg1 = -1, reg2 = -1;
 *   while (reg_mask) {
 *     reg_mask = GenPairWise(reg_mask, & reg1, & reg2);
 *     if (UNLIKELY(reg2 < 0)) {
 *       // Single register in reg1.
 *     } else {
 *       // Pair in reg1, reg2.
 *     }
 *   }
 * @endcode
 */
uint32_t Arm64Mir2Lir::GenPairWise(uint32_t reg_mask, int* reg1, int* reg2) {
  // Find first register.
  int first_bit_set = __builtin_ctz(reg_mask) + 1;
  int reg = *reg1 + first_bit_set;
  reg_mask >>= first_bit_set;

  if (LIKELY(reg_mask)) {
    // Save the first register, find the second and use the pair opcode.
    int second_bit_set = __builtin_ctz(reg_mask) + 1;
    *reg2 = reg;
    reg_mask >>= second_bit_set;
    *reg1 = reg + second_bit_set;
    return reg_mask;
  }

  // Use the single opcode, as we just have one register.
  *reg1 = reg;
  *reg2 = -1;
  return reg_mask;
}

void Arm64Mir2Lir::UnSpillCoreRegs(RegStorage base, int offset, uint32_t reg_mask) {
  int reg1 = -1, reg2 = -1;
  const int reg_log2_size = 3;

  for (offset = (offset >> reg_log2_size); reg_mask; offset += 2) {
     reg_mask = GenPairWise(reg_mask, & reg1, & reg2);
    if (UNLIKELY(reg2 < 0)) {
      NewLIR3(WIDE(kA64Ldr3rXD), RegStorage::Solo64(reg1).GetReg(), base.GetReg(), offset);
    } else {
      DCHECK_LE(offset, 63);
      NewLIR4(WIDE(kA64Ldp4rrXD), RegStorage::Solo64(reg2).GetReg(),
              RegStorage::Solo64(reg1).GetReg(), base.GetReg(), offset);
    }
  }
}

void Arm64Mir2Lir::SpillCoreRegs(RegStorage base, int offset, uint32_t reg_mask) {
  int reg1 = -1, reg2 = -1;
  const int reg_log2_size = 3;

  for (offset = (offset >> reg_log2_size); reg_mask; offset += 2) {
    reg_mask = GenPairWise(reg_mask, & reg1, & reg2);
    if (UNLIKELY(reg2 < 0)) {
      NewLIR3(WIDE(kA64Str3rXD), RegStorage::Solo64(reg1).GetReg(), base.GetReg(), offset);
    } else {
      NewLIR4(WIDE(kA64Stp4rrXD), RegStorage::Solo64(reg2).GetReg(),
              RegStorage::Solo64(reg1).GetReg(), base.GetReg(), offset);
    }
  }
}

void Arm64Mir2Lir::UnSpillFPRegs(RegStorage base, int offset, uint32_t reg_mask) {
  int reg1 = -1, reg2 = -1;
  const int reg_log2_size = 3;

  for (offset = (offset >> reg_log2_size); reg_mask; offset += 2) {
     reg_mask = GenPairWise(reg_mask, & reg1, & reg2);
    if (UNLIKELY(reg2 < 0)) {
      NewLIR3(FWIDE(kA64Ldr3fXD), RegStorage::FloatSolo64(reg1).GetReg(), base.GetReg(), offset);
    } else {
      NewLIR4(WIDE(kA64Ldp4ffXD), RegStorage::FloatSolo64(reg2).GetReg(),
              RegStorage::FloatSolo64(reg1).GetReg(), base.GetReg(), offset);
    }
  }
}

// TODO(Arm64): consider using ld1 and st1?
void Arm64Mir2Lir::SpillFPRegs(RegStorage base, int offset, uint32_t reg_mask) {
  int reg1 = -1, reg2 = -1;
  const int reg_log2_size = 3;

  for (offset = (offset >> reg_log2_size); reg_mask; offset += 2) {
    reg_mask = GenPairWise(reg_mask, & reg1, & reg2);
    if (UNLIKELY(reg2 < 0)) {
      NewLIR3(FWIDE(kA64Str3fXD), RegStorage::FloatSolo64(reg1).GetReg(), base.GetReg(), offset);
    } else {
      NewLIR4(WIDE(kA64Stp4ffXD), RegStorage::FloatSolo64(reg2).GetReg(),
              RegStorage::FloatSolo64(reg1).GetReg(), base.GetReg(), offset);
    }
  }
}

bool Arm64Mir2Lir::GenInlinedReverseBits(CallInfo* info, OpSize size) {
  ArmOpcode wide = (size == k64) ? WIDE(0) : UNWIDE(0);
  RegLocation rl_src_i = info->args[0];
  RegLocation rl_dest = (size == k64) ? InlineTargetWide(info) : InlineTarget(info);  // result reg
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  RegLocation rl_i = (size == k64) ? LoadValueWide(rl_src_i, kCoreReg) : LoadValue(rl_src_i, kCoreReg);
  NewLIR2(kA64Rbit2rr | wide, rl_result.reg.GetReg(), rl_i.reg.GetReg());
  (size == k64) ? StoreValueWide(rl_dest, rl_result) : StoreValue(rl_dest, rl_result);
  return true;
}

}  // namespace art
