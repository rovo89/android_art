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

#include "x86_lir.h"
#include "../codegen_util.h"
#include "../ralloc_util.h"

namespace art {

/* This file contains codegen for the X86 ISA */

void GenBarrier(CompilationUnit *cUnit);
void LoadPair(CompilationUnit *cUnit, int base, int lowReg, int highReg);
LIR *LoadWordDisp(CompilationUnit *cUnit, int rBase, int displacement,
                      int rDest);
LIR *StoreWordDisp(CompilationUnit *cUnit, int rBase,
                       int displacement, int rSrc);
LIR *LoadConstant(CompilationUnit *cUnit, int rDest, int value);

LIR *FpRegCopy(CompilationUnit *cUnit, int rDest, int rSrc)
{
  int opcode;
  /* must be both DOUBLE or both not DOUBLE */
  DCHECK_EQ(X86_DOUBLEREG(rDest), X86_DOUBLEREG(rSrc));
  if (X86_DOUBLEREG(rDest)) {
    opcode = kX86MovsdRR;
  } else {
    if (X86_SINGLEREG(rDest)) {
      if (X86_SINGLEREG(rSrc)) {
        opcode = kX86MovssRR;
      } else {  // Fpr <- Gpr
        opcode = kX86MovdxrRR;
      }
    } else {  // Gpr <- Fpr
      DCHECK(X86_SINGLEREG(rSrc));
      opcode = kX86MovdrxRR;
    }
  }
  DCHECK_NE((EncodingMap[opcode].flags & IS_BINARY_OP), 0ULL);
  LIR* res = RawLIR(cUnit, cUnit->currentDalvikOffset, opcode, rDest, rSrc);
  if (rDest == rSrc) {
    res->flags.isNop = true;
  }
  return res;
}

/*
 * Load a immediate using a shortcut if possible; otherwise
 * grab from the per-translation literal pool.  If target is
 * a high register, build constant into a low register and copy.
 *
 * No additional register clobbering operation performed. Use this version when
 * 1) rDest is freshly returned from AllocTemp or
 * 2) The codegen is under fixed register usage
 */
LIR *LoadConstantNoClobber(CompilationUnit *cUnit, int rDest, int value)
{
  int rDestSave = rDest;
  if (X86_FPREG(rDest)) {
    if (value == 0) {
      return NewLIR2(cUnit, kX86XorpsRR, rDest, rDest);
    }
    DCHECK(X86_SINGLEREG(rDest));
    rDest = AllocTemp(cUnit);
  }

  LIR *res;
  if (value == 0) {
    res = NewLIR2(cUnit, kX86Xor32RR, rDest, rDest);
  } else {
    // Note, there is no byte immediate form of a 32 bit immediate move.
    res = NewLIR2(cUnit, kX86Mov32RI, rDest, value);
  }

  if (X86_FPREG(rDestSave)) {
    NewLIR2(cUnit, kX86MovdxrRR, rDestSave, rDest);
    FreeTemp(cUnit, rDest);
  }

  return res;
}

LIR* OpBranchUnconditional(CompilationUnit *cUnit, OpKind op)
{
  CHECK_EQ(op, kOpUncondBr);
  return NewLIR1(cUnit, kX86Jmp8, 0 /* offset to be patched */ );
}

LIR *LoadMultiple(CompilationUnit *cUnit, int rBase, int rMask);

X86ConditionCode X86ConditionEncoding(ConditionCode cond);
LIR* OpCondBranch(CompilationUnit* cUnit, ConditionCode cc, LIR* target)
{
  LIR* branch = NewLIR2(cUnit, kX86Jcc8, 0 /* offset to be patched */,
                        X86ConditionEncoding(cc));
  branch->target = target;
  return branch;
}

LIR *OpReg(CompilationUnit *cUnit, OpKind op, int rDestSrc)
{
  X86OpCode opcode = kX86Bkpt;
  switch (op) {
    case kOpNeg: opcode = kX86Neg32R; break;
    case kOpNot: opcode = kX86Not32R; break;
    case kOpBlx: opcode = kX86CallR; break;
    default:
      LOG(FATAL) << "Bad case in OpReg " << op;
  }
  return NewLIR1(cUnit, opcode, rDestSrc);
}

LIR *OpRegImm(CompilationUnit *cUnit, OpKind op, int rDestSrc1, int value)
{
  X86OpCode opcode = kX86Bkpt;
  bool byteImm = IS_SIMM8(value);
  DCHECK(!X86_FPREG(rDestSrc1));
  switch (op) {
    case kOpLsl: opcode = kX86Sal32RI; break;
    case kOpLsr: opcode = kX86Shr32RI; break;
    case kOpAsr: opcode = kX86Sar32RI; break;
    case kOpAdd: opcode = byteImm ? kX86Add32RI8 : kX86Add32RI; break;
    case kOpOr:  opcode = byteImm ? kX86Or32RI8  : kX86Or32RI;  break;
    case kOpAdc: opcode = byteImm ? kX86Adc32RI8 : kX86Adc32RI; break;
    //case kOpSbb: opcode = kX86Sbb32RI; break;
    case kOpAnd: opcode = byteImm ? kX86And32RI8 : kX86And32RI; break;
    case kOpSub: opcode = byteImm ? kX86Sub32RI8 : kX86Sub32RI; break;
    case kOpXor: opcode = byteImm ? kX86Xor32RI8 : kX86Xor32RI; break;
    case kOpCmp: opcode = byteImm ? kX86Cmp32RI8 : kX86Cmp32RI; break;
    case kOpMov: return LoadConstantNoClobber(cUnit, rDestSrc1, value);
    case kOpMul:
      opcode = byteImm ? kX86Imul32RRI8 : kX86Imul32RRI;
      return NewLIR3(cUnit, opcode, rDestSrc1, rDestSrc1, value);
    default:
      LOG(FATAL) << "Bad case in OpRegImm " << op;
  }
  return NewLIR2(cUnit, opcode, rDestSrc1, value);
}

LIR *OpRegReg(CompilationUnit *cUnit, OpKind op, int rDestSrc1, int rSrc2)
{
    X86OpCode opcode = kX86Nop;
    bool src2_must_be_cx = false;
    switch (op) {
        // X86 unary opcodes
      case kOpMvn:
        OpRegCopy(cUnit, rDestSrc1, rSrc2);
        return OpReg(cUnit, kOpNot, rDestSrc1);
      case kOpNeg:
        OpRegCopy(cUnit, rDestSrc1, rSrc2);
        return OpReg(cUnit, kOpNeg, rDestSrc1);
        // X86 binary opcodes
      case kOpSub: opcode = kX86Sub32RR; break;
      case kOpSbc: opcode = kX86Sbb32RR; break;
      case kOpLsl: opcode = kX86Sal32RC; src2_must_be_cx = true; break;
      case kOpLsr: opcode = kX86Shr32RC; src2_must_be_cx = true; break;
      case kOpAsr: opcode = kX86Sar32RC; src2_must_be_cx = true; break;
      case kOpMov: opcode = kX86Mov32RR; break;
      case kOpCmp: opcode = kX86Cmp32RR; break;
      case kOpAdd: opcode = kX86Add32RR; break;
      case kOpAdc: opcode = kX86Adc32RR; break;
      case kOpAnd: opcode = kX86And32RR; break;
      case kOpOr:  opcode = kX86Or32RR; break;
      case kOpXor: opcode = kX86Xor32RR; break;
      case kOp2Byte:
        // Use shifts instead of a byte operand if the source can't be byte accessed.
        if (rSrc2 >= 4) {
          NewLIR2(cUnit, kX86Mov32RR, rDestSrc1, rSrc2);
          NewLIR2(cUnit, kX86Sal32RI, rDestSrc1, 24);
          return NewLIR2(cUnit, kX86Sar32RI, rDestSrc1, 24);
        } else {
          opcode = kX86Movsx8RR;
        }
        break;
      case kOp2Short: opcode = kX86Movsx16RR; break;
      case kOp2Char: opcode = kX86Movzx16RR; break;
      case kOpMul: opcode = kX86Imul32RR; break;
      default:
        LOG(FATAL) << "Bad case in OpRegReg " << op;
        break;
    }
    CHECK(!src2_must_be_cx || rSrc2 == rCX);
    return NewLIR2(cUnit, opcode, rDestSrc1, rSrc2);
}

LIR* OpRegMem(CompilationUnit *cUnit, OpKind op, int rDest, int rBase,
              int offset)
{
  X86OpCode opcode = kX86Nop;
  switch (op) {
      // X86 binary opcodes
    case kOpSub: opcode = kX86Sub32RM; break;
    case kOpMov: opcode = kX86Mov32RM; break;
    case kOpCmp: opcode = kX86Cmp32RM; break;
    case kOpAdd: opcode = kX86Add32RM; break;
    case kOpAnd: opcode = kX86And32RM; break;
    case kOpOr:  opcode = kX86Or32RM; break;
    case kOpXor: opcode = kX86Xor32RM; break;
    case kOp2Byte: opcode = kX86Movsx8RM; break;
    case kOp2Short: opcode = kX86Movsx16RM; break;
    case kOp2Char: opcode = kX86Movzx16RM; break;
    case kOpMul:
    default:
      LOG(FATAL) << "Bad case in OpRegMem " << op;
      break;
  }
  return NewLIR3(cUnit, opcode, rDest, rBase, offset);
}

LIR* OpRegRegReg(CompilationUnit *cUnit, OpKind op, int rDest, int rSrc1,
                 int rSrc2)
{
  if (rDest != rSrc1 && rDest != rSrc2) {
    if (op == kOpAdd) { // lea special case, except can't encode rbp as base
      if (rSrc1 == rSrc2) {
        OpRegCopy(cUnit, rDest, rSrc1);
        return OpRegImm(cUnit, kOpLsl, rDest, 1);
      } else if (rSrc1 != rBP) {
        return NewLIR5(cUnit, kX86Lea32RA, rDest, rSrc1 /* base */,
                       rSrc2 /* index */, 0 /* scale */, 0 /* disp */);
      } else {
        return NewLIR5(cUnit, kX86Lea32RA, rDest, rSrc2 /* base */,
                       rSrc1 /* index */, 0 /* scale */, 0 /* disp */);
      }
    } else {
      OpRegCopy(cUnit, rDest, rSrc1);
      return OpRegReg(cUnit, op, rDest, rSrc2);
    }
  } else if (rDest == rSrc1) {
    return OpRegReg(cUnit, op, rDest, rSrc2);
  } else {  // rDest == rSrc2
    switch (op) {
      case kOpSub:  // non-commutative
        OpReg(cUnit, kOpNeg, rDest);
        op = kOpAdd;
        break;
      case kOpSbc:
      case kOpLsl: case kOpLsr: case kOpAsr: case kOpRor: {
        int tReg = AllocTemp(cUnit);
        OpRegCopy(cUnit, tReg, rSrc1);
        OpRegReg(cUnit, op, tReg, rSrc2);
        LIR* res = OpRegCopy(cUnit, rDest, tReg);
        FreeTemp(cUnit, tReg);
        return res;
      }
      case kOpAdd:  // commutative
      case kOpOr:
      case kOpAdc:
      case kOpAnd:
      case kOpXor:
        break;
      default:
        LOG(FATAL) << "Bad case in OpRegRegReg " << op;
    }
    return OpRegReg(cUnit, op, rDest, rSrc1);
  }
}

LIR* OpRegRegImm(CompilationUnit *cUnit, OpKind op, int rDest, int rSrc,
                 int value)
{
  if (op == kOpMul) {
    X86OpCode opcode = IS_SIMM8(value) ? kX86Imul32RRI8 : kX86Imul32RRI;
    return NewLIR3(cUnit, opcode, rDest, rSrc, value);
  } else if (op == kOpAnd) {
    if (value == 0xFF && rSrc < 4) {
      return NewLIR2(cUnit, kX86Movzx8RR, rDest, rSrc);
    } else if (value == 0xFFFF) {
      return NewLIR2(cUnit, kX86Movzx16RR, rDest, rSrc);
    }
  }
  if (rDest != rSrc) {
    if (false && op == kOpLsl && value >= 0 && value <= 3) { // lea shift special case
      // TODO: fix bug in LEA encoding when disp == 0
      return NewLIR5(cUnit, kX86Lea32RA, rDest,  r5sib_no_base /* base */,
                     rSrc /* index */, value /* scale */, 0 /* disp */);
    } else if (op == kOpAdd) { // lea add special case
      return NewLIR5(cUnit, kX86Lea32RA, rDest, rSrc /* base */,
                     r4sib_no_index /* index */, 0 /* scale */, value /* disp */);
    }
    OpRegCopy(cUnit, rDest, rSrc);
  }
  return OpRegImm(cUnit, op, rDest, value);
}

LIR* OpThreadMem(CompilationUnit* cUnit, OpKind op, int threadOffset)
{
  X86OpCode opcode = kX86Bkpt;
  switch (op) {
    case kOpBlx: opcode = kX86CallT;  break;
    default:
      LOG(FATAL) << "Bad opcode: " << op;
      break;
  }
  return NewLIR1(cUnit, opcode, threadOffset);
}

LIR* OpMem(CompilationUnit* cUnit, OpKind op, int rBase, int disp)
{
  X86OpCode opcode = kX86Bkpt;
  switch (op) {
    case kOpBlx: opcode = kX86CallM;  break;
    default:
      LOG(FATAL) << "Bad opcode: " << op;
      break;
  }
  return NewLIR2(cUnit, opcode, rBase, disp);
}

LIR *LoadConstantValueWide(CompilationUnit *cUnit, int rDestLo,
                           int rDestHi, int valLo, int valHi)
{
    LIR *res;
    if (X86_FPREG(rDestLo)) {
      DCHECK(X86_FPREG(rDestHi));  // ignore rDestHi
      if (valLo == 0 && valHi == 0) {
        return NewLIR2(cUnit, kX86XorpsRR, rDestLo, rDestLo);
      } else {
        if (valLo == 0) {
          res = NewLIR2(cUnit, kX86XorpsRR, rDestLo, rDestLo);
        } else {
          res = LoadConstantNoClobber(cUnit, rDestLo, valLo);
        }
        if (valHi != 0) {
          LoadConstantNoClobber(cUnit, rDestHi, valHi);
          NewLIR2(cUnit, kX86PsllqRI, rDestHi, 32);
          NewLIR2(cUnit, kX86OrpsRR, rDestLo, rDestHi);
        }
      }
    } else {
      res = LoadConstantNoClobber(cUnit, rDestLo, valLo);
      LoadConstantNoClobber(cUnit, rDestHi, valHi);
    }
    return res;
}

LIR *LoadMultiple(CompilationUnit *cUnit, int rBase, int rMask)
{
  UNIMPLEMENTED(FATAL) << "LoadMultiple";
  NewLIR0(cUnit, kX86Bkpt);
  return NULL;
}

LIR *StoreMultiple(CompilationUnit *cUnit, int rBase, int rMask)
{
  UNIMPLEMENTED(FATAL) << "StoreMultiple";
  NewLIR0(cUnit, kX86Bkpt);
  return NULL;
}

LIR* LoadBaseIndexedDisp(CompilationUnit *cUnit,
                         int rBase, int rIndex, int scale, int displacement,
                         int rDest, int rDestHi,
                         OpSize size, int sReg) {
  LIR *load = NULL;
  LIR *load2 = NULL;
  bool isArray = rIndex != INVALID_REG;
  bool pair = false;
  bool is64bit = false;
  X86OpCode opcode = kX86Nop;
  switch (size) {
    case kLong:
    case kDouble:
      is64bit = true;
      if (X86_FPREG(rDest)) {
        opcode = isArray ? kX86MovsdRA : kX86MovsdRM;
        if (X86_SINGLEREG(rDest)) {
          DCHECK(X86_FPREG(rDestHi));
          DCHECK_EQ(rDest, (rDestHi - 1));
          rDest = S2d(rDest, rDestHi);
        }
        rDestHi = rDest + 1;
      } else {
        pair = true;
        opcode = isArray ? kX86Mov32RA  : kX86Mov32RM;
      }
      // TODO: double store is to unaligned address
      DCHECK_EQ((displacement & 0x3), 0);
      break;
    case kWord:
    case kSingle:
      opcode = isArray ? kX86Mov32RA : kX86Mov32RM;
      if (X86_FPREG(rDest)) {
        opcode = isArray ? kX86MovssRA : kX86MovssRM;
        DCHECK(X86_SINGLEREG(rDest));
      }
      DCHECK_EQ((displacement & 0x3), 0);
      break;
    case kUnsignedHalf:
      opcode = isArray ? kX86Movzx16RA : kX86Movzx16RM;
      DCHECK_EQ((displacement & 0x1), 0);
      break;
    case kSignedHalf:
      opcode = isArray ? kX86Movsx16RA : kX86Movsx16RM;
      DCHECK_EQ((displacement & 0x1), 0);
      break;
    case kUnsignedByte:
      opcode = isArray ? kX86Movzx8RA : kX86Movzx8RM;
      break;
    case kSignedByte:
      opcode = isArray ? kX86Movsx8RA : kX86Movsx8RM;
      break;
    default:
      LOG(FATAL) << "Bad case in LoadBaseIndexedDispBody";
  }

  if (!isArray) {
    if (!pair) {
      load = NewLIR3(cUnit, opcode, rDest, rBase, displacement + LOWORD_OFFSET);
    } else {
      if (rBase == rDest) {
        load2 = NewLIR3(cUnit, opcode, rDestHi, rBase,
                        displacement + HIWORD_OFFSET);
        load = NewLIR3(cUnit, opcode, rDest, rBase, displacement + LOWORD_OFFSET);
      } else {
        load = NewLIR3(cUnit, opcode, rDest, rBase, displacement + LOWORD_OFFSET);
        load2 = NewLIR3(cUnit, opcode, rDestHi, rBase,
                        displacement + HIWORD_OFFSET);
      }
    }
    if (rBase == rX86_SP) {
      AnnotateDalvikRegAccess(load, (displacement + (pair ? LOWORD_OFFSET : 0))
                              >> 2, true /* isLoad */, is64bit);
      if (pair) {
        AnnotateDalvikRegAccess(load2, (displacement + HIWORD_OFFSET) >> 2,
                                true /* isLoad */, is64bit);
      }
    }
  } else {
    if (!pair) {
      load = NewLIR5(cUnit, opcode, rDest, rBase, rIndex, scale,
                     displacement + LOWORD_OFFSET);
    } else {
      if (rBase == rDest) {
        load2 = NewLIR5(cUnit, opcode, rDestHi, rBase, rIndex, scale,
                        displacement + HIWORD_OFFSET);
        load = NewLIR5(cUnit, opcode, rDest, rBase, rIndex, scale,
                       displacement + LOWORD_OFFSET);
      } else {
        load = NewLIR5(cUnit, opcode, rDest, rBase, rIndex, scale,
                       displacement + LOWORD_OFFSET);
        load2 = NewLIR5(cUnit, opcode, rDestHi, rBase, rIndex, scale,
                        displacement + HIWORD_OFFSET);
      }
    }
  }

  return load;
}

/* Load value from base + scaled index. */
LIR *LoadBaseIndexed(CompilationUnit *cUnit, int rBase,
                     int rIndex, int rDest, int scale, OpSize size) {
  return LoadBaseIndexedDisp(cUnit, rBase, rIndex, scale, 0,
                             rDest, INVALID_REG, size, INVALID_SREG);
}

LIR *LoadBaseDisp(CompilationUnit *cUnit,
                  int rBase, int displacement,
                  int rDest,
                  OpSize size, int sReg) {
  return LoadBaseIndexedDisp(cUnit, rBase, INVALID_REG, 0, displacement,
                             rDest, INVALID_REG, size, sReg);
}

LIR *LoadBaseDispWide(CompilationUnit *cUnit,
                      int rBase, int displacement,
                      int rDestLo, int rDestHi,
                      int sReg) {
  return LoadBaseIndexedDisp(cUnit, rBase, INVALID_REG, 0, displacement,
                             rDestLo, rDestHi, kLong, sReg);
}

LIR* StoreBaseIndexedDisp(CompilationUnit *cUnit,
                          int rBase, int rIndex, int scale, int displacement,
                          int rSrc, int rSrcHi,
                          OpSize size, int sReg) {
  LIR *store = NULL;
  LIR *store2 = NULL;
  bool isArray = rIndex != INVALID_REG;
  bool pair = false;
  bool is64bit = false;
  X86OpCode opcode = kX86Nop;
  switch (size) {
    case kLong:
    case kDouble:
      is64bit = true;
      if (X86_FPREG(rSrc)) {
        opcode = isArray ? kX86MovsdAR : kX86MovsdMR;
        if (X86_SINGLEREG(rSrc)) {
          DCHECK(X86_FPREG(rSrcHi));
          DCHECK_EQ(rSrc, (rSrcHi - 1));
          rSrc = S2d(rSrc, rSrcHi);
        }
        rSrcHi = rSrc + 1;
      } else {
        pair = true;
        opcode = isArray ? kX86Mov32AR  : kX86Mov32MR;
      }
      // TODO: double store is to unaligned address
      DCHECK_EQ((displacement & 0x3), 0);
      break;
    case kWord:
    case kSingle:
      opcode = isArray ? kX86Mov32AR : kX86Mov32MR;
      if (X86_FPREG(rSrc)) {
        opcode = isArray ? kX86MovssAR : kX86MovssMR;
        DCHECK(X86_SINGLEREG(rSrc));
      }
      DCHECK_EQ((displacement & 0x3), 0);
      break;
    case kUnsignedHalf:
    case kSignedHalf:
      opcode = isArray ? kX86Mov16AR : kX86Mov16MR;
      DCHECK_EQ((displacement & 0x1), 0);
      break;
    case kUnsignedByte:
    case kSignedByte:
      opcode = isArray ? kX86Mov8AR : kX86Mov8MR;
      break;
    default:
      LOG(FATAL) << "Bad case in LoadBaseIndexedDispBody";
  }

  if (!isArray) {
    if (!pair) {
      store = NewLIR3(cUnit, opcode, rBase, displacement + LOWORD_OFFSET, rSrc);
    } else {
      store = NewLIR3(cUnit, opcode, rBase, displacement + LOWORD_OFFSET, rSrc);
      store2 = NewLIR3(cUnit, opcode, rBase, displacement + HIWORD_OFFSET, rSrcHi);
    }
    if (rBase == rX86_SP) {
      AnnotateDalvikRegAccess(store, (displacement + (pair ? LOWORD_OFFSET : 0))
                              >> 2, false /* isLoad */, is64bit);
      if (pair) {
        AnnotateDalvikRegAccess(store2, (displacement + HIWORD_OFFSET) >> 2,
                                false /* isLoad */, is64bit);
      }
    }
  } else {
    if (!pair) {
      store = NewLIR5(cUnit, opcode, rBase, rIndex, scale,
                      displacement + LOWORD_OFFSET, rSrc);
    } else {
      store = NewLIR5(cUnit, opcode, rBase, rIndex, scale,
                      displacement + LOWORD_OFFSET, rSrc);
      store2 = NewLIR5(cUnit, opcode, rBase, rIndex, scale,
                       displacement + HIWORD_OFFSET, rSrcHi);
    }
  }

  return store;
}

/* store value base base + scaled index. */
LIR *StoreBaseIndexed(CompilationUnit *cUnit, int rBase, int rIndex, int rSrc,
                      int scale, OpSize size)
{
  return StoreBaseIndexedDisp(cUnit, rBase, rIndex, scale, 0,
                              rSrc, INVALID_REG, size, INVALID_SREG);
}

LIR *StoreBaseDisp(CompilationUnit *cUnit, int rBase, int displacement,
                   int rSrc, OpSize size)
{
    return StoreBaseIndexedDisp(cUnit, rBase, INVALID_REG, 0,
                                displacement, rSrc, INVALID_REG, size,
                                INVALID_SREG);
}

LIR *StoreBaseDispWide(CompilationUnit *cUnit, int rBase, int displacement,
                       int rSrcLo, int rSrcHi)
{
  return StoreBaseIndexedDisp(cUnit, rBase, INVALID_REG, 0, displacement,
                              rSrcLo, rSrcHi, kLong, INVALID_SREG);
}

void LoadPair(CompilationUnit *cUnit, int base, int lowReg, int highReg)
{
  LoadBaseDispWide(cUnit, base, 0, lowReg, highReg, INVALID_SREG);
}

}  // namespace art
