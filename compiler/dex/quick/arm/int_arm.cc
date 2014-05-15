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

LIR* ArmMir2Lir::OpCmpBranch(ConditionCode cond, RegStorage src1, RegStorage src2, LIR* target) {
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

void ArmMir2Lir::UpdateIT(LIR* it, const char* new_guide) {
  int mask;
  int mask3 = 0;
  int mask2 = 0;
  int mask1 = 0;
  ArmConditionCode code = static_cast<ArmConditionCode>(it->operands[0]);
  int cond_bit = code & 1;
  int alt_bit = cond_bit ^ 1;

  // Note: case fallthroughs intentional
  switch (strlen(new_guide)) {
    case 3:
      mask1 = (new_guide[2] == 'T') ? cond_bit : alt_bit;
    case 2:
      mask2 = (new_guide[1] == 'T') ? cond_bit : alt_bit;
    case 1:
      mask3 = (new_guide[0] == 'T') ? cond_bit : alt_bit;
      break;
    case 0:
      break;
    default:
      LOG(FATAL) << "OAT: bad case in UpdateIT";
  }
  mask = (mask3 << 3) | (mask2 << 2) | (mask1 << 1) |
      (1 << (3 - strlen(new_guide)));
  it->operands[1] = mask;
}

void ArmMir2Lir::OpEndIT(LIR* it) {
  // TODO: use the 'it' pointer to do some checks with the LIR, for example
  //       we could check that the number of instructions matches the mask
  //       in the IT instruction.
  CHECK(it != nullptr);
  GenBarrier();
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
void ArmMir2Lir::GenCmpLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) {
  LIR* target1;
  LIR* target2;
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  RegStorage t_reg = AllocTemp();
  LoadConstant(t_reg, -1);
  OpRegReg(kOpCmp, rl_src1.reg.GetHigh(), rl_src2.reg.GetHigh());
  LIR* branch1 = OpCondBranch(kCondLt, NULL);
  LIR* branch2 = OpCondBranch(kCondGt, NULL);
  OpRegRegReg(kOpSub, t_reg, rl_src1.reg.GetLow(), rl_src2.reg.GetLow());
  LIR* branch3 = OpCondBranch(kCondEq, NULL);

  LIR* it = OpIT(kCondHi, "E");
  NewLIR2(kThumb2MovI8M, t_reg.GetReg(), ModifiedImmediate(-1));
  LoadConstant(t_reg, 1);
  OpEndIT(it);

  target2 = NewLIR0(kPseudoTargetLabel);
  OpRegReg(kOpNeg, t_reg, t_reg);

  target1 = NewLIR0(kPseudoTargetLabel);

  RegLocation rl_temp = LocCReturn();  // Just using as template, will change
  rl_temp.reg.SetReg(t_reg.GetReg());
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
  RegStorage low_reg = rl_src1.reg.GetLow();
  RegStorage high_reg = rl_src1.reg.GetHigh();

  if (val == 0 && (ccode == kCondEq || ccode == kCondNe)) {
    RegStorage t_reg = AllocTemp();
    NewLIR4(kThumb2OrrRRRs, t_reg.GetReg(), low_reg.GetReg(), high_reg.GetReg(), 0);
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
      LIR* it = OpIT(true_val == 0 ? kCondNe : kCondUge, "");
      LoadConstant(rl_result.reg, false_val);
      OpEndIT(it);  // Add a scheduling barrier to keep the IT shadow intact
    } else if (cheap_false_val && ccode == kCondEq && true_val == 1) {
      OpRegRegImm(kOpRsub, rl_result.reg, rl_src.reg, 1);
      DCHECK(last_lir_insn_->u.m.def_mask & ENCODE_CCODE);
      LIR* it = OpIT(kCondLs, "");
      LoadConstant(rl_result.reg, false_val);
      OpEndIT(it);  // Add a scheduling barrier to keep the IT shadow intact
    } else if (cheap_false_val && InexpensiveConstantInt(true_val)) {
      OpRegImm(kOpCmp, rl_src.reg, 0);
      LIR* it = OpIT(ccode, "E");
      LoadConstant(rl_result.reg, true_val);
      LoadConstant(rl_result.reg, false_val);
      OpEndIT(it);  // Add a scheduling barrier to keep the IT shadow intact
    } else {
      // Unlikely case - could be tuned.
      RegStorage t_reg1 = AllocTemp();
      RegStorage t_reg2 = AllocTemp();
      LoadConstant(t_reg1, true_val);
      LoadConstant(t_reg2, false_val);
      OpRegImm(kOpCmp, rl_src.reg, 0);
      LIR* it = OpIT(ccode, "E");
      OpRegCopy(rl_result.reg, t_reg1);
      OpRegCopy(rl_result.reg, t_reg2);
      OpEndIT(it);  // Add a scheduling barrier to keep the IT shadow intact
    }
  } else {
    // MOVE case
    RegLocation rl_true = mir_graph_->reg_location_[mir->ssa_rep->uses[1]];
    RegLocation rl_false = mir_graph_->reg_location_[mir->ssa_rep->uses[2]];
    rl_true = LoadValue(rl_true, kCoreReg);
    rl_false = LoadValue(rl_false, kCoreReg);
    rl_result = EvalLoc(rl_dest, kCoreReg, true);
    OpRegImm(kOpCmp, rl_src.reg, 0);
    LIR* it = nullptr;
    if (rl_result.reg.GetReg() == rl_true.reg.GetReg()) {  // Is the "true" case already in place?
      it = OpIT(NegateComparison(ccode), "");
      OpRegCopy(rl_result.reg, rl_false.reg);
    } else if (rl_result.reg.GetReg() == rl_false.reg.GetReg()) {  // False case in place?
      it = OpIT(ccode, "");
      OpRegCopy(rl_result.reg, rl_true.reg);
    } else {  // Normal - select between the two.
      it = OpIT(ccode, "E");
      OpRegCopy(rl_result.reg, rl_true.reg);
      OpRegCopy(rl_result.reg, rl_false.reg);
    }
    OpEndIT(it);  // Add a scheduling barrier to keep the IT shadow intact
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
LIR* ArmMir2Lir::OpCmpImmBranch(ConditionCode cond, RegStorage reg, int check_value, LIR* target) {
  LIR* branch;
  ArmConditionCode arm_cond = ArmConditionEncoding(cond);
  /*
   * A common use of OpCmpImmBranch is for null checks, and using the Thumb 16-bit
   * compare-and-branch if zero is ideal if it will reach.  However, because null checks
   * branch forward to a slow path, they will frequently not reach - and thus have to
   * be converted to a long form during assembly (which will trigger another assembly
   * pass).  Here we estimate the branch distance for checks, and if large directly
   * generate the long form in an attempt to avoid an extra assembly pass.
   * TODO: consider interspersing slowpaths in code following unconditional branches.
   */
  bool skip = ((target != NULL) && (target->opcode == kPseudoThrowTarget));
  skip &= ((cu_->code_item->insns_size_in_code_units_ - current_dalvik_offset_) > 64);
  if (!skip && reg.Low8() && (check_value == 0) &&
     ((arm_cond == kArmCondEq) || (arm_cond == kArmCondNe))) {
    branch = NewLIR2((arm_cond == kArmCondEq) ? kThumb2Cbz : kThumb2Cbnz,
                     reg.GetReg(), 0);
  } else {
    OpRegImm(kOpCmp, reg, check_value);
    branch = NewLIR2(kThumbBCond, 0, arm_cond);
  }
  branch->target = target;
  return branch;
}

LIR* ArmMir2Lir::OpRegCopyNoInsert(RegStorage r_dest, RegStorage r_src) {
  LIR* res;
  int opcode;
  // If src or dest is a pair, we'll be using low reg.
  if (r_dest.IsPair()) {
    r_dest = r_dest.GetLow();
  }
  if (r_src.IsPair()) {
    r_src = r_src.GetLow();
  }
  if (r_dest.IsFloat() || r_src.IsFloat())
    return OpFpRegCopy(r_dest, r_src);
  if (r_dest.Low8() && r_src.Low8())
    opcode = kThumbMovRR;
  else if (!r_dest.Low8() && !r_src.Low8())
     opcode = kThumbMovRR_H2H;
  else if (r_dest.Low8())
     opcode = kThumbMovRR_H2L;
  else
     opcode = kThumbMovRR_L2H;
  res = RawLIR(current_dalvik_offset_, opcode, r_dest.GetReg(), r_src.GetReg());
  if (!(cu_->disable_opt & (1 << kSafeOptimizations)) && r_dest == r_src) {
    res->flags.is_nop = true;
  }
  return res;
}

void ArmMir2Lir::OpRegCopy(RegStorage r_dest, RegStorage r_src) {
  if (r_dest != r_src) {
    LIR* res = OpRegCopyNoInsert(r_dest, r_src);
    AppendLIR(res);
  }
}

void ArmMir2Lir::OpRegCopyWide(RegStorage r_dest, RegStorage r_src) {
  if (r_dest != r_src) {
    bool dest_fp = r_dest.IsFloat();
    bool src_fp = r_src.IsFloat();
    DCHECK(r_dest.Is64Bit());
    DCHECK(r_src.Is64Bit());
    if (dest_fp) {
      if (src_fp) {
        OpRegCopy(r_dest, r_src);
      } else {
        NewLIR3(kThumb2Fmdrr, r_dest.GetReg(), r_src.GetLowReg(), r_src.GetHighReg());
      }
    } else {
      if (src_fp) {
        NewLIR3(kThumb2Fmrrd, r_dest.GetLowReg(), r_dest.GetHighReg(), r_src.GetReg());
      } else {
        // Handle overlap
        if (r_src.GetHighReg() == r_dest.GetLowReg()) {
          DCHECK_NE(r_src.GetLowReg(), r_dest.GetHighReg());
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

  RegStorage r_magic = AllocTemp();
  LoadConstant(r_magic, magic_table[lit].magic);
  rl_src = LoadValue(rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  RegStorage r_hi = AllocTemp();
  RegStorage r_lo = AllocTemp();

  // rl_dest and rl_src might overlap.
  // Reuse r_hi to save the div result for reminder case.
  RegStorage r_div_result = is_div ? rl_result.reg : r_hi;

  NewLIR4(kThumb2Smull, r_lo.GetReg(), r_hi.GetReg(), r_magic.GetReg(), rl_src.reg.GetReg());
  switch (pattern) {
    case Divide3:
      OpRegRegRegShift(kOpSub, r_div_result, r_hi, rl_src.reg, EncodeShift(kArmAsr, 31));
      break;
    case Divide5:
      OpRegRegImm(kOpAsr, r_lo, rl_src.reg, 31);
      OpRegRegRegShift(kOpRsub, r_div_result, r_lo, r_hi,
                       EncodeShift(kArmAsr, magic_table[lit].shift));
      break;
    case Divide7:
      OpRegReg(kOpAdd, r_hi, rl_src.reg);
      OpRegRegImm(kOpAsr, r_lo, rl_src.reg, 31);
      OpRegRegRegShift(kOpRsub, r_div_result, r_lo, r_hi,
                       EncodeShift(kArmAsr, magic_table[lit].shift));
      break;
    default:
      LOG(FATAL) << "Unexpected pattern: " << pattern;
  }

  if (!is_div) {
    // div_result = src / lit
    // tmp1 = div_result * lit
    // dest = src - tmp1
    RegStorage tmp1 = r_lo;
    EasyMultiplyOp ops[2];

    bool canEasyMultiply = GetEasyMultiplyTwoOps(lit, ops);
    DCHECK_NE(canEasyMultiply, false);

    GenEasyMultiplyTwoOps(tmp1, r_div_result, ops);
    OpRegRegReg(kOpSub, rl_result.reg, rl_src.reg, tmp1);
  }

  StoreValue(rl_dest, rl_result);
  return true;
}

// Try to convert *lit to 1 RegRegRegShift/RegRegShift form.
bool ArmMir2Lir::GetEasyMultiplyOp(int lit, ArmMir2Lir::EasyMultiplyOp* op) {
  if (IsPowerOfTwo(lit)) {
    op->op = kOpLsl;
    op->shift = LowestSetBit(lit);
    return true;
  }

  if (IsPowerOfTwo(lit - 1)) {
    op->op = kOpAdd;
    op->shift = LowestSetBit(lit - 1);
    return true;
  }

  if (IsPowerOfTwo(lit + 1)) {
    op->op = kOpRsub;
    op->shift = LowestSetBit(lit + 1);
    return true;
  }

  op->op = kOpInvalid;
  op->shift = 0;
  return false;
}

// Try to convert *lit to 1~2 RegRegRegShift/RegRegShift forms.
bool ArmMir2Lir::GetEasyMultiplyTwoOps(int lit, EasyMultiplyOp* ops) {
  GetEasyMultiplyOp(lit, &ops[0]);
  if (GetEasyMultiplyOp(lit, &ops[0])) {
    ops[1].op = kOpInvalid;
    ops[1].shift = 0;
    return true;
  }

  int lit1 = lit;
  uint32_t shift = LowestSetBit(lit1);
  if (GetEasyMultiplyOp(lit1 >> shift, &ops[0])) {
    ops[1].op = kOpLsl;
    ops[1].shift = shift;
    return true;
  }

  lit1 = lit - 1;
  shift = LowestSetBit(lit1);
  if (GetEasyMultiplyOp(lit1 >> shift, &ops[0])) {
    ops[1].op = kOpAdd;
    ops[1].shift = shift;
    return true;
  }

  lit1 = lit + 1;
  shift = LowestSetBit(lit1);
  if (GetEasyMultiplyOp(lit1 >> shift, &ops[0])) {
    ops[1].op = kOpRsub;
    ops[1].shift = shift;
    return true;
  }

  return false;
}

// Generate instructions to do multiply.
// Additional temporary register is required,
// if it need to generate 2 instructions and src/dest overlap.
void ArmMir2Lir::GenEasyMultiplyTwoOps(RegStorage r_dest, RegStorage r_src, EasyMultiplyOp* ops) {
  // tmp1 = ( src << shift1) + [ src | -src | 0 ]
  // dest = (tmp1 << shift2) + [ src | -src | 0 ]

  RegStorage r_tmp1;
  if (ops[1].op == kOpInvalid) {
    r_tmp1 = r_dest;
  } else if (r_dest.GetReg() != r_src.GetReg()) {
    r_tmp1 = r_dest;
  } else {
    r_tmp1 = AllocTemp();
  }

  switch (ops[0].op) {
    case kOpLsl:
      OpRegRegImm(kOpLsl, r_tmp1, r_src, ops[0].shift);
      break;
    case kOpAdd:
      OpRegRegRegShift(kOpAdd, r_tmp1, r_src, r_src, EncodeShift(kArmLsl, ops[0].shift));
      break;
    case kOpRsub:
      OpRegRegRegShift(kOpRsub, r_tmp1, r_src, r_src, EncodeShift(kArmLsl, ops[0].shift));
      break;
    default:
      DCHECK_EQ(ops[0].op, kOpInvalid);
      break;
  }

  switch (ops[1].op) {
    case kOpInvalid:
      return;
    case kOpLsl:
      OpRegRegImm(kOpLsl, r_dest, r_tmp1, ops[1].shift);
      break;
    case kOpAdd:
      OpRegRegRegShift(kOpAdd, r_dest, r_src, r_tmp1, EncodeShift(kArmLsl, ops[1].shift));
      break;
    case kOpRsub:
      OpRegRegRegShift(kOpRsub, r_dest, r_src, r_tmp1, EncodeShift(kArmLsl, ops[1].shift));
      break;
    default:
      LOG(FATAL) << "Unexpected opcode passed to GenEasyMultiplyTwoOps";
      break;
  }
}

bool ArmMir2Lir::EasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit) {
  EasyMultiplyOp ops[2];

  if (!GetEasyMultiplyTwoOps(lit, ops)) {
    return false;
  }

  rl_src = LoadValue(rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);

  GenEasyMultiplyTwoOps(rl_result.reg, rl_src.reg, ops);
  StoreValue(rl_dest, rl_result);
  return true;
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

RegLocation ArmMir2Lir::GenDivRemLit(RegLocation rl_dest, RegStorage reg1, int lit, bool is_div) {
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

RegLocation ArmMir2Lir::GenDivRem(RegLocation rl_dest, RegStorage reg1, RegStorage reg2,
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

bool ArmMir2Lir::GenInlinedMinMaxInt(CallInfo* info, bool is_min) {
  DCHECK_EQ(cu_->instruction_set, kThumb2);
  RegLocation rl_src1 = info->args[0];
  RegLocation rl_src2 = info->args[1];
  rl_src1 = LoadValue(rl_src1, kCoreReg);
  rl_src2 = LoadValue(rl_src2, kCoreReg);
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  OpRegReg(kOpCmp, rl_src1.reg, rl_src2.reg);
  LIR* it = OpIT((is_min) ? kCondGt : kCondLt, "E");
  OpRegReg(kOpMov, rl_result.reg, rl_src2.reg);
  OpRegReg(kOpMov, rl_result.reg, rl_src1.reg);
  OpEndIT(it);
  StoreValue(rl_dest, rl_result);
  return true;
}

bool ArmMir2Lir::GenInlinedPeek(CallInfo* info, OpSize size) {
  RegLocation rl_src_address = info->args[0];  // long address
  rl_src_address = NarrowRegLoc(rl_src_address);  // ignore high half in info->args[1]
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_address = LoadValue(rl_src_address, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (size == k64) {
    // Fake unaligned LDRD by two unaligned LDR instructions on ARMv7 with SCTLR.A set to 0.
    if (rl_address.reg.GetReg() != rl_result.reg.GetLowReg()) {
      Load32Disp(rl_address.reg, 0, rl_result.reg.GetLow());
      Load32Disp(rl_address.reg, 4, rl_result.reg.GetHigh());
    } else {
      Load32Disp(rl_address.reg, 4, rl_result.reg.GetHigh());
      Load32Disp(rl_address.reg, 0, rl_result.reg.GetLow());
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

bool ArmMir2Lir::GenInlinedPoke(CallInfo* info, OpSize size) {
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

void ArmMir2Lir::OpLea(RegStorage r_base, RegStorage reg1, RegStorage reg2, int scale, int offset) {
  LOG(FATAL) << "Unexpected use of OpLea for Arm";
}

void ArmMir2Lir::OpTlsCmp(ThreadOffset<4> offset, int val) {
  LOG(FATAL) << "Unexpected use of OpTlsCmp for Arm";
}

void ArmMir2Lir::OpTlsCmp(ThreadOffset<8> offset, int val) {
  UNIMPLEMENTED(FATAL) << "Should not be called.";
}

bool ArmMir2Lir::GenInlinedCas(CallInfo* info, bool is_long, bool is_object) {
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
  DCHECK(!GetRegInfo(rs_rARM_LR)->IsTemp());
  MarkTemp(rs_rARM_LR);
  FreeTemp(rs_rARM_LR);
  LockTemp(rs_rARM_LR);
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

  RegStorage r_ptr = rs_rARM_LR;
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
    RegStorage low_reg = AllocTemp();
    RegStorage high_reg = AllocTemp();
    rl_new_value.reg = RegStorage::MakeRegPair(low_reg, high_reg);
    rl_expected = rl_new_value;
  }

  // do {
  //   tmp = [r_ptr] - expected;
  // } while (tmp == 0 && failure([r_ptr] <- r_new_value));
  // result = tmp != 0;

  RegStorage r_tmp = AllocTemp();
  LIR* target = NewLIR0(kPseudoTargetLabel);

  LIR* it = nullptr;
  if (is_long) {
    RegStorage r_tmp_high = AllocTemp();
    if (!load_early) {
      LoadValueDirectWide(rl_src_expected, rl_expected.reg);
    }
    NewLIR3(kThumb2Ldrexd, r_tmp.GetReg(), r_tmp_high.GetReg(), r_ptr.GetReg());
    OpRegReg(kOpSub, r_tmp, rl_expected.reg.GetLow());
    OpRegReg(kOpSub, r_tmp_high, rl_expected.reg.GetHigh());
    if (!load_early) {
      LoadValueDirectWide(rl_src_new_value, rl_new_value.reg);
    }
    // Make sure we use ORR that sets the ccode
    if (r_tmp.Low8() && r_tmp_high.Low8()) {
      NewLIR2(kThumbOrr, r_tmp.GetReg(), r_tmp_high.GetReg());
    } else {
      NewLIR4(kThumb2OrrRRRs, r_tmp.GetReg(), r_tmp.GetReg(), r_tmp_high.GetReg(), 0);
    }
    FreeTemp(r_tmp_high);  // Now unneeded

    DCHECK(last_lir_insn_->u.m.def_mask & ENCODE_CCODE);
    it = OpIT(kCondEq, "T");
    NewLIR4(kThumb2Strexd /* eq */, r_tmp.GetReg(), rl_new_value.reg.GetLowReg(), rl_new_value.reg.GetHighReg(), r_ptr.GetReg());

  } else {
    NewLIR3(kThumb2Ldrex, r_tmp.GetReg(), r_ptr.GetReg(), 0);
    OpRegReg(kOpSub, r_tmp, rl_expected.reg);
    DCHECK(last_lir_insn_->u.m.def_mask & ENCODE_CCODE);
    it = OpIT(kCondEq, "T");
    NewLIR4(kThumb2Strex /* eq */, r_tmp.GetReg(), rl_new_value.reg.GetReg(), r_ptr.GetReg(), 0);
  }

  // Still one conditional left from OpIT(kCondEq, "T") from either branch
  OpRegImm(kOpCmp /* eq */, r_tmp, 1);
  OpEndIT(it);

  OpCondBranch(kCondEq, target);

  if (!load_early) {
    FreeTemp(rl_expected.reg);  // Now unneeded.
  }

  // result := (tmp1 != 0) ? 0 : 1;
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  OpRegRegImm(kOpRsub, rl_result.reg, r_tmp, 1);
  DCHECK(last_lir_insn_->u.m.def_mask & ENCODE_CCODE);
  it = OpIT(kCondUlt, "");
  LoadConstant(rl_result.reg, 0); /* cc */
  FreeTemp(r_tmp);  // Now unneeded.
  OpEndIT(it);     // Barrier to terminate OpIT.

  StoreValue(rl_dest, rl_result);

  // Now, restore lr to its non-temp status.
  Clobber(rs_rARM_LR);
  UnmarkTemp(rs_rARM_LR);
  return true;
}

LIR* ArmMir2Lir::OpPcRelLoad(RegStorage reg, LIR* target) {
  return RawLIR(current_dalvik_offset_, kThumb2LdrPcRel12, reg.GetReg(), 0, 0, 0, 0, target);
}

LIR* ArmMir2Lir::OpVldm(RegStorage r_base, int count) {
  return NewLIR3(kThumb2Vldms, r_base.GetReg(), rs_fr0.GetReg(), count);
}

LIR* ArmMir2Lir::OpVstm(RegStorage r_base, int count) {
  return NewLIR3(kThumb2Vstms, r_base.GetReg(), rs_fr0.GetReg(), count);
}

void ArmMir2Lir::GenMultiplyByTwoBitMultiplier(RegLocation rl_src,
                                               RegLocation rl_result, int lit,
                                               int first_bit, int second_bit) {
  OpRegRegRegShift(kOpAdd, rl_result.reg, rl_src.reg, rl_src.reg,
                   EncodeShift(kArmLsl, second_bit - first_bit));
  if (first_bit != 0) {
    OpRegRegImm(kOpLsl, rl_result.reg, rl_result.reg, first_bit);
  }
}

void ArmMir2Lir::GenDivZeroCheckWide(RegStorage reg) {
  DCHECK(reg.IsPair());   // TODO: support k64BitSolo.
  RegStorage t_reg = AllocTemp();
  NewLIR4(kThumb2OrrRRRs, t_reg.GetReg(), reg.GetLowReg(), reg.GetHighReg(), 0);
  FreeTemp(t_reg);
  GenDivZeroCheck(kCondEq);
}

// Test suspend flag, return target of taken suspend branch
LIR* ArmMir2Lir::OpTestSuspend(LIR* target) {
  NewLIR2(kThumbSubRI8, rs_rARM_SUSPEND.GetReg(), 1);
  return OpCondBranch((target == NULL) ? kCondEq : kCondNe, target);
}

// Decrement register and branch on condition
LIR* ArmMir2Lir::OpDecAndBranch(ConditionCode c_code, RegStorage reg, LIR* target) {
  // Combine sub & test using sub setflags encoding here
  OpRegRegImm(kOpSub, reg, reg, 1);  // For value == 1, this should set flags.
  DCHECK(last_lir_insn_->u.m.def_mask & ENCODE_CCODE);
  return OpCondBranch(c_code, target);
}

bool ArmMir2Lir::GenMemBarrier(MemBarrierKind barrier_kind) {
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
      || (barrier != nullptr && (barrier->opcode != kThumb2Dmb || barrier->operands[0] != dmb_flavor))) {
    barrier = NewLIR1(kThumb2Dmb, dmb_flavor);
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

void ArmMir2Lir::GenNegLong(RegLocation rl_dest, RegLocation rl_src) {
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

void ArmMir2Lir::GenMulLong(Instruction::Code opcode, RegLocation rl_dest,
                            RegLocation rl_src1, RegLocation rl_src2) {
    /*
     * tmp1     = src1.hi * src2.lo;  // src1.hi is no longer needed
     * dest     = src1.lo * src2.lo;
     * tmp1    += src1.lo * src2.hi;
     * dest.hi += tmp1;
     *
     * To pull off inline multiply, we have a worst-case requirement of 7 temporary
     * registers.  Normally for Arm, we get 5.  We can get to 6 by including
     * lr in the temp set.  The only problematic case is all operands and result are
     * distinct, and none have been promoted.  In that case, we can succeed by aggressively
     * freeing operand temp registers after they are no longer needed.  All other cases
     * can proceed normally.  We'll just punt on the case of the result having a misaligned
     * overlap with either operand and send that case to a runtime handler.
     */
    RegLocation rl_result;
    if (BadOverlap(rl_src1, rl_dest) || (BadOverlap(rl_src2, rl_dest))) {
      ThreadOffset<4> func_offset = QUICK_ENTRYPOINT_OFFSET(4, pLmul);
      FlushAllRegs();
      CallRuntimeHelperRegLocationRegLocation(func_offset, rl_src1, rl_src2, false);
      rl_result = GetReturnWide(false);
      StoreValueWide(rl_dest, rl_result);
      return;
    }

    rl_src1 = LoadValueWide(rl_src1, kCoreReg);
    rl_src2 = LoadValueWide(rl_src2, kCoreReg);

    int reg_status = 0;
    RegStorage res_lo;
    RegStorage res_hi;
    bool dest_promoted = rl_dest.location == kLocPhysReg && rl_dest.reg.Valid() &&
        !IsTemp(rl_dest.reg.GetLow()) && !IsTemp(rl_dest.reg.GetHigh());
    bool src1_promoted = !IsTemp(rl_src1.reg.GetLow()) && !IsTemp(rl_src1.reg.GetHigh());
    bool src2_promoted = !IsTemp(rl_src2.reg.GetLow()) && !IsTemp(rl_src2.reg.GetHigh());
    // Check if rl_dest is *not* either operand and we have enough temp registers.
    if ((rl_dest.s_reg_low != rl_src1.s_reg_low && rl_dest.s_reg_low != rl_src2.s_reg_low) &&
        (dest_promoted || src1_promoted || src2_promoted)) {
      // In this case, we do not need to manually allocate temp registers for result.
      rl_result = EvalLoc(rl_dest, kCoreReg, true);
      res_lo = rl_result.reg.GetLow();
      res_hi = rl_result.reg.GetHigh();
    } else {
      res_lo = AllocTemp();
      if ((rl_src1.s_reg_low == rl_src2.s_reg_low) || src1_promoted || src2_promoted) {
        // In this case, we have enough temp registers to be allocated for result.
        res_hi = AllocTemp();
        reg_status = 1;
      } else {
        // In this case, all temps are now allocated.
        // res_hi will be allocated after we can free src1_hi.
        reg_status = 2;
      }
    }

    // Temporarily add LR to the temp pool, and assign it to tmp1
    MarkTemp(rs_rARM_LR);
    FreeTemp(rs_rARM_LR);
    RegStorage tmp1 = rs_rARM_LR;
    LockTemp(rs_rARM_LR);

    if (rl_src1.reg == rl_src2.reg) {
      DCHECK(res_hi.Valid());
      DCHECK(res_lo.Valid());
      NewLIR3(kThumb2MulRRR, tmp1.GetReg(), rl_src1.reg.GetLowReg(), rl_src1.reg.GetHighReg());
      NewLIR4(kThumb2Umull, res_lo.GetReg(), res_hi.GetReg(), rl_src1.reg.GetLowReg(),
              rl_src1.reg.GetLowReg());
      OpRegRegRegShift(kOpAdd, res_hi, res_hi, tmp1, EncodeShift(kArmLsl, 1));
    } else {
      NewLIR3(kThumb2MulRRR, tmp1.GetReg(), rl_src2.reg.GetLowReg(), rl_src1.reg.GetHighReg());
      if (reg_status == 2) {
        DCHECK(!res_hi.Valid());
        DCHECK_NE(rl_src1.reg.GetLowReg(), rl_src2.reg.GetLowReg());
        DCHECK_NE(rl_src1.reg.GetHighReg(), rl_src2.reg.GetHighReg());
        FreeTemp(rl_src1.reg.GetHigh());
        res_hi = AllocTemp();
      }
      DCHECK(res_hi.Valid());
      DCHECK(res_lo.Valid());
      NewLIR4(kThumb2Umull, res_lo.GetReg(), res_hi.GetReg(), rl_src2.reg.GetLowReg(),
              rl_src1.reg.GetLowReg());
      NewLIR4(kThumb2Mla, tmp1.GetReg(), rl_src1.reg.GetLowReg(), rl_src2.reg.GetHighReg(),
              tmp1.GetReg());
      NewLIR4(kThumb2AddRRR, res_hi.GetReg(), tmp1.GetReg(), res_hi.GetReg(), 0);
      if (reg_status == 2) {
        // Clobber rl_src1 since it was corrupted.
        FreeTemp(rl_src1.reg);
        Clobber(rl_src1.reg);
      }
    }

    // Now, restore lr to its non-temp status.
    FreeTemp(tmp1);
    Clobber(rs_rARM_LR);
    UnmarkTemp(rs_rARM_LR);

    if (reg_status != 0) {
      // We had manually allocated registers for rl_result.
      // Now construct a RegLocation.
      rl_result = GetReturnWide(false);  // Just using as a template.
      rl_result.reg = RegStorage::MakeRegPair(res_lo, res_hi);
    }

    StoreValueWide(rl_dest, rl_result);
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
      OpRegRegRegShift(kOpAdd, reg_ptr, rl_array.reg, rl_index.reg, EncodeShift(kArmLsl, scale));
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
void ArmMir2Lir::GenArrayPut(int opt_flags, OpSize size, RegLocation rl_array,
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
      OpRegRegRegShift(kOpAdd, reg_ptr, rl_array.reg, rl_index.reg, EncodeShift(kArmLsl, scale));
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
        OpRegRegRegShift(kOpOr, rl_result.reg.GetHigh(), rl_result.reg.GetHigh(), rl_src.reg.GetLow(),
                         EncodeShift(kArmLsr, 32 - shift_amount));
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
        OpRegRegRegShift(kOpOr, rl_result.reg.GetLow(), t_reg, rl_src.reg.GetHigh(),
                         EncodeShift(kArmLsl, 32 - shift_amount));
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
        OpRegRegRegShift(kOpOr, rl_result.reg.GetLow(), t_reg, rl_src.reg.GetHigh(),
                         EncodeShift(kArmLsl, 32 - shift_amount));
        FreeTemp(t_reg);
        OpRegRegImm(kOpLsr, rl_result.reg.GetHigh(), rl_src.reg.GetHigh(), shift_amount);
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
    default:
      LOG(FATAL) << "Unexpected opcode " << opcode;
  }
  StoreValueWide(rl_dest, rl_result);
}

}  // namespace art
