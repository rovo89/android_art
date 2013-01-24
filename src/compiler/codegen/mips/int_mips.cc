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

/* This file contains codegen for the Mips ISA */

#include "oat/runtime/oat_support_entrypoints.h"
#include "mips_lir.h"
#include "codegen_mips.h"
#include "../codegen_util.h"
#include "../ralloc_util.h"

namespace art {

/*
 * Compare two 64-bit values
 *    x = y     return  0
 *    x < y     return -1
 *    x > y     return  1
 *
 *    slt   t0,  x.hi, y.hi;        # (x.hi < y.hi) ? 1:0
 *    sgt   t1,  x.hi, y.hi;        # (y.hi > x.hi) ? 1:0
 *    subu  res, t0, t1             # res = -1:1:0 for [ < > = ]
 *    bnez  res, finish
 *    sltu  t0, x.lo, y.lo
 *    sgtu  r1, x.lo, y.lo
 *    subu  res, t0, t1
 * finish:
 *
 */
void MipsCodegen::GenCmpLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                             RegLocation rl_src2)
{
  rl_src1 = LoadValueWide(cu, rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(cu, rl_src2, kCoreReg);
  int t0 = AllocTemp(cu);
  int t1 = AllocTemp(cu);
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  NewLIR3(cu, kMipsSlt, t0, rl_src1.high_reg, rl_src2.high_reg);
  NewLIR3(cu, kMipsSlt, t1, rl_src2.high_reg, rl_src1.high_reg);
  NewLIR3(cu, kMipsSubu, rl_result.low_reg, t1, t0);
  LIR* branch = OpCmpImmBranch(cu, kCondNe, rl_result.low_reg, 0, NULL);
  NewLIR3(cu, kMipsSltu, t0, rl_src1.low_reg, rl_src2.low_reg);
  NewLIR3(cu, kMipsSltu, t1, rl_src2.low_reg, rl_src1.low_reg);
  NewLIR3(cu, kMipsSubu, rl_result.low_reg, t1, t0);
  FreeTemp(cu, t0);
  FreeTemp(cu, t1);
  LIR* target = NewLIR0(cu, kPseudoTargetLabel);
  branch->target = target;
  StoreValue(cu, rl_dest, rl_result);
}

LIR* MipsCodegen::OpCmpBranch(CompilationUnit* cu, ConditionCode cond, int src1, int src2,
                              LIR* target)
{
  LIR* branch;
  MipsOpCode slt_op;
  MipsOpCode br_op;
  bool cmp_zero = false;
  bool swapped = false;
  switch (cond) {
    case kCondEq:
      br_op = kMipsBeq;
      cmp_zero = true;
      break;
    case kCondNe:
      br_op = kMipsBne;
      cmp_zero = true;
      break;
    case kCondCc:
      slt_op = kMipsSltu;
      br_op = kMipsBnez;
      break;
    case kCondCs:
      slt_op = kMipsSltu;
      br_op = kMipsBeqz;
      break;
    case kCondGe:
      slt_op = kMipsSlt;
      br_op = kMipsBeqz;
      break;
    case kCondGt:
      slt_op = kMipsSlt;
      br_op = kMipsBnez;
      swapped = true;
      break;
    case kCondLe:
      slt_op = kMipsSlt;
      br_op = kMipsBeqz;
      swapped = true;
      break;
    case kCondLt:
      slt_op = kMipsSlt;
      br_op = kMipsBnez;
      break;
    case kCondHi:  // Gtu
      slt_op = kMipsSltu;
      br_op = kMipsBnez;
      swapped = true;
      break;
    default:
      LOG(FATAL) << "No support for ConditionCode: " << cond;
      return NULL;
  }
  if (cmp_zero) {
    branch = NewLIR2(cu, br_op, src1, src2);
  } else {
    int t_reg = AllocTemp(cu);
    if (swapped) {
      NewLIR3(cu, slt_op, t_reg, src2, src1);
    } else {
      NewLIR3(cu, slt_op, t_reg, src1, src2);
    }
    branch = NewLIR1(cu, br_op, t_reg);
    FreeTemp(cu, t_reg);
  }
  branch->target = target;
  return branch;
}

LIR* MipsCodegen::OpCmpImmBranch(CompilationUnit* cu, ConditionCode cond, int reg,
                                 int check_value, LIR* target)
{
  LIR* branch;
  if (check_value != 0) {
    // TUNING: handle s16 & kCondLt/Mi case using slti
    int t_reg = AllocTemp(cu);
    LoadConstant(cu, t_reg, check_value);
    branch = OpCmpBranch(cu, cond, reg, t_reg, target);
    FreeTemp(cu, t_reg);
    return branch;
  }
  MipsOpCode opc;
  switch (cond) {
    case kCondEq: opc = kMipsBeqz; break;
    case kCondGe: opc = kMipsBgez; break;
    case kCondGt: opc = kMipsBgtz; break;
    case kCondLe: opc = kMipsBlez; break;
    //case KCondMi:
    case kCondLt: opc = kMipsBltz; break;
    case kCondNe: opc = kMipsBnez; break;
    default:
      // Tuning: use slti when applicable
      int t_reg = AllocTemp(cu);
      LoadConstant(cu, t_reg, check_value);
      branch = OpCmpBranch(cu, cond, reg, t_reg, target);
      FreeTemp(cu, t_reg);
      return branch;
  }
  branch = NewLIR1(cu, opc, reg);
  branch->target = target;
  return branch;
}

LIR* MipsCodegen::OpRegCopyNoInsert(CompilationUnit *cu, int r_dest, int r_src)
{
  if (MIPS_FPREG(r_dest) || MIPS_FPREG(r_src))
    return OpFpRegCopy(cu, r_dest, r_src);
  LIR* res = RawLIR(cu, cu->current_dalvik_offset, kMipsMove,
            r_dest, r_src);
  if (!(cu->disable_opt & (1 << kSafeOptimizations)) && r_dest == r_src) {
    res->flags.is_nop = true;
  }
  return res;
}

LIR* MipsCodegen::OpRegCopy(CompilationUnit *cu, int r_dest, int r_src)
{
  LIR *res = OpRegCopyNoInsert(cu, r_dest, r_src);
  AppendLIR(cu, res);
  return res;
}

void MipsCodegen::OpRegCopyWide(CompilationUnit *cu, int dest_lo, int dest_hi, int src_lo,
                                int src_hi)
{
  bool dest_fp = MIPS_FPREG(dest_lo) && MIPS_FPREG(dest_hi);
  bool src_fp = MIPS_FPREG(src_lo) && MIPS_FPREG(src_hi);
  assert(MIPS_FPREG(src_lo) == MIPS_FPREG(src_hi));
  assert(MIPS_FPREG(dest_lo) == MIPS_FPREG(dest_hi));
  if (dest_fp) {
    if (src_fp) {
      OpRegCopy(cu, S2d(dest_lo, dest_hi), S2d(src_lo, src_hi));
    } else {
       /* note the operands are swapped for the mtc1 instr */
      NewLIR2(cu, kMipsMtc1, src_lo, dest_lo);
      NewLIR2(cu, kMipsMtc1, src_hi, dest_hi);
    }
  } else {
    if (src_fp) {
      NewLIR2(cu, kMipsMfc1, dest_lo, src_lo);
      NewLIR2(cu, kMipsMfc1, dest_hi, src_hi);
    } else {
      // Handle overlap
      if (src_hi == dest_lo) {
        OpRegCopy(cu, dest_hi, src_hi);
        OpRegCopy(cu, dest_lo, src_lo);
      } else {
        OpRegCopy(cu, dest_lo, src_lo);
        OpRegCopy(cu, dest_hi, src_hi);
      }
    }
  }
}

void MipsCodegen::GenFusedLongCmpBranch(CompilationUnit* cu, BasicBlock* bb, MIR* mir)
{
  UNIMPLEMENTED(FATAL) << "Need codegen for fused long cmp branch";
}

LIR* MipsCodegen::GenRegMemCheck(CompilationUnit* cu, ConditionCode c_code,
                    int reg1, int base, int offset, ThrowKind kind)
{
  LOG(FATAL) << "Unexpected use of GenRegMemCheck for Arm";
  return NULL;
}

RegLocation MipsCodegen::GenDivRem(CompilationUnit* cu, RegLocation rl_dest, int reg1, int reg2,
                                    bool is_div)
{
  NewLIR4(cu, kMipsDiv, r_HI, r_LO, reg1, reg2);
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  if (is_div) {
    NewLIR2(cu, kMipsMflo, rl_result.low_reg, r_LO);
  } else {
    NewLIR2(cu, kMipsMfhi, rl_result.low_reg, r_HI);
  }
  return rl_result;
}

RegLocation MipsCodegen::GenDivRemLit(CompilationUnit* cu, RegLocation rl_dest, int reg1, int lit,
                                       bool is_div)
{
  int t_reg = AllocTemp(cu);
  NewLIR3(cu, kMipsAddiu, t_reg, r_ZERO, lit);
  NewLIR4(cu, kMipsDiv, r_HI, r_LO, reg1, t_reg);
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  if (is_div) {
    NewLIR2(cu, kMipsMflo, rl_result.low_reg, r_LO);
  } else {
    NewLIR2(cu, kMipsMfhi, rl_result.low_reg, r_HI);
  }
  FreeTemp(cu, t_reg);
  return rl_result;
}

void MipsCodegen::OpLea(CompilationUnit* cu, int rBase, int reg1, int reg2, int scale, int offset)
{
  LOG(FATAL) << "Unexpected use of OpLea for Arm";
}

void MipsCodegen::OpTlsCmp(CompilationUnit* cu, int offset, int val)
{
  LOG(FATAL) << "Unexpected use of OpTlsCmp for Arm";
}

bool MipsCodegen::GenInlinedCas32(CompilationUnit* cu, CallInfo* info, bool need_write_barrier) {
  DCHECK_NE(cu->instruction_set, kThumb2);
  return false;
}

bool MipsCodegen::GenInlinedSqrt(CompilationUnit* cu, CallInfo* info) {
  DCHECK_NE(cu->instruction_set, kThumb2);
  return false;
}

LIR* MipsCodegen::OpPcRelLoad(CompilationUnit* cu, int reg, LIR* target) {
  LOG(FATAL) << "Unexpected use of OpPcRelLoad for Mips";
  return NULL;
}

LIR* MipsCodegen::OpVldm(CompilationUnit* cu, int rBase, int count)
{
  LOG(FATAL) << "Unexpected use of OpVldm for Mips";
  return NULL;
}

LIR* MipsCodegen::OpVstm(CompilationUnit* cu, int rBase, int count)
{
  LOG(FATAL) << "Unexpected use of OpVstm for Mips";
  return NULL;
}

void MipsCodegen::GenMultiplyByTwoBitMultiplier(CompilationUnit* cu, RegLocation rl_src,
                                                RegLocation rl_result, int lit,
                                                int first_bit, int second_bit)
{
  int t_reg = AllocTemp(cu);
  OpRegRegImm(cu, kOpLsl, t_reg, rl_src.low_reg, second_bit - first_bit);
  OpRegRegReg(cu, kOpAdd, rl_result.low_reg, rl_src.low_reg, t_reg);
  FreeTemp(cu, t_reg);
  if (first_bit != 0) {
    OpRegRegImm(cu, kOpLsl, rl_result.low_reg, rl_result.low_reg, first_bit);
  }
}

void MipsCodegen::GenDivZeroCheck(CompilationUnit* cu, int reg_lo, int reg_hi)
{
  int t_reg = AllocTemp(cu);
  OpRegRegReg(cu, kOpOr, t_reg, reg_lo, reg_hi);
  GenImmedCheck(cu, kCondEq, t_reg, 0, kThrowDivZero);
  FreeTemp(cu, t_reg);
}

// Test suspend flag, return target of taken suspend branch
LIR* MipsCodegen::OpTestSuspend(CompilationUnit* cu, LIR* target)
{
  OpRegImm(cu, kOpSub, rMIPS_SUSPEND, 1);
  return OpCmpImmBranch(cu, (target == NULL) ? kCondEq : kCondNe, rMIPS_SUSPEND, 0, target);
}

// Decrement register and branch on condition
LIR* MipsCodegen::OpDecAndBranch(CompilationUnit* cu, ConditionCode c_code, int reg, LIR* target)
{
  OpRegImm(cu, kOpSub, reg, 1);
  return OpCmpImmBranch(cu, c_code, reg, 0, target);
}

bool MipsCodegen::SmallLiteralDivide(CompilationUnit* cu, Instruction::Code dalvik_opcode,
                                     RegLocation rl_src, RegLocation rl_dest, int lit)
{
  LOG(FATAL) << "Unexpected use of smallLiteralDive in Mips";
  return false;
}

LIR* MipsCodegen::OpIT(CompilationUnit* cu, ConditionCode cond, const char* guide)
{
  LOG(FATAL) << "Unexpected use of OpIT in Mips";
  return NULL;
}

bool MipsCodegen::GenAddLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                             RegLocation rl_src2)
{
  rl_src1 = LoadValueWide(cu, rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(cu, rl_src2, kCoreReg);
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  /*
   *  [v1 v0] =  [a1 a0] + [a3 a2];
   *  addu v0,a2,a0
   *  addu t1,a3,a1
   *  sltu v1,v0,a2
   *  addu v1,v1,t1
   */

  OpRegRegReg(cu, kOpAdd, rl_result.low_reg, rl_src2.low_reg, rl_src1.low_reg);
  int t_reg = AllocTemp(cu);
  OpRegRegReg(cu, kOpAdd, t_reg, rl_src2.high_reg, rl_src1.high_reg);
  NewLIR3(cu, kMipsSltu, rl_result.high_reg, rl_result.low_reg, rl_src2.low_reg);
  OpRegRegReg(cu, kOpAdd, rl_result.high_reg, rl_result.high_reg, t_reg);
  FreeTemp(cu, t_reg);
  StoreValueWide(cu, rl_dest, rl_result);
  return false;
}

bool MipsCodegen::GenSubLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                             RegLocation rl_src2)
{
  rl_src1 = LoadValueWide(cu, rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(cu, rl_src2, kCoreReg);
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  /*
   *  [v1 v0] =  [a1 a0] - [a3 a2];
   *  sltu  t1,a0,a2
   *  subu  v0,a0,a2
   *  subu  v1,a1,a3
   *  subu  v1,v1,t1
   */

  int t_reg = AllocTemp(cu);
  NewLIR3(cu, kMipsSltu, t_reg, rl_src1.low_reg, rl_src2.low_reg);
  OpRegRegReg(cu, kOpSub, rl_result.low_reg, rl_src1.low_reg, rl_src2.low_reg);
  OpRegRegReg(cu, kOpSub, rl_result.high_reg, rl_src1.high_reg, rl_src2.high_reg);
  OpRegRegReg(cu, kOpSub, rl_result.high_reg, rl_result.high_reg, t_reg);
  FreeTemp(cu, t_reg);
  StoreValueWide(cu, rl_dest, rl_result);
  return false;
}

bool MipsCodegen::GenNegLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
{
  rl_src = LoadValueWide(cu, rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  /*
   *  [v1 v0] =  -[a1 a0]
   *  negu  v0,a0
   *  negu  v1,a1
   *  sltu  t1,r_zero
   *  subu  v1,v1,t1
   */

  OpRegReg(cu, kOpNeg, rl_result.low_reg, rl_src.low_reg);
  OpRegReg(cu, kOpNeg, rl_result.high_reg, rl_src.high_reg);
  int t_reg = AllocTemp(cu);
  NewLIR3(cu, kMipsSltu, t_reg, r_ZERO, rl_result.low_reg);
  OpRegRegReg(cu, kOpSub, rl_result.high_reg, rl_result.high_reg, t_reg);
  FreeTemp(cu, t_reg);
  StoreValueWide(cu, rl_dest, rl_result);
  return false;
}

bool MipsCodegen::GenAndLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                             RegLocation rl_src2)
{
  LOG(FATAL) << "Unexpected use of GenAndLong for Mips";
  return false;
}

bool MipsCodegen::GenOrLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2)
{
  LOG(FATAL) << "Unexpected use of GenOrLong for Mips";
  return false;
}

