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

/* This file contains codegen for the X86 ISA */

#include "codegen_x86.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "dex/reg_storage_eq.h"
#include "mirror/art_method.h"
#include "mirror/array.h"
#include "x86_lir.h"

namespace art {

/*
 * Compare two 64-bit values
 *    x = y     return  0
 *    x < y     return -1
 *    x > y     return  1
 */
void X86Mir2Lir::GenCmpLong(RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) {
  if (cu_->target64) {
    rl_src1 = LoadValueWide(rl_src1, kCoreReg);
    rl_src2 = LoadValueWide(rl_src2, kCoreReg);
    RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
    RegStorage temp_reg = AllocTemp();
    OpRegReg(kOpCmp, rl_src1.reg, rl_src2.reg);
    NewLIR2(kX86Set8R, rl_result.reg.GetReg(), kX86CondG);   // result = (src1 > src2) ? 1 : 0
    NewLIR2(kX86Set8R, temp_reg.GetReg(), kX86CondL);  // temp = (src1 >= src2) ? 0 : 1
    NewLIR2(kX86Sub8RR, rl_result.reg.GetReg(), temp_reg.GetReg());
    NewLIR2(kX86Movsx8qRR, rl_result.reg.GetReg(), rl_result.reg.GetReg());

    StoreValue(rl_dest, rl_result);
    FreeTemp(temp_reg);
    return;
  }

  FlushAllRegs();
  LockCallTemps();  // Prepare for explicit register usage
  RegStorage r_tmp1 = RegStorage::MakeRegPair(rs_r0, rs_r1);
  RegStorage r_tmp2 = RegStorage::MakeRegPair(rs_r2, rs_r3);
  LoadValueDirectWideFixed(rl_src1, r_tmp1);
  LoadValueDirectWideFixed(rl_src2, r_tmp2);
  // Compute (r1:r0) = (r1:r0) - (r3:r2)
  OpRegReg(kOpSub, rs_r0, rs_r2);  // r0 = r0 - r2
  OpRegReg(kOpSbc, rs_r1, rs_r3);  // r1 = r1 - r3 - CF
  NewLIR2(kX86Set8R, rs_r2.GetReg(), kX86CondL);  // r2 = (r1:r0) < (r3:r2) ? 1 : 0
  NewLIR2(kX86Movzx8RR, rs_r2.GetReg(), rs_r2.GetReg());
  OpReg(kOpNeg, rs_r2);         // r2 = -r2
  OpRegReg(kOpOr, rs_r0, rs_r1);   // r0 = high | low - sets ZF
  NewLIR2(kX86Set8R, rs_r0.GetReg(), kX86CondNz);  // r0 = (r1:r0) != (r3:r2) ? 1 : 0
  NewLIR2(kX86Movzx8RR, r0, r0);
  OpRegReg(kOpOr, rs_r0, rs_r2);   // r0 = r0 | r2
  RegLocation rl_result = LocCReturn();
  StoreValue(rl_dest, rl_result);
}

X86ConditionCode X86ConditionEncoding(ConditionCode cond) {
  switch (cond) {
    case kCondEq: return kX86CondEq;
    case kCondNe: return kX86CondNe;
    case kCondCs: return kX86CondC;
    case kCondCc: return kX86CondNc;
    case kCondUlt: return kX86CondC;
    case kCondUge: return kX86CondNc;
    case kCondMi: return kX86CondS;
    case kCondPl: return kX86CondNs;
    case kCondVs: return kX86CondO;
    case kCondVc: return kX86CondNo;
    case kCondHi: return kX86CondA;
    case kCondLs: return kX86CondBe;
    case kCondGe: return kX86CondGe;
    case kCondLt: return kX86CondL;
    case kCondGt: return kX86CondG;
    case kCondLe: return kX86CondLe;
    case kCondAl:
    case kCondNv: LOG(FATAL) << "Should not reach here";
  }
  return kX86CondO;
}

LIR* X86Mir2Lir::OpCmpBranch(ConditionCode cond, RegStorage src1, RegStorage src2, LIR* target) {
  NewLIR2(src1.Is64Bit() ? kX86Cmp64RR : kX86Cmp32RR, src1.GetReg(), src2.GetReg());
  X86ConditionCode cc = X86ConditionEncoding(cond);
  LIR* branch = NewLIR2(kX86Jcc8, 0 /* lir operand for Jcc offset */ ,
                        cc);
  branch->target = target;
  return branch;
}

LIR* X86Mir2Lir::OpCmpImmBranch(ConditionCode cond, RegStorage reg,
                                int check_value, LIR* target) {
  if ((check_value == 0) && (cond == kCondEq || cond == kCondNe)) {
    // TODO: when check_value == 0 and reg is rCX, use the jcxz/nz opcode
    NewLIR2(reg.Is64Bit() ? kX86Test64RR: kX86Test32RR, reg.GetReg(), reg.GetReg());
  } else {
    if (reg.Is64Bit()) {
      NewLIR2(IS_SIMM8(check_value) ? kX86Cmp64RI8 : kX86Cmp64RI, reg.GetReg(), check_value);
    } else {
      NewLIR2(IS_SIMM8(check_value) ? kX86Cmp32RI8 : kX86Cmp32RI, reg.GetReg(), check_value);
    }
  }
  X86ConditionCode cc = X86ConditionEncoding(cond);
  LIR* branch = NewLIR2(kX86Jcc8, 0 /* lir operand for Jcc offset */ , cc);
  branch->target = target;
  return branch;
}

LIR* X86Mir2Lir::OpRegCopyNoInsert(RegStorage r_dest, RegStorage r_src) {
  // If src or dest is a pair, we'll be using low reg.
  if (r_dest.IsPair()) {
    r_dest = r_dest.GetLow();
  }
  if (r_src.IsPair()) {
    r_src = r_src.GetLow();
  }
  if (r_dest.IsFloat() || r_src.IsFloat())
    return OpFpRegCopy(r_dest, r_src);
  LIR* res = RawLIR(current_dalvik_offset_, r_dest.Is64Bit() ? kX86Mov64RR : kX86Mov32RR,
                    r_dest.GetReg(), r_src.GetReg());
  if (!(cu_->disable_opt & (1 << kSafeOptimizations)) && r_dest == r_src) {
    res->flags.is_nop = true;
  }
  return res;
}

void X86Mir2Lir::OpRegCopy(RegStorage r_dest, RegStorage r_src) {
  if (r_dest != r_src) {
    LIR *res = OpRegCopyNoInsert(r_dest, r_src);
    AppendLIR(res);
  }
}

void X86Mir2Lir::OpRegCopyWide(RegStorage r_dest, RegStorage r_src) {
  if (r_dest != r_src) {
    bool dest_fp = r_dest.IsFloat();
    bool src_fp = r_src.IsFloat();
    if (dest_fp) {
      if (src_fp) {
        OpRegCopy(r_dest, r_src);
      } else {
        // TODO: Prevent this from happening in the code. The result is often
        // unused or could have been loaded more easily from memory.
        if (!r_src.IsPair()) {
          DCHECK(!r_dest.IsPair());
          NewLIR2(kX86MovqxrRR, r_dest.GetReg(), r_src.GetReg());
        } else {
          NewLIR2(kX86MovdxrRR, r_dest.GetReg(), r_src.GetLowReg());
          RegStorage r_tmp = AllocTempDouble();
          NewLIR2(kX86MovdxrRR, r_tmp.GetReg(), r_src.GetHighReg());
          NewLIR2(kX86PunpckldqRR, r_dest.GetReg(), r_tmp.GetReg());
          FreeTemp(r_tmp);
        }
      }
    } else {
      if (src_fp) {
        if (!r_dest.IsPair()) {
          DCHECK(!r_src.IsPair());
          NewLIR2(kX86MovqrxRR, r_dest.GetReg(), r_src.GetReg());
        } else {
          NewLIR2(kX86MovdrxRR, r_dest.GetLowReg(), r_src.GetReg());
          RegStorage temp_reg = AllocTempDouble();
          NewLIR2(kX86MovsdRR, temp_reg.GetReg(), r_src.GetReg());
          NewLIR2(kX86PsrlqRI, temp_reg.GetReg(), 32);
          NewLIR2(kX86MovdrxRR, r_dest.GetHighReg(), temp_reg.GetReg());
        }
      } else {
        DCHECK_EQ(r_dest.IsPair(), r_src.IsPair());
        if (!r_src.IsPair()) {
          // Just copy the register directly.
          OpRegCopy(r_dest, r_src);
        } else {
          // Handle overlap
          if (r_src.GetHighReg() == r_dest.GetLowReg() &&
              r_src.GetLowReg() == r_dest.GetHighReg()) {
            // Deal with cycles.
            RegStorage temp_reg = AllocTemp();
            OpRegCopy(temp_reg, r_dest.GetHigh());
            OpRegCopy(r_dest.GetHigh(), r_dest.GetLow());
            OpRegCopy(r_dest.GetLow(), temp_reg);
            FreeTemp(temp_reg);
          } else if (r_src.GetHighReg() == r_dest.GetLowReg()) {
            OpRegCopy(r_dest.GetHigh(), r_src.GetHigh());
            OpRegCopy(r_dest.GetLow(), r_src.GetLow());
          } else {
            OpRegCopy(r_dest.GetLow(), r_src.GetLow());
            OpRegCopy(r_dest.GetHigh(), r_src.GetHigh());
          }
        }
      }
    }
  }
}

// Set rs_dest to 0 or 1 depending on the comparison between left_op and right_op.
// rs_dest := (left_op <code> right_op) ? [true_val] : [!true_val]
//
// Implemented as:
//         true_val = true =>  rs_dest := 0;
//                             rs_dest := (left_op <code> right_op) ? 1 : rs_dest;
//         true_val = false => rs_dest := 0;
//                             rs_dest := (left_op <~code> right_op) ? 1 : rs_dest;
void X86Mir2Lir::GenSelectConst01(RegStorage left_op, RegStorage right_op, ConditionCode code,
                                  bool true_val, RegStorage rs_dest) {
  LoadConstant(rs_dest, 0);
  OpRegReg(kOpCmp, left_op, right_op);
  // Set the low byte of the result to 0 or 1 from the compare condition code.
  NewLIR2(kX86Set8R, rs_dest.GetReg(),
          X86ConditionEncoding(true_val ? code : FlipComparisonOrder(code)));
}

void X86Mir2Lir::GenSelectConst32(RegStorage left_op, RegStorage right_op, ConditionCode code,
                                  int32_t true_val, int32_t false_val, RegStorage rs_dest,
                                  int dest_reg_class) {
  if ((true_val == 0 && false_val == 1) || (true_val == 1 && false_val == 0)) {
    // Can we use Setcc?
    if (rs_dest.Is64Bit() || rs_dest.GetRegNum() < 4) {
      GenSelectConst01(left_op, right_op, code, true_val == 1, rs_dest);
      return;
    }
  }

  // TODO: Refactor the code below to make this more general.
  UNIMPLEMENTED(FATAL) << "General GenSelectConst32 not implemented for x86.";
}

void X86Mir2Lir::GenSelect(BasicBlock* bb, MIR* mir) {
  RegLocation rl_result;
  RegLocation rl_src = mir_graph_->GetSrc(mir, 0);
  RegLocation rl_dest = mir_graph_->GetDest(mir);
  // Avoid using float regs here.
  RegisterClass src_reg_class = rl_src.ref ? kRefReg : kCoreReg;
  RegisterClass result_reg_class = rl_dest.ref ? kRefReg : kCoreReg;
  rl_src = LoadValue(rl_src, src_reg_class);
  ConditionCode ccode = mir->meta.ccode;

  // The kMirOpSelect has two variants, one for constants and one for moves.
  const bool is_constant_case = (mir->ssa_rep->num_uses == 1);

  if (is_constant_case) {
    int true_val = mir->dalvikInsn.vB;
    int false_val = mir->dalvikInsn.vC;
    rl_result = EvalLoc(rl_dest, result_reg_class, true);

    /*
     * For ccode == kCondEq:
     *
     * 1) When the true case is zero and result_reg is not same as src_reg:
     *     xor result_reg, result_reg
     *     cmp $0, src_reg
     *     mov t1, $false_case
     *     cmovnz result_reg, t1
     * 2) When the false case is zero and result_reg is not same as src_reg:
     *     xor result_reg, result_reg
     *     cmp $0, src_reg
     *     mov t1, $true_case
     *     cmovz result_reg, t1
     * 3) All other cases (we do compare first to set eflags):
     *     cmp $0, src_reg
     *     mov result_reg, $false_case
     *     mov t1, $true_case
     *     cmovz result_reg, t1
     */
    // FIXME: depending on how you use registers you could get a false != mismatch when dealing
    // with different views of the same underlying physical resource (i.e. solo32 vs. solo64).
    const bool result_reg_same_as_src =
        (rl_src.location == kLocPhysReg && rl_src.reg.GetRegNum() == rl_result.reg.GetRegNum());
    const bool true_zero_case = (true_val == 0 && false_val != 0 && !result_reg_same_as_src);
    const bool false_zero_case = (false_val == 0 && true_val != 0 && !result_reg_same_as_src);
    const bool catch_all_case = !(true_zero_case || false_zero_case);

    if (true_zero_case || false_zero_case) {
      OpRegReg(kOpXor, rl_result.reg, rl_result.reg);
    }

    if (true_zero_case || false_zero_case || catch_all_case) {
      OpRegImm(kOpCmp, rl_src.reg, 0);
    }

    if (catch_all_case) {
      OpRegImm(kOpMov, rl_result.reg, false_val);
    }

    if (true_zero_case || false_zero_case || catch_all_case) {
      ConditionCode cc = true_zero_case ? NegateComparison(ccode) : ccode;
      int immediateForTemp = true_zero_case ? false_val : true_val;
      RegStorage temp1_reg = AllocTypedTemp(false, result_reg_class);
      OpRegImm(kOpMov, temp1_reg, immediateForTemp);

      OpCondRegReg(kOpCmov, cc, rl_result.reg, temp1_reg);

      FreeTemp(temp1_reg);
    }
  } else {
    RegLocation rl_true = mir_graph_->GetSrc(mir, 1);
    RegLocation rl_false = mir_graph_->GetSrc(mir, 2);
    rl_true = LoadValue(rl_true, result_reg_class);
    rl_false = LoadValue(rl_false, result_reg_class);
    rl_result = EvalLoc(rl_dest, result_reg_class, true);

    /*
     * For ccode == kCondEq:
     *
     * 1) When true case is already in place:
     *     cmp $0, src_reg
     *     cmovnz result_reg, false_reg
     * 2) When false case is already in place:
     *     cmp $0, src_reg
     *     cmovz result_reg, true_reg
     * 3) When neither cases are in place:
     *     cmp $0, src_reg
     *     mov result_reg, false_reg
     *     cmovz result_reg, true_reg
     */

    // kMirOpSelect is generated just for conditional cases when comparison is done with zero.
    OpRegImm(kOpCmp, rl_src.reg, 0);

    if (rl_result.reg.GetReg() == rl_true.reg.GetReg()) {
      OpCondRegReg(kOpCmov, NegateComparison(ccode), rl_result.reg, rl_false.reg);
    } else if (rl_result.reg.GetReg() == rl_false.reg.GetReg()) {
      OpCondRegReg(kOpCmov, ccode, rl_result.reg, rl_true.reg);
    } else {
      OpRegCopy(rl_result.reg, rl_false.reg);
      OpCondRegReg(kOpCmov, ccode, rl_result.reg, rl_true.reg);
    }
  }

  StoreValue(rl_dest, rl_result);
}

void X86Mir2Lir::GenFusedLongCmpBranch(BasicBlock* bb, MIR* mir) {
  LIR* taken = &block_label_list_[bb->taken];
  RegLocation rl_src1 = mir_graph_->GetSrcWide(mir, 0);
  RegLocation rl_src2 = mir_graph_->GetSrcWide(mir, 2);
  ConditionCode ccode = mir->meta.ccode;

  if (rl_src1.is_const) {
    std::swap(rl_src1, rl_src2);
    ccode = FlipComparisonOrder(ccode);
  }
  if (rl_src2.is_const) {
    // Do special compare/branch against simple const operand
    int64_t val = mir_graph_->ConstantValueWide(rl_src2);
    GenFusedLongCmpImmBranch(bb, rl_src1, val, ccode);
    return;
  }

  if (cu_->target64) {
    rl_src1 = LoadValueWide(rl_src1, kCoreReg);
    rl_src2 = LoadValueWide(rl_src2, kCoreReg);

    OpRegReg(kOpCmp, rl_src1.reg, rl_src2.reg);
    OpCondBranch(ccode, taken);
    return;
  }

  FlushAllRegs();
  LockCallTemps();  // Prepare for explicit register usage
  RegStorage r_tmp1 = RegStorage::MakeRegPair(rs_r0, rs_r1);
  RegStorage r_tmp2 = RegStorage::MakeRegPair(rs_r2, rs_r3);
  LoadValueDirectWideFixed(rl_src1, r_tmp1);
  LoadValueDirectWideFixed(rl_src2, r_tmp2);

  // Swap operands and condition code to prevent use of zero flag.
  if (ccode == kCondLe || ccode == kCondGt) {
    // Compute (r3:r2) = (r3:r2) - (r1:r0)
    OpRegReg(kOpSub, rs_r2, rs_r0);  // r2 = r2 - r0
    OpRegReg(kOpSbc, rs_r3, rs_r1);  // r3 = r3 - r1 - CF
  } else {
    // Compute (r1:r0) = (r1:r0) - (r3:r2)
    OpRegReg(kOpSub, rs_r0, rs_r2);  // r0 = r0 - r2
    OpRegReg(kOpSbc, rs_r1, rs_r3);  // r1 = r1 - r3 - CF
  }
  switch (ccode) {
    case kCondEq:
    case kCondNe:
      OpRegReg(kOpOr, rs_r0, rs_r1);  // r0 = r0 | r1
      break;
    case kCondLe:
      ccode = kCondGe;
      break;
    case kCondGt:
      ccode = kCondLt;
      break;
    case kCondLt:
    case kCondGe:
      break;
    default:
      LOG(FATAL) << "Unexpected ccode: " << ccode;
  }
  OpCondBranch(ccode, taken);
}

void X86Mir2Lir::GenFusedLongCmpImmBranch(BasicBlock* bb, RegLocation rl_src1,
                                          int64_t val, ConditionCode ccode) {
  int32_t val_lo = Low32Bits(val);
  int32_t val_hi = High32Bits(val);
  LIR* taken = &block_label_list_[bb->taken];
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  bool is_equality_test = ccode == kCondEq || ccode == kCondNe;

  if (cu_->target64) {
    if (is_equality_test && val == 0) {
      // We can simplify of comparing for ==, != to 0.
      NewLIR2(kX86Test64RR, rl_src1.reg.GetReg(), rl_src1.reg.GetReg());
    } else if (is_equality_test && val_hi == 0 && val_lo > 0) {
      OpRegImm(kOpCmp, rl_src1.reg, val_lo);
    } else {
      RegStorage tmp = AllocTypedTempWide(false, kCoreReg);
      LoadConstantWide(tmp, val);
      OpRegReg(kOpCmp, rl_src1.reg, tmp);
      FreeTemp(tmp);
    }
    OpCondBranch(ccode, taken);
    return;
  }

  if (is_equality_test && val != 0) {
    rl_src1 = ForceTempWide(rl_src1);
  }
  RegStorage low_reg = rl_src1.reg.GetLow();
  RegStorage high_reg = rl_src1.reg.GetHigh();

  if (is_equality_test) {
    // We can simplify of comparing for ==, != to 0.
    if (val == 0) {
      if (IsTemp(low_reg)) {
        OpRegReg(kOpOr, low_reg, high_reg);
        // We have now changed it; ignore the old values.
        Clobber(rl_src1.reg);
      } else {
        RegStorage t_reg = AllocTemp();
        OpRegRegReg(kOpOr, t_reg, low_reg, high_reg);
        FreeTemp(t_reg);
      }
      OpCondBranch(ccode, taken);
      return;
    }

    // Need to compute the actual value for ==, !=.
    OpRegImm(kOpSub, low_reg, val_lo);
    NewLIR2(kX86Sbb32RI, high_reg.GetReg(), val_hi);
    OpRegReg(kOpOr, high_reg, low_reg);
    Clobber(rl_src1.reg);
  } else if (ccode == kCondLe || ccode == kCondGt) {
    // Swap operands and condition code to prevent use of zero flag.
    RegStorage tmp = AllocTypedTempWide(false, kCoreReg);
    LoadConstantWide(tmp, val);
    OpRegReg(kOpSub, tmp.GetLow(), low_reg);
    OpRegReg(kOpSbc, tmp.GetHigh(), high_reg);
    ccode = (ccode == kCondLe) ? kCondGe : kCondLt;
    FreeTemp(tmp);
  } else {
    // We can use a compare for the low word to set CF.
    OpRegImm(kOpCmp, low_reg, val_lo);
    if (IsTemp(high_reg)) {
      NewLIR2(kX86Sbb32RI, high_reg.GetReg(), val_hi);
      // We have now changed it; ignore the old values.
      Clobber(rl_src1.reg);
    } else {
      // mov temp_reg, high_reg; sbb temp_reg, high_constant
      RegStorage t_reg = AllocTemp();
      OpRegCopy(t_reg, high_reg);
      NewLIR2(kX86Sbb32RI, t_reg.GetReg(), val_hi);
      FreeTemp(t_reg);
    }
  }

  OpCondBranch(ccode, taken);
}

void X86Mir2Lir::CalculateMagicAndShift(int divisor, int& magic, int& shift) {
  // It does not make sense to calculate magic and shift for zero divisor.
  DCHECK_NE(divisor, 0);

  /* According to H.S.Warren's Hacker's Delight Chapter 10 and
   * T,Grablund, P.L.Montogomery's Division by invariant integers using multiplication.
   * The magic number M and shift S can be calculated in the following way:
   * Let nc be the most positive value of numerator(n) such that nc = kd - 1,
   * where divisor(d) >=2.
   * Let nc be the most negative value of numerator(n) such that nc = kd + 1,
   * where divisor(d) <= -2.
   * Thus nc can be calculated like:
   * nc = 2^31 + 2^31 % d - 1, where d >= 2
   * nc = -2^31 + (2^31 + 1) % d, where d >= 2.
   *
   * So the shift p is the smallest p satisfying
   * 2^p > nc * (d - 2^p % d), where d >= 2
   * 2^p > nc * (d + 2^p % d), where d <= -2.
   *
   * the magic number M is calcuated by
   * M = (2^p + d - 2^p % d) / d, where d >= 2
   * M = (2^p - d - 2^p % d) / d, where d <= -2.
   *
   * Notice that p is always bigger than or equal to 32, so we just return 32-p as
   * the shift number S.
   */

  int32_t p = 31;
  const uint32_t two31 = 0x80000000U;

  // Initialize the computations.
  uint32_t abs_d = (divisor >= 0) ? divisor : -divisor;
  uint32_t tmp = two31 + (static_cast<uint32_t>(divisor) >> 31);
  uint32_t abs_nc = tmp - 1 - tmp % abs_d;
  uint32_t quotient1 = two31 / abs_nc;
  uint32_t remainder1 = two31 % abs_nc;
  uint32_t quotient2 = two31 / abs_d;
  uint32_t remainder2 = two31 % abs_d;

  /*
   * To avoid handling both positive and negative divisor, Hacker's Delight
   * introduces a method to handle these 2 cases together to avoid duplication.
   */
  uint32_t delta;
  do {
    p++;
    quotient1 = 2 * quotient1;
    remainder1 = 2 * remainder1;
    if (remainder1 >= abs_nc) {
      quotient1++;
      remainder1 = remainder1 - abs_nc;
    }
    quotient2 = 2 * quotient2;
    remainder2 = 2 * remainder2;
    if (remainder2 >= abs_d) {
      quotient2++;
      remainder2 = remainder2 - abs_d;
    }
    delta = abs_d - remainder2;
  } while (quotient1 < delta || (quotient1 == delta && remainder1 == 0));

  magic = (divisor > 0) ? (quotient2 + 1) : (-quotient2 - 1);
  shift = p - 32;
}

RegLocation X86Mir2Lir::GenDivRemLit(RegLocation rl_dest, RegStorage reg_lo, int lit, bool is_div) {
  LOG(FATAL) << "Unexpected use of GenDivRemLit for x86";
  return rl_dest;
}

RegLocation X86Mir2Lir::GenDivRemLit(RegLocation rl_dest, RegLocation rl_src,
                                     int imm, bool is_div) {
  // Use a multiply (and fixup) to perform an int div/rem by a constant.

  // We have to use fixed registers, so flush all the temps.
  FlushAllRegs();
  LockCallTemps();  // Prepare for explicit register usage.

  // Assume that the result will be in EDX.
  RegLocation rl_result = {kLocPhysReg, 0, 0, 0, 0, 0, 0, 0, 1, rs_r2, INVALID_SREG, INVALID_SREG};

  // handle div/rem by 1 special case.
  if (imm == 1) {
    if (is_div) {
      // x / 1 == x.
      StoreValue(rl_result, rl_src);
    } else {
      // x % 1 == 0.
      LoadConstantNoClobber(rs_r0, 0);
      // For this case, return the result in EAX.
      rl_result.reg.SetReg(r0);
    }
  } else if (imm == -1) {  // handle 0x80000000 / -1 special case.
    if (is_div) {
      LIR *minint_branch = 0;
      LoadValueDirectFixed(rl_src, rs_r0);
      OpRegImm(kOpCmp, rs_r0, 0x80000000);
      minint_branch = NewLIR2(kX86Jcc8, 0, kX86CondEq);

      // for x != MIN_INT, x / -1 == -x.
      NewLIR1(kX86Neg32R, r0);

      LIR* branch_around = NewLIR1(kX86Jmp8, 0);
      // The target for cmp/jmp above.
      minint_branch->target = NewLIR0(kPseudoTargetLabel);
      // EAX already contains the right value (0x80000000),
      branch_around->target = NewLIR0(kPseudoTargetLabel);
    } else {
      // x % -1 == 0.
      LoadConstantNoClobber(rs_r0, 0);
    }
    // For this case, return the result in EAX.
    rl_result.reg.SetReg(r0);
  } else {
    CHECK(imm <= -2 || imm >= 2);
    // Use H.S.Warren's Hacker's Delight Chapter 10 and
    // T,Grablund, P.L.Montogomery's Division by invariant integers using multiplication.
    int magic, shift;
    CalculateMagicAndShift(imm, magic, shift);

    /*
     * For imm >= 2,
     *     int(n/imm) = floor(n/imm) = floor(M*n/2^S), while n > 0
     *     int(n/imm) = ceil(n/imm) = floor(M*n/2^S) +1, while n < 0.
     * For imm <= -2,
     *     int(n/imm) = ceil(n/imm) = floor(M*n/2^S) +1 , while n > 0
     *     int(n/imm) = floor(n/imm) = floor(M*n/2^S), while n < 0.
     * We implement this algorithm in the following way:
     * 1. multiply magic number m and numerator n, get the higher 32bit result in EDX
     * 2. if imm > 0 and magic < 0, add numerator to EDX
     *    if imm < 0 and magic > 0, sub numerator from EDX
     * 3. if S !=0, SAR S bits for EDX
     * 4. add 1 to EDX if EDX < 0
     * 5. Thus, EDX is the quotient
     */

    // Numerator into EAX.
    RegStorage numerator_reg;
    if (!is_div || (imm > 0 && magic < 0) || (imm < 0 && magic > 0)) {
      // We will need the value later.
      if (rl_src.location == kLocPhysReg) {
        // We can use it directly.
        DCHECK(rl_src.reg.GetReg() != rs_r0.GetReg() && rl_src.reg.GetReg() != rs_r2.GetReg());
        numerator_reg = rl_src.reg;
      } else {
        numerator_reg = rs_r1;
        LoadValueDirectFixed(rl_src, numerator_reg);
      }
      OpRegCopy(rs_r0, numerator_reg);
    } else {
      // Only need this once.  Just put it into EAX.
      LoadValueDirectFixed(rl_src, rs_r0);
    }

    // EDX = magic.
    LoadConstantNoClobber(rs_r2, magic);

    // EDX:EAX = magic & dividend.
    NewLIR1(kX86Imul32DaR, rs_r2.GetReg());

    if (imm > 0 && magic < 0) {
      // Add numerator to EDX.
      DCHECK(numerator_reg.Valid());
      NewLIR2(kX86Add32RR, rs_r2.GetReg(), numerator_reg.GetReg());
    } else if (imm < 0 && magic > 0) {
      DCHECK(numerator_reg.Valid());
      NewLIR2(kX86Sub32RR, rs_r2.GetReg(), numerator_reg.GetReg());
    }

    // Do we need the shift?
    if (shift != 0) {
      // Shift EDX by 'shift' bits.
      NewLIR2(kX86Sar32RI, rs_r2.GetReg(), shift);
    }

    // Add 1 to EDX if EDX < 0.

    // Move EDX to EAX.
    OpRegCopy(rs_r0, rs_r2);

    // Move sign bit to bit 0, zeroing the rest.
    NewLIR2(kX86Shr32RI, rs_r2.GetReg(), 31);

    // EDX = EDX + EAX.
    NewLIR2(kX86Add32RR, rs_r2.GetReg(), rs_r0.GetReg());

    // Quotient is in EDX.
    if (!is_div) {
      // We need to compute the remainder.
      // Remainder is divisor - (quotient * imm).
      DCHECK(numerator_reg.Valid());
      OpRegCopy(rs_r0, numerator_reg);

      // EAX = numerator * imm.
      OpRegRegImm(kOpMul, rs_r2, rs_r2, imm);

      // EDX -= EAX.
      NewLIR2(kX86Sub32RR, rs_r0.GetReg(), rs_r2.GetReg());

      // For this case, return the result in EAX.
      rl_result.reg.SetReg(r0);
    }
  }

  return rl_result;
}

RegLocation X86Mir2Lir::GenDivRem(RegLocation rl_dest, RegStorage reg_lo, RegStorage reg_hi,
                                  bool is_div) {
  LOG(FATAL) << "Unexpected use of GenDivRem for x86";
  return rl_dest;
}

RegLocation X86Mir2Lir::GenDivRem(RegLocation rl_dest, RegLocation rl_src1,
                                  RegLocation rl_src2, bool is_div, bool check_zero) {
  // We have to use fixed registers, so flush all the temps.
  FlushAllRegs();
  LockCallTemps();  // Prepare for explicit register usage.

  // Load LHS into EAX.
  LoadValueDirectFixed(rl_src1, rs_r0);

  // Load RHS into EBX.
  LoadValueDirectFixed(rl_src2, rs_r1);

  // Copy LHS sign bit into EDX.
  NewLIR0(kx86Cdq32Da);

  if (check_zero) {
    // Handle division by zero case.
    GenDivZeroCheck(rs_r1);
  }

  // Have to catch 0x80000000/-1 case, or we will get an exception!
  OpRegImm(kOpCmp, rs_r1, -1);
  LIR *minus_one_branch = NewLIR2(kX86Jcc8, 0, kX86CondNe);

  // RHS is -1.
  OpRegImm(kOpCmp, rs_r0, 0x80000000);
  LIR * minint_branch = NewLIR2(kX86Jcc8, 0, kX86CondNe);

  // In 0x80000000/-1 case.
  if (!is_div) {
    // For DIV, EAX is already right. For REM, we need EDX 0.
    LoadConstantNoClobber(rs_r2, 0);
  }
  LIR* done = NewLIR1(kX86Jmp8, 0);

  // Expected case.
  minus_one_branch->target = NewLIR0(kPseudoTargetLabel);
  minint_branch->target = minus_one_branch->target;
  NewLIR1(kX86Idivmod32DaR, rs_r1.GetReg());
  done->target = NewLIR0(kPseudoTargetLabel);

  // Result is in EAX for div and EDX for rem.
  RegLocation rl_result = {kLocPhysReg, 0, 0, 0, 0, 0, 0, 0, 1, rs_r0, INVALID_SREG, INVALID_SREG};
  if (!is_div) {
    rl_result.reg.SetReg(r2);
  }
  return rl_result;
}

bool X86Mir2Lir::GenInlinedMinMax(CallInfo* info, bool is_min, bool is_long) {
  DCHECK(cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64);

  if (is_long && cu_->instruction_set == kX86) {
    return false;
  }

  // Get the two arguments to the invoke and place them in GP registers.
  RegLocation rl_src1 = info->args[0];
  RegLocation rl_src2 = (is_long) ? info->args[2] : info->args[1];
  rl_src1 = (is_long) ? LoadValueWide(rl_src1, kCoreReg) : LoadValue(rl_src1, kCoreReg);
  rl_src2 = (is_long) ? LoadValueWide(rl_src2, kCoreReg) : LoadValue(rl_src2, kCoreReg);

  RegLocation rl_dest = (is_long) ? InlineTargetWide(info) : InlineTarget(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);

  /*
   * If the result register is the same as the second element, then we need to be careful.
   * The reason is that the first copy will inadvertently clobber the second element with
   * the first one thus yielding the wrong result. Thus we do a swap in that case.
   */
  if (rl_result.reg.GetReg() == rl_src2.reg.GetReg()) {
    std::swap(rl_src1, rl_src2);
  }

  // Pick the first integer as min/max.
  OpRegCopy(rl_result.reg, rl_src1.reg);

  // If the integers are both in the same register, then there is nothing else to do
  // because they are equal and we have already moved one into the result.
  if (rl_src1.reg.GetReg() != rl_src2.reg.GetReg()) {
    // It is possible we didn't pick correctly so do the actual comparison now.
    OpRegReg(kOpCmp, rl_src1.reg, rl_src2.reg);

    // Conditionally move the other integer into the destination register.
    ConditionCode condition_code = is_min ? kCondGt : kCondLt;
    OpCondRegReg(kOpCmov, condition_code, rl_result.reg, rl_src2.reg);
  }

  if (is_long) {
    StoreValueWide(rl_dest, rl_result);
  } else {
    StoreValue(rl_dest, rl_result);
  }
  return true;
}

bool X86Mir2Lir::GenInlinedPeek(CallInfo* info, OpSize size) {
  RegLocation rl_src_address = info->args[0];  // long address
  RegLocation rl_address;
  if (!cu_->target64) {
    rl_src_address = NarrowRegLoc(rl_src_address);  // ignore high half in info->args[0]
    rl_address = LoadValue(rl_src_address, kCoreReg);
  } else {
    rl_address = LoadValueWide(rl_src_address, kCoreReg);
  }
  RegLocation rl_dest = size == k64 ? InlineTargetWide(info) : InlineTarget(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  // Unaligned access is allowed on x86.
  LoadBaseDisp(rl_address.reg, 0, rl_result.reg, size, kNotVolatile);
  if (size == k64) {
    StoreValueWide(rl_dest, rl_result);
  } else {
    DCHECK(size == kSignedByte || size == kSignedHalf || size == k32);
    StoreValue(rl_dest, rl_result);
  }
  return true;
}

bool X86Mir2Lir::GenInlinedPoke(CallInfo* info, OpSize size) {
  RegLocation rl_src_address = info->args[0];  // long address
  RegLocation rl_address;
  if (!cu_->target64) {
    rl_src_address = NarrowRegLoc(rl_src_address);  // ignore high half in info->args[0]
    rl_address = LoadValue(rl_src_address, kCoreReg);
  } else {
    rl_address = LoadValueWide(rl_src_address, kCoreReg);
  }
  RegLocation rl_src_value = info->args[2];  // [size] value
  RegLocation rl_value;
  if (size == k64) {
    // Unaligned access is allowed on x86.
    rl_value = LoadValueWide(rl_src_value, kCoreReg);
  } else {
    DCHECK(size == kSignedByte || size == kSignedHalf || size == k32);
    // In 32-bit mode the only EAX..EDX registers can be used with Mov8MR.
    if (!cu_->target64 && size == kSignedByte) {
      rl_src_value = UpdateLocTyped(rl_src_value, kCoreReg);
      if (rl_src_value.location == kLocPhysReg && !IsByteRegister(rl_src_value.reg)) {
        RegStorage temp = AllocateByteRegister();
        OpRegCopy(temp, rl_src_value.reg);
        rl_value.reg = temp;
      } else {
        rl_value = LoadValue(rl_src_value, kCoreReg);
      }
    } else {
      rl_value = LoadValue(rl_src_value, kCoreReg);
    }
  }
  StoreBaseDisp(rl_address.reg, 0, rl_value.reg, size, kNotVolatile);
  return true;
}

void X86Mir2Lir::OpLea(RegStorage r_base, RegStorage reg1, RegStorage reg2, int scale, int offset) {
  NewLIR5(kX86Lea32RA, r_base.GetReg(), reg1.GetReg(), reg2.GetReg(), scale, offset);
}

void X86Mir2Lir::OpTlsCmp(ThreadOffset<4> offset, int val) {
  DCHECK_EQ(kX86, cu_->instruction_set);
  NewLIR2(kX86Cmp16TI8, offset.Int32Value(), val);
}

void X86Mir2Lir::OpTlsCmp(ThreadOffset<8> offset, int val) {
  DCHECK_EQ(kX86_64, cu_->instruction_set);
  NewLIR2(kX86Cmp16TI8, offset.Int32Value(), val);
}

static bool IsInReg(X86Mir2Lir *pMir2Lir, const RegLocation &rl, RegStorage reg) {
  return rl.reg.Valid() && rl.reg.GetReg() == reg.GetReg() && (pMir2Lir->IsLive(reg) || rl.home);
}

bool X86Mir2Lir::GenInlinedCas(CallInfo* info, bool is_long, bool is_object) {
  DCHECK(cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64);
  // Unused - RegLocation rl_src_unsafe = info->args[0];
  RegLocation rl_src_obj = info->args[1];  // Object - known non-null
  RegLocation rl_src_offset = info->args[2];  // long low
  if (!cu_->target64) {
    rl_src_offset = NarrowRegLoc(rl_src_offset);  // ignore high half in info->args[3]
  }
  RegLocation rl_src_expected = info->args[4];  // int, long or Object
  // If is_long, high half is in info->args[5]
  RegLocation rl_src_new_value = info->args[is_long ? 6 : 5];  // int, long or Object
  // If is_long, high half is in info->args[7]

  if (is_long && cu_->target64) {
    // RAX must hold expected for CMPXCHG. Neither rl_new_value, nor r_ptr may be in RAX.
    FlushReg(rs_r0q);
    Clobber(rs_r0q);
    LockTemp(rs_r0q);

    RegLocation rl_object = LoadValue(rl_src_obj, kRefReg);
    RegLocation rl_new_value = LoadValueWide(rl_src_new_value, kCoreReg);
    RegLocation rl_offset = LoadValueWide(rl_src_offset, kCoreReg);
    LoadValueDirectWide(rl_src_expected, rs_r0q);
    NewLIR5(kX86LockCmpxchg64AR, rl_object.reg.GetReg(), rl_offset.reg.GetReg(), 0, 0,
            rl_new_value.reg.GetReg());

    // After a store we need to insert barrier in case of potential load. Since the
    // locked cmpxchg has full barrier semantics, only a scheduling barrier will be generated.
    GenMemBarrier(kAnyAny);

    FreeTemp(rs_r0q);
  } else if (is_long) {
    // TODO: avoid unnecessary loads of SI and DI when the values are in registers.
    // TODO: CFI support.
    FlushAllRegs();
    LockCallTemps();
    RegStorage r_tmp1 = RegStorage::MakeRegPair(rs_rAX, rs_rDX);
    RegStorage r_tmp2 = RegStorage::MakeRegPair(rs_rBX, rs_rCX);
    LoadValueDirectWideFixed(rl_src_expected, r_tmp1);
    LoadValueDirectWideFixed(rl_src_new_value, r_tmp2);
    // FIXME: needs 64-bit update.
    const bool obj_in_di = IsInReg(this, rl_src_obj, rs_rDI);
    const bool obj_in_si = IsInReg(this, rl_src_obj, rs_rSI);
    DCHECK(!obj_in_si || !obj_in_di);
    const bool off_in_di = IsInReg(this, rl_src_offset, rs_rDI);
    const bool off_in_si = IsInReg(this, rl_src_offset, rs_rSI);
    DCHECK(!off_in_si || !off_in_di);
    // If obj/offset is in a reg, use that reg. Otherwise, use the empty reg.
    RegStorage rs_obj = obj_in_di ? rs_rDI : obj_in_si ? rs_rSI : !off_in_di ? rs_rDI : rs_rSI;
    RegStorage rs_off = off_in_si ? rs_rSI : off_in_di ? rs_rDI : !obj_in_si ? rs_rSI : rs_rDI;
    bool push_di = (!obj_in_di && !off_in_di) && (rs_obj == rs_rDI || rs_off == rs_rDI);
    bool push_si = (!obj_in_si && !off_in_si) && (rs_obj == rs_rSI || rs_off == rs_rSI);
    if (push_di) {
      NewLIR1(kX86Push32R, rs_rDI.GetReg());
      MarkTemp(rs_rDI);
      LockTemp(rs_rDI);
    }
    if (push_si) {
      NewLIR1(kX86Push32R, rs_rSI.GetReg());
      MarkTemp(rs_rSI);
      LockTemp(rs_rSI);
    }
    ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
    const size_t push_offset = (push_si ? 4u : 0u) + (push_di ? 4u : 0u);
    if (!obj_in_si && !obj_in_di) {
      LoadWordDisp(rs_rX86_SP, SRegOffset(rl_src_obj.s_reg_low) + push_offset, rs_obj);
      // Dalvik register annotation in LoadBaseIndexedDisp() used wrong offset. Fix it.
      DCHECK(!DECODE_ALIAS_INFO_WIDE(last_lir_insn_->flags.alias_info));
      int reg_id = DECODE_ALIAS_INFO_REG(last_lir_insn_->flags.alias_info) - push_offset / 4u;
      AnnotateDalvikRegAccess(last_lir_insn_, reg_id, true, false);
    }
    if (!off_in_si && !off_in_di) {
      LoadWordDisp(rs_rX86_SP, SRegOffset(rl_src_offset.s_reg_low) + push_offset, rs_off);
      // Dalvik register annotation in LoadBaseIndexedDisp() used wrong offset. Fix it.
      DCHECK(!DECODE_ALIAS_INFO_WIDE(last_lir_insn_->flags.alias_info));
      int reg_id = DECODE_ALIAS_INFO_REG(last_lir_insn_->flags.alias_info) - push_offset / 4u;
      AnnotateDalvikRegAccess(last_lir_insn_, reg_id, true, false);
    }
    NewLIR4(kX86LockCmpxchg64A, rs_obj.GetReg(), rs_off.GetReg(), 0, 0);

    // After a store we need to insert barrier to prevent reordering with either
    // earlier or later memory accesses.  Since
    // locked cmpxchg has full barrier semantics, only a scheduling barrier will be generated,
    // and it will be associated with the cmpxchg instruction, preventing both.
    GenMemBarrier(kAnyAny);

    if (push_si) {
      FreeTemp(rs_rSI);
      UnmarkTemp(rs_rSI);
      NewLIR1(kX86Pop32R, rs_rSI.GetReg());
    }
    if (push_di) {
      FreeTemp(rs_rDI);
      UnmarkTemp(rs_rDI);
      NewLIR1(kX86Pop32R, rs_rDI.GetReg());
    }
    FreeCallTemps();
  } else {
    // EAX must hold expected for CMPXCHG. Neither rl_new_value, nor r_ptr may be in EAX.
    FlushReg(rs_r0);
    Clobber(rs_r0);
    LockTemp(rs_r0);

    RegLocation rl_object = LoadValue(rl_src_obj, kRefReg);
    RegLocation rl_new_value = LoadValue(rl_src_new_value);

    if (is_object && !mir_graph_->IsConstantNullRef(rl_new_value)) {
      // Mark card for object assuming new value is stored.
      FreeTemp(rs_r0);  // Temporarily release EAX for MarkGCCard().
      MarkGCCard(rl_new_value.reg, rl_object.reg);
      LockTemp(rs_r0);
    }

    RegLocation rl_offset;
    if (cu_->target64) {
      rl_offset = LoadValueWide(rl_src_offset, kCoreReg);
    } else {
      rl_offset = LoadValue(rl_src_offset, kCoreReg);
    }
    LoadValueDirect(rl_src_expected, rs_r0);
    NewLIR5(kX86LockCmpxchgAR, rl_object.reg.GetReg(), rl_offset.reg.GetReg(), 0, 0,
            rl_new_value.reg.GetReg());

    // After a store we need to insert barrier to prevent reordering with either
    // earlier or later memory accesses.  Since
    // locked cmpxchg has full barrier semantics, only a scheduling barrier will be generated,
    // and it will be associated with the cmpxchg instruction, preventing both.
    GenMemBarrier(kAnyAny);

    FreeTemp(rs_r0);
  }

  // Convert ZF to boolean
  RegLocation rl_dest = InlineTarget(info);  // boolean place for result
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  RegStorage result_reg = rl_result.reg;

  // For 32-bit, SETcc only works with EAX..EDX.
  if (!IsByteRegister(result_reg)) {
    result_reg = AllocateByteRegister();
  }
  NewLIR2(kX86Set8R, result_reg.GetReg(), kX86CondZ);
  NewLIR2(kX86Movzx8RR, rl_result.reg.GetReg(), result_reg.GetReg());
  if (IsTemp(result_reg)) {
    FreeTemp(result_reg);
  }
  StoreValue(rl_dest, rl_result);
  return true;
}

LIR* X86Mir2Lir::OpPcRelLoad(RegStorage reg, LIR* target) {
  CHECK(base_of_code_ != nullptr);

  // Address the start of the method
  RegLocation rl_method = mir_graph_->GetRegLocation(base_of_code_->s_reg_low);
  if (rl_method.wide) {
    LoadValueDirectWideFixed(rl_method, reg);
  } else {
    LoadValueDirectFixed(rl_method, reg);
  }
  store_method_addr_used_ = true;

  // Load the proper value from the literal area.
  // We don't know the proper offset for the value, so pick one that will force
  // 4 byte offset.  We will fix this up in the assembler later to have the right
  // value.
  ScopedMemRefType mem_ref_type(this, ResourceMask::kLiteral);
  LIR *res = RawLIR(current_dalvik_offset_, kX86Mov32RM, reg.GetReg(), reg.GetReg(), 256,
                    0, 0, target);
  res->target = target;
  res->flags.fixup = kFixupLoad;
  store_method_addr_used_ = true;
  return res;
}

LIR* X86Mir2Lir::OpVldm(RegStorage r_base, int count) {
  LOG(FATAL) << "Unexpected use of OpVldm for x86";
  return NULL;
}

LIR* X86Mir2Lir::OpVstm(RegStorage r_base, int count) {
  LOG(FATAL) << "Unexpected use of OpVstm for x86";
  return NULL;
}

void X86Mir2Lir::GenMultiplyByTwoBitMultiplier(RegLocation rl_src,
                                               RegLocation rl_result, int lit,
                                               int first_bit, int second_bit) {
  RegStorage t_reg = AllocTemp();
  OpRegRegImm(kOpLsl, t_reg, rl_src.reg, second_bit - first_bit);
  OpRegRegReg(kOpAdd, rl_result.reg, rl_src.reg, t_reg);
  FreeTemp(t_reg);
  if (first_bit != 0) {
    OpRegRegImm(kOpLsl, rl_result.reg, rl_result.reg, first_bit);
  }
}

void X86Mir2Lir::GenDivZeroCheckWide(RegStorage reg) {
  if (cu_->target64) {
    DCHECK(reg.Is64Bit());

    NewLIR2(kX86Cmp64RI8, reg.GetReg(), 0);
  } else {
    DCHECK(reg.IsPair());

    // We are not supposed to clobber the incoming storage, so allocate a temporary.
    RegStorage t_reg = AllocTemp();
    // Doing an OR is a quick way to check if both registers are zero. This will set the flags.
    OpRegRegReg(kOpOr, t_reg, reg.GetLow(), reg.GetHigh());
    // The temp is no longer needed so free it at this time.
    FreeTemp(t_reg);
  }

  // In case of zero, throw ArithmeticException.
  GenDivZeroCheck(kCondEq);
}

void X86Mir2Lir::GenArrayBoundsCheck(RegStorage index,
                                     RegStorage array_base,
                                     int len_offset) {
  class ArrayBoundsCheckSlowPath : public Mir2Lir::LIRSlowPath {
   public:
    ArrayBoundsCheckSlowPath(Mir2Lir* m2l, LIR* branch,
                             RegStorage index, RegStorage array_base, int32_t len_offset)
        : LIRSlowPath(m2l, m2l->GetCurrentDexPc(), branch),
          index_(index), array_base_(array_base), len_offset_(len_offset) {
    }

    void Compile() OVERRIDE {
      m2l_->ResetRegPool();
      m2l_->ResetDefTracking();
      GenerateTargetLabel(kPseudoThrowTarget);

      RegStorage new_index = index_;
      // Move index out of kArg1, either directly to kArg0, or to kArg2.
      // TODO: clean-up to check not a number but with type
      if (index_ == m2l_->TargetReg(kArg1, kNotWide)) {
        if (array_base_ == m2l_->TargetReg(kArg0, kRef)) {
          m2l_->OpRegCopy(m2l_->TargetReg(kArg2, kNotWide), index_);
          new_index = m2l_->TargetReg(kArg2, kNotWide);
        } else {
          m2l_->OpRegCopy(m2l_->TargetReg(kArg0, kNotWide), index_);
          new_index = m2l_->TargetReg(kArg0, kNotWide);
        }
      }
      // Load array length to kArg1.
      m2l_->OpRegMem(kOpMov, m2l_->TargetReg(kArg1, kNotWide), array_base_, len_offset_);
      if (cu_->target64) {
        m2l_->CallRuntimeHelperRegReg(QUICK_ENTRYPOINT_OFFSET(8, pThrowArrayBounds),
                                      new_index, m2l_->TargetReg(kArg1, kNotWide), true);
      } else {
        m2l_->CallRuntimeHelperRegReg(QUICK_ENTRYPOINT_OFFSET(4, pThrowArrayBounds),
                                      new_index, m2l_->TargetReg(kArg1, kNotWide), true);
      }
    }

   private:
    const RegStorage index_;
    const RegStorage array_base_;
    const int32_t len_offset_;
  };

  OpRegMem(kOpCmp, index, array_base, len_offset);
  MarkPossibleNullPointerException(0);
  LIR* branch = OpCondBranch(kCondUge, nullptr);
  AddSlowPath(new (arena_) ArrayBoundsCheckSlowPath(this, branch,
                                                    index, array_base, len_offset));
}

void X86Mir2Lir::GenArrayBoundsCheck(int32_t index,
                                     RegStorage array_base,
                                     int32_t len_offset) {
  class ArrayBoundsCheckSlowPath : public Mir2Lir::LIRSlowPath {
   public:
    ArrayBoundsCheckSlowPath(Mir2Lir* m2l, LIR* branch,
                             int32_t index, RegStorage array_base, int32_t len_offset)
        : LIRSlowPath(m2l, m2l->GetCurrentDexPc(), branch),
          index_(index), array_base_(array_base), len_offset_(len_offset) {
    }

    void Compile() OVERRIDE {
      m2l_->ResetRegPool();
      m2l_->ResetDefTracking();
      GenerateTargetLabel(kPseudoThrowTarget);

      // Load array length to kArg1.
      m2l_->OpRegMem(kOpMov, m2l_->TargetReg(kArg1, kNotWide), array_base_, len_offset_);
      m2l_->LoadConstant(m2l_->TargetReg(kArg0, kNotWide), index_);
      if (cu_->target64) {
        m2l_->CallRuntimeHelperRegReg(QUICK_ENTRYPOINT_OFFSET(8, pThrowArrayBounds),
                                      m2l_->TargetReg(kArg0, kNotWide),
                                      m2l_->TargetReg(kArg1, kNotWide), true);
      } else {
        m2l_->CallRuntimeHelperRegReg(QUICK_ENTRYPOINT_OFFSET(4, pThrowArrayBounds),
                                      m2l_->TargetReg(kArg0, kNotWide),
                                      m2l_->TargetReg(kArg1, kNotWide), true);
      }
    }

   private:
    const int32_t index_;
    const RegStorage array_base_;
    const int32_t len_offset_;
  };

  NewLIR3(IS_SIMM8(index) ? kX86Cmp32MI8 : kX86Cmp32MI, array_base.GetReg(), len_offset, index);
  MarkPossibleNullPointerException(0);
  LIR* branch = OpCondBranch(kCondLs, nullptr);
  AddSlowPath(new (arena_) ArrayBoundsCheckSlowPath(this, branch,
                                                    index, array_base, len_offset));
}

// Test suspend flag, return target of taken suspend branch
LIR* X86Mir2Lir::OpTestSuspend(LIR* target) {
  if (cu_->target64) {
    OpTlsCmp(Thread::ThreadFlagsOffset<8>(), 0);
  } else {
    OpTlsCmp(Thread::ThreadFlagsOffset<4>(), 0);
  }
  return OpCondBranch((target == NULL) ? kCondNe : kCondEq, target);
}

// Decrement register and branch on condition
LIR* X86Mir2Lir::OpDecAndBranch(ConditionCode c_code, RegStorage reg, LIR* target) {
  OpRegImm(kOpSub, reg, 1);
  return OpCondBranch(c_code, target);
}

bool X86Mir2Lir::SmallLiteralDivRem(Instruction::Code dalvik_opcode, bool is_div,
                                    RegLocation rl_src, RegLocation rl_dest, int lit) {
  LOG(FATAL) << "Unexpected use of smallLiteralDive in x86";
  return false;
}

bool X86Mir2Lir::EasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit) {
  LOG(FATAL) << "Unexpected use of easyMultiply in x86";
  return false;
}

LIR* X86Mir2Lir::OpIT(ConditionCode cond, const char* guide) {
  LOG(FATAL) << "Unexpected use of OpIT in x86";
  return NULL;
}

void X86Mir2Lir::OpEndIT(LIR* it) {
  LOG(FATAL) << "Unexpected use of OpEndIT in x86";
}

void X86Mir2Lir::GenImulRegImm(RegStorage dest, RegStorage src, int val) {
  switch (val) {
    case 0:
      NewLIR2(kX86Xor32RR, dest.GetReg(), dest.GetReg());
      break;
    case 1:
      OpRegCopy(dest, src);
      break;
    default:
      OpRegRegImm(kOpMul, dest, src, val);
      break;
  }
}

void X86Mir2Lir::GenImulMemImm(RegStorage dest, int sreg, int displacement, int val) {
  // All memory accesses below reference dalvik regs.
  ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);

