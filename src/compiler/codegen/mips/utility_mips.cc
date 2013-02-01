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

#include "codegen_mips.h"
#include "compiler/codegen/codegen_util.h"
#include "compiler/codegen/ralloc_util.h"
#include "mips_lir.h"

namespace art {

/* This file contains codegen for the MIPS32 ISA. */
LIR* MipsCodegen::OpFpRegCopy(CompilationUnit *cu, int r_dest, int r_src)
{
  int opcode;
  /* must be both DOUBLE or both not DOUBLE */
  DCHECK_EQ(MIPS_DOUBLEREG(r_dest),MIPS_DOUBLEREG(r_src));
  if (MIPS_DOUBLEREG(r_dest)) {
    opcode = kMipsFmovd;
  } else {
    if (MIPS_SINGLEREG(r_dest)) {
      if (MIPS_SINGLEREG(r_src)) {
        opcode = kMipsFmovs;
      } else {
        /* note the operands are swapped for the mtc1 instr */
        int t_opnd = r_src;
        r_src = r_dest;
        r_dest = t_opnd;
        opcode = kMipsMtc1;
      }
    } else {
      DCHECK(MIPS_SINGLEREG(r_src));
      opcode = kMipsMfc1;
    }
  }
  LIR* res = RawLIR(cu, cu->current_dalvik_offset, opcode, r_src, r_dest);
  if (!(cu->disable_opt & (1 << kSafeOptimizations)) && r_dest == r_src) {
    res->flags.is_nop = true;
  }
  return res;
}

bool MipsCodegen::InexpensiveConstant(int reg, int value)
{
  bool res = false;
  if (value == 0) {
    res = true;
  } else if (IsUint(16, value)) {
    res = true;
  } else if ((value < 0) && (value >= -32768)) {
    res = true;
  }
  return res;
}

/*
 * Load a immediate using a shortcut if possible; otherwise
 * grab from the per-translation literal pool.  If target is
 * a high register, build constant into a low register and copy.
 *
 * No additional register clobbering operation performed. Use this version when
 * 1) r_dest is freshly returned from AllocTemp or
 * 2) The codegen is under fixed register usage
 */
LIR* MipsCodegen::LoadConstantNoClobber(CompilationUnit *cu, int r_dest, int value)
{
  LIR *res;

  int r_dest_save = r_dest;
  int is_fp_reg = MIPS_FPREG(r_dest);
  if (is_fp_reg) {
    DCHECK(MIPS_SINGLEREG(r_dest));
    r_dest = AllocTemp(cu);
  }

  /* See if the value can be constructed cheaply */
  if (value == 0) {
    res = NewLIR2(cu, kMipsMove, r_dest, r_ZERO);
  } else if ((value > 0) && (value <= 65535)) {
    res = NewLIR3(cu, kMipsOri, r_dest, r_ZERO, value);
  } else if ((value < 0) && (value >= -32768)) {
    res = NewLIR3(cu, kMipsAddiu, r_dest, r_ZERO, value);
  } else {
    res = NewLIR2(cu, kMipsLui, r_dest, value>>16);
    if (value & 0xffff)
      NewLIR3(cu, kMipsOri, r_dest, r_dest, value);
  }

  if (is_fp_reg) {
    NewLIR2(cu, kMipsMtc1, r_dest, r_dest_save);
    FreeTemp(cu, r_dest);
  }

  return res;
}

LIR* MipsCodegen::OpUnconditionalBranch(CompilationUnit* cu, LIR* target)
{
  LIR* res = NewLIR1(cu, kMipsB, 0 /* offset to be patched during assembly*/ );
  res->target = target;
  return res;
}

LIR* MipsCodegen::OpReg(CompilationUnit *cu, OpKind op, int r_dest_src)
{
  MipsOpCode opcode = kMipsNop;
  switch (op) {
    case kOpBlx:
      opcode = kMipsJalr;
      break;
    case kOpBx:
      return NewLIR1(cu, kMipsJr, r_dest_src);
      break;
    default:
      LOG(FATAL) << "Bad case in OpReg";
  }
  return NewLIR2(cu, opcode, r_RA, r_dest_src);
}

LIR* MipsCodegen::OpRegImm(CompilationUnit *cu, OpKind op, int r_dest_src1,
          int value)
{
  LIR *res;
  bool neg = (value < 0);
  int abs_value = (neg) ? -value : value;
  bool short_form = (abs_value & 0xff) == abs_value;
  MipsOpCode opcode = kMipsNop;
  switch (op) {
    case kOpAdd:
      return OpRegRegImm(cu, op, r_dest_src1, r_dest_src1, value);
      break;
    case kOpSub:
      return OpRegRegImm(cu, op, r_dest_src1, r_dest_src1, value);
      break;
    default:
      LOG(FATAL) << "Bad case in OpRegImm";
      break;
  }
  if (short_form)
    res = NewLIR2(cu, opcode, r_dest_src1, abs_value);
  else {
    int r_scratch = AllocTemp(cu);
    res = LoadConstant(cu, r_scratch, value);
    if (op == kOpCmp)
      NewLIR2(cu, opcode, r_dest_src1, r_scratch);
    else
      NewLIR3(cu, opcode, r_dest_src1, r_dest_src1, r_scratch);
  }
  return res;
}

LIR* MipsCodegen::OpRegRegReg(CompilationUnit *cu, OpKind op, int r_dest, int r_src1, int r_src2)
{
  MipsOpCode opcode = kMipsNop;
  switch (op) {
    case kOpAdd:
      opcode = kMipsAddu;
      break;
    case kOpSub:
      opcode = kMipsSubu;
      break;
    case kOpAnd:
      opcode = kMipsAnd;
      break;
    case kOpMul:
      opcode = kMipsMul;
      break;
    case kOpOr:
      opcode = kMipsOr;
      break;
    case kOpXor:
      opcode = kMipsXor;
      break;
    case kOpLsl:
      opcode = kMipsSllv;
      break;
    case kOpLsr:
      opcode = kMipsSrlv;
      break;
    case kOpAsr:
      opcode = kMipsSrav;
      break;
    case kOpAdc:
    case kOpSbc:
      LOG(FATAL) << "No carry bit on MIPS";
      break;
    default:
      LOG(FATAL) << "bad case in OpRegRegReg";
      break;
  }
  return NewLIR3(cu, opcode, r_dest, r_src1, r_src2);
}

LIR* MipsCodegen::OpRegRegImm(CompilationUnit *cu, OpKind op, int r_dest, int r_src1, int value)
{
  LIR *res;
  MipsOpCode opcode = kMipsNop;
  bool short_form = true;

  switch (op) {
    case kOpAdd:
      if (IS_SIMM16(value)) {
        opcode = kMipsAddiu;
      }
      else {
        short_form = false;
        opcode = kMipsAddu;
      }
      break;
    case kOpSub:
      if (IS_SIMM16((-value))) {
        value = -value;
        opcode = kMipsAddiu;
      }
      else {
        short_form = false;
        opcode = kMipsSubu;
      }
      break;
    case kOpLsl:
        DCHECK(value >= 0 && value <= 31);
        opcode = kMipsSll;
        break;
    case kOpLsr:
        DCHECK(value >= 0 && value <= 31);
        opcode = kMipsSrl;
        break;
    case kOpAsr:
        DCHECK(value >= 0 && value <= 31);
        opcode = kMipsSra;
        break;
    case kOpAnd:
      if (IS_UIMM16((value))) {
        opcode = kMipsAndi;
      }
      else {
        short_form = false;
        opcode = kMipsAnd;
      }
      break;
    case kOpOr:
      if (IS_UIMM16((value))) {
        opcode = kMipsOri;
      }
      else {
        short_form = false;
        opcode = kMipsOr;
      }
      break;
    case kOpXor:
      if (IS_UIMM16((value))) {
        opcode = kMipsXori;
      }
      else {
        short_form = false;
        opcode = kMipsXor;
      }
      break;
    case kOpMul:
      short_form = false;
      opcode = kMipsMul;
      break;
    default:
      LOG(FATAL) << "Bad case in OpRegRegImm";
      break;
  }

  if (short_form)
    res = NewLIR3(cu, opcode, r_dest, r_src1, value);
  else {
    if (r_dest != r_src1) {
      res = LoadConstant(cu, r_dest, value);
      NewLIR3(cu, opcode, r_dest, r_src1, r_dest);
    } else {
      int r_scratch = AllocTemp(cu);
      res = LoadConstant(cu, r_scratch, value);
      NewLIR3(cu, opcode, r_dest, r_src1, r_scratch);
    }
  }
  return res;
}

LIR* MipsCodegen::OpRegReg(CompilationUnit *cu, OpKind op, int r_dest_src1, int r_src2)
{
  MipsOpCode opcode = kMipsNop;
  LIR *res;
  switch (op) {
    case kOpMov:
      opcode = kMipsMove;
      break;
    case kOpMvn:
      return NewLIR3(cu, kMipsNor, r_dest_src1, r_src2, r_ZERO);
    case kOpNeg:
      return NewLIR3(cu, kMipsSubu, r_dest_src1, r_ZERO, r_src2);
    case kOpAdd:
    case kOpAnd:
    case kOpMul:
    case kOpOr:
    case kOpSub:
    case kOpXor:
      return OpRegRegReg(cu, op, r_dest_src1, r_dest_src1, r_src2);
    case kOp2Byte:
#if __mips_isa_rev>=2
      res = NewLIR2(cu, kMipsSeb, r_dest_src1, r_src2);
#else
      res = OpRegRegImm(cu, kOpLsl, r_dest_src1, r_src2, 24);
      OpRegRegImm(cu, kOpAsr, r_dest_src1, r_dest_src1, 24);
#endif
      return res;
    case kOp2Short:
#if __mips_isa_rev>=2
      res = NewLIR2(cu, kMipsSeh, r_dest_src1, r_src2);
#else
      res = OpRegRegImm(cu, kOpLsl, r_dest_src1, r_src2, 16);
      OpRegRegImm(cu, kOpAsr, r_dest_src1, r_dest_src1, 16);
#endif
      return res;
    case kOp2Char:
       return NewLIR3(cu, kMipsAndi, r_dest_src1, r_src2, 0xFFFF);
    default:
      LOG(FATAL) << "Bad case in OpRegReg";
      break;
  }
  return NewLIR2(cu, opcode, r_dest_src1, r_src2);
}

LIR* MipsCodegen::LoadConstantValueWide(CompilationUnit *cu, int r_dest_lo, int r_dest_hi,
                                        int val_lo, int val_hi)
{
  LIR *res;
  res = LoadConstantNoClobber(cu, r_dest_lo, val_lo);
  LoadConstantNoClobber(cu, r_dest_hi, val_hi);
  return res;
}

/* Load value from base + scaled index. */
LIR* MipsCodegen::LoadBaseIndexed(CompilationUnit *cu, int rBase, int r_index, int r_dest,
                                  int scale, OpSize size)
{
  LIR *first = NULL;
  LIR *res;
  MipsOpCode opcode = kMipsNop;
  int t_reg = AllocTemp(cu);

  if (MIPS_FPREG(r_dest)) {
    DCHECK(MIPS_SINGLEREG(r_dest));
    DCHECK((size == kWord) || (size == kSingle));
    size = kSingle;
  } else {
    if (size == kSingle)
      size = kWord;
  }

  if (!scale) {
    first = NewLIR3(cu, kMipsAddu, t_reg , rBase, r_index);
  } else {
    first = OpRegRegImm(cu, kOpLsl, t_reg, r_index, scale);
    NewLIR3(cu, kMipsAddu, t_reg , rBase, t_reg);
  }

  switch (size) {
    case kSingle:
      opcode = kMipsFlwc1;
      break;
    case kWord:
      opcode = kMipsLw;
      break;
    case kUnsignedHalf:
      opcode = kMipsLhu;
      break;
    case kSignedHalf:
      opcode = kMipsLh;
      break;
    case kUnsignedByte:
      opcode = kMipsLbu;
      break;
    case kSignedByte:
      opcode = kMipsLb;
      break;
    default:
      LOG(FATAL) << "Bad case in LoadBaseIndexed";
  }

  res = NewLIR3(cu, opcode, r_dest, 0, t_reg);
  FreeTemp(cu, t_reg);
  return (first) ? first : res;
}

/* store value base base + scaled index. */
LIR* MipsCodegen::StoreBaseIndexed(CompilationUnit *cu, int rBase, int r_index, int r_src,
                                   int scale, OpSize size)
{
  LIR *first = NULL;
  MipsOpCode opcode = kMipsNop;
  int r_new_index = r_index;
  int t_reg = AllocTemp(cu);

  if (MIPS_FPREG(r_src)) {
    DCHECK(MIPS_SINGLEREG(r_src));
    DCHECK((size == kWord) || (size == kSingle));
    size = kSingle;
  } else {
    if (size == kSingle)
      size = kWord;
  }

  if (!scale) {
    first = NewLIR3(cu, kMipsAddu, t_reg , rBase, r_index);
  } else {
    first = OpRegRegImm(cu, kOpLsl, t_reg, r_index, scale);
    NewLIR3(cu, kMipsAddu, t_reg , rBase, t_reg);
  }

  switch (size) {
    case kSingle:
      opcode = kMipsFswc1;
      break;
    case kWord:
      opcode = kMipsSw;
      break;
    case kUnsignedHalf:
    case kSignedHalf:
      opcode = kMipsSh;
      break;
    case kUnsignedByte:
    case kSignedByte:
      opcode = kMipsSb;
      break;
    default:
      LOG(FATAL) << "Bad case in StoreBaseIndexed";
  }
  NewLIR3(cu, opcode, r_src, 0, t_reg);
  FreeTemp(cu, r_new_index);
  return first;
}

LIR* MipsCodegen::LoadBaseDispBody(CompilationUnit *cu, int rBase, int displacement, int r_dest,
                                   int r_dest_hi, OpSize size, int s_reg)
/*
 * Load value from base + displacement.  Optionally perform null check
 * on base (which must have an associated s_reg and MIR).  If not
 * performing null check, incoming MIR can be null. IMPORTANT: this
 * code must not allocate any new temps.  If a new register is needed
 * and base and dest are the same, spill some other register to
 * rlp and then restore.
 */
{
  LIR *res;
  LIR *load = NULL;
  LIR *load2 = NULL;
  MipsOpCode opcode = kMipsNop;
  bool short_form = IS_SIMM16(displacement);
  bool pair = false;

  switch (size) {
    case kLong:
    case kDouble:
      pair = true;
      opcode = kMipsLw;
      if (MIPS_FPREG(r_dest)) {
        opcode = kMipsFlwc1;
        if (MIPS_DOUBLEREG(r_dest)) {
          r_dest = r_dest - MIPS_FP_DOUBLE;
        } else {
          DCHECK(MIPS_FPREG(r_dest_hi));
          DCHECK(r_dest == (r_dest_hi - 1));
        }
        r_dest_hi = r_dest + 1;
      }
      short_form = IS_SIMM16_2WORD(displacement);
      DCHECK_EQ((displacement & 0x3), 0);
      break;
    case kWord:
    case kSingle:
      opcode = kMipsLw;
      if (MIPS_FPREG(r_dest)) {
        opcode = kMipsFlwc1;
        DCHECK(MIPS_SINGLEREG(r_dest));
      }
      DCHECK_EQ((displacement & 0x3), 0);
      break;
    case kUnsignedHalf:
      opcode = kMipsLhu;
      DCHECK_EQ((displacement & 0x1), 0);
      break;
    case kSignedHalf:
      opcode = kMipsLh;
      DCHECK_EQ((displacement & 0x1), 0);
      break;
    case kUnsignedByte:
      opcode = kMipsLbu;
      break;
    case kSignedByte:
      opcode = kMipsLb;
      break;
    default:
      LOG(FATAL) << "Bad case in LoadBaseIndexedBody";
  }

  if (short_form) {
    if (!pair) {
      load = res = NewLIR3(cu, opcode, r_dest, displacement, rBase);
    } else {
      load = res = NewLIR3(cu, opcode, r_dest,
                           displacement + LOWORD_OFFSET, rBase);
      load2 = NewLIR3(cu, opcode, r_dest_hi,
                      displacement + HIWORD_OFFSET, rBase);
    }
  } else {
    if (pair) {
      int r_tmp = AllocFreeTemp(cu);
      res = OpRegRegImm(cu, kOpAdd, r_tmp, rBase, displacement);
      load = NewLIR3(cu, opcode, r_dest, LOWORD_OFFSET, r_tmp);
      load2 = NewLIR3(cu, opcode, r_dest_hi, HIWORD_OFFSET, r_tmp);
      FreeTemp(cu, r_tmp);
    } else {
      int r_tmp = (rBase == r_dest) ? AllocFreeTemp(cu) : r_dest;
      res = OpRegRegImm(cu, kOpAdd, r_tmp, rBase, displacement);
      load = NewLIR3(cu, opcode, r_dest, 0, r_tmp);
      if (r_tmp != r_dest)
        FreeTemp(cu, r_tmp);
    }
  }

  if (rBase == rMIPS_SP) {
    AnnotateDalvikRegAccess(cu, load, (displacement + (pair ? LOWORD_OFFSET : 0)) >> 2,
                            true /* is_load */, pair /* is64bit */);
    if (pair) {
      AnnotateDalvikRegAccess(cu, load2, (displacement + HIWORD_OFFSET) >> 2,
                              true /* is_load */, pair /* is64bit */);
    }
  }
  return load;
}

LIR* MipsCodegen::LoadBaseDisp(CompilationUnit *cu, int rBase, int displacement, int r_dest,
                               OpSize size, int s_reg)
{
  return LoadBaseDispBody(cu, rBase, displacement, r_dest, -1,
                          size, s_reg);
}

LIR* MipsCodegen::LoadBaseDispWide(CompilationUnit *cu, int rBase, int displacement,
                                   int r_dest_lo, int r_dest_hi, int s_reg)
{
  return LoadBaseDispBody(cu, rBase, displacement, r_dest_lo, r_dest_hi, kLong, s_reg);
}

LIR* MipsCodegen::StoreBaseDispBody(CompilationUnit *cu, int rBase, int displacement,
                                    int r_src, int r_src_hi, OpSize size)
{
  LIR *res;
  LIR *store = NULL;
  LIR *store2 = NULL;
  MipsOpCode opcode = kMipsNop;
  bool short_form = IS_SIMM16(displacement);
  bool pair = false;

  switch (size) {
    case kLong:
    case kDouble:
      pair = true;
      opcode = kMipsSw;
      if (MIPS_FPREG(r_src)) {
        opcode = kMipsFswc1;
        if (MIPS_DOUBLEREG(r_src)) {
          r_src = r_src - MIPS_FP_DOUBLE;
        } else {
          DCHECK(MIPS_FPREG(r_src_hi));
          DCHECK_EQ(r_src, (r_src_hi - 1));
        }
        r_src_hi = r_src + 1;
      }
      short_form = IS_SIMM16_2WORD(displacement);
      DCHECK_EQ((displacement & 0x3), 0);
      break;
    case kWord:
    case kSingle:
      opcode = kMipsSw;
      if (MIPS_FPREG(r_src)) {
        opcode = kMipsFswc1;
        DCHECK(MIPS_SINGLEREG(r_src));
      }
      DCHECK_EQ((displacement & 0x3), 0);
      break;
    case kUnsignedHalf:
    case kSignedHalf:
      opcode = kMipsSh;
      DCHECK_EQ((displacement & 0x1), 0);
      break;
    case kUnsignedByte:
    case kSignedByte:
      opcode = kMipsSb;
      break;
    default:
      LOG(FATAL) << "Bad case in StoreBaseIndexedBody";
  }

  if (short_form) {
    if (!pair) {
      store = res = NewLIR3(cu, opcode, r_src, displacement, rBase);
    } else {
      store = res = NewLIR3(cu, opcode, r_src, displacement + LOWORD_OFFSET,
                            rBase);
      store2 = NewLIR3(cu, opcode, r_src_hi, displacement + HIWORD_OFFSET,
                       rBase);
    }
  } else {
    int r_scratch = AllocTemp(cu);
    res = OpRegRegImm(cu, kOpAdd, r_scratch, rBase, displacement);
    if (!pair) {
      store =  NewLIR3(cu, opcode, r_src, 0, r_scratch);
    } else {
      store =  NewLIR3(cu, opcode, r_src, LOWORD_OFFSET, r_scratch);
      store2 = NewLIR3(cu, opcode, r_src_hi, HIWORD_OFFSET, r_scratch);
    }
    FreeTemp(cu, r_scratch);
  }

  if (rBase == rMIPS_SP) {
    AnnotateDalvikRegAccess(cu, store, (displacement + (pair ? LOWORD_OFFSET : 0)) >> 2,
                            false /* is_load */, pair /* is64bit */);
    if (pair) {
      AnnotateDalvikRegAccess(cu, store2, (displacement + HIWORD_OFFSET) >> 2,
                              false /* is_load */, pair /* is64bit */);
    }
  }

  return res;
}

LIR* MipsCodegen::StoreBaseDisp(CompilationUnit *cu, int rBase, int displacement, int r_src,
                                OpSize size)
{
  return StoreBaseDispBody(cu, rBase, displacement, r_src, -1, size);
}

LIR* MipsCodegen::StoreBaseDispWide(CompilationUnit *cu, int rBase, int displacement,
                                    int r_src_lo, int r_src_hi)
{
  return StoreBaseDispBody(cu, rBase, displacement, r_src_lo, r_src_hi, kLong);
}

LIR* MipsCodegen::OpThreadMem(CompilationUnit* cu, OpKind op, int thread_offset)
{
  LOG(FATAL) << "Unexpected use of OpThreadMem for MIPS";
  return NULL;
}

LIR* MipsCodegen::OpMem(CompilationUnit* cu, OpKind op, int rBase, int disp)
{
  LOG(FATAL) << "Unexpected use of OpMem for MIPS";
  return NULL;
}

LIR* MipsCodegen::StoreBaseIndexedDisp(CompilationUnit *cu,
                          int rBase, int r_index, int scale, int displacement,
                          int r_src, int r_src_hi,
                          OpSize size, int s_reg)
{
  LOG(FATAL) << "Unexpected use of StoreBaseIndexedDisp for MIPS";
  return NULL;
}

LIR* MipsCodegen::OpRegMem(CompilationUnit *cu, OpKind op, int r_dest, int rBase,
              int offset)
{
  LOG(FATAL) << "Unexpected use of OpRegMem for MIPS";
  return NULL;
}

LIR* MipsCodegen::LoadBaseIndexedDisp(CompilationUnit *cu,
                         int rBase, int r_index, int scale, int displacement,
                         int r_dest, int r_dest_hi,
                         OpSize size, int s_reg)
{
  LOG(FATAL) << "Unexpected use of LoadBaseIndexedDisp for MIPS";
  return NULL;
}

LIR* MipsCodegen::OpCondBranch(CompilationUnit* cu, ConditionCode cc, LIR* target)
{
  LOG(FATAL) << "Unexpected use of OpCondBranch for MIPS";
  return NULL;
}

}  // namespace art