bool MipsCodegen::GenXorLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                             RegLocation rl_src2)
{
  LOG(FATAL) << "Unexpected use of GenXorLong for Mips";
  return false;
}

/*
 * Generate array load
 */
void MipsCodegen::GenArrayGet(CompilationUnit* cu, int opt_flags, OpSize size, RegLocation rl_array,
                          RegLocation rl_index, RegLocation rl_dest, int scale)
{
  RegisterClass reg_class = oat_reg_class_by_size(size);
  int len_offset = mirror::Array::LengthOffset().Int32Value();
  int data_offset;
  RegLocation rl_result;
  rl_array = LoadValue(cu, rl_array, kCoreReg);
  rl_index = LoadValue(cu, rl_index, kCoreReg);

  if (size == kLong || size == kDouble) {
    data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Int32Value();
  } else {
    data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Int32Value();
  }

  /* null object? */
  GenNullCheck(cu, rl_array.s_reg_low, rl_array.low_reg, opt_flags);

  int reg_ptr = AllocTemp(cu);
  bool needs_range_check = (!(opt_flags & MIR_IGNORE_RANGE_CHECK));
  int reg_len = INVALID_REG;
  if (needs_range_check) {
    reg_len = AllocTemp(cu);
    /* Get len */
    LoadWordDisp(cu, rl_array.low_reg, len_offset, reg_len);
  }
  /* reg_ptr -> array data */
  OpRegRegImm(cu, kOpAdd, reg_ptr, rl_array.low_reg, data_offset);
  FreeTemp(cu, rl_array.low_reg);
  if ((size == kLong) || (size == kDouble)) {
    if (scale) {
      int r_new_index = AllocTemp(cu);
      OpRegRegImm(cu, kOpLsl, r_new_index, rl_index.low_reg, scale);
      OpRegReg(cu, kOpAdd, reg_ptr, r_new_index);
      FreeTemp(cu, r_new_index);
    } else {
      OpRegReg(cu, kOpAdd, reg_ptr, rl_index.low_reg);
    }
    FreeTemp(cu, rl_index.low_reg);
    rl_result = EvalLoc(cu, rl_dest, reg_class, true);

    if (needs_range_check) {
      // TODO: change kCondCS to a more meaningful name, is the sense of
      // carry-set/clear flipped?
      GenRegRegCheck(cu, kCondCs, rl_index.low_reg, reg_len, kThrowArrayBounds);
      FreeTemp(cu, reg_len);
    }
    LoadBaseDispWide(cu, reg_ptr, 0, rl_result.low_reg, rl_result.high_reg, INVALID_SREG);

    FreeTemp(cu, reg_ptr);
    StoreValueWide(cu, rl_dest, rl_result);
  } else {
    rl_result = EvalLoc(cu, rl_dest, reg_class, true);

    if (needs_range_check) {
      // TODO: change kCondCS to a more meaningful name, is the sense of
      // carry-set/clear flipped?
      GenRegRegCheck(cu, kCondCs, rl_index.low_reg, reg_len, kThrowArrayBounds);
      FreeTemp(cu, reg_len);
    }
    LoadBaseIndexed(cu, reg_ptr, rl_index.low_reg, rl_result.low_reg, scale, size);

    FreeTemp(cu, reg_ptr);
    StoreValue(cu, rl_dest, rl_result);
  }
}