  LIR *m;
  switch (val) {
    case 0:
      NewLIR2(kX86Xor32RR, dest.GetReg(), dest.GetReg());
      break;
    case 1:
      LoadBaseDisp(rs_rX86_SP, displacement, dest, k32, kNotVolatile);
      break;
    default:
      m = NewLIR4(IS_SIMM8(val) ? kX86Imul32RMI8 : kX86Imul32RMI, dest.GetReg(),
                  rs_rX86_SP.GetReg(), displacement, val);
      AnnotateDalvikRegAccess(m, displacement >> 2, true /* is_load */, true /* is_64bit */);
      break;
  }
}

void X86Mir2Lir::GenMulLong(Instruction::Code, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) {
  // All memory accesses below reference dalvik regs.
  ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);

  if (cu_->target64) {
    if (rl_src1.is_const) {
      std::swap(rl_src1, rl_src2);
    }
    // Are we multiplying by a constant?
    if (rl_src2.is_const) {
      int64_t val = mir_graph_->ConstantValueWide(rl_src2);
      if (val == 0) {
        RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
        OpRegReg(kOpXor, rl_result.reg, rl_result.reg);
        StoreValueWide(rl_dest, rl_result);
        return;
      } else if (val == 1) {
        StoreValueWide(rl_dest, rl_src1);
        return;
      } else if (val == 2) {
        GenAddLong(Instruction::ADD_LONG, rl_dest, rl_src1, rl_src1);
        return;
      } else if (IsPowerOfTwo(val)) {
        int shift_amount = LowestSetBit(val);
        if (!BadOverlap(rl_src1, rl_dest)) {
          rl_src1 = LoadValueWide(rl_src1, kCoreReg);
          RegLocation rl_result = GenShiftImmOpLong(Instruction::SHL_LONG, rl_dest,
                                                    rl_src1, shift_amount);
          StoreValueWide(rl_dest, rl_result);
          return;
        }
      }
    }
    rl_src1 = LoadValueWide(rl_src1, kCoreReg);
    rl_src2 = LoadValueWide(rl_src2, kCoreReg);
    RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
    if (rl_result.reg.GetReg() == rl_src1.reg.GetReg() &&
        rl_result.reg.GetReg() == rl_src2.reg.GetReg()) {
      NewLIR2(kX86Imul64RR, rl_result.reg.GetReg(), rl_result.reg.GetReg());
    } else if (rl_result.reg.GetReg() != rl_src1.reg.GetReg() &&
               rl_result.reg.GetReg() == rl_src2.reg.GetReg()) {
      NewLIR2(kX86Imul64RR, rl_result.reg.GetReg(), rl_src1.reg.GetReg());
    } else if (rl_result.reg.GetReg() == rl_src1.reg.GetReg() &&
               rl_result.reg.GetReg() != rl_src2.reg.GetReg()) {
      NewLIR2(kX86Imul64RR, rl_result.reg.GetReg(), rl_src2.reg.GetReg());
    } else {
      OpRegCopy(rl_result.reg, rl_src1.reg);
      NewLIR2(kX86Imul64RR, rl_result.reg.GetReg(), rl_src2.reg.GetReg());
    }
    StoreValueWide(rl_dest, rl_result);
    return;
  }

  if (rl_src1.is_const) {
    std::swap(rl_src1, rl_src2);
  }
  // Are we multiplying by a constant?
  if (rl_src2.is_const) {
    // Do special compare/branch against simple const operand
    int64_t val = mir_graph_->ConstantValueWide(rl_src2);
    if (val == 0) {
      RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
      OpRegReg(kOpXor, rl_result.reg.GetLow(), rl_result.reg.GetLow());
      OpRegReg(kOpXor, rl_result.reg.GetHigh(), rl_result.reg.GetHigh());
      StoreValueWide(rl_dest, rl_result);
      return;
    } else if (val == 1) {
      StoreValueWide(rl_dest, rl_src1);
      return;
    } else if (val == 2) {
      GenAddLong(Instruction::ADD_LONG, rl_dest, rl_src1, rl_src1);
      return;
    } else if (IsPowerOfTwo(val)) {
      int shift_amount = LowestSetBit(val);
      if (!BadOverlap(rl_src1, rl_dest)) {
        rl_src1 = LoadValueWide(rl_src1, kCoreReg);
        RegLocation rl_result = GenShiftImmOpLong(Instruction::SHL_LONG, rl_dest,
                                                  rl_src1, shift_amount);
        StoreValueWide(rl_dest, rl_result);
        return;
      }
    }

    // Okay, just bite the bullet and do it.
    int32_t val_lo = Low32Bits(val);
    int32_t val_hi = High32Bits(val);
    FlushAllRegs();
    LockCallTemps();  // Prepare for explicit register usage.
    rl_src1 = UpdateLocWideTyped(rl_src1, kCoreReg);
    bool src1_in_reg = rl_src1.location == kLocPhysReg;
    int displacement = SRegOffset(rl_src1.s_reg_low);

    // ECX <- 1H * 2L
    // EAX <- 1L * 2H
    if (src1_in_reg) {
      GenImulRegImm(rs_r1, rl_src1.reg.GetHigh(), val_lo);
      GenImulRegImm(rs_r0, rl_src1.reg.GetLow(), val_hi);
    } else {
      GenImulMemImm(rs_r1, GetSRegHi(rl_src1.s_reg_low), displacement + HIWORD_OFFSET, val_lo);
      GenImulMemImm(rs_r0, rl_src1.s_reg_low, displacement + LOWORD_OFFSET, val_hi);
    }

    // ECX <- ECX + EAX  (2H * 1L) + (1H * 2L)
    NewLIR2(kX86Add32RR, rs_r1.GetReg(), rs_r0.GetReg());

    // EAX <- 2L
    LoadConstantNoClobber(rs_r0, val_lo);

    // EDX:EAX <- 2L * 1L (double precision)
    if (src1_in_reg) {
      NewLIR1(kX86Mul32DaR, rl_src1.reg.GetLowReg());
    } else {
      LIR *m = NewLIR2(kX86Mul32DaM, rs_rX86_SP.GetReg(), displacement + LOWORD_OFFSET);
      AnnotateDalvikRegAccess(m, (displacement + LOWORD_OFFSET) >> 2,
                              true /* is_load */, true /* is_64bit */);
    }

    // EDX <- EDX + ECX (add high words)
    NewLIR2(kX86Add32RR, rs_r2.GetReg(), rs_r1.GetReg());

    // Result is EDX:EAX
    RegLocation rl_result = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1,
                             RegStorage::MakeRegPair(rs_r0, rs_r2), INVALID_SREG, INVALID_SREG};
    StoreValueWide(rl_dest, rl_result);
    return;
  }

  // Nope.  Do it the hard way
  // Check for V*V.  We can eliminate a multiply in that case, as 2L*1H == 2H*1L.
  bool is_square = mir_graph_->SRegToVReg(rl_src1.s_reg_low) ==
                   mir_graph_->SRegToVReg(rl_src2.s_reg_low);

  FlushAllRegs();
  LockCallTemps();  // Prepare for explicit register usage.
  rl_src1 = UpdateLocWideTyped(rl_src1, kCoreReg);
  rl_src2 = UpdateLocWideTyped(rl_src2, kCoreReg);

  // At this point, the VRs are in their home locations.
  bool src1_in_reg = rl_src1.location == kLocPhysReg;
  bool src2_in_reg = rl_src2.location == kLocPhysReg;

  // ECX <- 1H
  if (src1_in_reg) {
    NewLIR2(kX86Mov32RR, rs_r1.GetReg(), rl_src1.reg.GetHighReg());
  } else {
    LoadBaseDisp(rs_rX86_SP, SRegOffset(rl_src1.s_reg_low) + HIWORD_OFFSET, rs_r1, k32,
                 kNotVolatile);
  }

  if (is_square) {
    // Take advantage of the fact that the values are the same.
    // ECX <- ECX * 2L  (1H * 2L)
    if (src2_in_reg) {
      NewLIR2(kX86Imul32RR, rs_r1.GetReg(), rl_src2.reg.GetLowReg());
    } else {
      int displacement = SRegOffset(rl_src2.s_reg_low);
      LIR *m = NewLIR3(kX86Imul32RM, rs_r1.GetReg(), rs_rX86_SP.GetReg(),
                       displacement + LOWORD_OFFSET);
      AnnotateDalvikRegAccess(m, (displacement + LOWORD_OFFSET) >> 2,
                              true /* is_load */, true /* is_64bit */);
    }

    // ECX <- 2*ECX (2H * 1L) + (1H * 2L)
    NewLIR2(kX86Add32RR, rs_r1.GetReg(), rs_r1.GetReg());
  } else {
    // EAX <- 2H
    if (src2_in_reg) {
      NewLIR2(kX86Mov32RR, rs_r0.GetReg(), rl_src2.reg.GetHighReg());
    } else {
      LoadBaseDisp(rs_rX86_SP, SRegOffset(rl_src2.s_reg_low) + HIWORD_OFFSET, rs_r0, k32,
                   kNotVolatile);
    }

    // EAX <- EAX * 1L  (2H * 1L)
    if (src1_in_reg) {
      NewLIR2(kX86Imul32RR, rs_r0.GetReg(), rl_src1.reg.GetLowReg());
    } else {
      int displacement = SRegOffset(rl_src1.s_reg_low);
      LIR *m = NewLIR3(kX86Imul32RM, rs_r0.GetReg(), rs_rX86_SP.GetReg(),
                       displacement + LOWORD_OFFSET);
      AnnotateDalvikRegAccess(m, (displacement + LOWORD_OFFSET) >> 2,
                              true /* is_load */, true /* is_64bit */);
    }

    // ECX <- ECX * 2L  (1H * 2L)
    if (src2_in_reg) {
      NewLIR2(kX86Imul32RR, rs_r1.GetReg(), rl_src2.reg.GetLowReg());
    } else {
      int displacement = SRegOffset(rl_src2.s_reg_low);
      LIR *m = NewLIR3(kX86Imul32RM, rs_r1.GetReg(), rs_rX86_SP.GetReg(),
                       displacement + LOWORD_OFFSET);
      AnnotateDalvikRegAccess(m, (displacement + LOWORD_OFFSET) >> 2,
                              true /* is_load */, true /* is_64bit */);
    }

    // ECX <- ECX + EAX  (2H * 1L) + (1H * 2L)
    NewLIR2(kX86Add32RR, rs_r1.GetReg(), rs_r0.GetReg());
  }

  // EAX <- 2L
  if (src2_in_reg) {
    NewLIR2(kX86Mov32RR, rs_r0.GetReg(), rl_src2.reg.GetLowReg());
  } else {
    LoadBaseDisp(rs_rX86_SP, SRegOffset(rl_src2.s_reg_low) + LOWORD_OFFSET, rs_r0, k32,
                 kNotVolatile);
  }

  // EDX:EAX <- 2L * 1L (double precision)
  if (src1_in_reg) {
    NewLIR1(kX86Mul32DaR, rl_src1.reg.GetLowReg());
  } else {
    int displacement = SRegOffset(rl_src1.s_reg_low);
    LIR *m = NewLIR2(kX86Mul32DaM, rs_rX86_SP.GetReg(), displacement + LOWORD_OFFSET);
    AnnotateDalvikRegAccess(m, (displacement + LOWORD_OFFSET) >> 2,
                            true /* is_load */, true /* is_64bit */);
  }

  // EDX <- EDX + ECX (add high words)
  NewLIR2(kX86Add32RR, rs_r2.GetReg(), rs_r1.GetReg());

  // Result is EDX:EAX
  RegLocation rl_result = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1,
                           RegStorage::MakeRegPair(rs_r0, rs_r2), INVALID_SREG, INVALID_SREG};
  StoreValueWide(rl_dest, rl_result);
}

