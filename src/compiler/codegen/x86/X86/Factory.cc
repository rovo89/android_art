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

namespace art {

/*
 * This file contains codegen for the X86 ISA and is intended to be
 * includes by:
 *
 *        Codegen-$(TARGET_ARCH_VARIANT).c
 *
 */

//FIXME: restore "static" when usage uncovered
/*static*/ int coreRegs[] = {
  rAX, rCX, rDX, rBX, rSP, rBP, rSI, rDI
#ifdef TARGET_REX_SUPPORT
  r8, r9, r10, r11, r12, r13, r14, 15
#endif
};
/*static*/ int reservedRegs[] = {rSP};
/*static*/ int coreTemps[] = {rAX, rCX, rDX, rBX};
/*static*/ int fpRegs[] = {
  fr0, fr1, fr2, fr3, fr4, fr5, fr6, fr7,
#ifdef TARGET_REX_SUPPORT
  fr8, fr9, fr10, fr11, fr12, fr13, fr14, fr15
#endif
};
/*static*/ int fpTemps[] = {
  fr0, fr1, fr2, fr3, fr4, fr5, fr6, fr7,
#ifdef TARGET_REX_SUPPORT
  fr8, fr9, fr10, fr11, fr12, fr13, fr14, fr15
#endif
};

void genBarrier(CompilationUnit *cUnit);
void loadPair(CompilationUnit *cUnit, int base, int lowReg, int highReg);
LIR *loadWordDisp(CompilationUnit *cUnit, int rBase, int displacement,
                      int rDest);
LIR *storeWordDisp(CompilationUnit *cUnit, int rBase,
                       int displacement, int rSrc);
LIR *loadConstant(CompilationUnit *cUnit, int rDest, int value);

LIR *fpRegCopy(CompilationUnit *cUnit, int rDest, int rSrc)
{
  int opcode;
  /* must be both DOUBLE or both not DOUBLE */
  DCHECK_EQ(DOUBLEREG(rDest), DOUBLEREG(rSrc));
  if (DOUBLEREG(rDest)) {
    opcode = kX86MovsdRR;
  } else {
    if (SINGLEREG(rDest)) {
      if (SINGLEREG(rSrc)) {
        opcode = kX86MovssRR;
      } else {  // Fpr <- Gpr
        opcode = kX86MovdxrRR;
      }
    } else {  // Gpr <- Fpr
      DCHECK(SINGLEREG(rSrc));
      opcode = kX86MovdrxRR;
    }
  }
  DCHECK_NE((EncodingMap[opcode].flags & IS_BINARY_OP), 0);
  LIR* res = rawLIR(cUnit, cUnit->currentDalvikOffset, opcode, rDest, rSrc);
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
 * 1) rDest is freshly returned from oatAllocTemp or
 * 2) The codegen is under fixed register usage
 */
LIR *loadConstantNoClobber(CompilationUnit *cUnit, int rDest, int value)
{
  int rDestSave = rDest;
  if (FPREG(rDest)) {
    if (value == 0) {
      return newLIR2(cUnit, kX86XorpsRR, rDest, rDest);
    }
    DCHECK(SINGLEREG(rDest));
    rDest = oatAllocTemp(cUnit);
  }

  LIR *res;
  if (value == 0) {
    res = newLIR2(cUnit, kX86Xor32RR, rDest, rDest);
  } else {
    // Note, there is no byte immediate form of a 32 bit immediate move.
    res = newLIR2(cUnit, kX86Mov32RI, rDest, value);
  }

  if (FPREG(rDestSave)) {
    newLIR2(cUnit, kX86MovdxrRR, rDestSave, rDest);
    oatFreeTemp(cUnit, rDest);
  }

  return res;
}

LIR* opBranchUnconditional(CompilationUnit *cUnit, OpKind op)
{
  CHECK_EQ(op, kOpUncondBr);
  return newLIR1(cUnit, kX86Jmp8, 0 /* offset to be patched */ );
}

