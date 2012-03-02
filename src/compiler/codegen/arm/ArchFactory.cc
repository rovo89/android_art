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
 * This file contains arm-specific codegen factory support.
 * It is included by
 *
 *        Codegen-$(TARGET_ARCH_VARIANT).c
 *
 */

namespace art {

void genDebuggerUpdate(CompilationUnit* cUnit, int32_t offset);

int loadHelper(CompilationUnit* cUnit, int offset)
{
    loadWordDisp(cUnit, rSELF, offset, rLR);
    return rLR;
}

void genEntrySequence(CompilationUnit* cUnit, BasicBlock* bb)
{
    int spillCount = cUnit->numCoreSpills + cUnit->numFPSpills;
    /*
     * On entry, r0, r1, r2 & r3 are live.  Let the register allocation
     * mechanism know so it doesn't try to use any of them when
     * expanding the frame or flushing.  This leaves the utility
     * code with a single temp: r12.  This should be enough.
     */
    oatLockTemp(cUnit, r0);
    oatLockTemp(cUnit, r1);
    oatLockTemp(cUnit, r2);
    oatLockTemp(cUnit, r3);

    /*
     * We can safely skip the stack overflow check if we're
     * a leaf *and* our frame size < fudge factor.
     */
    bool skipOverflowCheck = ((cUnit->attrs & METHOD_IS_LEAF) &&
                              ((size_t)cUnit->frameSize <
                              Thread::kStackOverflowReservedBytes));
    newLIR0(cUnit, kPseudoMethodEntry);
    if (!skipOverflowCheck) {
        /* Load stack limit */
        loadWordDisp(cUnit, rSELF,
                     Thread::StackEndOffset().Int32Value(), r12);
    }
    /* Spill core callee saves */
    newLIR1(cUnit, kThumb2Push, cUnit->coreSpillMask);
    /* Need to spill any FP regs? */
    if (cUnit->numFPSpills) {
        /*
         * NOTE: fp spills are a little different from core spills in that
         * they are pushed as a contiguous block.  When promoting from
         * the fp set, we must allocate all singles from s16..highest-promoted
         */
        newLIR1(cUnit, kThumb2VPushCS, cUnit->numFPSpills);
    }
    if (!skipOverflowCheck) {
        opRegRegImm(cUnit, kOpSub, rLR, rSP,
                    cUnit->frameSize - (spillCount * 4));
        genRegRegCheck(cUnit, kCondCc, rLR, r12, NULL,
                       kThrowStackOverflow);
        opRegCopy(cUnit, rSP, rLR);         // Establish stack
    } else {
        opRegImm(cUnit, kOpSub, rSP,
                 cUnit->frameSize - (spillCount * 4));
    }
    storeBaseDisp(cUnit, rSP, 0, r0, kWord);
    flushIns(cUnit);

    if (cUnit->genDebugger) {
        // Refresh update debugger callout
        loadWordDisp(cUnit, rSELF,
                     OFFSETOF_MEMBER(Thread, pUpdateDebuggerFromCode), rSUSPEND);
        genDebuggerUpdate(cUnit, DEBUGGER_METHOD_ENTRY);
    }

    oatFreeTemp(cUnit, r0);
    oatFreeTemp(cUnit, r1);
    oatFreeTemp(cUnit, r2);
    oatFreeTemp(cUnit, r3);
}

void genExitSequence(CompilationUnit* cUnit, BasicBlock* bb)
{
    int spillCount = cUnit->numCoreSpills + cUnit->numFPSpills;
    /*
     * In the exit path, r0/r1 are live - make sure they aren't
     * allocated by the register utilities as temps.
     */
    oatLockTemp(cUnit, r0);
    oatLockTemp(cUnit, r1);

    newLIR0(cUnit, kPseudoMethodExit);
    /* If we're compiling for the debugger, generate an update callout */
    if (cUnit->genDebugger) {
        genDebuggerUpdate(cUnit, DEBUGGER_METHOD_EXIT);
    }
    opRegImm(cUnit, kOpAdd, rSP, cUnit->frameSize - (spillCount * 4));
    /* Need to restore any FP callee saves? */
    if (cUnit->numFPSpills) {
        newLIR1(cUnit, kThumb2VPopCS, cUnit->numFPSpills);
    }
    if (cUnit->coreSpillMask & (1 << rLR)) {
        /* Unspill rLR to rPC */
        cUnit->coreSpillMask &= ~(1 << rLR);
        cUnit->coreSpillMask |= (1 << rPC);
    }
    newLIR1(cUnit, kThumb2Pop, cUnit->coreSpillMask);
    if (!(cUnit->coreSpillMask & (1 << rPC))) {
        /* We didn't pop to rPC, so must do a bv rLR */
        newLIR1(cUnit, kThumbBx, rLR);
    }
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
        if ((thisLIR->opcode == kThumbBUncond) ||
            (thisLIR->opcode == kThumb2BUncond)) {
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

    for (i = 0; i < kArmLast; i++) {
        if (EncodingMap[i].opcode != i) {
            LOG(FATAL) << "Encoding order for " << EncodingMap[i].name <<
               " is wrong: expecting " << i << ", seeing " <<
               (int)EncodingMap[i].opcode;
        }
    }

    return oatArchVariantInit();
}
}  // namespace art