void X86Mir2Lir::GenLongRegOrMemOp(RegLocation rl_dest, RegLocation rl_src,
                                   Instruction::Code op) {
  DCHECK_EQ(rl_dest.location, kLocPhysReg);
  X86OpCode x86op = GetOpcode(op, rl_dest, rl_src, false);
  if (rl_src.location == kLocPhysReg) {
    // Both operands are in registers.
    // But we must ensure that rl_src is in pair
    if (cu_->target64) {
      NewLIR2(x86op, rl_dest.reg.GetReg(), rl_src.reg.GetReg());
    } else {
      rl_src = LoadValueWide(rl_src, kCoreReg);
      if (rl_dest.reg.GetLowReg() == rl_src.reg.GetHighReg()) {
        // The registers are the same, so we would clobber it before the use.
        RegStorage temp_reg = AllocTemp();
        OpRegCopy(temp_reg, rl_dest.reg);
        rl_src.reg.SetHighReg(temp_reg.GetReg());
      }
      NewLIR2(x86op, rl_dest.reg.GetLowReg(), rl_src.reg.GetLowReg());

      x86op = GetOpcode(op, rl_dest, rl_src, true);
      NewLIR2(x86op, rl_dest.reg.GetHighReg(), rl_src.reg.GetHighReg());
      FreeTemp(rl_src.reg);  // ???
    }
    return;
  }

  // RHS is in memory.
  DCHECK((rl_src.location == kLocDalvikFrame) ||
         (rl_src.location == kLocCompilerTemp));
  int r_base = rs_rX86_SP.GetReg();
  int displacement = SRegOffset(rl_src.s_reg_low);

  ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
  LIR *lir = NewLIR3(x86op, cu_->target64 ? rl_dest.reg.GetReg() : rl_dest.reg.GetLowReg(),
                     r_base, displacement + LOWORD_OFFSET);
  AnnotateDalvikRegAccess(lir, (displacement + LOWORD_OFFSET) >> 2,
                          true /* is_load */, true /* is64bit */);
  if (!cu_->target64) {
    x86op = GetOpcode(op, rl_dest, rl_src, true);
    lir = NewLIR3(x86op, rl_dest.reg.GetHighReg(), r_base, displacement + HIWORD_OFFSET);
    AnnotateDalvikRegAccess(lir, (displacement + HIWORD_OFFSET) >> 2,
                            true /* is_load */, true /* is64bit */);
  }
}

