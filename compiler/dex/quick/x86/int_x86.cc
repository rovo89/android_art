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
      NewLIR2(kX86PsllqRI, dest_hi, 32);
      NewLIR2(kX86OrpsRR, dest_lo, dest_hi);
      FreeTemp(dest_hi);
    }
  } else {
    if (src_fp) {
      NewLIR2(kX86MovdrxRR, dest_lo, src_lo);
      NewLIR2(kX86PsrlqRI, src_lo, 32);
      NewLIR2(kX86MovdrxRR, dest_hi, src_lo);
    } else {
      // Handle overlap
      if (src_hi == dest_lo) {
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
  UNIMPLEMENTED(FATAL) << "Need codegen for GenSelect";
}

void X86Mir2Lir::GenFusedLongCmpBranch(BasicBlock* bb, MIR* mir) {
  LIR* taken = &block_label_list_[bb->taken];
  RegLocation rl_src1 = mir_graph_->GetSrcWide(mir, 0);
  RegLocation rl_src2 = mir_graph_->GetSrcWide(mir, 2);
  ConditionCode ccode = static_cast<ConditionCode>(mir->dalvikInsn.arg[0]);

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
  int32_t low_reg = rl_src1.low_reg;
  int32_t high_reg = rl_src1.high_reg;

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

RegLocation X86Mir2Lir::GenDivRemLit(RegLocation rl_dest, int reg_lo,
                                     int lit, bool is_div) {
  LOG(FATAL) << "Unexpected use of GenDivRemLit for x86";
  return rl_dest;
}

RegLocation X86Mir2Lir::GenDivRem(RegLocation rl_dest, int reg_lo,
                                  int reg_hi, bool is_div) {
  LOG(FATAL) << "Unexpected use of GenDivRem for x86";
  return rl_dest;
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
  if (rl_result.low_reg == rl_src2.low_reg) {
    std::swap(rl_src1, rl_src2);
  }

  // Pick the first integer as min/max.
  OpRegCopy(rl_result.low_reg, rl_src1.low_reg);

  // If the integers are both in the same register, then there is nothing else to do
  // because they are equal and we have already moved one into the result.
  if (rl_src1.low_reg != rl_src2.low_reg) {
    // It is possible we didn't pick correctly so do the actual comparison now.
    OpRegReg(kOpCmp, rl_src1.low_reg, rl_src2.low_reg);

    // Conditionally move the other integer into the destination register.
    ConditionCode condition_code = is_min ? kCondGt : kCondLt;
    OpCondRegReg(kOpCmov, condition_code, rl_result.low_reg, rl_src2.low_reg);
  }

  StoreValue(rl_dest, rl_result);
  return true;
}

bool X86Mir2Lir::GenInlinedPeek(CallInfo* info, OpSize size) {
  RegLocation rl_src_address = info->args[0];  // long address
  rl_src_address.wide = 0;  // ignore high half in info->args[1]
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_address = LoadValue(rl_src_address, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (size == kLong) {
    // Unaligned access is allowed on x86.
    LoadBaseDispWide(rl_address.low_reg, 0, rl_result.low_reg, rl_result.high_reg, INVALID_SREG);
    StoreValueWide(rl_dest, rl_result);
  } else {
    DCHECK(size == kSignedByte || size == kSignedHalf || size == kWord);
    // Unaligned access is allowed on x86.
    LoadBaseDisp(rl_address.low_reg, 0, rl_result.low_reg, size, INVALID_SREG);
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
    StoreBaseDispWide(rl_address.low_reg, 0, rl_value.low_reg, rl_value.high_reg);
  } else {
    DCHECK(size == kSignedByte || size == kSignedHalf || size == kWord);
    // Unaligned access is allowed on x86.
    RegLocation rl_value = LoadValue(rl_src_value, kCoreReg);
    StoreBaseDisp(rl_address.low_reg, 0, rl_value.low_reg, size);
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
      MarkGCCard(rl_new_value.low_reg, rl_object.low_reg);
      LockTemp(r0);
    }

    RegLocation rl_offset = LoadValue(rl_src_offset, kCoreReg);
    LoadValueDirect(rl_src_expected, r0);
    NewLIR5(kX86LockCmpxchgAR, rl_object.low_reg, rl_offset.low_reg, 0, 0, rl_new_value.low_reg);

    FreeTemp(r0);
  }

  // Convert ZF to boolean
  RegLocation rl_dest = InlineTarget(info);  // boolean place for result
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  NewLIR2(kX86Set8R, rl_result.low_reg, kX86CondZ);
  NewLIR2(kX86Movzx8RR, rl_result.low_reg, rl_result.low_reg);
  StoreValue(rl_dest, rl_result);
  return true;
}

LIR* X86Mir2Lir::OpPcRelLoad(int reg, LIR* target) {
  LOG(FATAL) << "Unexpected use of OpPcRelLoad for x86";
  return NULL;
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
  OpRegRegImm(kOpLsl, t_reg, rl_src.low_reg, second_bit - first_bit);
  OpRegRegReg(kOpAdd, rl_result.low_reg, rl_src.low_reg, t_reg);
  FreeTemp(t_reg);
  if (first_bit != 0) {
    OpRegRegImm(kOpLsl, rl_result.low_reg, rl_result.low_reg, first_bit);
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

void X86Mir2Lir::GenMulLong(RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) {
  LOG(FATAL) << "Unexpected use of GenX86Long for x86";
}
void X86Mir2Lir::GenAddLong(RegLocation rl_dest, RegLocation rl_src1,
                         RegLocation rl_src2) {
  // TODO: fixed register usage here as we only have 4 temps and temporary allocation isn't smart
  // enough.
  FlushAllRegs();
  LockCallTemps();  // Prepare for explicit register usage
  LoadValueDirectWideFixed(rl_src1, r0, r1);
  LoadValueDirectWideFixed(rl_src2, r2, r3);
  // Compute (r1:r0) = (r1:r0) + (r2:r3)
  OpRegReg(kOpAdd, r0, r2);  // r0 = r0 + r2
  OpRegReg(kOpAdc, r1, r3);  // r1 = r1 + r3 + CF
  RegLocation rl_result = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, kVectorNotUsed, r0, r1,
                          INVALID_SREG, INVALID_SREG};
  StoreValueWide(rl_dest, rl_result);
}

void X86Mir2Lir::GenSubLong(RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) {
  // TODO: fixed register usage here as we only have 4 temps and temporary allocation isn't smart
  // enough.
  FlushAllRegs();
  LockCallTemps();  // Prepare for explicit register usage
  LoadValueDirectWideFixed(rl_src1, r0, r1);
  LoadValueDirectWideFixed(rl_src2, r2, r3);
  // Compute (r1:r0) = (r1:r0) + (r2:r3)
  OpRegReg(kOpSub, r0, r2);  // r0 = r0 - r2
  OpRegReg(kOpSbc, r1, r3);  // r1 = r1 - r3 - CF
  RegLocation rl_result = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, kVectorNotUsed, r0, r1,
                          INVALID_SREG, INVALID_SREG};
  StoreValueWide(rl_dest, rl_result);
}

void X86Mir2Lir::GenAndLong(RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) {
  // TODO: fixed register usage here as we only have 4 temps and temporary allocation isn't smart
  // enough.
  FlushAllRegs();
  LockCallTemps();  // Prepare for explicit register usage
  LoadValueDirectWideFixed(rl_src1, r0, r1);
  LoadValueDirectWideFixed(rl_src2, r2, r3);
  // Compute (r1:r0) = (r1:r0) & (r2:r3)
  OpRegReg(kOpAnd, r0, r2);  // r0 = r0 & r2
  OpRegReg(kOpAnd, r1, r3);  // r1 = r1 & r3
  RegLocation rl_result = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, kVectorNotUsed, r0, r1,
                          INVALID_SREG, INVALID_SREG};
  StoreValueWide(rl_dest, rl_result);
}

void X86Mir2Lir::GenOrLong(RegLocation rl_dest,
                           RegLocation rl_src1, RegLocation rl_src2) {
  // TODO: fixed register usage here as we only have 4 temps and temporary allocation isn't smart
  // enough.
  FlushAllRegs();
  LockCallTemps();  // Prepare for explicit register usage
  LoadValueDirectWideFixed(rl_src1, r0, r1);
  LoadValueDirectWideFixed(rl_src2, r2, r3);
  // Compute (r1:r0) = (r1:r0) | (r2:r3)
  OpRegReg(kOpOr, r0, r2);  // r0 = r0 | r2
  OpRegReg(kOpOr, r1, r3);  // r1 = r1 | r3
  RegLocation rl_result = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, kVectorNotUsed, r0, r1,
                          INVALID_SREG, INVALID_SREG};
  StoreValueWide(rl_dest, rl_result);
}

void X86Mir2Lir::GenXorLong(RegLocation rl_dest,
                            RegLocation rl_src1, RegLocation rl_src2) {
  // TODO: fixed register usage here as we only have 4 temps and temporary allocation isn't smart
  // enough.
  FlushAllRegs();
  LockCallTemps();  // Prepare for explicit register usage
  LoadValueDirectWideFixed(rl_src1, r0, r1);
  LoadValueDirectWideFixed(rl_src2, r2, r3);
  // Compute (r1:r0) = (r1:r0) ^ (r2:r3)
  OpRegReg(kOpXor, r0, r2);  // r0 = r0 ^ r2
  OpRegReg(kOpXor, r1, r3);  // r1 = r1 ^ r3
  RegLocation rl_result = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, kVectorNotUsed, r0, r1,
                          INVALID_SREG, INVALID_SREG};
  StoreValueWide(rl_dest, rl_result);
}

