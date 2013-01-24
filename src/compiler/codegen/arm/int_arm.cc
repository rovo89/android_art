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

#include "oat_compilation_unit.h"
#include "oat/runtime/oat_support_entrypoints.h"
#include "arm_lir.h"
#include "codegen_arm.h"
#include "../codegen_util.h"
#include "../ralloc_util.h"

namespace art {

LIR* ArmCodegen::OpCmpBranch(CompilationUnit* cu, ConditionCode cond, int src1,
         int src2, LIR* target)
{
  OpRegReg(cu, kOpCmp, src1, src2);
  return OpCondBranch(cu, cond, target);
}

/*
 * Generate a Thumb2 IT instruction, which can nullify up to
 * four subsequent instructions based on a condition and its
 * inverse.  The condition applies to the first instruction, which
 * is executed if the condition is met.  The string "guide" consists
 * of 0 to 3 chars, and applies to the 2nd through 4th instruction.
 * A "T" means the instruction is executed if the condition is
 * met, and an "E" means the instruction is executed if the condition
 * is not met.
 */
LIR* ArmCodegen::OpIT(CompilationUnit* cu, ConditionCode ccode, const char* guide)
{
  int mask;
  int mask3 = 0;
  int mask2 = 0;
  int mask1 = 0;
  ArmConditionCode code = ArmConditionEncoding(ccode);
  int cond_bit = code & 1;
  int alt_bit = cond_bit ^ 1;

  //Note: case fallthroughs intentional
  switch (strlen(guide)) {
    case 3:
      mask1 = (guide[2] == 'T') ? cond_bit : alt_bit;
    case 2:
      mask2 = (guide[1] == 'T') ? cond_bit : alt_bit;
    case 1:
      mask3 = (guide[0] == 'T') ? cond_bit : alt_bit;
      break;
    case 0:
      break;
    default:
      LOG(FATAL) << "OAT: bad case in OpIT";
  }
  mask = (mask3 << 3) | (mask2 << 2) | (mask1 << 1) |
       (1 << (3 - strlen(guide)));
  return NewLIR2(cu, kThumb2It, code, mask);
}

/*
 * 64-bit 3way compare function.
 *     mov   rX, #-1
 *     cmp   op1hi, op2hi
 *     blt   done
 *     bgt   flip
 *     sub   rX, op1lo, op2lo (treat as unsigned)
 *     beq   done
 *     ite   hi
 *     mov(hi)   rX, #-1
 *     mov(!hi)  rX, #1
 * flip:
 *     neg   rX
 * done:
 */
void ArmCodegen::GenCmpLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2)
{
  LIR* target1;
  LIR* target2;
  rl_src1 = LoadValueWide(cu, rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(cu, rl_src2, kCoreReg);
  int t_reg = AllocTemp(cu);
  LoadConstant(cu, t_reg, -1);
  OpRegReg(cu, kOpCmp, rl_src1.high_reg, rl_src2.high_reg);
  LIR* branch1 = OpCondBranch(cu, kCondLt, NULL);
  LIR* branch2 = OpCondBranch(cu, kCondGt, NULL);
  OpRegRegReg(cu, kOpSub, t_reg, rl_src1.low_reg, rl_src2.low_reg);
  LIR* branch3 = OpCondBranch(cu, kCondEq, NULL);

  OpIT(cu, kCondHi, "E");
  NewLIR2(cu, kThumb2MovImmShift, t_reg, ModifiedImmediate(-1));
  LoadConstant(cu, t_reg, 1);
  GenBarrier(cu);

  target2 = NewLIR0(cu, kPseudoTargetLabel);
  OpRegReg(cu, kOpNeg, t_reg, t_reg);

  target1 = NewLIR0(cu, kPseudoTargetLabel);

  RegLocation rl_temp = LocCReturn(); // Just using as template, will change
  rl_temp.low_reg = t_reg;
  StoreValue(cu, rl_dest, rl_temp);
  FreeTemp(cu, t_reg);

  branch1->target = target1;
  branch2->target = target2;
  branch3->target = branch1->target;
}

void ArmCodegen::GenFusedLongCmpBranch(CompilationUnit* cu, BasicBlock* bb, MIR* mir)
{
  LIR* label_list = cu->block_label_list;
  LIR* taken = &label_list[bb->taken->id];
  LIR* not_taken = &label_list[bb->fall_through->id];
  RegLocation rl_src1 = GetSrcWide(cu, mir, 0);
  RegLocation rl_src2 = GetSrcWide(cu, mir, 2);
  rl_src1 = LoadValueWide(cu, rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(cu, rl_src2, kCoreReg);
  ConditionCode ccode = static_cast<ConditionCode>(mir->dalvikInsn.arg[0]);
  OpRegReg(cu, kOpCmp, rl_src1.high_reg, rl_src2.high_reg);
  switch(ccode) {
    case kCondEq:
      OpCondBranch(cu, kCondNe, not_taken);
      break;
    case kCondNe:
      OpCondBranch(cu, kCondNe, taken);
      break;
    case kCondLt:
      OpCondBranch(cu, kCondLt, taken);
      OpCondBranch(cu, kCondGt, not_taken);
      ccode = kCondCc;
      break;
    case kCondLe:
      OpCondBranch(cu, kCondLt, taken);
      OpCondBranch(cu, kCondGt, not_taken);
      ccode = kCondLs;
      break;
    case kCondGt:
      OpCondBranch(cu, kCondGt, taken);
      OpCondBranch(cu, kCondLt, not_taken);
      ccode = kCondHi;
      break;
    case kCondGe:
      OpCondBranch(cu, kCondGt, taken);
      OpCondBranch(cu, kCondLt, not_taken);
      ccode = kCondCs;
      break;
    default:
      LOG(FATAL) << "Unexpected ccode: " << ccode;
  }
  OpRegReg(cu, kOpCmp, rl_src1.low_reg, rl_src2.low_reg);
  OpCondBranch(cu, ccode, taken);
}

/*
 * Generate a register comparison to an immediate and branch.  Caller
 * is responsible for setting branch target field.
 */
LIR* ArmCodegen::OpCmpImmBranch(CompilationUnit* cu, ConditionCode cond, int reg, int check_value,
                                LIR* target)
{
  LIR* branch;
  int mod_imm;
  ArmConditionCode arm_cond = ArmConditionEncoding(cond);
  if ((ARM_LOWREG(reg)) && (check_value == 0) &&
     ((arm_cond == kArmCondEq) || (arm_cond == kArmCondNe))) {
    branch = NewLIR2(cu, (arm_cond == kArmCondEq) ? kThumb2Cbz : kThumb2Cbnz,
                     reg, 0);
  } else {
    mod_imm = ModifiedImmediate(check_value);
    if (ARM_LOWREG(reg) && ((check_value & 0xff) == check_value)) {
      NewLIR2(cu, kThumbCmpRI8, reg, check_value);
    } else if (mod_imm >= 0) {
      NewLIR2(cu, kThumb2CmpRI8, reg, mod_imm);
    } else {
      int t_reg = AllocTemp(cu);
      LoadConstant(cu, t_reg, check_value);
      OpRegReg(cu, kOpCmp, reg, t_reg);
    }
    branch = NewLIR2(cu, kThumbBCond, 0, arm_cond);
  }
  branch->target = target;
  return branch;
}

LIR* ArmCodegen::OpRegCopyNoInsert(CompilationUnit* cu, int r_dest, int r_src)
{
  LIR* res;
  int opcode;
  if (ARM_FPREG(r_dest) || ARM_FPREG(r_src))
    return OpFpRegCopy(cu, r_dest, r_src);
  if (ARM_LOWREG(r_dest) && ARM_LOWREG(r_src))
    opcode = kThumbMovRR;
  else if (!ARM_LOWREG(r_dest) && !ARM_LOWREG(r_src))
     opcode = kThumbMovRR_H2H;
  else if (ARM_LOWREG(r_dest))
     opcode = kThumbMovRR_H2L;
  else
     opcode = kThumbMovRR_L2H;
  res = RawLIR(cu, cu->current_dalvik_offset, opcode, r_dest, r_src);
  if (!(cu->disable_opt & (1 << kSafeOptimizations)) && r_dest == r_src) {
    res->flags.is_nop = true;
  }
  return res;
}

LIR* ArmCodegen::OpRegCopy(CompilationUnit* cu, int r_dest, int r_src)
{
  LIR* res = OpRegCopyNoInsert(cu, r_dest, r_src);
  AppendLIR(cu, res);
  return res;
}

void ArmCodegen::OpRegCopyWide(CompilationUnit* cu, int dest_lo, int dest_hi, int src_lo,
                               int src_hi)
{
  bool dest_fp = ARM_FPREG(dest_lo) && ARM_FPREG(dest_hi);
  bool src_fp = ARM_FPREG(src_lo) && ARM_FPREG(src_hi);
  DCHECK_EQ(ARM_FPREG(src_lo), ARM_FPREG(src_hi));
  DCHECK_EQ(ARM_FPREG(dest_lo), ARM_FPREG(dest_hi));
  if (dest_fp) {
    if (src_fp) {
      OpRegCopy(cu, S2d(dest_lo, dest_hi), S2d(src_lo, src_hi));
    } else {
      NewLIR3(cu, kThumb2Fmdrr, S2d(dest_lo, dest_hi), src_lo, src_hi);
    }
  } else {
    if (src_fp) {
      NewLIR3(cu, kThumb2Fmrrd, dest_lo, dest_hi, S2d(src_lo, src_hi));
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
bool ArmCodegen::SmallLiteralDivide(CompilationUnit* cu, Instruction::Code dalvik_opcode,
                                    RegLocation rl_src, RegLocation rl_dest, int lit)
{
  if ((lit < 0) || (lit >= static_cast<int>(sizeof(magic_table)/sizeof(magic_table[0])))) {
    return false;
  }
  DividePattern pattern = magic_table[lit].pattern;
  if (pattern == DivideNone) {
    return false;
  }
  // Tuning: add rem patterns
  if (dalvik_opcode != Instruction::DIV_INT_LIT8) {
    return false;
  }

  int r_magic = AllocTemp(cu);
  LoadConstant(cu, r_magic, magic_table[lit].magic);
  rl_src = LoadValue(cu, rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  int r_hi = AllocTemp(cu);
  int r_lo = AllocTemp(cu);
  NewLIR4(cu, kThumb2Smull, r_lo, r_hi, r_magic, rl_src.low_reg);
  switch(pattern) {
    case Divide3:
      OpRegRegRegShift(cu, kOpSub, rl_result.low_reg, r_hi,
               rl_src.low_reg, EncodeShift(kArmAsr, 31));
      break;
    case Divide5:
      OpRegRegImm(cu, kOpAsr, r_lo, rl_src.low_reg, 31);
      OpRegRegRegShift(cu, kOpRsub, rl_result.low_reg, r_lo, r_hi,
               EncodeShift(kArmAsr, magic_table[lit].shift));
      break;
    case Divide7:
      OpRegReg(cu, kOpAdd, r_hi, rl_src.low_reg);
      OpRegRegImm(cu, kOpAsr, r_lo, rl_src.low_reg, 31);
      OpRegRegRegShift(cu, kOpRsub, rl_result.low_reg, r_lo, r_hi,
               EncodeShift(kArmAsr, magic_table[lit].shift));
      break;
    default:
      LOG(FATAL) << "Unexpected pattern: " << pattern;
  }
  StoreValue(cu, rl_dest, rl_result);
  return true;
}

LIR* ArmCodegen::GenRegMemCheck(CompilationUnit* cu, ConditionCode c_code,
                    int reg1, int base, int offset, ThrowKind kind)
{
  LOG(FATAL) << "Unexpected use of GenRegMemCheck for Arm";
  return NULL;
}

RegLocation ArmCodegen::GenDivRemLit(CompilationUnit* cu, RegLocation rl_dest, int reg1, int lit,
                                     bool is_div)
{
  LOG(FATAL) << "Unexpected use of GenDivRemLit for Arm";
  return rl_dest;
}

RegLocation ArmCodegen::GenDivRem(CompilationUnit* cu, RegLocation rl_dest, int reg1, int reg2,
                                  bool is_div)
{
  LOG(FATAL) << "Unexpected use of GenDivRem for Arm";
  return rl_dest;
}

bool ArmCodegen::GenInlinedMinMaxInt(CompilationUnit *cu, CallInfo* info, bool is_min)
{
  DCHECK_EQ(cu->instruction_set, kThumb2);
  RegLocation rl_src1 = info->args[0];
  RegLocation rl_src2 = info->args[1];
  rl_src1 = LoadValue(cu, rl_src1, kCoreReg);
  rl_src2 = LoadValue(cu, rl_src2, kCoreReg);
  RegLocation rl_dest = InlineTarget(cu, info);
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  OpRegReg(cu, kOpCmp, rl_src1.low_reg, rl_src2.low_reg);
  OpIT(cu, (is_min) ? kCondGt : kCondLt, "E");
  OpRegReg(cu, kOpMov, rl_result.low_reg, rl_src2.low_reg);
  OpRegReg(cu, kOpMov, rl_result.low_reg, rl_src1.low_reg);
  GenBarrier(cu);
  StoreValue(cu, rl_dest, rl_result);
  return true;
}

void ArmCodegen::OpLea(CompilationUnit* cu, int rBase, int reg1, int reg2, int scale, int offset)
{
  LOG(FATAL) << "Unexpected use of OpLea for Arm";
}

void ArmCodegen::OpTlsCmp(CompilationUnit* cu, int offset, int val)
{
  LOG(FATAL) << "Unexpected use of OpTlsCmp for Arm";
}

bool ArmCodegen::GenInlinedCas32(CompilationUnit* cu, CallInfo* info, bool need_write_barrier) {
  DCHECK_EQ(cu->instruction_set, kThumb2);
  // Unused - RegLocation rl_src_unsafe = info->args[0];
  RegLocation rl_src_obj= info->args[1];  // Object - known non-null
  RegLocation rl_src_offset= info->args[2];  // long low
  rl_src_offset.wide = 0;  // ignore high half in info->args[3]
  RegLocation rl_src_expected= info->args[4];  // int or Object
  RegLocation rl_src_new_value= info->args[5];  // int or Object
  RegLocation rl_dest = InlineTarget(cu, info);  // boolean place for result


  // Release store semantics, get the barrier out of the way.  TODO: revisit
  GenMemBarrier(cu, kStoreLoad);

  RegLocation rl_object = LoadValue(cu, rl_src_obj, kCoreReg);
  RegLocation rl_new_value = LoadValue(cu, rl_src_new_value, kCoreReg);

  if (need_write_barrier) {
    // Mark card for object assuming new value is stored.
    MarkGCCard(cu, rl_new_value.low_reg, rl_object.low_reg);
  }

  RegLocation rl_offset = LoadValue(cu, rl_src_offset, kCoreReg);

  int r_ptr = AllocTemp(cu);
  OpRegRegReg(cu, kOpAdd, r_ptr, rl_object.low_reg, rl_offset.low_reg);

  // Free now unneeded rl_object and rl_offset to give more temps.
  ClobberSReg(cu, rl_object.s_reg_low);
  FreeTemp(cu, rl_object.low_reg);
  ClobberSReg(cu, rl_offset.s_reg_low);
  FreeTemp(cu, rl_offset.low_reg);

  int r_old_value = AllocTemp(cu);
  NewLIR3(cu, kThumb2Ldrex, r_old_value, r_ptr, 0);  // r_old_value := [r_ptr]

  RegLocation rl_expected = LoadValue(cu, rl_src_expected, kCoreReg);

  // if (r_old_value == rExpected) {
  //   [r_ptr] <- r_new_value && r_result := success ? 0 : 1
  //   r_result ^= 1
  // } else {
  //   r_result := 0
  // }
  OpRegReg(cu, kOpCmp, r_old_value, rl_expected.low_reg);
  FreeTemp(cu, r_old_value);  // Now unneeded.
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  OpIT(cu, kCondEq, "TE");
  NewLIR4(cu, kThumb2Strex, rl_result.low_reg, rl_new_value.low_reg, r_ptr, 0);
  FreeTemp(cu, r_ptr);  // Now unneeded.
  OpRegImm(cu, kOpXor, rl_result.low_reg, 1);
  OpRegReg(cu, kOpXor, rl_result.low_reg, rl_result.low_reg);

  StoreValue(cu, rl_dest, rl_result);

  return true;
}

LIR* ArmCodegen::OpPcRelLoad(CompilationUnit* cu, int reg, LIR* target)
{
  return RawLIR(cu, cu->current_dalvik_offset, kThumb2LdrPcRel12, reg, 0, 0, 0, 0, target);
}

LIR* ArmCodegen::OpVldm(CompilationUnit* cu, int rBase, int count)
{
  return NewLIR3(cu, kThumb2Vldms, rBase, fr0, count);
}

LIR* ArmCodegen::OpVstm(CompilationUnit* cu, int rBase, int count)
{
  return NewLIR3(cu, kThumb2Vstms, rBase, fr0, count);
}

void ArmCodegen::GenMultiplyByTwoBitMultiplier(CompilationUnit* cu, RegLocation rl_src,
                                               RegLocation rl_result, int lit,
                                               int first_bit, int second_bit)
{
  OpRegRegRegShift(cu, kOpAdd, rl_result.low_reg, rl_src.low_reg, rl_src.low_reg,
                   EncodeShift(kArmLsl, second_bit - first_bit));
  if (first_bit != 0) {
    OpRegRegImm(cu, kOpLsl, rl_result.low_reg, rl_result.low_reg, first_bit);
  }
}

void ArmCodegen::GenDivZeroCheck(CompilationUnit* cu, int reg_lo, int reg_hi)
{
  int t_reg = AllocTemp(cu);
  NewLIR4(cu, kThumb2OrrRRRs, t_reg, reg_lo, reg_hi, 0);
  FreeTemp(cu, t_reg);
  GenCheck(cu, kCondEq, kThrowDivZero);
}

// Test suspend flag, return target of taken suspend branch
LIR* ArmCodegen::OpTestSuspend(CompilationUnit* cu, LIR* target)
{
  NewLIR2(cu, kThumbSubRI8, rARM_SUSPEND, 1);
  return OpCondBranch(cu, (target == NULL) ? kCondEq : kCondNe, target);
}

// Decrement register and branch on condition
LIR* ArmCodegen::OpDecAndBranch(CompilationUnit* cu, ConditionCode c_code, int reg, LIR* target)
{
  // Combine sub & test using sub setflags encoding here
  NewLIR3(cu, kThumb2SubsRRI12, reg, reg, 1);
  return OpCondBranch(cu, c_code, target);
}

void ArmCodegen::GenMemBarrier(CompilationUnit* cu, MemBarrierKind barrier_kind)
{
#if ANDROID_SMP != 0
  int dmb_flavor;
  // TODO: revisit Arm barrier kinds
  switch (barrier_kind) {
    case kLoadStore: dmb_flavor = kSY; break;
    case kLoadLoad: dmb_flavor = kSY; break;
    case kStoreStore: dmb_flavor = kST; break;
    case kStoreLoad: dmb_flavor = kSY; break;
    default:
      LOG(FATAL) << "Unexpected MemBarrierKind: " << barrier_kind;
      dmb_flavor = kSY;  // quiet gcc.
      break;
  }
  LIR* dmb = NewLIR1(cu, kThumb2Dmb, dmb_flavor);
  dmb->def_mask = ENCODE_ALL;
#endif
}

bool ArmCodegen::GenNegLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src)
{
  rl_src = LoadValueWide(cu, rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  int z_reg = AllocTemp(cu);
  LoadConstantNoClobber(cu, z_reg, 0);
  // Check for destructive overlap
  if (rl_result.low_reg == rl_src.high_reg) {
    int t_reg = AllocTemp(cu);
    OpRegRegReg(cu, kOpSub, rl_result.low_reg, z_reg, rl_src.low_reg);
    OpRegRegReg(cu, kOpSbc, rl_result.high_reg, z_reg, t_reg);
    FreeTemp(cu, t_reg);
  } else {
    OpRegRegReg(cu, kOpSub, rl_result.low_reg, z_reg, rl_src.low_reg);
    OpRegRegReg(cu, kOpSbc, rl_result.high_reg, z_reg, rl_src.high_reg);
  }
  FreeTemp(cu, z_reg);
  StoreValueWide(cu, rl_dest, rl_result);
  return false;
}

bool ArmCodegen::GenAddLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2)
{
  LOG(FATAL) << "Unexpected use of GenAddLong for Arm";
  return false;
}

bool ArmCodegen::GenSubLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2)
{
  LOG(FATAL) << "Unexpected use of GenSubLong for Arm";
  return false;
}

bool ArmCodegen::GenAndLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2)
{
  LOG(FATAL) << "Unexpected use of GenAndLong for Arm";
  return false;
}

bool ArmCodegen::GenOrLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                           RegLocation rl_src2)
{
  LOG(FATAL) << "Unexpected use of GenOrLong for Arm";
  return false;
}

bool ArmCodegen::GenXorLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2)
{
  LOG(FATAL) << "Unexpected use of genXoLong for Arm";
  return false;
}

/*
 * Generate array load
 */
void ArmCodegen::GenArrayGet(CompilationUnit* cu, int opt_flags, OpSize size, RegLocation rl_array,
                          RegLocation rl_index, RegLocation rl_dest, int scale)
{
  RegisterClass reg_class = oat_reg_class_by_size(size);
  int len_offset = mirror::Array::LengthOffset().Int32Value();
  int data_offset;
  RegLocation rl_result;
  rl_array = LoadValue(cu, rl_array, kCoreReg);
  rl_index = LoadValue(cu, rl_index, kCoreReg);

  if (rl_dest.wide) {
    data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Int32Value();
  } else {
    data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Int32Value();
  }

  /* null object? */
  GenNullCheck(cu, rl_array.s_reg_low, rl_array.low_reg, opt_flags);

  bool needs_range_check = (!(opt_flags & MIR_IGNORE_RANGE_CHECK));
  int reg_len = INVALID_REG;
  if (needs_range_check) {
    reg_len = AllocTemp(cu);
    /* Get len */
    LoadWordDisp(cu, rl_array.low_reg, len_offset, reg_len);
  }
  if (rl_dest.wide || rl_dest.fp) {
    // No special indexed operation, lea + load w/ displacement
    int reg_ptr = AllocTemp(cu);
    OpRegRegRegShift(cu, kOpAdd, reg_ptr, rl_array.low_reg, rl_index.low_reg,
                     EncodeShift(kArmLsl, scale));
    FreeTemp(cu, rl_index.low_reg);
    rl_result = EvalLoc(cu, rl_dest, reg_class, true);

    if (needs_range_check) {
      // TODO: change kCondCS to a more meaningful name, is the sense of
      // carry-set/clear flipped?
      GenRegRegCheck(cu, kCondCs, rl_index.low_reg, reg_len, kThrowArrayBounds);
      FreeTemp(cu, reg_len);
    }
    if (rl_dest.wide) {
      LoadBaseDispWide(cu, reg_ptr, data_offset, rl_result.low_reg, rl_result.high_reg, INVALID_SREG);
      FreeTemp(cu, reg_ptr);
      StoreValueWide(cu, rl_dest, rl_result);
    } else {
      LoadBaseDisp(cu, reg_ptr, data_offset, rl_result.low_reg, size, INVALID_SREG);
      FreeTemp(cu, reg_ptr);
      StoreValue(cu, rl_dest, rl_result);
    }
  } else {
    // Offset base, then use indexed load
    int reg_ptr = AllocTemp(cu);
    OpRegRegImm(cu, kOpAdd, reg_ptr, rl_array.low_reg, data_offset);
    FreeTemp(cu, rl_array.low_reg);
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
void ArmCodegen::GenArrayPut(CompilationUnit* cu, int opt_flags, OpSize size, RegLocation rl_array,
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
  /* at this point, reg_ptr points to array, 2 live temps */
  if (rl_src.wide || rl_src.fp) {
    if (rl_src.wide) {
      rl_src = LoadValueWide(cu, rl_src, reg_class);
    } else {
      rl_src = LoadValue(cu, rl_src, reg_class);
    }
    OpRegRegRegShift(cu, kOpAdd, reg_ptr, rl_array.low_reg, rl_index.low_reg,
                     EncodeShift(kArmLsl, scale));
    if (needs_range_check) {
      GenRegRegCheck(cu, kCondCs, rl_index.low_reg, reg_len, kThrowArrayBounds);
      FreeTemp(cu, reg_len);
    }
    if (rl_src.wide) {
      StoreBaseDispWide(cu, reg_ptr, data_offset, rl_src.low_reg, rl_src.high_reg);
    } else {
      StoreBaseDisp(cu, reg_ptr, data_offset, rl_src.low_reg, size);
    }
  } else {
    /* reg_ptr -> array data */
    OpRegRegImm(cu, kOpAdd, reg_ptr, rl_array.low_reg, data_offset);
    rl_src = LoadValue(cu, rl_src, reg_class);
    if (needs_range_check) {
      GenRegRegCheck(cu, kCondCs, rl_index.low_reg, reg_len, kThrowArrayBounds);
      FreeTemp(cu, reg_len);
    }
    StoreBaseIndexed(cu, reg_ptr, rl_index.low_reg, rl_src.low_reg,
                     scale, size);
  }
  FreeTemp(cu, reg_ptr);
}

/*
 * Generate array store
 *
 */
void ArmCodegen::GenArrayObjPut(CompilationUnit* cu, int opt_flags, RegLocation rl_array,
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
