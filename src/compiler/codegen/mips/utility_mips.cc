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

#include "mips_lir.h"
#include "../codegen_util.h"
#include "../ralloc_util.h"

namespace art {

/* This file contains codegen for the MIPS32 ISA. */

void GenBarrier(CompilationUnit *cUnit);
void LoadPair(CompilationUnit *cUnit, int base, int lowReg, int highReg);
LIR *LoadWordDisp(CompilationUnit *cUnit, int rBase, int displacement,
                      int rDest);
LIR *StoreWordDisp(CompilationUnit *cUnit, int rBase,
                       int displacement, int rSrc);
LIR *LoadConstant(CompilationUnit *cUnit, int rDest, int value);

#ifdef __mips_hard_float
LIR *FpRegCopy(CompilationUnit *cUnit, int rDest, int rSrc)
{
  int opcode;
  /* must be both DOUBLE or both not DOUBLE */
  DCHECK_EQ(MIPS_DOUBLEREG(rDest),MIPS_DOUBLEREG(rSrc));
  if (MIPS_DOUBLEREG(rDest)) {
    opcode = kMipsFmovd;
  } else {
    if (MIPS_SINGLEREG(rDest)) {
      if (MIPS_SINGLEREG(rSrc)) {
        opcode = kMipsFmovs;
      } else {
        /* note the operands are swapped for the mtc1 instr */
        int tOpnd = rSrc;
        rSrc = rDest;
        rDest = tOpnd;
        opcode = kMipsMtc1;
      }
    } else {
      DCHECK(MIPS_SINGLEREG(rSrc));
      opcode = kMipsMfc1;
    }
  }
  LIR* res = RawLIR(cUnit, cUnit->currentDalvikOffset, opcode, rSrc, rDest);
  if (!(cUnit->disableOpt & (1 << kSafeOptimizations)) && rDest == rSrc) {
    res->flags.isNop = true;
  }
  return res;
}
#endif

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
  LIR *res;

#ifdef __mips_hard_float
  int rDestSave = rDest;
  int isFpReg = MIPS_FPREG(rDest);
  if (isFpReg) {
    DCHECK(MIPS_SINGLEREG(rDest));
    rDest = AllocTemp(cUnit);
  }
#endif

  /* See if the value can be constructed cheaply */
  if (value == 0) {
    res = NewLIR2(cUnit, kMipsMove, rDest, r_ZERO);
  } else if ((value > 0) && (value <= 65535)) {
    res = NewLIR3(cUnit, kMipsOri, rDest, r_ZERO, value);
  } else if ((value < 0) && (value >= -32768)) {
    res = NewLIR3(cUnit, kMipsAddiu, rDest, r_ZERO, value);
  } else {
    res = NewLIR2(cUnit, kMipsLui, rDest, value>>16);
    if (value & 0xffff)
      NewLIR3(cUnit, kMipsOri, rDest, rDest, value);
  }

#ifdef __mips_hard_float
  if (isFpReg) {
    NewLIR2(cUnit, kMipsMtc1, rDest, rDestSave);
    FreeTemp(cUnit, rDest);
  }
#endif

  return res;
}

LIR *OpBranchUnconditional(CompilationUnit *cUnit, OpKind op)
{
  DCHECK_EQ(op, kOpUncondBr);
  return NewLIR1(cUnit, kMipsB, 0 /* offset to be patched */ );
}

LIR *LoadMultiple(CompilationUnit *cUnit, int rBase, int rMask);

LIR *OpReg(CompilationUnit *cUnit, OpKind op, int rDestSrc)
{
  MipsOpCode opcode = kMipsNop;
  switch (op) {
    case kOpBlx:
      opcode = kMipsJalr;
      break;
    case kOpBx:
      return NewLIR1(cUnit, kMipsJr, rDestSrc);
      break;
    default:
      LOG(FATAL) << "Bad case in OpReg";
  }
  return NewLIR2(cUnit, opcode, r_RA, rDestSrc);
}

LIR *OpRegRegImm(CompilationUnit *cUnit, OpKind op, int rDest,
           int rSrc1, int value);
