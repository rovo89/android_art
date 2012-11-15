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

namespace art {

/* This file contains codegen for the Thumb ISA. */

int encodeImmSingle(int value)
{
  int res;
  int bitA =  (value & 0x80000000) >> 31;
  int notBitB = (value & 0x40000000) >> 30;
  int bitB =  (value & 0x20000000) >> 29;
  int bSmear =  (value & 0x3e000000) >> 25;
  int slice =   (value & 0x01f80000) >> 19;
  int zeroes =  (value & 0x0007ffff);
  if (zeroes != 0)
    return -1;
  if (bitB) {
    if ((notBitB != 0) || (bSmear != 0x1f))
      return -1;
  } else {
    if ((notBitB != 1) || (bSmear != 0x0))
      return -1;
  }
  res = (bitA << 7) | (bitB << 6) | slice;
  return res;
}

LIR* loadFPConstantValue(CompilationUnit* cUnit, int rDest, int value)
{
  int encodedImm = encodeImmSingle(value);
  DCHECK(ARM_SINGLEREG(rDest));
  if (encodedImm >= 0) {
    return newLIR2(cUnit, kThumb2Vmovs_IMM8, rDest, encodedImm);
  }
  LIR* dataTarget = scanLiteralPool(cUnit->literalList, value, 0);
  if (dataTarget == NULL) {
    dataTarget = addWordData(cUnit, &cUnit->literalList, value);
  }
  LIR* loadPcRel = rawLIR(cUnit, cUnit->currentDalvikOffset, kThumb2Vldrs,
                          rDest, r15pc, 0, 0, 0, dataTarget);
  setMemRefType(loadPcRel, true, kLiteral);
  loadPcRel->aliasInfo = (intptr_t)dataTarget;
  oatAppendLIR(cUnit, (LIR* ) loadPcRel);
  return loadPcRel;
}

int leadingZeros(u4 val)
{
  u4 alt;
  int n;
  int count;

  count = 16;
  n = 32;
  do {
    alt = val >> count;
    if (alt != 0) {
      n = n - count;
      val = alt;
    }
    count >>= 1;
  } while (count);
  return n - val;
}

/*
 * Determine whether value can be encoded as a Thumb2 modified
 * immediate.  If not, return -1.  If so, return i:imm3:a:bcdefgh form.
 */
int modifiedImmediate(u4 value)
{
   int zLeading;
   int zTrailing;
   u4 b0 = value & 0xff;

   /* Note: case of value==0 must use 0:000:0:0000000 encoding */
   if (value <= 0xFF)
     return b0;  // 0:000:a:bcdefgh
   if (value == ((b0 << 16) | b0))
     return (0x1 << 8) | b0; /* 0:001:a:bcdefgh */
   if (value == ((b0 << 24) | (b0 << 16) | (b0 << 8) | b0))
     return (0x3 << 8) | b0; /* 0:011:a:bcdefgh */
   b0 = (value >> 8) & 0xff;
   if (value == ((b0 << 24) | (b0 << 8)))
     return (0x2 << 8) | b0; /* 0:010:a:bcdefgh */
   /* Can we do it with rotation? */
   zLeading = leadingZeros(value);
   zTrailing = 32 - leadingZeros(~value & (value - 1));
   /* A run of eight or fewer active bits? */
   if ((zLeading + zTrailing) < 24)
     return -1;  /* No - bail */
   /* left-justify the constant, discarding msb (known to be 1) */
   value <<= zLeading + 1;
   /* Create bcdefgh */
   value >>= 25;
   /* Put it all together */
   return value | ((0x8 + zLeading) << 7); /* [01000..11111]:bcdefgh */
}

/*
 * Load a immediate using a shortcut if possible; otherwise
 * grab from the per-translation literal pool.
 *
 * No additional register clobbering operation performed. Use this version when
 * 1) rDest is freshly returned from oatAllocTemp or
 * 2) The codegen is under fixed register usage
 */
LIR* loadConstantNoClobber(CompilationUnit* cUnit, int rDest, int value)
{
  LIR* res;
  int modImm;

  if (ARM_FPREG(rDest)) {
    return loadFPConstantValue(cUnit, rDest, value);
  }

  /* See if the value can be constructed cheaply */
  if (ARM_LOWREG(rDest) && (value >= 0) && (value <= 255)) {
    return newLIR2(cUnit, kThumbMovImm, rDest, value);
  }
  /* Check Modified immediate special cases */
  modImm = modifiedImmediate(value);
  if (modImm >= 0) {
    res = newLIR2(cUnit, kThumb2MovImmShift, rDest, modImm);
    return res;
  }
  modImm = modifiedImmediate(~value);
  if (modImm >= 0) {
    res = newLIR2(cUnit, kThumb2MvnImm12, rDest, modImm);
    return res;
  }
  /* 16-bit immediate? */
  if ((value & 0xffff) == value) {
    res = newLIR2(cUnit, kThumb2MovImm16, rDest, value);
    return res;
  }
  /* No shortcut - go ahead and use literal pool */
  LIR* dataTarget = scanLiteralPool(cUnit->literalList, value, 0);
  if (dataTarget == NULL) {
    dataTarget = addWordData(cUnit, &cUnit->literalList, value);
  }
  LIR* loadPcRel = rawLIR(cUnit, cUnit->currentDalvikOffset,
                          kThumb2LdrPcRel12, rDest, 0, 0, 0, 0, dataTarget);
  setMemRefType(loadPcRel, true, kLiteral);
  loadPcRel->aliasInfo = (intptr_t)dataTarget;
  res = loadPcRel;
  oatAppendLIR(cUnit, (LIR* ) loadPcRel);

  /*
   * To save space in the constant pool, we use the ADD_RRI8 instruction to
   * add up to 255 to an existing constant value.
   */
  if (dataTarget->operands[0] != value) {
    opRegImm(cUnit, kOpAdd, rDest, value - dataTarget->operands[0]);
  }
  return res;
}

LIR* opBranchUnconditional(CompilationUnit* cUnit, OpKind op)
{
  DCHECK_EQ(op, kOpUncondBr);
  return newLIR1(cUnit, kThumbBUncond, 0 /* offset to be patched */);
}

LIR* opCondBranch(CompilationUnit* cUnit, ConditionCode cc, LIR* target)
{
  LIR* branch = newLIR2(cUnit, kThumb2BCond, 0 /* offset to be patched */,
                        oatArmConditionEncoding(cc));
  branch->target = target;
  return branch;
}

LIR* opReg(CompilationUnit* cUnit, OpKind op, int rDestSrc)
{
  ArmOpcode opcode = kThumbBkpt;
  switch (op) {
    case kOpBlx:
      opcode = kThumbBlxR;
      break;
    default:
      LOG(FATAL) << "Bad opcode " << (int)op;
  }
  return newLIR1(cUnit, opcode, rDestSrc);
}

LIR* opRegRegShift(CompilationUnit* cUnit, OpKind op, int rDestSrc1,
                   int rSrc2, int shift)
{
  bool thumbForm = ((shift == 0) && ARM_LOWREG(rDestSrc1) && ARM_LOWREG(rSrc2));
  ArmOpcode opcode = kThumbBkpt;
  switch (op) {
    case kOpAdc:
      opcode = (thumbForm) ? kThumbAdcRR : kThumb2AdcRRR;
      break;
    case kOpAnd:
      opcode = (thumbForm) ? kThumbAndRR : kThumb2AndRRR;
      break;
    case kOpBic:
      opcode = (thumbForm) ? kThumbBicRR : kThumb2BicRRR;
      break;
    case kOpCmn:
      DCHECK_EQ(shift, 0);
      opcode = (thumbForm) ? kThumbCmnRR : kThumb2CmnRR;
      break;
    case kOpCmp:
      if (thumbForm)
        opcode = kThumbCmpRR;
      else if ((shift == 0) && !ARM_LOWREG(rDestSrc1) && !ARM_LOWREG(rSrc2))
        opcode = kThumbCmpHH;
      else if ((shift == 0) && ARM_LOWREG(rDestSrc1))
        opcode = kThumbCmpLH;
      else if (shift == 0)
        opcode = kThumbCmpHL;
      else
        opcode = kThumb2CmpRR;
      break;
    case kOpXor:
      opcode = (thumbForm) ? kThumbEorRR : kThumb2EorRRR;
      break;
    case kOpMov:
      DCHECK_EQ(shift, 0);
      if (ARM_LOWREG(rDestSrc1) && ARM_LOWREG(rSrc2))
        opcode = kThumbMovRR;
      else if (!ARM_LOWREG(rDestSrc1) && !ARM_LOWREG(rSrc2))
        opcode = kThumbMovRR_H2H;
      else if (ARM_LOWREG(rDestSrc1))
        opcode = kThumbMovRR_H2L;
      else
        opcode = kThumbMovRR_L2H;
      break;
    case kOpMul:
      DCHECK_EQ(shift, 0);
      opcode = (thumbForm) ? kThumbMul : kThumb2MulRRR;
      break;
    case kOpMvn:
      opcode = (thumbForm) ? kThumbMvn : kThumb2MnvRR;
      break;
    case kOpNeg:
      DCHECK_EQ(shift, 0);
      opcode = (thumbForm) ? kThumbNeg : kThumb2NegRR;
      break;
    case kOpOr:
      opcode = (thumbForm) ? kThumbOrr : kThumb2OrrRRR;
      break;
    case kOpSbc:
      opcode = (thumbForm) ? kThumbSbc : kThumb2SbcRRR;
      break;
    case kOpTst:
      opcode = (thumbForm) ? kThumbTst : kThumb2TstRR;
      break;
    case kOpLsl:
      DCHECK_EQ(shift, 0);
      opcode = (thumbForm) ? kThumbLslRR : kThumb2LslRRR;
      break;
    case kOpLsr:
      DCHECK_EQ(shift, 0);
      opcode = (thumbForm) ? kThumbLsrRR : kThumb2LsrRRR;
      break;
    case kOpAsr:
      DCHECK_EQ(shift, 0);
      opcode = (thumbForm) ? kThumbAsrRR : kThumb2AsrRRR;
      break;
    case kOpRor:
      DCHECK_EQ(shift, 0);
      opcode = (thumbForm) ? kThumbRorRR : kThumb2RorRRR;
      break;
    case kOpAdd:
      opcode = (thumbForm) ? kThumbAddRRR : kThumb2AddRRR;
      break;
    case kOpSub:
      opcode = (thumbForm) ? kThumbSubRRR : kThumb2SubRRR;
      break;
    case kOp2Byte:
      DCHECK_EQ(shift, 0);
      return newLIR4(cUnit, kThumb2Sbfx, rDestSrc1, rSrc2, 0, 8);
    case kOp2Short:
      DCHECK_EQ(shift, 0);
      return newLIR4(cUnit, kThumb2Sbfx, rDestSrc1, rSrc2, 0, 16);
    case kOp2Char:
      DCHECK_EQ(shift, 0);
      return newLIR4(cUnit, kThumb2Ubfx, rDestSrc1, rSrc2, 0, 16);
    default:
      LOG(FATAL) << "Bad opcode: " << (int)op;
      break;
  }
  DCHECK_GE(static_cast<int>(opcode), 0);
  if (EncodingMap[opcode].flags & IS_BINARY_OP)
    return newLIR2(cUnit, opcode, rDestSrc1, rSrc2);
  else if (EncodingMap[opcode].flags & IS_TERTIARY_OP) {
    if (EncodingMap[opcode].fieldLoc[2].kind == kFmtShift)
      return newLIR3(cUnit, opcode, rDestSrc1, rSrc2, shift);
    else
      return newLIR3(cUnit, opcode, rDestSrc1, rDestSrc1, rSrc2);
  } else if (EncodingMap[opcode].flags & IS_QUAD_OP)
    return newLIR4(cUnit, opcode, rDestSrc1, rDestSrc1, rSrc2, shift);
  else {
    LOG(FATAL) << "Unexpected encoding operand count";
    return NULL;
  }
}

LIR* opRegReg(CompilationUnit* cUnit, OpKind op, int rDestSrc1, int rSrc2)
{
  return opRegRegShift(cUnit, op, rDestSrc1, rSrc2, 0);
}

LIR* opRegRegRegShift(CompilationUnit* cUnit, OpKind op, int rDest, int rSrc1,
            int rSrc2, int shift)
{
  ArmOpcode opcode = kThumbBkpt;
  bool thumbForm = (shift == 0) && ARM_LOWREG(rDest) && ARM_LOWREG(rSrc1) &&
      ARM_LOWREG(rSrc2);
  switch (op) {
    case kOpAdd:
      opcode = (thumbForm) ? kThumbAddRRR : kThumb2AddRRR;
      break;
    case kOpSub:
      opcode = (thumbForm) ? kThumbSubRRR : kThumb2SubRRR;
      break;
    case kOpRsub:
      opcode = kThumb2RsubRRR;
      break;
    case kOpAdc:
      opcode = kThumb2AdcRRR;
      break;
    case kOpAnd:
      opcode = kThumb2AndRRR;
      break;
    case kOpBic:
      opcode = kThumb2BicRRR;
      break;
    case kOpXor:
      opcode = kThumb2EorRRR;
      break;
    case kOpMul:
      DCHECK_EQ(shift, 0);
      opcode = kThumb2MulRRR;
      break;
    case kOpOr:
      opcode = kThumb2OrrRRR;
      break;
    case kOpSbc:
      opcode = kThumb2SbcRRR;
      break;
    case kOpLsl:
      DCHECK_EQ(shift, 0);
      opcode = kThumb2LslRRR;
      break;
    case kOpLsr:
      DCHECK_EQ(shift, 0);
      opcode = kThumb2LsrRRR;
      break;
    case kOpAsr:
      DCHECK_EQ(shift, 0);
      opcode = kThumb2AsrRRR;
      break;
    case kOpRor:
      DCHECK_EQ(shift, 0);
      opcode = kThumb2RorRRR;
      break;
    default:
      LOG(FATAL) << "Bad opcode: " << (int)op;
      break;
  }
  DCHECK_GE(static_cast<int>(opcode), 0);
  if (EncodingMap[opcode].flags & IS_QUAD_OP)
    return newLIR4(cUnit, opcode, rDest, rSrc1, rSrc2, shift);
  else {
    DCHECK(EncodingMap[opcode].flags & IS_TERTIARY_OP);
    return newLIR3(cUnit, opcode, rDest, rSrc1, rSrc2);
  }
}

LIR* opRegRegReg(CompilationUnit* cUnit, OpKind op, int rDest, int rSrc1,
                 int rSrc2)
{
  return opRegRegRegShift(cUnit, op, rDest, rSrc1, rSrc2, 0);
}

LIR* opRegRegImm(CompilationUnit* cUnit, OpKind op, int rDest, int rSrc1,
                 int value)
{
  LIR* res;
  bool neg = (value < 0);
  int absValue = (neg) ? -value : value;
  ArmOpcode opcode = kThumbBkpt;
  ArmOpcode altOpcode = kThumbBkpt;
  bool allLowRegs = (ARM_LOWREG(rDest) && ARM_LOWREG(rSrc1));
  int modImm = modifiedImmediate(value);
  int modImmNeg = modifiedImmediate(-value);

  switch (op) {
    case kOpLsl:
      if (allLowRegs)
        return newLIR3(cUnit, kThumbLslRRI5, rDest, rSrc1, value);
      else
        return newLIR3(cUnit, kThumb2LslRRI5, rDest, rSrc1, value);
    case kOpLsr:
      if (allLowRegs)
        return newLIR3(cUnit, kThumbLsrRRI5, rDest, rSrc1, value);
      else
        return newLIR3(cUnit, kThumb2LsrRRI5, rDest, rSrc1, value);
    case kOpAsr:
      if (allLowRegs)
        return newLIR3(cUnit, kThumbAsrRRI5, rDest, rSrc1, value);
      else
        return newLIR3(cUnit, kThumb2AsrRRI5, rDest, rSrc1, value);
    case kOpRor:
      return newLIR3(cUnit, kThumb2RorRRI5, rDest, rSrc1, value);
    case kOpAdd:
      if (ARM_LOWREG(rDest) && (rSrc1 == r13sp) &&
        (value <= 1020) && ((value & 0x3)==0)) {
        return newLIR3(cUnit, kThumbAddSpRel, rDest, rSrc1, value >> 2);
      } else if (ARM_LOWREG(rDest) && (rSrc1 == r15pc) &&
          (value <= 1020) && ((value & 0x3)==0)) {
        return newLIR3(cUnit, kThumbAddPcRel, rDest, rSrc1, value >> 2);
      }
      // Note: intentional fallthrough
    case kOpSub:
      if (allLowRegs && ((absValue & 0x7) == absValue)) {
        if (op == kOpAdd)
          opcode = (neg) ? kThumbSubRRI3 : kThumbAddRRI3;
        else
          opcode = (neg) ? kThumbAddRRI3 : kThumbSubRRI3;
        return newLIR3(cUnit, opcode, rDest, rSrc1, absValue);
      } else if ((absValue & 0xff) == absValue) {
        if (op == kOpAdd)
          opcode = (neg) ? kThumb2SubRRI12 : kThumb2AddRRI12;
        else
          opcode = (neg) ? kThumb2AddRRI12 : kThumb2SubRRI12;
        return newLIR3(cUnit, opcode, rDest, rSrc1, absValue);
      }
      if (modImmNeg >= 0) {
        op = (op == kOpAdd) ? kOpSub : kOpAdd;
        modImm = modImmNeg;
      }
      if (op == kOpSub) {
        opcode = kThumb2SubRRI8;
        altOpcode = kThumb2SubRRR;
      } else {
        opcode = kThumb2AddRRI8;
        altOpcode = kThumb2AddRRR;
      }
      break;
    case kOpAdc:
      opcode = kThumb2AdcRRI8;
      altOpcode = kThumb2AdcRRR;
      break;
    case kOpSbc:
      opcode = kThumb2SbcRRI8;
      altOpcode = kThumb2SbcRRR;
      break;
    case kOpOr:
      opcode = kThumb2OrrRRI8;
      altOpcode = kThumb2OrrRRR;
      break;
    case kOpAnd:
      opcode = kThumb2AndRRI8;
      altOpcode = kThumb2AndRRR;
      break;
    case kOpXor:
      opcode = kThumb2EorRRI8;
      altOpcode = kThumb2EorRRR;
      break;
    case kOpMul:
      //TUNING: power of 2, shift & add
      modImm = -1;
      altOpcode = kThumb2MulRRR;
      break;
    case kOpCmp: {
      int modImm = modifiedImmediate(value);
      LIR* res;
      if (modImm >= 0) {
        res = newLIR2(cUnit, kThumb2CmpRI8, rSrc1, modImm);
      } else {
        int rTmp = oatAllocTemp(cUnit);
        res = loadConstant(cUnit, rTmp, value);
        opRegReg(cUnit, kOpCmp, rSrc1, rTmp);
        oatFreeTemp(cUnit, rTmp);
      }
      return res;
    }
    default:
      LOG(FATAL) << "Bad opcode: " << (int)op;
  }

  if (modImm >= 0) {
    return newLIR3(cUnit, opcode, rDest, rSrc1, modImm);
  } else {
    int rScratch = oatAllocTemp(cUnit);
    loadConstant(cUnit, rScratch, value);
    if (EncodingMap[altOpcode].flags & IS_QUAD_OP)
      res = newLIR4(cUnit, altOpcode, rDest, rSrc1, rScratch, 0);
    else
      res = newLIR3(cUnit, altOpcode, rDest, rSrc1, rScratch);
    oatFreeTemp(cUnit, rScratch);
    return res;
  }
}

/* Handle Thumb-only variants here - otherwise punt to opRegRegImm */
LIR* opRegImm(CompilationUnit* cUnit, OpKind op, int rDestSrc1, int value)
{
  bool neg = (value < 0);
  int absValue = (neg) ? -value : value;
  bool shortForm = (((absValue & 0xff) == absValue) && ARM_LOWREG(rDestSrc1));
  ArmOpcode opcode = kThumbBkpt;
  switch (op) {
    case kOpAdd:
      if ( !neg && (rDestSrc1 == r13sp) && (value <= 508)) { /* sp */
        DCHECK_EQ((value & 0x3), 0);
        return newLIR1(cUnit, kThumbAddSpI7, value >> 2);
      } else if (shortForm) {
        opcode = (neg) ? kThumbSubRI8 : kThumbAddRI8;
      }
      break;
    case kOpSub:
      if (!neg && (rDestSrc1 == r13sp) && (value <= 508)) { /* sp */
        DCHECK_EQ((value & 0x3), 0);
        return newLIR1(cUnit, kThumbSubSpI7, value >> 2);
      } else if (shortForm) {
        opcode = (neg) ? kThumbAddRI8 : kThumbSubRI8;
      }
      break;
    case kOpCmp:
      if (ARM_LOWREG(rDestSrc1) && shortForm)
        opcode = (shortForm) ?  kThumbCmpRI8 : kThumbCmpRR;
      else if (ARM_LOWREG(rDestSrc1))
        opcode = kThumbCmpRR;
      else {
        shortForm = false;
        opcode = kThumbCmpHL;
      }
      break;
    default:
      /* Punt to opRegRegImm - if bad case catch it there */
      shortForm = false;
      break;
  }
  if (shortForm)
    return newLIR2(cUnit, opcode, rDestSrc1, absValue);
  else {
    return opRegRegImm(cUnit, op, rDestSrc1, rDestSrc1, value);
  }
}

/*
 * Determine whether value can be encoded as a Thumb2 floating point
 * immediate.  If not, return -1.  If so return encoded 8-bit value.
 */
int encodeImmDoubleHigh(int value)
{
  int res;
  int bitA =  (value & 0x80000000) >> 31;
  int notBitB = (value & 0x40000000) >> 30;
  int bitB =  (value & 0x20000000) >> 29;
  int bSmear =  (value & 0x3fc00000) >> 22;
  int slice =   (value & 0x003f0000) >> 16;
  int zeroes =  (value & 0x0000ffff);
  if (zeroes != 0)
    return -1;
  if (bitB) {
    if ((notBitB != 0) || (bSmear != 0xff))
      return -1;
  } else {
    if ((notBitB != 1) || (bSmear != 0x0))
      return -1;
  }
  res = (bitA << 7) | (bitB << 6) | slice;
  return res;
}

int encodeImmDouble(int valLo, int valHi)
{
  int res = -1;
  if (valLo == 0)
    res = encodeImmDoubleHigh(valHi);
  return res;
}

LIR* loadConstantValueWide(CompilationUnit* cUnit, int rDestLo, int rDestHi,
               int valLo, int valHi)
{
  int encodedImm = encodeImmDouble(valLo, valHi);
  LIR* res;
  if (ARM_FPREG(rDestLo)) {
    if (encodedImm >= 0) {
      res = newLIR2(cUnit, kThumb2Vmovd_IMM8, s2d(rDestLo, rDestHi),
              encodedImm);
    } else {
      LIR* dataTarget = scanLiteralPoolWide(cUnit->literalList, valLo, valHi);
      if (dataTarget == NULL) {
        dataTarget = addWideData(cUnit, &cUnit->literalList, valLo, valHi);
      }
      LIR* loadPcRel =
          rawLIR(cUnit, cUnit->currentDalvikOffset, kThumb2Vldrd,
                 s2d(rDestLo, rDestHi), r15pc, 0, 0, 0, dataTarget);
      setMemRefType(loadPcRel, true, kLiteral);
      loadPcRel->aliasInfo = (intptr_t)dataTarget;
      oatAppendLIR(cUnit, (LIR* ) loadPcRel);
      res = loadPcRel;
    }
  } else {
    res = loadConstantNoClobber(cUnit, rDestLo, valLo);
    loadConstantNoClobber(cUnit, rDestHi, valHi);
  }
  return res;
}

int encodeShift(int code, int amount) {
  return ((amount & 0x1f) << 2) | code;
}

LIR* loadBaseIndexed(CompilationUnit* cUnit, int rBase, int rIndex, int rDest,
                     int scale, OpSize size)
{
  bool allLowRegs = ARM_LOWREG(rBase) && ARM_LOWREG(rIndex) && ARM_LOWREG(rDest);
  LIR* load;
  ArmOpcode opcode = kThumbBkpt;
  bool thumbForm = (allLowRegs && (scale == 0));
  int regPtr;

  if (ARM_FPREG(rDest)) {
    if (ARM_SINGLEREG(rDest)) {
      DCHECK((size == kWord) || (size == kSingle));
      opcode = kThumb2Vldrs;
      size = kSingle;
    } else {
      DCHECK(ARM_DOUBLEREG(rDest));
      DCHECK((size == kLong) || (size == kDouble));
      DCHECK_EQ((rDest & 0x1), 0);
      opcode = kThumb2Vldrd;
      size = kDouble;
    }
  } else {
    if (size == kSingle)
      size = kWord;
  }

  switch (size) {
    case kDouble: // fall-through
    case kSingle:
      regPtr = oatAllocTemp(cUnit);
      if (scale) {
        newLIR4(cUnit, kThumb2AddRRR, regPtr, rBase, rIndex,
                encodeShift(kArmLsl, scale));
      } else {
        opRegRegReg(cUnit, kOpAdd, regPtr, rBase, rIndex);
      }
      load = newLIR3(cUnit, opcode, rDest, regPtr, 0);
      oatFreeTemp(cUnit, regPtr);
      return load;
    case kWord:
      opcode = (thumbForm) ? kThumbLdrRRR : kThumb2LdrRRR;
      break;
    case kUnsignedHalf:
      opcode = (thumbForm) ? kThumbLdrhRRR : kThumb2LdrhRRR;
      break;
    case kSignedHalf:
      opcode = (thumbForm) ? kThumbLdrshRRR : kThumb2LdrshRRR;
      break;
    case kUnsignedByte:
      opcode = (thumbForm) ? kThumbLdrbRRR : kThumb2LdrbRRR;
      break;
    case kSignedByte:
      opcode = (thumbForm) ? kThumbLdrsbRRR : kThumb2LdrsbRRR;
      break;
    default:
      LOG(FATAL) << "Bad size: " << (int)size;
  }
  if (thumbForm)
    load = newLIR3(cUnit, opcode, rDest, rBase, rIndex);
  else
    load = newLIR4(cUnit, opcode, rDest, rBase, rIndex, scale);

  return load;
}

LIR* storeBaseIndexed(CompilationUnit* cUnit, int rBase, int rIndex, int rSrc,
                      int scale, OpSize size)
{
  bool allLowRegs = ARM_LOWREG(rBase) && ARM_LOWREG(rIndex) && ARM_LOWREG(rSrc);
  LIR* store;
  ArmOpcode opcode = kThumbBkpt;
  bool thumbForm = (allLowRegs && (scale == 0));
  int regPtr;

  if (ARM_FPREG(rSrc)) {
    if (ARM_SINGLEREG(rSrc)) {
      DCHECK((size == kWord) || (size == kSingle));
      opcode = kThumb2Vstrs;
      size = kSingle;
    } else {
      DCHECK(ARM_DOUBLEREG(rSrc));
      DCHECK((size == kLong) || (size == kDouble));
      DCHECK_EQ((rSrc & 0x1), 0);
      opcode = kThumb2Vstrd;
      size = kDouble;
    }
  } else {
    if (size == kSingle)
      size = kWord;
  }

  switch (size) {
    case kDouble: // fall-through
    case kSingle:
      regPtr = oatAllocTemp(cUnit);
      if (scale) {
        newLIR4(cUnit, kThumb2AddRRR, regPtr, rBase, rIndex,
                encodeShift(kArmLsl, scale));
      } else {
        opRegRegReg(cUnit, kOpAdd, regPtr, rBase, rIndex);
      }
      store = newLIR3(cUnit, opcode, rSrc, regPtr, 0);
      oatFreeTemp(cUnit, regPtr);
      return store;
    case kWord:
      opcode = (thumbForm) ? kThumbStrRRR : kThumb2StrRRR;
      break;
    case kUnsignedHalf:
    case kSignedHalf:
      opcode = (thumbForm) ? kThumbStrhRRR : kThumb2StrhRRR;
      break;
    case kUnsignedByte:
    case kSignedByte:
      opcode = (thumbForm) ? kThumbStrbRRR : kThumb2StrbRRR;
      break;
    default:
      LOG(FATAL) << "Bad size: " << (int)size;
  }
  if (thumbForm)
    store = newLIR3(cUnit, opcode, rSrc, rBase, rIndex);
  else
    store = newLIR4(cUnit, opcode, rSrc, rBase, rIndex, scale);

  return store;
}

/*
 * Load value from base + displacement.  Optionally perform null check
 * on base (which must have an associated sReg and MIR).  If not
 * performing null check, incoming MIR can be null.
 */
LIR* loadBaseDispBody(CompilationUnit* cUnit, int rBase,
                      int displacement, int rDest, int rDestHi, OpSize size,
                      int sReg)
{
  LIR* res;
  LIR* load;
  ArmOpcode opcode = kThumbBkpt;
  bool shortForm = false;
  bool thumb2Form = (displacement < 4092 && displacement >= 0);
  bool allLowRegs = (ARM_LOWREG(rBase) && ARM_LOWREG(rDest));
  int encodedDisp = displacement;
  bool is64bit = false;
  switch (size) {
    case kDouble:
    case kLong:
      is64bit = true;
      if (ARM_FPREG(rDest)) {
        if (ARM_SINGLEREG(rDest)) {
          DCHECK(ARM_FPREG(rDestHi));
          rDest = s2d(rDest, rDestHi);
        }
        opcode = kThumb2Vldrd;
        if (displacement <= 1020) {
          shortForm = true;
          encodedDisp >>= 2;
        }
        break;
      } else {
        res = loadBaseDispBody(cUnit, rBase, displacement, rDest,
                               -1, kWord, sReg);
        loadBaseDispBody(cUnit, rBase, displacement + 4, rDestHi,
                         -1, kWord, INVALID_SREG);
        return res;
      }
    case kSingle:
    case kWord:
      if (ARM_FPREG(rDest)) {
        opcode = kThumb2Vldrs;
        if (displacement <= 1020) {
          shortForm = true;
          encodedDisp >>= 2;
        }
        break;
      }
      if (ARM_LOWREG(rDest) && (rBase == r15pc) &&
          (displacement <= 1020) && (displacement >= 0)) {
        shortForm = true;
        encodedDisp >>= 2;
        opcode = kThumbLdrPcRel;
      } else if (ARM_LOWREG(rDest) && (rBase == r13sp) &&
          (displacement <= 1020) && (displacement >= 0)) {
        shortForm = true;
        encodedDisp >>= 2;
        opcode = kThumbLdrSpRel;
      } else if (allLowRegs && displacement < 128 && displacement >= 0) {
        DCHECK_EQ((displacement & 0x3), 0);
        shortForm = true;
        encodedDisp >>= 2;
        opcode = kThumbLdrRRI5;
      } else if (thumb2Form) {
        shortForm = true;
        opcode = kThumb2LdrRRI12;
      }
      break;
    case kUnsignedHalf:
      if (allLowRegs && displacement < 64 && displacement >= 0) {
        DCHECK_EQ((displacement & 0x1), 0);
        shortForm = true;
        encodedDisp >>= 1;
        opcode = kThumbLdrhRRI5;
      } else if (displacement < 4092 && displacement >= 0) {
        shortForm = true;
        opcode = kThumb2LdrhRRI12;
      }
      break;
    case kSignedHalf:
      if (thumb2Form) {
        shortForm = true;
        opcode = kThumb2LdrshRRI12;
      }
      break;
    case kUnsignedByte:
      if (allLowRegs && displacement < 32 && displacement >= 0) {
        shortForm = true;
        opcode = kThumbLdrbRRI5;
      } else if (thumb2Form) {
        shortForm = true;
        opcode = kThumb2LdrbRRI12;
      }
      break;
    case kSignedByte:
      if (thumb2Form) {
        shortForm = true;
        opcode = kThumb2LdrsbRRI12;
      }
      break;
    default:
      LOG(FATAL) << "Bad size: " << (int)size;
  }

  if (shortForm) {
    load = res = newLIR3(cUnit, opcode, rDest, rBase, encodedDisp);
  } else {
    int regOffset = oatAllocTemp(cUnit);
    res = loadConstant(cUnit, regOffset, encodedDisp);
    load = loadBaseIndexed(cUnit, rBase, regOffset, rDest, 0, size);
    oatFreeTemp(cUnit, regOffset);
  }

  // TODO: in future may need to differentiate Dalvik accesses w/ spills
  if (rBase == rARM_SP) {
    annotateDalvikRegAccess(load, displacement >> 2, true /* isLoad */, is64bit);
  }
  return load;
}

LIR* loadBaseDisp(CompilationUnit* cUnit, int rBase,
                  int displacement, int rDest, OpSize size, int sReg)
{
  return loadBaseDispBody(cUnit, rBase, displacement, rDest, -1, size,
                          sReg);
}

