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

namespace art {

/*
 * This source files contains "gen" codegen routines that should
 * be applicable to most targets.  Only mid-level support utilities
 * and "op" calls may be used here.
 */

typedef int (*NextCallInsn)(CompilationUnit*, MIR*, int, uint32_t dexIdx,
                            uint32_t methodIdx, uintptr_t directCode,
                            uintptr_t directMethod, InvokeType type);
LIR* opCondBranch(CompilationUnit* cUnit, ConditionCode cc, LIR* target);

/*
 * If there are any ins passed in registers that have not been promoted
 * to a callee-save register, flush them to the frame.  Perform intial
 * assignment of promoted arguments.
 */
void flushIns(CompilationUnit* cUnit)
{
    /*
     * Dummy up a RegLocation for the incoming Method*
     * It will attempt to keep rARG0 live (or copy it to home location
     * if promoted).
     */
    RegLocation rlSrc = cUnit->regLocation[cUnit->methodSReg];
    RegLocation rlMethod = cUnit->regLocation[cUnit->methodSReg];
    rlSrc.location = kLocPhysReg;
    rlSrc.lowReg = rARG0;
    rlSrc.home = false;
    oatMarkLive(cUnit, rlSrc.lowReg, rlSrc.sRegLow);
    storeValue(cUnit, rlMethod, rlSrc);
    // If Method* has been promoted, explicitly flush
    if (rlMethod.location == kLocPhysReg) {
        storeWordDisp(cUnit, rSP, 0, rARG0);
    }

    if (cUnit->numIns == 0)
        return;
    const int numArgRegs = 3;
    static int argRegs[] = {rARG1, rARG2, rARG3};
    int startVReg = cUnit->numDalvikRegisters - cUnit->numIns;
    /*
     * Copy incoming arguments to their proper home locations.
     * NOTE: an older version of dx had an issue in which
     * it would reuse static method argument registers.
     * This could result in the same Dalvik virtual register
     * being promoted to both core and fp regs. To account for this,
     * we only copy to the corresponding promoted physical register
     * if it matches the type of the SSA name for the incoming
     * argument.  It is also possible that long and double arguments
     * end up half-promoted.  In those cases, we must flush the promoted
     * half to memory as well.
     */
    for (int i = 0; i < cUnit->numIns; i++) {
        PromotionMap* vMap = &cUnit->promotionMap[startVReg + i];
        if (i < numArgRegs) {
            // If arriving in register
            bool needFlush = true;
            RegLocation* tLoc = &cUnit->regLocation[startVReg + i];
            if ((vMap->coreLocation == kLocPhysReg) && !tLoc->fp) {
                opRegCopy(cUnit, vMap->coreReg, argRegs[i]);
                needFlush = false;
            } else if ((vMap->fpLocation == kLocPhysReg) && tLoc->fp) {
                opRegCopy(cUnit, vMap->fpReg, argRegs[i]);
                needFlush = false;
            } else {
                needFlush = true;
            }

            // For wide args, force flush if only half is promoted
            if (tLoc->wide) {
                PromotionMap* pMap = vMap + (tLoc->highWord ? -1 : +1);
                needFlush |= (pMap->coreLocation != vMap->coreLocation) ||
                             (pMap->fpLocation != vMap->fpLocation);
            }
            if (needFlush) {
                storeBaseDisp(cUnit, rSP, oatSRegOffset(cUnit, startVReg + i),
                              argRegs[i], kWord);
            }
        } else {
            // If arriving in frame & promoted
            if (vMap->coreLocation == kLocPhysReg) {
                loadWordDisp(cUnit, rSP, oatSRegOffset(cUnit, startVReg + i),
                             vMap->coreReg);
            }
            if (vMap->fpLocation == kLocPhysReg) {
                loadWordDisp(cUnit, rSP, oatSRegOffset(cUnit, startVReg + i),
                             vMap->fpReg);
            }
        }
    }
}

void scanMethodLiteralPool(CompilationUnit* cUnit, LIR** methodTarget, LIR** codeTarget, const DexFile* dexFile, uint32_t dexMethodIdx)
{
    LIR* curTarget = cUnit->methodLiteralList;
    LIR* nextTarget = curTarget != NULL ? curTarget->next : NULL;
    while (curTarget != NULL && nextTarget != NULL) {
      if (curTarget->operands[0] == (int)dexFile &&
          nextTarget->operands[0] == (int)dexMethodIdx) {
        *codeTarget = curTarget;
        *methodTarget = nextTarget;
        DCHECK((*codeTarget)->next == *methodTarget);
        DCHECK_EQ((*codeTarget)->operands[0], (int)dexFile);
        DCHECK_EQ((*methodTarget)->operands[0], (int)dexMethodIdx);
        break;
      }
      curTarget = nextTarget->next;
      nextTarget = curTarget != NULL ? curTarget->next : NULL;
    }
}

/*
 * Bit of a hack here - in the absence of a real scheduling pass,
 * emit the next instruction in static & direct invoke sequences.
 */
int nextSDCallInsn(CompilationUnit* cUnit, MIR* mir,
                   int state, uint32_t dexIdx, uint32_t unused,
                   uintptr_t directCode, uintptr_t directMethod,
                   InvokeType type)
{
#if !defined(TARGET_ARM)
    directCode = 0;
    directMethod = 0;
#endif
    if (directCode != 0 && directMethod != 0) {
        switch (state) {
        case 0:  // Get the current Method* [sets rARG0]
            if (directCode != (uintptr_t)-1) {
                loadConstant(cUnit, rINVOKE_TGT, directCode);
            } else {
                LIR* dataTarget = scanLiteralPool(cUnit->codeLiteralList, dexIdx, 0);
                if (dataTarget == NULL) {
                    dataTarget = addWordData(cUnit, &cUnit->codeLiteralList, dexIdx);
                    dataTarget->operands[1] = type;
                }
#if defined(TARGET_ARM)
                LIR* loadPcRel = rawLIR(cUnit, cUnit->currentDalvikOffset,
                                        kThumb2LdrPcRel12, rINVOKE_TGT, 0, 0, 0, 0, dataTarget);
                oatAppendLIR(cUnit, loadPcRel);
#else
                UNIMPLEMENTED(FATAL) << (void*)dataTarget;
#endif
            }
            if (directMethod != (uintptr_t)-1) {
                loadConstant(cUnit, rARG0, directMethod);
            } else {
                LIR* dataTarget = scanLiteralPool(cUnit->methodLiteralList, dexIdx, 0);
                if (dataTarget == NULL) {
                    dataTarget = addWordData(cUnit, &cUnit->methodLiteralList, dexIdx);
                    dataTarget->operands[1] = type;
                }
#if defined(TARGET_ARM)
                LIR* loadPcRel = rawLIR(cUnit, cUnit->currentDalvikOffset,
                                        kThumb2LdrPcRel12, rARG0, 0, 0, 0, 0, dataTarget);
                oatAppendLIR(cUnit, loadPcRel);
#else
                UNIMPLEMENTED(FATAL) << (void*)dataTarget;
#endif
            }
            break;
        default:
            return -1;
      }
    } else {
        switch (state) {
        case 0:  // Get the current Method* [sets rARG0]
            // TUNING: we can save a reg copy if Method* has been promoted
            loadCurrMethodDirect(cUnit, rARG0);
            break;
        case 1:  // Get method->dex_cache_resolved_methods_
            loadWordDisp(cUnit, rARG0,
                Method::DexCacheResolvedMethodsOffset().Int32Value(),
                rARG0);
            // Set up direct code if known.
            if (directCode != 0) {
                if (directCode != (uintptr_t)-1) {
                    loadConstant(cUnit, rINVOKE_TGT, directCode);
                } else {
                    LIR* dataTarget = scanLiteralPool(cUnit->codeLiteralList, dexIdx, 0);
                    if (dataTarget == NULL) {
                        dataTarget = addWordData(cUnit, &cUnit->codeLiteralList, dexIdx);
                        dataTarget->operands[1] = type;
                    }
#if defined(TARGET_ARM)
                    LIR* loadPcRel = rawLIR(cUnit, cUnit->currentDalvikOffset,
                                            kThumb2LdrPcRel12, rINVOKE_TGT, 0, 0, 0, 0, dataTarget);
                    oatAppendLIR(cUnit, loadPcRel);
#else
                    UNIMPLEMENTED(FATAL) << (void*)dataTarget;
#endif
                }
            }
            break;
        case 2:  // Grab target method*
            loadWordDisp(cUnit, rARG0,
                Array::DataOffset(sizeof(Object*)).Int32Value() + dexIdx * 4,
                rARG0);
            break;
#if !defined(TARGET_X86)
        case 3:  // Grab the code from the method*
            if (directCode == 0) {
                loadWordDisp(cUnit, rARG0, Method::GetCodeOffset().Int32Value(),
                             rINVOKE_TGT);
            }
            break;
#endif
        default:
            return -1;
        }
    }
    return state + 1;
}

/*
 * Bit of a hack here - in the absence of a real scheduling pass,
 * emit the next instruction in a virtual invoke sequence.
 * We can use rLR as a temp prior to target address loading
 * Note also that we'll load the first argument ("this") into
 * rARG1 here rather than the standard loadArgRegs.
 */
int nextVCallInsn(CompilationUnit* cUnit, MIR* mir,
                  int state, uint32_t dexIdx, uint32_t methodIdx,
                  uintptr_t unused, uintptr_t unused2, InvokeType unused3)
{
    RegLocation rlArg;
    /*
     * This is the fast path in which the target virtual method is
     * fully resolved at compile time.
     */
    switch (state) {
        case 0:  // Get "this" [set rARG1]
            rlArg = oatGetSrc(cUnit, mir, 0);
            loadValueDirectFixed(cUnit, rlArg, rARG1);
            break;
        case 1: // Is "this" null? [use rARG1]
            genNullCheck(cUnit, oatSSASrc(mir,0), rARG1, mir);
            // get this->klass_ [use rARG1, set rINVOKE_TGT]
            loadWordDisp(cUnit, rARG1, Object::ClassOffset().Int32Value(),
                         rINVOKE_TGT);
            break;
        case 2: // Get this->klass_->vtable [usr rINVOKE_TGT, set rINVOKE_TGT]
            loadWordDisp(cUnit, rINVOKE_TGT, Class::VTableOffset().Int32Value(),
                         rINVOKE_TGT);
            break;
        case 3: // Get target method [use rINVOKE_TGT, set rARG0]
            loadWordDisp(cUnit, rINVOKE_TGT, (methodIdx * 4) +
                         Array::DataOffset(sizeof(Object*)).Int32Value(),
                         rARG0);
            break;
#if !defined(TARGET_X86)
        case 4: // Get the compiled code address [uses rARG0, sets rINVOKE_TGT]
            loadWordDisp(cUnit, rARG0, Method::GetCodeOffset().Int32Value(),
                         rINVOKE_TGT);
            break;
#endif
        default:
            return -1;
    }
    return state + 1;
}

int nextInvokeInsnSP(CompilationUnit* cUnit, MIR* mir, int trampoline,
                     int state, uint32_t dexIdx, uint32_t methodIdx)
{
    /*
     * This handles the case in which the base method is not fully
     * resolved at compile time, we bail to a runtime helper.
     */
#if !defined(TARGET_X86)
    if (state == 0) {
        // Load trampoline target
        loadWordDisp(cUnit, rSELF, trampoline, rINVOKE_TGT);
        // Load rARG0 with method index
        loadConstant(cUnit, rARG0, dexIdx);
        return 1;
    }
#endif
    return -1;
}

int nextStaticCallInsnSP(CompilationUnit* cUnit, MIR* mir,
                         int state, uint32_t dexIdx, uint32_t methodIdx,
                         uintptr_t unused, uintptr_t unused2,
                         InvokeType unused3)
{
  int trampoline = OFFSETOF_MEMBER(Thread, pInvokeStaticTrampolineWithAccessCheck);
  return nextInvokeInsnSP(cUnit, mir, trampoline, state, dexIdx, 0);
}

int nextDirectCallInsnSP(CompilationUnit* cUnit, MIR* mir, int state,
                         uint32_t dexIdx, uint32_t methodIdx, uintptr_t unused,
                         uintptr_t unused2, InvokeType unused3)
{
  int trampoline = OFFSETOF_MEMBER(Thread, pInvokeDirectTrampolineWithAccessCheck);
  return nextInvokeInsnSP(cUnit, mir, trampoline, state, dexIdx, 0);
}

int nextSuperCallInsnSP(CompilationUnit* cUnit, MIR* mir, int state,
                        uint32_t dexIdx, uint32_t methodIdx, uintptr_t unused,
                        uintptr_t unused2, InvokeType unused3)
{
  int trampoline = OFFSETOF_MEMBER(Thread, pInvokeSuperTrampolineWithAccessCheck);
  return nextInvokeInsnSP(cUnit, mir, trampoline, state, dexIdx, 0);
}

int nextVCallInsnSP(CompilationUnit* cUnit, MIR* mir, int state,
                    uint32_t dexIdx, uint32_t methodIdx, uintptr_t unused,
                    uintptr_t unused2, InvokeType unused3)
{
  int trampoline = OFFSETOF_MEMBER(Thread, pInvokeVirtualTrampolineWithAccessCheck);
  return nextInvokeInsnSP(cUnit, mir, trampoline, state, dexIdx, 0);
}

/*
 * All invoke-interface calls bounce off of art_invoke_interface_trampoline,
 * which will locate the target and continue on via a tail call.
 */
int nextInterfaceCallInsn(CompilationUnit* cUnit, MIR* mir, int state,
                          uint32_t dexIdx, uint32_t unused, uintptr_t unused2,
                          uintptr_t unused3, InvokeType unused4)
{
  int trampoline = OFFSETOF_MEMBER(Thread, pInvokeInterfaceTrampoline);
  return nextInvokeInsnSP(cUnit, mir, trampoline, state, dexIdx, 0);
}

int nextInterfaceCallInsnWithAccessCheck(CompilationUnit* cUnit, MIR* mir,
                                         int state, uint32_t dexIdx,
                                         uint32_t unused, uintptr_t unused2,
                                         uintptr_t unused3, InvokeType unused4)
{
  int trampoline = OFFSETOF_MEMBER(Thread, pInvokeInterfaceTrampolineWithAccessCheck);
  return nextInvokeInsnSP(cUnit, mir, trampoline, state, dexIdx, 0);
}

int loadArgRegs(CompilationUnit* cUnit, MIR* mir, DecodedInstruction* dInsn,
                int callState, NextCallInsn nextCallInsn, uint32_t dexIdx,
                uint32_t methodIdx, uintptr_t directCode,
                uintptr_t directMethod, InvokeType type, bool skipThis)
{
#if !defined(TARGET_X86)
    int lastArgReg = rARG3;
#else
    int lastArgReg = rARG2;
#endif
    int nextReg = rARG1;
    int nextArg = 0;
    if (skipThis) {
        nextReg++;
        nextArg++;
    }
    for (; (nextReg <= lastArgReg) && (nextArg < mir->ssaRep->numUses); nextReg++) {
        RegLocation rlArg = oatGetRawSrc(cUnit, mir, nextArg++);
        rlArg = oatUpdateRawLoc(cUnit, rlArg);
        if (rlArg.wide && (nextReg <= rARG2)) {
            loadValueDirectWideFixed(cUnit, rlArg, nextReg, nextReg + 1);
            nextReg++;
            nextArg++;
        } else {
            rlArg.wide = false;
            loadValueDirectFixed(cUnit, rlArg, nextReg);
        }
        callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx,
                                 directCode, directMethod, type);
    }
    return callState;
}