void X86Mir2Lir::GenLongArith(RegLocation rl_dest, RegLocation rl_src, Instruction::Code op) {
  rl_dest = UpdateLocWideTyped(rl_dest, kCoreReg);
  if (rl_dest.location == kLocPhysReg) {
    // Ensure we are in a register pair
    RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);

    rl_src = UpdateLocWideTyped(rl_src, kCoreReg);
    GenLongRegOrMemOp(rl_result, rl_src, op);
    StoreFinalValueWide(rl_dest, rl_result);
    return;
  }

  // It wasn't in registers, so it better be in memory.
  DCHECK((rl_dest.location == kLocDalvikFrame) ||
         (rl_dest.location == kLocCompilerTemp));
  rl_src = LoadValueWide(rl_src, kCoreReg);

  // Operate directly into memory.
  X86OpCode x86op = GetOpcode(op, rl_dest, rl_src, false);
  int r_base = rs_rX86_SP.GetReg();
  int displacement = SRegOffset(rl_dest.s_reg_low);

  ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
  LIR *lir = NewLIR3(x86op, r_base, displacement + LOWORD_OFFSET,
                     cu_->target64 ? rl_src.reg.GetReg() : rl_src.reg.GetLowReg());
  AnnotateDalvikRegAccess(lir, (displacement + LOWORD_OFFSET) >> 2,
                          true /* is_load */, true /* is64bit */);
  AnnotateDalvikRegAccess(lir, (displacement + LOWORD_OFFSET) >> 2,
                          false /* is_load */, true /* is64bit */);
  if (!cu_->target64) {
    x86op = GetOpcode(op, rl_dest, rl_src, true);
    lir = NewLIR3(x86op, r_base, displacement + HIWORD_OFFSET, rl_src.reg.GetHighReg());
    AnnotateDalvikRegAccess(lir, (displacement + HIWORD_OFFSET) >> 2,
                            true /* is_load */, true /* is64bit */);
    AnnotateDalvikRegAccess(lir, (displacement + HIWORD_OFFSET) >> 2,
                            false /* is_load */, true /* is64bit */);
  }
  FreeTemp(rl_src.reg);
}

