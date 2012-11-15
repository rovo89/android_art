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

#include "../../compiler_internals.h"
#include "mips_lir.h"
#include "../ralloc_util.h"
#include "../codegen_util.h"

#include <string>

namespace art {

static int coreRegs[] = {r_ZERO, r_AT, r_V0, r_V1, r_A0, r_A1, r_A2, r_A3,
                         r_T0, r_T1, r_T2, r_T3, r_T4, r_T5, r_T6, r_T7,
                         r_S0, r_S1, r_S2, r_S3, r_S4, r_S5, r_S6, r_S7, r_T8,
                         r_T9, r_K0, r_K1, r_GP, r_SP, r_FP, r_RA};
static int reservedRegs[] = {r_ZERO, r_AT, r_S0, r_S1, r_K0, r_K1, r_GP, r_SP,
                             r_RA};
static int coreTemps[] = {r_V0, r_V1, r_A0, r_A1, r_A2, r_A3, r_T0, r_T1, r_T2,
                          r_T3, r_T4, r_T5, r_T6, r_T7, r_T8};
#ifdef __mips_hard_float
static int fpRegs[] = {r_F0, r_F1, r_F2, r_F3, r_F4, r_F5, r_F6, r_F7,
                       r_F8, r_F9, r_F10, r_F11, r_F12, r_F13, r_F14, r_F15};
static int fpTemps[] = {r_F0, r_F1, r_F2, r_F3, r_F4, r_F5, r_F6, r_F7,
                        r_F8, r_F9, r_F10, r_F11, r_F12, r_F13, r_F14, r_F15};
#endif

RegLocation locCReturn()
{
  RegLocation res = MIPS_LOC_C_RETURN;
  return res;
}

RegLocation locCReturnWide()
{
  RegLocation res = MIPS_LOC_C_RETURN_WIDE;
  return res;
}

RegLocation locCReturnFloat()
{
  RegLocation res = MIPS_LOC_C_RETURN_FLOAT;
  return res;
}

RegLocation locCReturnDouble()
{
  RegLocation res = MIPS_LOC_C_RETURN_DOUBLE;
  return res;
}

// Return a target-dependent special register.
int targetReg(SpecialTargetRegister reg) {
  int res = INVALID_REG;
  switch (reg) {
    case kSelf: res = rMIPS_SELF; break;
    case kSuspend: res =  rMIPS_SUSPEND; break;
    case kLr: res =  rMIPS_LR; break;
    case kPc: res =  rMIPS_PC; break;
    case kSp: res =  rMIPS_SP; break;
    case kArg0: res = rMIPS_ARG0; break;
    case kArg1: res = rMIPS_ARG1; break;
    case kArg2: res = rMIPS_ARG2; break;
    case kArg3: res = rMIPS_ARG3; break;
    case kFArg0: res = rMIPS_FARG0; break;
    case kFArg1: res = rMIPS_FARG1; break;
    case kFArg2: res = rMIPS_FARG2; break;
    case kFArg3: res = rMIPS_FARG3; break;
    case kRet0: res = rMIPS_RET0; break;
    case kRet1: res = rMIPS_RET1; break;
    case kInvokeTgt: res = rMIPS_INVOKE_TGT; break;
    case kCount: res = rMIPS_COUNT; break;
  }
  return res;
}

// Create a double from a pair of singles.
int s2d(int lowReg, int highReg)
{
  return MIPS_S2D(lowReg, highReg);
}

// Is reg a single or double?
bool fpReg(int reg)
{
  return MIPS_FPREG(reg);
}

// Is reg a single?
bool singleReg(int reg)
{
  return MIPS_SINGLEREG(reg);
}

// Is reg a double?
bool doubleReg(int reg)
{
  return MIPS_DOUBLEREG(reg);
}

// Return mask to strip off fp reg flags and bias.
uint32_t fpRegMask()
{
  return MIPS_FP_REG_MASK;
}

// True if both regs single, both core or both double.
bool sameRegType(int reg1, int reg2)
{
  return (MIPS_REGTYPE(reg1) == MIPS_REGTYPE(reg2));
}

/*
 * Decode the register id.
 */
uint64_t getRegMaskCommon(CompilationUnit* cUnit, int reg)
{
  uint64_t seed;
  int shift;
  int regId;


  regId = reg & 0x1f;
  /* Each double register is equal to a pair of single-precision FP registers */
  seed = MIPS_DOUBLEREG(reg) ? 3 : 1;
  /* FP register starts at bit position 16 */
  shift = MIPS_FPREG(reg) ? kMipsFPReg0 : 0;
  /* Expand the double register id into single offset */
  shift += regId;
  return (seed << shift);
}

uint64_t getPCUseDefEncoding()
{
  return ENCODE_MIPS_REG_PC;
}


void setupTargetResourceMasks(CompilationUnit* cUnit, LIR* lir)
{
  DCHECK_EQ(cUnit->instructionSet, kMips);

  // Mips-specific resource map setup here.
  uint64_t flags = EncodingMap[lir->opcode].flags;

  if (flags & REG_DEF_SP) {
    lir->defMask |= ENCODE_MIPS_REG_SP;
  }

  if (flags & REG_USE_SP) {
    lir->useMask |= ENCODE_MIPS_REG_SP;
  }

  if (flags & REG_DEF_LR) {
    lir->defMask |= ENCODE_MIPS_REG_LR;
  }
}

/* For dumping instructions */
#define MIPS_REG_COUNT 32
static const char *mipsRegName[MIPS_REG_COUNT] = {
  "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
  "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
  "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
  "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

/*
 * Interpret a format string and build a string no longer than size
 * See format key in Assemble.c.
 */
std::string buildInsnString(const char *fmt, LIR *lir, unsigned char* baseAddr)
{
  std::string buf;
  int i;
  const char *fmtEnd = &fmt[strlen(fmt)];
  char tbuf[256];
  char nc;
  while (fmt < fmtEnd) {
    int operand;
    if (*fmt == '!') {
      fmt++;
      DCHECK_LT(fmt, fmtEnd);
      nc = *fmt++;
      if (nc=='!') {
        strcpy(tbuf, "!");
      } else {
         DCHECK_LT(fmt, fmtEnd);
         DCHECK_LT((unsigned)(nc-'0'), 4u);
         operand = lir->operands[nc-'0'];
         switch (*fmt++) {
           case 'b':
             strcpy(tbuf,"0000");
             for (i=3; i>= 0; i--) {
               tbuf[i] += operand & 1;
               operand >>= 1;
             }
             break;
           case 's':
             sprintf(tbuf,"$f%d",operand & MIPS_FP_REG_MASK);
             break;
           case 'S':
             DCHECK_EQ(((operand & MIPS_FP_REG_MASK) & 1), 0);
             sprintf(tbuf,"$f%d",operand & MIPS_FP_REG_MASK);
             break;
           case 'h':
             sprintf(tbuf,"%04x", operand);
             break;
           case 'M':
           case 'd':
             sprintf(tbuf,"%d", operand);
             break;
           case 'D':
             sprintf(tbuf,"%d", operand+1);
             break;
           case 'E':
             sprintf(tbuf,"%d", operand*4);
             break;
           case 'F':
             sprintf(tbuf,"%d", operand*2);
             break;
           case 't':
             sprintf(tbuf,"0x%08x (L%p)", (int) baseAddr + lir->offset + 4 +
                     (operand << 2), lir->target);
             break;
           case 'T':
             sprintf(tbuf,"0x%08x", (int) (operand << 2));
             break;
           case 'u': {
             int offset_1 = lir->operands[0];
             int offset_2 = NEXT_LIR(lir)->operands[0];
             intptr_t target =
                 ((((intptr_t) baseAddr + lir->offset + 4) & ~3) +
                 (offset_1 << 21 >> 9) + (offset_2 << 1)) & 0xfffffffc;
             sprintf(tbuf, "%p", (void *) target);
             break;
          }

           /* Nothing to print for BLX_2 */
           case 'v':
             strcpy(tbuf, "see above");
             break;
           case 'r':
             DCHECK(operand >= 0 && operand < MIPS_REG_COUNT);
             strcpy(tbuf, mipsRegName[operand]);
             break;
           case 'N':
             // Placeholder for delay slot handling
             strcpy(tbuf, ";  nop");
             break;
           default:
             strcpy(tbuf,"DecodeError");
             break;
         }
         buf += tbuf;
      }
    } else {
       buf += *fmt++;
    }
  }
  return buf;
}

// FIXME: need to redo resource maps for MIPS - fix this at that time
void oatDumpResourceMask(LIR *lir, uint64_t mask, const char *prefix)
{
  char buf[256];
  buf[0] = 0;
  LIR *mipsLIR = (LIR *) lir;

  if (mask == ENCODE_ALL) {
    strcpy(buf, "all");
  } else {
    char num[8];
    int i;

    for (i = 0; i < kMipsRegEnd; i++) {
      if (mask & (1ULL << i)) {
        sprintf(num, "%d ", i);
        strcat(buf, num);
      }
    }

    if (mask & ENCODE_CCODE) {
      strcat(buf, "cc ");
    }
    if (mask & ENCODE_FP_STATUS) {
      strcat(buf, "fpcc ");
    }
    /* Memory bits */
    if (mipsLIR && (mask & ENCODE_DALVIK_REG)) {
      sprintf(buf + strlen(buf), "dr%d%s", mipsLIR->aliasInfo & 0xffff,
              (mipsLIR->aliasInfo & 0x80000000) ? "(+1)" : "");
    }
    if (mask & ENCODE_LITERAL) {
      strcat(buf, "lit ");
    }

    if (mask & ENCODE_HEAP_REF) {
      strcat(buf, "heap ");
    }
    if (mask & ENCODE_MUST_NOT_ALIAS) {
      strcat(buf, "noalias ");
    }
  }
  if (buf[0]) {
    LOG(INFO) << prefix << ": " <<  buf;
  }
}

/*
 * TUNING: is leaf?  Can't just use "hasInvoke" to determine as some
 * instructions might call out to C/assembly helper functions.  Until
 * machinery is in place, always spill lr.
 */

void oatAdjustSpillMask(CompilationUnit* cUnit)
{
  cUnit->coreSpillMask |= (1 << r_RA);
  cUnit->numCoreSpills++;
}

/*
 * Mark a callee-save fp register as promoted.  Note that
 * vpush/vpop uses contiguous register lists so we must
 * include any holes in the mask.  Associate holes with
 * Dalvik register INVALID_VREG (0xFFFFU).
 */
void oatMarkPreservedSingle(CompilationUnit* cUnit, int sReg, int reg)
{
  LOG(FATAL) << "No support yet for promoted FP regs";
}

void oatFlushRegWide(CompilationUnit* cUnit, int reg1, int reg2)
{
  RegisterInfo* info1 = oatGetRegInfo(cUnit, reg1);
  RegisterInfo* info2 = oatGetRegInfo(cUnit, reg2);
  DCHECK(info1 && info2 && info1->pair && info2->pair &&
         (info1->partner == info2->reg) &&
         (info2->partner == info1->reg));
  if ((info1->live && info1->dirty) || (info2->live && info2->dirty)) {
    if (!(info1->isTemp && info2->isTemp)) {
      /* Should not happen.  If it does, there's a problem in evalLoc */
      LOG(FATAL) << "Long half-temp, half-promoted";
    }

    info1->dirty = false;
    info2->dirty = false;
    if (SRegToVReg(cUnit, info2->sReg) < SRegToVReg(cUnit, info1->sReg))
      info1 = info2;
    int vReg = SRegToVReg(cUnit, info1->sReg);
    storeBaseDispWide(cUnit, rMIPS_SP, oatVRegOffset(cUnit, vReg), info1->reg, info1->partner);
  }
}

void oatFlushReg(CompilationUnit* cUnit, int reg)
{
  RegisterInfo* info = oatGetRegInfo(cUnit, reg);
  if (info->live && info->dirty) {
    info->dirty = false;
    int vReg = SRegToVReg(cUnit, info->sReg);
    storeBaseDisp(cUnit, rMIPS_SP, oatVRegOffset(cUnit, vReg), reg, kWord);
  }
}

/* Give access to the target-dependent FP register encoding to common code */
bool oatIsFpReg(int reg) {
  return MIPS_FPREG(reg);
}

uint32_t oatFpRegMask() {
  return MIPS_FP_REG_MASK;
}

/* Clobber all regs that might be used by an external C call */
extern void oatClobberCalleeSave(CompilationUnit *cUnit)
{
  oatClobber(cUnit, r_ZERO);
  oatClobber(cUnit, r_AT);
  oatClobber(cUnit, r_V0);
  oatClobber(cUnit, r_V1);
  oatClobber(cUnit, r_A0);
  oatClobber(cUnit, r_A1);
  oatClobber(cUnit, r_A2);
  oatClobber(cUnit, r_A3);
  oatClobber(cUnit, r_T0);
  oatClobber(cUnit, r_T1);
  oatClobber(cUnit, r_T2);
  oatClobber(cUnit, r_T3);
  oatClobber(cUnit, r_T4);
  oatClobber(cUnit, r_T5);
  oatClobber(cUnit, r_T6);
  oatClobber(cUnit, r_T7);
  oatClobber(cUnit, r_T8);
  oatClobber(cUnit, r_T9);
  oatClobber(cUnit, r_K0);
  oatClobber(cUnit, r_K1);
  oatClobber(cUnit, r_GP);
  oatClobber(cUnit, r_FP);
  oatClobber(cUnit, r_RA);
  oatClobber(cUnit, r_F0);
  oatClobber(cUnit, r_F1);
  oatClobber(cUnit, r_F2);
  oatClobber(cUnit, r_F3);
  oatClobber(cUnit, r_F4);
  oatClobber(cUnit, r_F5);
  oatClobber(cUnit, r_F6);
  oatClobber(cUnit, r_F7);
  oatClobber(cUnit, r_F8);
  oatClobber(cUnit, r_F9);
  oatClobber(cUnit, r_F10);
  oatClobber(cUnit, r_F11);
  oatClobber(cUnit, r_F12);
  oatClobber(cUnit, r_F13);
  oatClobber(cUnit, r_F14);
  oatClobber(cUnit, r_F15);
}

extern RegLocation oatGetReturnWideAlt(CompilationUnit* cUnit)
{
  UNIMPLEMENTED(FATAL) << "No oatGetReturnWideAlt for MIPS";
  RegLocation res = locCReturnWide();
  return res;
}

extern RegLocation oatGetReturnAlt(CompilationUnit* cUnit)
{
  UNIMPLEMENTED(FATAL) << "No oatGetReturnAlt for MIPS";
  RegLocation res = locCReturn();
  return res;
}

extern RegisterInfo* oatGetRegInfo(CompilationUnit* cUnit, int reg)
{
  return MIPS_FPREG(reg) ? &cUnit->regPool->FPRegs[reg & MIPS_FP_REG_MASK]
            : &cUnit->regPool->coreRegs[reg];
}

/* To be used when explicitly managing register use */
extern void oatLockCallTemps(CompilationUnit* cUnit)
{
  oatLockTemp(cUnit, rMIPS_ARG0);
  oatLockTemp(cUnit, rMIPS_ARG1);
  oatLockTemp(cUnit, rMIPS_ARG2);
  oatLockTemp(cUnit, rMIPS_ARG3);
}

/* To be used when explicitly managing register use */
extern void oatFreeCallTemps(CompilationUnit* cUnit)
{
  oatFreeTemp(cUnit, rMIPS_ARG0);
  oatFreeTemp(cUnit, rMIPS_ARG1);
  oatFreeTemp(cUnit, rMIPS_ARG2);
  oatFreeTemp(cUnit, rMIPS_ARG3);
}

/* Convert an instruction to a NOP */
void oatNopLIR( LIR* lir)
{
  ((LIR*)lir)->flags.isNop = true;
}

/*
 * Determine the initial instruction set to be used for this trace.
 * Later components may decide to change this.
 */
InstructionSet oatInstructionSet()
{
  return kMips;
}

/* Architecture-specific initializations and checks go here */
bool oatArchVariantInit(void)
{
  return true;
}

void oatGenMemBarrier(CompilationUnit *cUnit, int barrierKind)
{
#if ANDROID_SMP != 0
  newLIR1(cUnit, kMipsSync, barrierKind);
#endif
}

/*
 * Alloc a pair of core registers, or a double.  Low reg in low byte,
 * high reg in next byte.
 */
int oatAllocTypedTempPair(CompilationUnit *cUnit, bool fpHint,
                  int regClass)
{
  int highReg;
  int lowReg;
  int res = 0;

#ifdef __mips_hard_float
  if (((regClass == kAnyReg) && fpHint) || (regClass == kFPReg)) {
    lowReg = oatAllocTempDouble(cUnit);
    highReg = lowReg + 1;
    res = (lowReg & 0xff) | ((highReg & 0xff) << 8);
    return res;
  }
#endif

  lowReg = oatAllocTemp(cUnit);
  highReg = oatAllocTemp(cUnit);
  res = (lowReg & 0xff) | ((highReg & 0xff) << 8);
  return res;
}

int oatAllocTypedTemp(CompilationUnit *cUnit, bool fpHint, int regClass)
{
#ifdef __mips_hard_float
  if (((regClass == kAnyReg) && fpHint) || (regClass == kFPReg))
{
    return oatAllocTempFloat(cUnit);
}
#endif
  return oatAllocTemp(cUnit);
}

void oatInitializeRegAlloc(CompilationUnit* cUnit)
{
  int numRegs = sizeof(coreRegs)/sizeof(*coreRegs);
  int numReserved = sizeof(reservedRegs)/sizeof(*reservedRegs);
  int numTemps = sizeof(coreTemps)/sizeof(*coreTemps);
#ifdef __mips_hard_float
  int numFPRegs = sizeof(fpRegs)/sizeof(*fpRegs);
  int numFPTemps = sizeof(fpTemps)/sizeof(*fpTemps);
#else
  int numFPRegs = 0;
  int numFPTemps = 0;
#endif
  RegisterPool *pool = (RegisterPool *)oatNew(cUnit, sizeof(*pool), true,
                        kAllocRegAlloc);
  cUnit->regPool = pool;
  pool->numCoreRegs = numRegs;
  pool->coreRegs = (RegisterInfo *)
      oatNew(cUnit, numRegs * sizeof(*cUnit->regPool->coreRegs),
             true, kAllocRegAlloc);
  pool->numFPRegs = numFPRegs;
  pool->FPRegs = (RegisterInfo *)
      oatNew(cUnit, numFPRegs * sizeof(*cUnit->regPool->FPRegs), true,
             kAllocRegAlloc);
  oatInitPool(pool->coreRegs, coreRegs, pool->numCoreRegs);
  oatInitPool(pool->FPRegs, fpRegs, pool->numFPRegs);
  // Keep special registers from being allocated
  for (int i = 0; i < numReserved; i++) {
    if (NO_SUSPEND && (reservedRegs[i] == rMIPS_SUSPEND)) {
      //To measure cost of suspend check
      continue;
    }
    oatMarkInUse(cUnit, reservedRegs[i]);
  }
  // Mark temp regs - all others not in use can be used for promotion
  for (int i = 0; i < numTemps; i++) {
    oatMarkTemp(cUnit, coreTemps[i]);
  }
  for (int i = 0; i < numFPTemps; i++) {
    oatMarkTemp(cUnit, fpTemps[i]);
  }
  // Construct the alias map.
  cUnit->phiAliasMap = (int*)oatNew(cUnit, cUnit->numSSARegs *
                                    sizeof(cUnit->phiAliasMap[0]), false,
                                    kAllocDFInfo);
  for (int i = 0; i < cUnit->numSSARegs; i++) {
    cUnit->phiAliasMap[i] = i;
  }
  for (MIR* phi = cUnit->phiList; phi; phi = phi->meta.phiNext) {
    int defReg = phi->ssaRep->defs[0];
    for (int i = 0; i < phi->ssaRep->numUses; i++) {
       for (int j = 0; j < cUnit->numSSARegs; j++) {
         if (cUnit->phiAliasMap[j] == phi->ssaRep->uses[i]) {
           cUnit->phiAliasMap[j] = defReg;
         }
       }
    }
  }
}

void freeRegLocTemps(CompilationUnit* cUnit, RegLocation rlKeep,
           RegLocation rlFree)
{
  if ((rlFree.lowReg != rlKeep.lowReg) && (rlFree.lowReg != rlKeep.highReg) &&
    (rlFree.highReg != rlKeep.lowReg) && (rlFree.highReg != rlKeep.highReg)) {
    // No overlap, free both
    oatFreeTemp(cUnit, rlFree.lowReg);
    oatFreeTemp(cUnit, rlFree.highReg);
  }
}
/*
 * In the Arm code a it is typical to use the link register
 * to hold the target address.  However, for Mips we must
 * ensure that all branch instructions can be restarted if
 * there is a trap in the shadow.  Allocate a temp register.
 */
int loadHelper(CompilationUnit* cUnit, int offset)
{
  loadWordDisp(cUnit, rMIPS_SELF, offset, r_T9);
  return r_T9;
}

void spillCoreRegs(CompilationUnit* cUnit)
{
  if (cUnit->numCoreSpills == 0) {
    return;
  }
  uint32_t mask = cUnit->coreSpillMask;
  int offset = cUnit->numCoreSpills * 4;
  opRegImm(cUnit, kOpSub, rMIPS_SP, offset);
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      offset -= 4;
      storeWordDisp(cUnit, rMIPS_SP, offset, reg);
    }
  }
}

