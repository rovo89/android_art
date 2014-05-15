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
#include "entrypoints/quick/quick_entrypoints.h"
#include "mirror/array.h"

namespace art {

LIR* Arm64Mir2Lir::OpCmpBranch(ConditionCode cond, RegStorage src1, RegStorage src2, LIR* target) {
  OpRegReg(kOpCmp, src1, src2);
  return OpCondBranch(cond, target);
}

// TODO(Arm64): remove this.
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
 *     csinc wC, wzr, wzr, eq
 *     csneg wC, wC, wC, le
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
          rl_result.reg.GetReg(), kArmCondLe);
  StoreValue(rl_dest, rl_result);
}

void Arm64Mir2Lir::GenFusedLongCmpImmBranch(BasicBlock* bb, RegLocation rl_src1,
                                            int64_t val, ConditionCode ccode) {
  LIR* taken = &block_label_list_[bb->taken];
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);

  if (val == 0 && (ccode == kCondEq || ccode == kCondNe)) {
    ArmOpcode opcode = (ccode == kCondEq) ? kA64Cbz2rt : kA64Cbnz2rt;
    LIR* branch = NewLIR2(WIDE(opcode), rl_src1.reg.GetLowReg(), 0);
    branch->target = taken;
  } else {
    OpRegImm64(kOpCmp, rl_src1.reg, val, /*is_wide*/true);
    OpCondBranch(ccode, taken);
  }
}

void Arm64Mir2Lir::GenSelect(BasicBlock* bb, MIR* mir) {
  // TODO(Arm64): implement this.
  UNIMPLEMENTED(FATAL);

  RegLocation rl_result;
  RegLocation rl_src = mir_graph_->GetSrc(mir, 0);
  RegLocation rl_dest = mir_graph_->GetDest(mir);
  rl_src = LoadValue(rl_src, kCoreReg);
  ConditionCode ccode = mir->meta.ccode;
  if (mir->ssa_rep->num_uses == 1) {
    // CONST case
    int true_val = mir->dalvikInsn.vB;
    int false_val = mir->dalvikInsn.vC;
    rl_result = EvalLoc(rl_dest, kCoreReg, true);
    // Change kCondNe to kCondEq for the special cases below.
    if (ccode == kCondNe) {
      ccode = kCondEq;
      std::swap(true_val, false_val);
    }
    bool cheap_false_val = InexpensiveConstantInt(false_val);
    if (cheap_false_val && ccode == kCondEq && (true_val == 0 || true_val == -1)) {
      OpRegRegImm(kOpSub, rl_result.reg, rl_src.reg, -true_val);
      DCHECK(last_lir_insn_->u.m.def_mask & ENCODE_CCODE);
      OpIT(true_val == 0 ? kCondNe : kCondUge, "");
      LoadConstant(rl_result.reg, false_val);
      GenBarrier();  // Add a scheduling barrier to keep the IT shadow intact
    } else if (cheap_false_val && ccode == kCondEq && true_val == 1) {
      OpRegRegImm(kOpRsub, rl_result.reg, rl_src.reg, 1);
      DCHECK(last_lir_insn_->u.m.def_mask & ENCODE_CCODE);
      OpIT(kCondLs, "");
      LoadConstant(rl_result.reg, false_val);
      GenBarrier();  // Add a scheduling barrier to keep the IT shadow intact
    } else if (cheap_false_val && InexpensiveConstantInt(true_val)) {
      OpRegImm(kOpCmp, rl_src.reg, 0);
      OpIT(ccode, "E");
      LoadConstant(rl_result.reg, true_val);
      LoadConstant(rl_result.reg, false_val);
      GenBarrier();  // Add a scheduling barrier to keep the IT shadow intact
    } else {
      // Unlikely case - could be tuned.
      RegStorage t_reg1 = AllocTemp();
      RegStorage t_reg2 = AllocTemp();
      LoadConstant(t_reg1, true_val);
      LoadConstant(t_reg2, false_val);
      OpRegImm(kOpCmp, rl_src.reg, 0);
      OpIT(ccode, "E");
      OpRegCopy(rl_result.reg, t_reg1);
      OpRegCopy(rl_result.reg, t_reg2);
      GenBarrier();  // Add a scheduling barrier to keep the IT shadow intact
    }
  } else {
    // MOVE case
    RegLocation rl_true = mir_graph_->reg_location_[mir->ssa_rep->uses[1]];
    RegLocation rl_false = mir_graph_->reg_location_[mir->ssa_rep->uses[2]];
    rl_true = LoadValue(rl_true, kCoreReg);
    rl_false = LoadValue(rl_false, kCoreReg);
    rl_result = EvalLoc(rl_dest, kCoreReg, true);
    OpRegImm(kOpCmp, rl_src.reg, 0);
    if (rl_result.reg.GetReg() == rl_true.reg.GetReg()) {  // Is the "true" case already in place?
      OpIT(NegateComparison(ccode), "");
      OpRegCopy(rl_result.reg, rl_false.reg);
    } else if (rl_result.reg.GetReg() == rl_false.reg.GetReg()) {  // False case in place?
      OpIT(ccode, "");
      OpRegCopy(rl_result.reg, rl_true.reg);
    } else {  // Normal - select between the two.
      OpIT(ccode, "E");
      OpRegCopy(rl_result.reg, rl_true.reg);
      OpRegCopy(rl_result.reg, rl_false.reg);
    }
    GenBarrier();  // Add a scheduling barrier to keep the IT shadow intact
  }
  StoreValue(rl_dest, rl_result);
}