 LIR* loadBaseDispWide(CompilationUnit* cUnit, int rBase,
                       int displacement, int rDestLo, int rDestHi, int sReg)
{
  return loadBaseDispBody(cUnit, rBase, displacement, rDestLo, rDestHi,
                          kLong, sReg);
}


LIR* storeBaseDispBody(CompilationUnit* cUnit, int rBase, int displacement,
                       int rSrc, int rSrcHi, OpSize size)
{
  LIR* res, *store;
  ArmOpcode opcode = kThumbBkpt;
  bool shortForm = false;
  bool thumb2Form = (displacement < 4092 && displacement >= 0);
  bool allLowRegs = (ARM_LOWREG(rBase) && ARM_LOWREG(rSrc));
  int encodedDisp = displacement;
  bool is64bit = false;
  switch (size) {
    case kLong:
    case kDouble:
      is64bit = true;
      if (!ARM_FPREG(rSrc)) {
        res = storeBaseDispBody(cUnit, rBase, displacement, rSrc, -1, kWord);
        storeBaseDispBody(cUnit, rBase, displacement + 4, rSrcHi, -1, kWord);
        return res;
      }
      if (ARM_SINGLEREG(rSrc)) {
        DCHECK(ARM_FPREG(rSrcHi));
        rSrc = s2d(rSrc, rSrcHi);
      }
      opcode = kThumb2Vstrd;
      if (displacement <= 1020) {
        shortForm = true;
        encodedDisp >>= 2;
      }
      break;
    case kSingle:
    case kWord:
      if (ARM_FPREG(rSrc)) {
        DCHECK(ARM_SINGLEREG(rSrc));
        opcode = kThumb2Vstrs;
        if (displacement <= 1020) {
          shortForm = true;
          encodedDisp >>= 2;
        }
      break;
      }
      if (allLowRegs && displacement < 128 && displacement >= 0) {
        DCHECK_EQ((displacement & 0x3), 0);
        shortForm = true;
        encodedDisp >>= 2;
        opcode = kThumbStrRRI5;
      } else if (thumb2Form) {
        shortForm = true;
        opcode = kThumb2StrRRI12;
      }
      break;
    case kUnsignedHalf:
    case kSignedHalf:
      if (allLowRegs && displacement < 64 && displacement >= 0) {
        DCHECK_EQ((displacement & 0x1), 0);
        shortForm = true;
        encodedDisp >>= 1;
        opcode = kThumbStrhRRI5;
      } else if (thumb2Form) {
        shortForm = true;
        opcode = kThumb2StrhRRI12;
      }
      break;
    case kUnsignedByte:
    case kSignedByte:
      if (allLowRegs && displacement < 32 && displacement >= 0) {
        shortForm = true;
        opcode = kThumbStrbRRI5;
      } else if (thumb2Form) {
        shortForm = true;
        opcode = kThumb2StrbRRI12;
      }
      break;
    default:
      LOG(FATAL) << "Bad size: " << (int)size;
  }
  if (shortForm) {
    store = res = newLIR3(cUnit, opcode, rSrc, rBase, encodedDisp);
  } else {
    int rScratch = oatAllocTemp(cUnit);
    res = loadConstant(cUnit, rScratch, encodedDisp);
    store = storeBaseIndexed(cUnit, rBase, rScratch, rSrc, 0, size);
    oatFreeTemp(cUnit, rScratch);
  }

  // TODO: In future, may need to differentiate Dalvik & spill accesses
  if (rBase == rARM_SP) {
    annotateDalvikRegAccess(store, displacement >> 2, false /* isLoad */,
                            is64bit);
  }
  return res;
}

LIR* storeBaseDisp(CompilationUnit* cUnit, int rBase, int displacement,
                   int rSrc, OpSize size)
{
  return storeBaseDispBody(cUnit, rBase, displacement, rSrc, -1, size);
}

LIR* storeBaseDispWide(CompilationUnit* cUnit, int rBase, int displacement,
                       int rSrcLo, int rSrcHi)
{
  return storeBaseDispBody(cUnit, rBase, displacement, rSrcLo, rSrcHi, kLong);
}

void loadPair(CompilationUnit* cUnit, int base, int lowReg, int highReg)
{
  loadBaseDispWide(cUnit, base, 0, lowReg, highReg, INVALID_SREG);
}

LIR* fpRegCopy(CompilationUnit* cUnit, int rDest, int rSrc)
{
  int opcode;
  DCHECK_EQ(ARM_DOUBLEREG(rDest), ARM_DOUBLEREG(rSrc));
  if (ARM_DOUBLEREG(rDest)) {
    opcode = kThumb2Vmovd;
  } else {
    if (ARM_SINGLEREG(rDest)) {
      opcode = ARM_SINGLEREG(rSrc) ? kThumb2Vmovs : kThumb2Fmsr;
    } else {
      DCHECK(ARM_SINGLEREG(rSrc));
      opcode = kThumb2Fmrs;
    }
  }
  LIR* res = rawLIR(cUnit, cUnit->currentDalvikOffset, opcode, rDest, rSrc);
  if (!(cUnit->disableOpt & (1 << kSafeOptimizations)) && rDest == rSrc) {
    res->flags.isNop = true;
  }
  return res;
}

LIR* opThreadMem(CompilationUnit* cUnit, OpKind op, int threadOffset)
{
  LOG(FATAL) << "Unexpected use of opThreadMem for Arm";
  return NULL;
}

LIR* opMem(CompilationUnit* cUnit, OpKind op, int rBase, int disp)
{
  LOG(FATAL) << "Unexpected use of opMem for Arm";
  return NULL;
}

LIR* storeBaseIndexedDisp(CompilationUnit *cUnit,
                          int rBase, int rIndex, int scale, int displacement,
                          int rSrc, int rSrcHi,
                          OpSize size, int sReg)
{
  LOG(FATAL) << "Unexpected use of storeBaseIndexedDisp for Arm";
  return NULL;
}

LIR* opRegMem(CompilationUnit *cUnit, OpKind op, int rDest, int rBase,
              int offset)
{
  LOG(FATAL) << "Unexpected use of opRegMem for Arm";
  return NULL;
}

LIR* loadBaseIndexedDisp(CompilationUnit *cUnit,
                         int rBase, int rIndex, int scale, int displacement,
                         int rDest, int rDestHi,
                         OpSize size, int sReg)
{
  LOG(FATAL) << "Unexpected use of loadBaseIndexedDisp for Arm";
  return NULL;
}

}  // namespace art