/*
 * Generate array store
 *
 */
void MipsCodegen::GenArrayPut(CompilationUnit* cu, int opt_flags, OpSize size, RegLocation rl_array,
                          RegLocation rl_index, RegLocation rl_src, int scale)
{
  RegisterClass reg_class = oat_reg_class_by_size(size);
  int len_offset = mirror::Array::LengthOffset().Int32Value();
  int data_offset;

  if (size == kLong || size == kDouble) {
    data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Int32Value();
  } else {
    data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Int32Value();
  }

  rl_array = LoadValue(cu, rl_array, kCoreReg);
  rl_index = LoadValue(cu, rl_index, kCoreReg);
  int reg_ptr = INVALID_REG;
  if (IsTemp(cu, rl_array.low_reg)) {
    Clobber(cu, rl_array.low_reg);
    reg_ptr = rl_array.low_reg;
  } else {
    reg_ptr = AllocTemp(cu);
    OpRegCopy(cu, reg_ptr, rl_array.low_reg);
  }

  /* null object? */
  GenNullCheck(cu, rl_array.s_reg_low, rl_array.low_reg, opt_flags);

  bool needs_range_check = (!(opt_flags & MIR_IGNORE_RANGE_CHECK));
  int reg_len = INVALID_REG;
  if (needs_range_check) {
    reg_len = AllocTemp(cu);
    //NOTE: max live temps(4) here.
    /* Get len */
    LoadWordDisp(cu, rl_array.low_reg, len_offset, reg_len);
  }
  /* reg_ptr -> array data */
  OpRegImm(cu, kOpAdd, reg_ptr, data_offset);
  /* at this point, reg_ptr points to array, 2 live temps */
  if ((size == kLong) || (size == kDouble)) {
    //TUNING: specific wide routine that can handle fp regs
    if (scale) {
      int r_new_index = AllocTemp(cu);
      OpRegRegImm(cu, kOpLsl, r_new_index, rl_index.low_reg, scale);
      OpRegReg(cu, kOpAdd, reg_ptr, r_new_index);
      FreeTemp(cu, r_new_index);
    } else {
      OpRegReg(cu, kOpAdd, reg_ptr, rl_index.low_reg);
    }
    rl_src = LoadValueWide(cu, rl_src, reg_class);

    if (needs_range_check) {
      GenRegRegCheck(cu, kCondCs, rl_index.low_reg, reg_len, kThrowArrayBounds);
      FreeTemp(cu, reg_len);
    }

    StoreBaseDispWide(cu, reg_ptr, 0, rl_src.low_reg, rl_src.high_reg);

    FreeTemp(cu, reg_ptr);
  } else {
    rl_src = LoadValue(cu, rl_src, reg_class);
    if (needs_range_check) {
      GenRegRegCheck(cu, kCondCs, rl_index.low_reg, reg_len, kThrowArrayBounds);
      FreeTemp(cu, reg_len);
    }
    StoreBaseIndexed(cu, reg_ptr, rl_index.low_reg, rl_src.low_reg,
                     scale, size);
  }
}