void Arm64Mir2Lir::GenFusedLongCmpBranch(BasicBlock* bb, MIR* mir) {
  // TODO(Arm64): implement this.
  UNIMPLEMENTED(FATAL);

  RegLocation rl_src1 = mir_graph_->GetSrcWide(mir, 0);
  RegLocation rl_src2 = mir_graph_->GetSrcWide(mir, 2);
  // Normalize such that if either operand is constant, src2 will be constant.
  ConditionCode ccode = mir->meta.ccode;
  if (rl_src1.is_const) {
    std::swap(rl_src1, rl_src2);
    ccode = FlipComparisonOrder(ccode);
  }
  if (rl_src2.is_const) {
    RegLocation rl_temp = UpdateLocWide(rl_src2);
    // Do special compare/branch against simple const operand if not already in registers.
    int64_t val = mir_graph_->ConstantValueWide(rl_src2);
    if ((rl_temp.location != kLocPhysReg)
     /*&& ((ModifiedImmediate(Low32Bits(val)) >= 0) && (ModifiedImmediate(High32Bits(val)) >= 0))*/) {
      GenFusedLongCmpImmBranch(bb, rl_src1, val, ccode);
      return;
    }
  }
  LIR* taken = &block_label_list_[bb->taken];
  LIR* not_taken = &block_label_list_[bb->fall_through];
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  OpRegReg(kOpCmp, rl_src1.reg.GetHigh(), rl_src2.reg.GetHigh());
  switch (ccode) {
    case kCondEq:
      OpCondBranch(kCondNe, not_taken);
      break;
    case kCondNe:
      OpCondBranch(kCondNe, taken);
      break;
    case kCondLt:
      OpCondBranch(kCondLt, taken);
      OpCondBranch(kCondGt, not_taken);
      ccode = kCondUlt;
      break;
    case kCondLe:
      OpCondBranch(kCondLt, taken);
      OpCondBranch(kCondGt, not_taken);
      ccode = kCondLs;
      break;
    case kCondGt:
      OpCondBranch(kCondGt, taken);
      OpCondBranch(kCondLt, not_taken);
      ccode = kCondHi;
      break;
    case kCondGe:
      OpCondBranch(kCondGt, taken);
      OpCondBranch(kCondLt, not_taken);
      ccode = kCondUge;
      break;
    default:
      LOG(FATAL) << "Unexpected ccode: " << ccode;
  }
  OpRegReg(kOpCmp, rl_src1.reg.GetLow(), rl_src2.reg.GetLow());
  OpCondBranch(ccode, taken);
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
    branch = NewLIR2(opcode, reg.GetReg(), 0);
  } else {
    OpRegImm(kOpCmp, reg, check_value);
    branch = NewLIR2(kA64B2ct, arm_cond, 0);
  }
  branch->target = target;
  return branch;
}