void X86Mir2Lir::GenLongArith(RegLocation rl_dest, RegLocation rl_src1,
                              RegLocation rl_src2, Instruction::Code op,
                              bool is_commutative) {
  // Is this really a 2 operand operation?
  switch (op) {
    case Instruction::ADD_LONG_2ADDR:
    case Instruction::SUB_LONG_2ADDR:
    case Instruction::AND_LONG_2ADDR:
    case Instruction::OR_LONG_2ADDR:
    case Instruction::XOR_LONG_2ADDR:
      if (GenerateTwoOperandInstructions()) {
        GenLongArith(rl_dest, rl_src2, op);
        return;
      }
      break;

    default:
      break;
  }

  if (rl_dest.location == kLocPhysReg) {
    RegLocation rl_result = LoadValueWide(rl_src1, kCoreReg);

    // We are about to clobber the LHS, so it needs to be a temp.
    rl_result = ForceTempWide(rl_result);

    // Perform the operation using the RHS.
    rl_src2 = UpdateLocWideTyped(rl_src2, kCoreReg);
    GenLongRegOrMemOp(rl_result, rl_src2, op);

    // And now record that the result is in the temp.
    StoreFinalValueWide(rl_dest, rl_result);
    return;
  }

  // It wasn't in registers, so it better be in memory.
  DCHECK((rl_dest.location == kLocDalvikFrame) ||
         (rl_dest.location == kLocCompilerTemp));
  rl_src1 = UpdateLocWideTyped(rl_src1, kCoreReg);
  rl_src2 = UpdateLocWideTyped(rl_src2, kCoreReg);

  // Get one of the source operands into temporary register.
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  if (cu_->target64) {
    if (IsTemp(rl_src1.reg)) {
      GenLongRegOrMemOp(rl_src1, rl_src2, op);
    } else if (is_commutative) {
      rl_src2 = LoadValueWide(rl_src2, kCoreReg);
      // We need at least one of them to be a temporary.
      if (!IsTemp(rl_src2.reg)) {
        rl_src1 = ForceTempWide(rl_src1);
        GenLongRegOrMemOp(rl_src1, rl_src2, op);
      } else {
        GenLongRegOrMemOp(rl_src2, rl_src1, op);
        StoreFinalValueWide(rl_dest, rl_src2);
        return;
      }
    } else {
      // Need LHS to be the temp.
      rl_src1 = ForceTempWide(rl_src1);
      GenLongRegOrMemOp(rl_src1, rl_src2, op);
    }
  } else {
    if (IsTemp(rl_src1.reg.GetLow()) && IsTemp(rl_src1.reg.GetHigh())) {
      GenLongRegOrMemOp(rl_src1, rl_src2, op);
    } else if (is_commutative) {
      rl_src2 = LoadValueWide(rl_src2, kCoreReg);
      // We need at least one of them to be a temporary.
      if (!(IsTemp(rl_src2.reg.GetLow()) && IsTemp(rl_src2.reg.GetHigh()))) {
        rl_src1 = ForceTempWide(rl_src1);
        GenLongRegOrMemOp(rl_src1, rl_src2, op);
      } else {
        GenLongRegOrMemOp(rl_src2, rl_src1, op);
        StoreFinalValueWide(rl_dest, rl_src2);
        return;
      }
    } else {
      // Need LHS to be the temp.
      rl_src1 = ForceTempWide(rl_src1);
      GenLongRegOrMemOp(rl_src1, rl_src2, op);
    }
  }

  StoreFinalValueWide(rl_dest, rl_src1);
}

void X86Mir2Lir::GenAddLong(Instruction::Code opcode, RegLocation rl_dest,
                            RegLocation rl_src1, RegLocation rl_src2) {
  GenLongArith(rl_dest, rl_src1, rl_src2, opcode, true);
}

void X86Mir2Lir::GenSubLong(Instruction::Code opcode, RegLocation rl_dest,
                            RegLocation rl_src1, RegLocation rl_src2) {
  GenLongArith(rl_dest, rl_src1, rl_src2, opcode, false);
}

void X86Mir2Lir::GenAndLong(Instruction::Code opcode, RegLocation rl_dest,
                            RegLocation rl_src1, RegLocation rl_src2) {
  GenLongArith(rl_dest, rl_src1, rl_src2, opcode, true);
}

void X86Mir2Lir::GenOrLong(Instruction::Code opcode, RegLocation rl_dest,
                           RegLocation rl_src1, RegLocation rl_src2) {
  GenLongArith(rl_dest, rl_src1, rl_src2, opcode, true);
}

void X86Mir2Lir::GenXorLong(Instruction::Code opcode, RegLocation rl_dest,
                            RegLocation rl_src1, RegLocation rl_src2) {
  GenLongArith(rl_dest, rl_src1, rl_src2, opcode, true);
}

void X86Mir2Lir::GenNotLong(RegLocation rl_dest, RegLocation rl_src) {
  if (cu_->target64) {
    rl_src = LoadValueWide(rl_src, kCoreReg);
    RegLocation rl_result;
    rl_result = EvalLocWide(rl_dest, kCoreReg, true);
    OpRegCopy(rl_result.reg, rl_src.reg);
    OpReg(kOpNot, rl_result.reg);
    StoreValueWide(rl_dest, rl_result);
  } else {
    LOG(FATAL) << "Unexpected use GenNotLong()";
  }
}

void X86Mir2Lir::GenDivRemLong(Instruction::Code, RegLocation rl_dest, RegLocation rl_src1,
                           RegLocation rl_src2, bool is_div) {
  if (!cu_->target64) {
    LOG(FATAL) << "Unexpected use GenDivRemLong()";
    return;
  }

  // We have to use fixed registers, so flush all the temps.
  FlushAllRegs();
  LockCallTemps();  // Prepare for explicit register usage.

  // Load LHS into RAX.
  LoadValueDirectWideFixed(rl_src1, rs_r0q);

  // Load RHS into RCX.
  LoadValueDirectWideFixed(rl_src2, rs_r1q);

  // Copy LHS sign bit into RDX.
  NewLIR0(kx86Cqo64Da);

  // Handle division by zero case.
  GenDivZeroCheckWide(rs_r1q);

  // Have to catch 0x8000000000000000/-1 case, or we will get an exception!
  NewLIR2(kX86Cmp64RI8, rs_r1q.GetReg(), -1);
  LIR *minus_one_branch = NewLIR2(kX86Jcc8, 0, kX86CondNe);

  // RHS is -1.
  LoadConstantWide(rs_r6q, 0x8000000000000000);
  NewLIR2(kX86Cmp64RR, rs_r0q.GetReg(), rs_r6q.GetReg());
  LIR * minint_branch = NewLIR2(kX86Jcc8, 0, kX86CondNe);

  // In 0x8000000000000000/-1 case.
  if (!is_div) {
    // For DIV, RAX is already right. For REM, we need RDX 0.
    NewLIR2(kX86Xor64RR, rs_r2q.GetReg(), rs_r2q.GetReg());
  }
  LIR* done = NewLIR1(kX86Jmp8, 0);

  // Expected case.
  minus_one_branch->target = NewLIR0(kPseudoTargetLabel);
  minint_branch->target = minus_one_branch->target;
  NewLIR1(kX86Idivmod64DaR, rs_r1q.GetReg());
  done->target = NewLIR0(kPseudoTargetLabel);

  // Result is in RAX for div and RDX for rem.
  RegLocation rl_result = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, rs_r0q, INVALID_SREG, INVALID_SREG};
  if (!is_div) {
    rl_result.reg.SetReg(r2q);
  }

  StoreValueWide(rl_dest, rl_result);
}

void X86Mir2Lir::GenNegLong(RegLocation rl_dest, RegLocation rl_src) {
  rl_src = LoadValueWide(rl_src, kCoreReg);
  RegLocation rl_result;
  if (cu_->target64) {
    rl_result = EvalLocWide(rl_dest, kCoreReg, true);
    OpRegReg(kOpNeg, rl_result.reg, rl_src.reg);
  } else {
    rl_result = ForceTempWide(rl_src);
    if (((rl_dest.location == kLocPhysReg) && (rl_src.location == kLocPhysReg)) &&
        ((rl_dest.reg.GetLowReg() == rl_src.reg.GetHighReg()))) {
      // The registers are the same, so we would clobber it before the use.
      RegStorage temp_reg = AllocTemp();
      OpRegCopy(temp_reg, rl_result.reg);
      rl_result.reg.SetHighReg(temp_reg.GetReg());
    }
    OpRegReg(kOpNeg, rl_result.reg.GetLow(), rl_result.reg.GetLow());    // rLow = -rLow
    OpRegImm(kOpAdc, rl_result.reg.GetHigh(), 0);                   // rHigh = rHigh + CF
    OpRegReg(kOpNeg, rl_result.reg.GetHigh(), rl_result.reg.GetHigh());  // rHigh = -rHigh
  }
  StoreValueWide(rl_dest, rl_result);
}

