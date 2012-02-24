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

#include "object_utils.h"

namespace art {

#define DISPLAY_MISSING_TARGETS (cUnit->enableDebug & \
    (1 << kDebugDisplayMissingTargets))

STATIC const RegLocation badLoc = {kLocDalvikFrame, 0, 0, 0, 0, 0, 0, INVALID_REG,
                                   INVALID_REG, INVALID_SREG};

/* Mark register usage state and return long retloc */
STATIC RegLocation getRetLocWide(CompilationUnit* cUnit)
{
    RegLocation res = LOC_DALVIK_RETURN_VAL_WIDE;
    oatLockTemp(cUnit, res.lowReg);
    oatLockTemp(cUnit, res.highReg);
    oatMarkPair(cUnit, res.lowReg, res.highReg);
    return res;
}

STATIC RegLocation getRetLoc(CompilationUnit* cUnit)
{
    RegLocation res = LOC_DALVIK_RETURN_VAL;
    oatLockTemp(cUnit, res.lowReg);
    return res;
}

/*
 * Let helper function take care of everything.  Will call
 * Array::AllocFromCode(type_idx, method, count);
 * Note: AllocFromCode will handle checks for errNegativeArraySize.
 */
STATIC void genNewArray(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                        RegLocation rlSrc)
{
    oatFlushAllRegs(cUnit);    /* Everything to home location */
    uint32_t type_idx = mir->dalvikInsn.vC;
    if (cUnit->compiler->CanAccessTypeWithoutChecks(cUnit->method_idx,
                                                    cUnit->dex_cache,
                                                    *cUnit->dex_file,
                                                    type_idx)) {
        loadWordDisp(cUnit, rSELF,
                     OFFSETOF_MEMBER(Thread, pAllocArrayFromCode), rLR);
    } else {
        loadWordDisp(cUnit, rSELF,
                     OFFSETOF_MEMBER(Thread, pAllocArrayFromCodeWithAccessCheck), rLR);
    }
    loadCurrMethodDirect(cUnit, r1);              // arg1 <- Method*
    loadConstant(cUnit, r0, type_idx);            // arg0 <- type_id
    loadValueDirectFixed(cUnit, rlSrc, r2);       // arg2 <- count
    callRuntimeHelper(cUnit, rLR);
    RegLocation rlResult = oatGetReturn(cUnit);
    storeValue(cUnit, rlDest, rlResult);
}

/*
 * Similar to genNewArray, but with post-allocation initialization.
 * Verifier guarantees we're dealing with an array class.  Current
 * code throws runtime exception "bad Filled array req" for 'D' and 'J'.
 * Current code also throws internal unimp if not 'L', '[' or 'I'.
 */
STATIC void genFilledNewArray(CompilationUnit* cUnit, MIR* mir, bool isRange)
{
    DecodedInstruction* dInsn = &mir->dalvikInsn;
    int elems = dInsn->vA;
    int typeId = dInsn->vB;
    oatFlushAllRegs(cUnit);    /* Everything to home location */
    if (cUnit->compiler->CanAccessTypeWithoutChecks(cUnit->method_idx,
                                                    cUnit->dex_cache,
                                                    *cUnit->dex_file,
                                                    typeId)) {
        loadWordDisp(cUnit, rSELF,
                     OFFSETOF_MEMBER(Thread, pCheckAndAllocArrayFromCode), rLR);
    } else {
        loadWordDisp(cUnit, rSELF,
                     OFFSETOF_MEMBER(Thread, pCheckAndAllocArrayFromCodeWithAccessCheck), rLR);
    }
    loadCurrMethodDirect(cUnit, r1);              // arg1 <- Method*
    loadConstant(cUnit, r0, typeId);              // arg0 <- type_id
    loadConstant(cUnit, r2, elems);               // arg2 <- count
    callRuntimeHelper(cUnit, rLR);
    /*
     * NOTE: the implicit target for OP_FILLED_NEW_ARRAY is the
     * return region.  Because AllocFromCode placed the new array
     * in r0, we'll just lock it into place.  When debugger support is
     * added, it may be necessary to additionally copy all return
     * values to a home location in thread-local storage
     */
    oatLockTemp(cUnit, r0);

    // Having a range of 0 is legal
    if (isRange && (dInsn->vA > 0)) {
        /*
         * Bit of ugliness here.  We're going generate a mem copy loop
         * on the register range, but it is possible that some regs
         * in the range have been promoted.  This is unlikely, but
         * before generating the copy, we'll just force a flush
         * of any regs in the source range that have been promoted to
         * home location.
         */
        for (unsigned int i = 0; i < dInsn->vA; i++) {
            RegLocation loc = oatUpdateLoc(cUnit,
                oatGetSrc(cUnit, mir, i));
            if (loc.location == kLocPhysReg) {
                storeBaseDisp(cUnit, rSP, oatSRegOffset(cUnit, loc.sRegLow),
                              loc.lowReg, kWord);
            }
        }
        /*
         * TUNING note: generated code here could be much improved, but
         * this is an uncommon operation and isn't especially performance
         * critical.
         */
        int rSrc = oatAllocTemp(cUnit);
        int rDst = oatAllocTemp(cUnit);
        int rIdx = oatAllocTemp(cUnit);
        int rVal = rLR;  // Using a lot of temps, rLR is known free here
        // Set up source pointer
        RegLocation rlFirst = oatGetSrc(cUnit, mir, 0);
        opRegRegImm(cUnit, kOpAdd, rSrc, rSP,
                    oatSRegOffset(cUnit, rlFirst.sRegLow));
        // Set up the target pointer
        opRegRegImm(cUnit, kOpAdd, rDst, r0,
                    Array::DataOffset().Int32Value());
        // Set up the loop counter (known to be > 0)
        loadConstant(cUnit, rIdx, dInsn->vA - 1);
        // Generate the copy loop.  Going backwards for convenience
        ArmLIR* target = newLIR0(cUnit, kArmPseudoTargetLabel);
        target->defMask = ENCODE_ALL;
        // Copy next element
        loadBaseIndexed(cUnit, rSrc, rIdx, rVal, 2, kWord);
        storeBaseIndexed(cUnit, rDst, rIdx, rVal, 2, kWord);
        // Use setflags encoding here
        newLIR3(cUnit, kThumb2SubsRRI12, rIdx, rIdx, 1);
        ArmLIR* branch = opCondBranch(cUnit, kArmCondGe);
        branch->generic.target = (LIR*)target;
    } else if (!isRange) {
        // TUNING: interleave
        for (unsigned int i = 0; i < dInsn->vA; i++) {
            RegLocation rlArg = loadValue(cUnit,
                oatGetSrc(cUnit, mir, i), kCoreReg);
            storeBaseDisp(cUnit, r0,
                          Array::DataOffset().Int32Value() +
                          i * 4, rlArg.lowReg, kWord);
            // If the loadValue caused a temp to be allocated, free it
            if (oatIsTemp(cUnit, rlArg.lowReg)) {
                oatFreeTemp(cUnit, rlArg.lowReg);
            }
        }
    }
}

STATIC void genSput(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc,
                    bool isLongOrDouble, bool isObject)
{
    int fieldOffset;
    int ssbIndex;
    bool isVolatile;
    bool isReferrersClass;
    uint32_t fieldIdx = mir->dalvikInsn.vB;
    bool fastPath =
        cUnit->compiler->ComputeStaticFieldInfo(fieldIdx, cUnit,
                                                fieldOffset, ssbIndex,
                                                isReferrersClass, isVolatile, true);
    if (fastPath && !SLOW_FIELD_PATH) {
        DCHECK_GE(fieldOffset, 0);
        int rBase;
        int rMethod;
        if (isReferrersClass) {
            // Fast path, static storage base is this method's class
            rMethod  = loadCurrMethod(cUnit);
            rBase = oatAllocTemp(cUnit);
            loadWordDisp(cUnit, rMethod,
                         Method::DeclaringClassOffset().Int32Value(), rBase);
        } else {
            // Medium path, static storage base in a different class which
            // requires checks that the other class is initialized.
            DCHECK_GE(ssbIndex, 0);
            // May do runtime call so everything to home locations.
            oatFlushAllRegs(cUnit);
            // Using fixed register to sync with possible call to runtime
            // support.
            rMethod = r1;
            oatLockTemp(cUnit, rMethod);
            loadCurrMethodDirect(cUnit, rMethod);
            rBase = r0;
            oatLockTemp(cUnit, rBase);
            loadWordDisp(cUnit, rMethod,
                Method::DexCacheInitializedStaticStorageOffset().Int32Value(),
                rBase);
            loadWordDisp(cUnit, rBase,
                         Array::DataOffset().Int32Value() + sizeof(int32_t*) * ssbIndex,
                         rBase);
            // rBase now points at appropriate static storage base (Class*)
            // or NULL if not initialized. Check for NULL and call helper if NULL.
            // TUNING: fast path should fall through
            ArmLIR* branchOver = genCmpImmBranch(cUnit, kArmCondNe, rBase, 0);
            loadWordDisp(cUnit, rSELF,
                         OFFSETOF_MEMBER(Thread, pInitializeStaticStorage), rLR);
            loadConstant(cUnit, r0, ssbIndex);
            callRuntimeHelper(cUnit, rLR);
            ArmLIR* skipTarget = newLIR0(cUnit, kArmPseudoTargetLabel);
            skipTarget->defMask = ENCODE_ALL;
            branchOver->generic.target = (LIR*)skipTarget;
        }
        // rBase now holds static storage base
        oatFreeTemp(cUnit, rMethod);
        if (isLongOrDouble) {
            rlSrc = oatGetSrcWide(cUnit, mir, 0, 1);
            rlSrc = loadValueWide(cUnit, rlSrc, kAnyReg);
        } else {
            rlSrc = oatGetSrc(cUnit, mir, 0);
            rlSrc = loadValue(cUnit, rlSrc, kAnyReg);
        }
        if (isVolatile) {
            oatGenMemBarrier(cUnit, kST);
        }
        if (isLongOrDouble) {
            storeBaseDispWide(cUnit, rBase, fieldOffset, rlSrc.lowReg,
                              rlSrc.highReg);
        } else {
            storeWordDisp(cUnit, rBase, fieldOffset, rlSrc.lowReg);
        }
        if (isVolatile) {
            oatGenMemBarrier(cUnit, kSY);
        }
        if (isObject) {
            markGCCard(cUnit, rlSrc.lowReg, rBase);
        }
        oatFreeTemp(cUnit, rBase);
    } else {
        oatFlushAllRegs(cUnit);  // Everything to home locations
        int setterOffset = isLongOrDouble ? OFFSETOF_MEMBER(Thread, pSet64Static) :
                           (isObject ? OFFSETOF_MEMBER(Thread, pSetObjStatic)
                                     : OFFSETOF_MEMBER(Thread, pSet32Static));
        loadWordDisp(cUnit, rSELF, setterOffset, rLR);
        loadConstant(cUnit, r0, fieldIdx);
        if (isLongOrDouble) {
            loadValueDirectWideFixed(cUnit, rlSrc, r2, r3);
        } else {
            loadValueDirect(cUnit, rlSrc, r1);
        }
        callRuntimeHelper(cUnit, rLR);
    }
}