/*
 * Load up to 5 arguments, the first three of which will be in
 * rARG1 .. rARG3.  On entry rARG0 contains the current method pointer,
 * and as part of the load sequence, it must be replaced with
 * the target method pointer.  Note, this may also be called
 * for "range" variants if the number of arguments is 5 or fewer.
 */
int genDalvikArgsNoRange(CompilationUnit* cUnit, MIR* mir,
                         DecodedInstruction* dInsn, int callState,
                         LIR** pcrLabel, NextCallInsn nextCallInsn,
                         uint32_t dexIdx, uint32_t methodIdx,
                         uintptr_t directCode, uintptr_t directMethod,
                         InvokeType type, bool skipThis)
{
    RegLocation rlArg;

    /* If no arguments, just return */
    if (dInsn->vA == 0)
        return callState;

    callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx,
                             directCode, directMethod, type);

    DCHECK_LE(dInsn->vA, 5U);
    if (dInsn->vA > 3) {
        uint32_t nextUse = 3;
        //Detect special case of wide arg spanning arg3/arg4
        RegLocation rlUse0 = oatGetRawSrc(cUnit, mir, 0);
        RegLocation rlUse1 = oatGetRawSrc(cUnit, mir, 1);
        RegLocation rlUse2 = oatGetRawSrc(cUnit, mir, 2);
        if (((!rlUse0.wide && !rlUse1.wide) || rlUse0.wide) &&
            rlUse2.wide) {
            int reg = -1;
            // Wide spans, we need the 2nd half of uses[2].
            rlArg = oatUpdateLocWide(cUnit, rlUse2);
            if (rlArg.location == kLocPhysReg) {
                reg = rlArg.highReg;
            } else {
                // rARG2 & rARG3 can safely be used here
                reg = rARG3;
                loadWordDisp(cUnit, rSP,
                             oatSRegOffset(cUnit, rlArg.sRegLow) + 4, reg);
                callState = nextCallInsn(cUnit, mir, callState, dexIdx,
                                         methodIdx, directCode, directMethod,
                                         type);
            }
            storeBaseDisp(cUnit, rSP, (nextUse + 1) * 4, reg, kWord);
            storeBaseDisp(cUnit, rSP, 16 /* (3+1)*4 */, reg, kWord);
            callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx,
                                     directCode, directMethod, type);
            nextUse++;
        }
        // Loop through the rest
        while (nextUse < dInsn->vA) {
            int lowReg;
            int highReg = -1;
            rlArg = oatGetRawSrc(cUnit, mir, nextUse);
            rlArg = oatUpdateRawLoc(cUnit, rlArg);
            if (rlArg.location == kLocPhysReg) {
                lowReg = rlArg.lowReg;
                highReg = rlArg.highReg;
            } else {
                lowReg = rARG2;
                if (rlArg.wide) {
                    highReg = rARG3;
                    loadValueDirectWideFixed(cUnit, rlArg, lowReg, highReg);
                } else {
                    loadValueDirectFixed(cUnit, rlArg, lowReg);
                }
                callState = nextCallInsn(cUnit, mir, callState, dexIdx,
                                         methodIdx, directCode, directMethod,
                                         type);
            }
            int outsOffset = (nextUse + 1) * 4;
            if (rlArg.wide) {
                storeBaseDispWide(cUnit, rSP, outsOffset, lowReg, highReg);
                nextUse += 2;
            } else {
                storeWordDisp(cUnit, rSP, outsOffset, lowReg);
                nextUse++;
            }
            callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx,
                                     directCode, directMethod, type);
        }
    }

    callState = loadArgRegs(cUnit, mir, dInsn, callState, nextCallInsn,
                            dexIdx, methodIdx, directCode, directMethod,
                            type, skipThis);

    if (pcrLabel) {
        *pcrLabel = genNullCheck(cUnit, oatSSASrc(mir,0), rARG1, mir);
    }
    return callState;
}

