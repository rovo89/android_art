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
 * This file contains mips-specific codegen factory support.
 * It is included by
 *
 *        Codegen-$(TARGET_ARCH_VARIANT).c
 *
 */

#define SLOW_FIELD_PATH (cUnit->enableDebug & (1 << kDebugSlowFieldPath))
#define SLOW_INVOKE_PATH (cUnit->enableDebug & (1 << kDebugSlowInvokePath))
#define SLOW_STRING_PATH (cUnit->enableDebug & (1 << kDebugSlowStringPath))
#define SLOW_TYPE_PATH (cUnit->enableDebug & (1 << kDebugSlowTypePath))
#define EXERCISE_SLOWEST_FIELD_PATH (cUnit->enableDebug & \
    (1 << kDebugSlowestFieldPath))
#define EXERCISE_SLOWEST_STRING_PATH (cUnit->enableDebug & \
    (1 << kDebugSlowestStringPath))
#define EXERCISE_RESOLVE_METHOD (cUnit->enableDebug & \
    (1 << kDebugExerciseResolveMethod))

// FIXME - this is the Mips version, change to MIPS

namespace art {

STATIC void genDebuggerUpdate(CompilationUnit* cUnit, int32_t offset);

/* Generate conditional branch instructions */
STATIC MipsLIR* genConditionalBranch(CompilationUnit* cUnit,
                                    MipsConditionCode cond,
                                    MipsLIR* target)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
    return NULL;
#if 0
    MipsLIR* branch = opCondBranch(cUnit, cond);
    branch->generic.target = (LIR*) target;
    return branch;
#endif
}

/* Generate unconditional branch instructions */
STATIC MipsLIR* genUnconditionalBranch(CompilationUnit* cUnit, MipsLIR* target)
{
    MipsLIR* branch = opNone(cUnit, kOpUncondBr);
    branch->generic.target = (LIR*) target;
    return branch;
}

STATIC MipsLIR* callRuntimeHelper(CompilationUnit* cUnit, int reg)
{
    oatClobberCalleeSave(cUnit);
    return opReg(cUnit, kOpBlx, reg);
}

/*
 * Mark garbage collection card. Skip if the value we're storing is null.
 */
STATIC void markGCCard(CompilationUnit* cUnit, int valReg, int tgtAddrReg)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
    int regCardBase = oatAllocTemp(cUnit);
    int regCardNo = oatAllocTemp(cUnit);
    MipsLIR* branchOver = genCmpImmBranch(cUnit, kMipsCondEq, valReg, 0);
    loadWordDisp(cUnit, rSELF, Thread::CardTableOffset().Int32Value(),
                 regCardBase);
    opRegRegImm(cUnit, kOpLsr, regCardNo, tgtAddrReg, GC_CARD_SHIFT);
    storeBaseIndexed(cUnit, regCardBase, regCardNo, regCardBase, 0,
                     kUnsignedByte);
    MipsLIR* target = newLIR0(cUnit, kMipsPseudoTargetLabel);
    target->defMask = ENCODE_ALL;
    branchOver->generic.target = (LIR*)target;
    oatFreeTemp(cUnit, regCardBase);
    oatFreeTemp(cUnit, regCardNo);
#endif
}

/*
 * Utiltiy to load the current Method*.  Broken out
 * to allow easy change between placing the current Method* in a
 * dedicated register or its home location in the frame.
 */
STATIC void loadCurrMethodDirect(CompilationUnit *cUnit, int rTgt)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
#if defined(METHOD_IN_REG)
    genRegCopy(cUnit, rTgt, rMETHOD);
#else
    loadWordDisp(cUnit, rSP, 0, rTgt);
#endif
#endif
}

STATIC int loadCurrMethod(CompilationUnit *cUnit)
{
#if defined(METHOD_IN_REG)
    return rMETHOD;
#else
    int mReg = oatAllocTemp(cUnit);
    loadCurrMethodDirect(cUnit, mReg);
    return mReg;
#endif
}

STATIC MipsLIR* genCheck(CompilationUnit* cUnit, MipsConditionCode cCode,
                        MIR* mir, MipsThrowKind kind)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
    return 0;