STATIC void genSget(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                    bool isLongOrDouble, bool isObject)
{
    int fieldOffset;
    int ssbIndex;
    bool isVolatile;
    bool isReferrersClass;
    uint32_t fieldIdx = mir->dalvikInsn.vB;
    bool fastPath =
        cUnit->compiler->ComputeStaticFieldInfo(fieldIdx, cUnit,
                                                fieldOffset, ssbIndex,
                                                isReferrersClass, isVolatile, false);
    if (fastPath && !SLOW_FIELD_PATH) {
        DCHECK_GE(fieldOffset, 0);
        int rBase;
        int rMethod;
        if (isReferrersClass) {
            // Fast path, static storage base is this method's class
            rMethod  = loadCurrMethod(cUnit);
            rBase = oatAllocTemp(cUnit);
            loadWordDisp(cUnit, rMethod,
                         Method::DeclaringClassOffset().Int32Value(), rBase);
        } else {
            // Medium path, static storage base in a different class which
            // requires checks that the other class is initialized
            DCHECK_GE(ssbIndex, 0);
            // May do runtime call so everything to home locations.
            oatFlushAllRegs(cUnit);
            // Using fixed register to sync with possible call to runtime
            // support
            rMethod = r1;
            oatLockTemp(cUnit, rMethod);
            loadCurrMethodDirect(cUnit, rMethod);
            rBase = r0;
            oatLockTemp(cUnit, rBase);
            loadWordDisp(cUnit, rMethod,
                Method::DexCacheInitializedStaticStorageOffset().Int32Value(),
                rBase);
            loadWordDisp(cUnit, rBase,
                         Array::DataOffset().Int32Value() + sizeof(int32_t*) * ssbIndex,
                         rBase);
            // rBase now points at appropriate static storage base (Class*)
            // or NULL if not initialized. Check for NULL and call helper if NULL.
            // TUNING: fast path should fall through
            ArmLIR* branchOver = genCmpImmBranch(cUnit, kArmCondNe, rBase, 0);
            loadWordDisp(cUnit, rSELF,
                         OFFSETOF_MEMBER(Thread, pInitializeStaticStorage), rLR);
            loadConstant(cUnit, r0, ssbIndex);
            callRuntimeHelper(cUnit, rLR);
            ArmLIR* skipTarget = newLIR0(cUnit, kArmPseudoTargetLabel);
            skipTarget->defMask = ENCODE_ALL;
            branchOver->generic.target = (LIR*)skipTarget;
        }
        // rBase now holds static storage base
        oatFreeTemp(cUnit, rMethod);
        rlDest = isLongOrDouble ? oatGetDestWide(cUnit, mir, 0, 1)
                                : oatGetDest(cUnit, mir, 0);
        RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
        if (isVolatile) {
            oatGenMemBarrier(cUnit, kSY);
        }
        if (isLongOrDouble) {
            loadBaseDispWide(cUnit, NULL, rBase, fieldOffset, rlResult.lowReg,
                             rlResult.highReg, INVALID_SREG);
        } else {
            loadWordDisp(cUnit, rBase, fieldOffset, rlResult.lowReg);
        }
        oatFreeTemp(cUnit, rBase);
        if (isLongOrDouble) {
            storeValueWide(cUnit, rlDest, rlResult);
        } else {
            storeValue(cUnit, rlDest, rlResult);
        }
    } else {
        oatFlushAllRegs(cUnit);  // Everything to home locations
        int getterOffset = isLongOrDouble ? OFFSETOF_MEMBER(Thread, pGet64Static) :
                           (isObject ? OFFSETOF_MEMBER(Thread, pGetObjStatic)
                                     : OFFSETOF_MEMBER(Thread, pGet32Static));
        loadWordDisp(cUnit, rSELF, getterOffset, rLR);
        loadConstant(cUnit, r0, fieldIdx);
        callRuntimeHelper(cUnit, rLR);
        if (isLongOrDouble) {
            RegLocation rlResult = oatGetReturnWide(cUnit);
            storeValueWide(cUnit, rlDest, rlResult);
        } else {
            RegLocation rlResult = oatGetReturn(cUnit);
            storeValue(cUnit, rlDest, rlResult);
        }
    }
}

typedef int (*NextCallInsn)(CompilationUnit*, MIR*, int, uint32_t dexIdx,
                            uint32_t methodIdx);

/*
 * Bit of a hack here - in leiu of a real scheduling pass,
 * emit the next instruction in static & direct invoke sequences.
 */
STATIC int nextSDCallInsn(CompilationUnit* cUnit, MIR* mir,
                          int state, uint32_t dexIdx, uint32_t unused)
{
    switch(state) {
        case 0:  // Get the current Method* [sets r0]
            loadCurrMethodDirect(cUnit, r0);
            break;
        case 1:  // Get method->code_and_direct_methods_
            loadWordDisp(cUnit, r0,
                Method::GetDexCacheCodeAndDirectMethodsOffset().Int32Value(),
                r0);
            break;
        case 2:  // Grab target method* and target code_
            loadWordDisp(cUnit, r0,
                CodeAndDirectMethods::CodeOffsetInBytes(dexIdx), rLR);
            loadWordDisp(cUnit, r0,
                CodeAndDirectMethods::MethodOffsetInBytes(dexIdx), r0);
            break;
        default:
            return -1;
    }
    return state + 1;
}

/*
 * Bit of a hack here - in leiu of a real scheduling pass,
 * emit the next instruction in a virtual invoke sequence.
 * We can use rLR as a temp prior to target address loading
 * Note also that we'll load the first argument ("this") into
 * r1 here rather than the standard loadArgRegs.
 */
STATIC int nextVCallInsn(CompilationUnit* cUnit, MIR* mir,
                         int state, uint32_t dexIdx, uint32_t methodIdx)
{
    RegLocation rlArg;
    /*
     * This is the fast path in which the target virtual method is
     * fully resolved at compile time.
     */
    switch(state) {
        case 0:  // Get "this" [set r1]
            rlArg = oatGetSrc(cUnit, mir, 0);
            loadValueDirectFixed(cUnit, rlArg, r1);
            break;
        case 1: // Is "this" null? [use r1]
            genNullCheck(cUnit, oatSSASrc(mir,0), r1, mir);
            // get this->klass_ [use r1, set rLR]
            loadWordDisp(cUnit, r1, Object::ClassOffset().Int32Value(), rLR);
            break;
        case 2: // Get this->klass_->vtable [usr rLR, set rLR]
            loadWordDisp(cUnit, rLR, Class::VTableOffset().Int32Value(), rLR);
            break;
        case 3: // Get target method [use rLR, set r0]
            loadWordDisp(cUnit, rLR, (methodIdx * 4) +
                         Array::DataOffset().Int32Value(), r0);
            break;
        case 4: // Get the target compiled code address [uses r0, sets rLR]
            loadWordDisp(cUnit, r0, Method::GetCodeOffset().Int32Value(), rLR);
            break;
        default:
            return -1;
    }
    return state + 1;
}

/*
 * Interleave launch code for INVOKE_SUPER.  See comments
 * for nextVCallIns.
 */