void unSpillCoreRegs(CompilationUnit* cUnit)
{
  if (cUnit->numCoreSpills == 0) {
    return;
  }
  uint32_t mask = cUnit->coreSpillMask;
  int offset = cUnit->frameSize;
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      offset -= 4;
      loadWordDisp(cUnit, rMIPS_SP, offset, reg);
    }
  }
  opRegImm(cUnit, kOpAdd, rMIPS_SP, cUnit->frameSize);
}

/*
 * Nop any unconditional branches that go to the next instruction.
 * Note: new redundant branches may be inserted later, and we'll
 * use a check in final instruction assembly to nop those out.
 */
void removeRedundantBranches(CompilationUnit* cUnit)
{
  LIR* thisLIR;

  for (thisLIR = (LIR*) cUnit->firstLIRInsn;
     thisLIR != (LIR*) cUnit->lastLIRInsn;
     thisLIR = NEXT_LIR(thisLIR)) {

    /* Branch to the next instruction */
    if (thisLIR->opcode == kMipsB) {
      LIR* nextLIR = thisLIR;

      while (true) {
        nextLIR = NEXT_LIR(nextLIR);

        /*
         * Is the branch target the next instruction?
         */
        if (nextLIR == (LIR*) thisLIR->target) {
          thisLIR->flags.isNop = true;
          break;
        }

        /*
         * Found real useful stuff between the branch and the target.
         * Need to explicitly check the lastLIRInsn here because it
         * might be the last real instruction.
         */
        if (!isPseudoOpcode(nextLIR->opcode) ||
          (nextLIR = (LIR*) cUnit->lastLIRInsn))
          break;
      }
    }
  }
}


/* Common initialization routine for an architecture family */
bool oatArchInit()
{
  int i;

  for (i = 0; i < kMipsLast; i++) {
    if (EncodingMap[i].opcode != i) {
      LOG(FATAL) << "Encoding order for " << EncodingMap[i].name <<
         " is wrong: expecting " << i << ", seeing " <<
         (int)EncodingMap[i].opcode;
    }
  }

  return oatArchVariantInit();
}

} // namespace art