LIR* Arm64Mir2Lir::OpRegCopyNoInsert(RegStorage r_dest, RegStorage r_src) {
  bool dest_is_fp = r_dest.IsFloat();
  bool src_is_fp = r_src.IsFloat();
  ArmOpcode opcode = kA64Brk1d;
  LIR* res;

  if (LIKELY(dest_is_fp == src_is_fp)) {
    if (LIKELY(!dest_is_fp)) {
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
        DCHECK(r_src.IsSingle());
        opcode = kA64Fmov2sw;
      }
    } else {
      if (r_src.IsDouble()) {
        opcode = kA64Fmov2xS;
      } else {
        DCHECK(r_dest.Is32Bit());
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
  uint32_t magic;
  uint32_t shift;
  DividePattern pattern;
};

static const MagicTable magic_table[] = {
  {0, 0, DivideNone},        // 0
  {0, 0, DivideNone},        // 1
  {0, 0, DivideNone},        // 2
  {0x55555556, 0, Divide3},  // 3
  {0, 0, DivideNone},        // 4
  {0x66666667, 1, Divide5},  // 5
  {0x2AAAAAAB, 0, Divide3},  // 6
  {0x92492493, 2, Divide7},  // 7
  {0, 0, DivideNone},        // 8
  {0x38E38E39, 1, Divide5},  // 9
  {0x66666667, 2, Divide5},  // 10
  {0x2E8BA2E9, 1, Divide5},  // 11
  {0x2AAAAAAB, 1, Divide5},  // 12
  {0x4EC4EC4F, 2, Divide5},  // 13
  {0x92492493, 3, Divide7},  // 14
  {0x88888889, 3, Divide7},  // 15
};

// Integer division by constant via reciprocal multiply (Hacker's Delight, 10-4)
bool Arm64Mir2Lir::SmallLiteralDivRem(Instruction::Code dalvik_opcode, bool is_div,
                                    RegLocation rl_src, RegLocation rl_dest, int lit) {
  // TODO(Arm64): fix this for Arm64. Note: may be worth revisiting the magic table.
  //   It should be possible subtracting one from all its entries, and using smaddl
  //   to counteract this. The advantage is that integers should then be easier to
  //   encode as logical immediates (0x55555555 rather than 0x55555556).
  UNIMPLEMENTED(FATAL);

  if ((lit < 0) || (lit >= static_cast<int>(sizeof(magic_table)/sizeof(magic_table[0])))) {
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
  LoadConstant(r_magic, magic_table[lit].magic);
  rl_src = LoadValue(rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  RegStorage r_hi = AllocTemp();
  RegStorage r_lo = AllocTemp();
  NewLIR4(kA64Smaddl4xwwx, r_lo.GetReg(), r_magic.GetReg(), rl_src.reg.GetReg(), rxzr);
  switch (pattern) {
    case Divide3:
      OpRegRegRegShift(kOpSub, rl_result.reg.GetReg(), r_hi.GetReg(),
               rl_src.reg.GetReg(), EncodeShift(kA64Asr, 31));
      break;
    case Divide5:
      OpRegRegImm(kOpAsr, r_lo, rl_src.reg, 31);
      OpRegRegRegShift(kOpRsub, rl_result.reg.GetReg(), r_lo.GetReg(), r_hi.GetReg(),
               EncodeShift(kA64Asr, magic_table[lit].shift));
      break;
    case Divide7:
      OpRegReg(kOpAdd, r_hi, rl_src.reg);
      OpRegRegImm(kOpAsr, r_lo, rl_src.reg, 31);
      OpRegRegRegShift(kOpRsub, rl_result.reg.GetReg(), r_lo.GetReg(), r_hi.GetReg(),
               EncodeShift(kA64Asr, magic_table[lit].shift));
      break;
    default:
      LOG(FATAL) << "Unexpected pattern: " << pattern;
  }
  StoreValue(rl_dest, rl_result);
  return true;
}

bool Arm64Mir2Lir::EasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit) {
  LOG(FATAL) << "Unexpected use of EasyMultiply for Arm64";
  return false;
}

RegLocation Arm64Mir2Lir::GenDivRem(RegLocation rl_dest, RegLocation rl_src1,
                      RegLocation rl_src2, bool is_div, bool check_zero) {
  LOG(FATAL) << "Unexpected use of GenDivRem for Arm64";
  return rl_dest;
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

RegLocation Arm64Mir2Lir::GenDivRem(RegLocation rl_dest, RegStorage reg1, RegStorage reg2,
                                  bool is_div) {
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (is_div) {
    // Simple case, use sdiv instruction.
    OpRegRegReg(kOpDiv, rl_result.reg, reg1, reg2);
  } else {
    // Remainder case, use the following code:
    // temp = reg1 / reg2      - integer division
    // temp = temp * reg2
    // dest = reg1 - temp

    RegStorage temp = AllocTemp();
    OpRegRegReg(kOpDiv, temp, reg1, reg2);
    OpRegReg(kOpMul, temp, reg2);
    OpRegRegReg(kOpSub, rl_result.reg, reg1, temp);
    FreeTemp(temp);
  }

  return rl_result;
}

bool Arm64Mir2Lir::GenInlinedMinMaxInt(CallInfo* info, bool is_min) {
  // TODO(Arm64): implement this.
  UNIMPLEMENTED(FATAL);

  DCHECK_EQ(cu_->instruction_set, kThumb2);
  RegLocation rl_src1 = info->args[0];
  RegLocation rl_src2 = info->args[1];
  rl_src1 = LoadValue(rl_src1, kCoreReg);
  rl_src2 = LoadValue(rl_src2, kCoreReg);
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  OpRegReg(kOpCmp, rl_src1.reg, rl_src2.reg);
  OpIT((is_min) ? kCondGt : kCondLt, "E");
  OpRegReg(kOpMov, rl_result.reg, rl_src2.reg);
  OpRegReg(kOpMov, rl_result.reg, rl_src1.reg);
  GenBarrier();
  StoreValue(rl_dest, rl_result);
  return true;
}

bool Arm64Mir2Lir::GenInlinedPeek(CallInfo* info, OpSize size) {
  // TODO(Arm64): implement this.
  UNIMPLEMENTED(WARNING);

  RegLocation rl_src_address = info->args[0];  // long address
  rl_src_address = NarrowRegLoc(rl_src_address);  // ignore high half in info->args[1]
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_address = LoadValue(rl_src_address, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (size == k64) {
    // Fake unaligned LDRD by two unaligned LDR instructions on ARMv7 with SCTLR.A set to 0.
    if (rl_address.reg.GetReg() != rl_result.reg.GetLowReg()) {
      LoadWordDisp(rl_address.reg, 0, rl_result.reg.GetLow());
      LoadWordDisp(rl_address.reg, 4, rl_result.reg.GetHigh());
    } else {
      LoadWordDisp(rl_address.reg, 4, rl_result.reg.GetHigh());
      LoadWordDisp(rl_address.reg, 0, rl_result.reg.GetLow());
    }
    StoreValueWide(rl_dest, rl_result);
  } else {
    DCHECK(size == kSignedByte || size == kSignedHalf || size == k32);
    // Unaligned load with LDR and LDRSH is allowed on ARMv7 with SCTLR.A set to 0.
    LoadBaseDisp(rl_address.reg, 0, rl_result.reg, size);
    StoreValue(rl_dest, rl_result);
  }
  return true;
}

bool Arm64Mir2Lir::GenInlinedPoke(CallInfo* info, OpSize size) {
  // TODO(Arm64): implement this.
  UNIMPLEMENTED(WARNING);

  RegLocation rl_src_address = info->args[0];  // long address
  rl_src_address = NarrowRegLoc(rl_src_address);  // ignore high half in info->args[1]
  RegLocation rl_src_value = info->args[2];  // [size] value
  RegLocation rl_address = LoadValue(rl_src_address, kCoreReg);
  if (size == k64) {
    // Fake unaligned STRD by two unaligned STR instructions on ARMv7 with SCTLR.A set to 0.
    RegLocation rl_value = LoadValueWide(rl_src_value, kCoreReg);
    StoreBaseDisp(rl_address.reg, 0, rl_value.reg.GetLow(), k32);
    StoreBaseDisp(rl_address.reg, 4, rl_value.reg.GetHigh(), k32);
  } else {
    DCHECK(size == kSignedByte || size == kSignedHalf || size == k32);
    // Unaligned store with STR and STRSH is allowed on ARMv7 with SCTLR.A set to 0.
    RegLocation rl_value = LoadValue(rl_src_value, kCoreReg);
    StoreBaseDisp(rl_address.reg, 0, rl_value.reg, size);
  }
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
  // TODO(Arm64): implement this.
  UNIMPLEMENTED(WARNING);

  DCHECK_EQ(cu_->instruction_set, kThumb2);
  // Unused - RegLocation rl_src_unsafe = info->args[0];
  RegLocation rl_src_obj = info->args[1];  // Object - known non-null
  RegLocation rl_src_offset = info->args[2];  // long low
  rl_src_offset = NarrowRegLoc(rl_src_offset);  // ignore high half in info->args[3]
  RegLocation rl_src_expected = info->args[4];  // int, long or Object
  // If is_long, high half is in info->args[5]
  RegLocation rl_src_new_value = info->args[is_long ? 6 : 5];  // int, long or Object
  // If is_long, high half is in info->args[7]
  RegLocation rl_dest = InlineTarget(info);  // boolean place for result

  // We have only 5 temporary registers available and actually only 4 if the InlineTarget
  // above locked one of the temps. For a straightforward CAS64 we need 7 registers:
  // r_ptr (1), new_value (2), expected(2) and ldrexd result (2). If neither expected nor
  // new_value is in a non-temp core register we shall reload them in the ldrex/strex loop
  // into the same temps, reducing the number of required temps down to 5. We shall work
  // around the potentially locked temp by using LR for r_ptr, unconditionally.
  // TODO: Pass information about the need for more temps to the stack frame generation
  // code so that we can rely on being able to allocate enough temps.
  DCHECK(!GetRegInfo(rs_rA64_LR)->IsTemp());
  MarkTemp(rs_rA64_LR);
  FreeTemp(rs_rA64_LR);
  LockTemp(rs_rA64_LR);
  bool load_early = true;
  if (is_long) {
    RegStorage expected_reg = rl_src_expected.reg.IsPair() ? rl_src_expected.reg.GetLow() :
        rl_src_expected.reg;
    RegStorage new_val_reg = rl_src_new_value.reg.IsPair() ? rl_src_new_value.reg.GetLow() :
        rl_src_new_value.reg;
    bool expected_is_core_reg = rl_src_expected.location == kLocPhysReg && !expected_reg.IsFloat();
    bool new_value_is_core_reg = rl_src_new_value.location == kLocPhysReg && !new_val_reg.IsFloat();
    bool expected_is_good_reg = expected_is_core_reg && !IsTemp(expected_reg);
    bool new_value_is_good_reg = new_value_is_core_reg && !IsTemp(new_val_reg);

    if (!expected_is_good_reg && !new_value_is_good_reg) {
      // None of expected/new_value is non-temp reg, need to load both late
      load_early = false;
      // Make sure they are not in the temp regs and the load will not be skipped.
      if (expected_is_core_reg) {
        FlushRegWide(rl_src_expected.reg);
        ClobberSReg(rl_src_expected.s_reg_low);
        ClobberSReg(GetSRegHi(rl_src_expected.s_reg_low));
        rl_src_expected.location = kLocDalvikFrame;
      }
      if (new_value_is_core_reg) {
        FlushRegWide(rl_src_new_value.reg);
        ClobberSReg(rl_src_new_value.s_reg_low);
        ClobberSReg(GetSRegHi(rl_src_new_value.s_reg_low));
        rl_src_new_value.location = kLocDalvikFrame;
      }
    }
  }

  // Release store semantics, get the barrier out of the way.  TODO: revisit
  GenMemBarrier(kStoreLoad);

  RegLocation rl_object = LoadValue(rl_src_obj, kCoreReg);
  RegLocation rl_new_value;
  if (!is_long) {
    rl_new_value = LoadValue(rl_src_new_value, kCoreReg);
  } else if (load_early) {
    rl_new_value = LoadValueWide(rl_src_new_value, kCoreReg);
  }

  if (is_object && !mir_graph_->IsConstantNullRef(rl_new_value)) {
    // Mark card for object assuming new value is stored.
    MarkGCCard(rl_new_value.reg, rl_object.reg);
  }

  RegLocation rl_offset = LoadValue(rl_src_offset, kCoreReg);

  RegStorage r_ptr = rs_rA64_LR;
  OpRegRegReg(kOpAdd, r_ptr, rl_object.reg, rl_offset.reg);

  // Free now unneeded rl_object and rl_offset to give more temps.
  ClobberSReg(rl_object.s_reg_low);
  FreeTemp(rl_object.reg);
  ClobberSReg(rl_offset.s_reg_low);
  FreeTemp(rl_offset.reg);

  RegLocation rl_expected;
  if (!is_long) {
    rl_expected = LoadValue(rl_src_expected, kCoreReg);
  } else if (load_early) {
    rl_expected = LoadValueWide(rl_src_expected, kCoreReg);
  } else {
    // NOTE: partially defined rl_expected & rl_new_value - but we just want the regs.
    int low_reg = AllocTemp().GetReg();
    int high_reg = AllocTemp().GetReg();
    rl_new_value.reg = RegStorage(RegStorage::k64BitPair, low_reg, high_reg);
    rl_expected = rl_new_value;
  }

  // do {
  //   tmp = [r_ptr] - expected;
  // } while (tmp == 0 && failure([r_ptr] <- r_new_value));
  // result = tmp != 0;

  RegStorage r_tmp = AllocTemp();
  LIR* target = NewLIR0(kPseudoTargetLabel);

  if (is_long) {
    RegStorage r_tmp_high = AllocTemp();
    if (!load_early) {
      LoadValueDirectWide(rl_src_expected, rl_expected.reg);
    }
    NewLIR3(kA64Ldxr2rX, r_tmp.GetReg(), r_tmp_high.GetReg(), r_ptr.GetReg());
    OpRegReg(kOpSub, r_tmp, rl_expected.reg.GetLow());
    OpRegReg(kOpSub, r_tmp_high, rl_expected.reg.GetHigh());
    if (!load_early) {
      LoadValueDirectWide(rl_src_new_value, rl_new_value.reg);
    }

    LIR* branch1 = OpCmpImmBranch(kCondNe, r_tmp, 0, NULL);
    LIR* branch2 = OpCmpImmBranch(kCondNe, r_tmp_high, 0, NULL);
    NewLIR4(WIDE(kA64Stxr3wrX) /* eq */, r_tmp.GetReg(), rl_new_value.reg.GetReg(),
            rl_new_value.reg.GetHighReg(), r_ptr.GetReg());
    LIR* target2 = NewLIR0(kPseudoTargetLabel);
    branch1->target = target2;
    branch2->target = target2;
    FreeTemp(r_tmp_high);  // Now unneeded

  } else {
    NewLIR3(kA64Ldxr2rX, r_tmp.GetReg(), r_ptr.GetReg(), 0);
    OpRegReg(kOpSub, r_tmp, rl_expected.reg);
    DCHECK(last_lir_insn_->u.m.def_mask & ENCODE_CCODE);
    OpIT(kCondEq, "T");
    NewLIR4(kA64Stxr3wrX /* eq */, r_tmp.GetReg(), rl_new_value.reg.GetReg(), r_ptr.GetReg(), 0);
  }

  // Still one conditional left from OpIT(kCondEq, "T") from either branch
  OpRegImm(kOpCmp /* eq */, r_tmp, 1);
  OpCondBranch(kCondEq, target);

  if (!load_early) {
    FreeTemp(rl_expected.reg);  // Now unneeded.
  }

  // result := (tmp1 != 0) ? 0 : 1;
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  OpRegRegImm(kOpRsub, rl_result.reg, r_tmp, 1);
  DCHECK(last_lir_insn_->u.m.def_mask & ENCODE_CCODE);
  OpIT(kCondUlt, "");
  LoadConstant(rl_result.reg, 0); /* cc */
  FreeTemp(r_tmp);  // Now unneeded.

  StoreValue(rl_dest, rl_result);

  // Now, restore lr to its non-temp status.
  Clobber(rs_rA64_LR);
  UnmarkTemp(rs_rA64_LR);
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
  OpRegRegRegShift(kOpAdd, rl_result.reg.GetReg(), rl_src.reg.GetReg(), rl_src.reg.GetReg(),
                   EncodeShift(kA64Lsl, second_bit - first_bit));
  if (first_bit != 0) {
    OpRegRegImm(kOpLsl, rl_result.reg, rl_result.reg, first_bit);
  }
}

void Arm64Mir2Lir::GenDivZeroCheckWide(RegStorage reg) {
  DCHECK(reg.IsPair());   // TODO: support k64BitSolo.
  OpRegImm64(kOpCmp, reg, 0, /*is_wide*/true);
  GenDivZeroCheck(kCondEq);
}

// TODO(Arm64): the function below should go.
// Test suspend flag, return target of taken suspend branch
LIR* Arm64Mir2Lir::OpTestSuspend(LIR* target) {
  NewLIR3(kA64Subs3rRd, rA64_SUSPEND, rA64_SUSPEND, 1);
  return OpCondBranch((target == NULL) ? kCondEq : kCondNe, target);
}

// Decrement register and branch on condition
LIR* Arm64Mir2Lir::OpDecAndBranch(ConditionCode c_code, RegStorage reg, LIR* target) {
  // Combine sub & test using sub setflags encoding here
  OpRegRegImm(kOpSub, reg, reg, 1);  // For value == 1, this should set flags.
  DCHECK(last_lir_insn_->u.m.def_mask & ENCODE_CCODE);
  return OpCondBranch(c_code, target);
}

bool Arm64Mir2Lir::GenMemBarrier(MemBarrierKind barrier_kind) {
#if ANDROID_SMP != 0
  // Start off with using the last LIR as the barrier. If it is not enough, then we will generate one.
  LIR* barrier = last_lir_insn_;

  int dmb_flavor;
  // TODO: revisit Arm barrier kinds
  switch (barrier_kind) {
    case kLoadStore: dmb_flavor = kISH; break;
    case kLoadLoad: dmb_flavor = kISH; break;
    case kStoreStore: dmb_flavor = kISHST; break;
    case kStoreLoad: dmb_flavor = kISH; break;
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
  barrier->u.m.def_mask = ENCODE_ALL;
  return ret;
#else
  return false;
#endif
}

void Arm64Mir2Lir::GenNegLong(RegLocation rl_dest, RegLocation rl_src) {
  rl_src = LoadValueWide(rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  RegStorage z_reg = AllocTemp();
  LoadConstantNoClobber(z_reg, 0);
  // Check for destructive overlap
  if (rl_result.reg.GetLowReg() == rl_src.reg.GetHighReg()) {
    RegStorage t_reg = AllocTemp();
    OpRegRegReg(kOpSub, rl_result.reg.GetLow(), z_reg, rl_src.reg.GetLow());
    OpRegRegReg(kOpSbc, rl_result.reg.GetHigh(), z_reg, t_reg);
    FreeTemp(t_reg);
  } else {
    OpRegRegReg(kOpSub, rl_result.reg.GetLow(), z_reg, rl_src.reg.GetLow());
    OpRegRegReg(kOpSbc, rl_result.reg.GetHigh(), z_reg, rl_src.reg.GetHigh());
  }
  FreeTemp(z_reg);
  StoreValueWide(rl_dest, rl_result);
}

void Arm64Mir2Lir::GenLongOp(OpKind op, RegLocation rl_dest, RegLocation rl_src1,
                             RegLocation rl_src2) {
  RegLocation rl_result;
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  OpRegRegRegShift(op, rl_result.reg.GetReg(), rl_src1.reg.GetReg(), rl_src2.reg.GetReg(),
                   ENCODE_NO_SHIFT, /*is_wide*/ true);
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
  // TODO(Arm64): check this.
  UNIMPLEMENTED(WARNING);

  RegisterClass reg_class = RegClassBySize(size);
  int len_offset = mirror::Array::LengthOffset().Int32Value();
  int data_offset;
  RegLocation rl_result;
  bool constant_index = rl_index.is_const;
  rl_array = LoadValue(rl_array, kCoreReg);
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
      reg_ptr = AllocTemp();
      OpRegRegRegShift(kOpAdd, reg_ptr.GetReg(), rl_array.reg.GetReg(), rl_index.reg.GetReg(),
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
    LoadBaseDisp(reg_ptr, data_offset, rl_result.reg, size);
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
    RegStorage reg_ptr = AllocTemp();
    OpRegRegImm(kOpAdd, reg_ptr, rl_array.reg, data_offset);
    FreeTemp(rl_array.reg);
    rl_result = EvalLoc(rl_dest, reg_class, true);

    if (needs_range_check) {
      GenArrayBoundsCheck(rl_index.reg, reg_len);
      FreeTemp(reg_len);
    }
    LoadBaseIndexed(reg_ptr, rl_index.reg, rl_result.reg, scale, size);
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
  // TODO(Arm64): check this.
  UNIMPLEMENTED(WARNING);

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

  rl_array = LoadValue(rl_array, kCoreReg);
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
    reg_ptr = AllocTemp();
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
      OpRegRegRegShift(kOpAdd, reg_ptr.GetReg(), rl_array.reg.GetReg(), rl_index.reg.GetReg(),
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

    StoreBaseDisp(reg_ptr, data_offset, rl_src.reg, size);
    MarkPossibleNullPointerException(opt_flags);
  } else {
    /* reg_ptr -> array data */
    OpRegRegImm(kOpAdd, reg_ptr, rl_array.reg, data_offset);
    rl_src = LoadValue(rl_src, reg_class);
    if (needs_range_check) {
      GenArrayBoundsCheck(rl_index.reg, reg_len);
      FreeTemp(reg_len);
    }
    StoreBaseIndexed(reg_ptr, rl_index.reg, rl_src.reg, scale, size);
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
  // TODO(Arm64): check this.
  UNIMPLEMENTED(WARNING);

  rl_src = LoadValueWide(rl_src, kCoreReg);
  // Per spec, we only care about low 6 bits of shift amount.
  int shift_amount = mir_graph_->ConstantValue(rl_shift) & 0x3f;
  if (shift_amount == 0) {
    StoreValueWide(rl_dest, rl_src);
    return;
  }
  if (BadOverlap(rl_src, rl_dest)) {
    GenShiftOpLong(opcode, rl_dest, rl_src, rl_shift);
    return;
  }
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  switch (opcode) {
    case Instruction::SHL_LONG:
    case Instruction::SHL_LONG_2ADDR:
      if (shift_amount == 1) {
        OpRegRegReg(kOpAdd, rl_result.reg.GetLow(), rl_src.reg.GetLow(), rl_src.reg.GetLow());
        OpRegRegReg(kOpAdc, rl_result.reg.GetHigh(), rl_src.reg.GetHigh(), rl_src.reg.GetHigh());
      } else if (shift_amount == 32) {
        OpRegCopy(rl_result.reg.GetHigh(), rl_src.reg);
        LoadConstant(rl_result.reg.GetLow(), 0);
      } else if (shift_amount > 31) {
        OpRegRegImm(kOpLsl, rl_result.reg.GetHigh(), rl_src.reg.GetLow(), shift_amount - 32);
        LoadConstant(rl_result.reg.GetLow(), 0);
      } else {
        OpRegRegImm(kOpLsl, rl_result.reg.GetHigh(), rl_src.reg.GetHigh(), shift_amount);
        OpRegRegRegShift(kOpOr, rl_result.reg.GetHighReg(), rl_result.reg.GetHighReg(), rl_src.reg.GetLowReg(),
                         EncodeShift(kA64Lsr, 32 - shift_amount));
        OpRegRegImm(kOpLsl, rl_result.reg.GetLow(), rl_src.reg.GetLow(), shift_amount);
      }
      break;
    case Instruction::SHR_LONG:
    case Instruction::SHR_LONG_2ADDR:
      if (shift_amount == 32) {
        OpRegCopy(rl_result.reg.GetLow(), rl_src.reg.GetHigh());
        OpRegRegImm(kOpAsr, rl_result.reg.GetHigh(), rl_src.reg.GetHigh(), 31);
      } else if (shift_amount > 31) {
        OpRegRegImm(kOpAsr, rl_result.reg.GetLow(), rl_src.reg.GetHigh(), shift_amount - 32);
        OpRegRegImm(kOpAsr, rl_result.reg.GetHigh(), rl_src.reg.GetHigh(), 31);
      } else {
        RegStorage t_reg = AllocTemp();
        OpRegRegImm(kOpLsr, t_reg, rl_src.reg.GetLow(), shift_amount);
        OpRegRegRegShift(kOpOr, rl_result.reg.GetLowReg(), t_reg.GetReg(), rl_src.reg.GetHighReg(),
                         EncodeShift(kA64Lsl, 32 - shift_amount));
        FreeTemp(t_reg);
        OpRegRegImm(kOpAsr, rl_result.reg.GetHigh(), rl_src.reg.GetHigh(), shift_amount);
      }
      break;
    case Instruction::USHR_LONG:
    case Instruction::USHR_LONG_2ADDR:
      if (shift_amount == 32) {
        OpRegCopy(rl_result.reg.GetLow(), rl_src.reg.GetHigh());
        LoadConstant(rl_result.reg.GetHigh(), 0);
      } else if (shift_amount > 31) {
        OpRegRegImm(kOpLsr, rl_result.reg.GetLow(), rl_src.reg.GetHigh(), shift_amount - 32);
        LoadConstant(rl_result.reg.GetHigh(), 0);
      } else {
        RegStorage t_reg = AllocTemp();
        OpRegRegImm(kOpLsr, t_reg, rl_src.reg.GetLow(), shift_amount);
        OpRegRegRegShift(kOpOr, rl_result.reg.GetLowReg(), t_reg.GetReg(), rl_src.reg.GetHighReg(),
                         EncodeShift(kA64Lsl, 32 - shift_amount));
        FreeTemp(t_reg);
        OpRegRegImm(kOpLsr, rl_result.reg.GetHigh(), rl_src.reg.GetHigh(), shift_amount);
      }
      break;
    default:
      LOG(FATAL) << "Unexpected case";
  }
  StoreValueWide(rl_dest, rl_result);
}

void Arm64Mir2Lir::GenArithImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                     RegLocation rl_src1, RegLocation rl_src2) {
  // TODO(Arm64): implement this.
  UNIMPLEMENTED(WARNING);

  if ((opcode == Instruction::SUB_LONG_2ADDR) || (opcode == Instruction::SUB_LONG)) {
    if (!rl_src2.is_const) {
      // Don't bother with special handling for subtract from immediate.
      GenArithOpLong(opcode, rl_dest, rl_src1, rl_src2);
      return;
    }
  } else {
    // Normalize
    if (!rl_src2.is_const) {
      DCHECK(rl_src1.is_const);
      std::swap(rl_src1, rl_src2);
    }
  }
  if (BadOverlap(rl_src1, rl_dest)) {
    GenArithOpLong(opcode, rl_dest, rl_src1, rl_src2);
    return;
  }
  DCHECK(rl_src2.is_const);
  // TODO(Arm64): implement this.
  //  int64_t val = mir_graph_->ConstantValueWide(rl_src2);
  int32_t mod_imm_lo = -1;  // ModifiedImmediate(val_lo);
  int32_t mod_imm_hi = -1;  // ModifiedImmediate(val_hi);

  // Only a subset of add/sub immediate instructions set carry - so bail if we don't fit
  switch (opcode) {
    case Instruction::ADD_LONG:
    case Instruction::ADD_LONG_2ADDR:
    case Instruction::SUB_LONG:
    case Instruction::SUB_LONG_2ADDR:
      if ((mod_imm_lo < 0) || (mod_imm_hi < 0)) {
        GenArithOpLong(opcode, rl_dest, rl_src1, rl_src2);
        return;
      }
      break;
    default:
      break;
  }
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  // NOTE: once we've done the EvalLoc on dest, we can no longer bail.
  switch (opcode) {
#if 0
    case Instruction::ADD_LONG:
    case Instruction::ADD_LONG_2ADDR:
      NewLIR3(kThumb2AddRRI8M, rl_result.reg.GetLowReg(), rl_src1.reg.GetLowReg(), mod_imm_lo);
      NewLIR3(kThumb2AdcRRI8M, rl_result.reg.GetHighReg(), rl_src1.reg.GetHighReg(), mod_imm_hi);
      break;
    case Instruction::OR_LONG:
    case Instruction::OR_LONG_2ADDR:
      if ((val_lo != 0) || (rl_result.reg.GetLowReg() != rl_src1.reg.GetLowReg())) {
        OpRegRegImm(kOpOr, rl_result.reg.GetLow(), rl_src1.reg.GetLow(), val_lo);
      }
      if ((val_hi != 0) || (rl_result.reg.GetHighReg() != rl_src1.reg.GetHighReg())) {
        OpRegRegImm(kOpOr, rl_result.reg.GetHigh(), rl_src1.reg.GetHigh(), val_hi);
      }
      break;
    case Instruction::XOR_LONG:
    case Instruction::XOR_LONG_2ADDR:
      OpRegRegImm(kOpXor, rl_result.reg.GetLow(), rl_src1.reg.GetLow(), val_lo);
      OpRegRegImm(kOpXor, rl_result.reg.GetHigh(), rl_src1.reg.GetHigh(), val_hi);
      break;
    case Instruction::AND_LONG:
    case Instruction::AND_LONG_2ADDR:
      if ((val_lo != 0xffffffff) || (rl_result.reg.GetLowReg() != rl_src1.reg.GetLowReg())) {
        OpRegRegImm(kOpAnd, rl_result.reg.GetLow(), rl_src1.reg.GetLow(), val_lo);
      }
      if ((val_hi != 0xffffffff) || (rl_result.reg.GetHighReg() != rl_src1.reg.GetHighReg())) {
        OpRegRegImm(kOpAnd, rl_result.reg.GetHigh(), rl_src1.reg.GetHigh(), val_hi);
      }
      break;
    case Instruction::SUB_LONG_2ADDR:
    case Instruction::SUB_LONG:
      NewLIR3(kThumb2SubRRI8M, rl_result.reg.GetLowReg(), rl_src1.reg.GetLowReg(), mod_imm_lo);
      NewLIR3(kThumb2SbcRRI8M, rl_result.reg.GetHighReg(), rl_src1.reg.GetHighReg(), mod_imm_hi);
      break;
#endif
    default:
      LOG(FATAL) << "Unexpected opcode " << opcode;
  }
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
  const int pop_log2_size = 3;

  for (offset = (offset >> pop_log2_size) - 1; reg_mask; offset--) {
     reg_mask = GenPairWise(reg_mask, & reg1, & reg2);
    if (UNLIKELY(reg2 < 0)) {
      // TODO(Arm64): replace Solo32 with Solo64, once rxN are defined properly.
      NewLIR3(WIDE(kA64Ldr3rXD), RegStorage::Solo32(reg1).GetReg(), base.GetReg(), offset);
    } else {
      // TODO(Arm64): replace Solo32 with Solo64 (twice below), once rxN are defined properly.
      NewLIR4(WIDE(kA64Ldp4rrXD), RegStorage::Solo32(reg1).GetReg(),
              RegStorage::Solo32(reg2).GetReg(), base.GetReg(), offset);
    }
  }
}

void Arm64Mir2Lir::SpillCoreRegs(RegStorage base, int offset, uint32_t reg_mask) {
  int reg1 = -1, reg2 = -1;
  const int pop_log2_size = 3;

  for (offset = (offset >> pop_log2_size) - 1; reg_mask; offset--) {
    reg_mask = GenPairWise(reg_mask, & reg1, & reg2);
    if (UNLIKELY(reg2 < 0)) {
      // TODO(Arm64): replace Solo32 with Solo64, once rxN are defined properly.
      NewLIR3(WIDE(kA64Str3rXD), RegStorage::Solo32(reg1).GetReg(), base.GetReg(), offset);
    } else {
      // TODO(Arm64): replace Solo32 with Solo64 (twice below), once rxN are defined properly.
      NewLIR4(WIDE(kA64Stp4rrXD), RegStorage::Solo32(reg1).GetReg(),
              RegStorage::Solo32(reg2).GetReg(), base.GetReg(), offset);
    }
  }
}

}  // namespace art