LIR *loadMultiple(CompilationUnit *cUnit, int rBase, int rMask);

X86ConditionCode oatX86ConditionEncoding(ConditionCode cond);
LIR* opCondBranch(CompilationUnit* cUnit, ConditionCode cc, LIR* target)
{
  LIR* branch = newLIR2(cUnit, kX86Jcc8, 0 /* offset to be patched */,
                        oatX86ConditionEncoding(cc));
  branch->target = target;
  return branch;
}

LIR *opReg(CompilationUnit *cUnit, OpKind op, int rDestSrc)
{
  X86OpCode opcode = kX86Bkpt;
  switch (op) {
    case kOpNeg: opcode = kX86Neg32R; break;
    case kOpNot: opcode = kX86Not32R; break;
    case kOpBlx: opcode = kX86CallR; break;
    default:
      LOG(FATAL) << "Bad case in opReg " << op;
  }
  return newLIR1(cUnit, opcode, rDestSrc);
}

LIR *opRegImm(CompilationUnit *cUnit, OpKind op, int rDestSrc1, int value)
{
  X86OpCode opcode = kX86Bkpt;
  bool byteImm = IS_SIMM8(value);
  DCHECK(!FPREG(rDestSrc1));
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
    case kOpMov: return loadConstantNoClobber(cUnit, rDestSrc1, value);
    case kOpMul:
      opcode = byteImm ? kX86Imul32RRI8 : kX86Imul32RRI;
      return newLIR3(cUnit, opcode, rDestSrc1, rDestSrc1, value);
    default:
      LOG(FATAL) << "Bad case in opRegImm " << op;
  }
  return newLIR2(cUnit, opcode, rDestSrc1, value);
}

LIR *opRegReg(CompilationUnit *cUnit, OpKind op, int rDestSrc1, int rSrc2)
{
    X86OpCode opcode = kX86Nop;
    bool src2_must_be_cx = false;
    switch (op) {
        // X86 unary opcodes
      case kOpMvn:
        opRegCopy(cUnit, rDestSrc1, rSrc2);
        return opReg(cUnit, kOpNot, rDestSrc1);
      case kOpNeg:
        opRegCopy(cUnit, rDestSrc1, rSrc2);
        return opReg(cUnit, kOpNeg, rDestSrc1);
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
          newLIR2(cUnit, kX86Mov32RR, rDestSrc1, rSrc2);
          newLIR2(cUnit, kX86Sal32RI, rDestSrc1, 24);
          return newLIR2(cUnit, kX86Sar32RI, rDestSrc1, 24);
        } else {
          opcode = kX86Movsx8RR;
        }
        break;
      case kOp2Short: opcode = kX86Movsx16RR; break;
      case kOp2Char: opcode = kX86Movzx16RR; break;
      case kOpMul: opcode = kX86Imul32RR; break;
      default:
        LOG(FATAL) << "Bad case in opRegReg " << op;
        break;
    }
    CHECK(!src2_must_be_cx || rSrc2 == rCX);
    return newLIR2(cUnit, opcode, rDestSrc1, rSrc2);
}

LIR* opRegMem(CompilationUnit *cUnit, OpKind op, int rDest, int rBase,
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
      LOG(FATAL) << "Bad case in opRegMem " << op;
      break;
  }
  return newLIR3(cUnit, opcode, rDest, rBase, offset);
}