void X86Mir2Lir::OpRegThreadMem(OpKind op, RegStorage r_dest, ThreadOffset<4> thread_offset) {
  DCHECK_EQ(kX86, cu_->instruction_set);
  X86OpCode opcode = kX86Bkpt;
  switch (op) {
  case kOpCmp: opcode = kX86Cmp32RT;  break;
  case kOpMov: opcode = kX86Mov32RT;  break;
  default:
    LOG(FATAL) << "Bad opcode: " << op;
    break;
  }
  NewLIR2(opcode, r_dest.GetReg(), thread_offset.Int32Value());
}

void X86Mir2Lir::OpRegThreadMem(OpKind op, RegStorage r_dest, ThreadOffset<8> thread_offset) {
  DCHECK_EQ(kX86_64, cu_->instruction_set);
  X86OpCode opcode = kX86Bkpt;
  if (cu_->target64 && r_dest.Is64BitSolo()) {
    switch (op) {
    case kOpCmp: opcode = kX86Cmp64RT;  break;
    case kOpMov: opcode = kX86Mov64RT;  break;
    default:
      LOG(FATAL) << "Bad opcode(OpRegThreadMem 64): " << op;
      break;
    }
  } else {
    switch (op) {
    case kOpCmp: opcode = kX86Cmp32RT;  break;
    case kOpMov: opcode = kX86Mov32RT;  break;
    default:
      LOG(FATAL) << "Bad opcode: " << op;
      break;
    }
  }
  NewLIR2(opcode, r_dest.GetReg(), thread_offset.Int32Value());
}

/*
 * Generate array load
 */
void X86Mir2Lir::GenArrayGet(int opt_flags, OpSize size, RegLocation rl_array,
                             RegLocation rl_index, RegLocation rl_dest, int scale) {
  RegisterClass reg_class = RegClassBySize(size);
  int len_offset = mirror::Array::LengthOffset().Int32Value();
  RegLocation rl_result;
  rl_array = LoadValue(rl_array, kRefReg);

  int data_offset;
  if (size == k64 || size == kDouble) {
    data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Int32Value();
  } else {
    data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Int32Value();
  }

  bool constant_index = rl_index.is_const;
  int32_t constant_index_value = 0;
  if (!constant_index) {
    rl_index = LoadValue(rl_index, kCoreReg);
  } else {
    constant_index_value = mir_graph_->ConstantValue(rl_index);
    // If index is constant, just fold it into the data offset
    data_offset += constant_index_value << scale;
    // treat as non array below
    rl_index.reg = RegStorage::InvalidReg();
  }

  /* null object? */
  GenNullCheck(rl_array.reg, opt_flags);

  if (!(opt_flags & MIR_IGNORE_RANGE_CHECK)) {
    if (constant_index) {
      GenArrayBoundsCheck(constant_index_value, rl_array.reg, len_offset);
    } else {
      GenArrayBoundsCheck(rl_index.reg, rl_array.reg, len_offset);
    }
  }
  rl_result = EvalLoc(rl_dest, reg_class, true);
  LoadBaseIndexedDisp(rl_array.reg, rl_index.reg, scale, data_offset, rl_result.reg, size);
  if ((size == k64) || (size == kDouble)) {
    StoreValueWide(rl_dest, rl_result);
  } else {
    StoreValue(rl_dest, rl_result);
  }
}

/*
 * Generate array store
 *
 */
void X86Mir2Lir::GenArrayPut(int opt_flags, OpSize size, RegLocation rl_array,
                             RegLocation rl_index, RegLocation rl_src, int scale, bool card_mark) {
  RegisterClass reg_class = RegClassBySize(size);
  int len_offset = mirror::Array::LengthOffset().Int32Value();
  int data_offset;

  if (size == k64 || size == kDouble) {
    data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Int32Value();
  } else {
    data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Int32Value();
  }

  rl_array = LoadValue(rl_array, kRefReg);
  bool constant_index = rl_index.is_const;
  int32_t constant_index_value = 0;
  if (!constant_index) {
    rl_index = LoadValue(rl_index, kCoreReg);
  } else {
    // If index is constant, just fold it into the data offset
    constant_index_value = mir_graph_->ConstantValue(rl_index);
    data_offset += constant_index_value << scale;
    // treat as non array below
    rl_index.reg = RegStorage::InvalidReg();
  }

  /* null object? */
  GenNullCheck(rl_array.reg, opt_flags);

  if (!(opt_flags & MIR_IGNORE_RANGE_CHECK)) {
    if (constant_index) {
      GenArrayBoundsCheck(constant_index_value, rl_array.reg, len_offset);
    } else {
      GenArrayBoundsCheck(rl_index.reg, rl_array.reg, len_offset);
    }
  }
  if ((size == k64) || (size == kDouble)) {
    rl_src = LoadValueWide(rl_src, reg_class);
  } else {
    rl_src = LoadValue(rl_src, reg_class);
  }
  // If the src reg can't be byte accessed, move it to a temp first.
  if ((size == kSignedByte || size == kUnsignedByte) && !IsByteRegister(rl_src.reg)) {
    RegStorage temp = AllocTemp();
    OpRegCopy(temp, rl_src.reg);
    StoreBaseIndexedDisp(rl_array.reg, rl_index.reg, scale, data_offset, temp, size);
  } else {
    StoreBaseIndexedDisp(rl_array.reg, rl_index.reg, scale, data_offset, rl_src.reg, size);
  }
  if (card_mark) {
    // Free rl_index if its a temp. Ensures there are 2 free regs for card mark.
    if (!constant_index) {
      FreeTemp(rl_index.reg);
    }
    MarkGCCard(rl_src.reg, rl_array.reg);
  }
}

RegLocation X86Mir2Lir::GenShiftImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                          RegLocation rl_src, int shift_amount) {
  RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  if (cu_->target64) {
    OpKind op = static_cast<OpKind>(0);    /* Make gcc happy */
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
  } else {
    switch (opcode) {
      case Instruction::SHL_LONG:
      case Instruction::SHL_LONG_2ADDR:
        DCHECK_NE(shift_amount, 1);  // Prevent a double store from happening.
        if (shift_amount == 32) {
          OpRegCopy(rl_result.reg.GetHigh(), rl_src.reg.GetLow());
          LoadConstant(rl_result.reg.GetLow(), 0);
        } else if (shift_amount > 31) {
          OpRegCopy(rl_result.reg.GetHigh(), rl_src.reg.GetLow());
          NewLIR2(kX86Sal32RI, rl_result.reg.GetHighReg(), shift_amount - 32);
          LoadConstant(rl_result.reg.GetLow(), 0);
        } else {
          OpRegCopy(rl_result.reg.GetLow(), rl_src.reg.GetLow());
          OpRegCopy(rl_result.reg.GetHigh(), rl_src.reg.GetHigh());
          NewLIR3(kX86Shld32RRI, rl_result.reg.GetHighReg(), rl_result.reg.GetLowReg(),
                  shift_amount);
          NewLIR2(kX86Sal32RI, rl_result.reg.GetLowReg(), shift_amount);
        }
        break;
      case Instruction::SHR_LONG:
      case Instruction::SHR_LONG_2ADDR:
        if (shift_amount == 32) {
          OpRegCopy(rl_result.reg.GetLow(), rl_src.reg.GetHigh());
          OpRegCopy(rl_result.reg.GetHigh(), rl_src.reg.GetHigh());
          NewLIR2(kX86Sar32RI, rl_result.reg.GetHighReg(), 31);
        } else if (shift_amount > 31) {
          OpRegCopy(rl_result.reg.GetLow(), rl_src.reg.GetHigh());
          OpRegCopy(rl_result.reg.GetHigh(), rl_src.reg.GetHigh());
          NewLIR2(kX86Sar32RI, rl_result.reg.GetLowReg(), shift_amount - 32);
          NewLIR2(kX86Sar32RI, rl_result.reg.GetHighReg(), 31);
        } else {
          OpRegCopy(rl_result.reg.GetLow(), rl_src.reg.GetLow());
          OpRegCopy(rl_result.reg.GetHigh(), rl_src.reg.GetHigh());
          NewLIR3(kX86Shrd32RRI, rl_result.reg.GetLowReg(), rl_result.reg.GetHighReg(),
                  shift_amount);
          NewLIR2(kX86Sar32RI, rl_result.reg.GetHighReg(), shift_amount);
        }
        break;
      case Instruction::USHR_LONG:
      case Instruction::USHR_LONG_2ADDR:
        if (shift_amount == 32) {
          OpRegCopy(rl_result.reg.GetLow(), rl_src.reg.GetHigh());
          LoadConstant(rl_result.reg.GetHigh(), 0);
        } else if (shift_amount > 31) {
          OpRegCopy(rl_result.reg.GetLow(), rl_src.reg.GetHigh());
          NewLIR2(kX86Shr32RI, rl_result.reg.GetLowReg(), shift_amount - 32);
          LoadConstant(rl_result.reg.GetHigh(), 0);
        } else {
          OpRegCopy(rl_result.reg.GetLow(), rl_src.reg.GetLow());
          OpRegCopy(rl_result.reg.GetHigh(), rl_src.reg.GetHigh());
          NewLIR3(kX86Shrd32RRI, rl_result.reg.GetLowReg(), rl_result.reg.GetHighReg(),
                  shift_amount);
          NewLIR2(kX86Shr32RI, rl_result.reg.GetHighReg(), shift_amount);
        }
        break;
      default:
        LOG(FATAL) << "Unexpected case";
    }
  }
  return rl_result;
}

void X86Mir2Lir::GenShiftImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                   RegLocation rl_src, RegLocation rl_shift) {
  // Per spec, we only care about low 6 bits of shift amount.
  int shift_amount = mir_graph_->ConstantValue(rl_shift) & 0x3f;
  if (shift_amount == 0) {
    rl_src = LoadValueWide(rl_src, kCoreReg);
    StoreValueWide(rl_dest, rl_src);
    return;
  } else if (shift_amount == 1 &&
            (opcode ==  Instruction::SHL_LONG || opcode == Instruction::SHL_LONG_2ADDR)) {
    // Need to handle this here to avoid calling StoreValueWide twice.
    GenAddLong(Instruction::ADD_LONG, rl_dest, rl_src, rl_src);
    return;
  }
  if (BadOverlap(rl_src, rl_dest)) {
    GenShiftOpLong(opcode, rl_dest, rl_src, rl_shift);
    return;
  }
  rl_src = LoadValueWide(rl_src, kCoreReg);
  RegLocation rl_result = GenShiftImmOpLong(opcode, rl_dest, rl_src, shift_amount);
  StoreValueWide(rl_dest, rl_result);
}

void X86Mir2Lir::GenArithImmOpLong(Instruction::Code opcode,
                                   RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) {
  bool isConstSuccess = false;
  switch (opcode) {
    case Instruction::ADD_LONG:
    case Instruction::AND_LONG:
    case Instruction::OR_LONG:
    case Instruction::XOR_LONG:
      if (rl_src2.is_const) {
        isConstSuccess = GenLongLongImm(rl_dest, rl_src1, rl_src2, opcode);
      } else {
        DCHECK(rl_src1.is_const);
        isConstSuccess = GenLongLongImm(rl_dest, rl_src2, rl_src1, opcode);
      }
      break;
    case Instruction::SUB_LONG:
    case Instruction::SUB_LONG_2ADDR:
      if (rl_src2.is_const) {
        isConstSuccess = GenLongLongImm(rl_dest, rl_src1, rl_src2, opcode);
      } else {
        GenSubLong(opcode, rl_dest, rl_src1, rl_src2);
        isConstSuccess = true;
      }
      break;
    case Instruction::ADD_LONG_2ADDR:
    case Instruction::OR_LONG_2ADDR:
    case Instruction::XOR_LONG_2ADDR:
    case Instruction::AND_LONG_2ADDR:
      if (rl_src2.is_const) {
        if (GenerateTwoOperandInstructions()) {
          isConstSuccess = GenLongImm(rl_dest, rl_src2, opcode);
        } else {
          isConstSuccess = GenLongLongImm(rl_dest, rl_src1, rl_src2, opcode);
        }
      } else {
        DCHECK(rl_src1.is_const);
        isConstSuccess = GenLongLongImm(rl_dest, rl_src2, rl_src1, opcode);
      }
      break;
    default:
      isConstSuccess = false;
      break;
  }

  if (!isConstSuccess) {
    // Default - bail to non-const handler.
    GenArithOpLong(opcode, rl_dest, rl_src1, rl_src2);
  }
}

bool X86Mir2Lir::IsNoOp(Instruction::Code op, int32_t value) {
  switch (op) {
    case Instruction::AND_LONG_2ADDR:
    case Instruction::AND_LONG:
      return value == -1;
    case Instruction::OR_LONG:
    case Instruction::OR_LONG_2ADDR:
    case Instruction::XOR_LONG:
    case Instruction::XOR_LONG_2ADDR:
      return value == 0;
    default:
      return false;
  }
}

X86OpCode X86Mir2Lir::GetOpcode(Instruction::Code op, RegLocation dest, RegLocation rhs,
                                bool is_high_op) {
  bool rhs_in_mem = rhs.location != kLocPhysReg;
  bool dest_in_mem = dest.location != kLocPhysReg;
  bool is64Bit = cu_->target64;
  DCHECK(!rhs_in_mem || !dest_in_mem);
  switch (op) {
    case Instruction::ADD_LONG:
    case Instruction::ADD_LONG_2ADDR:
      if (dest_in_mem) {
        return is64Bit ? kX86Add64MR : is_high_op ? kX86Adc32MR : kX86Add32MR;
      } else if (rhs_in_mem) {
        return is64Bit ? kX86Add64RM : is_high_op ? kX86Adc32RM : kX86Add32RM;
      }
      return is64Bit ? kX86Add64RR : is_high_op ? kX86Adc32RR : kX86Add32RR;
    case Instruction::SUB_LONG:
    case Instruction::SUB_LONG_2ADDR:
      if (dest_in_mem) {
        return is64Bit ? kX86Sub64MR : is_high_op ? kX86Sbb32MR : kX86Sub32MR;
      } else if (rhs_in_mem) {
        return is64Bit ? kX86Sub64RM : is_high_op ? kX86Sbb32RM : kX86Sub32RM;
      }
      return is64Bit ? kX86Sub64RR : is_high_op ? kX86Sbb32RR : kX86Sub32RR;
    case Instruction::AND_LONG_2ADDR:
    case Instruction::AND_LONG:
      if (dest_in_mem) {
        return is64Bit ? kX86And64MR : kX86And32MR;
      }
      if (is64Bit) {
        return rhs_in_mem ? kX86And64RM : kX86And64RR;
      }
      return rhs_in_mem ? kX86And32RM : kX86And32RR;
    case Instruction::OR_LONG:
    case Instruction::OR_LONG_2ADDR:
      if (dest_in_mem) {
        return is64Bit ? kX86Or64MR : kX86Or32MR;
      }
      if (is64Bit) {
        return rhs_in_mem ? kX86Or64RM : kX86Or64RR;
      }
      return rhs_in_mem ? kX86Or32RM : kX86Or32RR;
    case Instruction::XOR_LONG:
    case Instruction::XOR_LONG_2ADDR:
      if (dest_in_mem) {
        return is64Bit ? kX86Xor64MR : kX86Xor32MR;
      }
      if (is64Bit) {
        return rhs_in_mem ? kX86Xor64RM : kX86Xor64RR;
      }
      return rhs_in_mem ? kX86Xor32RM : kX86Xor32RR;
    default:
      LOG(FATAL) << "Unexpected opcode: " << op;
      return kX86Add32RR;
  }
}