LIR *OpRegImm(CompilationUnit *cUnit, OpKind op, int rDestSrc1,
          int value)
{
  LIR *res;
  bool neg = (value < 0);
  int absValue = (neg) ? -value : value;
  bool shortForm = (absValue & 0xff) == absValue;
  MipsOpCode opcode = kMipsNop;
  switch (op) {
    case kOpAdd:
      return OpRegRegImm(cUnit, op, rDestSrc1, rDestSrc1, value);
      break;
    case kOpSub:
      return OpRegRegImm(cUnit, op, rDestSrc1, rDestSrc1, value);
      break;
    default:
      LOG(FATAL) << "Bad case in OpRegImm";
      break;
  }
  if (shortForm)
    res = NewLIR2(cUnit, opcode, rDestSrc1, absValue);
  else {
    int rScratch = AllocTemp(cUnit);
    res = LoadConstant(cUnit, rScratch, value);
    if (op == kOpCmp)
      NewLIR2(cUnit, opcode, rDestSrc1, rScratch);
    else
      NewLIR3(cUnit, opcode, rDestSrc1, rDestSrc1, rScratch);
  }
  return res;
}

LIR *OpRegRegReg(CompilationUnit *cUnit, OpKind op, int rDest,
                 int rSrc1, int rSrc2)
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
  return NewLIR3(cUnit, opcode, rDest, rSrc1, rSrc2);
}

LIR *OpRegRegImm(CompilationUnit *cUnit, OpKind op, int rDest,
                 int rSrc1, int value)
{
  LIR *res;
  MipsOpCode opcode = kMipsNop;
  bool shortForm = true;

  switch (op) {
    case kOpAdd:
      if (IS_SIMM16(value)) {
        opcode = kMipsAddiu;
      }
      else {
        shortForm = false;
        opcode = kMipsAddu;
      }
      break;
    case kOpSub:
      if (IS_SIMM16((-value))) {
        value = -value;
        opcode = kMipsAddiu;
      }
      else {
        shortForm = false;
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
        shortForm = false;
        opcode = kMipsAnd;
      }
      break;
    case kOpOr:
      if (IS_UIMM16((value))) {
        opcode = kMipsOri;
      }
      else {
        shortForm = false;
        opcode = kMipsOr;
      }
      break;
    case kOpXor:
      if (IS_UIMM16((value))) {
        opcode = kMipsXori;
      }
      else {
        shortForm = false;
        opcode = kMipsXor;
      }
      break;
    case kOpMul:
      shortForm = false;
      opcode = kMipsMul;
      break;
    default:
      LOG(FATAL) << "Bad case in OpRegRegImm";
      break;
  }

  if (shortForm)
    res = NewLIR3(cUnit, opcode, rDest, rSrc1, value);
  else {
    if (rDest != rSrc1) {
      res = LoadConstant(cUnit, rDest, value);
      NewLIR3(cUnit, opcode, rDest, rSrc1, rDest);
    } else {
      int rScratch = AllocTemp(cUnit);
      res = LoadConstant(cUnit, rScratch, value);
      NewLIR3(cUnit, opcode, rDest, rSrc1, rScratch);
    }
  }
  return res;
}

LIR *OpRegReg(CompilationUnit *cUnit, OpKind op, int rDestSrc1, int rSrc2)
{
  MipsOpCode opcode = kMipsNop;
  LIR *res;
  switch (op) {
    case kOpMov:
      opcode = kMipsMove;
      break;
    case kOpMvn:
      return NewLIR3(cUnit, kMipsNor, rDestSrc1, rSrc2, r_ZERO);
    case kOpNeg:
      return NewLIR3(cUnit, kMipsSubu, rDestSrc1, r_ZERO, rSrc2);
    case kOpAdd:
    case kOpAnd:
    case kOpMul:
    case kOpOr:
    case kOpSub:
    case kOpXor:
      return OpRegRegReg(cUnit, op, rDestSrc1, rDestSrc1, rSrc2);
    case kOp2Byte:
#if __mips_isa_rev>=2
      res = NewLIR2(cUnit, kMipsSeb, rDestSrc1, rSrc2);
#else
      res = OpRegRegImm(cUnit, kOpLsl, rDestSrc1, rSrc2, 24);
      OpRegRegImm(cUnit, kOpAsr, rDestSrc1, rDestSrc1, 24);
#endif
      return res;
    case kOp2Short:
#if __mips_isa_rev>=2
      res = NewLIR2(cUnit, kMipsSeh, rDestSrc1, rSrc2);
#else
      res = OpRegRegImm(cUnit, kOpLsl, rDestSrc1, rSrc2, 16);
      OpRegRegImm(cUnit, kOpAsr, rDestSrc1, rDestSrc1, 16);
#endif
      return res;
    case kOp2Char:
       return NewLIR3(cUnit, kMipsAndi, rDestSrc1, rSrc2, 0xFFFF);
    default:
      LOG(FATAL) << "Bad case in OpRegReg";
      break;
  }
  return NewLIR2(cUnit, opcode, rDestSrc1, rSrc2);
}