LIR* opRegRegReg(CompilationUnit *cUnit, OpKind op, int rDest, int rSrc1,
                 int rSrc2)
{
  if (rDest != rSrc1 && rDest != rSrc2) {
    if (op == kOpAdd) { // lea special case, except can't encode rbp as base
      if (rSrc1 == rSrc2) {
        opRegCopy(cUnit, rDest, rSrc1);
        return opRegImm(cUnit, kOpLsl, rDest, 1);
      } else if (rSrc1 != rBP) {
        return newLIR5(cUnit, kX86Lea32RA, rDest, rSrc1 /* base */,
                       rSrc2 /* index */, 0 /* scale */, 0 /* disp */);
      } else {
        return newLIR5(cUnit, kX86Lea32RA, rDest, rSrc2 /* base */,
                       rSrc1 /* index */, 0 /* scale */, 0 /* disp */);
      }
    } else {
      opRegCopy(cUnit, rDest, rSrc1);
      return opRegReg(cUnit, op, rDest, rSrc2);
    }
  } else if (rDest == rSrc1) {
    return opRegReg(cUnit, op, rDest, rSrc2);
  } else {  // rDest == rSrc2
    switch (op) {
      case kOpSub:  // non-commutative
        opReg(cUnit, kOpNeg, rDest);
        op = kOpAdd;
        break;
      case kOpSbc:
      case kOpLsl: case kOpLsr: case kOpAsr: case kOpRor: {
        int tReg = oatAllocTemp(cUnit);
        opRegCopy(cUnit, tReg, rSrc1);
        opRegReg(cUnit, op, tReg, rSrc2);
        LIR* res = opRegCopy(cUnit, rDest, tReg);
        oatFreeTemp(cUnit, tReg);
        return res;
      }
      case kOpAdd:  // commutative
      case kOpOr:
      case kOpAdc:
      case kOpAnd:
      case kOpXor:
        break;
      default:
        LOG(FATAL) << "Bad case in opRegRegReg " << op;
    }
    return opRegReg(cUnit, op, rDest, rSrc1);
  }
}

LIR* opRegRegImm(CompilationUnit *cUnit, OpKind op, int rDest, int rSrc,
                 int value)
{
  if (op == kOpMul) {
    X86OpCode opcode = IS_SIMM8(value) ? kX86Imul32RRI8 : kX86Imul32RRI;
    return newLIR3(cUnit, opcode, rDest, rSrc, value);
  } else if (op == kOpAnd) {
    if (value == 0xFF && rSrc < 4) {
      return newLIR2(cUnit, kX86Movzx8RR, rDest, rSrc);
    } else if (value == 0xFFFF) {
      return newLIR2(cUnit, kX86Movzx16RR, rDest, rSrc);
    }
  }
  if (rDest != rSrc) {
    if (false && op == kOpLsl && value >= 0 && value <= 3) { // lea shift special case
      // TODO: fix bug in LEA encoding when disp == 0
      return newLIR5(cUnit, kX86Lea32RA, rDest,  r5sib_no_base /* base */,
                     rSrc /* index */, value /* scale */, 0 /* disp */);
    } else if (op == kOpAdd) { // lea add special case
      return newLIR5(cUnit, kX86Lea32RA, rDest, rSrc /* base */,
                     r4sib_no_index /* index */, 0 /* scale */, value /* disp */);
    }
    opRegCopy(cUnit, rDest, rSrc);
  }
  return opRegImm(cUnit, op, rDest, value);
}

LIR* opThreadMem(CompilationUnit* cUnit, OpKind op, int threadOffset)
{
  X86OpCode opcode = kX86Bkpt;
  switch (op) {
    case kOpBlx: opcode = kX86CallT;  break;
    default:
      LOG(FATAL) << "Bad opcode: " << op;
      break;
  }
  return newLIR1(cUnit, opcode, threadOffset);
}

LIR* opMem(CompilationUnit* cUnit, OpKind op, int rBase, int disp)
{
  X86OpCode opcode = kX86Bkpt;
  switch (op) {
    case kOpBlx: opcode = kX86CallM;  break;
    default:
      LOG(FATAL) << "Bad opcode: " << op;
      break;
  }
  return newLIR2(cUnit, opcode, rBase, disp);
}

