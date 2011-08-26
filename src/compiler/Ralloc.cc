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

#include "Dalvik.h"
#include "CompilerInternals.h"
#include "Dataflow.h"
#include "codegen/Ralloc.h"

/*
 * Quick & dirty - make FP usage sticky.  This is strictly a hint - local
 * code generation will handle misses.  It might be worthwhile to collaborate
 * with dx/dexopt to avoid reusing the same Dalvik temp for values of
 * different types.
 */
static void inferTypes(CompilationUnit* cUnit, BasicBlock* bb)
{
    MIR *mir;
    if (bb->blockType != kDalvikByteCode && bb->blockType != kEntryBlock)
        return;

    for (mir = bb->firstMIRInsn; mir; mir = mir->next) {
        SSARepresentation *ssaRep = mir->ssaRep;
        if (ssaRep) {
            int i;
            for (i=0; ssaRep->fpUse && i< ssaRep->numUses; i++) {
                if (ssaRep->fpUse[i])
                    cUnit->regLocation[ssaRep->uses[i]].fp = true;
            }
            for (i=0; ssaRep->fpDef && i< ssaRep->numDefs; i++) {
                if (ssaRep->fpDef[i])
                    cUnit->regLocation[ssaRep->defs[i]].fp = true;
            }
        }
    }
}

static const char* storageName[] = {" Frame ", "PhysReg", " Spill "};

void oatDumpRegLocTable(RegLocation* table, int count)
{
    for (int i = 0; i < count; i++) {
        char buf[100];
        snprintf(buf, 100, "Loc[%02d] : %s, %c %c r%d r%d S%d : %s s%d s%d",
             i, storageName[table[i].location], table[i].wide ? 'W' : 'N',
             table[i].fp ? 'F' : 'C', table[i].lowReg, table[i].highReg,
             table[i].sRegLow, storageName[table[i].fpLocation],
             table[i].fpLowReg & FP_REG_MASK, table[i].fpHighReg &
             FP_REG_MASK);
        LOG(INFO) << buf;
    }
}

static const RegLocation freshLoc = {kLocDalvikFrame, 0, 0, INVALID_REG,
                                     INVALID_REG, INVALID_SREG, 0,
                                     kLocDalvikFrame, INVALID_REG, INVALID_REG,
                                     INVALID_OFFSET};

/*
 * Simple register allocation.  Some Dalvik virtual registers may
 * be promoted to physical registers.  Most of the work for temp
 * allocation is done on the fly.  We also do some initilization and
 * type inference here.
 */
void oatSimpleRegAlloc(CompilationUnit* cUnit)
{
    int i;
    RegLocation* loc;

    /* Allocate the location map */
    loc = (RegLocation*)oatNew(cUnit->numSSARegs * sizeof(*loc), true);
    for (i=0; i< cUnit->numSSARegs; i++) {
        loc[i] = freshLoc;
        loc[i].sRegLow = i;
    }
    cUnit->regLocation = loc;

    GrowableListIterator iterator;

    oatGrowableListIteratorInit(&cUnit->blockList, &iterator);
    /* Do type inference pass */
    while (true) {
        BasicBlock *bb = (BasicBlock *) oatGrowableListIteratorNext(&iterator);
        if (bb == NULL) break;
        inferTypes(cUnit, bb);
    }

    /*
     * Set the sRegLow field to refer to the pre-SSA name of the
     * base Dalvik virtual register.  Once we add a better register
     * allocator, remove this remapping.
     */
    for (i=0; i < cUnit->numSSARegs; i++) {
        cUnit->regLocation[i].sRegLow =
                DECODE_REG(oatConvertSSARegToDalvik(cUnit, loc[i].sRegLow));
    }

    cUnit->coreSpillMask = 0;
    cUnit->fpSpillMask = 0;
    cUnit->numSpills = 0;

    oatDoPromotion(cUnit);

    if (cUnit->printMe && !(cUnit->disableOpt & (1 << kPromoteRegs))) {
        LOG(INFO) << "After Promotion";
        oatDumpRegLocTable(cUnit->regLocation, cUnit->numSSARegs);
    }

    /* Figure out the frame size */
    cUnit->numIns = cUnit->method->NumIns();
    cUnit->numRegs = cUnit->method->NumRegisters() - cUnit->numIns;
    cUnit->numOuts = cUnit->method->NumOuts();
    cUnit->numPadding = (STACK_ALIGN_WORDS -
        (cUnit->numSpills + cUnit->numRegs +
         cUnit->numOuts + 2)) & (STACK_ALIGN_WORDS-1);
    cUnit->frameSize = (cUnit->numSpills + cUnit->numRegs + cUnit->numOuts +
                        cUnit->numPadding + 2) * 4;
    cUnit->insOffset = cUnit->frameSize + 4;
    cUnit->regsOffset = (cUnit->numOuts + cUnit->numPadding + 1) * 4;

    /* Compute sp-relative home location offsets */
    for (i = 0; i < cUnit->numSSARegs; i++) {
        int vReg = oatS2VReg(cUnit, cUnit->regLocation[i].sRegLow);
        cUnit->regLocation[i].spOffset = oatVRegOffset(cUnit, vReg);
    }
}
