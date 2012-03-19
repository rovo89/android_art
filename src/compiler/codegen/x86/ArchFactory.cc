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

/*
 * This file contains x86-specific codegen factory support.
 * It is included by
 *
 *        Codegen-$(TARGET_ARCH_VARIANT).c
 *
 */

namespace art {

bool genAddLong(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
    rlSrc1 = loadValueWide(cUnit, rlSrc1, kCoreReg);
    rlSrc2 = loadValueWide(cUnit, rlSrc2, kCoreReg);
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    /*
     *  [v1 v0] =  [a1 a0] + [a3 a2];
     *    add v0,a2,a0
     *    adc v1,a3,a1
     */

    opRegRegReg(cUnit, kOpAdd, rlResult.lowReg, rlSrc2.lowReg, rlSrc1.lowReg);
    opRegRegReg(cUnit, kOpAdc, rlResult.highReg, rlSrc2.highReg, rlSrc1.highReg);
    storeValueWide(cUnit, rlDest, rlResult);
    return false;
}

bool genSubLong(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
    rlSrc1 = loadValueWide(cUnit, rlSrc1, kCoreReg);
    rlSrc2 = loadValueWide(cUnit, rlSrc2, kCoreReg);
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    /*
     *  [v1 v0] =  [a1 a0] - [a3 a2];
     *    sub    v0,a0,a2
     *    sbb    v1,a1,a3
     */

    opRegRegReg(cUnit, kOpSub, rlResult.lowReg, rlSrc1.lowReg, rlSrc2.lowReg);
    opRegRegReg(cUnit, kOpSbc, rlResult.highReg, rlSrc1.highReg, rlSrc2.highReg);
    storeValueWide(cUnit, rlDest, rlResult);
    return false;
}

bool genNegLong(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                RegLocation rlSrc)
{
    UNIMPLEMENTED(WARNING) << "genNegLong";
#if 0
    rlSrc = loadValueWide(cUnit, rlSrc, kCoreReg);
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    /*
     *  [v1 v0] =  -[a1 a0]
     *    negu    v0,a0
     *    negu    v1,a1
     *    sltu    t1,r_zero
     *    subu    v1,v1,t1
     */

    opRegReg(cUnit, kOpNeg, rlResult.lowReg, rlSrc.lowReg);
    opRegReg(cUnit, kOpNeg, rlResult.highReg, rlSrc.highReg);
    int tReg = oatAllocTemp(cUnit);
    newLIR3(cUnit, kX86Sltu, tReg, r_ZERO, rlResult.lowReg);
    opRegRegReg(cUnit, kOpSub, rlResult.highReg, rlResult.highReg, tReg);
    oatFreeTemp(cUnit, tReg);
    storeValueWide(cUnit, rlDest, rlResult);
#endif
    return false;
}

void genDebuggerUpdate(CompilationUnit* cUnit, int32_t offset);

void spillCoreRegs(CompilationUnit* cUnit) {
  if (cUnit->numCoreSpills == 0) {
    return;
  }
  // Spill mask not including fake return address register
  uint32_t mask = cUnit->coreSpillMask & ~(1 << rRET);
  int offset = cUnit->frameSize - 4;
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      offset -= 4;
      storeWordDisp(cUnit, rSP, offset, reg);
    }
  }
}

void unSpillCoreRegs(CompilationUnit* cUnit) {
  if (cUnit->numCoreSpills == 0) {
    return;
  }
  // Spill mask not including fake return address register
  uint32_t mask = cUnit->coreSpillMask & ~(1 << rRET);
  int offset = cUnit->frameSize - 4;
  for (int reg = 0; mask; mask >>= 1, reg++) {
    if (mask & 0x1) {
      offset -= 4;
      loadWordDisp(cUnit, rSP, offset, reg);
    }
  }
}

void opRegThreadMem(CompilationUnit* cUnit, OpKind op, int rDest, int threadOffset) {
  X86OpCode opcode = kX86Bkpt;
  switch (op) {
    case kOpCmp: opcode = kX86Cmp32RT;  break;
    default:
      LOG(FATAL) << "Bad opcode: " << op;
      break;
  }
  newLIR2(cUnit, opcode, rDest, threadOffset);
}

void genEntrySequence(CompilationUnit* cUnit, BasicBlock* bb)
{
    /*
     * On entry, rARG0, rARG1, rARG2 are live.  Let the register
     * allocation mechanism know so it doesn't try to use any of them when
     * expanding the frame or flushing.  This leaves the utility
     * code with no spare temps.
     */
    oatLockTemp(cUnit, rARG0);
    oatLockTemp(cUnit, rARG1);
    oatLockTemp(cUnit, rARG2);

    /* Build frame, return address already on stack */
    opRegImm(cUnit, kOpSub, rSP, cUnit->frameSize - 4);

    /*
     * We can safely skip the stack overflow check if we're
     * a leaf *and* our frame size < fudge factor.
     */
    bool skipOverflowCheck = ((cUnit->attrs & METHOD_IS_LEAF) &&
                              ((size_t)cUnit->frameSize <
                              Thread::kStackOverflowReservedBytes));
    newLIR0(cUnit, kPseudoMethodEntry);
    /* Spill core callee saves */
    spillCoreRegs(cUnit);
    /* NOTE: promotion of FP regs currently unsupported, thus no FP spill */
    DCHECK_EQ(cUnit->numFPSpills, 0);
    if (!skipOverflowCheck) {
        // cmp rSP, fs:[stack_end_]; jcc throw_launchpad
        LIR* tgt = rawLIR(cUnit, 0, kPseudoThrowTarget, kThrowStackOverflow, 0, 0, 0, 0);
        opRegThreadMem(cUnit, kOpCmp, rSP, Thread::StackEndOffset().Int32Value());
        opCondBranch(cUnit, kCondUlt, tgt);
        // Remember branch target - will process later
        oatInsertGrowableList(cUnit, &cUnit->throwLaunchpads, (intptr_t)tgt);
    }

    flushIns(cUnit);

    if (cUnit->genDebugger) {
        // Refresh update debugger callout
        UNIMPLEMENTED(WARNING) << "genDebugger";
#if 0
        loadWordDisp(cUnit, rSELF,
                     OFFSETOF_MEMBER(Thread, pUpdateDebuggerFromCode), rSUSPEND);
        genDebuggerUpdate(cUnit, DEBUGGER_METHOD_ENTRY);
#endif
    }

    oatFreeTemp(cUnit, rARG0);
    oatFreeTemp(cUnit, rARG1);
    oatFreeTemp(cUnit, rARG2);
}

void genExitSequence(CompilationUnit* cUnit, BasicBlock* bb) {
  /*
   * In the exit path, rRET0/rRET1 are live - make sure they aren't
   * allocated by the register utilities as temps.
   */
  oatLockTemp(cUnit, rRET0);
  oatLockTemp(cUnit, rRET1);

  newLIR0(cUnit, kPseudoMethodExit);
  /* If we're compiling for the debugger, generate an update callout */
  if (cUnit->genDebugger) {
    genDebuggerUpdate(cUnit, DEBUGGER_METHOD_EXIT);
  }
  unSpillCoreRegs(cUnit);
  /* Remove frame except for return address */
  opRegImm(cUnit, kOpAdd, rSP, cUnit->frameSize - 4);
  newLIR0(cUnit, kX86Ret);
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
    if (thisLIR->opcode == kX86Jmp) {
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
          << " is wrong: expecting " << i << ", seeing " << (int)EncodingMap[i].opcode;
    }
  }

  return oatArchVariantInit();
}

}  // namespace art