STATIC int nextSuperCallInsn(CompilationUnit* cUnit, MIR* mir,
                             int state, uint32_t dexIdx, uint32_t methodIdx)
{
    /*
     * This is the fast path in which the target virtual method is
     * fully resolved at compile time.  Note also that this path assumes
     * that the check to verify that the target method index falls
     * within the size of the super's vtable has been done at compile-time.
     */
    RegLocation rlArg;
    switch(state) {
        case 0: // Get current Method* [set r0]
            loadCurrMethodDirect(cUnit, r0);
            // Load "this" [set r1]
            rlArg = oatGetSrc(cUnit, mir, 0);
            loadValueDirectFixed(cUnit, rlArg, r1);
            // Get method->declaring_class_ [use r0, set rLR]
            loadWordDisp(cUnit, r0, Method::DeclaringClassOffset().Int32Value(),
                         rLR);
            // Is "this" null? [use r1]
            genNullCheck(cUnit, oatSSASrc(mir,0), r1, mir);
            break;
        case 1: // Get method->declaring_class_->super_class [usr rLR, set rLR]
            loadWordDisp(cUnit, rLR, Class::SuperClassOffset().Int32Value(),
                         rLR);
            break;
        case 2: // Get ...->super_class_->vtable [u/s rLR]
            loadWordDisp(cUnit, rLR, Class::VTableOffset().Int32Value(), rLR);
            break;
        case 3: // Get target method [use rLR, set r0]
            loadWordDisp(cUnit, rLR, (methodIdx * 4) +
                         Array::DataOffset().Int32Value(), r0);
            break;
        case 4: // Get the target compiled code address [uses r0, sets rLR]
            loadWordDisp(cUnit, r0, Method::GetCodeOffset().Int32Value(), rLR);
            break;
        default:
            return -1;
    }
    return state + 1;
}

STATIC int nextInvokeInsnSP(CompilationUnit* cUnit, MIR* mir, int trampoline,
                            int state, uint32_t dexIdx, uint32_t methodIdx)
{
    /*
     * This handles the case in which the base method is not fully
     * resolved at compile time, we bail to a runtime helper.
     */
    if (state == 0) {
        // Load trampoline target
        loadWordDisp(cUnit, rSELF, trampoline, rLR);
        // Load r0 with method index
        loadConstant(cUnit, r0, dexIdx);
        return 1;
    }
    return -1;
}

STATIC int nextStaticCallInsnSP(CompilationUnit* cUnit, MIR* mir,
                                int state, uint32_t dexIdx, uint32_t methodIdx)
{
  int trampoline = OFFSETOF_MEMBER(Thread, pInvokeStaticTrampolineWithAccessCheck);
  return nextInvokeInsnSP(cUnit, mir, trampoline, state, dexIdx, 0);
}

STATIC int nextDirectCallInsnSP(CompilationUnit* cUnit, MIR* mir,
                                int state, uint32_t dexIdx, uint32_t methodIdx)
{
  int trampoline = OFFSETOF_MEMBER(Thread, pInvokeDirectTrampolineWithAccessCheck);
  return nextInvokeInsnSP(cUnit, mir, trampoline, state, dexIdx, 0);
}

STATIC int nextSuperCallInsnSP(CompilationUnit* cUnit, MIR* mir,
                               int state, uint32_t dexIdx, uint32_t methodIdx)
{
  int trampoline = OFFSETOF_MEMBER(Thread, pInvokeSuperTrampolineWithAccessCheck);
  return nextInvokeInsnSP(cUnit, mir, trampoline, state, dexIdx, 0);
}

STATIC int nextVCallInsnSP(CompilationUnit* cUnit, MIR* mir,
                           int state, uint32_t dexIdx, uint32_t methodIdx)
{
  int trampoline = OFFSETOF_MEMBER(Thread, pInvokeVirtualTrampolineWithAccessCheck);
  return nextInvokeInsnSP(cUnit, mir, trampoline, state, dexIdx, 0);
}

/*
 * All invoke-interface calls bounce off of art_invoke_interface_trampoline,
 * which will locate the target and continue on via a tail call.
 */
STATIC int nextInterfaceCallInsn(CompilationUnit* cUnit, MIR* mir,
                                 int state, uint32_t dexIdx, uint32_t unused)
{
  int trampoline = OFFSETOF_MEMBER(Thread, pInvokeInterfaceTrampoline);
  return nextInvokeInsnSP(cUnit, mir, trampoline, state, dexIdx, 0);
}

STATIC int nextInterfaceCallInsnWithAccessCheck(CompilationUnit* cUnit,
                                                MIR* mir, int state,
                                                uint32_t dexIdx,
                                                uint32_t unused)
{
  int trampoline = OFFSETOF_MEMBER(Thread, pInvokeInterfaceTrampolineWithAccessCheck);
  return nextInvokeInsnSP(cUnit, mir, trampoline, state, dexIdx, 0);
}

STATIC int loadArgRegs(CompilationUnit* cUnit, MIR* mir,
                          DecodedInstruction* dInsn, int callState,
                          NextCallInsn nextCallInsn, uint32_t dexIdx,
                          uint32_t methodIdx, bool skipThis)
{
    int nextReg = r1;
    int nextArg = 0;
    if (skipThis) {
        nextReg++;
        nextArg++;
    }
    for (; (nextReg <= r3) && (nextArg < mir->ssaRep->numUses); nextReg++) {
        RegLocation rlArg = oatGetRawSrc(cUnit, mir, nextArg++);
        rlArg = oatUpdateRawLoc(cUnit, rlArg);
        if (rlArg.wide && (nextReg <= r2)) {
            loadValueDirectWideFixed(cUnit, rlArg, nextReg, nextReg + 1);
            nextReg++;
            nextArg++;
        } else {
            rlArg.wide = false;
            loadValueDirectFixed(cUnit, rlArg, nextReg);
        }
        callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx);
    }
    return callState;
}

/*
 * Load up to 5 arguments, the first three of which will be in
 * r1 .. r3.  On entry r0 contains the current method pointer,
 * and as part of the load sequence, it must be replaced with
 * the target method pointer.  Note, this may also be called
 * for "range" variants if the number of arguments is 5 or fewer.
 */
STATIC int genDalvikArgsNoRange(CompilationUnit* cUnit, MIR* mir,
                                DecodedInstruction* dInsn, int callState,
                                ArmLIR** pcrLabel, NextCallInsn nextCallInsn,
                                uint32_t dexIdx, uint32_t methodIdx,
                                bool skipThis)
{
    RegLocation rlArg;

    /* If no arguments, just return */
    if (dInsn->vA == 0)
        return callState;

    callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx);

    DCHECK_LE(dInsn->vA, 5U);
    if (dInsn->vA > 3) {
        uint32_t nextUse = 3;
        //Detect special case of wide arg spanning arg3/arg4
        RegLocation rlUse0 = oatGetRawSrc(cUnit, mir, 0);
        RegLocation rlUse1 = oatGetRawSrc(cUnit, mir, 1);
        RegLocation rlUse2 = oatGetRawSrc(cUnit, mir, 2);
        if (((!rlUse0.wide && !rlUse1.wide) || rlUse0.wide) &&
            rlUse2.wide) {
            int reg;
            // Wide spans, we need the 2nd half of uses[2].
            rlArg = oatUpdateLocWide(cUnit, rlUse2);
            if (rlArg.location == kLocPhysReg) {
                reg = rlArg.highReg;
            } else {
                // r2 & r3 can safely be used here
                reg = r3;
                loadWordDisp(cUnit, rSP,
                             oatSRegOffset(cUnit, rlArg.sRegLow) + 4, reg);
                callState = nextCallInsn(cUnit, mir, callState, dexIdx,
                                         methodIdx);
            }
            storeBaseDisp(cUnit, rSP, (nextUse + 1) * 4, reg, kWord);
            storeBaseDisp(cUnit, rSP, 16 /* (3+1)*4 */, reg, kWord);
            callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx);
            nextUse++;
        }
        // Loop through the rest
        while (nextUse < dInsn->vA) {
            int lowReg;
            int highReg;
            rlArg = oatGetRawSrc(cUnit, mir, nextUse);
            rlArg = oatUpdateRawLoc(cUnit, rlArg);
            if (rlArg.location == kLocPhysReg) {
                lowReg = rlArg.lowReg;
                highReg = rlArg.highReg;
            } else {
                lowReg = r2;
                highReg = r3;
                if (rlArg.wide) {
                    loadValueDirectWideFixed(cUnit, rlArg, lowReg, highReg);
                } else {
                    loadValueDirectFixed(cUnit, rlArg, lowReg);
                }
                callState = nextCallInsn(cUnit, mir, callState, dexIdx,
                                         methodIdx);
            }
            int outsOffset = (nextUse + 1) * 4;
            if (rlArg.wide) {
                storeBaseDispWide(cUnit, rSP, outsOffset, lowReg, highReg);
                nextUse += 2;
            } else {
                storeWordDisp(cUnit, rSP, outsOffset, lowReg);
                nextUse++;
            }
            callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx);
        }
    }

    callState = loadArgRegs(cUnit, mir, dInsn, callState, nextCallInsn,
                            dexIdx, methodIdx, skipThis);

    if (pcrLabel) {
        *pcrLabel = genNullCheck(cUnit, oatSSASrc(mir,0), r1, mir);
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
 *       Pass arg0, arg1 & arg2 in r1-r3
 *    If 20+ arguments
 *       Pass args arg19+ using memcpy block copy
 *       Pass arg0, arg1 & arg2 in r1-r3
 *
 */
STATIC int genDalvikArgsRange(CompilationUnit* cUnit, MIR* mir,
                              DecodedInstruction* dInsn, int callState,
                              ArmLIR** pcrLabel, NextCallInsn nextCallInsn,
                              uint32_t dexIdx, uint32_t methodIdx,
                              bool skipThis)
{
    int firstArg = dInsn->vC;
    int numArgs = dInsn->vA;

    // If we can treat it as non-range (Jumbo ops will use range form)
    if (numArgs <= 5)
        return genDalvikArgsNoRange(cUnit, mir, dInsn, callState, pcrLabel,
                                    nextCallInsn, dexIdx, methodIdx,
                                    skipThis);
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
    if (numArgs >= 20) {
        // Generate memcpy
        opRegRegImm(cUnit, kOpAdd, r0, rSP, outsOffset);
        opRegRegImm(cUnit, kOpAdd, r1, rSP, startOffset);
        loadWordDisp(cUnit, rSELF, OFFSETOF_MEMBER(Thread, pMemcpy), rLR);
        loadConstant(cUnit, r2, (numArgs - 3) * 4);
        callRuntimeHelper(cUnit, rLR);
        // Restore Method*
        loadCurrMethodDirect(cUnit, r0);
    } else {
        // Use vldm/vstm pair using r3 as a temp
        int regsLeft = std::min(numArgs - 3, 16);
        callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx);
        opRegRegImm(cUnit, kOpAdd, r3, rSP, startOffset);
        ArmLIR* ld = newLIR3(cUnit, kThumb2Vldms, r3, fr0, regsLeft);
        //TUNING: loosen barrier
        ld->defMask = ENCODE_ALL;
        setMemRefType(ld, true /* isLoad */, kDalvikReg);
        callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx);
        opRegRegImm(cUnit, kOpAdd, r3, rSP, 4 /* Method* */ + (3 * 4));
        callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx);
        ArmLIR* st = newLIR3(cUnit, kThumb2Vstms, r3, fr0, regsLeft);
        setMemRefType(st, false /* isLoad */, kDalvikReg);
        st->defMask = ENCODE_ALL;
        callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx);
    }

    callState = loadArgRegs(cUnit, mir, dInsn, callState, nextCallInsn,
                            dexIdx, methodIdx, skipThis);

    callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx);
    if (pcrLabel) {
        *pcrLabel = genNullCheck(cUnit, oatSSASrc(mir,0), r1, mir);
    }
    return callState;
}