#if 0
    MipsLIR* tgt = (MipsLIR*)oatNew(cUnit, sizeof(MipsLIR), true, kAllocLIR);
    tgt->opcode = kMipsPseudoThrowTarget;
    tgt->operands[0] = kind;
    tgt->operands[1] = mir ? mir->offset : 0;
    MipsLIR* branch = genConditionalBranch(cUnit, cCode, tgt);
    // Remember branch target - will process later
    oatInsertGrowableList(cUnit, &cUnit->throwLaunchpads, (intptr_t)tgt);
    return branch;
#endif
}

STATIC MipsLIR* genImmedCheck(CompilationUnit* cUnit, MipsConditionCode cCode,
                             int reg, int immVal, MIR* mir, MipsThrowKind kind)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
    return 0;
#if 0
    MipsLIR* tgt = (MipsLIR*)oatNew(cUnit, sizeof(MipsLIR), true, kAllocLIR);
    tgt->opcode = kMipsPseudoThrowTarget;
    tgt->operands[0] = kind;
    tgt->operands[1] = mir->offset;
    MipsLIR* branch;
    if (cCode == kMipsCondAl) {
        branch = genUnconditionalBranch(cUnit, tgt);
    } else {
        branch = genCmpImmBranch(cUnit, cCode, reg, immVal);
        branch->generic.target = (LIR*)tgt;
    }
    // Remember branch target - will process later
    oatInsertGrowableList(cUnit, &cUnit->throwLaunchpads, (intptr_t)tgt);
    return branch;
#endif
}

/* Perform null-check on a register.  */
STATIC MipsLIR* genNullCheck(CompilationUnit* cUnit, int sReg, int mReg,
                             MIR* mir)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
    return 0;
#if 0
    if (!(cUnit->disableOpt & (1 << kNullCheckElimination)) &&
        mir->optimizationFlags & MIR_IGNORE_NULL_CHECK) {
        return NULL;
    }
    return genImmedCheck(cUnit, kMipsCondEq, mReg, 0, mir, kMipsThrowNullPointer);
#endif
}

/* Perform check on two registers */
STATIC TGT_LIR* genRegRegCheck(CompilationUnit* cUnit, MipsConditionCode cCode,
                               int reg1, int reg2, MIR* mir, MipsThrowKind kind)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
    return 0;
#if 0
    MipsLIR* tgt = (MipsLIR*)oatNew(cUnit, sizeof(MipsLIR), true, kAllocLIR);
    tgt->opcode = kMipsPseudoThrowTarget;
    tgt->operands[0] = kind;
    tgt->operands[1] = mir ? mir->offset : 0;
    tgt->operands[2] = reg1;
    tgt->operands[3] = reg2;
    opRegReg(cUnit, kOpCmp, reg1, reg2);
    MipsLIR* branch = genConditionalBranch(cUnit, cCode, tgt);
    // Remember branch target - will process later
    oatInsertGrowableList(cUnit, &cUnit->throwLaunchpads, (intptr_t)tgt);
    return branch;
#endif
}

/*
 * Let helper function take care of everything.  Will call
 * Array::AllocFromCode(type_idx, method, count);
 * Note: AllocFromCode will handle checks for errNegativeArraySize.
 */
STATIC void genNewArray(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                        RegLocation rlSrc)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
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
#endif
}

/*
 * Similar to genNewArray, but with post-allocation initialization.
 * Verifier guarantees we're dealing with an array class.  Current
 * code throws runtime exception "bad Filled array req" for 'D' and 'J'.
 * Current code also throws internal unimp if not 'L', '[' or 'I'.
 */
STATIC void genFilledNewArray(CompilationUnit* cUnit, MIR* mir, bool isRange)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
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
        MipsLIR* target = newLIR0(cUnit, kMipsPseudoTargetLabel);
        target->defMask = ENCODE_ALL;
        // Copy next element
        loadBaseIndexed(cUnit, rSrc, rIdx, rVal, 2, kWord);
        storeBaseIndexed(cUnit, rDst, rIdx, rVal, 2, kWord);
        // Use setflags encoding here
        newLIR3(cUnit, kThumb2SubsRRI12, rIdx, rIdx, 1);
        MipsLIR* branch = opCondBranch(cUnit, kMipsCondGe);
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
#endif
}