void X86Mir2Lir::GenNegLong(RegLocation rl_dest, RegLocation rl_src) {
  FlushAllRegs();
  LockCallTemps();  // Prepare for explicit register usage
  LoadValueDirectWideFixed(rl_src, r0, r1);
  // Compute (r1:r0) = -(r1:r0)
  OpRegReg(kOpNeg, r0, r0);  // r0 = -r0
  OpRegImm(kOpAdc, r1, 0);   // r1 = r1 + CF
  OpRegReg(kOpNeg, r1, r1);  // r1 = -r1
  RegLocation rl_result = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, kVectorNotUsed, r0, r1,
                          INVALID_SREG, INVALID_SREG};
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
    rl_index.low_reg = INVALID_REG;
  }

  /* null object? */
  GenNullCheck(rl_array.s_reg_low, rl_array.low_reg, opt_flags);

  if (!(opt_flags & MIR_IGNORE_RANGE_CHECK)) {
    if (constant_index) {
      GenMemImmedCheck(kCondLs, rl_array.low_reg, len_offset,
                       constant_index_value, kThrowConstantArrayBounds);
    } else {
      GenRegMemCheck(kCondUge, rl_index.low_reg, rl_array.low_reg,
                     len_offset, kThrowArrayBounds);
    }
  }
  rl_result = EvalLoc(rl_dest, reg_class, true);
  if ((size == kLong) || (size == kDouble)) {
    LoadBaseIndexedDisp(rl_array.low_reg, rl_index.low_reg, scale, data_offset, rl_result.low_reg,
                        rl_result.high_reg, size, INVALID_SREG);
    StoreValueWide(rl_dest, rl_result);
  } else {
    LoadBaseIndexedDisp(rl_array.low_reg, rl_index.low_reg, scale,
                        data_offset, rl_result.low_reg, INVALID_REG, size,
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
    rl_index.low_reg = INVALID_REG;
  }

  /* null object? */
  GenNullCheck(rl_array.s_reg_low, rl_array.low_reg, opt_flags);

  if (!(opt_flags & MIR_IGNORE_RANGE_CHECK)) {
    if (constant_index) {
      GenMemImmedCheck(kCondLs, rl_array.low_reg, len_offset,
                       constant_index_value, kThrowConstantArrayBounds);
    } else {
      GenRegMemCheck(kCondUge, rl_index.low_reg, rl_array.low_reg,
                     len_offset, kThrowArrayBounds);
    }
  }
  if ((size == kLong) || (size == kDouble)) {
    rl_src = LoadValueWide(rl_src, reg_class);
  } else {
    rl_src = LoadValue(rl_src, reg_class);
  }
  // If the src reg can't be byte accessed, move it to a temp first.
  if ((size == kSignedByte || size == kUnsignedByte) && rl_src.low_reg >= 4) {
    int temp = AllocTemp();
    OpRegCopy(temp, rl_src.low_reg);
    StoreBaseIndexedDisp(rl_array.low_reg, rl_index.low_reg, scale, data_offset, temp,
                         INVALID_REG, size, INVALID_SREG);
  } else {
    StoreBaseIndexedDisp(rl_array.low_reg, rl_index.low_reg, scale, data_offset, rl_src.low_reg,
                         rl_src.high_reg, size, INVALID_SREG);
  }
  if (card_mark) {
    // Free rl_index if its a temp. Ensures there are 2 free regs for card mark.
    if (!constant_index) {
      FreeTemp(rl_index.low_reg);
    }
    MarkGCCard(rl_src.low_reg, rl_array.low_reg);
  }
}

void X86Mir2Lir::GenShiftImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                   RegLocation rl_src1, RegLocation rl_shift) {
  // Default implementation is just to ignore the constant case.
  GenShiftOpLong(opcode, rl_dest, rl_src1, rl_shift);
}

void X86Mir2Lir::GenArithImmOpLong(Instruction::Code opcode,
                                   RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) {
  // Default - bail to non-const handler.
  GenArithOpLong(opcode, rl_dest, rl_src1, rl_src2);
}

}  // namespace art