// Debugging routine - if null target, branch to DebugMe
STATIC void genShowTarget(CompilationUnit* cUnit)
{
    ArmLIR* branchOver = genCmpImmBranch(cUnit, kArmCondNe, rLR, 0);
    loadWordDisp(cUnit, rSELF,
                 OFFSETOF_MEMBER(Thread, pDebugMe), rLR);
    ArmLIR* target = newLIR0(cUnit, kArmPseudoTargetLabel);
    target->defMask = -1;
    branchOver->generic.target = (LIR*)target;
}

STATIC void genInvoke(CompilationUnit* cUnit, MIR* mir,
                      InvokeType type, bool isRange)
{
    DecodedInstruction* dInsn = &mir->dalvikInsn;
    int callState = 0;
    ArmLIR* nullCk;
    ArmLIR** pNullCk = NULL;
    NextCallInsn nextCallInsn;
    oatFlushAllRegs(cUnit);    /* Everything to home location */
    // Explicit register usage
    oatLockCallTemps(cUnit);

    uint32_t dexMethodIdx = dInsn->vB;
    int vtableIdx;
    bool skipThis;
    bool fastPath =
        cUnit->compiler->ComputeInvokeInfo(dexMethodIdx, cUnit, type,
                                           vtableIdx)
        && !SLOW_INVOKE_PATH;
    if (type == kInterface) {
      nextCallInsn = fastPath ? nextInterfaceCallInsn
                              : nextInterfaceCallInsnWithAccessCheck;
      skipThis = false;
    } else if (type == kDirect) {
      if (fastPath) {
        pNullCk = &nullCk;
      }
      nextCallInsn = fastPath ? nextSDCallInsn : nextDirectCallInsnSP;
      skipThis = false;
    } else if (type == kStatic) {
      nextCallInsn = fastPath ? nextSDCallInsn : nextStaticCallInsnSP;
      skipThis = false;
    } else if (type == kSuper) {
      nextCallInsn = fastPath ? nextSuperCallInsn : nextSuperCallInsnSP;
      skipThis = fastPath;
    } else {
      DCHECK_EQ(type, kVirtual);
      nextCallInsn = fastPath ? nextVCallInsn : nextVCallInsnSP;
      skipThis = fastPath;
    }
    if (!isRange) {
        callState = genDalvikArgsNoRange(cUnit, mir, dInsn, callState, pNullCk,
                                         nextCallInsn, dexMethodIdx,
                                         vtableIdx, skipThis);
    } else {
        callState = genDalvikArgsRange(cUnit, mir, dInsn, callState, pNullCk,
                                       nextCallInsn, dexMethodIdx, vtableIdx,
                                       skipThis);
    }
    // Finish up any of the call sequence not interleaved in arg loading
    while (callState >= 0) {
        callState = nextCallInsn(cUnit, mir, callState, dexMethodIdx,
                                 vtableIdx);
    }
    if (DISPLAY_MISSING_TARGETS) {
        genShowTarget(cUnit);
    }
    opReg(cUnit, kOpBlx, rLR);
    oatClobberCalleeSave(cUnit);
}