LIR *loadConstantValueWide(CompilationUnit *cUnit, int rDestLo,
                           int rDestHi, int valLo, int valHi)
{
    LIR *res;
    if (FPREG(rDestLo)) {
      DCHECK(FPREG(rDestHi));  // ignore rDestHi
      if (valLo == 0 && valHi == 0) {
        return newLIR2(cUnit, kX86XorpsRR, rDestLo, rDestLo);
      } else {
        if (valLo == 0) {
          res = newLIR2(cUnit, kX86XorpsRR, rDestLo, rDestLo);
        } else {
          res = loadConstantNoClobber(cUnit, rDestLo, valLo);
        }
        if (valHi != 0) {
          loadConstantNoClobber(cUnit, rDestHi, valHi);
          newLIR2(cUnit, kX86PsllqRI, rDestHi, 32);
          newLIR2(cUnit, kX86OrpsRR, rDestLo, rDestHi);
        }
      }
    } else {
      res = loadConstantNoClobber(cUnit, rDestLo, valLo);
      loadConstantNoClobber(cUnit, rDestHi, valHi);
    }
    return res;
}

LIR *loadMultiple(CompilationUnit *cUnit, int rBase, int rMask)
{
  UNIMPLEMENTED(FATAL) << "loadMultiple";
  newLIR0(cUnit, kX86Bkpt);
  return NULL;
}

LIR *storeMultiple(CompilationUnit *cUnit, int rBase, int rMask)
{
  UNIMPLEMENTED(FATAL) << "storeMultiple";
  newLIR0(cUnit, kX86Bkpt);
  return NULL;
}