/*
 * May have 0+ arguments (also used for jumbo).  Note that
 * source virtual registers may be in physical registers, so may
 * need to be flushed to home location before copying.  This
 * applies to arg3 and above (see below).
 *
 * Two general strategies:
 *    If < 20 arguments
 *       Pass args 3-18 using vldm/vstm block copy
 *       Pass arg0, arg1 & arg2 in rARG1-rARG3
 *    If 20+ arguments
 *       Pass args arg19+ using memcpy block copy
 *       Pass arg0, arg1 & arg2 in rARG1-rARG3
 *
 */
int genDalvikArgsRange(CompilationUnit* cUnit, MIR* mir,
                       DecodedInstruction* dInsn, int callState,
                       LIR** pcrLabel, NextCallInsn nextCallInsn,
                       uint32_t dexIdx, uint32_t methodIdx,
                       uintptr_t directCode, uintptr_t directMethod,
                       InvokeType type, bool skipThis)
{
    int firstArg = dInsn->vC;
    int numArgs = dInsn->vA;

    // If we can treat it as non-range (Jumbo ops will use range form)
    if (numArgs <= 5)
        return genDalvikArgsNoRange(cUnit, mir, dInsn, callState, pcrLabel,
                                    nextCallInsn, dexIdx, methodIdx,
                                    directCode, directMethod, type, skipThis);
    /*
     * Make sure range list doesn't span the break between in normal
     * Dalvik vRegs and the ins.
     */
    int highestArg = oatGetSrc(cUnit, mir, numArgs-1).sRegLow;
    int boundaryReg = cUnit->numDalvikRegisters - cUnit->numIns;
    if ((firstArg < boundaryReg) && (highestArg >= boundaryReg)) {
        LOG(FATAL) << "Argument list spanned locals & args";
    }

    /*
     * First load the non-register arguments.  Both forms expect all
     * of the source arguments to be in their home frame location, so
     * scan the sReg names and flush any that have been promoted to
     * frame backing storage.
     */
    // Scan the rest of the args - if in physReg flush to memory
    for (int nextArg = 0; nextArg < numArgs;) {
        RegLocation loc = oatGetRawSrc(cUnit, mir, nextArg);
        if (loc.wide) {
            loc = oatUpdateLocWide(cUnit, loc);
            if ((nextArg >= 2) && (loc.location == kLocPhysReg)) {
                storeBaseDispWide(cUnit, rSP,
                                  oatSRegOffset(cUnit, loc.sRegLow),
                                  loc.lowReg, loc.highReg);
            }
            nextArg += 2;
        } else {
            loc = oatUpdateLoc(cUnit, loc);
            if ((nextArg >= 3) && (loc.location == kLocPhysReg)) {
                storeBaseDisp(cUnit, rSP, oatSRegOffset(cUnit, loc.sRegLow),
                              loc.lowReg, kWord);
            }
            nextArg++;
        }
    }

    int startOffset = oatSRegOffset(cUnit,
        cUnit->regLocation[mir->ssaRep->uses[3]].sRegLow);
    int outsOffset = 4 /* Method* */ + (3 * 4);
#if defined(TARGET_MIPS) || defined(TARGET_X86)
    // Generate memcpy
    opRegRegImm(cUnit, kOpAdd, rARG0, rSP, outsOffset);
    opRegRegImm(cUnit, kOpAdd, rARG1, rSP, startOffset);
    callRuntimeHelperRegRegImm(cUnit, OFFSETOF_MEMBER(Thread, pMemcpy),
                               rARG0, rARG1, (numArgs - 3) * 4);
#else
    if (numArgs >= 20) {
        // Generate memcpy
        opRegRegImm(cUnit, kOpAdd, rARG0, rSP, outsOffset);
        opRegRegImm(cUnit, kOpAdd, rARG1, rSP, startOffset);
        callRuntimeHelperRegRegImm(cUnit, OFFSETOF_MEMBER(Thread, pMemcpy),
                                   rARG0, rARG1, (numArgs - 3) * 4);
    } else {
        // Use vldm/vstm pair using rARG3 as a temp
        int regsLeft = std::min(numArgs - 3, 16);
        callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx,
                                 directCode, directMethod, type);
        opRegRegImm(cUnit, kOpAdd, rARG3, rSP, startOffset);
        LIR* ld = newLIR3(cUnit, kThumb2Vldms, rARG3, fr0, regsLeft);
        //TUNING: loosen barrier
        ld->defMask = ENCODE_ALL;
        setMemRefType(ld, true /* isLoad */, kDalvikReg);
        callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx,
                                 directCode, directMethod, type);
        opRegRegImm(cUnit, kOpAdd, rARG3, rSP, 4 /* Method* */ + (3 * 4));
        callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx,
                                 directCode, directMethod, type);
        LIR* st = newLIR3(cUnit, kThumb2Vstms, rARG3, fr0, regsLeft);
        setMemRefType(st, false /* isLoad */, kDalvikReg);
        st->defMask = ENCODE_ALL;
        callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx,
                                 directCode, directMethod, type);

    }