STATIC bool compileDalvikInstruction(CompilationUnit* cUnit, MIR* mir,
                                     BasicBlock* bb, ArmLIR* labelList)
{
    bool res = false;   // Assume success
    RegLocation rlSrc[3];
    RegLocation rlDest = badLoc;
    RegLocation rlResult = badLoc;
    Opcode opcode = mir->dalvikInsn.opcode;

    /* Prep Src and Dest locations */
    int nextSreg = 0;
    int nextLoc = 0;
    int attrs = oatDataFlowAttributes[opcode];
    rlSrc[0] = rlSrc[1] = rlSrc[2] = badLoc;
    if (attrs & DF_UA) {
        rlSrc[nextLoc++] = oatGetSrc(cUnit, mir, nextSreg);
        nextSreg++;
    } else if (attrs & DF_UA_WIDE) {
        rlSrc[nextLoc++] = oatGetSrcWide(cUnit, mir, nextSreg,
                                                 nextSreg + 1);
        nextSreg+= 2;
    }
    if (attrs & DF_UB) {
        rlSrc[nextLoc++] = oatGetSrc(cUnit, mir, nextSreg);
        nextSreg++;
    } else if (attrs & DF_UB_WIDE) {
        rlSrc[nextLoc++] = oatGetSrcWide(cUnit, mir, nextSreg,
                                                 nextSreg + 1);
        nextSreg+= 2;
    }
    if (attrs & DF_UC) {
        rlSrc[nextLoc++] = oatGetSrc(cUnit, mir, nextSreg);
    } else if (attrs & DF_UC_WIDE) {
        rlSrc[nextLoc++] = oatGetSrcWide(cUnit, mir, nextSreg,
                                                 nextSreg + 1);
    }
    if (attrs & DF_DA) {
        rlDest = oatGetDest(cUnit, mir, 0);
    } else if (attrs & DF_DA_WIDE) {
        rlDest = oatGetDestWide(cUnit, mir, 0, 1);
    }

    switch(opcode) {
        case OP_NOP:
            break;

        case OP_MOVE_EXCEPTION:
            int exOffset;
            int resetReg;
            exOffset = Thread::ExceptionOffset().Int32Value();
            resetReg = oatAllocTemp(cUnit);
            rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
            loadWordDisp(cUnit, rSELF, exOffset, rlResult.lowReg);
            loadConstant(cUnit, resetReg, 0);
            storeWordDisp(cUnit, rSELF, exOffset, resetReg);
            storeValue(cUnit, rlDest, rlResult);
            break;

        case OP_RETURN_VOID:
            genSuspendTest(cUnit, mir);
            break;

        case OP_RETURN:
        case OP_RETURN_OBJECT:
            genSuspendTest(cUnit, mir);
            storeValue(cUnit, getRetLoc(cUnit), rlSrc[0]);
            break;

        case OP_RETURN_WIDE:
            genSuspendTest(cUnit, mir);
            storeValueWide(cUnit, getRetLocWide(cUnit), rlSrc[0]);
            break;

        case OP_MOVE_RESULT_WIDE:
            if (mir->optimizationFlags & MIR_INLINED)
                break;  // Nop - combined w/ previous invoke
            storeValueWide(cUnit, rlDest, getRetLocWide(cUnit));
            break;

        case OP_MOVE_RESULT:
        case OP_MOVE_RESULT_OBJECT:
            if (mir->optimizationFlags & MIR_INLINED)
                break;  // Nop - combined w/ previous invoke
            storeValue(cUnit, rlDest, getRetLoc(cUnit));
            break;

        case OP_MOVE:
        case OP_MOVE_OBJECT:
        case OP_MOVE_16:
        case OP_MOVE_OBJECT_16:
        case OP_MOVE_FROM16:
        case OP_MOVE_OBJECT_FROM16:
            storeValue(cUnit, rlDest, rlSrc[0]);
            break;

        case OP_MOVE_WIDE:
        case OP_MOVE_WIDE_16:
        case OP_MOVE_WIDE_FROM16:
            storeValueWide(cUnit, rlDest, rlSrc[0]);
            break;

        case OP_CONST:
        case OP_CONST_4:
        case OP_CONST_16:
            rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
            loadConstantNoClobber(cUnit, rlResult.lowReg, mir->dalvikInsn.vB);
            storeValue(cUnit, rlDest, rlResult);
            break;

        case OP_CONST_HIGH16:
            rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
            loadConstantNoClobber(cUnit, rlResult.lowReg,
                                  mir->dalvikInsn.vB << 16);
            storeValue(cUnit, rlDest, rlResult);
            break;

        case OP_CONST_WIDE_16:
        case OP_CONST_WIDE_32:
            rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
            loadConstantValueWide(cUnit, rlResult.lowReg, rlResult.highReg,
                                  mir->dalvikInsn.vB,
                                  (mir->dalvikInsn.vB & 0x80000000) ? -1 : 0);
            storeValueWide(cUnit, rlDest, rlResult);
            break;

        case OP_CONST_WIDE:
            rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
            loadConstantValueWide(cUnit, rlResult.lowReg, rlResult.highReg,
                          mir->dalvikInsn.vB_wide & 0xffffffff,
                          (mir->dalvikInsn.vB_wide >> 32) & 0xffffffff);
            storeValueWide(cUnit, rlDest, rlResult);
            break;

        case OP_CONST_WIDE_HIGH16:
            rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
            loadConstantValueWide(cUnit, rlResult.lowReg, rlResult.highReg,
                                  0, mir->dalvikInsn.vB << 16);
            storeValueWide(cUnit, rlDest, rlResult);
            break;

        case OP_MONITOR_ENTER:
            genMonitorEnter(cUnit, mir, rlSrc[0]);
            break;

        case OP_MONITOR_EXIT:
            genMonitorExit(cUnit, mir, rlSrc[0]);
            break;

        case OP_CHECK_CAST:
            genCheckCast(cUnit, mir, rlSrc[0]);
            break;

        case OP_INSTANCE_OF:
            genInstanceof(cUnit, mir, rlDest, rlSrc[0]);
            break;

        case OP_NEW_INSTANCE:
            genNewInstance(cUnit, mir, rlDest);
            break;

        case OP_THROW:
            genThrow(cUnit, mir, rlSrc[0]);
            break;

        case OP_THROW_VERIFICATION_ERROR:
            loadWordDisp(cUnit, rSELF,
                OFFSETOF_MEMBER(Thread, pThrowVerificationErrorFromCode), rLR);
            loadConstant(cUnit, r0, mir->dalvikInsn.vA);
            loadConstant(cUnit, r1, mir->dalvikInsn.vB);
            callRuntimeHelper(cUnit, rLR);
            break;

        case OP_ARRAY_LENGTH:
            int lenOffset;
            lenOffset = Array::LengthOffset().Int32Value();
            rlSrc[0] = loadValue(cUnit, rlSrc[0], kCoreReg);
            genNullCheck(cUnit, rlSrc[0].sRegLow, rlSrc[0].lowReg, mir);
            rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
            loadWordDisp(cUnit, rlSrc[0].lowReg, lenOffset,
                         rlResult.lowReg);
            storeValue(cUnit, rlDest, rlResult);
            break;

        case OP_CONST_STRING:
        case OP_CONST_STRING_JUMBO:
            genConstString(cUnit, mir, rlDest, rlSrc[0]);
            break;

        case OP_CONST_CLASS:
            genConstClass(cUnit, mir, rlDest, rlSrc[0]);
            break;

        case OP_FILL_ARRAY_DATA:
            genFillArrayData(cUnit, mir, rlSrc[0]);
            break;

        case OP_FILLED_NEW_ARRAY:
            genFilledNewArray(cUnit, mir, false /* not range */);
            break;

        case OP_FILLED_NEW_ARRAY_RANGE:
            genFilledNewArray(cUnit, mir, true /* range */);
            break;

        case OP_NEW_ARRAY:
            genNewArray(cUnit, mir, rlDest, rlSrc[0]);
            break;

        case OP_GOTO:
        case OP_GOTO_16:
        case OP_GOTO_32:
            if (bb->taken->startOffset <= mir->offset) {
                genSuspendTest(cUnit, mir);
            }
            genUnconditionalBranch(cUnit, &labelList[bb->taken->id]);
            break;

        case OP_PACKED_SWITCH:
            genPackedSwitch(cUnit, mir, rlSrc[0]);
            break;

        case OP_SPARSE_SWITCH:
            genSparseSwitch(cUnit, mir, rlSrc[0]);
            break;

        case OP_CMPL_FLOAT:
        case OP_CMPG_FLOAT:
        case OP_CMPL_DOUBLE:
        case OP_CMPG_DOUBLE:
            res = genCmpFP(cUnit, mir, rlDest, rlSrc[0], rlSrc[1]);
            break;

        case OP_CMP_LONG:
            genCmpLong(cUnit, mir, rlDest, rlSrc[0], rlSrc[1]);
            break;

        case OP_IF_EQ:
        case OP_IF_NE:
        case OP_IF_LT:
        case OP_IF_GE:
        case OP_IF_GT:
        case OP_IF_LE: {
            bool backwardBranch;
            ArmConditionCode cond;
            backwardBranch = (bb->taken->startOffset <= mir->offset);
            if (backwardBranch) {
                genSuspendTest(cUnit, mir);
            }
            rlSrc[0] = loadValue(cUnit, rlSrc[0], kCoreReg);
            rlSrc[1] = loadValue(cUnit, rlSrc[1], kCoreReg);
            opRegReg(cUnit, kOpCmp, rlSrc[0].lowReg, rlSrc[1].lowReg);
            switch(opcode) {
                case OP_IF_EQ:
                    cond = kArmCondEq;
                    break;
                case OP_IF_NE:
                    cond = kArmCondNe;
                    break;
                case OP_IF_LT:
                    cond = kArmCondLt;
                    break;
                case OP_IF_GE:
                    cond = kArmCondGe;
                    break;
                case OP_IF_GT:
                    cond = kArmCondGt;
                    break;
                case OP_IF_LE:
                    cond = kArmCondLe;
                    break;
                default:
                    cond = (ArmConditionCode)0;
                    LOG(FATAL) << "Unexpected opcode " << (int)opcode;
            }
            genConditionalBranch(cUnit, cond, &labelList[bb->taken->id]);
            genUnconditionalBranch(cUnit, &labelList[bb->fallThrough->id]);
            break;
            }

        case OP_IF_EQZ:
        case OP_IF_NEZ:
        case OP_IF_LTZ:
        case OP_IF_GEZ:
        case OP_IF_GTZ:
        case OP_IF_LEZ: {
            bool backwardBranch;
            ArmConditionCode cond;
            backwardBranch = (bb->taken->startOffset <= mir->offset);
            if (backwardBranch) {
                genSuspendTest(cUnit, mir);
            }
            rlSrc[0] = loadValue(cUnit, rlSrc[0], kCoreReg);
            opRegImm(cUnit, kOpCmp, rlSrc[0].lowReg, 0);
            switch(opcode) {
                case OP_IF_EQZ:
                    cond = kArmCondEq;
                    break;
                case OP_IF_NEZ:
                    cond = kArmCondNe;
                    break;
                case OP_IF_LTZ:
                    cond = kArmCondLt;
                    break;
                case OP_IF_GEZ:
                    cond = kArmCondGe;
                    break;
                case OP_IF_GTZ:
                    cond = kArmCondGt;
                    break;
                case OP_IF_LEZ:
                    cond = kArmCondLe;
                    break;
                default:
                    cond = (ArmConditionCode)0;
                    LOG(FATAL) << "Unexpected opcode " << (int)opcode;
            }
            genConditionalBranch(cUnit, cond, &labelList[bb->taken->id]);
            genUnconditionalBranch(cUnit, &labelList[bb->fallThrough->id]);
            break;
            }

      case OP_AGET_WIDE:
            genArrayGet(cUnit, mir, kLong, rlSrc[0], rlSrc[1], rlDest, 3);
            break;
        case OP_AGET:
        case OP_AGET_OBJECT:
            genArrayGet(cUnit, mir, kWord, rlSrc[0], rlSrc[1], rlDest, 2);
            break;
        case OP_AGET_BOOLEAN:
            genArrayGet(cUnit, mir, kUnsignedByte, rlSrc[0], rlSrc[1],
                        rlDest, 0);
            break;
        case OP_AGET_BYTE:
            genArrayGet(cUnit, mir, kSignedByte, rlSrc[0], rlSrc[1], rlDest, 0);
            break;
        case OP_AGET_CHAR:
            genArrayGet(cUnit, mir, kUnsignedHalf, rlSrc[0], rlSrc[1],
                        rlDest, 1);
            break;
        case OP_AGET_SHORT:
            genArrayGet(cUnit, mir, kSignedHalf, rlSrc[0], rlSrc[1], rlDest, 1);
            break;
        case OP_APUT_WIDE:
            genArrayPut(cUnit, mir, kLong, rlSrc[1], rlSrc[2], rlSrc[0], 3);
            break;
        case OP_APUT:
            genArrayPut(cUnit, mir, kWord, rlSrc[1], rlSrc[2], rlSrc[0], 2);
            break;
        case OP_APUT_OBJECT:
            genArrayObjPut(cUnit, mir, rlSrc[1], rlSrc[2], rlSrc[0], 2);
            break;
        case OP_APUT_SHORT:
        case OP_APUT_CHAR:
            genArrayPut(cUnit, mir, kUnsignedHalf, rlSrc[1], rlSrc[2],
                        rlSrc[0], 1);
            break;
        case OP_APUT_BYTE:
        case OP_APUT_BOOLEAN:
            genArrayPut(cUnit, mir, kUnsignedByte, rlSrc[1], rlSrc[2],
                        rlSrc[0], 0);
            break;

        case OP_IGET_OBJECT:
        case OP_IGET_OBJECT_VOLATILE:
            genIGet(cUnit, mir, kWord, rlDest, rlSrc[0], false, true);
            break;

        case OP_IGET_WIDE:
        case OP_IGET_WIDE_VOLATILE:
            genIGet(cUnit, mir, kLong, rlDest, rlSrc[0], true, false);
            break;

        case OP_IGET:
        case OP_IGET_VOLATILE:
            genIGet(cUnit, mir, kWord, rlDest, rlSrc[0], false, false);
            break;

        case OP_IGET_CHAR:
            genIGet(cUnit, mir, kUnsignedHalf, rlDest, rlSrc[0], false, false);
            break;

        case OP_IGET_SHORT:
            genIGet(cUnit, mir, kSignedHalf, rlDest, rlSrc[0], false, false);
            break;

        case OP_IGET_BOOLEAN:
        case OP_IGET_BYTE:
            genIGet(cUnit, mir, kUnsignedByte, rlDest, rlSrc[0], false, false);
            break;

        case OP_IPUT_WIDE:
        case OP_IPUT_WIDE_VOLATILE:
            genIPut(cUnit, mir, kLong, rlSrc[0], rlSrc[1], true, false);
            break;

        case OP_IPUT_OBJECT:
        case OP_IPUT_OBJECT_VOLATILE:
            genIPut(cUnit, mir, kWord, rlSrc[0], rlSrc[1], false, true);
            break;

        case OP_IPUT:
        case OP_IPUT_VOLATILE:
            genIPut(cUnit, mir, kWord, rlSrc[0], rlSrc[1], false, false);
            break;

        case OP_IPUT_BOOLEAN:
        case OP_IPUT_BYTE:
            genIPut(cUnit, mir, kUnsignedByte, rlSrc[0], rlSrc[1], false, false);
            break;

        case OP_IPUT_CHAR:
            genIPut(cUnit, mir, kUnsignedHalf, rlSrc[0], rlSrc[1], false, false);
            break;

        case OP_IPUT_SHORT:
            genIPut(cUnit, mir, kSignedHalf, rlSrc[0], rlSrc[1], false, false);
            break;

        case OP_SGET_OBJECT:
          genSget(cUnit, mir, rlDest, false, true);
          break;
        case OP_SGET:
        case OP_SGET_BOOLEAN:
        case OP_SGET_BYTE:
        case OP_SGET_CHAR:
        case OP_SGET_SHORT:
            genSget(cUnit, mir, rlDest, false, false);
            break;

        case OP_SGET_WIDE:
            genSget(cUnit, mir, rlDest, true, false);
            break;

        case OP_SPUT_OBJECT:
          genSput(cUnit, mir, rlSrc[0], false, true);
          break;

        case OP_SPUT:
        case OP_SPUT_BOOLEAN:
        case OP_SPUT_BYTE:
        case OP_SPUT_CHAR:
        case OP_SPUT_SHORT:
            genSput(cUnit, mir, rlSrc[0], false, false);
            break;

        case OP_SPUT_WIDE:
            genSput(cUnit, mir, rlSrc[0], true, false);
            break;

        case OP_INVOKE_STATIC_RANGE:
            genInvoke(cUnit, mir, kStatic, true /*range*/);
            break;
        case OP_INVOKE_STATIC:
            genInvoke(cUnit, mir, kStatic, false /*range*/);
            break;

        case OP_INVOKE_DIRECT:
            genInvoke(cUnit, mir, kDirect, false /*range*/);
            break;
        case OP_INVOKE_DIRECT_RANGE:
            genInvoke(cUnit, mir, kDirect, true /*range*/);
            break;

        case OP_INVOKE_VIRTUAL:
            genInvoke(cUnit, mir, kVirtual, false /*range*/);
            break;
        case OP_INVOKE_VIRTUAL_RANGE:
            genInvoke(cUnit, mir, kVirtual, true /*range*/);
            break;

        case OP_INVOKE_SUPER:
            genInvoke(cUnit, mir, kSuper, false /*range*/);
            break;
        case OP_INVOKE_SUPER_RANGE:
            genInvoke(cUnit, mir, kSuper, true /*range*/);
            break;

        case OP_INVOKE_INTERFACE:
            genInvoke(cUnit, mir, kInterface, false /*range*/);
            break;
        case OP_INVOKE_INTERFACE_RANGE:
            genInvoke(cUnit, mir, kInterface, true /*range*/);
            break;

        case OP_NEG_INT:
        case OP_NOT_INT:
            res = genArithOpInt(cUnit, mir, rlDest, rlSrc[0], rlSrc[0]);
            break;

        case OP_NEG_LONG:
        case OP_NOT_LONG:
            res = genArithOpLong(cUnit, mir, rlDest, rlSrc[0], rlSrc[0]);
            break;

        case OP_NEG_FLOAT:
            res = genArithOpFloat(cUnit, mir, rlDest, rlSrc[0], rlSrc[0]);
            break;

        case OP_NEG_DOUBLE:
            res = genArithOpDouble(cUnit, mir, rlDest, rlSrc[0], rlSrc[0]);
            break;

        case OP_INT_TO_LONG:
            rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
            if (rlSrc[0].location == kLocPhysReg) {
                genRegCopy(cUnit, rlResult.lowReg, rlSrc[0].lowReg);
            } else {
                loadValueDirect(cUnit, rlSrc[0], rlResult.lowReg);
            }
            opRegRegImm(cUnit, kOpAsr, rlResult.highReg,
                        rlResult.lowReg, 31);
            storeValueWide(cUnit, rlDest, rlResult);
            break;

        case OP_LONG_TO_INT:
            rlSrc[0] = oatUpdateLocWide(cUnit, rlSrc[0]);
            rlSrc[0] = oatWideToNarrow(cUnit, rlSrc[0]);
            storeValue(cUnit, rlDest, rlSrc[0]);
            break;

        case OP_INT_TO_BYTE:
            rlSrc[0] = loadValue(cUnit, rlSrc[0], kCoreReg);
            rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
            opRegReg(cUnit, kOp2Byte, rlResult.lowReg, rlSrc[0].lowReg);
            storeValue(cUnit, rlDest, rlResult);
            break;

        case OP_INT_TO_SHORT:
            rlSrc[0] = loadValue(cUnit, rlSrc[0], kCoreReg);
            rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
            opRegReg(cUnit, kOp2Short, rlResult.lowReg, rlSrc[0].lowReg);
            storeValue(cUnit, rlDest, rlResult);
            break;

        case OP_INT_TO_CHAR:
            rlSrc[0] = loadValue(cUnit, rlSrc[0], kCoreReg);
            rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
            opRegReg(cUnit, kOp2Char, rlResult.lowReg, rlSrc[0].lowReg);
            storeValue(cUnit, rlDest, rlResult);
            break;

        case OP_INT_TO_FLOAT:
        case OP_INT_TO_DOUBLE:
        case OP_LONG_TO_FLOAT:
        case OP_LONG_TO_DOUBLE:
        case OP_FLOAT_TO_INT:
        case OP_FLOAT_TO_LONG:
        case OP_FLOAT_TO_DOUBLE:
        case OP_DOUBLE_TO_INT:
        case OP_DOUBLE_TO_LONG:
        case OP_DOUBLE_TO_FLOAT:
            genConversion(cUnit, mir);
            break;

        case OP_ADD_INT:
        case OP_SUB_INT:
        case OP_MUL_INT:
        case OP_DIV_INT:
        case OP_REM_INT:
        case OP_AND_INT:
        case OP_OR_INT:
        case OP_XOR_INT:
        case OP_SHL_INT:
        case OP_SHR_INT:
        case OP_USHR_INT:
        case OP_ADD_INT_2ADDR:
        case OP_SUB_INT_2ADDR:
        case OP_MUL_INT_2ADDR:
        case OP_DIV_INT_2ADDR:
        case OP_REM_INT_2ADDR:
        case OP_AND_INT_2ADDR:
        case OP_OR_INT_2ADDR:
        case OP_XOR_INT_2ADDR:
        case OP_SHL_INT_2ADDR:
        case OP_SHR_INT_2ADDR:
        case OP_USHR_INT_2ADDR:
            genArithOpInt(cUnit, mir, rlDest, rlSrc[0], rlSrc[1]);
            break;

        case OP_ADD_LONG:
        case OP_SUB_LONG:
        case OP_MUL_LONG:
        case OP_DIV_LONG:
        case OP_REM_LONG:
        case OP_AND_LONG:
        case OP_OR_LONG:
        case OP_XOR_LONG:
        case OP_ADD_LONG_2ADDR:
        case OP_SUB_LONG_2ADDR:
        case OP_MUL_LONG_2ADDR:
        case OP_DIV_LONG_2ADDR:
        case OP_REM_LONG_2ADDR:
        case OP_AND_LONG_2ADDR:
        case OP_OR_LONG_2ADDR:
        case OP_XOR_LONG_2ADDR:
            genArithOpLong(cUnit, mir, rlDest, rlSrc[0], rlSrc[1]);
            break;

        case OP_SHL_LONG:
        case OP_SHR_LONG:
        case OP_USHR_LONG:
        case OP_SHL_LONG_2ADDR:
        case OP_SHR_LONG_2ADDR:
        case OP_USHR_LONG_2ADDR:
            genShiftOpLong(cUnit,mir, rlDest, rlSrc[0], rlSrc[1]);
            break;

        case OP_ADD_FLOAT:
        case OP_SUB_FLOAT:
        case OP_MUL_FLOAT:
        case OP_DIV_FLOAT:
        case OP_REM_FLOAT:
        case OP_ADD_FLOAT_2ADDR:
        case OP_SUB_FLOAT_2ADDR:
        case OP_MUL_FLOAT_2ADDR:
        case OP_DIV_FLOAT_2ADDR:
        case OP_REM_FLOAT_2ADDR:
            genArithOpFloat(cUnit, mir, rlDest, rlSrc[0], rlSrc[1]);
            break;

        case OP_ADD_DOUBLE:
        case OP_SUB_DOUBLE:
        case OP_MUL_DOUBLE:
        case OP_DIV_DOUBLE:
        case OP_REM_DOUBLE:
        case OP_ADD_DOUBLE_2ADDR:
        case OP_SUB_DOUBLE_2ADDR:
        case OP_MUL_DOUBLE_2ADDR:
        case OP_DIV_DOUBLE_2ADDR:
        case OP_REM_DOUBLE_2ADDR:
            genArithOpDouble(cUnit, mir, rlDest, rlSrc[0], rlSrc[1]);
            break;

        case OP_RSUB_INT:
        case OP_ADD_INT_LIT16:
        case OP_MUL_INT_LIT16:
        case OP_DIV_INT_LIT16:
        case OP_REM_INT_LIT16:
        case OP_AND_INT_LIT16:
        case OP_OR_INT_LIT16:
        case OP_XOR_INT_LIT16:
        case OP_ADD_INT_LIT8:
        case OP_RSUB_INT_LIT8:
        case OP_MUL_INT_LIT8:
        case OP_DIV_INT_LIT8:
        case OP_REM_INT_LIT8:
        case OP_AND_INT_LIT8:
        case OP_OR_INT_LIT8:
        case OP_XOR_INT_LIT8:
        case OP_SHL_INT_LIT8:
        case OP_SHR_INT_LIT8:
        case OP_USHR_INT_LIT8:
            genArithOpIntLit(cUnit, mir, rlDest, rlSrc[0], mir->dalvikInsn.vC);
            break;

        default:
            res = true;
    }
    return res;
}

