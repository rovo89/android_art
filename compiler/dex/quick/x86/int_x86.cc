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
#include "mirror/array.h"
#include "x86_lir.h"

namespace art {

/*
 * Perform register memory operation.
 */
LIR* X86Mir2Lir::GenRegMemCheck(ConditionCode c_code,
                                int reg1, int base, int offset, ThrowKind kind) {
  LIR* tgt = RawLIR(0, kPseudoThrowTarget, kind,
                    current_dalvik_offset_, reg1, base, offset);
  OpRegMem(kOpCmp, reg1, base, offset);
  LIR* branch = OpCondBranch(c_code, tgt);
  // Remember branch target - will process later
  throw_launchpads_.Insert(tgt);
  return branch;
}

/*
 * Perform a compare of memory to immediate value
 */
LIR* X86Mir2Lir::GenMemImmedCheck(ConditionCode c_code,
                                int base, int offset, int check_value, ThrowKind kind) {
  LIR* tgt = RawLIR(0, kPseudoThrowTarget, kind,
                    current_dalvik_offset_, base, check_value, 0);
  NewLIR3(IS_SIMM8(check_value) ? kX86Cmp32MI8 : kX86Cmp32MI, base, offset, check_value);
  LIR* branch = OpCondBranch(c_code, tgt);
  // Remember branch target - will process later
  throw_launchpads_.Insert(tgt);
  return branch;
}

/*
 * Compare two 64-bit values
 *    x = y     return  0
 *    x < y     return -1
 *    x > y     return  1
 */
void X86Mir2Lir::GenCmpLong(RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) {
  FlushAllRegs();
  LockCallTemps();  // Prepare for explicit register usage
  LoadValueDirectWideFixed(rl_src1, r0, r1);
  LoadValueDirectWideFixed(rl_src2, r2, r3);
  // Compute (r1:r0) = (r1:r0) - (r3:r2)
  OpRegReg(kOpSub, r0, r2);  // r0 = r0 - r2
  OpRegReg(kOpSbc, r1, r3);  // r1 = r1 - r3 - CF
  NewLIR2(kX86Set8R, r2, kX86CondL);  // r2 = (r1:r0) < (r3:r2) ? 1 : 0
  NewLIR2(kX86Movzx8RR, r2, r2);
  OpReg(kOpNeg, r2);         // r2 = -r2
  OpRegReg(kOpOr, r0, r1);   // r0 = high | low - sets ZF
  NewLIR2(kX86Set8R, r0, kX86CondNz);  // r0 = (r1:r0) != (r3:r2) ? 1 : 0
  NewLIR2(kX86Movzx8RR, r0, r0);
  OpRegReg(kOpOr, r0, r2);   // r0 = r0 | r2
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

LIR* X86Mir2Lir::OpCmpBranch(ConditionCode cond, int src1, int src2,
                             LIR* target) {
  NewLIR2(kX86Cmp32RR, src1, src2);
  X86ConditionCode cc = X86ConditionEncoding(cond);
  LIR* branch = NewLIR2(kX86Jcc8, 0 /* lir operand for Jcc offset */ ,
                        cc);
  branch->target = target;
  return branch;
}

LIR* X86Mir2Lir::OpCmpImmBranch(ConditionCode cond, int reg,
                                int check_value, LIR* target) {
  if ((check_value == 0) && (cond == kCondEq || cond == kCondNe)) {
    // TODO: when check_value == 0 and reg is rCX, use the jcxz/nz opcode
    NewLIR2(kX86Test32RR, reg, reg);
  } else {
    NewLIR2(IS_SIMM8(check_value) ? kX86Cmp32RI8 : kX86Cmp32RI, reg, check_value);
  }
  X86ConditionCode cc = X86ConditionEncoding(cond);
  LIR* branch = NewLIR2(kX86Jcc8, 0 /* lir operand for Jcc offset */ , cc);
  branch->target = target;
  return branch;
}

LIR* X86Mir2Lir::OpRegCopyNoInsert(int r_dest, int r_src) {
  if (X86_FPREG(r_dest) || X86_FPREG(r_src))
    return OpFpRegCopy(r_dest, r_src);
  LIR* res = RawLIR(current_dalvik_offset_, kX86Mov32RR,
                    r_dest, r_src);
  if (!(cu_->disable_opt & (1 << kSafeOptimizations)) && r_dest == r_src) {
    res->flags.is_nop = true;
  }
  return res;
}

LIR* X86Mir2Lir::OpRegCopy(int r_dest, int r_src) {
  LIR *res = OpRegCopyNoInsert(r_dest, r_src);
  AppendLIR(res);
  return res;
}

void X86Mir2Lir::OpRegCopyWide(int dest_lo, int dest_hi,
                               int src_lo, int src_hi) {
  bool dest_fp = X86_FPREG(dest_lo) && X86_FPREG(dest_hi);
  bool src_fp = X86_FPREG(src_lo) && X86_FPREG(src_hi);
  assert(X86_FPREG(src_lo) == X86_FPREG(src_hi));
  assert(X86_FPREG(dest_lo) == X86_FPREG(dest_hi));
  if (dest_fp) {
    if (src_fp) {
      OpRegCopy(S2d(dest_lo, dest_hi), S2d(src_lo, src_hi));
    } else {
      // TODO: Prevent this from happening in the code. The result is often
      // unused or could have been loaded more easily from memory.
      NewLIR2(kX86MovdxrRR, dest_lo, src_lo);
      dest_hi = AllocTempDouble();
      NewLIR2(kX86MovdxrRR, dest_hi, src_hi);
      NewLIR2(kX86PunpckldqRR, dest_lo, dest_hi);
      FreeTemp(dest_hi);
    }
  } else {
    if (src_fp) {
      NewLIR2(kX86MovdrxRR, dest_lo, src_lo);
      NewLIR2(kX86PsrlqRI, src_lo, 32);
      NewLIR2(kX86MovdrxRR, dest_hi, src_lo);
    } else {
      // Handle overlap
      if (src_hi == dest_lo && src_lo == dest_hi) {
        // Deal with cycles.
        int temp_reg = AllocTemp();
        OpRegCopy(temp_reg, dest_hi);
        OpRegCopy(dest_hi, dest_lo);
        OpRegCopy(dest_lo, temp_reg);
        FreeTemp(temp_reg);
      } else if (src_hi == dest_lo) {
        OpRegCopy(dest_hi, src_hi);
        OpRegCopy(dest_lo, src_lo);
      } else {
        OpRegCopy(dest_lo, src_lo);
        OpRegCopy(dest_hi, src_hi);
      }
    }
  }
}

void X86Mir2Lir::GenSelect(BasicBlock* bb, MIR* mir) {
  RegLocation rl_result;
  RegLocation rl_src = mir_graph_->GetSrc(mir, 0);
  RegLocation rl_dest = mir_graph_->GetDest(mir);
  rl_src = LoadValue(rl_src, kCoreReg);

  // The kMirOpSelect has two variants, one for constants and one for moves.
  const bool is_constant_case = (mir->ssa_rep->num_uses == 1);

  if (is_constant_case) {
    int true_val = mir->dalvikInsn.vB;
    int false_val = mir->dalvikInsn.vC;
    rl_result = EvalLoc(rl_dest, kCoreReg, true);

    /*
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
     *     mov result_reg, $true_case
     *     mov t1, $false_case
     *     cmovnz result_reg, t1
     */
    const bool result_reg_same_as_src = (rl_src.location == kLocPhysReg && rl_src.reg.GetReg() == rl_result.reg.GetReg());
    const bool true_zero_case = (true_val == 0 && false_val != 0 && !result_reg_same_as_src);
    const bool false_zero_case = (false_val == 0 && true_val != 0 && !result_reg_same_as_src);
    const bool catch_all_case = !(true_zero_case || false_zero_case);

    if (true_zero_case || false_zero_case) {
      OpRegReg(kOpXor, rl_result.reg.GetReg(), rl_result.reg.GetReg());
    }

    if (true_zero_case || false_zero_case || catch_all_case) {
      OpRegImm(kOpCmp, rl_src.reg.GetReg(), 0);
    }

    if (catch_all_case) {
      OpRegImm(kOpMov, rl_result.reg.GetReg(), true_val);
    }

    if (true_zero_case || false_zero_case || catch_all_case) {
      int immediateForTemp = false_zero_case ? true_val : false_val;
      int temp1_reg = AllocTemp();
      OpRegImm(kOpMov, temp1_reg, immediateForTemp);

      ConditionCode cc = false_zero_case ? kCondEq : kCondNe;
      OpCondRegReg(kOpCmov, cc, rl_result.reg.GetReg(), temp1_reg);

      FreeTemp(temp1_reg);
    }
  } else {
    RegLocation rl_true = mir_graph_->GetSrc(mir, 1);
    RegLocation rl_false = mir_graph_->GetSrc(mir, 2);
    rl_true = LoadValue(rl_true, kCoreReg);
    rl_false = LoadValue(rl_false, kCoreReg);
    rl_result = EvalLoc(rl_dest, kCoreReg, true);

    /*
     * 1) When true case is already in place:
     *     cmp $0, src_reg
     *     cmovnz result_reg, false_reg
     * 2) When false case is already in place:
     *     cmp $0, src_reg
     *     cmovz result_reg, true_reg
     * 3) When neither cases are in place:
     *     cmp $0, src_reg
     *     mov result_reg, true_reg
     *     cmovnz result_reg, false_reg
     */

    // kMirOpSelect is generated just for conditional cases when comparison is done with zero.
    OpRegImm(kOpCmp, rl_src.reg.GetReg(), 0);

    if (rl_result.reg.GetReg() == rl_true.reg.GetReg()) {
      OpCondRegReg(kOpCmov, kCondNe, rl_result.reg.GetReg(), rl_false.reg.GetReg());
    } else if (rl_result.reg.GetReg() == rl_false.reg.GetReg()) {
      OpCondRegReg(kOpCmov, kCondEq, rl_result.reg.GetReg(), rl_true.reg.GetReg());
    } else {
      OpRegCopy(rl_result.reg.GetReg(), rl_true.reg.GetReg());
      OpCondRegReg(kOpCmov, kCondNe, rl_result.reg.GetReg(), rl_false.reg.GetReg());
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

  FlushAllRegs();
  LockCallTemps();  // Prepare for explicit register usage
  LoadValueDirectWideFixed(rl_src1, r0, r1);
  LoadValueDirectWideFixed(rl_src2, r2, r3);
  // Swap operands and condition code to prevent use of zero flag.
  if (ccode == kCondLe || ccode == kCondGt) {
    // Compute (r3:r2) = (r3:r2) - (r1:r0)
    OpRegReg(kOpSub, r2, r0);  // r2 = r2 - r0
    OpRegReg(kOpSbc, r3, r1);  // r3 = r3 - r1 - CF
  } else {
    // Compute (r1:r0) = (r1:r0) - (r3:r2)
    OpRegReg(kOpSub, r0, r2);  // r0 = r0 - r2
    OpRegReg(kOpSbc, r1, r3);  // r1 = r1 - r3 - CF
  }
  switch (ccode) {
    case kCondEq:
    case kCondNe:
      OpRegReg(kOpOr, r0, r1);  // r0 = r0 | r1
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
  LIR* not_taken = &block_label_list_[bb->fall_through];
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  int32_t low_reg = rl_src1.reg.GetReg();
  int32_t high_reg = rl_src1.reg.GetHighReg();

  if (val == 0 && (ccode == kCondEq || ccode == kCondNe)) {
    int t_reg = AllocTemp();
    OpRegRegReg(kOpOr, t_reg, low_reg, high_reg);
    FreeTemp(t_reg);
    OpCondBranch(ccode, taken);
    return;
  }

  OpRegImm(kOpCmp, high_reg, val_hi);
  switch (ccode) {
    case kCondEq:
    case kCondNe:
      OpCondBranch(kCondNe, (ccode == kCondEq) ? not_taken : taken);
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
  OpCmpImmBranch(ccode, low_reg, val_lo, taken);
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

RegLocation X86Mir2Lir::GenDivRemLit(RegLocation rl_dest, int reg_lo,
                                     int lit, bool is_div) {
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
  RegLocation rl_result = {kLocPhysReg, 0, 0, 0, 0, 0, 0, 0, 1, kVectorNotUsed,
                           RegStorage(RegStorage::k32BitSolo, r2), INVALID_SREG, INVALID_SREG};

  // handle div/rem by 1 special case.
  if (imm == 1) {
    if (is_div) {
      // x / 1 == x.
      StoreValue(rl_result, rl_src);
    } else {
      // x % 1 == 0.
      LoadConstantNoClobber(r0, 0);
      // For this case, return the result in EAX.
      rl_result.reg.SetReg(r0);
    }
  } else if (imm == -1) {  // handle 0x80000000 / -1 special case.
    if (is_div) {
      LIR *minint_branch = 0;
      LoadValueDirectFixed(rl_src, r0);
      OpRegImm(kOpCmp, r0, 0x80000000);
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
      LoadConstantNoClobber(r0, 0);
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
    int numerator_reg = -1;
    if (!is_div || (imm > 0 && magic < 0) || (imm < 0 && magic > 0)) {
      // We will need the value later.
      if (rl_src.location == kLocPhysReg) {
        // We can use it directly.
        DCHECK(rl_src.reg.GetReg() != r0 && rl_src.reg.GetReg() != r2);
        numerator_reg = rl_src.reg.GetReg();
      } else {
        LoadValueDirectFixed(rl_src, r1);
        numerator_reg = r1;
      }
      OpRegCopy(r0, numerator_reg);
    } else {
      // Only need this once.  Just put it into EAX.
      LoadValueDirectFixed(rl_src, r0);
    }

    // EDX = magic.
    LoadConstantNoClobber(r2, magic);

    // EDX:EAX = magic & dividend.
    NewLIR1(kX86Imul32DaR, r2);

    if (imm > 0 && magic < 0) {
      // Add numerator to EDX.
      DCHECK_NE(numerator_reg, -1);
      NewLIR2(kX86Add32RR, r2, numerator_reg);
    } else if (imm < 0 && magic > 0) {
      DCHECK_NE(numerator_reg, -1);
      NewLIR2(kX86Sub32RR, r2, numerator_reg);
    }

    // Do we need the shift?
    if (shift != 0) {
      // Shift EDX by 'shift' bits.
      NewLIR2(kX86Sar32RI, r2, shift);
    }

    // Add 1 to EDX if EDX < 0.

    // Move EDX to EAX.
    OpRegCopy(r0, r2);

    // Move sign bit to bit 0, zeroing the rest.
    NewLIR2(kX86Shr32RI, r2, 31);

    // EDX = EDX + EAX.
    NewLIR2(kX86Add32RR, r2, r0);

    // Quotient is in EDX.
    if (!is_div) {
      // We need to compute the remainder.
      // Remainder is divisor - (quotient * imm).
      DCHECK_NE(numerator_reg, -1);
      OpRegCopy(r0, numerator_reg);

      // EAX = numerator * imm.
      OpRegRegImm(kOpMul, r2, r2, imm);

      // EDX -= EAX.
      NewLIR2(kX86Sub32RR, r0, r2);

      // For this case, return the result in EAX.
      rl_result.reg.SetReg(r0);
    }
  }

  return rl_result;
}

RegLocation X86Mir2Lir::GenDivRem(RegLocation rl_dest, int reg_lo,
                                  int reg_hi, bool is_div) {
  LOG(FATAL) << "Unexpected use of GenDivRem for x86";
  return rl_dest;
}

RegLocation X86Mir2Lir::GenDivRem(RegLocation rl_dest, RegLocation rl_src1,
                                  RegLocation rl_src2, bool is_div, bool check_zero) {
  // We have to use fixed registers, so flush all the temps.
  FlushAllRegs();
  LockCallTemps();  // Prepare for explicit register usage.

  // Load LHS into EAX.
  LoadValueDirectFixed(rl_src1, r0);

  // Load RHS into EBX.
  LoadValueDirectFixed(rl_src2, r1);

  // Copy LHS sign bit into EDX.
  NewLIR0(kx86Cdq32Da);

  if (check_zero) {
    // Handle division by zero case.
    GenImmedCheck(kCondEq, r1, 0, kThrowDivZero);
  }

  // Have to catch 0x80000000/-1 case, or we will get an exception!
  OpRegImm(kOpCmp, r1, -1);
  LIR *minus_one_branch = NewLIR2(kX86Jcc8, 0, kX86CondNe);

  // RHS is -1.
  OpRegImm(kOpCmp, r0, 0x80000000);
  LIR * minint_branch = NewLIR2(kX86Jcc8, 0, kX86CondNe);

  // In 0x80000000/-1 case.
  if (!is_div) {
    // For DIV, EAX is already right. For REM, we need EDX 0.
    LoadConstantNoClobber(r2, 0);
  }
  LIR* done = NewLIR1(kX86Jmp8, 0);

  // Expected case.
  minus_one_branch->target = NewLIR0(kPseudoTargetLabel);
  minint_branch->target = minus_one_branch->target;
  NewLIR1(kX86Idivmod32DaR, r1);
  done->target = NewLIR0(kPseudoTargetLabel);

  // Result is in EAX for div and EDX for rem.
  RegLocation rl_result = {kLocPhysReg, 0, 0, 0, 0, 0, 0, 0, 1, kVectorNotUsed,
                           RegStorage(RegStorage::k32BitSolo, r0), INVALID_SREG, INVALID_SREG};
  if (!is_div) {
    rl_result.reg.SetReg(r2);
  }
  return rl_result;
}

bool X86Mir2Lir::GenInlinedMinMaxInt(CallInfo* info, bool is_min) {
  DCHECK_EQ(cu_->instruction_set, kX86);

  // Get the two arguments to the invoke and place them in GP registers.
  RegLocation rl_src1 = info->args[0];
  RegLocation rl_src2 = info->args[1];
  rl_src1 = LoadValue(rl_src1, kCoreReg);
  rl_src2 = LoadValue(rl_src2, kCoreReg);

  RegLocation rl_dest = InlineTarget(info);
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
  OpRegCopy(rl_result.reg.GetReg(), rl_src1.reg.GetReg());

  // If the integers are both in the same register, then there is nothing else to do
  // because they are equal and we have already moved one into the result.
  if (rl_src1.reg.GetReg() != rl_src2.reg.GetReg()) {
    // It is possible we didn't pick correctly so do the actual comparison now.
    OpRegReg(kOpCmp, rl_src1.reg.GetReg(), rl_src2.reg.GetReg());

    // Conditionally move the other integer into the destination register.
    ConditionCode condition_code = is_min ? kCondGt : kCondLt;
    OpCondRegReg(kOpCmov, condition_code, rl_result.reg.GetReg(), rl_src2.reg.GetReg());
  }

  StoreValue(rl_dest, rl_result);
  return true;
}

bool X86Mir2Lir::GenInlinedPeek(CallInfo* info, OpSize size) {
  RegLocation rl_src_address = info->args[0];  // long address
  rl_src_address.wide = 0;  // ignore high half in info->args[1]
  RegLocation rl_dest = size == kLong ? InlineTargetWide(info) : InlineTarget(info);
  RegLocation rl_address = LoadValue(rl_src_address, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (size == kLong) {
    // Unaligned access is allowed on x86.
    LoadBaseDispWide(rl_address.reg.GetReg(), 0, rl_result.reg.GetReg(), rl_result.reg.GetHighReg(), INVALID_SREG);
    StoreValueWide(rl_dest, rl_result);
  } else {
    DCHECK(size == kSignedByte || size == kSignedHalf || size == kWord);
    // Unaligned access is allowed on x86.
    LoadBaseDisp(rl_address.reg.GetReg(), 0, rl_result.reg.GetReg(), size, INVALID_SREG);
    StoreValue(rl_dest, rl_result);
  }
  return true;
}

bool X86Mir2Lir::GenInlinedPoke(CallInfo* info, OpSize size) {
  RegLocation rl_src_address = info->args[0];  // long address
  rl_src_address.wide = 0;  // ignore high half in info->args[1]
  RegLocation rl_src_value = info->args[2];  // [size] value
  RegLocation rl_address = LoadValue(rl_src_address, kCoreReg);
  if (size == kLong) {
    // Unaligned access is allowed on x86.
    RegLocation rl_value = LoadValueWide(rl_src_value, kCoreReg);
    StoreBaseDispWide(rl_address.reg.GetReg(), 0, rl_value.reg.GetReg(), rl_value.reg.GetHighReg());
  } else {
    DCHECK(size == kSignedByte || size == kSignedHalf || size == kWord);
    // Unaligned access is allowed on x86.
    RegLocation rl_value = LoadValue(rl_src_value, kCoreReg);
    StoreBaseDisp(rl_address.reg.GetReg(), 0, rl_value.reg.GetReg(), size);
  }
  return true;
}

void X86Mir2Lir::OpLea(int rBase, int reg1, int reg2, int scale, int offset) {
  NewLIR5(kX86Lea32RA, rBase, reg1, reg2, scale, offset);
}

void X86Mir2Lir::OpTlsCmp(ThreadOffset offset, int val) {
  NewLIR2(kX86Cmp16TI8, offset.Int32Value(), val);
}

bool X86Mir2Lir::GenInlinedCas(CallInfo* info, bool is_long, bool is_object) {
  DCHECK_EQ(cu_->instruction_set, kX86);
  // Unused - RegLocation rl_src_unsafe = info->args[0];
  RegLocation rl_src_obj = info->args[1];  // Object - known non-null
  RegLocation rl_src_offset = info->args[2];  // long low
  rl_src_offset.wide = 0;  // ignore high half in info->args[3]
  RegLocation rl_src_expected = info->args[4];  // int, long or Object
  // If is_long, high half is in info->args[5]
  RegLocation rl_src_new_value = info->args[is_long ? 6 : 5];  // int, long or Object
  // If is_long, high half is in info->args[7]

  if (is_long) {
    FlushAllRegs();
    LockCallTemps();
    LoadValueDirectWideFixed(rl_src_expected, rAX, rDX);
    LoadValueDirectWideFixed(rl_src_new_value, rBX, rCX);
    NewLIR1(kX86Push32R, rDI);
    MarkTemp(rDI);
    LockTemp(rDI);
    NewLIR1(kX86Push32R, rSI);
    MarkTemp(rSI);
    LockTemp(rSI);
    const int push_offset = 4 /* push edi */ + 4 /* push esi */;
    LoadWordDisp(TargetReg(kSp), SRegOffset(rl_src_obj.s_reg_low) + push_offset, rDI);
    LoadWordDisp(TargetReg(kSp), SRegOffset(rl_src_offset.s_reg_low) + push_offset, rSI);
    NewLIR4(kX86LockCmpxchg8bA, rDI, rSI, 0, 0);
    FreeTemp(rSI);
    UnmarkTemp(rSI);
    NewLIR1(kX86Pop32R, rSI);
    FreeTemp(rDI);
    UnmarkTemp(rDI);
    NewLIR1(kX86Pop32R, rDI);
    FreeCallTemps();
  } else {
    // EAX must hold expected for CMPXCHG. Neither rl_new_value, nor r_ptr may be in EAX.
    FlushReg(r0);
    LockTemp(r0);

    // Release store semantics, get the barrier out of the way.  TODO: revisit
    GenMemBarrier(kStoreLoad);

    RegLocation rl_object = LoadValue(rl_src_obj, kCoreReg);
    RegLocation rl_new_value = LoadValue(rl_src_new_value, kCoreReg);

    if (is_object && !mir_graph_->IsConstantNullRef(rl_new_value)) {
      // Mark card for object assuming new value is stored.
      FreeTemp(r0);  // Temporarily release EAX for MarkGCCard().
      MarkGCCard(rl_new_value.reg.GetReg(), rl_object.reg.GetReg());
      LockTemp(r0);
    }

    RegLocation rl_offset = LoadValue(rl_src_offset, kCoreReg);
    LoadValueDirect(rl_src_expected, r0);
    NewLIR5(kX86LockCmpxchgAR, rl_object.reg.GetReg(), rl_offset.reg.GetReg(), 0, 0, rl_new_value.reg.GetReg());

    FreeTemp(r0);
  }

  // Convert ZF to boolean
  RegLocation rl_dest = InlineTarget(info);  // boolean place for result
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  NewLIR2(kX86Set8R, rl_result.reg.GetReg(), kX86CondZ);
  NewLIR2(kX86Movzx8RR, rl_result.reg.GetReg(), rl_result.reg.GetReg());
  StoreValue(rl_dest, rl_result);
  return true;
}

LIR* X86Mir2Lir::OpPcRelLoad(int reg, LIR* target) {
  CHECK(base_of_code_ != nullptr);

  // Address the start of the method
  RegLocation rl_method = mir_graph_->GetRegLocation(base_of_code_->s_reg_low);
  LoadValueDirectFixed(rl_method, reg);
  store_method_addr_used_ = true;

  // Load the proper value from the literal area.
  // We don't know the proper offset for the value, so pick one that will force
  // 4 byte offset.  We will fix this up in the assembler later to have the right
  // value.
  LIR *res = RawLIR(current_dalvik_offset_, kX86Mov32RM, reg, reg, 256, 0, 0, target);
  res->target = target;
  res->flags.fixup = kFixupLoad;
  SetMemRefType(res, true, kLiteral);
  store_method_addr_used_ = true;
  return res;
}

LIR* X86Mir2Lir::OpVldm(int rBase, int count) {
  LOG(FATAL) << "Unexpected use of OpVldm for x86";
  return NULL;
}

LIR* X86Mir2Lir::OpVstm(int rBase, int count) {
  LOG(FATAL) << "Unexpected use of OpVstm for x86";
  return NULL;
}

void X86Mir2Lir::GenMultiplyByTwoBitMultiplier(RegLocation rl_src,
                                               RegLocation rl_result, int lit,
                                               int first_bit, int second_bit) {
  int t_reg = AllocTemp();
  OpRegRegImm(kOpLsl, t_reg, rl_src.reg.GetReg(), second_bit - first_bit);
  OpRegRegReg(kOpAdd, rl_result.reg.GetReg(), rl_src.reg.GetReg(), t_reg);
  FreeTemp(t_reg);
  if (first_bit != 0) {
    OpRegRegImm(kOpLsl, rl_result.reg.GetReg(), rl_result.reg.GetReg(), first_bit);
  }
}

void X86Mir2Lir::GenDivZeroCheck(int reg_lo, int reg_hi) {
  // We are not supposed to clobber either of the provided registers, so allocate
  // a temporary to use for the check.
  int t_reg = AllocTemp();

  // Doing an OR is a quick way to check if both registers are zero. This will set the flags.
  OpRegRegReg(kOpOr, t_reg, reg_lo, reg_hi);

  // In case of zero, throw ArithmeticException.
  GenCheck(kCondEq, kThrowDivZero);

  // The temp is no longer needed so free it at this time.
  FreeTemp(t_reg);
}

// Test suspend flag, return target of taken suspend branch
LIR* X86Mir2Lir::OpTestSuspend(LIR* target) {
  OpTlsCmp(Thread::ThreadFlagsOffset(), 0);
  return OpCondBranch((target == NULL) ? kCondNe : kCondEq, target);
}

// Decrement register and branch on condition
LIR* X86Mir2Lir::OpDecAndBranch(ConditionCode c_code, int reg, LIR* target) {
  OpRegImm(kOpSub, reg, 1);
  return OpCondBranch(c_code, target);
}

bool X86Mir2Lir::SmallLiteralDivRem(Instruction::Code dalvik_opcode, bool is_div,
                                    RegLocation rl_src, RegLocation rl_dest, int lit) {
  LOG(FATAL) << "Unexpected use of smallLiteralDive in x86";
  return false;
}

LIR* X86Mir2Lir::OpIT(ConditionCode cond, const char* guide) {
  LOG(FATAL) << "Unexpected use of OpIT in x86";
  return NULL;
}

void X86Mir2Lir::GenImulRegImm(int dest, int src, int val) {
  switch (val) {
    case 0:
      NewLIR2(kX86Xor32RR, dest, dest);
      break;
    case 1:
      OpRegCopy(dest, src);
      break;
    default:
      OpRegRegImm(kOpMul, dest, src, val);
      break;
  }
}

void X86Mir2Lir::GenImulMemImm(int dest, int sreg, int displacement, int val) {
  LIR *m;
  switch (val) {
    case 0:
      NewLIR2(kX86Xor32RR, dest, dest);
      break;
    case 1:
      LoadBaseDisp(rX86_SP, displacement, dest, kWord, sreg);
      break;
    default:
      m = NewLIR4(IS_SIMM8(val) ? kX86Imul32RMI8 : kX86Imul32RMI, dest, rX86_SP,
                  displacement, val);
      AnnotateDalvikRegAccess(m, displacement >> 2, true /* is_load */, true /* is_64bit */);
      break;
  }
}

void X86Mir2Lir::GenMulLong(Instruction::Code, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) {
  if (rl_src1.is_const) {
    std::swap(rl_src1, rl_src2);
  }
  // Are we multiplying by a constant?
  if (rl_src2.is_const) {
    // Do special compare/branch against simple const operand
    int64_t val = mir_graph_->ConstantValueWide(rl_src2);
    if (val == 0) {
      RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
      OpRegReg(kOpXor, rl_result.reg.GetReg(), rl_result.reg.GetReg());
      OpRegReg(kOpXor, rl_result.reg.GetHighReg(), rl_result.reg.GetHighReg());
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
    rl_src1 = UpdateLocWide(rl_src1);
    bool src1_in_reg = rl_src1.location == kLocPhysReg;
    int displacement = SRegOffset(rl_src1.s_reg_low);

    // ECX <- 1H * 2L
    // EAX <- 1L * 2H
    if (src1_in_reg) {
      GenImulRegImm(r1, rl_src1.reg.GetHighReg(), val_lo);
      GenImulRegImm(r0, rl_src1.reg.GetReg(), val_hi);
    } else {
      GenImulMemImm(r1, GetSRegHi(rl_src1.s_reg_low), displacement + HIWORD_OFFSET, val_lo);
      GenImulMemImm(r0, rl_src1.s_reg_low, displacement + LOWORD_OFFSET, val_hi);
    }

    // ECX <- ECX + EAX  (2H * 1L) + (1H * 2L)
    NewLIR2(kX86Add32RR, r1, r0);

    // EAX <- 2L
    LoadConstantNoClobber(r0, val_lo);

    // EDX:EAX <- 2L * 1L (double precision)
    if (src1_in_reg) {
      NewLIR1(kX86Mul32DaR, rl_src1.reg.GetReg());
    } else {
      LIR *m = NewLIR2(kX86Mul32DaM, rX86_SP, displacement + LOWORD_OFFSET);
      AnnotateDalvikRegAccess(m, (displacement + LOWORD_OFFSET) >> 2,
                              true /* is_load */, true /* is_64bit */);
    }

    // EDX <- EDX + ECX (add high words)
    NewLIR2(kX86Add32RR, r2, r1);

    // Result is EDX:EAX
    RegLocation rl_result = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, kVectorNotUsed,
                             RegStorage(RegStorage::k64BitPair, r0, r2),
                             INVALID_SREG, INVALID_SREG};
    StoreValueWide(rl_dest, rl_result);
    return;
  }

  // Nope.  Do it the hard way
  // Check for V*V.  We can eliminate a multiply in that case, as 2L*1H == 2H*1L.
  bool is_square = mir_graph_->SRegToVReg(rl_src1.s_reg_low) ==
                   mir_graph_->SRegToVReg(rl_src2.s_reg_low);

  FlushAllRegs();
  LockCallTemps();  // Prepare for explicit register usage.
  rl_src1 = UpdateLocWide(rl_src1);
  rl_src2 = UpdateLocWide(rl_src2);

  // At this point, the VRs are in their home locations.
  bool src1_in_reg = rl_src1.location == kLocPhysReg;
  bool src2_in_reg = rl_src2.location == kLocPhysReg;

  // ECX <- 1H
  if (src1_in_reg) {
    NewLIR2(kX86Mov32RR, r1, rl_src1.reg.GetHighReg());
  } else {
    LoadBaseDisp(rX86_SP, SRegOffset(rl_src1.s_reg_low) + HIWORD_OFFSET, r1,
                 kWord, GetSRegHi(rl_src1.s_reg_low));
  }

  if (is_square) {
    // Take advantage of the fact that the values are the same.
    // ECX <- ECX * 2L  (1H * 2L)
    if (src2_in_reg) {
      NewLIR2(kX86Imul32RR, r1, rl_src2.reg.GetReg());
    } else {
      int displacement = SRegOffset(rl_src2.s_reg_low);
      LIR *m = NewLIR3(kX86Imul32RM, r1, rX86_SP, displacement + LOWORD_OFFSET);
      AnnotateDalvikRegAccess(m, (displacement + LOWORD_OFFSET) >> 2,
                              true /* is_load */, true /* is_64bit */);
    }

    // ECX <- 2*ECX (2H * 1L) + (1H * 2L)
    NewLIR2(kX86Add32RR, r1, r1);
  } else {
    // EAX <- 2H
    if (src2_in_reg) {
      NewLIR2(kX86Mov32RR, r0, rl_src2.reg.GetHighReg());
    } else {
      LoadBaseDisp(rX86_SP, SRegOffset(rl_src2.s_reg_low) + HIWORD_OFFSET, r0,
                   kWord, GetSRegHi(rl_src2.s_reg_low));
    }

    // EAX <- EAX * 1L  (2H * 1L)
    if (src1_in_reg) {
      NewLIR2(kX86Imul32RR, r0, rl_src1.reg.GetReg());
    } else {
      int displacement = SRegOffset(rl_src1.s_reg_low);
      LIR *m = NewLIR3(kX86Imul32RM, r0, rX86_SP, displacement + LOWORD_OFFSET);
      AnnotateDalvikRegAccess(m, (displacement + LOWORD_OFFSET) >> 2,
                              true /* is_load */, true /* is_64bit */);
    }

    // ECX <- ECX * 2L  (1H * 2L)
    if (src2_in_reg) {
      NewLIR2(kX86Imul32RR, r1, rl_src2.reg.GetReg());
    } else {
      int displacement = SRegOffset(rl_src2.s_reg_low);
      LIR *m = NewLIR3(kX86Imul32RM, r1, rX86_SP, displacement + LOWORD_OFFSET);
      AnnotateDalvikRegAccess(m, (displacement + LOWORD_OFFSET) >> 2,
                              true /* is_load */, true /* is_64bit */);
    }

    // ECX <- ECX + EAX  (2H * 1L) + (1H * 2L)
    NewLIR2(kX86Add32RR, r1, r0);
  }

  // EAX <- 2L
  if (src2_in_reg) {
    NewLIR2(kX86Mov32RR, r0, rl_src2.reg.GetReg());
  } else {
    LoadBaseDisp(rX86_SP, SRegOffset(rl_src2.s_reg_low) + LOWORD_OFFSET, r0,
                 kWord, rl_src2.s_reg_low);
  }

  // EDX:EAX <- 2L * 1L (double precision)
  if (src1_in_reg) {
    NewLIR1(kX86Mul32DaR, rl_src1.reg.GetReg());
  } else {
    int displacement = SRegOffset(rl_src1.s_reg_low);
    LIR *m = NewLIR2(kX86Mul32DaM, rX86_SP, displacement + LOWORD_OFFSET);
    AnnotateDalvikRegAccess(m, (displacement + LOWORD_OFFSET) >> 2,
                            true /* is_load */, true /* is_64bit */);
  }

  // EDX <- EDX + ECX (add high words)
  NewLIR2(kX86Add32RR, r2, r1);

  // Result is EDX:EAX
  RegLocation rl_result = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, kVectorNotUsed,
                           RegStorage(RegStorage::k64BitPair, r0, r2), INVALID_SREG, INVALID_SREG};
  StoreValueWide(rl_dest, rl_result);
}

void X86Mir2Lir::GenLongRegOrMemOp(RegLocation rl_dest, RegLocation rl_src,
                                   Instruction::Code op) {
  DCHECK_EQ(rl_dest.location, kLocPhysReg);
  X86OpCode x86op = GetOpcode(op, rl_dest, rl_src, false);
  if (rl_src.location == kLocPhysReg) {
    // Both operands are in registers.
    if (rl_dest.reg.GetReg() == rl_src.reg.GetHighReg()) {
      // The registers are the same, so we would clobber it before the use.
      int temp_reg = AllocTemp();
      OpRegCopy(temp_reg, rl_dest.reg.GetReg());
      rl_src.reg.SetHighReg(temp_reg);
    }
    NewLIR2(x86op, rl_dest.reg.GetReg(), rl_src.reg.GetReg());

    x86op = GetOpcode(op, rl_dest, rl_src, true);
    NewLIR2(x86op, rl_dest.reg.GetHighReg(), rl_src.reg.GetHighReg());
    FreeTemp(rl_src.reg.GetReg());
    FreeTemp(rl_src.reg.GetHighReg());
    return;
  }

  // RHS is in memory.
  DCHECK((rl_src.location == kLocDalvikFrame) ||
         (rl_src.location == kLocCompilerTemp));
  int rBase = TargetReg(kSp);
  int displacement = SRegOffset(rl_src.s_reg_low);

  LIR *lir = NewLIR3(x86op, rl_dest.reg.GetReg(), rBase, displacement + LOWORD_OFFSET);
  AnnotateDalvikRegAccess(lir, (displacement + LOWORD_OFFSET) >> 2,
                          true /* is_load */, true /* is64bit */);
  x86op = GetOpcode(op, rl_dest, rl_src, true);
  lir = NewLIR3(x86op, rl_dest.reg.GetHighReg(), rBase, displacement + HIWORD_OFFSET);
  AnnotateDalvikRegAccess(lir, (displacement + HIWORD_OFFSET) >> 2,
                          true /* is_load */, true /* is64bit */);
}

void X86Mir2Lir::GenLongArith(RegLocation rl_dest, RegLocation rl_src, Instruction::Code op) {
  rl_dest = UpdateLocWide(rl_dest);
  if (rl_dest.location == kLocPhysReg) {
    // Ensure we are in a register pair
    RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);

    rl_src = UpdateLocWide(rl_src);
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
  int rBase = TargetReg(kSp);
  int displacement = SRegOffset(rl_dest.s_reg_low);

  LIR *lir = NewLIR3(x86op, rBase, displacement + LOWORD_OFFSET, rl_src.reg.GetReg());
  AnnotateDalvikRegAccess(lir, (displacement + LOWORD_OFFSET) >> 2,
                          false /* is_load */, true /* is64bit */);
  x86op = GetOpcode(op, rl_dest, rl_src, true);
  lir = NewLIR3(x86op, rBase, displacement + HIWORD_OFFSET, rl_src.reg.GetHighReg());
  AnnotateDalvikRegAccess(lir, (displacement + HIWORD_OFFSET) >> 2,
                          false /* is_load */, true /* is64bit */);
  FreeTemp(rl_src.reg.GetReg());
  FreeTemp(rl_src.reg.GetHighReg());
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
      GenLongArith(rl_dest, rl_src2, op);
      return;
    default:
      break;
  }

  if (rl_dest.location == kLocPhysReg) {
    RegLocation rl_result = LoadValueWide(rl_src1, kCoreReg);

    // We are about to clobber the LHS, so it needs to be a temp.
    rl_result = ForceTempWide(rl_result);

    // Perform the operation using the RHS.
    rl_src2 = UpdateLocWide(rl_src2);
    GenLongRegOrMemOp(rl_result, rl_src2, op);

    // And now record that the result is in the temp.
    StoreFinalValueWide(rl_dest, rl_result);
    return;
  }

  // It wasn't in registers, so it better be in memory.
  DCHECK((rl_dest.location == kLocDalvikFrame) ||
         (rl_dest.location == kLocCompilerTemp));
  rl_src1 = UpdateLocWide(rl_src1);
  rl_src2 = UpdateLocWide(rl_src2);

  // Get one of the source operands into temporary register.
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  if (IsTemp(rl_src1.reg.GetReg()) && IsTemp(rl_src1.reg.GetHighReg())) {
    GenLongRegOrMemOp(rl_src1, rl_src2, op);
  } else if (is_commutative) {
    rl_src2 = LoadValueWide(rl_src2, kCoreReg);
    // We need at least one of them to be a temporary.
    if (!(IsTemp(rl_src2.reg.GetReg()) && IsTemp(rl_src2.reg.GetHighReg()))) {
      rl_src1 = ForceTempWide(rl_src1);
    }
    GenLongRegOrMemOp(rl_src1, rl_src2, op);
  } else {
    // Need LHS to be the temp.
    rl_src1 = ForceTempWide(rl_src1);
    GenLongRegOrMemOp(rl_src1, rl_src2, op);
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

void X86Mir2Lir::GenNegLong(RegLocation rl_dest, RegLocation rl_src) {
  rl_src = LoadValueWide(rl_src, kCoreReg);
  RegLocation rl_result = ForceTempWide(rl_src);
  if (((rl_dest.location == kLocPhysReg) && (rl_src.location == kLocPhysReg)) &&
      ((rl_dest.reg.GetReg() == rl_src.reg.GetHighReg()))) {
    // The registers are the same, so we would clobber it before the use.
    int temp_reg = AllocTemp();
    OpRegCopy(temp_reg, rl_result.reg.GetReg());
    rl_result.reg.SetHighReg(temp_reg);
  }
  OpRegReg(kOpNeg, rl_result.reg.GetReg(), rl_result.reg.GetReg());    // rLow = -rLow
  OpRegImm(kOpAdc, rl_result.reg.GetHighReg(), 0);                   // rHigh = rHigh + CF
  OpRegReg(kOpNeg, rl_result.reg.GetHighReg(), rl_result.reg.GetHighReg());  // rHigh = -rHigh
  StoreValueWide(rl_dest, rl_result);
}

void X86Mir2Lir::OpRegThreadMem(OpKind op, int r_dest, ThreadOffset thread_offset) {
  X86OpCode opcode = kX86Bkpt;
  switch (op) {
  case kOpCmp: opcode = kX86Cmp32RT;  break;
  case kOpMov: opcode = kX86Mov32RT;  break;
  default:
    LOG(FATAL) << "Bad opcode: " << op;
    break;
  }
  NewLIR2(opcode, r_dest, thread_offset.Int32Value());
}

/*
 * Generate array load
 */
void X86Mir2Lir::GenArrayGet(int opt_flags, OpSize size, RegLocation rl_array,
                             RegLocation rl_index, RegLocation rl_dest, int scale) {
  RegisterClass reg_class = oat_reg_class_by_size(size);
  int len_offset = mirror::Array::LengthOffset().Int32Value();
  RegLocation rl_result;
  rl_array = LoadValue(rl_array, kCoreReg);

  int data_offset;
  if (size == kLong || size == kDouble) {
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
    rl_index.reg = RegStorage(RegStorage::k32BitSolo, INVALID_REG);
  }

  /* null object? */
  GenNullCheck(rl_array.s_reg_low, rl_array.reg.GetReg(), opt_flags);

  if (!(opt_flags & MIR_IGNORE_RANGE_CHECK)) {
    if (constant_index) {
      GenMemImmedCheck(kCondLs, rl_array.reg.GetReg(), len_offset,
                       constant_index_value, kThrowConstantArrayBounds);
    } else {
      GenRegMemCheck(kCondUge, rl_index.reg.GetReg(), rl_array.reg.GetReg(),
                     len_offset, kThrowArrayBounds);
    }
  }
  rl_result = EvalLoc(rl_dest, reg_class, true);
  if ((size == kLong) || (size == kDouble)) {
    LoadBaseIndexedDisp(rl_array.reg.GetReg(), rl_index.reg.GetReg(), scale, data_offset, rl_result.reg.GetReg(),
                        rl_result.reg.GetHighReg(), size, INVALID_SREG);
    StoreValueWide(rl_dest, rl_result);
  } else {
    LoadBaseIndexedDisp(rl_array.reg.GetReg(), rl_index.reg.GetReg(), scale,
                        data_offset, rl_result.reg.GetReg(), INVALID_REG, size,
                        INVALID_SREG);
    StoreValue(rl_dest, rl_result);
  }
}

/*
 * Generate array store
 *
 */
void X86Mir2Lir::GenArrayPut(int opt_flags, OpSize size, RegLocation rl_array,
                             RegLocation rl_index, RegLocation rl_src, int scale, bool card_mark) {
  RegisterClass reg_class = oat_reg_class_by_size(size);
  int len_offset = mirror::Array::LengthOffset().Int32Value();
  int data_offset;

  if (size == kLong || size == kDouble) {
    data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Int32Value();
  } else {
    data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Int32Value();
  }

  rl_array = LoadValue(rl_array, kCoreReg);
  bool constant_index = rl_index.is_const;
  int32_t constant_index_value = 0;
  if (!constant_index) {
    rl_index = LoadValue(rl_index, kCoreReg);
  } else {
    // If index is constant, just fold it into the data offset
    constant_index_value = mir_graph_->ConstantValue(rl_index);
    data_offset += constant_index_value << scale;
    // treat as non array below
    rl_index.reg = RegStorage(RegStorage::k32BitSolo, INVALID_REG);
  }

  /* null object? */
  GenNullCheck(rl_array.s_reg_low, rl_array.reg.GetReg(), opt_flags);

  if (!(opt_flags & MIR_IGNORE_RANGE_CHECK)) {
    if (constant_index) {
      GenMemImmedCheck(kCondLs, rl_array.reg.GetReg(), len_offset,
                       constant_index_value, kThrowConstantArrayBounds);
    } else {
      GenRegMemCheck(kCondUge, rl_index.reg.GetReg(), rl_array.reg.GetReg(),
                     len_offset, kThrowArrayBounds);
    }
  }
  if ((size == kLong) || (size == kDouble)) {
    rl_src = LoadValueWide(rl_src, reg_class);
  } else {
    rl_src = LoadValue(rl_src, reg_class);
  }
  // If the src reg can't be byte accessed, move it to a temp first.
  if ((size == kSignedByte || size == kUnsignedByte) && rl_src.reg.GetReg() >= 4) {
    int temp = AllocTemp();
    OpRegCopy(temp, rl_src.reg.GetReg());
    StoreBaseIndexedDisp(rl_array.reg.GetReg(), rl_index.reg.GetReg(), scale, data_offset, temp,
                         INVALID_REG, size, INVALID_SREG);
  } else {
    StoreBaseIndexedDisp(rl_array.reg.GetReg(), rl_index.reg.GetReg(), scale, data_offset, rl_src.reg.GetReg(),
                         rl_src.wide ? rl_src.reg.GetHighReg() : INVALID_REG, size, INVALID_SREG);
  }
  if (card_mark) {
    // Free rl_index if its a temp. Ensures there are 2 free regs for card mark.
    if (!constant_index) {
      FreeTemp(rl_index.reg.GetReg());
    }
    MarkGCCard(rl_src.reg.GetReg(), rl_array.reg.GetReg());
  }
}

RegLocation X86Mir2Lir::GenShiftImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                          RegLocation rl_src, int shift_amount) {
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  switch (opcode) {
    case Instruction::SHL_LONG:
    case Instruction::SHL_LONG_2ADDR:
      DCHECK_NE(shift_amount, 1);  // Prevent a double store from happening.
      if (shift_amount == 32) {
        OpRegCopy(rl_result.reg.GetHighReg(), rl_src.reg.GetReg());
        LoadConstant(rl_result.reg.GetReg(), 0);
      } else if (shift_amount > 31) {
        OpRegCopy(rl_result.reg.GetHighReg(), rl_src.reg.GetReg());
        FreeTemp(rl_src.reg.GetHighReg());
        NewLIR2(kX86Sal32RI, rl_result.reg.GetHighReg(), shift_amount - 32);
        LoadConstant(rl_result.reg.GetReg(), 0);
      } else {
        OpRegCopy(rl_result.reg.GetReg(), rl_src.reg.GetReg());
        OpRegCopy(rl_result.reg.GetHighReg(), rl_src.reg.GetHighReg());
        NewLIR3(kX86Shld32RRI, rl_result.reg.GetHighReg(), rl_result.reg.GetReg(), shift_amount);
        NewLIR2(kX86Sal32RI, rl_result.reg.GetReg(), shift_amount);
      }
      break;
    case Instruction::SHR_LONG:
    case Instruction::SHR_LONG_2ADDR:
      if (shift_amount == 32) {
        OpRegCopy(rl_result.reg.GetReg(), rl_src.reg.GetHighReg());
        OpRegCopy(rl_result.reg.GetHighReg(), rl_src.reg.GetHighReg());
        NewLIR2(kX86Sar32RI, rl_result.reg.GetHighReg(), 31);
      } else if (shift_amount > 31) {
        OpRegCopy(rl_result.reg.GetReg(), rl_src.reg.GetHighReg());
        OpRegCopy(rl_result.reg.GetHighReg(), rl_src.reg.GetHighReg());
        NewLIR2(kX86Sar32RI, rl_result.reg.GetReg(), shift_amount - 32);
        NewLIR2(kX86Sar32RI, rl_result.reg.GetHighReg(), 31);
      } else {
        OpRegCopy(rl_result.reg.GetReg(), rl_src.reg.GetReg());
        OpRegCopy(rl_result.reg.GetHighReg(), rl_src.reg.GetHighReg());
        NewLIR3(kX86Shrd32RRI, rl_result.reg.GetReg(), rl_result.reg.GetHighReg(), shift_amount);
        NewLIR2(kX86Sar32RI, rl_result.reg.GetHighReg(), shift_amount);
      }
      break;
    case Instruction::USHR_LONG:
    case Instruction::USHR_LONG_2ADDR:
      if (shift_amount == 32) {
        OpRegCopy(rl_result.reg.GetReg(), rl_src.reg.GetHighReg());
        LoadConstant(rl_result.reg.GetHighReg(), 0);
      } else if (shift_amount > 31) {
        OpRegCopy(rl_result.reg.GetReg(), rl_src.reg.GetHighReg());
        NewLIR2(kX86Shr32RI, rl_result.reg.GetReg(), shift_amount - 32);
        LoadConstant(rl_result.reg.GetHighReg(), 0);
      } else {
        OpRegCopy(rl_result.reg.GetReg(), rl_src.reg.GetReg());
        OpRegCopy(rl_result.reg.GetHighReg(), rl_src.reg.GetHighReg());
        NewLIR3(kX86Shrd32RRI, rl_result.reg.GetReg(), rl_result.reg.GetHighReg(), shift_amount);
        NewLIR2(kX86Shr32RI, rl_result.reg.GetHighReg(), shift_amount);
      }
      break;
    default:
      LOG(FATAL) << "Unexpected case";
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
  switch (opcode) {
    case Instruction::ADD_LONG:
    case Instruction::AND_LONG:
    case Instruction::OR_LONG:
    case Instruction::XOR_LONG:
      if (rl_src2.is_const) {
        GenLongLongImm(rl_dest, rl_src1, rl_src2, opcode);
      } else {
        DCHECK(rl_src1.is_const);
        GenLongLongImm(rl_dest, rl_src2, rl_src1, opcode);
      }
      break;
    case Instruction::SUB_LONG:
    case Instruction::SUB_LONG_2ADDR:
      if (rl_src2.is_const) {
        GenLongLongImm(rl_dest, rl_src1, rl_src2, opcode);
      } else {
        GenSubLong(opcode, rl_dest, rl_src1, rl_src2);
      }
      break;
    case Instruction::ADD_LONG_2ADDR:
    case Instruction::OR_LONG_2ADDR:
    case Instruction::XOR_LONG_2ADDR:
    case Instruction::AND_LONG_2ADDR:
      if (rl_src2.is_const) {
        GenLongImm(rl_dest, rl_src2, opcode);
      } else {
        DCHECK(rl_src1.is_const);
        GenLongLongImm(rl_dest, rl_src2, rl_src1, opcode);
      }
      break;
    default:
      // Default - bail to non-const handler.
      GenArithOpLong(opcode, rl_dest, rl_src1, rl_src2);
      break;
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
  DCHECK(!rhs_in_mem || !dest_in_mem);
  switch (op) {
    case Instruction::ADD_LONG:
    case Instruction::ADD_LONG_2ADDR:
      if (dest_in_mem) {
        return is_high_op ? kX86Adc32MR : kX86Add32MR;
      } else if (rhs_in_mem) {
        return is_high_op ? kX86Adc32RM : kX86Add32RM;
      }
      return is_high_op ? kX86Adc32RR : kX86Add32RR;
    case Instruction::SUB_LONG:
    case Instruction::SUB_LONG_2ADDR:
      if (dest_in_mem) {
        return is_high_op ? kX86Sbb32MR : kX86Sub32MR;
      } else if (rhs_in_mem) {
        return is_high_op ? kX86Sbb32RM : kX86Sub32RM;
      }
      return is_high_op ? kX86Sbb32RR : kX86Sub32RR;
    case Instruction::AND_LONG_2ADDR:
    case Instruction::AND_LONG:
      if (dest_in_mem) {
        return kX86And32MR;
      }
      return rhs_in_mem ? kX86And32RM : kX86And32RR;
    case Instruction::OR_LONG:
    case Instruction::OR_LONG_2ADDR:
      if (dest_in_mem) {
        return kX86Or32MR;
      }
      return rhs_in_mem ? kX86Or32RM : kX86Or32RR;
    case Instruction::XOR_LONG:
    case Instruction::XOR_LONG_2ADDR:
      if (dest_in_mem) {
        return kX86Xor32MR;
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
  bool byte_imm = IS_SIMM8(value);
  DCHECK(in_mem || !IsFpReg(loc.reg.GetReg()));
  switch (op) {
    case Instruction::ADD_LONG:
    case Instruction::ADD_LONG_2ADDR:
      if (byte_imm) {
        if (in_mem) {
          return is_high_op ? kX86Adc32MI8 : kX86Add32MI8;
        }
        return is_high_op ? kX86Adc32RI8 : kX86Add32RI8;
      }
      if (in_mem) {
        return is_high_op ? kX86Adc32MI : kX86Add32MI;
      }
      return is_high_op ? kX86Adc32RI : kX86Add32RI;
    case Instruction::SUB_LONG:
    case Instruction::SUB_LONG_2ADDR:
      if (byte_imm) {
        if (in_mem) {
          return is_high_op ? kX86Sbb32MI8 : kX86Sub32MI8;
        }
        return is_high_op ? kX86Sbb32RI8 : kX86Sub32RI8;
      }
      if (in_mem) {
        return is_high_op ? kX86Sbb32MI : kX86Sub32MI;
      }
      return is_high_op ? kX86Sbb32RI : kX86Sub32RI;
    case Instruction::AND_LONG_2ADDR:
    case Instruction::AND_LONG:
      if (byte_imm) {
        return in_mem ? kX86And32MI8 : kX86And32RI8;
      }
      return in_mem ? kX86And32MI : kX86And32RI;
    case Instruction::OR_LONG:
    case Instruction::OR_LONG_2ADDR:
      if (byte_imm) {
        return in_mem ? kX86Or32MI8 : kX86Or32RI8;
      }
      return in_mem ? kX86Or32MI : kX86Or32RI;
    case Instruction::XOR_LONG:
    case Instruction::XOR_LONG_2ADDR:
      if (byte_imm) {
        return in_mem ? kX86Xor32MI8 : kX86Xor32RI8;
      }
      return in_mem ? kX86Xor32MI : kX86Xor32RI;
    default:
      LOG(FATAL) << "Unexpected opcode: " << op;
      return kX86Add32MI;
  }
}

void X86Mir2Lir::GenLongImm(RegLocation rl_dest, RegLocation rl_src, Instruction::Code op) {
  DCHECK(rl_src.is_const);
  int64_t val = mir_graph_->ConstantValueWide(rl_src);
  int32_t val_lo = Low32Bits(val);
  int32_t val_hi = High32Bits(val);
  rl_dest = UpdateLocWide(rl_dest);

  // Can we just do this into memory?
  if ((rl_dest.location == kLocDalvikFrame) ||
      (rl_dest.location == kLocCompilerTemp)) {
    int rBase = TargetReg(kSp);
    int displacement = SRegOffset(rl_dest.s_reg_low);

    if (!IsNoOp(op, val_lo)) {
      X86OpCode x86op = GetOpcode(op, rl_dest, false, val_lo);
      LIR *lir = NewLIR3(x86op, rBase, displacement + LOWORD_OFFSET, val_lo);
      AnnotateDalvikRegAccess(lir, (displacement + LOWORD_OFFSET) >> 2,
                              false /* is_load */, true /* is64bit */);
    }
    if (!IsNoOp(op, val_hi)) {
      X86OpCode x86op = GetOpcode(op, rl_dest, true, val_hi);
      LIR *lir = NewLIR3(x86op, rBase, displacement + HIWORD_OFFSET, val_hi);
      AnnotateDalvikRegAccess(lir, (displacement + HIWORD_OFFSET) >> 2,
                                false /* is_load */, true /* is64bit */);
    }
    return;
  }

  RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  DCHECK_EQ(rl_result.location, kLocPhysReg);
  DCHECK(!IsFpReg(rl_result.reg.GetReg()));

  if (!IsNoOp(op, val_lo)) {
    X86OpCode x86op = GetOpcode(op, rl_result, false, val_lo);
    NewLIR2(x86op, rl_result.reg.GetReg(), val_lo);
  }
  if (!IsNoOp(op, val_hi)) {
    X86OpCode x86op = GetOpcode(op, rl_result, true, val_hi);
    NewLIR2(x86op, rl_result.reg.GetHighReg(), val_hi);
  }
  StoreValueWide(rl_dest, rl_result);
}

void X86Mir2Lir::GenLongLongImm(RegLocation rl_dest, RegLocation rl_src1,
                                RegLocation rl_src2, Instruction::Code op) {
  DCHECK(rl_src2.is_const);
  int64_t val = mir_graph_->ConstantValueWide(rl_src2);
  int32_t val_lo = Low32Bits(val);
  int32_t val_hi = High32Bits(val);
  rl_dest = UpdateLocWide(rl_dest);
  rl_src1 = UpdateLocWide(rl_src1);

  // Can we do this directly into the destination registers?
  if (rl_dest.location == kLocPhysReg && rl_src1.location == kLocPhysReg &&
      rl_dest.reg.GetReg() == rl_src1.reg.GetReg() && rl_dest.reg.GetHighReg() == rl_src1.reg.GetHighReg() &&
      !IsFpReg(rl_dest.reg.GetReg())) {
    if (!IsNoOp(op, val_lo)) {
      X86OpCode x86op = GetOpcode(op, rl_dest, false, val_lo);
      NewLIR2(x86op, rl_dest.reg.GetReg(), val_lo);
    }
    if (!IsNoOp(op, val_hi)) {
      X86OpCode x86op = GetOpcode(op, rl_dest, true, val_hi);
      NewLIR2(x86op, rl_dest.reg.GetHighReg(), val_hi);
    }

    StoreFinalValueWide(rl_dest, rl_dest);
    return;
  }

  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  DCHECK_EQ(rl_src1.location, kLocPhysReg);

  // We need the values to be in a temporary
  RegLocation rl_result = ForceTempWide(rl_src1);
  if (!IsNoOp(op, val_lo)) {
    X86OpCode x86op = GetOpcode(op, rl_result, false, val_lo);
    NewLIR2(x86op, rl_result.reg.GetReg(), val_lo);
  }
  if (!IsNoOp(op, val_hi)) {
    X86OpCode x86op = GetOpcode(op, rl_result, true, val_hi);
    NewLIR2(x86op, rl_result.reg.GetHighReg(), val_hi);
  }

  StoreFinalValueWide(rl_dest, rl_result);
}

// For final classes there are no sub-classes to check and so we can answer the instance-of
// question with simple comparisons. Use compares to memory and SETEQ to optimize for x86.
void X86Mir2Lir::GenInstanceofFinal(bool use_declaring_class, uint32_t type_idx,
                                    RegLocation rl_dest, RegLocation rl_src) {
  RegLocation object = LoadValue(rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  int result_reg = rl_result.reg.GetReg();

  // SETcc only works with EAX..EDX.
  if (result_reg == object.reg.GetReg() || result_reg >= 4) {
    result_reg = AllocTypedTemp(false, kCoreReg);
    DCHECK_LT(result_reg, 4);
  }

  // Assume that there is no match.
  LoadConstant(result_reg, 0);
  LIR* null_branchover = OpCmpImmBranch(kCondEq, object.reg.GetReg(), 0, NULL);

  int check_class = AllocTypedTemp(false, kCoreReg);

  // If Method* is already in a register, we can save a copy.
  RegLocation rl_method = mir_graph_->GetMethodLoc();
  int32_t offset_of_type = mirror::Array::DataOffset(sizeof(mirror::Class*)).Int32Value() +
    (sizeof(mirror::Class*) * type_idx);

  if (rl_method.location == kLocPhysReg) {
    if (use_declaring_class) {
      LoadWordDisp(rl_method.reg.GetReg(),
                   mirror::ArtMethod::DeclaringClassOffset().Int32Value(),
                   check_class);
    } else {
      LoadWordDisp(rl_method.reg.GetReg(),
                   mirror::ArtMethod::DexCacheResolvedTypesOffset().Int32Value(),
                   check_class);
      LoadWordDisp(check_class, offset_of_type, check_class);
    }
  } else {
    LoadCurrMethodDirect(check_class);
    if (use_declaring_class) {
      LoadWordDisp(check_class,
                   mirror::ArtMethod::DeclaringClassOffset().Int32Value(),
                   check_class);
    } else {
      LoadWordDisp(check_class,
                   mirror::ArtMethod::DexCacheResolvedTypesOffset().Int32Value(),
                   check_class);
      LoadWordDisp(check_class, offset_of_type, check_class);
    }
  }

  // Compare the computed class to the class in the object.
  DCHECK_EQ(object.location, kLocPhysReg);
  OpRegMem(kOpCmp, check_class, object.reg.GetReg(),
           mirror::Object::ClassOffset().Int32Value());

  // Set the low byte of the result to 0 or 1 from the compare condition code.
  NewLIR2(kX86Set8R, result_reg, kX86CondEq);

  LIR* target = NewLIR0(kPseudoTargetLabel);
  null_branchover->target = target;
  FreeTemp(check_class);
  if (IsTemp(result_reg)) {
    OpRegCopy(rl_result.reg.GetReg(), result_reg);
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
  LoadCurrMethodDirect(TargetReg(kArg1));  // kArg1 gets current Method*.
  int class_reg = TargetReg(kArg2);  // kArg2 will hold the Class*.
  // Reference must end up in kArg0.
  if (needs_access_check) {
    // Check we have access to type_idx and if not throw IllegalAccessError,
    // Caller function returns Class* in kArg0.
    CallRuntimeHelperImm(QUICK_ENTRYPOINT_OFFSET(pInitializeTypeAndVerifyAccess),
                         type_idx, true);
    OpRegCopy(class_reg, TargetReg(kRet0));
    LoadValueDirectFixed(rl_src, TargetReg(kArg0));
  } else if (use_declaring_class) {
    LoadValueDirectFixed(rl_src, TargetReg(kArg0));
    LoadWordDisp(TargetReg(kArg1),
                 mirror::ArtMethod::DeclaringClassOffset().Int32Value(), class_reg);
  } else {
    // Load dex cache entry into class_reg (kArg2).
    LoadValueDirectFixed(rl_src, TargetReg(kArg0));
    LoadWordDisp(TargetReg(kArg1),
                 mirror::ArtMethod::DexCacheResolvedTypesOffset().Int32Value(), class_reg);
    int32_t offset_of_type =
        mirror::Array::DataOffset(sizeof(mirror::Class*)).Int32Value() + (sizeof(mirror::Class*)
        * type_idx);
    LoadWordDisp(class_reg, offset_of_type, class_reg);
    if (!can_assume_type_is_in_dex_cache) {
      // Need to test presence of type in dex cache at runtime.
      LIR* hop_branch = OpCmpImmBranch(kCondNe, class_reg, 0, NULL);
      // Type is not resolved. Call out to helper, which will return resolved type in kRet0/kArg0.
      CallRuntimeHelperImm(QUICK_ENTRYPOINT_OFFSET(pInitializeType), type_idx, true);
      OpRegCopy(TargetReg(kArg2), TargetReg(kRet0));  // Align usage with fast path.
      LoadValueDirectFixed(rl_src, TargetReg(kArg0));  /* Reload Ref. */
      // Rejoin code paths
      LIR* hop_target = NewLIR0(kPseudoTargetLabel);
      hop_branch->target = hop_target;
    }
  }
  /* kArg0 is ref, kArg2 is class. If ref==null, use directly as bool result. */
  RegLocation rl_result = GetReturn(false);

  // SETcc only works with EAX..EDX.
  DCHECK_LT(rl_result.reg.GetReg(), 4);

  // Is the class NULL?
  LIR* branch1 = OpCmpImmBranch(kCondEq, TargetReg(kArg0), 0, NULL);

  /* Load object->klass_. */
  DCHECK_EQ(mirror::Object::ClassOffset().Int32Value(), 0);
  LoadWordDisp(TargetReg(kArg0),  mirror::Object::ClassOffset().Int32Value(), TargetReg(kArg1));
  /* kArg0 is ref, kArg1 is ref->klass_, kArg2 is class. */
  LIR* branchover = nullptr;
  if (type_known_final) {
    // Ensure top 3 bytes of result are 0.
    LoadConstant(rl_result.reg.GetReg(), 0);
    OpRegReg(kOpCmp, TargetReg(kArg1), TargetReg(kArg2));
    // Set the low byte of the result to 0 or 1 from the compare condition code.
    NewLIR2(kX86Set8R, rl_result.reg.GetReg(), kX86CondEq);
  } else {
    if (!type_known_abstract) {
      LoadConstant(rl_result.reg.GetReg(), 1);     // Assume result succeeds.
      branchover = OpCmpBranch(kCondEq, TargetReg(kArg1), TargetReg(kArg2), NULL);
    }
    OpRegCopy(TargetReg(kArg0), TargetReg(kArg2));
    OpThreadMem(kOpBlx, QUICK_ENTRYPOINT_OFFSET(pInstanceofNonTrivial));
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

  // Get the div/rem stuff out of the way.
  if (is_div_rem) {
    rl_result = GenDivRem(rl_dest, rl_lhs, rl_rhs, op == kOpDiv, true);
    StoreValue(rl_dest, rl_result);
    return;
  }

  if (unary) {
    rl_lhs = LoadValue(rl_lhs, kCoreReg);
    rl_result = UpdateLoc(rl_dest);
    rl_result = EvalLoc(rl_dest, kCoreReg, true);
    OpRegReg(op, rl_result.reg.GetReg(), rl_lhs.reg.GetReg());
  } else {
    if (shift_op) {
      // X86 doesn't require masking and must use ECX.
      int t_reg = TargetReg(kCount);  // rCX
      LoadValueDirectFixed(rl_rhs, t_reg);
      if (is_two_addr) {
        // Can we do this directly into memory?
        rl_result = UpdateLoc(rl_dest);
        rl_rhs = LoadValue(rl_rhs, kCoreReg);
        if (rl_result.location != kLocPhysReg) {
          // Okay, we can do this into memory
          OpMemReg(op, rl_result, t_reg);
          FreeTemp(t_reg);
          return;
        } else if (!IsFpReg(rl_result.reg.GetReg())) {
          // Can do this directly into the result register
          OpRegReg(op, rl_result.reg.GetReg(), t_reg);
          FreeTemp(t_reg);
          StoreFinalValue(rl_dest, rl_result);
          return;
        }
      }
      // Three address form, or we can't do directly.
      rl_lhs = LoadValue(rl_lhs, kCoreReg);
      rl_result = EvalLoc(rl_dest, kCoreReg, true);
      OpRegRegReg(op, rl_result.reg.GetReg(), rl_lhs.reg.GetReg(), t_reg);
      FreeTemp(t_reg);
    } else {
      // Multiply is 3 operand only (sort of).
      if (is_two_addr && op != kOpMul) {
        // Can we do this directly into memory?
        rl_result = UpdateLoc(rl_dest);
        if (rl_result.location == kLocPhysReg) {
          // Can we do this from memory directly?
          rl_rhs = UpdateLoc(rl_rhs);
          if (rl_rhs.location != kLocPhysReg) {
            OpRegMem(op, rl_result.reg.GetReg(), rl_rhs);
            StoreFinalValue(rl_dest, rl_result);
            return;
          } else if (!IsFpReg(rl_rhs.reg.GetReg())) {
            OpRegReg(op, rl_result.reg.GetReg(), rl_rhs.reg.GetReg());
            StoreFinalValue(rl_dest, rl_result);
            return;
          }
        }
        rl_rhs = LoadValue(rl_rhs, kCoreReg);
        if (rl_result.location != kLocPhysReg) {
          // Okay, we can do this into memory.
          OpMemReg(op, rl_result, rl_rhs.reg.GetReg());
          return;
        } else if (!IsFpReg(rl_result.reg.GetReg())) {
          // Can do this directly into the result register.
          OpRegReg(op, rl_result.reg.GetReg(), rl_rhs.reg.GetReg());
          StoreFinalValue(rl_dest, rl_result);
          return;
        } else {
          rl_lhs = LoadValue(rl_lhs, kCoreReg);
          rl_result = EvalLoc(rl_dest, kCoreReg, true);
          OpRegRegReg(op, rl_result.reg.GetReg(), rl_lhs.reg.GetReg(), rl_rhs.reg.GetReg());
        }
      } else {
        // Try to use reg/memory instructions.
        rl_lhs = UpdateLoc(rl_lhs);
        rl_rhs = UpdateLoc(rl_rhs);
        // We can't optimize with FP registers.
        if (!IsOperationSafeWithoutTemps(rl_lhs, rl_rhs)) {
          // Something is difficult, so fall back to the standard case.
          rl_lhs = LoadValue(rl_lhs, kCoreReg);
          rl_rhs = LoadValue(rl_rhs, kCoreReg);
          rl_result = EvalLoc(rl_dest, kCoreReg, true);
          OpRegRegReg(op, rl_result.reg.GetReg(), rl_lhs.reg.GetReg(), rl_rhs.reg.GetReg());
        } else {
          // We can optimize by moving to result and using memory operands.
          if (rl_rhs.location != kLocPhysReg) {
            // Force LHS into result.
            rl_result = EvalLoc(rl_dest, kCoreReg, true);
            LoadValueDirect(rl_lhs, rl_result.reg.GetReg());
            OpRegMem(op, rl_result.reg.GetReg(), rl_rhs);
          } else if (rl_lhs.location != kLocPhysReg) {
            // RHS is in a register; LHS is in memory.
            if (op != kOpSub) {
              // Force RHS into result and operate on memory.
              rl_result = EvalLoc(rl_dest, kCoreReg, true);
              OpRegCopy(rl_result.reg.GetReg(), rl_rhs.reg.GetReg());
              OpRegMem(op, rl_result.reg.GetReg(), rl_lhs);
            } else {
              // Subtraction isn't commutative.
              rl_lhs = LoadValue(rl_lhs, kCoreReg);
              rl_rhs = LoadValue(rl_rhs, kCoreReg);
              rl_result = EvalLoc(rl_dest, kCoreReg, true);
              OpRegRegReg(op, rl_result.reg.GetReg(), rl_lhs.reg.GetReg(), rl_rhs.reg.GetReg());
            }
          } else {
            // Both are in registers.
            rl_lhs = LoadValue(rl_lhs, kCoreReg);
            rl_rhs = LoadValue(rl_rhs, kCoreReg);
            rl_result = EvalLoc(rl_dest, kCoreReg, true);
            OpRegRegReg(op, rl_result.reg.GetReg(), rl_lhs.reg.GetReg(), rl_rhs.reg.GetReg());
          }
        }
      }
    }
  }
  StoreValue(rl_dest, rl_result);
}

bool X86Mir2Lir::IsOperationSafeWithoutTemps(RegLocation rl_lhs, RegLocation rl_rhs) {
  // If we have non-core registers, then we can't do good things.
  if (rl_lhs.location == kLocPhysReg && IsFpReg(rl_lhs.reg.GetReg())) {
    return false;
  }
  if (rl_rhs.location == kLocPhysReg && IsFpReg(rl_rhs.reg.GetReg())) {
    return false;
  }

  // Everything will be fine :-).
  return true;
}
}  // namespace art