#endif

    callState = loadArgRegs(cUnit, mir, dInsn, callState, nextCallInsn,
                            dexIdx, methodIdx, directCode, directMethod,
                            type, skipThis);

    callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx,
                             directCode, directMethod, type);
    if (pcrLabel) {
        *pcrLabel = genNullCheck(cUnit, oatSSASrc(mir,0), rARG1, mir);
    }
    return callState;
}

RegLocation inlineTarget(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir)
{
    RegLocation res;
    mir = oatFindMoveResult(cUnit, bb, mir, false);
    if (mir == NULL) {
        res = oatGetReturn(cUnit, false);
    } else {
        res = oatGetDest(cUnit, mir, 0);
        mir->dalvikInsn.opcode = Instruction::NOP;
    }
    return res;
}

RegLocation inlineTargetWide(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir)
{
    RegLocation res;
    mir = oatFindMoveResult(cUnit, bb, mir, true);
    if (mir == NULL) {
        res = oatGetReturnWide(cUnit, false);
    } else {
        res = oatGetDestWide(cUnit, mir, 0, 1);
        mir->dalvikInsn.opcode = Instruction::NOP;
    }
    return res;
}

bool genInlinedCharAt(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                      InvokeType type, bool isRange)
{
#if defined(TARGET_ARM)
    // Location of reference to data array
    int valueOffset = String::ValueOffset().Int32Value();
    // Location of count
    int countOffset = String::CountOffset().Int32Value();
    // Starting offset within data array
    int offsetOffset = String::OffsetOffset().Int32Value();
    // Start of char data with array_
    int dataOffset = Array::DataOffset(sizeof(uint16_t)).Int32Value();

    RegLocation rlObj = oatGetSrc(cUnit, mir, 0);
    RegLocation rlIdx = oatGetSrc(cUnit, mir, 1);
    rlObj = loadValue(cUnit, rlObj, kCoreReg);
    rlIdx = loadValue(cUnit, rlIdx, kCoreReg);
    int regMax;
    int regOff = oatAllocTemp(cUnit);
    int regPtr = oatAllocTemp(cUnit);
    genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, mir);
    bool rangeCheck = (!(mir->optimizationFlags & MIR_IGNORE_RANGE_CHECK));
    if (rangeCheck) {
        regMax = oatAllocTemp(cUnit);
        loadWordDisp(cUnit, rlObj.lowReg, countOffset, regMax);
    }
    loadWordDisp(cUnit, rlObj.lowReg, offsetOffset, regOff);
    loadWordDisp(cUnit, rlObj.lowReg, valueOffset, regPtr);
    LIR* launchPad = NULL;
    if (rangeCheck) {
        // Set up a launch pad to allow retry in case of bounds violation */
        launchPad = rawLIR(cUnit, 0, kPseudoIntrinsicRetry, (int)mir, type);
        oatInsertGrowableList(cUnit, &cUnit->intrinsicLaunchpads,
                              (intptr_t)launchPad);
        opRegReg(cUnit, kOpCmp, rlIdx.lowReg, regMax);
        oatFreeTemp(cUnit, regMax);
        opCondBranch(cUnit, kCondCs, launchPad);
    }
    opRegImm(cUnit, kOpAdd, regPtr, dataOffset);
    opRegReg(cUnit, kOpAdd, regOff, rlIdx.lowReg);
    RegLocation rlDest = inlineTarget(cUnit, bb, mir);
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    loadBaseIndexed(cUnit, regPtr, regOff, rlResult.lowReg, 1, kUnsignedHalf);
    oatFreeTemp(cUnit, regOff);
    oatFreeTemp(cUnit, regPtr);
    storeValue(cUnit, rlDest, rlResult);
    if (rangeCheck) {
        launchPad->operands[2] = NULL;  // no resumption
        launchPad->operands[3] = (uintptr_t)bb;
    }
    // Record that we've already inlined & null checked
    mir->optimizationFlags |= (MIR_INLINED | MIR_IGNORE_NULL_CHECK);
    return true;