STATIC const char* extendedMIROpNames[kMirOpLast - kMirOpFirst] = {
    "kMirOpPhi",
    "kMirOpNullNRangeUpCheck",
    "kMirOpNullNRangeDownCheck",
    "kMirOpLowerBound",
    "kMirOpPunt",
    "kMirOpCheckInlinePrediction",
};

/* Extended MIR instructions like PHI */
STATIC void handleExtendedMethodMIR(CompilationUnit* cUnit, MIR* mir)
{
    int opOffset = mir->dalvikInsn.opcode - kMirOpFirst;
    char* msg = NULL;
    if (cUnit->printMe) {
        msg = (char*)oatNew(cUnit, strlen(extendedMIROpNames[opOffset]) + 1,
                            false, kAllocDebugInfo);
        strcpy(msg, extendedMIROpNames[opOffset]);
    }
    ArmLIR* op = newLIR1(cUnit, kArmPseudoExtended, (int) msg);

    switch ((ExtendedMIROpcode)mir->dalvikInsn.opcode) {
        case kMirOpPhi: {
            char* ssaString = NULL;
            if (cUnit->printMe) {
                ssaString = oatGetSSAString(cUnit, mir->ssaRep);
            }
            op->flags.isNop = true;
            newLIR1(cUnit, kArmPseudoSSARep, (int) ssaString);
            break;
        }
        default:
            break;
    }
}