STATIC void genSput(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc,
                    bool isLongOrDouble, bool isObject)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
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
                         Array::DataOffset().Int32Value() + sizeof(int32_t*) *
                         ssbIndex, rBase);
            // rBase now points at appropriate static storage base (Class*)
            // or NULL if not initialized. Check for NULL and call helper if NULL.
            // TUNING: fast path should fall through
            MipsLIR* branchOver = genCmpImmBranch(cUnit, kMipsCondNe, rBase, 0);
            loadWordDisp(cUnit, rSELF,
                         OFFSETOF_MEMBER(Thread, pInitializeStaticStorage), rLR);
            loadConstant(cUnit, r0, ssbIndex);
            callRuntimeHelper(cUnit, rLR);
            MipsLIR* skipTarget = newLIR0(cUnit, kMipsPseudoTargetLabel);
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
#endif
}

STATIC void genSget(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                    bool isLongOrDouble, bool isObject)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
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
            MipsLIR* branchOver = genCmpImmBranch(cUnit, kMipsCondNe, rBase, 0);
            loadWordDisp(cUnit, rSELF,
                         OFFSETOF_MEMBER(Thread, pInitializeStaticStorage), rLR);
            loadConstant(cUnit, r0, ssbIndex);
            callRuntimeHelper(cUnit, rLR);
            MipsLIR* skipTarget = newLIR0(cUnit, kMipsPseudoTargetLabel);
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
#endif
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
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
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
#endif
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
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
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
#endif
    return state + 1;
}

/*
 * Interleave launch code for INVOKE_SUPER.  See comments
 * for nextVCallIns.
 */
STATIC int nextSuperCallInsn(CompilationUnit* cUnit, MIR* mir,
                             int state, uint32_t dexIdx, uint32_t methodIdx)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
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
#endif
    return state + 1;
}

STATIC int nextInvokeInsnSP(CompilationUnit* cUnit, MIR* mir, int trampoline,
                            int state, uint32_t dexIdx, uint32_t methodIdx)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
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
#endif
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
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
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
#endif
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
                                MipsLIR** pcrLabel, NextCallInsn nextCallInsn,
                                uint32_t dexIdx, uint32_t methodIdx,
                                bool skipThis)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
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
#endif
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
                              MipsLIR** pcrLabel, NextCallInsn nextCallInsn,
                              uint32_t dexIdx, uint32_t methodIdx,
                              bool skipThis)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
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
        MipsLIR* ld = newLIR3(cUnit, kThumb2Vldms, r3, fr0, regsLeft);
        //TUNING: loosen barrier
        ld->defMask = ENCODE_ALL;
        setMemRefType(ld, true /* isLoad */, kDalvikReg);
        callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx);
        opRegRegImm(cUnit, kOpAdd, r3, rSP, 4 /* Method* */ + (3 * 4));
        callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx);
        MipsLIR* st = newLIR3(cUnit, kThumb2Vstms, r3, fr0, regsLeft);
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
#endif
    return callState;
}

// Debugging routine - if null target, branch to DebugMe
STATIC void genShowTarget(CompilationUnit* cUnit)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
    MipsLIR* branchOver = genCmpImmBranch(cUnit, kMipsCondNe, rLR, 0);
    loadWordDisp(cUnit, rSELF,
                 OFFSETOF_MEMBER(Thread, pDebugMe), rLR);
    MipsLIR* target = newLIR0(cUnit, kMipsPseudoTargetLabel);
    target->defMask = -1;
    branchOver->generic.target = (LIR*)target;
#endif
}

STATIC void genThrowVerificationError(CompilationUnit* cUnit, MIR* mir)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
    loadWordDisp(cUnit, rSELF,
                 OFFSETOF_MEMBER(Thread, pThrowVerificationErrorFromCode), rLR);
    loadConstant(cUnit, r0, mir->dalvikInsn.vA);
    loadConstant(cUnit, r1, mir->dalvikInsn.vB);
    callRuntimeHelper(cUnit, rLR);
#endif
}