#else
    return false;
#endif
}

bool genInlinedMinMaxInt(CompilationUnit *cUnit, BasicBlock* bb, MIR *mir,
                         bool isMin)
{
#if defined(TARGET_ARM)
    RegLocation rlSrc1 = oatGetSrc(cUnit, mir, 0);
    RegLocation rlSrc2 = oatGetSrc(cUnit, mir, 1);
    rlSrc1 = loadValue(cUnit, rlSrc1, kCoreReg);
    rlSrc2 = loadValue(cUnit, rlSrc2, kCoreReg);
    RegLocation rlDest = inlineTarget(cUnit, bb, mir);
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    opRegReg(cUnit, kOpCmp, rlSrc1.lowReg, rlSrc2.lowReg);
    opIT(cUnit, (isMin) ? kArmCondGt : kArmCondLt, "E");
    opRegReg(cUnit, kOpMov, rlResult.lowReg, rlSrc2.lowReg);
    opRegReg(cUnit, kOpMov, rlResult.lowReg, rlSrc1.lowReg);
    genBarrier(cUnit);
    storeValue(cUnit, rlDest, rlResult);
    return true;
#else
    return false;
#endif
}

// Generates an inlined String.isEmpty or String.length.
bool genInlinedStringIsEmptyOrLength(CompilationUnit* cUnit,
                                            BasicBlock* bb, MIR* mir,
                                            bool isEmpty)
{
#if defined(TARGET_ARM)
    // dst = src.length();
    RegLocation rlObj = oatGetSrc(cUnit, mir, 0);
    rlObj = loadValue(cUnit, rlObj, kCoreReg);
    RegLocation rlDest = inlineTarget(cUnit, bb, mir);
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, mir);
    loadWordDisp(cUnit, rlObj.lowReg, String::CountOffset().Int32Value(),
                 rlResult.lowReg);
    if (isEmpty) {
        // dst = (dst == 0);
        int tReg = oatAllocTemp(cUnit);
        opRegReg(cUnit, kOpNeg, tReg, rlResult.lowReg);
        opRegRegReg(cUnit, kOpAdc, rlResult.lowReg, rlResult.lowReg, tReg);
    }
    storeValue(cUnit, rlDest, rlResult);
    return true;