/*
 * Generate array store
 *
 */
void MipsCodegen::GenArrayObjPut(CompilationUnit* cu, int opt_flags, RegLocation rl_array,
                             RegLocation rl_index, RegLocation rl_src, int scale)
{
  int len_offset = mirror::Array::LengthOffset().Int32Value();
  int data_offset = mirror::Array::DataOffset(sizeof(mirror::Object*)).Int32Value();

  FlushAllRegs(cu);  // Use explicit registers
  LockCallTemps(cu);

  int r_value = TargetReg(kArg0);  // Register holding value
  int r_array_class = TargetReg(kArg1);  // Register holding array's Class
  int r_array = TargetReg(kArg2);  // Register holding array
  int r_index = TargetReg(kArg3);  // Register holding index into array

  LoadValueDirectFixed(cu, rl_array, r_array);  // Grab array
  LoadValueDirectFixed(cu, rl_src, r_value);  // Grab value
  LoadValueDirectFixed(cu, rl_index, r_index);  // Grab index

  GenNullCheck(cu, rl_array.s_reg_low, r_array, opt_flags);  // NPE?

  // Store of null?
  LIR* null_value_check = OpCmpImmBranch(cu, kCondEq, r_value, 0, NULL);

  // Get the array's class.
  LoadWordDisp(cu, r_array, mirror::Object::ClassOffset().Int32Value(), r_array_class);
  CallRuntimeHelperRegReg(cu, ENTRYPOINT_OFFSET(pCanPutArrayElementFromCode), r_value,
                          r_array_class, true);
  // Redo LoadValues in case they didn't survive the call.
  LoadValueDirectFixed(cu, rl_array, r_array);  // Reload array
  LoadValueDirectFixed(cu, rl_index, r_index);  // Reload index
  LoadValueDirectFixed(cu, rl_src, r_value);  // Reload value
  r_array_class = INVALID_REG;

  // Branch here if value to be stored == null
  LIR* target = NewLIR0(cu, kPseudoTargetLabel);
  null_value_check->target = target;

  bool needs_range_check = (!(opt_flags & MIR_IGNORE_RANGE_CHECK));
  int reg_len = INVALID_REG;
  if (needs_range_check) {
    reg_len = TargetReg(kArg1);
    LoadWordDisp(cu, r_array, len_offset, reg_len);  // Get len
  }
  /* r_ptr -> array data */
  int r_ptr = AllocTemp(cu);
  OpRegRegImm(cu, kOpAdd, r_ptr, r_array, data_offset);
  if (needs_range_check) {
    GenRegRegCheck(cu, kCondCs, r_index, reg_len, kThrowArrayBounds);
  }
  StoreBaseIndexed(cu, r_ptr, r_index, r_value, scale, kWord);
  FreeTemp(cu, r_ptr);
  FreeTemp(cu, r_index);
  MarkGCCard(cu, r_value, r_array);
}

}  // namespace art