LIR *LoadConstantValueWide(CompilationUnit *cUnit, int rDestLo,
                           int rDestHi, int valLo, int valHi)
{
  LIR *res;
  res = LoadConstantNoClobber(cUnit, rDestLo, valLo);
  LoadConstantNoClobber(cUnit, rDestHi, valHi);
  return res;
}

/* Load value from base + scaled index. */
LIR *LoadBaseIndexed(CompilationUnit *cUnit, int rBase,
                     int rIndex, int rDest, int scale, OpSize size)
{
  LIR *first = NULL;
  LIR *res;
  MipsOpCode opcode = kMipsNop;
  int tReg = AllocTemp(cUnit);

#ifdef __mips_hard_float
  if (MIPS_FPREG(rDest)) {
    DCHECK(MIPS_SINGLEREG(rDest));
    DCHECK((size == kWord) || (size == kSingle));
    size = kSingle;
  } else {
    if (size == kSingle)
      size = kWord;
  }
#endif

  if (!scale) {
    first = NewLIR3(cUnit, kMipsAddu, tReg , rBase, rIndex);
  } else {
    first = OpRegRegImm(cUnit, kOpLsl, tReg, rIndex, scale);
    NewLIR3(cUnit, kMipsAddu, tReg , rBase, tReg);
  }

  switch (size) {
#ifdef __mips_hard_float
    case kSingle:
      opcode = kMipsFlwc1;
      break;
#endif
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

  res = NewLIR3(cUnit, opcode, rDest, 0, tReg);
  FreeTemp(cUnit, tReg);
  return (first) ? first : res;
}

/* store value base base + scaled index. */
LIR *StoreBaseIndexed(CompilationUnit *cUnit, int rBase,
                      int rIndex, int rSrc, int scale, OpSize size)
{
  LIR *first = NULL;
  MipsOpCode opcode = kMipsNop;
  int rNewIndex = rIndex;
  int tReg = AllocTemp(cUnit);

#ifdef __mips_hard_float
  if (MIPS_FPREG(rSrc)) {
    DCHECK(MIPS_SINGLEREG(rSrc));
    DCHECK((size == kWord) || (size == kSingle));
    size = kSingle;
  } else {
    if (size == kSingle)
      size = kWord;
  }
#endif

  if (!scale) {
    first = NewLIR3(cUnit, kMipsAddu, tReg , rBase, rIndex);
  } else {
    first = OpRegRegImm(cUnit, kOpLsl, tReg, rIndex, scale);
    NewLIR3(cUnit, kMipsAddu, tReg , rBase, tReg);
  }

  switch (size) {
#ifdef __mips_hard_float
    case kSingle:
      opcode = kMipsFswc1;
      break;
#endif
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
  NewLIR3(cUnit, opcode, rSrc, 0, tReg);
  FreeTemp(cUnit, rNewIndex);
  return first;
}

LIR *LoadMultiple(CompilationUnit *cUnit, int rBase, int rMask)
{
  int i;
  int loadCnt = 0;
  LIR *res = NULL ;
  GenBarrier(cUnit);

  for (i = 0; i < 8; i++, rMask >>= 1) {
    if (rMask & 0x1) { /* map r0 to MIPS r_A0 */
      NewLIR3(cUnit, kMipsLw, i+r_A0, loadCnt*4, rBase);
      loadCnt++;
    }
  }

  if (loadCnt) {/* increment after */
    NewLIR3(cUnit, kMipsAddiu, rBase, rBase, loadCnt*4);
  }

  GenBarrier(cUnit);
  return res; /* NULL always returned which should be ok since no callers use it */
}

LIR *StoreMultiple(CompilationUnit *cUnit, int rBase, int rMask)
{
  int i;
  int storeCnt = 0;
  LIR *res = NULL ;
  GenBarrier(cUnit);

  for (i = 0; i < 8; i++, rMask >>= 1) {
    if (rMask & 0x1) { /* map r0 to MIPS r_A0 */
      NewLIR3(cUnit, kMipsSw, i+r_A0, storeCnt*4, rBase);
      storeCnt++;
    }
  }

  if (storeCnt) { /* increment after */
    NewLIR3(cUnit, kMipsAddiu, rBase, rBase, storeCnt*4);
  }

  GenBarrier(cUnit);
  return res; /* NULL always returned which should be ok since no callers use it */
}

LIR *LoadBaseDispBody(CompilationUnit *cUnit, int rBase,
                      int displacement, int rDest, int rDestHi,
                      OpSize size, int sReg)
/*
 * Load value from base + displacement.  Optionally perform null check
 * on base (which must have an associated sReg and MIR).  If not
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
  bool shortForm = IS_SIMM16(displacement);
  bool pair = false;

  switch (size) {
    case kLong:
    case kDouble:
      pair = true;
      opcode = kMipsLw;
#ifdef __mips_hard_float
      if (MIPS_FPREG(rDest)) {
        opcode = kMipsFlwc1;
        if (MIPS_DOUBLEREG(rDest)) {
          rDest = rDest - MIPS_FP_DOUBLE;
        } else {
          DCHECK(MIPS_FPREG(rDestHi));
          DCHECK(rDest == (rDestHi - 1));
        }
        rDestHi = rDest + 1;
      }
#endif
      shortForm = IS_SIMM16_2WORD(displacement);
      DCHECK_EQ((displacement & 0x3), 0);
      break;
    case kWord:
    case kSingle:
      opcode = kMipsLw;
#ifdef __mips_hard_float
      if (MIPS_FPREG(rDest)) {
        opcode = kMipsFlwc1;
        DCHECK(MIPS_SINGLEREG(rDest));
      }
#endif
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

  if (shortForm) {
    if (!pair) {
      load = res = NewLIR3(cUnit, opcode, rDest, displacement, rBase);
    } else {
      load = res = NewLIR3(cUnit, opcode, rDest,
                           displacement + LOWORD_OFFSET, rBase);
      load2 = NewLIR3(cUnit, opcode, rDestHi,
                      displacement + HIWORD_OFFSET, rBase);
    }
  } else {
    if (pair) {
      int rTmp = AllocFreeTemp(cUnit);
      res = OpRegRegImm(cUnit, kOpAdd, rTmp, rBase, displacement);
      load = NewLIR3(cUnit, opcode, rDest, LOWORD_OFFSET, rTmp);
      load2 = NewLIR3(cUnit, opcode, rDestHi, HIWORD_OFFSET, rTmp);
      FreeTemp(cUnit, rTmp);
    } else {
      int rTmp = (rBase == rDest) ? AllocFreeTemp(cUnit) : rDest;
      res = OpRegRegImm(cUnit, kOpAdd, rTmp, rBase, displacement);
      load = NewLIR3(cUnit, opcode, rDest, 0, rTmp);
      if (rTmp != rDest)
        FreeTemp(cUnit, rTmp);
    }
  }

  if (rBase == rMIPS_SP) {
    AnnotateDalvikRegAccess(load,
                            (displacement + (pair ? LOWORD_OFFSET : 0)) >> 2,
                            true /* isLoad */, pair /* is64bit */);
    if (pair) {
      AnnotateDalvikRegAccess(load2, (displacement + HIWORD_OFFSET) >> 2,
                              true /* isLoad */, pair /* is64bit */);
    }
  }
  return load;
}