#else
    return false;
#endif
}

bool genInlinedAbsInt(CompilationUnit *cUnit, BasicBlock* bb, MIR *mir)
{
#if defined(TARGET_ARM)
    RegLocation rlSrc = oatGetSrc(cUnit, mir, 0);
    rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
    RegLocation rlDest = inlineTarget(cUnit, bb, mir);
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    int signReg = oatAllocTemp(cUnit);
    // abs(x) = y<=x>>31, (x+y)^y.
    opRegRegImm(cUnit, kOpAsr, signReg, rlSrc.lowReg, 31);
    opRegRegReg(cUnit, kOpAdd, rlResult.lowReg, rlSrc.lowReg, signReg);
    opRegReg(cUnit, kOpXor, rlResult.lowReg, signReg);
    storeValue(cUnit, rlDest, rlResult);
    return true;
#else
    return false;
#endif
}

bool genInlinedAbsLong(CompilationUnit *cUnit, BasicBlock* bb, MIR *mir)
{
#if defined(TARGET_ARM)
    RegLocation rlSrc = oatGetSrcWide(cUnit, mir, 0, 1);
    rlSrc = loadValueWide(cUnit, rlSrc, kCoreReg);
    RegLocation rlDest = inlineTargetWide(cUnit, bb, mir);
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    int signReg = oatAllocTemp(cUnit);
    // abs(x) = y<=x>>31, (x+y)^y.
    opRegRegImm(cUnit, kOpAsr, signReg, rlSrc.highReg, 31);
    opRegRegReg(cUnit, kOpAdd, rlResult.lowReg, rlSrc.lowReg, signReg);
    opRegRegReg(cUnit, kOpAdc, rlResult.highReg, rlSrc.highReg, signReg);
    opRegReg(cUnit, kOpXor, rlResult.lowReg, signReg);
    opRegReg(cUnit, kOpXor, rlResult.highReg, signReg);
    storeValueWide(cUnit, rlDest, rlResult);
    return true;
#else
    return false;
#endif
}