/*
 * If there are any ins passed in registers that have not been promoted
 * to a callee-save register, flush them to the frame.  Perform intial
 * assignment of promoted arguments.
 */
STATIC void flushIns(CompilationUnit* cUnit)
{
    if (cUnit->numIns == 0)
        return;
    int firstArgReg = r1;
    int lastArgReg = r3;
    int startVReg = cUnit->numDalvikRegisters - cUnit->numIns;
    /*
     * Arguments passed in registers should be flushed
     * to their backing locations in the frame for now.
     * Also, we need to do initial assignment for promoted
     * arguments.  NOTE: an older version of dx had an issue
     * in which it would reuse static method argument registers.
     * This could result in the same Dalvik virtual register
     * being promoted to both core and fp regs.  In those
     * cases, copy argument to both.  This will be uncommon
     * enough that it isn't worth attempting to optimize.
     */
    for (int i = 0; i < cUnit->numIns; i++) {
        PromotionMap vMap = cUnit->promotionMap[startVReg + i];
        if (i <= (lastArgReg - firstArgReg)) {
            // If arriving in register
            if (vMap.coreLocation == kLocPhysReg) {
                genRegCopy(cUnit, vMap.coreReg, firstArgReg + i);
            }
            if (vMap.fpLocation == kLocPhysReg) {
                genRegCopy(cUnit, vMap.fpReg, firstArgReg + i);
            }
            // Also put a copy in memory in case we're partially promoted
            storeBaseDisp(cUnit, rSP, oatSRegOffset(cUnit, startVReg + i),
                          firstArgReg + i, kWord);
        } else {
            // If arriving in frame & promoted
            if (vMap.coreLocation == kLocPhysReg) {
                loadWordDisp(cUnit, rSP, oatSRegOffset(cUnit, startVReg + i),
                             vMap.coreReg);
            }
            if (vMap.fpLocation == kLocPhysReg) {
                loadWordDisp(cUnit, rSP, oatSRegOffset(cUnit, startVReg + i),
                             vMap.fpReg);
            }
        }
    }
}