X86OpCode X86Mir2Lir::GetOpcode(Instruction::Code op, RegLocation loc, bool is_high_op,
                                int32_t value) {
  bool in_mem = loc.location != kLocPhysReg;
  bool is64Bit = cu_->target64;
  bool byte_imm = IS_SIMM8(value);
  DCHECK(in_mem || !loc.reg.IsFloat());
  switch (op) {
    case Instruction::ADD_LONG:
    case Instruction::ADD_LONG_2ADDR:
      if (byte_imm) {
        if (in_mem) {
          return is64Bit ? kX86Add64MI8 : is_high_op ? kX86Adc32MI8 : kX86Add32MI8;
        }
        return is64Bit ? kX86Add64RI8 : is_high_op ? kX86Adc32RI8 : kX86Add32RI8;
      }
      if (in_mem) {
        return is64Bit ? kX86Add64MI : is_high_op ? kX86Adc32MI : kX86Add32MI;
      }
      return is64Bit ? kX86Add64RI : is_high_op ? kX86Adc32RI : kX86Add32RI;
    case Instruction::SUB_LONG:
    case Instruction::SUB_LONG_2ADDR:
      if (byte_imm) {
        if (in_mem) {
          return is64Bit ? kX86Sub64MI8 : is_high_op ? kX86Sbb32MI8 : kX86Sub32MI8;
        }
        return is64Bit ? kX86Sub64RI8 : is_high_op ? kX86Sbb32RI8 : kX86Sub32RI8;
      }
      if (in_mem) {
        return is64Bit ? kX86Sub64MI : is_high_op ? kX86Sbb32MI : kX86Sub32MI;
      }
      return is64Bit ? kX86Sub64RI : is_high_op ? kX86Sbb32RI : kX86Sub32RI;
    case Instruction::AND_LONG_2ADDR:
    case Instruction::AND_LONG:
      if (byte_imm) {
        if (is64Bit) {
          return in_mem ? kX86And64MI8 : kX86And64RI8;
        }
        return in_mem ? kX86And32MI8 : kX86And32RI8;
      }
      if (is64Bit) {
        return in_mem ? kX86And64MI : kX86And64RI;
      }
      return in_mem ? kX86And32MI : kX86And32RI;
    case Instruction::OR_LONG:
    case Instruction::OR_LONG_2ADDR:
      if (byte_imm) {
        if (is64Bit) {
          return in_mem ? kX86Or64MI8 : kX86Or64RI8;
        }
        return in_mem ? kX86Or32MI8 : kX86Or32RI8;
      }
      if (is64Bit) {
        return in_mem ? kX86Or64MI : kX86Or64RI;
      }
      return in_mem ? kX86Or32MI : kX86Or32RI;
    case Instruction::XOR_LONG:
    case Instruction::XOR_LONG_2ADDR:
      if (byte_imm) {
        if (is64Bit) {
          return in_mem ? kX86Xor64MI8 : kX86Xor64RI8;
        }
        return in_mem ? kX86Xor32MI8 : kX86Xor32RI8;
      }
      if (is64Bit) {
        return in_mem ? kX86Xor64MI : kX86Xor64RI;
      }
      return in_mem ? kX86Xor32MI : kX86Xor32RI;
    default:
      LOG(FATAL) << "Unexpected opcode: " << op;
      return kX86Add32MI;
  }
}

bool X86Mir2Lir::GenLongImm(RegLocation rl_dest, RegLocation rl_src, Instruction::Code op) {
  DCHECK(rl_src.is_const);
  int64_t val = mir_graph_->ConstantValueWide(rl_src);

  if (cu_->target64) {
    // We can do with imm only if it fits 32 bit
    if (val != (static_cast<int64_t>(static_cast<int32_t>(val)))) {
      return false;
    }

    rl_dest = UpdateLocWideTyped(rl_dest, kCoreReg);

    if ((rl_dest.location == kLocDalvikFrame) ||
        (rl_dest.location == kLocCompilerTemp)) {
      int r_base = rs_rX86_SP.GetReg();
      int displacement = SRegOffset(rl_dest.s_reg_low);

      ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
      X86OpCode x86op = GetOpcode(op, rl_dest, false, val);
      LIR *lir = NewLIR3(x86op, r_base, displacement + LOWORD_OFFSET, val);
      AnnotateDalvikRegAccess(lir, (displacement + LOWORD_OFFSET) >> 2,
                              true /* is_load */, true /* is64bit */);
      AnnotateDalvikRegAccess(lir, (displacement + LOWORD_OFFSET) >> 2,
                              false /* is_load */, true /* is64bit */);
      return true;
    }

    RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
    DCHECK_EQ(rl_result.location, kLocPhysReg);
    DCHECK(!rl_result.reg.IsFloat());

    X86OpCode x86op = GetOpcode(op, rl_result, false, val);
    NewLIR2(x86op, rl_result.reg.GetReg(), val);

    StoreValueWide(rl_dest, rl_result);
    return true;
  }

  int32_t val_lo = Low32Bits(val);
  int32_t val_hi = High32Bits(val);
  rl_dest = UpdateLocWideTyped(rl_dest, kCoreReg);

  // Can we just do this into memory?
  if ((rl_dest.location == kLocDalvikFrame) ||
      (rl_dest.location == kLocCompilerTemp)) {
    int r_base = rs_rX86_SP.GetReg();
    int displacement = SRegOffset(rl_dest.s_reg_low);

    ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
    if (!IsNoOp(op, val_lo)) {
      X86OpCode x86op = GetOpcode(op, rl_dest, false, val_lo);
      LIR *lir = NewLIR3(x86op, r_base, displacement + LOWORD_OFFSET, val_lo);
      AnnotateDalvikRegAccess(lir, (displacement + LOWORD_OFFSET) >> 2,
                              true /* is_load */, true /* is64bit */);
      AnnotateDalvikRegAccess(lir, (displacement + LOWORD_OFFSET) >> 2,
                              false /* is_load */, true /* is64bit */);
    }
    if (!IsNoOp(op, val_hi)) {
      X86OpCode x86op = GetOpcode(op, rl_dest, true, val_hi);
      LIR *lir = NewLIR3(x86op, r_base, displacement + HIWORD_OFFSET, val_hi);
      AnnotateDalvikRegAccess(lir, (displacement + HIWORD_OFFSET) >> 2,
                                true /* is_load */, true /* is64bit */);
      AnnotateDalvikRegAccess(lir, (displacement + HIWORD_OFFSET) >> 2,
                                false /* is_load */, true /* is64bit */);
    }
    return true;
  }

  RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  DCHECK_EQ(rl_result.location, kLocPhysReg);
  DCHECK(!rl_result.reg.IsFloat());

  if (!IsNoOp(op, val_lo)) {
    X86OpCode x86op = GetOpcode(op, rl_result, false, val_lo);
    NewLIR2(x86op, rl_result.reg.GetLowReg(), val_lo);
  }
  if (!IsNoOp(op, val_hi)) {
    X86OpCode x86op = GetOpcode(op, rl_result, true, val_hi);
    NewLIR2(x86op, rl_result.reg.GetHighReg(), val_hi);
  }
  StoreValueWide(rl_dest, rl_result);
  return true;
}

bool X86Mir2Lir::GenLongLongImm(RegLocation rl_dest, RegLocation rl_src1,
                                RegLocation rl_src2, Instruction::Code op) {
  DCHECK(rl_src2.is_const);
  int64_t val = mir_graph_->ConstantValueWide(rl_src2);

  if (cu_->target64) {
    // We can do with imm only if it fits 32 bit
    if (val != (static_cast<int64_t>(static_cast<int32_t>(val)))) {
      return false;
    }
    if (rl_dest.location == kLocPhysReg &&
        rl_src1.location == kLocPhysReg && !rl_dest.reg.IsFloat()) {
      X86OpCode x86op = GetOpcode(op, rl_dest, false, val);
      OpRegCopy(rl_dest.reg, rl_src1.reg);
      NewLIR2(x86op, rl_dest.reg.GetReg(), val);
      StoreFinalValueWide(rl_dest, rl_dest);
      return true;
    }

    rl_src1 = LoadValueWide(rl_src1, kCoreReg);
    // We need the values to be in a temporary
    RegLocation rl_result = ForceTempWide(rl_src1);

    X86OpCode x86op = GetOpcode(op, rl_result, false, val);
    NewLIR2(x86op, rl_result.reg.GetReg(), val);

    StoreFinalValueWide(rl_dest, rl_result);
    return true;
  }

  int32_t val_lo = Low32Bits(val);
  int32_t val_hi = High32Bits(val);
  rl_dest = UpdateLocWideTyped(rl_dest, kCoreReg);
  rl_src1 = UpdateLocWideTyped(rl_src1, kCoreReg);

  // Can we do this directly into the destination registers?
  if (rl_dest.location == kLocPhysReg && rl_src1.location == kLocPhysReg &&
      rl_dest.reg.GetLowReg() == rl_src1.reg.GetLowReg() &&
      rl_dest.reg.GetHighReg() == rl_src1.reg.GetHighReg() && !rl_dest.reg.IsFloat()) {
    if (!IsNoOp(op, val_lo)) {
      X86OpCode x86op = GetOpcode(op, rl_dest, false, val_lo);
      NewLIR2(x86op, rl_dest.reg.GetLowReg(), val_lo);
    }
    if (!IsNoOp(op, val_hi)) {
      X86OpCode x86op = GetOpcode(op, rl_dest, true, val_hi);
      NewLIR2(x86op, rl_dest.reg.GetHighReg(), val_hi);
    }

    StoreFinalValueWide(rl_dest, rl_dest);
    return true;
  }

  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  DCHECK_EQ(rl_src1.location, kLocPhysReg);

  // We need the values to be in a temporary
  RegLocation rl_result = ForceTempWide(rl_src1);
  if (!IsNoOp(op, val_lo)) {
    X86OpCode x86op = GetOpcode(op, rl_result, false, val_lo);
    NewLIR2(x86op, rl_result.reg.GetLowReg(), val_lo);
  }
  if (!IsNoOp(op, val_hi)) {
    X86OpCode x86op = GetOpcode(op, rl_result, true, val_hi);
    NewLIR2(x86op, rl_result.reg.GetHighReg(), val_hi);
  }

  StoreFinalValueWide(rl_dest, rl_result);
  return true;
}

