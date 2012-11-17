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
#include "x86_lir.h"
#include "../ralloc_util.h"
#include "../codegen_util.h"

#include <string>

namespace art {

//FIXME: restore "static" when usage uncovered
/*static*/ int coreRegs[] = {
  rAX, rCX, rDX, rBX, rX86_SP, rBP, rSI, rDI
#ifdef TARGET_REX_SUPPORT
  r8, r9, r10, r11, r12, r13, r14, 15
#endif
};
/*static*/ int reservedRegs[] = {rX86_SP};
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

RegLocation locCReturn()
{
  RegLocation res = X86_LOC_C_RETURN;
  return res;
}

RegLocation locCReturnWide()
{
  RegLocation res = X86_LOC_C_RETURN_WIDE;
  return res;
}

RegLocation locCReturnFloat()
{
  RegLocation res = X86_LOC_C_RETURN_FLOAT;
  return res;
}

RegLocation locCReturnDouble()
{
  RegLocation res = X86_LOC_C_RETURN_DOUBLE;
  return res;
}

// Return a target-dependent special register.
int targetReg(SpecialTargetRegister reg) {
  int res = INVALID_REG;
  switch (reg) {
    case kSelf: res = rX86_SELF; break;
    case kSuspend: res =  rX86_SUSPEND; break;
    case kLr: res =  rX86_LR; break;
    case kPc: res =  rX86_PC; break;
    case kSp: res =  rX86_SP; break;
    case kArg0: res = rX86_ARG0; break;
    case kArg1: res = rX86_ARG1; break;
    case kArg2: res = rX86_ARG2; break;
    case kArg3: res = rX86_ARG3; break;
    case kFArg0: res = rX86_FARG0; break;
    case kFArg1: res = rX86_FARG1; break;
    case kFArg2: res = rX86_FARG2; break;
    case kFArg3: res = rX86_FARG3; break;
    case kRet0: res = rX86_RET0; break;
    case kRet1: res = rX86_RET1; break;
    case kInvokeTgt: res = rX86_INVOKE_TGT; break;
    case kCount: res = rX86_COUNT; break;
  }
  return res;
}

// Create a double from a pair of singles.
int s2d(int lowReg, int highReg)
{
  return X86_S2D(lowReg, highReg);
}

// Is reg a single or double?
bool fpReg(int reg)
{
  return X86_FPREG(reg);
}

// Is reg a single?
bool singleReg(int reg)
{
  return X86_SINGLEREG(reg);
}

// Is reg a double?
bool doubleReg(int reg)
{
  return X86_DOUBLEREG(reg);
}

// Return mask to strip off fp reg flags and bias.
uint32_t fpRegMask()
{
  return X86_FP_REG_MASK;
}

// True if both regs single, both core or both double.
bool sameRegType(int reg1, int reg2)
{
  return (X86_REGTYPE(reg1) == X86_REGTYPE(reg2));
}

/*
 * Decode the register id.
 */
uint64_t getRegMaskCommon(CompilationUnit* cUnit, int reg)
{
  uint64_t seed;
  int shift;
  int regId;

  regId = reg & 0xf;
  /* Double registers in x86 are just a single FP register */
  seed = 1;
  /* FP register starts at bit position 16 */
  shift = X86_FPREG(reg) ? kX86FPReg0 : 0;
  /* Expand the double register id into single offset */
  shift += regId;
  return (seed << shift);
}

uint64_t getPCUseDefEncoding()
{
  /*
   * FIXME: might make sense to use a virtual resource encoding bit for pc.  Might be
   * able to clean up some of the x86/Arm_Mips differences
   */
  LOG(FATAL) << "Unexpected call to getPCUseDefEncoding for x86";
  return 0ULL;
}

void setupTargetResourceMasks(CompilationUnit* cUnit, LIR* lir)
{
  DCHECK_EQ(cUnit->instructionSet, kX86);

  // X86-specific resource map setup here.
  uint64_t flags = EncodingMap[lir->opcode].flags;

  if (flags & REG_USE_SP) {
    lir->useMask |= ENCODE_X86_REG_SP;
  }

  if (flags & REG_DEF_SP) {
    lir->defMask |= ENCODE_X86_REG_SP;
  }

  if (flags & REG_DEFA) {
    oatSetupRegMask(cUnit, &lir->defMask, rAX);
  }

  if (flags & REG_DEFD) {
    oatSetupRegMask(cUnit, &lir->defMask, rDX);
  }
  if (flags & REG_USEA) {
    oatSetupRegMask(cUnit, &lir->useMask, rAX);
  }

  if (flags & REG_USEC) {
    oatSetupRegMask(cUnit, &lir->useMask, rCX);
  }

  if (flags & REG_USED) {
    oatSetupRegMask(cUnit, &lir->useMask, rDX);
  }
}

/* For dumping instructions */
static const char* x86RegName[] = {
  "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
  "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
};

static const char* x86CondName[] = {
  "O",
  "NO",
  "B/NAE/C",
  "NB/AE/NC",
  "Z/EQ",
  "NZ/NE",
  "BE/NA",
  "NBE/A",
  "S",
  "NS",
  "P/PE",
  "NP/PO",
  "L/NGE",
  "NL/GE",
  "LE/NG",
  "NLE/G"
};

/*
 * Interpret a format string and build a string no longer than size
 * See format key in Assemble.cc.
 */
std::string buildInsnString(const char *fmt, LIR *lir, unsigned char* baseAddr) {
  std::string buf;
  size_t i = 0;
  size_t fmt_len = strlen(fmt);
  while (i < fmt_len) {
    if (fmt[i] != '!') {
      buf += fmt[i];
      i++;
    } else {
      i++;
      DCHECK_LT(i, fmt_len);
      char operand_number_ch = fmt[i];
      i++;
      if (operand_number_ch == '!') {
        buf += "!";
      } else {
        int operand_number = operand_number_ch - '0';
        DCHECK_LT(operand_number, 6);  // Expect upto 6 LIR operands.
        DCHECK_LT(i, fmt_len);
        int operand = lir->operands[operand_number];
        switch (fmt[i]) {
          case 'c':
            DCHECK_LT(static_cast<size_t>(operand), sizeof(x86CondName));
            buf += x86CondName[operand];
            break;
          case 'd':
            buf += StringPrintf("%d", operand);
            break;
          case 'p': {
            SwitchTable *tabRec = reinterpret_cast<SwitchTable*>(operand);
            buf += StringPrintf("0x%08x", tabRec->offset);
            break;
          }
          case 'r':
            if (X86_FPREG(operand) || X86_DOUBLEREG(operand)) {
              int fp_reg = operand & X86_FP_REG_MASK;
              buf += StringPrintf("xmm%d", fp_reg);
            } else {
              DCHECK_LT(static_cast<size_t>(operand), sizeof(x86RegName));
              buf += x86RegName[operand];
            }
            break;
          case 't':
            buf += StringPrintf("0x%08x (L%p)",
                                reinterpret_cast<uint32_t>(baseAddr)
                                + lir->offset + operand, lir->target);
            break;
          default:
            buf += StringPrintf("DecodeError '%c'", fmt[i]);
            break;
        }
        i++;
      }
    }
  }
  return buf;
}

void oatDumpResourceMask(LIR *lir, uint64_t mask, const char *prefix)
{
  char buf[256];
  buf[0] = 0;
  LIR *x86LIR = (LIR *) lir;

  if (mask == ENCODE_ALL) {
    strcpy(buf, "all");
  } else {
    char num[8];
    int i;

    for (i = 0; i < kX86RegEnd; i++) {
      if (mask & (1ULL << i)) {
        sprintf(num, "%d ", i);
        strcat(buf, num);
      }
    }

    if (mask & ENCODE_CCODE) {
      strcat(buf, "cc ");
    }
    /* Memory bits */
    if (x86LIR && (mask & ENCODE_DALVIK_REG)) {
      sprintf(buf + strlen(buf), "dr%d%s", x86LIR->aliasInfo & 0xffff,
              (x86LIR->aliasInfo & 0x80000000) ? "(+1)" : "");
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
void oatAdjustSpillMask(CompilationUnit* cUnit) {
  // Adjustment for LR spilling, x86 has no LR so nothing to do here
  cUnit->coreSpillMask |= (1 << rRET);
  cUnit->numCoreSpills++;
}

/*
 * Mark a callee-save fp register as promoted.  Note that
 * vpush/vpop uses contiguous register lists so we must
 * include any holes in the mask.  Associate holes with
 * Dalvik register INVALID_VREG (0xFFFFU).
 */
void oatMarkPreservedSingle(CompilationUnit* cUnit, int vReg, int reg)
{
  UNIMPLEMENTED(WARNING) << "oatMarkPreservedSingle";
#if 0
  LOG(FATAL) << "No support yet for promoted FP regs";
#endif
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
    storeBaseDispWide(cUnit, rX86_SP, oatVRegOffset(cUnit, vReg), info1->reg, info1->partner);
  }
}

void oatFlushReg(CompilationUnit* cUnit, int reg)
{
  RegisterInfo* info = oatGetRegInfo(cUnit, reg);
  if (info->live && info->dirty) {
    info->dirty = false;
    int vReg = SRegToVReg(cUnit, info->sReg);
    storeBaseDisp(cUnit, rX86_SP, oatVRegOffset(cUnit, vReg), reg, kWord);
  }
}

/* Give access to the target-dependent FP register encoding to common code */
bool oatIsFpReg(int reg) {
  return X86_FPREG(reg);
}

uint32_t oatFpRegMask() {
  return X86_FP_REG_MASK;
}

/* Clobber all regs that might be used by an external C call */
extern void oatClobberCalleeSave(CompilationUnit *cUnit)
{
  oatClobber(cUnit, rAX);
  oatClobber(cUnit, rCX);
  oatClobber(cUnit, rDX);
}

extern RegLocation oatGetReturnWideAlt(CompilationUnit* cUnit) {
  RegLocation res = locCReturnWide();
  CHECK(res.lowReg == rAX);
  CHECK(res.highReg == rDX);
  oatClobber(cUnit, rAX);
  oatClobber(cUnit, rDX);
  oatMarkInUse(cUnit, rAX);
  oatMarkInUse(cUnit, rDX);
  oatMarkPair(cUnit, res.lowReg, res.highReg);
  return res;
}

extern RegLocation oatGetReturnAlt(CompilationUnit* cUnit)
{
  RegLocation res = locCReturn();
  res.lowReg = rDX;
  oatClobber(cUnit, rDX);
  oatMarkInUse(cUnit, rDX);
  return res;
}

extern RegisterInfo* oatGetRegInfo(CompilationUnit* cUnit, int reg)
{
  return X86_FPREG(reg) ? &cUnit->regPool->FPRegs[reg & X86_FP_REG_MASK]
                    : &cUnit->regPool->coreRegs[reg];
}

/* To be used when explicitly managing register use */
extern void oatLockCallTemps(CompilationUnit* cUnit)
{
  oatLockTemp(cUnit, rX86_ARG0);
  oatLockTemp(cUnit, rX86_ARG1);
  oatLockTemp(cUnit, rX86_ARG2);
  oatLockTemp(cUnit, rX86_ARG3);
}

/* To be used when explicitly managing register use */
extern void oatFreeCallTemps(CompilationUnit* cUnit)
{
  oatFreeTemp(cUnit, rX86_ARG0);
  oatFreeTemp(cUnit, rX86_ARG1);
  oatFreeTemp(cUnit, rX86_ARG2);
  oatFreeTemp(cUnit, rX86_ARG3);
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
  return kX86;
}

/* Architecture-specific initializations and checks go here */
bool oatArchVariantInit(void)
{
  return true;
}

void oatGenMemBarrier(CompilationUnit *cUnit, int /* barrierKind */)
{
#if ANDROID_SMP != 0
  // TODO: optimize fences
  newLIR0(cUnit, kX86Mfence);
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

  if (((regClass == kAnyReg) && fpHint) || (regClass == kFPReg)) {
    lowReg = oatAllocTempDouble(cUnit);
    highReg = lowReg + 1;
    res = (lowReg & 0xff) | ((highReg & 0xff) << 8);
    return res;
  }

  lowReg = oatAllocTemp(cUnit);
  highReg = oatAllocTemp(cUnit);
  res = (lowReg & 0xff) | ((highReg & 0xff) << 8);
  return res;
}

int oatAllocTypedTemp(CompilationUnit *cUnit, bool fpHint, int regClass) {
  if (((regClass == kAnyReg) && fpHint) || (regClass == kFPReg)) {
    return oatAllocTempFloat(cUnit);
  }
  return oatAllocTemp(cUnit);
}

void oatInitializeRegAlloc(CompilationUnit* cUnit) {
  int numRegs = sizeof(coreRegs)/sizeof(*coreRegs);
  int numReserved = sizeof(reservedRegs)/sizeof(*reservedRegs);
  int numTemps = sizeof(coreTemps)/sizeof(*coreTemps);
  int numFPRegs = sizeof(fpRegs)/sizeof(*fpRegs);
  int numFPTemps = sizeof(fpTemps)/sizeof(*fpTemps);
  RegisterPool *pool = (RegisterPool *)oatNew(cUnit, sizeof(*pool), true,
                                              kAllocRegAlloc);
  cUnit->regPool = pool;
  pool->numCoreRegs = numRegs;
  pool->coreRegs = (RegisterInfo *)
      oatNew(cUnit, numRegs * sizeof(*cUnit->regPool->coreRegs), true,
             kAllocRegAlloc);
  pool->numFPRegs = numFPRegs;
  pool->FPRegs = (RegisterInfo *)
      oatNew(cUnit, numFPRegs * sizeof(*cUnit->regPool->FPRegs), true,
             kAllocRegAlloc);
  oatInitPool(pool->coreRegs, coreRegs, pool->numCoreRegs);
  oatInitPool(pool->FPRegs, fpRegs, pool->numFPRegs);
  // Keep special registers from being allocated
  for (int i = 0; i < numReserved; i++) {
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

void spillCoreRegs(CompilationUnit* cUnit) {
  if (cUnit->numCoreSpills == 0) {
    return;
  }
  // Spill mask not including fake return address register
  uint32_t mask = cUnit->coreSpillMask & ~(1 << rRET);
  int offset = cUnit->frameSize - (4 * cUnit->numCoreSpills);
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      storeWordDisp(cUnit, rX86_SP, offset, reg);
      offset += 4;
    }
  }
}

void unSpillCoreRegs(CompilationUnit* cUnit) {
  if (cUnit->numCoreSpills == 0) {
    return;
  }
  // Spill mask not including fake return address register
  uint32_t mask = cUnit->coreSpillMask & ~(1 << rRET);
  int offset = cUnit->frameSize - (4 * cUnit->numCoreSpills);
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      loadWordDisp(cUnit, rX86_SP, offset, reg);
      offset += 4;
    }
  }
}

/*
 * Nop any unconditional branches that go to the next instruction.
 * Note: new redundant branches may be inserted later, and we'll
 * use a check in final instruction assembly to nop those out.
 */
void removeRedundantBranches(CompilationUnit* cUnit) {
  LIR* thisLIR;

  for (thisLIR = (LIR*) cUnit->firstLIRInsn;
    thisLIR != (LIR*) cUnit->lastLIRInsn;
    thisLIR = NEXT_LIR(thisLIR)) {

  /* Branch to the next instruction */
  if (thisLIR->opcode == kX86Jmp8 || thisLIR->opcode == kX86Jmp32) {
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
bool oatArchInit() {
  int i;

  for (i = 0; i < kX86Last; i++) {
    if (EncodingMap[i].opcode != i) {
      LOG(FATAL) << "Encoding order for " << EncodingMap[i].name
                 << " is wrong: expecting " << i << ", seeing "
                 << (int)EncodingMap[i].opcode;
    }
  }

  return oatArchVariantInit();
}

// Not used in x86
int loadHelper(CompilationUnit* cUnit, int offset)
{
  LOG(FATAL) << "Unexpected use of loadHelper in x86";
  return INVALID_REG;
}

} // namespace art
