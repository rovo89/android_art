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

#include "arm_lir.h"
#include "codegen_arm.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "mirror/array.h"

namespace art {

LIR* ArmMir2Lir::OpCmpBranch(ConditionCode cond, int src1, int src2, LIR* target) {
  OpRegReg(kOpCmp, src1, src2);
  return OpCondBranch(cond, target);
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
LIR* ArmMir2Lir::OpIT(ConditionCode ccode, const char* guide) {
  int mask;
  int mask3 = 0;
  int mask2 = 0;
  int mask1 = 0;
  ArmConditionCode code = ArmConditionEncoding(ccode);
  int cond_bit = code & 1;
  int alt_bit = cond_bit ^ 1;

  // Note: case fallthroughs intentional
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
  return NewLIR2(kThumb2It, code, mask);
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
void ArmMir2Lir::GenCmpLong(RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) {
  LIR* target1;
  LIR* target2;
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  int t_reg = AllocTemp();
  LoadConstant(t_reg, -1);
  OpRegReg(kOpCmp, rl_src1.reg.GetHighReg(), rl_src2.reg.GetHighReg());
  LIR* branch1 = OpCondBranch(kCondLt, NULL);
  LIR* branch2 = OpCondBranch(kCondGt, NULL);
  OpRegRegReg(kOpSub, t_reg, rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  LIR* branch3 = OpCondBranch(kCondEq, NULL);

  OpIT(kCondHi, "E");
  NewLIR2(kThumb2MovI8M, t_reg, ModifiedImmediate(-1));
  LoadConstant(t_reg, 1);
  GenBarrier();

  target2 = NewLIR0(kPseudoTargetLabel);
  OpRegReg(kOpNeg, t_reg, t_reg);

  target1 = NewLIR0(kPseudoTargetLabel);

  RegLocation rl_temp = LocCReturn();  // Just using as template, will change
  rl_temp.reg.SetReg(t_reg);
  StoreValue(rl_dest, rl_temp);
  FreeTemp(t_reg);

  branch1->target = target1;
  branch2->target = target2;
  branch3->target = branch1->target;
}

void ArmMir2Lir::GenFusedLongCmpImmBranch(BasicBlock* bb, RegLocation rl_src1,
                                          int64_t val, ConditionCode ccode) {
  int32_t val_lo = Low32Bits(val);
  int32_t val_hi = High32Bits(val);
  DCHECK_GE(ModifiedImmediate(val_lo), 0);
  DCHECK_GE(ModifiedImmediate(val_hi), 0);
  LIR* taken = &block_label_list_[bb->taken];
  LIR* not_taken = &block_label_list_[bb->fall_through];
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  int32_t low_reg = rl_src1.reg.GetReg();
  int32_t high_reg = rl_src1.reg.GetHighReg();

  if (val == 0 && (ccode == kCondEq || ccode == kCondNe)) {
    int t_reg = AllocTemp();
    NewLIR4(kThumb2OrrRRRs, t_reg, low_reg, high_reg, 0);
    FreeTemp(t_reg);
    OpCondBranch(ccode, taken);
    return;
  }

  switch (ccode) {
    case kCondEq:
    case kCondNe:
      OpCmpImmBranch(kCondNe, high_reg, val_hi, (ccode == kCondEq) ? not_taken : taken);
      break;
    case kCondLt:
      OpCmpImmBranch(kCondLt, high_reg, val_hi, taken);
      OpCmpImmBranch(kCondGt, high_reg, val_hi, not_taken);
      ccode = kCondUlt;
      break;
    case kCondLe:
      OpCmpImmBranch(kCondLt, high_reg, val_hi, taken);
      OpCmpImmBranch(kCondGt, high_reg, val_hi, not_taken);
      ccode = kCondLs;
      break;
    case kCondGt:
      OpCmpImmBranch(kCondGt, high_reg, val_hi, taken);
      OpCmpImmBranch(kCondLt, high_reg, val_hi, not_taken);
      ccode = kCondHi;
      break;
    case kCondGe:
      OpCmpImmBranch(kCondGt, high_reg, val_hi, taken);
      OpCmpImmBranch(kCondLt, high_reg, val_hi, not_taken);
      ccode = kCondUge;
      break;
    default:
      LOG(FATAL) << "Unexpected ccode: " << ccode;
  }
  OpCmpImmBranch(ccode, low_reg, val_lo, taken);
}

void ArmMir2Lir::GenSelect(BasicBlock* bb, MIR* mir) {
  RegLocation rl_result;
  RegLocation rl_src = mir_graph_->GetSrc(mir, 0);
  RegLocation rl_dest = mir_graph_->GetDest(mir);
  rl_src = LoadValue(rl_src, kCoreReg);
  if (mir->ssa_rep->num_uses == 1) {
    // CONST case
    int true_val = mir->dalvikInsn.vB;
    int false_val = mir->dalvikInsn.vC;
    rl_result = EvalLoc(rl_dest, kCoreReg, true);
    if ((true_val == 1) && (false_val == 0)) {
      OpRegRegImm(kOpRsub, rl_result.reg.GetReg(), rl_src.reg.GetReg(), 1);
      OpIT(kCondUlt, "");
      LoadConstant(rl_result.reg.GetReg(), 0);
      GenBarrier();  // Add a scheduling barrier to keep the IT shadow intact
    } else if (InexpensiveConstantInt(true_val) && InexpensiveConstantInt(false_val)) {
      OpRegImm(kOpCmp, rl_src.reg.GetReg(), 0);
      OpIT(kCondEq, "E");
      LoadConstant(rl_result.reg.GetReg(), true_val);
      LoadConstant(rl_result.reg.GetReg(), false_val);
      GenBarrier();  // Add a scheduling barrier to keep the IT shadow intact
    } else {
      // Unlikely case - could be tuned.
      int t_reg1 = AllocTemp();
      int t_reg2 = AllocTemp();
      LoadConstant(t_reg1, true_val);
      LoadConstant(t_reg2, false_val);
      OpRegImm(kOpCmp, rl_src.reg.GetReg(), 0);
      OpIT(kCondEq, "E");
      OpRegCopy(rl_result.reg.GetReg(), t_reg1);
      OpRegCopy(rl_result.reg.GetReg(), t_reg2);
      GenBarrier();  // Add a scheduling barrier to keep the IT shadow intact
    }
  } else {
    // MOVE case
    RegLocation rl_true = mir_graph_->reg_location_[mir->ssa_rep->uses[1]];
    RegLocation rl_false = mir_graph_->reg_location_[mir->ssa_rep->uses[2]];
    rl_true = LoadValue(rl_true, kCoreReg);
    rl_false = LoadValue(rl_false, kCoreReg);
    rl_result = EvalLoc(rl_dest, kCoreReg, true);
    OpRegImm(kOpCmp, rl_src.reg.GetReg(), 0);
    if (rl_result.reg.GetReg() == rl_true.reg.GetReg()) {  // Is the "true" case already in place?
      OpIT(kCondNe, "");
      OpRegCopy(rl_result.reg.GetReg(), rl_false.reg.GetReg());
    } else if (rl_result.reg.GetReg() == rl_false.reg.GetReg()) {  // False case in place?
      OpIT(kCondEq, "");
      OpRegCopy(rl_result.reg.GetReg(), rl_true.reg.GetReg());
    } else {  // Normal - select between the two.
      OpIT(kCondEq, "E");
      OpRegCopy(rl_result.reg.GetReg(), rl_true.reg.GetReg());
      OpRegCopy(rl_result.reg.GetReg(), rl_false.reg.GetReg());
    }
    GenBarrier();  // Add a scheduling barrier to keep the IT shadow intact
  }
  StoreValue(rl_dest, rl_result);
}

void ArmMir2Lir::GenFusedLongCmpBranch(BasicBlock* bb, MIR* mir) {
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
    if ((rl_temp.location != kLocPhysReg) &&
        ((ModifiedImmediate(Low32Bits(val)) >= 0) && (ModifiedImmediate(High32Bits(val)) >= 0))) {
      GenFusedLongCmpImmBranch(bb, rl_src1, val, ccode);
      return;
    }
  }
  LIR* taken = &block_label_list_[bb->taken];
  LIR* not_taken = &block_label_list_[bb->fall_through];
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  OpRegReg(kOpCmp, rl_src1.reg.GetHighReg(), rl_src2.reg.GetHighReg());
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
  OpRegReg(kOpCmp, rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  OpCondBranch(ccode, taken);
}

/*
 * Generate a register comparison to an immediate and branch.  Caller
 * is responsible for setting branch target field.
 */
LIR* ArmMir2Lir::OpCmpImmBranch(ConditionCode cond, int reg, int check_value,
                                LIR* target) {
  LIR* branch;
  ArmConditionCode arm_cond = ArmConditionEncoding(cond);
  /*
   * A common use of OpCmpImmBranch is for null checks, and using the Thumb 16-bit
   * compare-and-branch if zero is ideal if it will reach.  However, because null checks
   * branch forward to a launch pad, they will frequently not reach - and thus have to
   * be converted to a long form during assembly (which will trigger another assembly
   * pass).  Here we estimate the branch distance for checks, and if large directly
   * generate the long form in an attempt to avoid an extra assembly pass.
   * TODO: consider interspersing launchpads in code following unconditional branches.
   */
  bool skip = ((target != NULL) && (target->opcode == kPseudoThrowTarget));
  skip &= ((cu_->code_item->insns_size_in_code_units_ - current_dalvik_offset_) > 64);
  if (!skip && (ARM_LOWREG(reg)) && (check_value == 0) &&
     ((arm_cond == kArmCondEq) || (arm_cond == kArmCondNe))) {
    branch = NewLIR2((arm_cond == kArmCondEq) ? kThumb2Cbz : kThumb2Cbnz,
                     reg, 0);
  } else {
    OpRegImm(kOpCmp, reg, check_value);
    branch = NewLIR2(kThumbBCond, 0, arm_cond);
  }
  branch->target = target;
  return branch;
}

LIR* ArmMir2Lir::OpRegCopyNoInsert(int r_dest, int r_src) {
  LIR* res;
  int opcode;
  if (ARM_FPREG(r_dest) || ARM_FPREG(r_src))
    return OpFpRegCopy(r_dest, r_src);
  if (ARM_LOWREG(r_dest) && ARM_LOWREG(r_src))
    opcode = kThumbMovRR;
  else if (!ARM_LOWREG(r_dest) && !ARM_LOWREG(r_src))
     opcode = kThumbMovRR_H2H;
  else if (ARM_LOWREG(r_dest))
     opcode = kThumbMovRR_H2L;
  else
     opcode = kThumbMovRR_L2H;
  res = RawLIR(current_dalvik_offset_, opcode, r_dest, r_src);
  if (!(cu_->disable_opt & (1 << kSafeOptimizations)) && r_dest == r_src) {
    res->flags.is_nop = true;
  }
  return res;
}

LIR* ArmMir2Lir::OpRegCopy(int r_dest, int r_src) {
  LIR* res = OpRegCopyNoInsert(r_dest, r_src);
  AppendLIR(res);
  return res;
}

void ArmMir2Lir::OpRegCopyWide(int dest_lo, int dest_hi, int src_lo,
                               int src_hi) {
  bool dest_fp = ARM_FPREG(dest_lo) && ARM_FPREG(dest_hi);
  bool src_fp = ARM_FPREG(src_lo) && ARM_FPREG(src_hi);
  DCHECK_EQ(ARM_FPREG(src_lo), ARM_FPREG(src_hi));
  DCHECK_EQ(ARM_FPREG(dest_lo), ARM_FPREG(dest_hi));
  if (dest_fp) {
    if (src_fp) {
      OpRegCopy(S2d(dest_lo, dest_hi), S2d(src_lo, src_hi));
    } else {
      NewLIR3(kThumb2Fmdrr, S2d(dest_lo, dest_hi), src_lo, src_hi);
    }
  } else {
    if (src_fp) {
      NewLIR3(kThumb2Fmrrd, dest_lo, dest_hi, S2d(src_lo, src_hi));
    } else {
      // Handle overlap
      if (src_hi == dest_lo) {
        DCHECK_NE(src_lo, dest_hi);
        OpRegCopy(dest_hi, src_hi);
        OpRegCopy(dest_lo, src_lo);
      } else {
        OpRegCopy(dest_lo, src_lo);
        OpRegCopy(dest_hi, src_hi);
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
bool ArmMir2Lir::SmallLiteralDivRem(Instruction::Code dalvik_opcode, bool is_div,
                                    RegLocation rl_src, RegLocation rl_dest, int lit) {
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

  int r_magic = AllocTemp();
  LoadConstant(r_magic, magic_table[lit].magic);
  rl_src = LoadValue(rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  int r_hi = AllocTemp();
  int r_lo = AllocTemp();
  NewLIR4(kThumb2Smull, r_lo, r_hi, r_magic, rl_src.reg.GetReg());
  switch (pattern) {
    case Divide3:
      OpRegRegRegShift(kOpSub, rl_result.reg.GetReg(), r_hi,
               rl_src.reg.GetReg(), EncodeShift(kArmAsr, 31));
      break;
    case Divide5:
      OpRegRegImm(kOpAsr, r_lo, rl_src.reg.GetReg(), 31);
      OpRegRegRegShift(kOpRsub, rl_result.reg.GetReg(), r_lo, r_hi,
               EncodeShift(kArmAsr, magic_table[lit].shift));
      break;
    case Divide7:
      OpRegReg(kOpAdd, r_hi, rl_src.reg.GetReg());
      OpRegRegImm(kOpAsr, r_lo, rl_src.reg.GetReg(), 31);
      OpRegRegRegShift(kOpRsub, rl_result.reg.GetReg(), r_lo, r_hi,
               EncodeShift(kArmAsr, magic_table[lit].shift));
      break;
    default:
      LOG(FATAL) << "Unexpected pattern: " << pattern;
  }
  StoreValue(rl_dest, rl_result);
  return true;
}

LIR* ArmMir2Lir::GenRegMemCheck(ConditionCode c_code,
                    int reg1, int base, int offset, ThrowKind kind) {
  LOG(FATAL) << "Unexpected use of GenRegMemCheck for Arm";
  return NULL;
}

RegLocation ArmMir2Lir::GenDivRem(RegLocation rl_dest, RegLocation rl_src1,
                      RegLocation rl_src2, bool is_div, bool check_zero) {
  LOG(FATAL) << "Unexpected use of GenDivRem for Arm";
  return rl_dest;
}

RegLocation ArmMir2Lir::GenDivRemLit(RegLocation rl_dest, RegLocation rl_src1, int lit, bool is_div) {
  LOG(FATAL) << "Unexpected use of GenDivRemLit for Arm";
  return rl_dest;
}

RegLocation ArmMir2Lir::GenDivRemLit(RegLocation rl_dest, int reg1, int lit,
                                     bool is_div) {
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);

  // Put the literal in a temp.
  int lit_temp = AllocTemp();
  LoadConstant(lit_temp, lit);
  // Use the generic case for div/rem with arg2 in a register.
  // TODO: The literal temp can be freed earlier during a modulus to reduce reg pressure.
  rl_result = GenDivRem(rl_result, reg1, lit_temp, is_div);
  FreeTemp(lit_temp);

  return rl_result;
}

RegLocation ArmMir2Lir::GenDivRem(RegLocation rl_dest, int reg1, int reg2,
                                  bool is_div) {
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (is_div) {
    // Simple case, use sdiv instruction.
    OpRegRegReg(kOpDiv, rl_result.reg.GetReg(), reg1, reg2);
  } else {
    // Remainder case, use the following code:
    // temp = reg1 / reg2      - integer division
    // temp = temp * reg2
    // dest = reg1 - temp

    int temp = AllocTemp();
    OpRegRegReg(kOpDiv, temp, reg1, reg2);
    OpRegReg(kOpMul, temp, reg2);
    OpRegRegReg(kOpSub, rl_result.reg.GetReg(), reg1, temp);
    FreeTemp(temp);
  }

  return rl_result;
}

bool ArmMir2Lir::GenInlinedMinMaxInt(CallInfo* info, bool is_min) {
  DCHECK_EQ(cu_->instruction_set, kThumb2);
  RegLocation rl_src1 = info->args[0];
  RegLocation rl_src2 = info->args[1];
  rl_src1 = LoadValue(rl_src1, kCoreReg);
  rl_src2 = LoadValue(rl_src2, kCoreReg);
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  OpRegReg(kOpCmp, rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  OpIT((is_min) ? kCondGt : kCondLt, "E");
  OpRegReg(kOpMov, rl_result.reg.GetReg(), rl_src2.reg.GetReg());
  OpRegReg(kOpMov, rl_result.reg.GetReg(), rl_src1.reg.GetReg());
  GenBarrier();
  StoreValue(rl_dest, rl_result);
  return true;
}

bool ArmMir2Lir::GenInlinedPeek(CallInfo* info, OpSize size) {
  RegLocation rl_src_address = info->args[0];  // long address
  rl_src_address.wide = 0;  // ignore high half in info->args[1]
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_address = LoadValue(rl_src_address, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (size == kLong) {
    // Fake unaligned LDRD by two unaligned LDR instructions on ARMv7 with SCTLR.A set to 0.
    if (rl_address.reg.GetReg() != rl_result.reg.GetReg()) {
      LoadBaseDisp(rl_address.reg.GetReg(), 0, rl_result.reg.GetReg(), kWord, INVALID_SREG);
      LoadBaseDisp(rl_address.reg.GetReg(), 4, rl_result.reg.GetHighReg(), kWord, INVALID_SREG);
    } else {
      LoadBaseDisp(rl_address.reg.GetReg(), 4, rl_result.reg.GetHighReg(), kWord, INVALID_SREG);
      LoadBaseDisp(rl_address.reg.GetReg(), 0, rl_result.reg.GetReg(), kWord, INVALID_SREG);
    }
    StoreValueWide(rl_dest, rl_result);
  } else {
    DCHECK(size == kSignedByte || size == kSignedHalf || size == kWord);
    // Unaligned load with LDR and LDRSH is allowed on ARMv7 with SCTLR.A set to 0.
    LoadBaseDisp(rl_address.reg.GetReg(), 0, rl_result.reg.GetReg(), size, INVALID_SREG);
    StoreValue(rl_dest, rl_result);
  }
  return true;
}

bool ArmMir2Lir::GenInlinedPoke(CallInfo* info, OpSize size) {
  RegLocation rl_src_address = info->args[0];  // long address
  rl_src_address.wide = 0;  // ignore high half in info->args[1]
  RegLocation rl_src_value = info->args[2];  // [size] value
  RegLocation rl_address = LoadValue(rl_src_address, kCoreReg);
  if (size == kLong) {
    // Fake unaligned STRD by two unaligned STR instructions on ARMv7 with SCTLR.A set to 0.
    RegLocation rl_value = LoadValueWide(rl_src_value, kCoreReg);
    StoreBaseDisp(rl_address.reg.GetReg(), 0, rl_value.reg.GetReg(), kWord);
    StoreBaseDisp(rl_address.reg.GetReg(), 4, rl_value.reg.GetHighReg(), kWord);
  } else {
    DCHECK(size == kSignedByte || size == kSignedHalf || size == kWord);
    // Unaligned store with STR and STRSH is allowed on ARMv7 with SCTLR.A set to 0.
    RegLocation rl_value = LoadValue(rl_src_value, kCoreReg);
    StoreBaseDisp(rl_address.reg.GetReg(), 0, rl_value.reg.GetReg(), size);
  }
  return true;
}

void ArmMir2Lir::OpLea(int rBase, int reg1, int reg2, int scale, int offset) {
  LOG(FATAL) << "Unexpected use of OpLea for Arm";
}

void ArmMir2Lir::OpTlsCmp(ThreadOffset offset, int val) {
  LOG(FATAL) << "Unexpected use of OpTlsCmp for Arm";
}

bool ArmMir2Lir::GenInlinedCas(CallInfo* info, bool is_long, bool is_object) {
  DCHECK_EQ(cu_->instruction_set, kThumb2);
  // Unused - RegLocation rl_src_unsafe = info->args[0];
  RegLocation rl_src_obj = info->args[1];  // Object - known non-null
  RegLocation rl_src_offset = info->args[2];  // long low
  rl_src_offset.wide = 0;  // ignore high half in info->args[3]
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
  DCHECK(!reg_pool_->core_regs[rARM_LR].is_temp);
  MarkTemp(rARM_LR);
  FreeTemp(rARM_LR);
  LockTemp(rARM_LR);
  bool load_early = true;
  if (is_long) {
    bool expected_is_core_reg =
        rl_src_expected.location == kLocPhysReg && !IsFpReg(rl_src_expected.reg.GetReg());
    bool new_value_is_core_reg =
        rl_src_new_value.location == kLocPhysReg && !IsFpReg(rl_src_new_value.reg.GetReg());
    bool expected_is_good_reg = expected_is_core_reg && !IsTemp(rl_src_expected.reg.GetReg());
    bool new_value_is_good_reg = new_value_is_core_reg && !IsTemp(rl_src_new_value.reg.GetReg());

    if (!expected_is_good_reg && !new_value_is_good_reg) {
      // None of expected/new_value is non-temp reg, need to load both late
      load_early = false;
      // Make sure they are not in the temp regs and the load will not be skipped.
      if (expected_is_core_reg) {
        FlushRegWide(rl_src_expected.reg.GetReg(), rl_src_expected.reg.GetHighReg());
        ClobberSReg(rl_src_expected.s_reg_low);
        ClobberSReg(GetSRegHi(rl_src_expected.s_reg_low));
        rl_src_expected.location = kLocDalvikFrame;
      }
      if (new_value_is_core_reg) {
        FlushRegWide(rl_src_new_value.reg.GetReg(), rl_src_new_value.reg.GetHighReg());
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
    MarkGCCard(rl_new_value.reg.GetReg(), rl_object.reg.GetReg());
  }

  RegLocation rl_offset = LoadValue(rl_src_offset, kCoreReg);

  int r_ptr = rARM_LR;
  OpRegRegReg(kOpAdd, r_ptr, rl_object.reg.GetReg(), rl_offset.reg.GetReg());

  // Free now unneeded rl_object and rl_offset to give more temps.
  ClobberSReg(rl_object.s_reg_low);
  FreeTemp(rl_object.reg.GetReg());
  ClobberSReg(rl_offset.s_reg_low);
  FreeTemp(rl_offset.reg.GetReg());

  RegLocation rl_expected;
  if (!is_long) {
    rl_expected = LoadValue(rl_src_expected, kCoreReg);
  } else if (load_early) {
    rl_expected = LoadValueWide(rl_src_expected, kCoreReg);
  } else {
    // NOTE: partially defined rl_expected & rl_new_value - but we just want the regs.
    int low_reg = AllocTemp();
    int high_reg = AllocTemp();
    rl_new_value.reg = RegStorage(RegStorage::k64BitPair, low_reg, high_reg);
    rl_expected = rl_new_value;
  }

  // do {
  //   tmp = [r_ptr] - expected;
  // } while (tmp == 0 && failure([r_ptr] <- r_new_value));
  // result = tmp != 0;

  int r_tmp = AllocTemp();
  LIR* target = NewLIR0(kPseudoTargetLabel);

  if (is_long) {
    int r_tmp_high = AllocTemp();
    if (!load_early) {
      LoadValueDirectWide(rl_src_expected, rl_expected.reg.GetReg(), rl_expected.reg.GetHighReg());
    }
    NewLIR3(kThumb2Ldrexd, r_tmp, r_tmp_high, r_ptr);
    OpRegReg(kOpSub, r_tmp, rl_expected.reg.GetReg());
    OpRegReg(kOpSub, r_tmp_high, rl_expected.reg.GetHighReg());
    if (!load_early) {
      LoadValueDirectWide(rl_src_new_value, rl_new_value.reg.GetReg(), rl_new_value.reg.GetHighReg());
    }
    // Make sure we use ORR that sets the ccode
    if (ARM_LOWREG(r_tmp) && ARM_LOWREG(r_tmp_high)) {
      NewLIR2(kThumbOrr, r_tmp, r_tmp_high);
    } else {
      NewLIR4(kThumb2OrrRRRs, r_tmp, r_tmp, r_tmp_high, 0);
    }
    FreeTemp(r_tmp_high);  // Now unneeded

    DCHECK(last_lir_insn_->u.m.def_mask & ENCODE_CCODE);
    OpIT(kCondEq, "T");
    NewLIR4(kThumb2Strexd /* eq */, r_tmp, rl_new_value.reg.GetReg(), rl_new_value.reg.GetHighReg(), r_ptr);

  } else {
    NewLIR3(kThumb2Ldrex, r_tmp, r_ptr, 0);
    OpRegReg(kOpSub, r_tmp, rl_expected.reg.GetReg());
    DCHECK(last_lir_insn_->u.m.def_mask & ENCODE_CCODE);
    OpIT(kCondEq, "T");
    NewLIR4(kThumb2Strex /* eq */, r_tmp, rl_new_value.reg.GetReg(), r_ptr, 0);
  }

  // Still one conditional left from OpIT(kCondEq, "T") from either branch
  OpRegImm(kOpCmp /* eq */, r_tmp, 1);
  OpCondBranch(kCondEq, target);

  if (!load_early) {
    FreeTemp(rl_expected.reg.GetReg());  // Now unneeded.
    FreeTemp(rl_expected.reg.GetHighReg());  // Now unneeded.
  }

  // result := (tmp1 != 0) ? 0 : 1;
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  OpRegRegImm(kOpRsub, rl_result.reg.GetReg(), r_tmp, 1);
  DCHECK(last_lir_insn_->u.m.def_mask & ENCODE_CCODE);
  OpIT(kCondUlt, "");
  LoadConstant(rl_result.reg.GetReg(), 0); /* cc */
  FreeTemp(r_tmp);  // Now unneeded.

  StoreValue(rl_dest, rl_result);

  // Now, restore lr to its non-temp status.
  Clobber(rARM_LR);
  UnmarkTemp(rARM_LR);
  return true;
}

LIR* ArmMir2Lir::OpPcRelLoad(int reg, LIR* target) {
  return RawLIR(current_dalvik_offset_, kThumb2LdrPcRel12, reg, 0, 0, 0, 0, target);
}

LIR* ArmMir2Lir::OpVldm(int rBase, int count) {
  return NewLIR3(kThumb2Vldms, rBase, fr0, count);
}

LIR* ArmMir2Lir::OpVstm(int rBase, int count) {
  return NewLIR3(kThumb2Vstms, rBase, fr0, count);
}

void ArmMir2Lir::GenMultiplyByTwoBitMultiplier(RegLocation rl_src,
                                               RegLocation rl_result, int lit,
                                               int first_bit, int second_bit) {
  OpRegRegRegShift(kOpAdd, rl_result.reg.GetReg(), rl_src.reg.GetReg(), rl_src.reg.GetReg(),
                   EncodeShift(kArmLsl, second_bit - first_bit));
  if (first_bit != 0) {
    OpRegRegImm(kOpLsl, rl_result.reg.GetReg(), rl_result.reg.GetReg(), first_bit);
  }
}

void ArmMir2Lir::GenDivZeroCheck(int reg_lo, int reg_hi) {
  int t_reg = AllocTemp();
  NewLIR4(kThumb2OrrRRRs, t_reg, reg_lo, reg_hi, 0);
  FreeTemp(t_reg);
  GenCheck(kCondEq, kThrowDivZero);
}

// Test suspend flag, return target of taken suspend branch
LIR* ArmMir2Lir::OpTestSuspend(LIR* target) {
  NewLIR2(kThumbSubRI8, rARM_SUSPEND, 1);
  return OpCondBranch((target == NULL) ? kCondEq : kCondNe, target);
}

// Decrement register and branch on condition
LIR* ArmMir2Lir::OpDecAndBranch(ConditionCode c_code, int reg, LIR* target) {
  // Combine sub & test using sub setflags encoding here
  OpRegRegImm(kOpSub, reg, reg, 1);  // For value == 1, this should set flags.
  DCHECK(last_lir_insn_->u.m.def_mask & ENCODE_CCODE);
  return OpCondBranch(c_code, target);
}

void ArmMir2Lir::GenMemBarrier(MemBarrierKind barrier_kind) {
#if ANDROID_SMP != 0
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
  LIR* dmb = NewLIR1(kThumb2Dmb, dmb_flavor);
  dmb->u.m.def_mask = ENCODE_ALL;
#endif
}

void ArmMir2Lir::GenNegLong(RegLocation rl_dest, RegLocation rl_src) {
  rl_src = LoadValueWide(rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  int z_reg = AllocTemp();
  LoadConstantNoClobber(z_reg, 0);
  // Check for destructive overlap
  if (rl_result.reg.GetReg() == rl_src.reg.GetHighReg()) {
    int t_reg = AllocTemp();
    OpRegRegReg(kOpSub, rl_result.reg.GetReg(), z_reg, rl_src.reg.GetReg());
    OpRegRegReg(kOpSbc, rl_result.reg.GetHighReg(), z_reg, t_reg);
    FreeTemp(t_reg);
  } else {
    OpRegRegReg(kOpSub, rl_result.reg.GetReg(), z_reg, rl_src.reg.GetReg());
    OpRegRegReg(kOpSbc, rl_result.reg.GetHighReg(), z_reg, rl_src.reg.GetHighReg());
  }
  FreeTemp(z_reg);
  StoreValueWide(rl_dest, rl_result);
}

void ArmMir2Lir::GenMulLong(Instruction::Code opcode, RegLocation rl_dest,
                            RegLocation rl_src1, RegLocation rl_src2) {
    /*
     * To pull off inline multiply, we have a worst-case requirement of 8 temporary
     * registers.  Normally for Arm, we get 5.  We can get to 6 by including
     * lr in the temp set.  The only problematic case is all operands and result are
     * distinct, and none have been promoted.  In that case, we can succeed by aggressively
     * freeing operand temp registers after they are no longer needed.  All other cases
     * can proceed normally.  We'll just punt on the case of the result having a misaligned
     * overlap with either operand and send that case to a runtime handler.
     */
    RegLocation rl_result;
    if (BadOverlap(rl_src1, rl_dest) || (BadOverlap(rl_src2, rl_dest))) {
      ThreadOffset func_offset = QUICK_ENTRYPOINT_OFFSET(pLmul);
      FlushAllRegs();
      CallRuntimeHelperRegLocationRegLocation(func_offset, rl_src1, rl_src2, false);
      rl_result = GetReturnWide(false);
      StoreValueWide(rl_dest, rl_result);
      return;
    }
    // Temporarily add LR to the temp pool, and assign it to tmp1
    MarkTemp(rARM_LR);
    FreeTemp(rARM_LR);
    int tmp1 = rARM_LR;
    LockTemp(rARM_LR);

    rl_src1 = LoadValueWide(rl_src1, kCoreReg);
    rl_src2 = LoadValueWide(rl_src2, kCoreReg);

    bool special_case = true;
    // If operands are the same, or any pair has been promoted we're not the special case.
    if ((rl_src1.s_reg_low == rl_src2.s_reg_low) ||
        (!IsTemp(rl_src1.reg.GetReg()) && !IsTemp(rl_src1.reg.GetHighReg())) ||
        (!IsTemp(rl_src2.reg.GetReg()) && !IsTemp(rl_src2.reg.GetHighReg()))) {
      special_case = false;
    }
    // Tuning: if rl_dest has been promoted and is *not* either operand, could use directly.
    int res_lo = AllocTemp();
    int res_hi;
    if (rl_src1.reg.GetReg() == rl_src2.reg.GetReg()) {
      res_hi = AllocTemp();
      NewLIR3(kThumb2MulRRR, tmp1, rl_src1.reg.GetReg(), rl_src1.reg.GetHighReg());
      NewLIR4(kThumb2Umull, res_lo, res_hi, rl_src1.reg.GetReg(), rl_src1.reg.GetReg());
      OpRegRegRegShift(kOpAdd, res_hi, res_hi, tmp1, EncodeShift(kArmLsl, 1));
    } else {
      // In the special case, all temps are now allocated
      NewLIR3(kThumb2MulRRR, tmp1, rl_src2.reg.GetReg(), rl_src1.reg.GetHighReg());
      if (special_case) {
        DCHECK_NE(rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
        DCHECK_NE(rl_src1.reg.GetHighReg(), rl_src2.reg.GetHighReg());
        FreeTemp(rl_src1.reg.GetHighReg());
      }
      res_hi = AllocTemp();

      NewLIR4(kThumb2Umull, res_lo, res_hi, rl_src2.reg.GetReg(), rl_src1.reg.GetReg());
      NewLIR4(kThumb2Mla, tmp1, rl_src1.reg.GetReg(), rl_src2.reg.GetHighReg(), tmp1);
      NewLIR4(kThumb2AddRRR, res_hi, tmp1, res_hi, 0);
      if (special_case) {
        FreeTemp(rl_src1.reg.GetReg());
        Clobber(rl_src1.reg.GetReg());
        Clobber(rl_src1.reg.GetHighReg());
      }
    }
    FreeTemp(tmp1);
    rl_result = GetReturnWide(false);  // Just using as a template.
    rl_result.reg.SetReg(res_lo);
    rl_result.reg.SetHighReg(res_hi);
    StoreValueWide(rl_dest, rl_result);
    // Now, restore lr to its non-temp status.
    Clobber(rARM_LR);
    UnmarkTemp(rARM_LR);
}

void ArmMir2Lir::GenAddLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) {
  LOG(FATAL) << "Unexpected use of GenAddLong for Arm";
}

void ArmMir2Lir::GenSubLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) {
  LOG(FATAL) << "Unexpected use of GenSubLong for Arm";
}

void ArmMir2Lir::GenAndLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) {
  LOG(FATAL) << "Unexpected use of GenAndLong for Arm";
}

void ArmMir2Lir::GenOrLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                           RegLocation rl_src2) {
  LOG(FATAL) << "Unexpected use of GenOrLong for Arm";
}

void ArmMir2Lir::GenXorLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) {
  LOG(FATAL) << "Unexpected use of genXoLong for Arm";
}

/*
 * Generate array load
 */
void ArmMir2Lir::GenArrayGet(int opt_flags, OpSize size, RegLocation rl_array,
                             RegLocation rl_index, RegLocation rl_dest, int scale) {
  RegisterClass reg_class = oat_reg_class_by_size(size);
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
  GenNullCheck(rl_array.s_reg_low, rl_array.reg.GetReg(), opt_flags);

  bool needs_range_check = (!(opt_flags & MIR_IGNORE_RANGE_CHECK));
  int reg_len = INVALID_REG;
  if (needs_range_check) {
    reg_len = AllocTemp();
    /* Get len */
    LoadWordDisp(rl_array.reg.GetReg(), len_offset, reg_len);
  }
  if (rl_dest.wide || rl_dest.fp || constant_index) {
    int reg_ptr;
    if (constant_index) {
      reg_ptr = rl_array.reg.GetReg();  // NOTE: must not alter reg_ptr in constant case.
    } else {
      // No special indexed operation, lea + load w/ displacement
      reg_ptr = AllocTemp();
      OpRegRegRegShift(kOpAdd, reg_ptr, rl_array.reg.GetReg(), rl_index.reg.GetReg(),
                       EncodeShift(kArmLsl, scale));
      FreeTemp(rl_index.reg.GetReg());
    }
    rl_result = EvalLoc(rl_dest, reg_class, true);

    if (needs_range_check) {
      if (constant_index) {
        GenImmedCheck(kCondLs, reg_len, mir_graph_->ConstantValue(rl_index), kThrowConstantArrayBounds);
      } else {
        GenRegRegCheck(kCondLs, reg_len, rl_index.reg.GetReg(), kThrowArrayBounds);
      }
      FreeTemp(reg_len);
    }
    if (rl_dest.wide) {
      LoadBaseDispWide(reg_ptr, data_offset, rl_result.reg.GetReg(), rl_result.reg.GetHighReg(), INVALID_SREG);
      if (!constant_index) {
        FreeTemp(reg_ptr);
      }
      StoreValueWide(rl_dest, rl_result);
    } else {
      LoadBaseDisp(reg_ptr, data_offset, rl_result.reg.GetReg(), size, INVALID_SREG);
      if (!constant_index) {
        FreeTemp(reg_ptr);
      }
      StoreValue(rl_dest, rl_result);
    }
  } else {
    // Offset base, then use indexed load
    int reg_ptr = AllocTemp();
    OpRegRegImm(kOpAdd, reg_ptr, rl_array.reg.GetReg(), data_offset);
    FreeTemp(rl_array.reg.GetReg());
    rl_result = EvalLoc(rl_dest, reg_class, true);

    if (needs_range_check) {
      GenRegRegCheck(kCondUge, rl_index.reg.GetReg(), reg_len, kThrowArrayBounds);
      FreeTemp(reg_len);
    }
    LoadBaseIndexed(reg_ptr, rl_index.reg.GetReg(), rl_result.reg.GetReg(), scale, size);
    FreeTemp(reg_ptr);
    StoreValue(rl_dest, rl_result);
  }
}

/*
 * Generate array store
 *
 */
void ArmMir2Lir::GenArrayPut(int opt_flags, OpSize size, RegLocation rl_array,
                             RegLocation rl_index, RegLocation rl_src, int scale, bool card_mark) {
  RegisterClass reg_class = oat_reg_class_by_size(size);
  int len_offset = mirror::Array::LengthOffset().Int32Value();
  bool constant_index = rl_index.is_const;

  int data_offset;
  if (size == kLong || size == kDouble) {
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

  int reg_ptr;
  bool allocated_reg_ptr_temp = false;
  if (constant_index) {
    reg_ptr = rl_array.reg.GetReg();
  } else if (IsTemp(rl_array.reg.GetReg()) && !card_mark) {
    Clobber(rl_array.reg.GetReg());
    reg_ptr = rl_array.reg.GetReg();
  } else {
    allocated_reg_ptr_temp = true;
    reg_ptr = AllocTemp();
  }

  /* null object? */
  GenNullCheck(rl_array.s_reg_low, rl_array.reg.GetReg(), opt_flags);

  bool needs_range_check = (!(opt_flags & MIR_IGNORE_RANGE_CHECK));
  int reg_len = INVALID_REG;
  if (needs_range_check) {
    reg_len = AllocTemp();
    // NOTE: max live temps(4) here.
    /* Get len */
    LoadWordDisp(rl_array.reg.GetReg(), len_offset, reg_len);
  }
  /* at this point, reg_ptr points to array, 2 live temps */
  if (rl_src.wide || rl_src.fp || constant_index) {
    if (rl_src.wide) {
      rl_src = LoadValueWide(rl_src, reg_class);
    } else {
      rl_src = LoadValue(rl_src, reg_class);
    }
    if (!constant_index) {
      OpRegRegRegShift(kOpAdd, reg_ptr, rl_array.reg.GetReg(), rl_index.reg.GetReg(),
                       EncodeShift(kArmLsl, scale));
    }
    if (needs_range_check) {
      if (constant_index) {
        GenImmedCheck(kCondLs, reg_len, mir_graph_->ConstantValue(rl_index), kThrowConstantArrayBounds);
      } else {
        GenRegRegCheck(kCondLs, reg_len, rl_index.reg.GetReg(), kThrowArrayBounds);
      }
      FreeTemp(reg_len);
    }

    if (rl_src.wide) {
      StoreBaseDispWide(reg_ptr, data_offset, rl_src.reg.GetReg(), rl_src.reg.GetHighReg());
    } else {
      StoreBaseDisp(reg_ptr, data_offset, rl_src.reg.GetReg(), size);
    }
  } else {
    /* reg_ptr -> array data */
    OpRegRegImm(kOpAdd, reg_ptr, rl_array.reg.GetReg(), data_offset);
    rl_src = LoadValue(rl_src, reg_class);
    if (needs_range_check) {
      GenRegRegCheck(kCondUge, rl_index.reg.GetReg(), reg_len, kThrowArrayBounds);
      FreeTemp(reg_len);
    }
    StoreBaseIndexed(reg_ptr, rl_index.reg.GetReg(), rl_src.reg.GetReg(),
                     scale, size);
  }
  if (allocated_reg_ptr_temp) {
    FreeTemp(reg_ptr);
  }
  if (card_mark) {
    MarkGCCard(rl_src.reg.GetReg(), rl_array.reg.GetReg());
  }
}


void ArmMir2Lir::GenShiftImmOpLong(Instruction::Code opcode,
                                   RegLocation rl_dest, RegLocation rl_src, RegLocation rl_shift) {
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
        OpRegRegReg(kOpAdd, rl_result.reg.GetReg(), rl_src.reg.GetReg(), rl_src.reg.GetReg());
        OpRegRegReg(kOpAdc, rl_result.reg.GetHighReg(), rl_src.reg.GetHighReg(), rl_src.reg.GetHighReg());
      } else if (shift_amount == 32) {
        OpRegCopy(rl_result.reg.GetHighReg(), rl_src.reg.GetReg());
        LoadConstant(rl_result.reg.GetReg(), 0);
      } else if (shift_amount > 31) {
        OpRegRegImm(kOpLsl, rl_result.reg.GetHighReg(), rl_src.reg.GetReg(), shift_amount - 32);
        LoadConstant(rl_result.reg.GetReg(), 0);
      } else {
        OpRegRegImm(kOpLsl, rl_result.reg.GetHighReg(), rl_src.reg.GetHighReg(), shift_amount);
        OpRegRegRegShift(kOpOr, rl_result.reg.GetHighReg(), rl_result.reg.GetHighReg(), rl_src.reg.GetReg(),
                         EncodeShift(kArmLsr, 32 - shift_amount));
        OpRegRegImm(kOpLsl, rl_result.reg.GetReg(), rl_src.reg.GetReg(), shift_amount);
      }
      break;
    case Instruction::SHR_LONG:
    case Instruction::SHR_LONG_2ADDR:
      if (shift_amount == 32) {
        OpRegCopy(rl_result.reg.GetReg(), rl_src.reg.GetHighReg());
        OpRegRegImm(kOpAsr, rl_result.reg.GetHighReg(), rl_src.reg.GetHighReg(), 31);
      } else if (shift_amount > 31) {
        OpRegRegImm(kOpAsr, rl_result.reg.GetReg(), rl_src.reg.GetHighReg(), shift_amount - 32);
        OpRegRegImm(kOpAsr, rl_result.reg.GetHighReg(), rl_src.reg.GetHighReg(), 31);
      } else {
        int t_reg = AllocTemp();
        OpRegRegImm(kOpLsr, t_reg, rl_src.reg.GetReg(), shift_amount);
        OpRegRegRegShift(kOpOr, rl_result.reg.GetReg(), t_reg, rl_src.reg.GetHighReg(),
                         EncodeShift(kArmLsl, 32 - shift_amount));
        FreeTemp(t_reg);
        OpRegRegImm(kOpAsr, rl_result.reg.GetHighReg(), rl_src.reg.GetHighReg(), shift_amount);
      }
      break;
    case Instruction::USHR_LONG:
    case Instruction::USHR_LONG_2ADDR:
      if (shift_amount == 32) {
        OpRegCopy(rl_result.reg.GetReg(), rl_src.reg.GetHighReg());
        LoadConstant(rl_result.reg.GetHighReg(), 0);
      } else if (shift_amount > 31) {
        OpRegRegImm(kOpLsr, rl_result.reg.GetReg(), rl_src.reg.GetHighReg(), shift_amount - 32);
        LoadConstant(rl_result.reg.GetHighReg(), 0);
      } else {
        int t_reg = AllocTemp();
        OpRegRegImm(kOpLsr, t_reg, rl_src.reg.GetReg(), shift_amount);
        OpRegRegRegShift(kOpOr, rl_result.reg.GetReg(), t_reg, rl_src.reg.GetHighReg(),
                         EncodeShift(kArmLsl, 32 - shift_amount));
        FreeTemp(t_reg);
        OpRegRegImm(kOpLsr, rl_result.reg.GetHighReg(), rl_src.reg.GetHighReg(), shift_amount);
      }
      break;
    default:
      LOG(FATAL) << "Unexpected case";
  }
  StoreValueWide(rl_dest, rl_result);
}

void ArmMir2Lir::GenArithImmOpLong(Instruction::Code opcode,
                                   RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) {
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
  int64_t val = mir_graph_->ConstantValueWide(rl_src2);
  uint32_t val_lo = Low32Bits(val);
  uint32_t val_hi = High32Bits(val);
  int32_t mod_imm_lo = ModifiedImmediate(val_lo);
  int32_t mod_imm_hi = ModifiedImmediate(val_hi);

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
    case Instruction::ADD_LONG:
    case Instruction::ADD_LONG_2ADDR:
      NewLIR3(kThumb2AddRRI8M, rl_result.reg.GetReg(), rl_src1.reg.GetReg(), mod_imm_lo);
      NewLIR3(kThumb2AdcRRI8M, rl_result.reg.GetHighReg(), rl_src1.reg.GetHighReg(), mod_imm_hi);
      break;
    case Instruction::OR_LONG:
    case Instruction::OR_LONG_2ADDR:
      if ((val_lo != 0) || (rl_result.reg.GetReg() != rl_src1.reg.GetReg())) {
        OpRegRegImm(kOpOr, rl_result.reg.GetReg(), rl_src1.reg.GetReg(), val_lo);
      }
      if ((val_hi != 0) || (rl_result.reg.GetHighReg() != rl_src1.reg.GetHighReg())) {
        OpRegRegImm(kOpOr, rl_result.reg.GetHighReg(), rl_src1.reg.GetHighReg(), val_hi);
      }
      break;
    case Instruction::XOR_LONG:
    case Instruction::XOR_LONG_2ADDR:
      OpRegRegImm(kOpXor, rl_result.reg.GetReg(), rl_src1.reg.GetReg(), val_lo);
      OpRegRegImm(kOpXor, rl_result.reg.GetHighReg(), rl_src1.reg.GetHighReg(), val_hi);
      break;
    case Instruction::AND_LONG:
    case Instruction::AND_LONG_2ADDR:
      if ((val_lo != 0xffffffff) || (rl_result.reg.GetReg() != rl_src1.reg.GetReg())) {
        OpRegRegImm(kOpAnd, rl_result.reg.GetReg(), rl_src1.reg.GetReg(), val_lo);
      }
      if ((val_hi != 0xffffffff) || (rl_result.reg.GetHighReg() != rl_src1.reg.GetHighReg())) {
        OpRegRegImm(kOpAnd, rl_result.reg.GetHighReg(), rl_src1.reg.GetHighReg(), val_hi);
      }
      break;
    case Instruction::SUB_LONG_2ADDR:
    case Instruction::SUB_LONG:
      NewLIR3(kThumb2SubRRI8M, rl_result.reg.GetReg(), rl_src1.reg.GetReg(), mod_imm_lo);
      NewLIR3(kThumb2SbcRRI8M, rl_result.reg.GetHighReg(), rl_src1.reg.GetHighReg(), mod_imm_hi);
      break;
    default:
      LOG(FATAL) << "Unexpected opcode " << opcode;
  }
  StoreValueWide(rl_dest, rl_result);
}

}  // namespace art