// For final classes there are no sub-classes to check and so we can answer the instance-of
// question with simple comparisons. Use compares to memory and SETEQ to optimize for x86.
void X86Mir2Lir::GenInstanceofFinal(bool use_declaring_class, uint32_t type_idx,
                                    RegLocation rl_dest, RegLocation rl_src) {
  RegLocation object = LoadValue(rl_src, kRefReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  RegStorage result_reg = rl_result.reg;

  // For 32-bit, SETcc only works with EAX..EDX.
  RegStorage object_32reg = object.reg.Is64Bit() ? As32BitReg(object.reg) : object.reg;
  if (result_reg.GetRegNum() == object_32reg.GetRegNum() || !IsByteRegister(result_reg)) {
    result_reg = AllocateByteRegister();
  }

  // Assume that there is no match.
  LoadConstant(result_reg, 0);
  LIR* null_branchover = OpCmpImmBranch(kCondEq, object.reg, 0, NULL);

  // We will use this register to compare to memory below.
  // References are 32 bit in memory, and 64 bit in registers (in 64 bit mode).
  // For this reason, force allocation of a 32 bit register to use, so that the
  // compare to memory will be done using a 32 bit comparision.
  // The LoadRefDisp(s) below will work normally, even in 64 bit mode.
  RegStorage check_class = AllocTemp();

  // If Method* is already in a register, we can save a copy.
  RegLocation rl_method = mir_graph_->GetMethodLoc();
  int32_t offset_of_type = mirror::Array::DataOffset(
      sizeof(mirror::HeapReference<mirror::Class*>)).Int32Value() +
      (sizeof(mirror::HeapReference<mirror::Class*>) * type_idx);

  if (rl_method.location == kLocPhysReg) {
    if (use_declaring_class) {
      LoadRefDisp(rl_method.reg, mirror::ArtMethod::DeclaringClassOffset().Int32Value(),
                  check_class, kNotVolatile);
    } else {
      LoadRefDisp(rl_method.reg, mirror::ArtMethod::DexCacheResolvedTypesOffset().Int32Value(),
                  check_class, kNotVolatile);
      LoadRefDisp(check_class, offset_of_type, check_class, kNotVolatile);
    }
  } else {
    LoadCurrMethodDirect(check_class);
    if (use_declaring_class) {
      LoadRefDisp(check_class, mirror::ArtMethod::DeclaringClassOffset().Int32Value(),
                  check_class, kNotVolatile);
    } else {
      LoadRefDisp(check_class, mirror::ArtMethod::DexCacheResolvedTypesOffset().Int32Value(),
                  check_class, kNotVolatile);
      LoadRefDisp(check_class, offset_of_type, check_class, kNotVolatile);
    }
  }

  // Compare the computed class to the class in the object.
  DCHECK_EQ(object.location, kLocPhysReg);
  OpRegMem(kOpCmp, check_class, object.reg, mirror::Object::ClassOffset().Int32Value());

  // Set the low byte of the result to 0 or 1 from the compare condition code.
  NewLIR2(kX86Set8R, result_reg.GetReg(), kX86CondEq);

  LIR* target = NewLIR0(kPseudoTargetLabel);
  null_branchover->target = target;
  FreeTemp(check_class);
  if (IsTemp(result_reg)) {
    OpRegCopy(rl_result.reg, result_reg);
    FreeTemp(result_reg);
  }
  StoreValue(rl_dest, rl_result);
}

void X86Mir2Lir::GenInstanceofCallingHelper(bool needs_access_check, bool type_known_final,
                                            bool type_known_abstract, bool use_declaring_class,
                                            bool can_assume_type_is_in_dex_cache,
                                            uint32_t type_idx, RegLocation rl_dest,
                                            RegLocation rl_src) {
  FlushAllRegs();
  // May generate a call - use explicit registers.
  LockCallTemps();
  RegStorage method_reg = TargetReg(kArg1, kRef);  // kArg1 gets current Method*.
  LoadCurrMethodDirect(method_reg);
  RegStorage class_reg = TargetReg(kArg2, kRef);  // kArg2 will hold the Class*.
  RegStorage ref_reg = TargetReg(kArg0, kRef);  // kArg2 will hold the ref.
  // Reference must end up in kArg0.
  if (needs_access_check) {
    // Check we have access to type_idx and if not throw IllegalAccessError,
    // Caller function returns Class* in kArg0.
    if (cu_->target64) {
      CallRuntimeHelperImm(QUICK_ENTRYPOINT_OFFSET(8, pInitializeTypeAndVerifyAccess),
                           type_idx, true);
    } else {
      CallRuntimeHelperImm(QUICK_ENTRYPOINT_OFFSET(4, pInitializeTypeAndVerifyAccess),
                           type_idx, true);
    }
    OpRegCopy(class_reg, TargetReg(kRet0, kRef));
    LoadValueDirectFixed(rl_src, ref_reg);
  } else if (use_declaring_class) {
    LoadValueDirectFixed(rl_src, ref_reg);
    LoadRefDisp(method_reg, mirror::ArtMethod::DeclaringClassOffset().Int32Value(),
                class_reg, kNotVolatile);
  } else {
    // Load dex cache entry into class_reg (kArg2).
    LoadValueDirectFixed(rl_src, ref_reg);
    LoadRefDisp(method_reg, mirror::ArtMethod::DexCacheResolvedTypesOffset().Int32Value(),
                class_reg, kNotVolatile);
    int32_t offset_of_type =
        mirror::Array::DataOffset(sizeof(mirror::HeapReference<mirror::Class*>)).Int32Value() +
        (sizeof(mirror::HeapReference<mirror::Class*>) * type_idx);
    LoadRefDisp(class_reg, offset_of_type, class_reg, kNotVolatile);
    if (!can_assume_type_is_in_dex_cache) {
      // Need to test presence of type in dex cache at runtime.
      LIR* hop_branch = OpCmpImmBranch(kCondNe, class_reg, 0, NULL);
      // Type is not resolved. Call out to helper, which will return resolved type in kRet0/kArg0.
      if (cu_->target64) {
        CallRuntimeHelperImm(QUICK_ENTRYPOINT_OFFSET(8, pInitializeType), type_idx, true);
      } else {
        CallRuntimeHelperImm(QUICK_ENTRYPOINT_OFFSET(4, pInitializeType), type_idx, true);
      }
      OpRegCopy(class_reg, TargetReg(kRet0, kRef));  // Align usage with fast path.
      LoadValueDirectFixed(rl_src, ref_reg);  /* Reload Ref. */
      // Rejoin code paths
      LIR* hop_target = NewLIR0(kPseudoTargetLabel);
      hop_branch->target = hop_target;
    }
  }
  /* kArg0 is ref, kArg2 is class. If ref==null, use directly as bool result. */
  RegLocation rl_result = GetReturn(kRefReg);

  // On x86-64 kArg0 is not EAX, so we have to copy ref from kArg0 to EAX.
  if (cu_->target64) {
    OpRegCopy(rl_result.reg, ref_reg);
  }

  // Is the class NULL?
  LIR* branch1 = OpCmpImmBranch(kCondEq, ref_reg, 0, NULL);

  RegStorage ref_class_reg = TargetReg(kArg1, kRef);  // kArg2 will hold the Class*.
  /* Load object->klass_. */
  DCHECK_EQ(mirror::Object::ClassOffset().Int32Value(), 0);
  LoadRefDisp(ref_reg,  mirror::Object::ClassOffset().Int32Value(), ref_class_reg,
              kNotVolatile);
  /* kArg0 is ref, kArg1 is ref->klass_, kArg2 is class. */
  LIR* branchover = nullptr;
  if (type_known_final) {
    GenSelectConst32(ref_class_reg, class_reg, kCondEq, 1, 0, rl_result.reg, kCoreReg);
  } else {
    if (!type_known_abstract) {
      LoadConstant(rl_result.reg, 1);     // Assume result succeeds.
      branchover = OpCmpBranch(kCondEq, ref_class_reg, class_reg, NULL);
    }
    OpRegCopy(TargetReg(kArg0, kRef), class_reg);
    if (cu_->target64) {
      OpThreadMem(kOpBlx, QUICK_ENTRYPOINT_OFFSET(8, pInstanceofNonTrivial));
    } else {
      OpThreadMem(kOpBlx, QUICK_ENTRYPOINT_OFFSET(4, pInstanceofNonTrivial));
    }
  }
  // TODO: only clobber when type isn't final?
  ClobberCallerSave();
  /* Branch targets here. */
  LIR* target = NewLIR0(kPseudoTargetLabel);
  StoreValue(rl_dest, rl_result);
  branch1->target = target;
  if (branchover != nullptr) {
    branchover->target = target;
  }
}

void X86Mir2Lir::GenArithOpInt(Instruction::Code opcode, RegLocation rl_dest,
                            RegLocation rl_lhs, RegLocation rl_rhs) {
  OpKind op = kOpBkpt;
  bool is_div_rem = false;
  bool unary = false;
  bool shift_op = false;
  bool is_two_addr = false;
  RegLocation rl_result;
  switch (opcode) {
    case Instruction::NEG_INT:
      op = kOpNeg;
      unary = true;
      break;
    case Instruction::NOT_INT:
      op = kOpMvn;
      unary = true;
      break;
    case Instruction::ADD_INT_2ADDR:
      is_two_addr = true;
      // Fallthrough
    case Instruction::ADD_INT:
      op = kOpAdd;
      break;
    case Instruction::SUB_INT_2ADDR:
      is_two_addr = true;
      // Fallthrough
    case Instruction::SUB_INT:
      op = kOpSub;
      break;
    case Instruction::MUL_INT_2ADDR:
      is_two_addr = true;
      // Fallthrough
    case Instruction::MUL_INT:
      op = kOpMul;
      break;
    case Instruction::DIV_INT_2ADDR:
      is_two_addr = true;
      // Fallthrough
    case Instruction::DIV_INT:
      op = kOpDiv;
      is_div_rem = true;
      break;
    /* NOTE: returns in kArg1 */
    case Instruction::REM_INT_2ADDR:
      is_two_addr = true;
      // Fallthrough
    case Instruction::REM_INT:
      op = kOpRem;
      is_div_rem = true;
      break;
    case Instruction::AND_INT_2ADDR:
      is_two_addr = true;
      // Fallthrough
    case Instruction::AND_INT:
      op = kOpAnd;
      break;
    case Instruction::OR_INT_2ADDR:
      is_two_addr = true;
      // Fallthrough
    case Instruction::OR_INT:
      op = kOpOr;
      break;
    case Instruction::XOR_INT_2ADDR:
      is_two_addr = true;
      // Fallthrough
    case Instruction::XOR_INT:
      op = kOpXor;
      break;
    case Instruction::SHL_INT_2ADDR:
      is_two_addr = true;
      // Fallthrough
    case Instruction::SHL_INT:
      shift_op = true;
      op = kOpLsl;
      break;
    case Instruction::SHR_INT_2ADDR:
      is_two_addr = true;
      // Fallthrough
    case Instruction::SHR_INT:
      shift_op = true;
      op = kOpAsr;
      break;
    case Instruction::USHR_INT_2ADDR:
      is_two_addr = true;
      // Fallthrough
    case Instruction::USHR_INT:
      shift_op = true;
      op = kOpLsr;
      break;
    default:
      LOG(FATAL) << "Invalid word arith op: " << opcode;
  }

  // Can we convert to a two address instruction?
  if (!is_two_addr &&
        (mir_graph_->SRegToVReg(rl_dest.s_reg_low) ==
         mir_graph_->SRegToVReg(rl_lhs.s_reg_low))) {
    is_two_addr = true;
  }

  if (!GenerateTwoOperandInstructions()) {
    is_two_addr = false;
  }

  // Get the div/rem stuff out of the way.
  if (is_div_rem) {
    rl_result = GenDivRem(rl_dest, rl_lhs, rl_rhs, op == kOpDiv, true);
    StoreValue(rl_dest, rl_result);
    return;
  }

  // If we generate any memory access below, it will reference a dalvik reg.
  ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);

  if (unary) {
    rl_lhs = LoadValue(rl_lhs, kCoreReg);
    rl_result = UpdateLocTyped(rl_dest, kCoreReg);
    rl_result = EvalLoc(rl_dest, kCoreReg, true);
    OpRegReg(op, rl_result.reg, rl_lhs.reg);
  } else {
    if (shift_op) {
      // X86 doesn't require masking and must use ECX.
      RegStorage t_reg = TargetReg(kCount, kNotWide);  // rCX
      LoadValueDirectFixed(rl_rhs, t_reg);
      if (is_two_addr) {
        // Can we do this directly into memory?
        rl_result = UpdateLocTyped(rl_dest, kCoreReg);
        rl_rhs = LoadValue(rl_rhs, kCoreReg);
        if (rl_result.location != kLocPhysReg) {
          // Okay, we can do this into memory
          OpMemReg(op, rl_result, t_reg.GetReg());
          FreeTemp(t_reg);
          return;
        } else if (!rl_result.reg.IsFloat()) {
          // Can do this directly into the result register
          OpRegReg(op, rl_result.reg, t_reg);
          FreeTemp(t_reg);
          StoreFinalValue(rl_dest, rl_result);
          return;
        }
      }
      // Three address form, or we can't do directly.
      rl_lhs = LoadValue(rl_lhs, kCoreReg);
      rl_result = EvalLoc(rl_dest, kCoreReg, true);
      OpRegRegReg(op, rl_result.reg, rl_lhs.reg, t_reg);
      FreeTemp(t_reg);
    } else {
      // Multiply is 3 operand only (sort of).
      if (is_two_addr && op != kOpMul) {
        // Can we do this directly into memory?
        rl_result = UpdateLocTyped(rl_dest, kCoreReg);
        if (rl_result.location == kLocPhysReg) {
          // Ensure res is in a core reg
          rl_result = EvalLoc(rl_dest, kCoreReg, true);
          // Can we do this from memory directly?
          rl_rhs = UpdateLocTyped(rl_rhs, kCoreReg);
          if (rl_rhs.location != kLocPhysReg) {
            OpRegMem(op, rl_result.reg, rl_rhs);
            StoreFinalValue(rl_dest, rl_result);
            return;
          } else if (!rl_rhs.reg.IsFloat()) {
            OpRegReg(op, rl_result.reg, rl_rhs.reg);
            StoreFinalValue(rl_dest, rl_result);
            return;
          }
        }
        rl_rhs = LoadValue(rl_rhs, kCoreReg);
        // It might happen rl_rhs and rl_dest are the same VR
        // in this case rl_dest is in reg after LoadValue while
        // rl_result is not updated yet, so do this
        rl_result = UpdateLocTyped(rl_dest, kCoreReg);
        if (rl_result.location != kLocPhysReg) {
          // Okay, we can do this into memory.
          OpMemReg(op, rl_result, rl_rhs.reg.GetReg());
          return;
        } else if (!rl_result.reg.IsFloat()) {
          // Can do this directly into the result register.
          OpRegReg(op, rl_result.reg, rl_rhs.reg);
          StoreFinalValue(rl_dest, rl_result);
          return;
        } else {
          rl_lhs = LoadValue(rl_lhs, kCoreReg);
          rl_result = EvalLoc(rl_dest, kCoreReg, true);
          OpRegRegReg(op, rl_result.reg, rl_lhs.reg, rl_rhs.reg);
        }
      } else {
        // Try to use reg/memory instructions.
        rl_lhs = UpdateLocTyped(rl_lhs, kCoreReg);
        rl_rhs = UpdateLocTyped(rl_rhs, kCoreReg);
        // We can't optimize with FP registers.
        if (!IsOperationSafeWithoutTemps(rl_lhs, rl_rhs)) {
          // Something is difficult, so fall back to the standard case.
          rl_lhs = LoadValue(rl_lhs, kCoreReg);
          rl_rhs = LoadValue(rl_rhs, kCoreReg);
          rl_result = EvalLoc(rl_dest, kCoreReg, true);
          OpRegRegReg(op, rl_result.reg, rl_lhs.reg, rl_rhs.reg);
        } else {
          // We can optimize by moving to result and using memory operands.
          if (rl_rhs.location != kLocPhysReg) {
            // Force LHS into result.
            // We should be careful with order here
            // If rl_dest and rl_lhs points to the same VR we should load first
            // If the are different we should find a register first for dest
            if (mir_graph_->SRegToVReg(rl_dest.s_reg_low) ==
                mir_graph_->SRegToVReg(rl_lhs.s_reg_low)) {
              rl_lhs = LoadValue(rl_lhs, kCoreReg);
              rl_result = EvalLoc(rl_dest, kCoreReg, true);
              // No-op if these are the same.
              OpRegCopy(rl_result.reg, rl_lhs.reg);
            } else {
              rl_result = EvalLoc(rl_dest, kCoreReg, true);
              LoadValueDirect(rl_lhs, rl_result.reg);
            }
            OpRegMem(op, rl_result.reg, rl_rhs);
          } else if (rl_lhs.location != kLocPhysReg) {
            // RHS is in a register; LHS is in memory.
            if (op != kOpSub) {
              // Force RHS into result and operate on memory.
              rl_result = EvalLoc(rl_dest, kCoreReg, true);
              OpRegCopy(rl_result.reg, rl_rhs.reg);
              OpRegMem(op, rl_result.reg, rl_lhs);
            } else {
              // Subtraction isn't commutative.
              rl_lhs = LoadValue(rl_lhs, kCoreReg);
              rl_rhs = LoadValue(rl_rhs, kCoreReg);
              rl_result = EvalLoc(rl_dest, kCoreReg, true);
              OpRegRegReg(op, rl_result.reg, rl_lhs.reg, rl_rhs.reg);
            }
          } else {
            // Both are in registers.
            rl_lhs = LoadValue(rl_lhs, kCoreReg);
            rl_rhs = LoadValue(rl_rhs, kCoreReg);
            rl_result = EvalLoc(rl_dest, kCoreReg, true);
            OpRegRegReg(op, rl_result.reg, rl_lhs.reg, rl_rhs.reg);
          }
        }
      }
    }
  }
  StoreValue(rl_dest, rl_result);
}

bool X86Mir2Lir::IsOperationSafeWithoutTemps(RegLocation rl_lhs, RegLocation rl_rhs) {
  // If we have non-core registers, then we can't do good things.
  if (rl_lhs.location == kLocPhysReg && rl_lhs.reg.IsFloat()) {
    return false;
  }
  if (rl_rhs.location == kLocPhysReg && rl_rhs.reg.IsFloat()) {
    return false;
  }

  // Everything will be fine :-).
  return true;
}

void X86Mir2Lir::GenIntToLong(RegLocation rl_dest, RegLocation rl_src) {
  if (!cu_->target64) {
    Mir2Lir::GenIntToLong(rl_dest, rl_src);
    return;
  }
  rl_src = UpdateLocTyped(rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (rl_src.location == kLocPhysReg) {
    NewLIR2(kX86MovsxdRR, rl_result.reg.GetReg(), rl_src.reg.GetReg());
  } else {
    int displacement = SRegOffset(rl_src.s_reg_low);
    ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
    LIR *m = NewLIR3(kX86MovsxdRM, rl_result.reg.GetReg(), rs_rX86_SP.GetReg(),
                     displacement + LOWORD_OFFSET);
    AnnotateDalvikRegAccess(m, (displacement + LOWORD_OFFSET) >> 2,
                            true /* is_load */, true /* is_64bit */);
  }
  StoreValueWide(rl_dest, rl_result);
}

void X86Mir2Lir::GenShiftOpLong(Instruction::Code opcode, RegLocation rl_dest,
                        RegLocation rl_src1, RegLocation rl_shift) {
  if (!cu_->target64) {
    Mir2Lir::GenShiftOpLong(opcode, rl_dest, rl_src1, rl_shift);
    return;
  }

  bool is_two_addr = false;
  OpKind op = kOpBkpt;
  RegLocation rl_result;

  switch (opcode) {
    case Instruction::SHL_LONG_2ADDR:
      is_two_addr = true;
      // Fallthrough
    case Instruction::SHL_LONG:
      op = kOpLsl;
      break;
    case Instruction::SHR_LONG_2ADDR:
      is_two_addr = true;
      // Fallthrough
    case Instruction::SHR_LONG:
      op = kOpAsr;
      break;
    case Instruction::USHR_LONG_2ADDR:
      is_two_addr = true;
      // Fallthrough
    case Instruction::USHR_LONG:
      op = kOpLsr;
      break;
    default:
      op = kOpBkpt;
  }

  // X86 doesn't require masking and must use ECX.
  RegStorage t_reg = TargetReg(kCount, kNotWide);  // rCX
  LoadValueDirectFixed(rl_shift, t_reg);
  if (is_two_addr) {
    // Can we do this directly into memory?
    rl_result = UpdateLocWideTyped(rl_dest, kCoreReg);
    if (rl_result.location != kLocPhysReg) {
      // Okay, we can do this into memory
      ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
      OpMemReg(op, rl_result, t_reg.GetReg());
    } else if (!rl_result.reg.IsFloat()) {
      // Can do this directly into the result register
      OpRegReg(op, rl_result.reg, t_reg);
      StoreFinalValueWide(rl_dest, rl_result);
    }
  } else {
    // Three address form, or we can't do directly.
    rl_src1 = LoadValueWide(rl_src1, kCoreReg);
    rl_result = EvalLocWide(rl_dest, kCoreReg, true);
    OpRegRegReg(op, rl_result.reg, rl_src1.reg, t_reg);
    StoreFinalValueWide(rl_dest, rl_result);
  }

  FreeTemp(t_reg);
}

}  // namespace art
