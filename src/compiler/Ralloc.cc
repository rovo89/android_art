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

static bool setFp(CompilationUnit* cUnit, int index, bool isFP) {
    bool change = false;
    if (isFP && !cUnit->regLocation[index].fp) {
        cUnit->regLocation[index].fp = true;
        change = true;
    }
    return change;
}

/*
 * Infer types and sizes.  We don't need to track change on sizes,
 * as it doesn't propagate.  We're guaranteed at least one pass through
 * the cfg.
 */
static bool inferTypeAndSize(CompilationUnit* cUnit, BasicBlock* bb)
{
    MIR *mir;
    bool changed = false;   // Did anything change?

    if (bb->dataFlowInfo == NULL) return false;
    if (bb->blockType != kDalvikByteCode && bb->blockType != kEntryBlock)
        return false;

    for (mir = bb->firstMIRInsn; mir; mir = mir->next) {
        SSARepresentation *ssaRep = mir->ssaRep;
        if (ssaRep) {
            int attrs = oatDataFlowAttributes[mir->dalvikInsn.opcode];
            int next = 0;
            if (attrs & DF_DA_WIDE) {
                cUnit->regLocation[ssaRep->defs[0]].wide = true;
            }
            if (attrs & DF_UA_WIDE) {
                cUnit->regLocation[ssaRep->uses[next]].wide = true;
                next += 2;
            }
            if (attrs & DF_UB_WIDE) {
                cUnit->regLocation[ssaRep->uses[next]].wide = true;
                next += 2;
            }
            if (attrs & DF_UC_WIDE) {
                cUnit->regLocation[ssaRep->uses[next]].wide = true;
            }
            for (int i=0; ssaRep->fpUse && i< ssaRep->numUses; i++) {
                if (ssaRep->fpUse[i])
                    changed |= setFp(cUnit, ssaRep->uses[i], true);
            }
            for (int i=0; ssaRep->fpDef && i< ssaRep->numDefs; i++) {
                if (ssaRep->fpDef[i])
                    changed |= setFp(cUnit, ssaRep->defs[i], true);
            }
            // Special-case handling for moves & Phi
            if (attrs & (DF_IS_MOVE | DF_NULL_TRANSFER_N)) {
                bool isFP = cUnit->regLocation[ssaRep->defs[0]].fp;
                for (int i = 0; i < ssaRep->numUses; i++) {
                    isFP |= cUnit->regLocation[ssaRep->uses[i]].fp;
                }
                changed |= setFp(cUnit, ssaRep->defs[0], isFP);
                for (int i = 0; i < ssaRep->numUses; i++) {
                    changed |= setFp(cUnit, ssaRep->uses[i], isFP);
                }
            }
        }
    }
    return changed;
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

    /* Add types of incoming arguments based on signature */
    int numRegs = cUnit->method->NumRegisters();
    int numIns = cUnit->method->NumIns();
    if (numIns > 0) {
        int sReg = numRegs - numIns;
        if (!cUnit->method->IsStatic()) {
            // Skip past "this"
            sReg++;
        }
        const String* shorty = cUnit->method->GetShorty();
        for (int i = 1; i < shorty->GetLength(); i++) {
            char arg = shorty->CharAt(i);
            // Is it wide?
            if ((arg == 'D') || (arg == 'J')) {
                cUnit->regLocation[sReg].wide = true;
                cUnit->regLocation[sReg+1].fp = cUnit->regLocation[sReg].fp;
                sReg++;  // Skip to next
            }
            sReg++;
        }
    }

    /* Do type & size inference pass */
    oatDataFlowAnalysisDispatcher(cUnit, inferTypeAndSize,
                                  kPreOrderDFSTraversal,
                                  true /* isIterative */);

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