bool genInlinedFloatCvt(CompilationUnit *cUnit, BasicBlock* bb, MIR *mir)
{
#if defined(TARGET_ARM)
    RegLocation rlSrc = oatGetSrc(cUnit, mir, 0);
    RegLocation rlDest = inlineTarget(cUnit, bb, mir);
    storeValue(cUnit, rlDest, rlSrc);
    return true;
#else
    return false;
#endif
}

bool genInlinedDoubleCvt(CompilationUnit *cUnit, BasicBlock* bb, MIR *mir)
{
#if defined(TARGET_ARM)
    RegLocation rlSrc = oatGetSrcWide(cUnit, mir, 0, 1);
    RegLocation rlDest = inlineTargetWide(cUnit, bb, mir);
    storeValueWide(cUnit, rlDest, rlSrc);
    return true;
#else
    return false;
#endif
}

/*
 * Fast string.indexOf(I) & (II).  Tests for simple case of char <= 0xffff,
 * otherwise bails to standard library code.
 */
bool genInlinedIndexOf(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                       InvokeType type, bool zeroBased)
{
#if defined(TARGET_ARM)

    oatClobberCalleeSave(cUnit);
    oatLockCallTemps(cUnit);  // Using fixed registers
    int regPtr = rARG0;
    int regChar = rARG1;
    int regStart = rARG2;

    RegLocation rlObj = oatGetSrc(cUnit, mir, 0);
    RegLocation rlChar = oatGetSrc(cUnit, mir, 1);
    RegLocation rlStart = oatGetSrc(cUnit, mir, 2);
    loadValueDirectFixed(cUnit, rlObj, regPtr);
    loadValueDirectFixed(cUnit, rlChar, regChar);
    if (zeroBased) {
        loadConstant(cUnit, regStart, 0);
    } else {
        loadValueDirectFixed(cUnit, rlStart, regStart);
    }
    int rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread, pIndexOf));
    genNullCheck(cUnit, rlObj.sRegLow, regPtr, mir);
    LIR* launchPad = rawLIR(cUnit, 0, kPseudoIntrinsicRetry, (int)mir, type);
    oatInsertGrowableList(cUnit, &cUnit->intrinsicLaunchpads,
                          (intptr_t)launchPad);
    opCmpImmBranch(cUnit, kCondGt, regChar, 0xFFFF, launchPad);
    opReg(cUnit, kOpBlx, rTgt);
    LIR* resumeTgt = newLIR0(cUnit, kPseudoTargetLabel);
    launchPad->operands[2] = (uintptr_t)resumeTgt;
    launchPad->operands[3] = (uintptr_t)bb;
    // Record that we've already inlined & null checked
    mir->optimizationFlags |= (MIR_INLINED | MIR_IGNORE_NULL_CHECK);
    return true;