LIR *LoadBaseDisp(CompilationUnit *cUnit, int rBase,
                  int displacement, int rDest, OpSize size, int sReg)
{
  return LoadBaseDispBody(cUnit, rBase, displacement, rDest, -1,
                          size, sReg);
}

LIR *LoadBaseDispWide(CompilationUnit *cUnit, int rBase,
                      int displacement, int rDestLo, int rDestHi, int sReg)
{
  return LoadBaseDispBody(cUnit, rBase, displacement, rDestLo, rDestHi,
                          kLong, sReg);
}

LIR *StoreBaseDispBody(CompilationUnit *cUnit, int rBase,
                       int displacement, int rSrc, int rSrcHi, OpSize size)
{
  LIR *res;
  LIR *store = NULL;
  LIR *store2 = NULL;
  MipsOpCode opcode = kMipsNop;
  bool shortForm = IS_SIMM16(displacement);
  bool pair = false;

  switch (size) {
    case kLong:
    case kDouble:
      pair = true;
      opcode = kMipsSw;
#ifdef __mips_hard_float
      if (MIPS_FPREG(rSrc)) {
        opcode = kMipsFswc1;
        if (MIPS_DOUBLEREG(rSrc)) {
          rSrc = rSrc - MIPS_FP_DOUBLE;
        } else {
          DCHECK(MIPS_FPREG(rSrcHi));
          DCHECK_EQ(rSrc, (rSrcHi - 1));
        }
        rSrcHi = rSrc + 1;
      }
#endif
      shortForm = IS_SIMM16_2WORD(displacement);
      DCHECK_EQ((displacement & 0x3), 0);
      break;
    case kWord:
    case kSingle:
      opcode = kMipsSw;
#ifdef __mips_hard_float
      if (MIPS_FPREG(rSrc)) {
        opcode = kMipsFswc1;
        DCHECK(MIPS_SINGLEREG(rSrc));
      }
#endif
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

  if (shortForm) {
    if (!pair) {
      store = res = NewLIR3(cUnit, opcode, rSrc, displacement, rBase);
    } else {
      store = res = NewLIR3(cUnit, opcode, rSrc, displacement + LOWORD_OFFSET,
                            rBase);
      store2 = NewLIR3(cUnit, opcode, rSrcHi, displacement + HIWORD_OFFSET,
                       rBase);
    }
  } else {
    int rScratch = AllocTemp(cUnit);
    res = OpRegRegImm(cUnit, kOpAdd, rScratch, rBase, displacement);
    if (!pair) {
      store =  NewLIR3(cUnit, opcode, rSrc, 0, rScratch);
    } else {
      store =  NewLIR3(cUnit, opcode, rSrc, LOWORD_OFFSET, rScratch);
      store2 = NewLIR3(cUnit, opcode, rSrcHi, HIWORD_OFFSET, rScratch);
    }
    FreeTemp(cUnit, rScratch);
  }

  if (rBase == rMIPS_SP) {
    AnnotateDalvikRegAccess(store, (displacement + (pair ? LOWORD_OFFSET : 0))
                            >> 2, false /* isLoad */, pair /* is64bit */);
    if (pair) {
      AnnotateDalvikRegAccess(store2, (displacement + HIWORD_OFFSET) >> 2,
                              false /* isLoad */, pair /* is64bit */);
    }
  }

  return res;
}

LIR *StoreBaseDisp(CompilationUnit *cUnit, int rBase,
                   int displacement, int rSrc, OpSize size)
{
  return StoreBaseDispBody(cUnit, rBase, displacement, rSrc, -1, size);
}

LIR *StoreBaseDispWide(CompilationUnit *cUnit, int rBase,
                       int displacement, int rSrcLo, int rSrcHi)
{
  return StoreBaseDispBody(cUnit, rBase, displacement, rSrcLo, rSrcHi, kLong);
}

void LoadPair(CompilationUnit *cUnit, int base, int lowReg, int highReg)
{
  LoadWordDisp(cUnit, base, LOWORD_OFFSET , lowReg);
  LoadWordDisp(cUnit, base, HIWORD_OFFSET , highReg);
}

LIR* OpThreadMem(CompilationUnit* cUnit, OpKind op, int threadOffset)
{
  LOG(FATAL) << "Unexpected use of OpThreadMem for MIPS";
  return NULL;
}

LIR* OpMem(CompilationUnit* cUnit, OpKind op, int rBase, int disp)
{
  LOG(FATAL) << "Unexpected use of OpMem for MIPS";
  return NULL;
}

LIR* StoreBaseIndexedDisp(CompilationUnit *cUnit,
                          int rBase, int rIndex, int scale, int displacement,
                          int rSrc, int rSrcHi,
                          OpSize size, int sReg)
{
  LOG(FATAL) << "Unexpected use of StoreBaseIndexedDisp for MIPS";
  return NULL;
}

LIR* OpRegMem(CompilationUnit *cUnit, OpKind op, int rDest, int rBase,
              int offset)
{
  LOG(FATAL) << "Unexpected use of OpRegMem for MIPS";
  return NULL;
}

LIR* LoadBaseIndexedDisp(CompilationUnit *cUnit,
                         int rBase, int rIndex, int scale, int displacement,
                         int rDest, int rDestHi,
                         OpSize size, int sReg)
{
  LOG(FATAL) << "Unexpected use of LoadBaseIndexedDisp for MIPS";
  return NULL;
}

LIR* OpCondBranch(CompilationUnit* cUnit, ConditionCode cc, LIR* target)
{
  LOG(FATAL) << "Unexpected use of OpCondBranch for MIPS";
  return NULL;
}

}  // namespace art