STATIC void genCompareAndBranch(CompilationUnit* cUnit, BasicBlock* bb,
                                MIR* mir, RegLocation rlSrc1,
                                RegLocation rlSrc2, MipsLIR* labelList)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
    MipsConditionCode cond;
    rlSrc1 = loadValue(cUnit, rlSrc1, kCoreReg);
    rlSrc2 = loadValue(cUnit, rlSrc2, kCoreReg);
    opRegReg(cUnit, kOpCmp, rlSrc1.lowReg, rlSrc2.lowReg);
    Opcode opcode = mir->dalvikInsn.opcode;
    switch(opcode) {
        case OP_IF_EQ:
            cond = kMipsCondEq;
            break;
        case OP_IF_NE:
            cond = kMipsCondNe;
            break;
        case OP_IF_LT:
            cond = kMipsCondLt;
            break;
        case OP_IF_GE:
            cond = kMipsCondGe;
            break;
        case OP_IF_GT:
            cond = kMipsCondGt;
            break;
        case OP_IF_LE:
            cond = kMipsCondLe;
            break;
        default:
            cond = (MipsConditionCode)0;
            LOG(FATAL) << "Unexpected opcode " << (int)opcode;
    }
    genConditionalBranch(cUnit, cond, &labelList[bb->taken->id]);
    genUnconditionalBranch(cUnit, &labelList[bb->fallThrough->id]);
#endif
}

STATIC void genCompareZeroAndBranch(CompilationUnit* cUnit, BasicBlock* bb,
                                   MIR* mir, RegLocation rlSrc,
                                   MipsLIR* labelList)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
    MipsConditionCode cond;
    rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
    opRegImm(cUnit, kOpCmp, rlSrc.lowReg, 0);
    Opcode opcode = mir->dalvikInsn.opcode;
    switch(opcode) {
        case OP_IF_EQZ:
            cond = kMipsCondEq;
            break;
        case OP_IF_NEZ:
            cond = kMipsCondNe;
            break;
        case OP_IF_LTZ:
            cond = kMipsCondLt;
            break;
        case OP_IF_GEZ:
            cond = kMipsCondGe;
            break;
        case OP_IF_GTZ:
            cond = kMipsCondGt;
            break;
        case OP_IF_LEZ:
            cond = kMipsCondLe;
            break;
        default:
            cond = (MipsConditionCode)0;
            LOG(FATAL) << "Unexpected opcode " << (int)opcode;
    }
    genConditionalBranch(cUnit, cond, &labelList[bb->taken->id]);
    genUnconditionalBranch(cUnit, &labelList[bb->fallThrough->id]);
#endif
}

STATIC void genIntToLong(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                         RegLocation rlSrc)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    if (rlSrc.location == kLocPhysReg) {
        genRegCopy(cUnit, rlResult.lowReg, rlSrc.lowReg);
    } else {
        loadValueDirect(cUnit, rlSrc, rlResult.lowReg);
    }
    opRegRegImm(cUnit, kOpAsr, rlResult.highReg,
                rlResult.lowReg, 31);
    storeValueWide(cUnit, rlDest, rlResult);
#endif
}

STATIC void genIntNarrowing(CompilationUnit* cUnit, MIR* mir,
                            RegLocation rlDest, RegLocation rlSrc)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
     rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
     RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
     OpKind op = kOpInvalid;
     switch(mir->dalvikInsn.opcode) {
         case OP_INT_TO_BYTE:
             op = kOp2Byte;
             break;
         case OP_INT_TO_SHORT:
              op = kOp2Short;
              break;
         case OP_INT_TO_CHAR:
              op = kOp2Char;
              break;
         default:
             LOG(ERROR) << "Bad int conversion type";
     }
     opRegReg(cUnit, op, rlResult.lowReg, rlSrc.lowReg);
     storeValue(cUnit, rlDest, rlResult);
#endif
}

/*
 * If there are any ins passed in registers that have not been promoted
 * to a callee-save register, flush them to the frame.  Perform intial
 * assignment of promoted arguments.
 */
STATIC void flushIns(CompilationUnit* cUnit)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
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
#endif
}

STATIC void genEntrySequence(CompilationUnit* cUnit, BasicBlock* bb)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
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
    newLIR0(cUnit, kMipsPseudoMethodEntry);
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
        genRegRegCheck(cUnit, kMipsCondCc, rLR, r12, NULL,
                       kMipsThrowStackOverflow);
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
#endif
}