#else
    return false;
#endif
}

/* Fast string.compareTo(Ljava/lang/string;)I. */
bool genInlinedStringCompareTo(CompilationUnit* cUnit, BasicBlock* bb,
                               MIR* mir, InvokeType type)
{
#if defined(TARGET_ARM)
    oatClobberCalleeSave(cUnit);
    oatLockCallTemps(cUnit);  // Using fixed registers
    int regThis = rARG0;
    int regCmp = rARG1;

    RegLocation rlThis = oatGetSrc(cUnit, mir, 0);
    RegLocation rlCmp = oatGetSrc(cUnit, mir, 1);
    loadValueDirectFixed(cUnit, rlThis, regThis);
    loadValueDirectFixed(cUnit, rlCmp, regCmp);
    int rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread, pStringCompareTo));
    genNullCheck(cUnit, rlThis.sRegLow, regThis, mir);
    //TUNING: check if rlCmp.sRegLow is already null checked
    LIR* launchPad = rawLIR(cUnit, 0, kPseudoIntrinsicRetry, (int)mir, type);
    oatInsertGrowableList(cUnit, &cUnit->intrinsicLaunchpads,
                          (intptr_t)launchPad);
    opCmpImmBranch(cUnit, kCondEq, regCmp, 0, launchPad);
    opReg(cUnit, kOpBlx, rTgt);
    launchPad->operands[2] = NULL;  // No return possible
    launchPad->operands[3] = (uintptr_t)bb;
    // Record that we've already inlined & null checked
    mir->optimizationFlags |= (MIR_INLINED | MIR_IGNORE_NULL_CHECK);
    return true;
#else
    return false;
#endif
}

bool genIntrinsic(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                  InvokeType type, bool isRange)
{
    if ((mir->optimizationFlags & MIR_INLINED) || isRange)  {
        return false;
    }
    /*
     * TODO: move these to a target-specific structured constant array
     * and use a generic match function.  The list of intrinsics may be
     * slightly different depending on target.
     * TODO: Fold this into a matching function that runs during
     * basic block building.  This should be part of the action for
     * small method inlining and recognition of the special object init
     * method.  By doing this during basic block construction, we can also
     * take advantage of/generate new useful dataflow info.
     */
    std::string tgtMethod = PrettyMethod(mir->dalvikInsn.vB, *cUnit->dex_file);
    if (tgtMethod.compare("char java.lang.String.charAt(int)") == 0) {
        return genInlinedCharAt(cUnit, bb, mir, type, isRange);
    }
    if (tgtMethod.compare("int java.lang.Math.min(int, int)") == 0) {
        return genInlinedMinMaxInt(cUnit, bb, mir, true /* isMin */);
    }
    if (tgtMethod.compare("int java.lang.Math.max(int, int)") == 0) {
        return genInlinedMinMaxInt(cUnit, bb, mir, false /* isMin */);
    }
    if (tgtMethod.compare("int java.lang.String.length()") == 0) {
        return genInlinedStringIsEmptyOrLength(cUnit, bb, mir, false /* isEmpty */);
    }
    if (tgtMethod.compare("boolean java.lang.String.isEmpty()") == 0) {
        return genInlinedStringIsEmptyOrLength(cUnit, bb, mir, true /* isEmpty */);
    }
    if (tgtMethod.compare("int java.lang.Math.abs(int)") == 0) {
        return genInlinedAbsInt(cUnit, bb, mir);
    }
    if (tgtMethod.compare("long java.lang.Math.abs(long)") == 0) {
        return genInlinedAbsLong(cUnit, bb, mir);
    }
    if (tgtMethod.compare("int java.lang.Float.floatToRawIntBits(float)") == 0) {
        return genInlinedFloatCvt(cUnit, bb, mir);
    }
    if (tgtMethod.compare("float java.lang.Float.intBitsToFloat(int)") == 0) {
        return genInlinedFloatCvt(cUnit, bb, mir);
    }
    if (tgtMethod.compare("long java.lang.Double.doubleToRawLongBits(double)") == 0) {
        return genInlinedDoubleCvt(cUnit, bb, mir);
    }
    if (tgtMethod.compare("double java.lang.Double.longBitsToDouble(long)") == 0) {
        return genInlinedDoubleCvt(cUnit, bb, mir);
    }
    if (tgtMethod.compare("int java.lang.String.indexOf(int, int)") == 0) {
        return genInlinedIndexOf(cUnit, bb, mir, type, false /* base 0 */);
    }
    if (tgtMethod.compare("int java.lang.String.indexOf(int)") == 0) {
        return genInlinedIndexOf(cUnit, bb, mir, type, true /* base 0 */);
    }
    if (tgtMethod.compare("int java.lang.String.compareTo(java.lang.String)") == 0) {
        return genInlinedStringCompareTo(cUnit, bb, mir, type);
    }
    return false;
}


}  // namespace art
