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

#include "../../compiler_internals.h"
#include "arm_lir.h"
#include "../ralloc_util.h"
#include "../codegen_util.h"

#include <string>

namespace art {

static int coreRegs[] = {r0, r1, r2, r3, rARM_SUSPEND, r5, r6, r7, r8, rARM_SELF, r10,
                         r11, r12, rARM_SP, rARM_LR, rARM_PC};
static int ReservedRegs[] = {rARM_SUSPEND, rARM_SELF, rARM_SP, rARM_LR, rARM_PC};
static int FpRegs[] = {fr0, fr1, fr2, fr3, fr4, fr5, fr6, fr7,
                       fr8, fr9, fr10, fr11, fr12, fr13, fr14, fr15,
                       fr16, fr17, fr18, fr19, fr20, fr21, fr22, fr23,
                       fr24, fr25, fr26, fr27, fr28, fr29, fr30, fr31};
static int coreTemps[] = {r0, r1, r2, r3, r12};
static int fpTemps[] = {fr0, fr1, fr2, fr3, fr4, fr5, fr6, fr7,
                        fr8, fr9, fr10, fr11, fr12, fr13, fr14, fr15};

RegLocation LocCReturn()
{
  RegLocation res = ARM_LOC_C_RETURN;
  return res;
}

RegLocation LocCReturnWide()
{
  RegLocation res = ARM_LOC_C_RETURN_WIDE;
  return res;
}

RegLocation LocCReturnFloat()
{
  RegLocation res = ARM_LOC_C_RETURN_FLOAT;
  return res;
}

RegLocation LocCReturnDouble()
{
  RegLocation res = ARM_LOC_C_RETURN_DOUBLE;
  return res;
}

// Return a target-dependent special register.
int TargetReg(SpecialTargetRegister reg) {
  int res = INVALID_REG;
  switch (reg) {
    case kSelf: res = rARM_SELF; break;
    case kSuspend: res =  rARM_SUSPEND; break;
    case kLr: res =  rARM_LR; break;
    case kPc: res =  rARM_PC; break;
    case kSp: res =  rARM_SP; break;
    case kArg0: res = rARM_ARG0; break;
    case kArg1: res = rARM_ARG1; break;
    case kArg2: res = rARM_ARG2; break;
    case kArg3: res = rARM_ARG3; break;
    case kFArg0: res = rARM_FARG0; break;
    case kFArg1: res = rARM_FARG1; break;
    case kFArg2: res = rARM_FARG2; break;
    case kFArg3: res = rARM_FARG3; break;
    case kRet0: res = rARM_RET0; break;
    case kRet1: res = rARM_RET1; break;
    case kInvokeTgt: res = rARM_INVOKE_TGT; break;
    case kCount: res = rARM_COUNT; break;
  }
  return res;
}


// Create a double from a pair of singles.
int S2d(int lowReg, int highReg)
{
  return ARM_S2D(lowReg, highReg);
}

// Is reg a single or double?
bool FpReg(int reg)
{
  return ARM_FPREG(reg);
}

// Is reg a single?
bool SingleReg(int reg)
{
  return ARM_SINGLEREG(reg);
}

// Is reg a double?
bool DoubleReg(int reg)
{
  return ARM_DOUBLEREG(reg);
}

// Return mask to strip off fp reg flags and bias.
uint32_t FpRegMask()
{
  return ARM_FP_REG_MASK;
}

// True if both regs single, both core or both double.
bool SameRegType(int reg1, int reg2)
{
  return (ARM_REGTYPE(reg1) == ARM_REGTYPE(reg2));
}

/*
 * Decode the register id.
 */
uint64_t GetRegMaskCommon(CompilationUnit* cUnit, int reg)
{
  uint64_t seed;
  int shift;
  int regId;


  regId = reg & 0x1f;
  /* Each double register is equal to a pair of single-precision FP registers */
  seed = ARM_DOUBLEREG(reg) ? 3 : 1;
  /* FP register starts at bit position 16 */
  shift = ARM_FPREG(reg) ? kArmFPReg0 : 0;
  /* Expand the double register id into single offset */
  shift += regId;
  return (seed << shift);
}

uint64_t GetPCUseDefEncoding()
{
  return ENCODE_ARM_REG_PC;
}

void SetupTargetResourceMasks(CompilationUnit* cUnit, LIR* lir)
{
  DCHECK_EQ(cUnit->instructionSet, kThumb2);

  // Thumb2 specific setup
  uint64_t flags = EncodingMap[lir->opcode].flags;
  int opcode = lir->opcode;

  if (flags & REG_DEF_SP) {
    lir->defMask |= ENCODE_ARM_REG_SP;
  }

  if (flags & REG_USE_SP) {
    lir->useMask |= ENCODE_ARM_REG_SP;
  }

  if (flags & REG_DEF_LIST0) {
    lir->defMask |= ENCODE_ARM_REG_LIST(lir->operands[0]);
  }

  if (flags & REG_DEF_LIST1) {
    lir->defMask |= ENCODE_ARM_REG_LIST(lir->operands[1]);
  }

  if (flags & REG_DEF_FPCS_LIST0) {
    lir->defMask |= ENCODE_ARM_REG_FPCS_LIST(lir->operands[0]);
  }

  if (flags & REG_DEF_FPCS_LIST2) {
    for (int i = 0; i < lir->operands[2]; i++) {
      SetupRegMask(cUnit, &lir->defMask, lir->operands[1] + i);
    }
  }

  if (flags & REG_USE_PC) {
    lir->useMask |= ENCODE_ARM_REG_PC;
  }

  /* Conservatively treat the IT block */
  if (flags & IS_IT) {
    lir->defMask = ENCODE_ALL;
  }

  if (flags & REG_USE_LIST0) {
    lir->useMask |= ENCODE_ARM_REG_LIST(lir->operands[0]);
  }

  if (flags & REG_USE_LIST1) {
    lir->useMask |= ENCODE_ARM_REG_LIST(lir->operands[1]);
  }

  if (flags & REG_USE_FPCS_LIST0) {
    lir->useMask |= ENCODE_ARM_REG_FPCS_LIST(lir->operands[0]);
  }

  if (flags & REG_USE_FPCS_LIST2) {
    for (int i = 0; i < lir->operands[2]; i++) {
      SetupRegMask(cUnit, &lir->useMask, lir->operands[1] + i);
    }
  }
  /* Fixup for kThumbPush/lr and kThumbPop/pc */
  if (opcode == kThumbPush || opcode == kThumbPop) {
    uint64_t r8Mask = GetRegMaskCommon(cUnit, r8);
    if ((opcode == kThumbPush) && (lir->useMask & r8Mask)) {
      lir->useMask &= ~r8Mask;
      lir->useMask |= ENCODE_ARM_REG_LR;
    } else if ((opcode == kThumbPop) && (lir->defMask & r8Mask)) {
      lir->defMask &= ~r8Mask;
      lir->defMask |= ENCODE_ARM_REG_PC;
    }
  }
  if (flags & REG_DEF_LR) {
    lir->defMask |= ENCODE_ARM_REG_LR;
  }
}

ArmConditionCode ArmConditionEncoding(ConditionCode ccode)
{
  ArmConditionCode res;
  switch (ccode) {
    case kCondEq: res = kArmCondEq; break;
    case kCondNe: res = kArmCondNe; break;
    case kCondCs: res = kArmCondCs; break;
    case kCondCc: res = kArmCondCc; break;
    case kCondMi: res = kArmCondMi; break;
    case kCondPl: res = kArmCondPl; break;
    case kCondVs: res = kArmCondVs; break;
    case kCondVc: res = kArmCondVc; break;
    case kCondHi: res = kArmCondHi; break;
    case kCondLs: res = kArmCondLs; break;
    case kCondGe: res = kArmCondGe; break;
    case kCondLt: res = kArmCondLt; break;
    case kCondGt: res = kArmCondGt; break;
    case kCondLe: res = kArmCondLe; break;
    case kCondAl: res = kArmCondAl; break;
    case kCondNv: res = kArmCondNv; break;
    default:
      LOG(FATAL) << "Bad condition code " << ccode;
      res = static_cast<ArmConditionCode>(0);  // Quiet gcc
  }
  return res;
}

static const char* coreRegNames[16] = {
  "r0",
  "r1",
  "r2",
  "r3",
  "r4",
  "r5",
  "r6",
  "r7",
  "r8",
  "rSELF",
  "r10",
  "r11",
  "r12",
  "sp",
  "lr",
  "pc",
};


static const char* shiftNames[4] = {
  "lsl",
  "lsr",
  "asr",
  "ror"};

/* Decode and print a ARM register name */
char* DecodeRegList(int opcode, int vector, char* buf)
{
  int i;
  bool printed = false;
  buf[0] = 0;
  for (i = 0; i < 16; i++, vector >>= 1) {
    if (vector & 0x1) {
      int regId = i;
      if (opcode == kThumbPush && i == 8) {
        regId = r14lr;
      } else if (opcode == kThumbPop && i == 8) {
        regId = r15pc;
      }
      if (printed) {
        sprintf(buf + strlen(buf), ", r%d", regId);
      } else {
        printed = true;
        sprintf(buf, "r%d", regId);
      }
    }
  }
  return buf;
}

char*  DecodeFPCSRegList(int count, int base, char* buf)
{
  sprintf(buf, "s%d", base);
  for (int i = 1; i < count; i++) {
    sprintf(buf + strlen(buf), ", s%d",base + i);
  }
  return buf;
}

int ExpandImmediate(int value)
{
  int mode = (value & 0xf00) >> 8;
  uint32_t bits = value & 0xff;
  switch (mode) {
    case 0:
      return bits;
     case 1:
      return (bits << 16) | bits;
     case 2:
      return (bits << 24) | (bits << 8);
     case 3:
      return (bits << 24) | (bits << 16) | (bits << 8) | bits;
    default:
      break;
  }
  bits = (bits | 0x80) << 24;
  return bits >> (((value & 0xf80) >> 7) - 8);
}

const char* ccNames[] = {"eq","ne","cs","cc","mi","pl","vs","vc",
                         "hi","ls","ge","lt","gt","le","al","nv"};
/*
 * Interpret a format string and build a string no longer than size
 * See format key in Assemble.c.
 */
std::string BuildInsnString(const char* fmt, LIR* lir, unsigned char* baseAddr)
{
  std::string buf;
  int i;
  const char* fmtEnd = &fmt[strlen(fmt)];
  char tbuf[256];
  const char* name;
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
         DCHECK_LT(static_cast<unsigned>(nc-'0'), 4U);
         operand = lir->operands[nc-'0'];
         switch (*fmt++) {
           case 'H':
             if (operand != 0) {
               sprintf(tbuf, ", %s %d",shiftNames[operand & 0x3], operand >> 2);
             } else {
               strcpy(tbuf,"");
             }
             break;
           case 'B':
             switch (operand) {
               case kSY:
                 name = "sy";
                 break;
               case kST:
                 name = "st";
                 break;
               case kISH:
                 name = "ish";
                 break;
               case kISHST:
                 name = "ishst";
                 break;
               case kNSH:
                 name = "nsh";
                 break;
               case kNSHST:
                 name = "shst";
                 break;
               default:
                 name = "DecodeError2";
                 break;
             }
             strcpy(tbuf, name);
             break;
           case 'b':
             strcpy(tbuf,"0000");
             for (i=3; i>= 0; i--) {
               tbuf[i] += operand & 1;
               operand >>= 1;
             }
             break;
           case 'n':
             operand = ~ExpandImmediate(operand);
             sprintf(tbuf,"%d [%#x]", operand, operand);
             break;
           case 'm':
             operand = ExpandImmediate(operand);
             sprintf(tbuf,"%d [%#x]", operand, operand);
             break;
           case 's':
             sprintf(tbuf,"s%d",operand & ARM_FP_REG_MASK);
             break;
           case 'S':
             sprintf(tbuf,"d%d",(operand & ARM_FP_REG_MASK) >> 1);
             break;
           case 'h':
             sprintf(tbuf,"%04x", operand);
             break;
           case 'M':
           case 'd':
             sprintf(tbuf,"%d", operand);
             break;
           case 'C':
             DCHECK_LT(operand, static_cast<int>(
                 sizeof(coreRegNames)/sizeof(coreRegNames[0])));
             sprintf(tbuf,"%s",coreRegNames[operand]);
             break;
           case 'E':
             sprintf(tbuf,"%d", operand*4);
             break;
           case 'F':
             sprintf(tbuf,"%d", operand*2);
             break;
           case 'c':
             strcpy(tbuf, ccNames[operand]);
             break;
           case 't':
             sprintf(tbuf,"0x%08x (L%p)",
                 reinterpret_cast<uintptr_t>(baseAddr) + lir->offset + 4 +
                 (operand << 1),
                 lir->target);
             break;
           case 'u': {
             int offset_1 = lir->operands[0];
             int offset_2 = NEXT_LIR(lir)->operands[0];
             uintptr_t target =
                 (((reinterpret_cast<uintptr_t>(baseAddr) + lir->offset + 4) &
                 ~3) + (offset_1 << 21 >> 9) + (offset_2 << 1)) &
                 0xfffffffc;
             sprintf(tbuf, "%p", reinterpret_cast<void *>(target));
             break;
          }

           /* Nothing to print for BLX_2 */
           case 'v':
             strcpy(tbuf, "see above");
             break;
           case 'R':
             DecodeRegList(lir->opcode, operand, tbuf);
             break;
           case 'P':
             DecodeFPCSRegList(operand, 16, tbuf);
             break;
           case 'Q':
             DecodeFPCSRegList(operand, 0, tbuf);
             break;
           default:
             strcpy(tbuf,"DecodeError1");
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

void DumpResourceMask(LIR* armLIR, uint64_t mask, const char* prefix)
{
  char buf[256];
  buf[0] = 0;

  if (mask == ENCODE_ALL) {
    strcpy(buf, "all");
  } else {
    char num[8];
    int i;

    for (i = 0; i < kArmRegEnd; i++) {
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
    if (armLIR && (mask & ENCODE_DALVIK_REG)) {
      sprintf(buf + strlen(buf), "dr%d%s", armLIR->aliasInfo & 0xffff,
              (armLIR->aliasInfo & 0x80000000) ? "(+1)" : "");
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
    LOG(INFO) << prefix << ": " << buf;
  }
}

bool BranchUnconditional(LIR* lir)
{
  return ((lir->opcode == kThumbBUncond) || (lir->opcode == kThumb2BUncond));
}

/* Common initialization routine for an architecture family */
bool ArchInit()
{
  int i;

  for (i = 0; i < kArmLast; i++) {
    if (EncodingMap[i].opcode != i) {
      LOG(FATAL) << "Encoding order for " << EncodingMap[i].name
                 << " is wrong: expecting " << i << ", seeing "
                 << static_cast<int>(EncodingMap[i].opcode);
    }
  }

  return ArchVariantInit();
}

/* Architecture-specific initializations and checks go here */
bool ArchVariantInit(void)
{
  return true;
}

/*
 * Alloc a pair of core registers, or a double.  Low reg in low byte,
 * high reg in next byte.
 */
int AllocTypedTempPair(CompilationUnit* cUnit, bool fpHint, int regClass)
{
  int highReg;
  int lowReg;
  int res = 0;

  if (((regClass == kAnyReg) && fpHint) || (regClass == kFPReg)) {
    lowReg = AllocTempDouble(cUnit);
    highReg = lowReg + 1;
  } else {
    lowReg = AllocTemp(cUnit);
    highReg = AllocTemp(cUnit);
  }
  res = (lowReg & 0xff) | ((highReg & 0xff) << 8);
  return res;
}

int AllocTypedTemp(CompilationUnit* cUnit, bool fpHint, int regClass)
{
  if (((regClass == kAnyReg) && fpHint) || (regClass == kFPReg))
    return AllocTempFloat(cUnit);
  return AllocTemp(cUnit);
}

void CompilerInitializeRegAlloc(CompilationUnit* cUnit)
{
  int numRegs = sizeof(coreRegs)/sizeof(*coreRegs);
  int numReserved = sizeof(ReservedRegs)/sizeof(*ReservedRegs);
  int numTemps = sizeof(coreTemps)/sizeof(*coreTemps);
  int numFPRegs = sizeof(FpRegs)/sizeof(*FpRegs);
  int numFPTemps = sizeof(fpTemps)/sizeof(*fpTemps);
  RegisterPool *pool =
      static_cast<RegisterPool*>(NewMem(cUnit, sizeof(*pool), true, kAllocRegAlloc));
  cUnit->regPool = pool;
  pool->numCoreRegs = numRegs;
  pool->coreRegs = reinterpret_cast<RegisterInfo*>
      (NewMem(cUnit, numRegs * sizeof(*cUnit->regPool->coreRegs), true, kAllocRegAlloc));
  pool->numFPRegs = numFPRegs;
  pool->FPRegs = static_cast<RegisterInfo*>
      (NewMem(cUnit, numFPRegs * sizeof(*cUnit->regPool->FPRegs), true, kAllocRegAlloc));
  CompilerInitPool(pool->coreRegs, coreRegs, pool->numCoreRegs);
  CompilerInitPool(pool->FPRegs, FpRegs, pool->numFPRegs);
  // Keep special registers from being allocated
  for (int i = 0; i < numReserved; i++) {
    if (NO_SUSPEND && (ReservedRegs[i] == rARM_SUSPEND)) {
      //To measure cost of suspend check
      continue;
    }
    MarkInUse(cUnit, ReservedRegs[i]);
  }
  // Mark temp regs - all others not in use can be used for promotion
  for (int i = 0; i < numTemps; i++) {
    MarkTemp(cUnit, coreTemps[i]);
  }
  for (int i = 0; i < numFPTemps; i++) {
    MarkTemp(cUnit, fpTemps[i]);
  }

  // Start allocation at r2 in an attempt to avoid clobbering return values
  pool->nextCoreReg = r2;

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
/*
 * TUNING: is leaf?  Can't just use "hasInvoke" to determine as some
 * instructions might call out to C/assembly helper functions.  Until
 * machinery is in place, always spill lr.
 */

void AdjustSpillMask(CompilationUnit* cUnit)
{
  cUnit->coreSpillMask |= (1 << rARM_LR);
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
  DCHECK_GE(reg, ARM_FP_REG_MASK + ARM_FP_CALLEE_SAVE_BASE);
  reg = (reg & ARM_FP_REG_MASK) - ARM_FP_CALLEE_SAVE_BASE;
  // Ensure fpVmapTable is large enough
  int tableSize = cUnit->fpVmapTable.size();
  for (int i = tableSize; i < (reg + 1); i++) {
    cUnit->fpVmapTable.push_back(INVALID_VREG);
  }
  // Add the current mapping
  cUnit->fpVmapTable[reg] = vReg;
  // Size of fpVmapTable is high-water mark, use to set mask
  cUnit->numFPSpills = cUnit->fpVmapTable.size();
  cUnit->fpSpillMask = ((1 << cUnit->numFPSpills) - 1) << ARM_FP_CALLEE_SAVE_BASE;
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
    if (SRegToVReg(cUnit, info2->sReg) <
      SRegToVReg(cUnit, info1->sReg))
      info1 = info2;
    int vReg = SRegToVReg(cUnit, info1->sReg);
    StoreBaseDispWide(cUnit, rARM_SP, VRegOffset(cUnit, vReg), info1->reg, info1->partner);
  }
}

void FlushReg(CompilationUnit* cUnit, int reg)
{
  RegisterInfo* info = GetRegInfo(cUnit, reg);
  if (info->live && info->dirty) {
    info->dirty = false;
    int vReg = SRegToVReg(cUnit, info->sReg);
    StoreBaseDisp(cUnit, rARM_SP, VRegOffset(cUnit, vReg), reg, kWord);
  }
}

/* Give access to the target-dependent FP register encoding to common code */
bool IsFpReg(int reg) {
  return ARM_FPREG(reg);
}

/* Clobber all regs that might be used by an external C call */
void ClobberCalleeSave(CompilationUnit *cUnit)
{
  Clobber(cUnit, r0);
  Clobber(cUnit, r1);
  Clobber(cUnit, r2);
  Clobber(cUnit, r3);
  Clobber(cUnit, r12);
  Clobber(cUnit, r14lr);
  Clobber(cUnit, fr0);
  Clobber(cUnit, fr1);
  Clobber(cUnit, fr2);
  Clobber(cUnit, fr3);
  Clobber(cUnit, fr4);
  Clobber(cUnit, fr5);
  Clobber(cUnit, fr6);
  Clobber(cUnit, fr7);
  Clobber(cUnit, fr8);
  Clobber(cUnit, fr9);
  Clobber(cUnit, fr10);
  Clobber(cUnit, fr11);
  Clobber(cUnit, fr12);
  Clobber(cUnit, fr13);
  Clobber(cUnit, fr14);
  Clobber(cUnit, fr15);
}

extern RegLocation GetReturnWideAlt(CompilationUnit* cUnit)
{
  RegLocation res = LocCReturnWide();
  res.lowReg = r2;
  res.highReg = r3;
  Clobber(cUnit, r2);
  Clobber(cUnit, r3);
  MarkInUse(cUnit, r2);
  MarkInUse(cUnit, r3);
  MarkPair(cUnit, res.lowReg, res.highReg);
  return res;
}

extern RegLocation GetReturnAlt(CompilationUnit* cUnit)
{
  RegLocation res = LocCReturn();
  res.lowReg = r1;
  Clobber(cUnit, r1);
  MarkInUse(cUnit, r1);
  return res;
}

extern RegisterInfo* GetRegInfo(CompilationUnit* cUnit, int reg)
{
  return ARM_FPREG(reg) ? &cUnit->regPool->FPRegs[reg & ARM_FP_REG_MASK]
      : &cUnit->regPool->coreRegs[reg];
}

/* To be used when explicitly managing register use */
extern void LockCallTemps(CompilationUnit* cUnit)
{
  LockTemp(cUnit, r0);
  LockTemp(cUnit, r1);
  LockTemp(cUnit, r2);
  LockTemp(cUnit, r3);
}

/* To be used when explicitly managing register use */
extern void FreeCallTemps(CompilationUnit* cUnit)
{
  FreeTemp(cUnit, r0);
  FreeTemp(cUnit, r1);
  FreeTemp(cUnit, r2);
  FreeTemp(cUnit, r3);
}

int LoadHelper(CompilationUnit* cUnit, int offset)
{
  LoadWordDisp(cUnit, rARM_SELF, offset, rARM_LR);
  return rARM_LR;
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

}  // namespace art