STATIC void genExitSequence(CompilationUnit* cUnit, BasicBlock* bb)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
    int spillCount = cUnit->numCoreSpills + cUnit->numFPSpills;
    /*
     * In the exit path, r0/r1 are live - make sure they aren't
     * allocated by the register utilities as temps.
     */
    oatLockTemp(cUnit, r0);
    oatLockTemp(cUnit, r1);

    newLIR0(cUnit, kMipsPseudoMethodExit);
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
#endif
}

/*
 * Nop any unconditional branches that go to the next instruction.
 * Note: new redundant branches may be inserted later, and we'll
 * use a check in final instruction assembly to nop those out.
 */
void removeRedundantBranches(CompilationUnit* cUnit)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
    MipsLIR* thisLIR;

    for (thisLIR = (MipsLIR*) cUnit->firstLIRInsn;
         thisLIR != (MipsLIR*) cUnit->lastLIRInsn;
         thisLIR = NEXT_LIR(thisLIR)) {

        /* Branch to the next instruction */
        if ((thisLIR->opcode == kThumbBUncond) ||
            (thisLIR->opcode == kThumb2BUncond)) {
            MipsLIR* nextLIR = thisLIR;

            while (true) {
                nextLIR = NEXT_LIR(nextLIR);

                /*
                 * Is the branch target the next instruction?
                 */
                if (nextLIR == (MipsLIR*) thisLIR->generic.target) {
                    thisLIR->flags.isNop = true;
                    break;
                }

                /*
                 * Found real useful stuff between the branch and the target.
                 * Need to explicitly check the lastLIRInsn here because it
                 * might be the last real instruction.
                 */
                if (!isPseudoOpcode(nextLIR->opcode) ||
                    (nextLIR = (MipsLIR*) cUnit->lastLIRInsn))
                    break;
            }
        }
    }
#endif
}

STATIC void handleSuspendLaunchpads(CompilationUnit *cUnit)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
    MipsLIR** suspendLabel =
        (MipsLIR **) cUnit->suspendLaunchpads.elemList;
    int numElems = cUnit->suspendLaunchpads.numUsed;

    for (int i = 0; i < numElems; i++) {
        /* TUNING: move suspend count load into helper */
        MipsLIR* lab = suspendLabel[i];
        MipsLIR* resumeLab = (MipsLIR*)lab->operands[0];
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
#endif
}

STATIC void handleThrowLaunchpads(CompilationUnit *cUnit)
{
    UNIMPLEMENTED(FATAL) << "Needs mips version";
#if 0
    MipsLIR** throwLabel =
        (MipsLIR **) cUnit->throwLaunchpads.elemList;
    int numElems = cUnit->throwLaunchpads.numUsed;
    int i;

    for (i = 0; i < numElems; i++) {
        MipsLIR* lab = throwLabel[i];
        cUnit->currentDalvikOffset = lab->operands[1];
        oatAppendLIR(cUnit, (LIR *)lab);
        int funcOffset = 0;
        int v1 = lab->operands[2];
        int v2 = lab->operands[3];
        switch(lab->operands[0]) {
            case kMipsThrowNullPointer:
                funcOffset = OFFSETOF_MEMBER(Thread, pThrowNullPointerFromCode);
                break;
            case kMipsThrowArrayBounds:
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
            case kMipsThrowDivZero:
                funcOffset = OFFSETOF_MEMBER(Thread, pThrowDivZeroFromCode);
                break;
            case kMipsThrowVerificationError:
                loadConstant(cUnit, r0, v1);
                loadConstant(cUnit, r1, v2);
                funcOffset =
                    OFFSETOF_MEMBER(Thread, pThrowVerificationErrorFromCode);
                break;
            case kMipsThrowNegArraySize:
                genRegCopy(cUnit, r0, v1);
                funcOffset =
                    OFFSETOF_MEMBER(Thread, pThrowNegArraySizeFromCode);
                break;
            case kMipsThrowNoSuchMethod:
                genRegCopy(cUnit, r0, v1);
                funcOffset =
                    OFFSETOF_MEMBER(Thread, pThrowNoSuchMethodFromCode);
                break;
            case kMipsThrowStackOverflow:
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
#endif
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

/* Needed by the Assembler */
void oatSetupResourceMasks(MipsLIR* lir)
{
    setupResourceMasks(lir);
}

}  // namespace art