/* Handle the content in each basic block */
STATIC bool methodBlockCodeGen(CompilationUnit* cUnit, BasicBlock* bb)
{
    MIR* mir;
    ArmLIR* labelList = (ArmLIR*) cUnit->blockLabelList;
    int blockId = bb->id;

    cUnit->curBlock = bb;
    labelList[blockId].operands[0] = bb->startOffset;

    /* Insert the block label */
    labelList[blockId].opcode = kArmPseudoNormalBlockLabel;
    oatAppendLIR(cUnit, (LIR*) &labelList[blockId]);

    /* Reset local optimization data on block boundaries */
    oatResetRegPool(cUnit);
    oatClobberAllRegs(cUnit);
    oatResetDefTracking(cUnit);

    ArmLIR* headLIR = NULL;

    int spillCount = cUnit->numCoreSpills + cUnit->numFPSpills;
    if (bb->blockType == kEntryBlock) {
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
        newLIR0(cUnit, kArmPseudoMethodEntry);
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
            genRegRegCheck(cUnit, kArmCondCc, rLR, r12, NULL,
                           kArmThrowStackOverflow);
            genRegCopy(cUnit, rSP, rLR);         // Establish stack
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
    } else if (bb->blockType == kExitBlock) {
        /*
         * In the exit path, r0/r1 are live - make sure they aren't
         * allocated by the register utilities as temps.
         */
        oatLockTemp(cUnit, r0);
        oatLockTemp(cUnit, r1);

        newLIR0(cUnit, kArmPseudoMethodExit);
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

    for (mir = bb->firstMIRInsn; mir; mir = mir->next) {

        oatResetRegPool(cUnit);
        if (cUnit->disableOpt & (1 << kTrackLiveTemps)) {
            oatClobberAllRegs(cUnit);
        }

        if (cUnit->disableOpt & (1 << kSuppressLoads)) {
            oatResetDefTracking(cUnit);
        }

        if ((int)mir->dalvikInsn.opcode >= (int)kMirOpFirst) {
            handleExtendedMethodMIR(cUnit, mir);
            continue;
        }

        cUnit->currentDalvikOffset = mir->offset;

        Opcode dalvikOpcode = mir->dalvikInsn.opcode;
        InstructionFormat dalvikFormat =
            dexGetFormatFromOpcode(dalvikOpcode);

        ArmLIR* boundaryLIR;

        /* Mark the beginning of a Dalvik instruction for line tracking */
        char* instStr = cUnit->printMe ?
           oatGetDalvikDisassembly(cUnit, &mir->dalvikInsn, "") : NULL;
        boundaryLIR = newLIR1(cUnit, kArmPseudoDalvikByteCodeBoundary,
                              (intptr_t) instStr);
        cUnit->boundaryMap.insert(std::make_pair(mir->offset,
                                 (LIR*)boundaryLIR));
        /* Remember the first LIR for this block */
        if (headLIR == NULL) {
            headLIR = boundaryLIR;
            /* Set the first boundaryLIR as a scheduling barrier */
            headLIR->defMask = ENCODE_ALL;
        }

        /* If we're compiling for the debugger, generate an update callout */
        if (cUnit->genDebugger) {
            genDebuggerUpdate(cUnit, mir->offset);
        }

        /* Don't generate the SSA annotation unless verbose mode is on */
        if (cUnit->printMe && mir->ssaRep) {
            char* ssaString = oatGetSSAString(cUnit, mir->ssaRep);
            newLIR1(cUnit, kArmPseudoSSARep, (int) ssaString);
        }

        bool notHandled = compileDalvikInstruction(cUnit, mir, bb, labelList);

        if (notHandled) {
            char buf[100];
            snprintf(buf, 100, "%#06x: Opcode %#x (%s) / Fmt %d not handled",
                 mir->offset,
                 dalvikOpcode, dexGetOpcodeName(dalvikOpcode),
                 dalvikFormat);
            LOG(FATAL) << buf;
        }
    }

    if (headLIR) {
        /*
         * Eliminate redundant loads/stores and delay stores into later
         * slots
         */
        oatApplyLocalOptimizations(cUnit, (LIR*) headLIR,
                                           cUnit->lastLIRInsn);

        /*
         * Generate an unconditional branch to the fallthrough block.
         */
        if (bb->fallThrough) {
            genUnconditionalBranch(cUnit,
                                   &labelList[bb->fallThrough->id]);
        }
    }
    return false;
}

/*
 * Nop any unconditional branches that go to the next instruction.
 * Note: new redundant branches may be inserted later, and we'll
 * use a check in final instruction assembly to nop those out.
 */
void removeRedundantBranches(CompilationUnit* cUnit)
{
    ArmLIR* thisLIR;

    for (thisLIR = (ArmLIR*) cUnit->firstLIRInsn;
         thisLIR != (ArmLIR*) cUnit->lastLIRInsn;
         thisLIR = NEXT_LIR(thisLIR)) {

        /* Branch to the next instruction */
        if ((thisLIR->opcode == kThumbBUncond) ||
            (thisLIR->opcode == kThumb2BUncond)) {
            ArmLIR* nextLIR = thisLIR;

            while (true) {
                nextLIR = NEXT_LIR(nextLIR);

                /*
                 * Is the branch target the next instruction?
                 */
                if (nextLIR == (ArmLIR*) thisLIR->generic.target) {
                    thisLIR->flags.isNop = true;
                    break;
                }

                /*
                 * Found real useful stuff between the branch and the target.
                 * Need to explicitly check the lastLIRInsn here because it
                 * might be the last real instruction.
                 */
                if (!isPseudoOpcode(nextLIR->opcode) ||
                    (nextLIR = (ArmLIR*) cUnit->lastLIRInsn))
                    break;
            }
        }
    }
}

STATIC void handleSuspendLaunchpads(CompilationUnit *cUnit)
{
    ArmLIR** suspendLabel =
        (ArmLIR **) cUnit->suspendLaunchpads.elemList;
    int numElems = cUnit->suspendLaunchpads.numUsed;

    for (int i = 0; i < numElems; i++) {
        /* TUNING: move suspend count load into helper */
        ArmLIR* lab = suspendLabel[i];
        ArmLIR* resumeLab = (ArmLIR*)lab->operands[0];
        cUnit->currentDalvikOffset = lab->operands[1];
        oatAppendLIR(cUnit, (LIR *)lab);
        loadWordDisp(cUnit, rSELF,
                     OFFSETOF_MEMBER(Thread, pTestSuspendFromCode), rLR);
        if (!cUnit->genDebugger) {
            // use rSUSPEND for suspend count
            loadWordDisp(cUnit, rSELF,
                         Thread::SuspendCountOffset().Int32Value(), rSUSPEND);
        }
        opReg(cUnit, kOpBlx, rLR);
        if ( cUnit->genDebugger) {
            // use rSUSPEND for update debugger
            loadWordDisp(cUnit, rSELF,
                         OFFSETOF_MEMBER(Thread, pUpdateDebuggerFromCode), rSUSPEND);
        }
        genUnconditionalBranch(cUnit, resumeLab);
    }
}

STATIC void handleThrowLaunchpads(CompilationUnit *cUnit)
{
    ArmLIR** throwLabel =
        (ArmLIR **) cUnit->throwLaunchpads.elemList;
    int numElems = cUnit->throwLaunchpads.numUsed;
    int i;

    for (i = 0; i < numElems; i++) {
        ArmLIR* lab = throwLabel[i];
        cUnit->currentDalvikOffset = lab->operands[1];
        oatAppendLIR(cUnit, (LIR *)lab);
        int funcOffset = 0;
        int v1 = lab->operands[2];
        int v2 = lab->operands[3];
        switch(lab->operands[0]) {
            case kArmThrowNullPointer:
                funcOffset = OFFSETOF_MEMBER(Thread, pThrowNullPointerFromCode);
                break;
            case kArmThrowArrayBounds:
                if (v2 != r0) {
                    genRegCopy(cUnit, r0, v1);
                    genRegCopy(cUnit, r1, v2);
                } else {
                    if (v1 == r1) {
                        genRegCopy(cUnit, r12, v1);
                        genRegCopy(cUnit, r1, v2);
                        genRegCopy(cUnit, r0, r12);
                    } else {
                        genRegCopy(cUnit, r1, v2);
                        genRegCopy(cUnit, r0, v1);
                    }
                }
                funcOffset = OFFSETOF_MEMBER(Thread, pThrowArrayBoundsFromCode);
                break;
            case kArmThrowDivZero:
                funcOffset = OFFSETOF_MEMBER(Thread, pThrowDivZeroFromCode);
                break;
            case kArmThrowVerificationError:
                loadConstant(cUnit, r0, v1);
                loadConstant(cUnit, r1, v2);
                funcOffset =
                    OFFSETOF_MEMBER(Thread, pThrowVerificationErrorFromCode);
                break;
            case kArmThrowNegArraySize:
                genRegCopy(cUnit, r0, v1);
                funcOffset =
                    OFFSETOF_MEMBER(Thread, pThrowNegArraySizeFromCode);
                break;
            case kArmThrowNoSuchMethod:
                genRegCopy(cUnit, r0, v1);
                funcOffset =
                    OFFSETOF_MEMBER(Thread, pThrowNoSuchMethodFromCode);
                break;
            case kArmThrowStackOverflow:
                funcOffset =
                    OFFSETOF_MEMBER(Thread, pThrowStackOverflowFromCode);
                // Restore stack alignment
                opRegImm(cUnit, kOpAdd, rSP,
                         (cUnit->numCoreSpills + cUnit->numFPSpills) * 4);
                break;
            default:
                LOG(FATAL) << "Unexpected throw kind: " << lab->operands[0];
        }
        loadWordDisp(cUnit, rSELF, funcOffset, rLR);
        callRuntimeHelper(cUnit, rLR);
    }
}

void oatMethodMIR2LIR(CompilationUnit* cUnit)
{
    /* Used to hold the labels of each block */
    cUnit->blockLabelList =
        (void *) oatNew(cUnit, sizeof(ArmLIR) * cUnit->numBlocks, true,
                        kAllocLIR);

    oatDataFlowAnalysisDispatcher(cUnit, methodBlockCodeGen,
                                  kPreOrderDFSTraversal, false /* Iterative */);
    handleSuspendLaunchpads(cUnit);

    handleThrowLaunchpads(cUnit);

    removeRedundantBranches(cUnit);
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

/* Needed by the Assembler */
void oatSetupResourceMasks(ArmLIR* lir)
{
    setupResourceMasks(lir);
}

/* Needed by the ld/st optmizatons */
ArmLIR* oatRegCopyNoInsert(CompilationUnit* cUnit, int rDest, int rSrc)
{
    return genRegCopyNoInsert(cUnit, rDest, rSrc);
}

/* Needed by the register allocator */
ArmLIR* oatRegCopy(CompilationUnit* cUnit, int rDest, int rSrc)
{
    return genRegCopy(cUnit, rDest, rSrc);
}

/* Needed by the register allocator */
void oatRegCopyWide(CompilationUnit* cUnit, int destLo, int destHi,
                            int srcLo, int srcHi)
{
    genRegCopyWide(cUnit, destLo, destHi, srcLo, srcHi);
}

void oatFlushRegImpl(CompilationUnit* cUnit, int rBase,
                             int displacement, int rSrc, OpSize size)
{
    storeBaseDisp(cUnit, rBase, displacement, rSrc, size);
}

void oatFlushRegWideImpl(CompilationUnit* cUnit, int rBase,
                                 int displacement, int rSrcLo, int rSrcHi)
{
    storeBaseDispWide(cUnit, rBase, displacement, rSrcLo, rSrcHi);
}

}  // namespace art