LIR* loadBaseIndexedDisp(CompilationUnit *cUnit,
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
      if (FPREG(rDest)) {
        opcode = isArray ? kX86MovsdRA : kX86MovsdRM;
        if (SINGLEREG(rDest)) {
          DCHECK(FPREG(rDestHi));
          DCHECK_EQ(rDest, (rDestHi - 1));
          rDest = S2D(rDest, rDestHi);
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
      if (FPREG(rDest)) {
        opcode = isArray ? kX86MovssRA : kX86MovssRM;
        DCHECK(SINGLEREG(rDest));
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
      LOG(FATAL) << "Bad case in loadBaseIndexedDispBody";
  }

  if (!isArray) {
    if (!pair) {
      load = newLIR3(cUnit, opcode, rDest, rBase, displacement + LOWORD_OFFSET);
    } else {
      if (rBase == rDest) {
        load2 = newLIR3(cUnit, opcode, rDestHi, rBase,
                        displacement + HIWORD_OFFSET);
        load = newLIR3(cUnit, opcode, rDest, rBase, displacement + LOWORD_OFFSET);
      } else {
        load = newLIR3(cUnit, opcode, rDest, rBase, displacement + LOWORD_OFFSET);
        load2 = newLIR3(cUnit, opcode, rDestHi, rBase,
                        displacement + HIWORD_OFFSET);
      }
    }
    if (rBase == rSP) {
      annotateDalvikRegAccess(load, (displacement + (pair ? LOWORD_OFFSET : 0))
                              >> 2, true /* isLoad */, is64bit);
      if (pair) {
        annotateDalvikRegAccess(load2, (displacement + HIWORD_OFFSET) >> 2,
                                true /* isLoad */, is64bit);
      }
    }
  } else {
    if (!pair) {
      load = newLIR5(cUnit, opcode, rDest, rBase, rIndex, scale,
                     displacement + LOWORD_OFFSET);
    } else {
      if (rBase == rDest) {
        load2 = newLIR5(cUnit, opcode, rDestHi, rBase, rIndex, scale,
                        displacement + HIWORD_OFFSET);
        load = newLIR5(cUnit, opcode, rDest, rBase, rIndex, scale,
                       displacement + LOWORD_OFFSET);
      } else {
        load = newLIR5(cUnit, opcode, rDest, rBase, rIndex, scale,
                       displacement + LOWORD_OFFSET);
        load2 = newLIR5(cUnit, opcode, rDestHi, rBase, rIndex, scale,
                        displacement + HIWORD_OFFSET);
      }
    }
  }

  return load;
}

/* Load value from base + scaled index. */
LIR *loadBaseIndexed(CompilationUnit *cUnit, int rBase,
                     int rIndex, int rDest, int scale, OpSize size) {
  return loadBaseIndexedDisp(cUnit, rBase, rIndex, scale, 0,
                             rDest, INVALID_REG, size, INVALID_SREG);
}

LIR *loadBaseDisp(CompilationUnit *cUnit,
                  int rBase, int displacement,
                  int rDest,
                  OpSize size, int sReg) {
  return loadBaseIndexedDisp(cUnit, rBase, INVALID_REG, 0, displacement,
                             rDest, INVALID_REG, size, sReg);
}

LIR *loadBaseDispWide(CompilationUnit *cUnit,
                      int rBase, int displacement,
                      int rDestLo, int rDestHi,
                      int sReg) {
  return loadBaseIndexedDisp(cUnit, rBase, INVALID_REG, 0, displacement,
                             rDestLo, rDestHi, kLong, sReg);
}

LIR* storeBaseIndexedDisp(CompilationUnit *cUnit,
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
      if (FPREG(rSrc)) {
        opcode = isArray ? kX86MovsdAR : kX86MovsdMR;
        if (SINGLEREG(rSrc)) {
          DCHECK(FPREG(rSrcHi));
          DCHECK_EQ(rSrc, (rSrcHi - 1));
          rSrc = S2D(rSrc, rSrcHi);
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
      if (FPREG(rSrc)) {
        opcode = isArray ? kX86MovssAR : kX86MovssMR;
        DCHECK(SINGLEREG(rSrc));
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
      LOG(FATAL) << "Bad case in loadBaseIndexedDispBody";
  }

  if (!isArray) {
    if (!pair) {
      store = newLIR3(cUnit, opcode, rBase, displacement + LOWORD_OFFSET, rSrc);
    } else {
      store = newLIR3(cUnit, opcode, rBase, displacement + LOWORD_OFFSET, rSrc);
      store2 = newLIR3(cUnit, opcode, rBase, displacement + HIWORD_OFFSET, rSrcHi);
    }
    if (rBase == rSP) {
      annotateDalvikRegAccess(store, (displacement + (pair ? LOWORD_OFFSET : 0))
                              >> 2, false /* isLoad */, is64bit);
      if (pair) {
        annotateDalvikRegAccess(store2, (displacement + HIWORD_OFFSET) >> 2,
                                false /* isLoad */, is64bit);
      }
    }
  } else {
    if (!pair) {
      store = newLIR5(cUnit, opcode, rBase, rIndex, scale,
                      displacement + LOWORD_OFFSET, rSrc);
    } else {
      store = newLIR5(cUnit, opcode, rBase, rIndex, scale,
                      displacement + LOWORD_OFFSET, rSrc);
      store2 = newLIR5(cUnit, opcode, rBase, rIndex, scale,
                       displacement + HIWORD_OFFSET, rSrcHi);
    }
  }

  return store;
}

/* store value base base + scaled index. */
LIR *storeBaseIndexed(CompilationUnit *cUnit, int rBase, int rIndex, int rSrc,
                      int scale, OpSize size)
{
  return storeBaseIndexedDisp(cUnit, rBase, rIndex, scale, 0,
                              rSrc, INVALID_REG, size, INVALID_SREG);
}

LIR *storeBaseDisp(CompilationUnit *cUnit, int rBase, int displacement,
                   int rSrc, OpSize size)
{
    return storeBaseIndexedDisp(cUnit, rBase, INVALID_REG, 0,
                                displacement, rSrc, INVALID_REG, size,
                                INVALID_SREG);
}

LIR *storeBaseDispWide(CompilationUnit *cUnit, int rBase, int displacement,
                       int rSrcLo, int rSrcHi)
{
  return storeBaseIndexedDisp(cUnit, rBase, INVALID_REG, 0, displacement,
                              rSrcLo, rSrcHi, kLong, INVALID_SREG);
}

void loadPair(CompilationUnit *cUnit, int base, int lowReg, int highReg)
{
  loadBaseDispWide(cUnit, base, 0, lowReg, highReg, INVALID_SREG);
}

}  // namespace art
