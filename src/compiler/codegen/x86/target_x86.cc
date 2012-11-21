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
/*static*/ int ReservedRegs[] = {rX86_SP};
/*static*/ int coreTemps[] = {rAX, rCX, rDX, rBX};
/*static*/ int FpRegs[] = {
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

RegLocation LocCReturn()
{
  RegLocation res = X86_LOC_C_RETURN;
  return res;
}

RegLocation LocCReturnWide()
{
  RegLocation res = X86_LOC_C_RETURN_WIDE;
  return res;
}

RegLocation LocCReturnFloat()
{
  RegLocation res = X86_LOC_C_RETURN_FLOAT;
  return res;
}

RegLocation LocCReturnDouble()
{
  RegLocation res = X86_LOC_C_RETURN_DOUBLE;
  return res;
}

// Return a target-dependent special register.
int TargetReg(SpecialTargetRegister reg) {
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
int S2d(int lowReg, int highReg)
{
  return X86_S2D(lowReg, highReg);
}

// Is reg a single or double?
bool FpReg(int reg)
{
  return X86_FPREG(reg);
}

// Is reg a single?
bool SingleReg(int reg)
{
  return X86_SINGLEREG(reg);
}

// Is reg a double?
bool DoubleReg(int reg)
{
  return X86_DOUBLEREG(reg);
}

// Return mask to strip off fp reg flags and bias.
uint32_t FpRegMask()
{
  return X86_FP_REG_MASK;
}

// True if both regs single, both core or both double.
bool SameRegType(int reg1, int reg2)
{
  return (X86_REGTYPE(reg1) == X86_REGTYPE(reg2));
}

/*
 * Decode the register id.
 */
uint64_t GetRegMaskCommon(CompilationUnit* cUnit, int reg)
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

uint64_t GetPCUseDefEncoding()
{
  /*
   * FIXME: might make sense to use a virtual resource encoding bit for pc.  Might be
   * able to clean up some of the x86/Arm_Mips differences
   */
  LOG(FATAL) << "Unexpected call to GetPCUseDefEncoding for x86";
  return 0ULL;
}

void SetupTargetResourceMasks(CompilationUnit* cUnit, LIR* lir)
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
    SetupRegMask(cUnit, &lir->defMask, rAX);
  }

  if (flags & REG_DEFD) {
    SetupRegMask(cUnit, &lir->defMask, rDX);
  }
  if (flags & REG_USEA) {
    SetupRegMask(cUnit, &lir->useMask, rAX);
  }

  if (flags & REG_USEC) {
    SetupRegMask(cUnit, &lir->useMask, rCX);
  }

  if (flags & REG_USED) {
    SetupRegMask(cUnit, &lir->useMask, rDX);
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
std::string BuildInsnString(const char *fmt, LIR *lir, unsigned char* baseAddr) {
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

void DumpResourceMask(LIR *x86LIR, uint64_t mask, const char *prefix)
{
  char buf[256];
  buf[0] = 0;

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
void AdjustSpillMask(CompilationUnit* cUnit) {
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
void MarkPreservedSingle(CompilationUnit* cUnit, int vReg, int reg)
{
  UNIMPLEMENTED(WARNING) << "MarkPreservedSingle";
#if 0
  LOG(FATAL) << "No support yet for promoted FP regs";
#endif
}

void FlushRegWide(CompilationUnit* cUnit, int reg1, int reg2)
{
  RegisterInfo* info1 = GetRegInfo(cUnit, reg1);
  RegisterInfo* info2 = GetRegInfo(cUnit, reg2);
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
    StoreBaseDispWide(cUnit, rX86_SP, VRegOffset(cUnit, vReg), info1->reg, info1->partner);
  }
}

void FlushReg(CompilationUnit* cUnit, int reg)
{
  RegisterInfo* info = GetRegInfo(cUnit, reg);
  if (info->live && info->dirty) {
    info->dirty = false;
    int vReg = SRegToVReg(cUnit, info->sReg);
    StoreBaseDisp(cUnit, rX86_SP, VRegOffset(cUnit, vReg), reg, kWord);
  }
}

/* Give access to the target-dependent FP register encoding to common code */
bool IsFpReg(int reg) {
  return X86_FPREG(reg);
}

/* Clobber all regs that might be used by an external C call */
extern void ClobberCalleeSave(CompilationUnit *cUnit)
{
  Clobber(cUnit, rAX);
  Clobber(cUnit, rCX);
  Clobber(cUnit, rDX);
}

extern RegLocation GetReturnWideAlt(CompilationUnit* cUnit) {
  RegLocation res = LocCReturnWide();
  CHECK(res.lowReg == rAX);
  CHECK(res.highReg == rDX);
  Clobber(cUnit, rAX);
  Clobber(cUnit, rDX);
  MarkInUse(cUnit, rAX);
  MarkInUse(cUnit, rDX);
  MarkPair(cUnit, res.lowReg, res.highReg);
  return res;
}

extern RegLocation GetReturnAlt(CompilationUnit* cUnit)
{
  RegLocation res = LocCReturn();
  res.lowReg = rDX;
  Clobber(cUnit, rDX);
  MarkInUse(cUnit, rDX);
  return res;
}

extern RegisterInfo* GetRegInfo(CompilationUnit* cUnit, int reg)
{
  return X86_FPREG(reg) ? &cUnit->regPool->FPRegs[reg & X86_FP_REG_MASK]
                    : &cUnit->regPool->coreRegs[reg];
}

/* To be used when explicitly managing register use */
extern void LockCallTemps(CompilationUnit* cUnit)
{
  LockTemp(cUnit, rX86_ARG0);
  LockTemp(cUnit, rX86_ARG1);
  LockTemp(cUnit, rX86_ARG2);
  LockTemp(cUnit, rX86_ARG3);
}

/* To be used when explicitly managing register use */
extern void FreeCallTemps(CompilationUnit* cUnit)
{
  FreeTemp(cUnit, rX86_ARG0);
  FreeTemp(cUnit, rX86_ARG1);
  FreeTemp(cUnit, rX86_ARG2);
  FreeTemp(cUnit, rX86_ARG3);
}

/* Architecture-specific initializations and checks go here */
bool ArchVariantInit(void)
{
  return true;
}

void GenMemBarrier(CompilationUnit *cUnit, MemBarrierKind barrierKind)
{
#if ANDROID_SMP != 0
  // TODO: optimize fences
  NewLIR0(cUnit, kX86Mfence);
#endif
}
/*
 * Alloc a pair of core registers, or a double.  Low reg in low byte,
 * high reg in next byte.
 */
int AllocTypedTempPair(CompilationUnit *cUnit, bool fpHint,
                          int regClass)
{
  int highReg;
  int lowReg;
  int res = 0;

  if (((regClass == kAnyReg) && fpHint) || (regClass == kFPReg)) {
    lowReg = AllocTempDouble(cUnit);
    highReg = lowReg + 1;
    res = (lowReg & 0xff) | ((highReg & 0xff) << 8);
    return res;
  }

  lowReg = AllocTemp(cUnit);
  highReg = AllocTemp(cUnit);
  res = (lowReg & 0xff) | ((highReg & 0xff) << 8);
  return res;
}

int AllocTypedTemp(CompilationUnit *cUnit, bool fpHint, int regClass) {
  if (((regClass == kAnyReg) && fpHint) || (regClass == kFPReg)) {
    return AllocTempFloat(cUnit);
  }
  return AllocTemp(cUnit);
}

void CompilerInitializeRegAlloc(CompilationUnit* cUnit) {
  int numRegs = sizeof(coreRegs)/sizeof(*coreRegs);
  int numReserved = sizeof(ReservedRegs)/sizeof(*ReservedRegs);
  int numTemps = sizeof(coreTemps)/sizeof(*coreTemps);
  int numFPRegs = sizeof(FpRegs)/sizeof(*FpRegs);
  int numFPTemps = sizeof(fpTemps)/sizeof(*fpTemps);
  RegisterPool *pool =
      static_cast<RegisterPool*>(NewMem(cUnit, sizeof(*pool), true, kAllocRegAlloc));
  cUnit->regPool = pool;
  pool->numCoreRegs = numRegs;
  pool->coreRegs =
      static_cast<RegisterInfo*>(NewMem(cUnit, numRegs * sizeof(*cUnit->regPool->coreRegs),
                                             true, kAllocRegAlloc));
  pool->numFPRegs = numFPRegs;
  pool->FPRegs =
      static_cast<RegisterInfo *>(NewMem(cUnit, numFPRegs * sizeof(*cUnit->regPool->FPRegs),
                                              true, kAllocRegAlloc));
  CompilerInitPool(pool->coreRegs, coreRegs, pool->numCoreRegs);
  CompilerInitPool(pool->FPRegs, FpRegs, pool->numFPRegs);
  // Keep special registers from being allocated
  for (int i = 0; i < numReserved; i++) {
    MarkInUse(cUnit, ReservedRegs[i]);
  }
  // Mark temp regs - all others not in use can be used for promotion
  for (int i = 0; i < numTemps; i++) {
    MarkTemp(cUnit, coreTemps[i]);
  }
  for (int i = 0; i < numFPTemps; i++) {
    MarkTemp(cUnit, fpTemps[i]);
  }
  // Construct the alias map.
  cUnit->phiAliasMap = static_cast<int*>
      (NewMem(cUnit, cUnit->numSSARegs * sizeof(cUnit->phiAliasMap[0]), false, kAllocDFInfo));
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

void FreeRegLocTemps(CompilationUnit* cUnit, RegLocation rlKeep,
                     RegLocation rlFree)
{
  if ((rlFree.lowReg != rlKeep.lowReg) && (rlFree.lowReg != rlKeep.highReg) &&
      (rlFree.highReg != rlKeep.lowReg) && (rlFree.highReg != rlKeep.highReg)) {
    // No overlap, free both
    FreeTemp(cUnit, rlFree.lowReg);
    FreeTemp(cUnit, rlFree.highReg);
  }
}

void SpillCoreRegs(CompilationUnit* cUnit) {
  if (cUnit->numCoreSpills == 0) {
    return;
  }
  // Spill mask not including fake return address register
  uint32_t mask = cUnit->coreSpillMask & ~(1 << rRET);
  int offset = cUnit->frameSize - (4 * cUnit->numCoreSpills);
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      StoreWordDisp(cUnit, rX86_SP, offset, reg);
      offset += 4;
    }
  }
}

void UnSpillCoreRegs(CompilationUnit* cUnit) {
  if (cUnit->numCoreSpills == 0) {
    return;
  }
  // Spill mask not including fake return address register
  uint32_t mask = cUnit->coreSpillMask & ~(1 << rRET);
  int offset = cUnit->frameSize - (4 * cUnit->numCoreSpills);
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      LoadWordDisp(cUnit, rX86_SP, offset, reg);
      offset += 4;
    }
  }
}

bool BranchUnconditional(LIR* lir)
{
  return (lir->opcode == kX86Jmp8 || lir->opcode == kX86Jmp32);
}

/* Common initialization routine for an architecture family */
bool ArchInit() {
  int i;

  for (i = 0; i < kX86Last; i++) {
    if (EncodingMap[i].opcode != i) {
      LOG(FATAL) << "Encoding order for " << EncodingMap[i].name
                 << " is wrong: expecting " << i << ", seeing "
                 << static_cast<int>(EncodingMap[i].opcode);
    }
  }

  return ArchVariantInit();
}

// Not used in x86
int LoadHelper(CompilationUnit* cUnit, int offset)
{
  LOG(FATAL) << "Unexpected use of LoadHelper in x86";
  return INVALID_REG;
}

uint64_t GetTargetInstFlags(int opcode)
{
  return EncodingMap[opcode].flags;
}

const char* GetTargetInstName(int opcode)
{
  return EncodingMap[opcode].name;
}

const char* GetTargetInstFmt(int opcode)
{
  return EncodingMap[opcode].fmt;
}

} // namespace art
