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

static const RegLocation badLoc = {kLocDalvikFrame, 0, 0, INVALID_REG,
                                   INVALID_REG, INVALID_SREG, 0,
                                   kLocDalvikFrame, INVALID_REG, INVALID_REG,
                                   INVALID_OFFSET};
static const RegLocation retLoc = LOC_DALVIK_RETURN_VAL;
static const RegLocation retLocWide = LOC_DALVIK_RETURN_VAL_WIDE;

/*
 * Let helper function take care of everything.  Will call
 * Array::AllocFromCode(type_idx, method, count);
 * Note: AllocFromCode will handle checks for errNegativeArraySize.
 */
static void genNewArray(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                        RegLocation rlSrc)
{
    oatFlushAllRegs(cUnit);    /* Everything to home location */
    loadWordDisp(cUnit, rSELF,
                 OFFSETOF_MEMBER(Thread, pAllocFromCode), rLR);
    loadCurrMethodDirect(cUnit, r1);              // arg1 <- Method*
    loadConstant(cUnit, r0, mir->dalvikInsn.vC);  // arg0 <- type_id
    loadValueDirectFixed(cUnit, rlSrc, r2);       // arg2 <- count
    opReg(cUnit, kOpBlx, rLR);
    oatClobberCallRegs(cUnit);
    RegLocation rlResult = oatGetReturn(cUnit);
    storeValue(cUnit, rlDest, rlResult);
}

/*
 * Similar to genNewArray, but with post-allocation initialization.
 * Verifier guarantees we're dealing with an array class.  Current
 * code throws runtime exception "bad Filled array req" for 'D' and 'J'.
 * Current code also throws internal unimp if not 'L', '[' or 'I'.
 */
static void genFilledNewArray(CompilationUnit* cUnit, MIR* mir, bool isRange)
{
    DecodedInstruction* dInsn = &mir->dalvikInsn;
    int elems;
    int typeId;
    if (isRange) {
        elems = dInsn->vA;
        typeId = dInsn->vB;
    } else {
        elems = dInsn->vB;
        typeId = dInsn->vC;
    }
    oatFlushAllRegs(cUnit);    /* Everything to home location */
    // TODO: Alloc variant that checks types (see header comment) */
    UNIMPLEMENTED(WARNING) << "Need AllocFromCode variant w/ extra checks";
    loadWordDisp(cUnit, rSELF,
                 OFFSETOF_MEMBER(Thread, pAllocFromCode), rLR);
    loadCurrMethodDirect(cUnit, r1);              // arg1 <- Method*
    loadConstant(cUnit, r0, typeId);              // arg0 <- type_id
    loadConstant(cUnit, r2, elems);               // arg2 <- count
    opReg(cUnit, kOpBlx, rLR);
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
                storeBaseDisp(cUnit, rSP, loc.spOffset, loc.lowReg, kWord);
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
        opRegRegImm(cUnit, kOpAdd, rSrc, rSP, rlFirst.spOffset);
        // Set up the target pointer
        opRegRegImm(cUnit, kOpAdd, rDst, r0,
                    Array::DataOffset().Int32Value());
        // Set up the loop counter (known to be > 0)
        loadConstant(cUnit, rIdx, dInsn->vA);
        // Generate the copy loop.  Going backwards for convenience
        ArmLIR* target = newLIR0(cUnit, kArmPseudoTargetLabel);
        target->defMask = ENCODE_ALL;
        // Copy next element
        loadBaseIndexed(cUnit, rSrc, rIdx, rVal, 2, kWord);
        storeBaseIndexed(cUnit, rDst, rIdx, rVal, 2, kWord);
        // Use setflags encoding here
        newLIR3(cUnit, kThumb2SubsRRI12, rIdx, rIdx, 1);
        ArmLIR* branch = opCondBranch(cUnit, kArmCondNe);
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

static void genSput(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc)
{
    bool slow_path = true;
    bool isObject = ((mir->dalvikInsn.opcode == OP_SPUT_OBJECT) ||
                     (mir->dalvikInsn.opcode == OP_SPUT_OBJECT_VOLATILE));
    UNIMPLEMENTED(WARNING) << "Implement sput fast path";
    int funcOffset;
    if (slow_path) {
        if (isObject) {
            funcOffset = OFFSETOF_MEMBER(Thread, pSetObjStatic);
        } else {
            funcOffset = OFFSETOF_MEMBER(Thread, pSet32Static);
        }
        oatFlushAllRegs(cUnit);
        loadWordDisp(cUnit, rSELF, funcOffset, rLR);
        loadConstant(cUnit, r0, mir->dalvikInsn.vB);
        loadCurrMethodDirect(cUnit, r1);
        loadValueDirect(cUnit, rlSrc, r2);
        opReg(cUnit, kOpBlx, rLR);
        oatClobberCallRegs(cUnit);
    } else {
        UNIMPLEMENTED(FATAL) << "Must update for new world";
#if 0
    int valOffset = OFFSETOF_MEMBER(StaticField, value);
    int tReg = oatAllocTemp(cUnit);
    int objHead;
    bool isVolatile;
    bool isSputObject;
    const Method *method = (mir->OptimizationFlags & MIR_CALLEE) ?
        mir->meta.calleeMethod : cUnit->method;
    void* fieldPtr = (void*)
      (method->clazz->pDvmDex->pResFields[mir->dalvikInsn.vB]);
    Opcode opcode = mir->dalvikInsn.opcode;

    if (fieldPtr == NULL) {
        // FIXME: need to handle this case for oat();
        UNIMPLEMENTED(FATAL);
    }

#if ANDROID_SMP != 0
    isVolatile = (opcode == OP_SPUT_VOLATILE) ||
                 (opcode == OP_SPUT_VOLATILE_JUMBO) ||
                 (opcode == OP_SPUT_OBJECT_VOLATILE) ||
                 (opcode == OP_SPUT_OBJECT_VOLATILE_JUMBO);
    assert(isVolatile == artIsVolatileField((Field *) fieldPtr));
#else
    isVolatile = artIsVolatileField((Field *) fieldPtr);
#endif

    isSputObject = (opcode == OP_SPUT_OBJECT) ||
                   (opcode == OP_SPUT_OBJECT_VOLATILE);

    rlSrc = oatGetSrc(cUnit, mir, 0);
    rlSrc = loadValue(cUnit, rlSrc, kAnyReg);
    loadConstant(cUnit, tReg,  (int) fieldPtr);
    if (isSputObject) {
        objHead = oatAllocTemp(cUnit);
        loadWordDisp(cUnit, tReg, OFFSETOF_MEMBER(Field, clazz), objHead);
    }
    storeWordDisp(cUnit, tReg, valOffset ,rlSrc.lowReg);
    oatFreeTemp(cUnit, tReg);
    if (isVolatile) {
        oatGenMemBarrier(cUnit, kSY);
    }
    if (isSputObject) {
        /* NOTE: marking card based sfield->clazz */
        markGCCard(cUnit, rlSrc.lowReg, objHead);
        oatFreeTemp(cUnit, objHead);
    }
#endif
    }
}

static void genSputWide(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc)
{
    bool slow_path = true;
    UNIMPLEMENTED(WARNING) << "Implement sput-wide fast path";
    int funcOffset;
    if (slow_path) {
        funcOffset = OFFSETOF_MEMBER(Thread, pSet64Static);
        oatFlushAllRegs(cUnit);
        loadWordDisp(cUnit, rSELF, funcOffset, rLR);
        loadConstant(cUnit, r0, mir->dalvikInsn.vB);
        loadCurrMethodDirect(cUnit, r1);
        loadValueDirectWideFixed(cUnit, rlSrc, r2, r3);
        opReg(cUnit, kOpBlx, rLR);
        oatClobberCallRegs(cUnit);
    } else {
        UNIMPLEMENTED(FATAL) << "Must update for new world";
#if 0
    int tReg = oatAllocTemp(cUnit);
    int valOffset = OFFSETOF_MEMBER(StaticField, value);
    const Method *method = (mir->OptimizationFlags & MIR_CALLEE) ?
        mir->meta.calleeMethod : cUnit->method;
    void* fieldPtr = (void*)
      (method->clazz->pDvmDex->pResFields[mir->dalvikInsn.vB]);

    if (fieldPtr == NULL) {
        // FIXME: need to handle this case for oat();
        UNIMPLEMENTED(FATAL);
    }

    rlSrc = oatGetSrcWide(cUnit, mir, 0, 1);
    rlSrc = loadValueWide(cUnit, rlSrc, kAnyReg);
    loadConstant(cUnit, tReg,  (int) fieldPtr + valOffset);

    storePair(cUnit, tReg, rlSrc.lowReg, rlSrc.highReg);
#endif
    }
}



static void genSgetWide(CompilationUnit* cUnit, MIR* mir,
                 RegLocation rlResult, RegLocation rlDest)
{
    bool slow_path = true;
    UNIMPLEMENTED(WARNING) << "Implement sget-wide fast path";
    int funcOffset;
    if (slow_path) {
        funcOffset = OFFSETOF_MEMBER(Thread, pGet64Static);
        oatFlushAllRegs(cUnit);
        loadWordDisp(cUnit, rSELF, funcOffset, rLR);
        loadConstant(cUnit, r0, mir->dalvikInsn.vB);
        loadCurrMethodDirect(cUnit, r1);
        opReg(cUnit, kOpBlx, rLR);
        RegLocation rlResult = oatGetReturnWide(cUnit);
        storeValueWide(cUnit, rlDest, rlResult);
    } else {
        UNIMPLEMENTED(FATAL) << "Must update for new world";
#if 0
    int valOffset = OFFSETOF_MEMBER(StaticField, value);
    const Method *method = (mir->OptimizationFlags & MIR_CALLEE) ?
        mir->meta.calleeMethod : cUnit->method;
    void* fieldPtr = (void*)
      (method->clazz->pDvmDex->pResFields[mir->dalvikInsn.vB]);

    if (fieldPtr == NULL) {
        // FIXME: need to handle this case for oat();
        UNIMPLEMENTED(FATAL);
    }

    int tReg = oatAllocTemp(cUnit);
    rlDest = oatGetDestWide(cUnit, mir, 0, 1);
    rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
    loadConstant(cUnit, tReg,  (int) fieldPtr + valOffset);

    loadPair(cUnit, tReg, rlResult.lowReg, rlResult.highReg);

    storeValueWide(cUnit, rlDest, rlResult);
#endif
    }
}

static void genSget(CompilationUnit* cUnit, MIR* mir,
             RegLocation rlResult, RegLocation rlDest)
{
    bool slow_path = true;
    bool isObject = ((mir->dalvikInsn.opcode == OP_SGET_OBJECT) ||
                     (mir->dalvikInsn.opcode == OP_SGET_OBJECT_VOLATILE));
    UNIMPLEMENTED(WARNING) << "Implement sget fast path";
    int funcOffset;
    if (slow_path) {
        if (isObject) {
            funcOffset = OFFSETOF_MEMBER(Thread, pGetObjStatic);
        } else {
            funcOffset = OFFSETOF_MEMBER(Thread, pGet32Static);
        }
        oatFlushAllRegs(cUnit);
        loadWordDisp(cUnit, rSELF, funcOffset, rLR);
        loadConstant(cUnit, r0, mir->dalvikInsn.vB);
        loadCurrMethodDirect(cUnit, r1);
        opReg(cUnit, kOpBlx, rLR);
        RegLocation rlResult = oatGetReturn(cUnit);
        storeValue(cUnit, rlDest, rlResult);
    } else {
        UNIMPLEMENTED(FATAL) << "Must update for new world";
#if 0
    int valOffset = OFFSETOF_MEMBER(StaticField, value);
    int tReg = oatAllocTemp(cUnit);
    bool isVolatile;
    const Method *method = cUnit->method;
    void* fieldPtr = (void*)
      (method->clazz->pDvmDex->pResFields[mir->dalvikInsn.vB]);

    if (fieldPtr == NULL) {
        // FIXME: need to handle this case for oat();
        UNIMPLEMENTED(FATAL);
    }

    /*
     * On SMP systems, Dalvik opcodes found to be referencing
     * volatile fields are rewritten to their _VOLATILE variant.
     * However, this does not happen on non-SMP systems. The compiler
     * still needs to know about volatility to avoid unsafe
     * optimizations so we determine volatility based on either
     * the opcode or the field access flags.
     */
#if ANDROID_SMP != 0
    Opcode opcode = mir->dalvikInsn.opcode;
    isVolatile = (opcode == OP_SGET_VOLATILE) ||
                 (opcode == OP_SGET_OBJECT_VOLATILE);
    assert(isVolatile == artIsVolatileField((Field *) fieldPtr));
#else
    isVolatile = artIsVolatileField((Field *) fieldPtr);
#endif

    rlDest = oatGetDest(cUnit, mir, 0);
    rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
    loadConstant(cUnit, tReg,  (int) fieldPtr + valOffset);

    if (isVolatile) {
        oatGenMemBarrier(cUnit, kSY);
    }
    loadWordDisp(cUnit, tReg, 0, rlResult.lowReg);

    storeValue(cUnit, rlDest, rlResult);
#endif
    }
}

typedef int (*NextCallInsn)(CompilationUnit*, MIR*, DecodedInstruction*, int,
                            ArmLIR*);

/*
 * Bit of a hack here - in leiu of a real scheduling pass,
 * emit the next instruction in static & direct invoke sequences.
 */
static int nextSDCallInsn(CompilationUnit* cUnit, MIR* mir,
                        DecodedInstruction* dInsn, int state,
                        ArmLIR* rollback)
{
    DCHECK(rollback == NULL);
    uint32_t idx = dInsn->vB;
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
                art::CodeAndDirectMethods::CodeOffsetInBytes(idx), rLR);
            loadWordDisp(cUnit, r0,
                art::CodeAndDirectMethods::MethodOffsetInBytes(idx), r0);
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
static int nextVCallInsn(CompilationUnit* cUnit, MIR* mir,
                        DecodedInstruction* dInsn, int state,
                        ArmLIR* rollback)
{
    DCHECK(rollback == NULL);
    RegLocation rlArg;
    /*
     * This is the fast path in which the target virtual method is
     * fully resolved at compile time.
     */
    Method* baseMethod = cUnit->method->GetDexCacheResolvedMethods()->
        Get(dInsn->vB);
    CHECK(baseMethod != NULL);
    uint32_t target_idx = baseMethod->GetMethodIndex();
    switch(state) {
        case 0:  // Get "this" [set r1]
            rlArg = oatGetSrc(cUnit, mir, 0);
            loadValueDirectFixed(cUnit, rlArg, r1);
            break;
        case 1: // Is "this" null? [use r1]
            genNullCheck(cUnit, oatSSASrc(mir,0), r1, mir->offset, NULL);
            // get this->klass_ [use r1, set rLR]
            loadWordDisp(cUnit, r1, Object::ClassOffset().Int32Value(), rLR);
            break;
        case 2: // Get this->klass_->vtable [usr rLR, set rLR]
            loadWordDisp(cUnit, rLR, Class::VTableOffset().Int32Value(), rLR);
            break;
        case 3: // Get target method [use rLR, set r0]
            loadWordDisp(cUnit, rLR, (target_idx * 4) +
                         art::Array::DataOffset().Int32Value(), r0);
            break;
        case 4: // Get the target compiled code address [uses r0, sets rLR]
            loadWordDisp(cUnit, r0, Method::GetCodeOffset().Int32Value(), rLR);
            break;
        default:
            return -1;
    }
    return state + 1;
}

static int nextVCallInsnSP(CompilationUnit* cUnit, MIR* mir,
                           DecodedInstruction* dInsn, int state,
                           ArmLIR* rollback)
{
    DCHECK(rollback != NULL);
    RegLocation rlArg;
    ArmLIR* skipBranch;
    ArmLIR* skipTarget;
    /*
     * This handles the case in which the base method is not fully
     * resolved at compile time.  We must generate code to test
     * for resolution a run time, bail to the slow path if not to
     * fill in all the tables.  In the latter case, we'll restart at
     * at the beginning of the sequence.
     */
    switch(state) {
        case 0:  // Get the current Method* [sets r0]
            loadCurrMethodDirect(cUnit, r0);
            break;
        case 1: // Get method->dex_cache_resolved_methods_
            loadWordDisp(cUnit, r0,
                Method::GetDexCacheResolvedMethodsOffset().Int32Value(), rLR);
            break;
        case 2: // method->dex_cache_resolved_methods_->Get(method_idx)
            loadWordDisp(cUnit, rLR, (dInsn->vB * 4) +
                         art::Array::DataOffset().Int32Value(), rLR);
            break;
        case 3: // Resolved?
            skipBranch = genCmpImmBranch(cUnit, kArmCondNe, rLR, 0);
            // Slowest path, bail to helper, rollback and retry
            loadWordDisp(cUnit, rSELF,
                         OFFSETOF_MEMBER(Thread, pResolveMethodFromCode), rLR);
            loadConstant(cUnit, r1, dInsn->vB);
            newLIR1(cUnit, kThumbBlxR, rLR);
            genUnconditionalBranch(cUnit, rollback);
            // Resume normal slow path
            skipTarget = newLIR0(cUnit, kArmPseudoTargetLabel);
            skipTarget->defMask = ENCODE_ALL;
            skipBranch->generic.target = (LIR*)skipTarget;
            // Get base_method->method_index [usr rLR, set r12]
            loadBaseDisp(cUnit, mir, rLR,
                         Method::GetMethodIndexOffset().Int32Value(), r0,
                         kUnsignedHalf, INVALID_SREG);
            // Load "this" [set r1]
            rlArg = oatGetSrc(cUnit, mir, 0);
            loadValueDirectFixed(cUnit, rlArg, r1);
            break;
        case 4:
            // Is "this" null? [use r1]
            genNullCheck(cUnit, oatSSASrc(mir,0), r1, mir->offset, NULL);
            // get this->clazz [use r1, set rLR]
            loadWordDisp(cUnit, r1, Object::ClassOffset().Int32Value(), rLR);
            break;
        case 5:
            // get this->klass_->vtable_ [usr rLR, set rLR]
            loadWordDisp(cUnit, rLR, Class::VTableOffset().Int32Value(), rLR);
            DCHECK((art::Array::DataOffset().Int32Value() & 0x3) == 0);
            // In load shadow fold vtable_ object header size into method_index_
            opRegImm(cUnit, kOpAdd, r0,
                     art::Array::DataOffset().Int32Value() / 4);
            // Get target Method*
            loadBaseIndexed(cUnit, rLR, r0, r0, 2, kWord);
            break;
        case 6: // Get the target compiled code address [uses r0, sets rLR]
            loadWordDisp(cUnit, r0, Method::GetCodeOffset().Int32Value(), rLR);
            break;
        default:
            return -1;
    }
    return state + 1;
}

/* Load up to 3 arguments in r1..r3 */
static int loadArgRegs(CompilationUnit* cUnit, MIR* mir,
                       DecodedInstruction* dInsn, int callState,
                       int *args, NextCallInsn nextCallInsn, ArmLIR* rollback)
{
    for (int i = 0; i < 3; i++) {
        if (args[i] != INVALID_REG) {
            RegLocation rlArg = oatGetSrc(cUnit, mir, i);
            // Arguments are treated as a series of untyped 32-bit values.
            rlArg.wide = false;
            loadValueDirectFixed(cUnit, rlArg, r1 + i);
            callState = nextCallInsn(cUnit, mir, dInsn, callState, rollback);
        }
    }
    return callState;
}

/*
 * Interleave launch code for INVOKE_INTERFACE.  The target is
 * identified using artFindInterfaceMethodInCache(class, ref, method, dex)
 * Note that we'll have to reload "this" following the helper call.
 *
 * FIXME: do we need to have artFindInterfaceMethodInCache return
 * a NULL if not found so we can throw exception here?  Otherwise,
 * may need to pass some additional info to allow the helper function
 * to throw on its own.
 */
static int nextInterfaceCallInsn(CompilationUnit* cUnit, MIR* mir,
                                 DecodedInstruction* dInsn, int state,
                                 ArmLIR* rollback)
{
    UNIMPLEMENTED(FATAL) << "Need findInterfaceMethodInCache";
#if 0
    RegLocation rlArg;
    switch(state) {
        case 0:
            // Load "this" [set r12]
            rlArg = oatGetSrc(cUnit, mir, 0);
            loadValueDirectFixed(cUnit, rlArg, r12);
            // Get the current Method* [set arg2]
            loadCurrMethodDirect(cUnit, r2);
            // Is "this" null? [use r12]
            genNullCheck(cUnit, oatSSASrc(mir,0), r12,
                           mir->offset, NULL);
            // Get curMethod->clazz [set arg3]
            loadBaseDisp(cUnit, mir, r2, OFFSETOF_MEMBER(Method, clazz),
                         r3, kWord, INVALID_SREG);
            // Load this->class [usr r12, set arg0]
            loadBaseDisp(cUnit, mir, r12, OFFSETOF_MEMBER(Class, clazz),
                         r3, kWord, INVALID_SREG);
            // Load address of helper function
            loadBaseDisp(cUnit, mir, rSELF,
                      OFFSETOF_MEMBER(Thread, pArtFindInterfaceMethodInCache),
                      rLR, kWord, INVALID_SREG);
            // Get dvmDex
            loadBaseDisp(cUnit, mir, r3, OFFSETOF_MEMBER(Class, pDvmDex),
                         r3, kWord, INVALID_SREG);
            // Load ref [set arg1]
            loadConstant(cUnit, r1, dInsn->vB);
            // Call out to helper, target Method returned in ret0
            newLIR1(cUnit, kThumbBlxR, rLR);
            break;
        case 1: // Get the target compiled code address [use r0, set rLR]
            loadBaseDisp(cUnit, mir, r0, OFFSETOF_MEMBER(Method, compiledInsns),
                         rLR, kWord, INVALID_SREG);
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
static int nextSuperCallInsn(CompilationUnit* cUnit, MIR* mir,
                             DecodedInstruction* dInsn, int state,
                             ArmLIR* rollback)
{
    UNIMPLEMENTED(FATAL) << "Need INVOKE_SUPER implementation";
#if 0
    RegLocation rlArg;
    switch(state) {
        case 0:
            // Get the current Method* [set r0]
            loadCurrMethodDirect(cUnit, r0);
            // Load "this" [set r1]
            rlArg = oatGetSrc(cUnit, mir, 0);
            loadValueDirectFixed(cUnit, rlArg, r1);
            // Get method->clazz [use r0, set r12]
            loadBaseDisp(cUnit, mir, r0, OFFSETOF_MEMBER(Method, clazz),
                         r12, kWord, INVALID_SREG);
            // Get pResmethods [use r0, set rLR]
            loadBaseDisp(cUnit, mir, r0, OFFSETOF_MEMBER(Method, pResMethods),
                         rLR, kWord, INVALID_SREG);
            // Get clazz->super [use r12, set r12]
            loadBaseDisp(cUnit, mir, r12, OFFSETOF_MEMBER(Class, super),
                         r12, kWord, INVALID_SREG);
            // Get base method [use rLR, set r0]
            loadBaseDisp(cUnit, mir, rLR, dInsn->vB * 4, r0,
                         kWord, INVALID_SREG);
            // Is "this" null? [use r1]
            genNullCheck(cUnit, oatSSASrc(mir,0), r1,
                           mir->offset, NULL);
            // Get methodIndex [use r0, set rLR]
            loadBaseDisp(cUnit, mir, r0, OFFSETOF_MEMBER(Method, methodIndex),
                         rLR, kUnsignedHalf, INVALID_SREG);
            // Get vtableCount [use r12, set r0]
            loadBaseDisp(cUnit, mir, r12,
                         OFFSETOF_MEMBER(Class, vtableCount),
                         r0, kWord, INVALID_SREG);
            // Compare method index w/ vtable count [use r12, use rLR]
            genRegRegCheck(cUnit, kArmCondGe, rLR, r0, mir->offset, NULL);
            // get target Method* [use rLR, use r12, set r0]
            loadBaseIndexed(cUnit, r0, r12, rLR, 2, kWord);
        case 1: // Get the target compiled code address [use r0, set rLR]
            loadBaseDisp(cUnit, mir, r0, OFFSETOF_MEMBER(Method, compiledInsns),
                         rLR, kWord, INVALID_SREG);
        default:
            return -1;
    }
#endif
    return state + 1;
}

/*
 * Load up to 5 arguments, the first three of which will be in
 * r1 .. r3.  On entry r0 contains the current method pointer,
 * and as part of the load sequence, it must be replaced with
 * the target method pointer.  Note, this may also be called
 * for "range" variants if the number of arguments is 5 or fewer.
 */
static int genDalvikArgsNoRange(CompilationUnit* cUnit, MIR* mir,
                                DecodedInstruction* dInsn, int callState,
                                ArmLIR** pcrLabel, bool isRange,
                                NextCallInsn nextCallInsn, ArmLIR* rollback)
{
    RegLocation rlArg;
    int registerArgs[3];

    /* If no arguments, just return */
    if (dInsn->vA == 0)
        return callState;

    oatLockCallTemps(cUnit);
    callState = nextCallInsn(cUnit, mir, dInsn, callState, rollback);

    /*
     * Load frame arguments arg4 & arg5 first. Coded a little odd to
     * pre-schedule the method pointer target.
     */
    for (unsigned int i=3; i < dInsn->vA; i++) {
        int reg;
        int arg = (isRange) ? dInsn->vC + i : i;
        rlArg = oatUpdateLoc(cUnit, oatGetSrc(cUnit, mir, arg));
        if (rlArg.location == kLocPhysReg) {
            reg = rlArg.lowReg;
        } else {
            reg = r1;
            loadValueDirectFixed(cUnit, rlArg, r1);
            callState = nextCallInsn(cUnit, mir, dInsn, callState, rollback);
        }
        storeBaseDisp(cUnit, rSP, (i + 1) * 4, reg, kWord);
        callState = nextCallInsn(cUnit, mir, dInsn, callState, rollback);
    }

    /* Load register arguments r1..r3 */
    for (unsigned int i = 0; i < 3; i++) {
        if (i < dInsn->vA)
            registerArgs[i] = (isRange) ? dInsn->vC + i : i;
        else
            registerArgs[i] = INVALID_REG;
    }
    callState = loadArgRegs(cUnit, mir, dInsn, callState, registerArgs,
                            nextCallInsn, rollback);

    // Load direct & need a "this" null check?
    if (pcrLabel) {
        *pcrLabel = genNullCheck(cUnit, oatSSASrc(mir,0), r1,
                                 mir->offset, NULL);
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
static int genDalvikArgsRange(CompilationUnit* cUnit, MIR* mir,
                              DecodedInstruction* dInsn, int callState,
                              ArmLIR** pcrLabel, NextCallInsn nextCallInsn,
                              ArmLIR* rollback)
{
    int firstArg = dInsn->vC;
    int numArgs = dInsn->vA;

    // If we can treat it as non-range (Jumbo ops will use range form)
    if (numArgs <= 5)
        return genDalvikArgsNoRange(cUnit, mir, dInsn, callState, pcrLabel,
                                    true, nextCallInsn, rollback);
    /*
     * Make sure range list doesn't span the break between in normal
     * Dalvik vRegs and the ins.
     */
    int highestArg = oatGetSrc(cUnit, mir, numArgs-1).sRegLow;
    int boundaryReg = cUnit->method->NumRegisters() - cUnit->method->NumIns();
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
    for (int i = 4; i < numArgs; i++) {
        RegLocation loc = oatGetSrc(cUnit, mir, i);
        //TODO: generic loc flushing routine
        if (loc.wide) {
            loc = oatUpdateLocWide(cUnit, loc);
            if (loc.location == kLocPhysReg) {  // TUNING: if dirty?
                storeBaseDispWide(cUnit, rSP, loc.spOffset, loc.lowReg,
                                  loc.highReg);
                callState = nextCallInsn(cUnit, mir, dInsn, callState,
                                         rollback);
            }
        } else {
            loc = oatUpdateLoc(cUnit, loc);
            if (loc.location == kLocPhysReg) {  // TUNING: if dirty?
                storeBaseDisp(cUnit, rSP, loc.spOffset, loc.lowReg, kWord);
                callState = nextCallInsn(cUnit, mir, dInsn, callState,
                                         rollback);
            }
        }
    }

    int startOffset = cUnit->regLocation[mir->ssaRep->uses[3]].spOffset;
    int outsOffset = 4 /* Method* */ + (3 * 4);
    if (numArgs >= 20) {
        // Generate memcpy, but first make sure all of
        opRegRegImm(cUnit, kOpAdd, r0, rSP, startOffset);
        opRegRegImm(cUnit, kOpAdd, r1, rSP, outsOffset);
        loadWordDisp(cUnit, rSELF, OFFSETOF_MEMBER(Thread, pMemcpy), rLR);
        loadConstant(cUnit, r2, (numArgs - 3) * 4);
        newLIR1(cUnit, kThumbBlxR, rLR);
    } else {
        // Use vldm/vstm pair using r3 as a temp
        int regsLeft = std::min(numArgs - 3, 16);
        callState = nextCallInsn(cUnit, mir, dInsn, callState, rollback);
        opRegRegImm(cUnit, kOpAdd, r3, rSP, startOffset);
        newLIR3(cUnit, kThumb2Vldms, r3, fr0, regsLeft);
        callState = nextCallInsn(cUnit, mir, dInsn, callState, rollback);
        opRegRegImm(cUnit, kOpAdd, r3, rSP, 4 /* Method* */ + (3 * 4));
        callState = nextCallInsn(cUnit, mir, dInsn, callState, rollback);
        newLIR3(cUnit, kThumb2Vstms, r3, fr0, regsLeft);
        callState = nextCallInsn(cUnit, mir, dInsn, callState, rollback);
    }

    // Handle the 1st 3 in r1, r2 & r3
    for (unsigned int i = 0; i < dInsn->vA && i < 3; i++) {
        RegLocation loc = oatGetSrc(cUnit, mir, firstArg + i);
        loadValueDirectFixed(cUnit, loc, r1 + i);
        callState = nextCallInsn(cUnit, mir, dInsn, callState, rollback);
    }

    // Finally, deal with the register arguments
    // We'll be using fixed registers here
    oatLockCallTemps(cUnit);
    callState = nextCallInsn(cUnit, mir, dInsn, callState, rollback);
    return callState;
}

static void genInvokeStaticDirect(CompilationUnit* cUnit, MIR* mir,
                                  bool direct, bool range)
{
    DecodedInstruction* dInsn = &mir->dalvikInsn;
    int callState = 0;
    ArmLIR* nullCk;
    ArmLIR** pNullCk = direct ? &nullCk : NULL;

    NextCallInsn nextCallInsn = nextSDCallInsn;

    if (range) {
        callState = genDalvikArgsRange(cUnit, mir, dInsn, callState, pNullCk,
                                       nextCallInsn, NULL);
    } else {
        callState = genDalvikArgsNoRange(cUnit, mir, dInsn, callState, pNullCk,
                                         false, nextCallInsn, NULL);
    }
    // Finish up any of the call sequence not interleaved in arg loading
    while (callState >= 0) {
        callState = nextCallInsn(cUnit, mir, dInsn, callState, NULL);
    }
    newLIR1(cUnit, kThumbBlxR, rLR);
}

static void genInvokeInterface(CompilationUnit* cUnit, MIR* mir)
{
    DecodedInstruction* dInsn = &mir->dalvikInsn;
    int callState = 0;
    ArmLIR* nullCk;
    /* Note: must call nextInterfaceCallInsn() prior to 1st argument load */
    callState = nextInterfaceCallInsn(cUnit, mir, dInsn, callState, NULL);
    if (mir->dalvikInsn.opcode == OP_INVOKE_INTERFACE)
        callState = genDalvikArgsNoRange(cUnit, mir, dInsn, callState, &nullCk,
                                         false, nextInterfaceCallInsn, NULL);
    else
        callState = genDalvikArgsRange(cUnit, mir, dInsn, callState, &nullCk,
                                       nextInterfaceCallInsn, NULL);
    // Finish up any of the call sequence not interleaved in arg loading
    while (callState >= 0) {
        callState = nextInterfaceCallInsn(cUnit, mir, dInsn, callState, NULL);
    }
    newLIR1(cUnit, kThumbBlxR, rLR);
}

static void genInvokeSuper(CompilationUnit* cUnit, MIR* mir)
{
    DecodedInstruction* dInsn = &mir->dalvikInsn;
    int callState = 0;
    ArmLIR* nullCk;
// TODO - redundantly loading arg0/r1 ("this")
    if (mir->dalvikInsn.opcode == OP_INVOKE_SUPER)
        callState = genDalvikArgsNoRange(cUnit, mir, dInsn, callState, &nullCk,
                                         false, nextSuperCallInsn, NULL);
    else
        callState = genDalvikArgsRange(cUnit, mir, dInsn, callState, &nullCk,
                                       nextSuperCallInsn, NULL);
    // Finish up any of the call sequence not interleaved in arg loading
    while (callState >= 0) {
        callState = nextSuperCallInsn(cUnit, mir, dInsn, callState, NULL);
    }
    newLIR1(cUnit, kThumbBlxR, rLR);
}

static void genInvokeVirtual(CompilationUnit* cUnit, MIR* mir)
{
    DecodedInstruction* dInsn = &mir->dalvikInsn;
    int callState = 0;
    ArmLIR* nullCk;
    ArmLIR* rollback;
    Method* method = cUnit->method->GetDexCacheResolvedMethods()->
        Get(dInsn->vB);
    NextCallInsn nextCallInsn;

    if (method == NULL) {
        // Slow path
        nextCallInsn = nextVCallInsnSP;
        // If we need a slow-path callout, we'll restart here
        rollback = newLIR0(cUnit, kArmPseudoTargetLabel);
        rollback->defMask = -1;
    } else {
        // Fast path
        nextCallInsn = nextVCallInsn;
        rollback = NULL;
    }
    // TODO - redundantly loading arg0/r1 ("this")
    if (mir->dalvikInsn.opcode == OP_INVOKE_VIRTUAL)
        callState = genDalvikArgsNoRange(cUnit, mir, dInsn, callState, &nullCk,
                                         false, nextCallInsn, rollback);
    else
        callState = genDalvikArgsRange(cUnit, mir, dInsn, callState, &nullCk,
                                       nextCallInsn, rollback);
    // Finish up any of the call sequence not interleaved in arg loading
    while (callState >= 0) {
        callState = nextCallInsn(cUnit, mir, dInsn, callState, rollback);
    }
    newLIR1(cUnit, kThumbBlxR, rLR);
}

// TODO: break out the case handlers.  Might make it easier to support x86
static bool compileDalvikInstruction(CompilationUnit* cUnit, MIR* mir,
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
            break;

        case OP_RETURN:
        case OP_RETURN_OBJECT:
            storeValue(cUnit, retLoc, rlSrc[0]);
            break;

        case OP_RETURN_WIDE:
            rlDest = retLocWide;
            rlDest.fp = rlSrc[0].fp;
            storeValueWide(cUnit, rlDest, rlSrc[0]);
            break;

        case OP_MOVE_RESULT_WIDE:
            if (mir->OptimizationFlags & MIR_INLINED)
                break;  // Nop - combined w/ previous invoke
            /*
             * Somewhat hacky here.   Because we're now passing
             * return values in registers, we have to let the
             * register allocation utilities know that the return
             * registers are live and may not be used for address
             * formation in storeValueWide.
             */
            assert(retLocWide.lowReg == r0);
            assert(retLocWide.lowReg == r1);
            oatLockTemp(cUnit, retLocWide.lowReg);
            oatLockTemp(cUnit, retLocWide.highReg);
            storeValueWide(cUnit, rlDest, retLocWide);
            oatFreeTemp(cUnit, retLocWide.lowReg);
            oatFreeTemp(cUnit, retLocWide.highReg);
            break;

        case OP_MOVE_RESULT:
        case OP_MOVE_RESULT_OBJECT:
            if (mir->OptimizationFlags & MIR_INLINED)
                break;  // Nop - combined w/ previous invoke
            /* See comment for OP_MOVE_RESULT_WIDE */
            assert(retLoc.lowReg == r0);
            oatLockTemp(cUnit, retLoc.lowReg);
            storeValue(cUnit, rlDest, retLoc);
            oatFreeTemp(cUnit, retLoc.lowReg);
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
            rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
            loadConstantNoClobber(cUnit, rlResult.lowReg, mir->dalvikInsn.vB);
            //TUNING: do high separately to avoid load dependency
            opRegRegImm(cUnit, kOpAsr, rlResult.highReg, rlResult.lowReg, 31);
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

        case OP_ARRAY_LENGTH:
            int lenOffset;
            lenOffset = Array::LengthOffset().Int32Value();
            rlSrc[0] = loadValue(cUnit, rlSrc[0], kCoreReg);
            genNullCheck(cUnit, rlSrc[0].sRegLow, rlSrc[0].lowReg,
                         mir->offset, NULL);
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
            // TUNING: add MIR flag to disable when unnecessary
            bool backwardBranch;
            backwardBranch = (bb->taken->startOffset <= mir->offset);
            if (backwardBranch) {
                genSuspendPoll(cUnit, mir);
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
                genSuspendPoll(cUnit, mir);
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
                genSuspendPoll(cUnit, mir);
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

        case OP_IGET_WIDE:
        case OP_IGET_WIDE_VOLATILE:
            genIGetWideX(cUnit, mir, rlDest, rlSrc[0]);
            break;

        case OP_IGET:
        case OP_IGET_VOLATILE:
        case OP_IGET_OBJECT:
        case OP_IGET_OBJECT_VOLATILE:
            genIGetX(cUnit, mir, kWord, rlDest, rlSrc[0]);
            break;

        case OP_IGET_BOOLEAN:
        case OP_IGET_BYTE:
            genIGetX(cUnit, mir, kUnsignedByte, rlDest, rlSrc[0]);
            break;

        case OP_IGET_CHAR:
            genIGetX(cUnit, mir, kUnsignedHalf, rlDest, rlSrc[0]);
            break;

        case OP_IGET_SHORT:
            genIGetX(cUnit, mir, kSignedHalf, rlDest, rlSrc[0]);
            break;

        case OP_IPUT_WIDE:
        case OP_IPUT_WIDE_VOLATILE:
            genIPutWideX(cUnit, mir, rlSrc[0], rlSrc[1]);
            break;

        case OP_IPUT_OBJECT:
        case OP_IPUT_OBJECT_VOLATILE:
            genIPutX(cUnit, mir, kWord, rlSrc[0], rlSrc[1], true);
            break;

        case OP_IPUT:
        case OP_IPUT_VOLATILE:
            genIPutX(cUnit, mir, kWord, rlSrc[0], rlSrc[1], false);
            break;

        case OP_IPUT_BOOLEAN:
        case OP_IPUT_BYTE:
            genIPutX(cUnit, mir, kUnsignedByte, rlSrc[0], rlSrc[1], false);
            break;

        case OP_IPUT_CHAR:
            genIPutX(cUnit, mir, kUnsignedHalf, rlSrc[0], rlSrc[1], false);
            break;

        case OP_IPUT_SHORT:
            genIPutX(cUnit, mir, kSignedHalf, rlSrc[0], rlSrc[1], false);
            break;

        case OP_SGET:
        case OP_SGET_OBJECT:
        case OP_SGET_BOOLEAN:
        case OP_SGET_BYTE:
        case OP_SGET_CHAR:
        case OP_SGET_SHORT:
            genSget(cUnit, mir, rlResult, rlDest);
            break;

        case OP_SGET_WIDE:
            genSgetWide(cUnit, mir, rlResult, rlDest);
            break;

        case OP_SPUT:
        case OP_SPUT_OBJECT:
        case OP_SPUT_BOOLEAN:
        case OP_SPUT_BYTE:
        case OP_SPUT_CHAR:
        case OP_SPUT_SHORT:
            genSput(cUnit, mir, rlSrc[0]);
            break;

        case OP_SPUT_WIDE:
            genSputWide(cUnit, mir, rlSrc[0]);
            break;

        case OP_INVOKE_STATIC_RANGE:
            genInvokeStaticDirect(cUnit, mir, false /*direct*/,
                                  true /*range*/);
            break;
        case OP_INVOKE_STATIC:
            genInvokeStaticDirect(cUnit, mir, false /*direct*/,
                                  false /*range*/);
            break;

        case OP_INVOKE_DIRECT:
            genInvokeStaticDirect(cUnit, mir, true /*direct*/,
                                  false /*range*/);
            break;
        case OP_INVOKE_DIRECT_RANGE:
            genInvokeStaticDirect(cUnit, mir, true /*direct*/,
                                  true /*range*/);
            break;

        case OP_INVOKE_VIRTUAL:
        case OP_INVOKE_VIRTUAL_RANGE:
            genInvokeVirtual(cUnit, mir);
            break;

        case OP_INVOKE_SUPER:
        case OP_INVOKE_SUPER_RANGE:
            genInvokeSuper(cUnit, mir);
            break;

        case OP_INVOKE_INTERFACE:
        case OP_INVOKE_INTERFACE_RANGE:
            genInvokeInterface(cUnit, mir);
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

static const char *extendedMIROpNames[kMirOpLast - kMirOpFirst] = {
    "kMirOpPhi",
    "kMirOpNullNRangeUpCheck",
    "kMirOpNullNRangeDownCheck",
    "kMirOpLowerBound",
    "kMirOpPunt",
    "kMirOpCheckInlinePrediction",
};

/* Extended MIR instructions like PHI */
static void handleExtendedMethodMIR(CompilationUnit* cUnit, MIR* mir)
{
    int opOffset = mir->dalvikInsn.opcode - kMirOpFirst;
    char* msg = (char*)oatNew(strlen(extendedMIROpNames[opOffset]) + 1, false);
    strcpy(msg, extendedMIROpNames[opOffset]);
    ArmLIR* op = newLIR1(cUnit, kArmPseudoExtended, (int) msg);

    switch ((ExtendedMIROpcode)mir->dalvikInsn.opcode) {
        case kMirOpPhi: {
            char* ssaString = oatGetSSAString(cUnit, mir->ssaRep);
            op->flags.isNop = true;
            newLIR1(cUnit, kArmPseudoSSARep, (int) ssaString);
            break;
        }
        default:
            break;
    }
}

/* If there are any ins passed in registers that have not been promoted
 * to a callee-save register, flush them to the frame.
 * Note: at this pointCopy any ins that are passed in register to their
 * home location */
static void flushIns(CompilationUnit* cUnit)
{
    if (cUnit->method->NumIns() == 0)
        return;
    int inRegs = (cUnit->method->NumIns() > 2) ? 3
                                               : cUnit->method->NumIns();
    int startReg = r1;
    int startLoc = cUnit->method->NumRegisters() -
        cUnit->method->NumIns();
    for (int i = 0; i < inRegs; i++) {
        RegLocation loc = cUnit->regLocation[startLoc + i];
        //TUNING: be smarter about flushing ins to frame
        storeBaseDisp(cUnit, rSP, loc.spOffset, startReg + i, kWord);
        if (loc.location == kLocPhysReg) {
            genRegCopy(cUnit, loc.lowReg, startReg + i);
        }
    }

    // Handle special case of wide argument half in regs, half in frame
    if (inRegs == 3) {
        RegLocation loc = cUnit->regLocation[startLoc + 2];
        if (loc.wide && loc.location == kLocPhysReg) {
            // Load the other half of the arg into the promoted pair
            loadWordDisp(cUnit, rSP, loc.spOffset + 4, loc.highReg);
            inRegs++;
        }
    }

    // Now, do initial assignment of all promoted arguments passed in frame
    for (int i = inRegs; i < cUnit->method->NumIns();) {
        RegLocation loc = cUnit->regLocation[startLoc + i];
        if (loc.fpLocation == kLocPhysReg) {
            loc.location = kLocPhysReg;
            loc.fp = true;
            loc.lowReg = loc.fpLowReg;
            loc.highReg = loc.fpHighReg;
        }
        if (loc.location == kLocPhysReg) {
            if (loc.wide) {
                loadBaseDispWide(cUnit, NULL, rSP, loc.spOffset,
                                 loc.lowReg, loc.highReg, INVALID_SREG);
                i++;
            } else {
                loadWordDisp(cUnit, rSP, loc.spOffset, loc.lowReg);
            }
        }
        i++;
    }
}

/* Handle the content in each basic block */
static bool methodBlockCodeGen(CompilationUnit* cUnit, BasicBlock* bb)
{
    MIR* mir;
    ArmLIR* labelList = (ArmLIR*) cUnit->blockLabelList;
    int blockId = bb->id;

    cUnit->curBlock = bb;
    labelList[blockId].operands[0] = bb->startOffset;

    /* Insert the block label */
    labelList[blockId].opcode = kArmPseudoNormalBlockLabel;
    oatAppendLIR(cUnit, (LIR*) &labelList[blockId]);

    oatClobberAllRegs(cUnit);
    oatResetNullCheck(cUnit);

    ArmLIR* headLIR = NULL;

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
        newLIR0(cUnit, kArmPseudoMethodEntry);
        /* Spill core callee saves */
        newLIR1(cUnit, kThumb2Push, cUnit->coreSpillMask);
        /* Need to spill any FP regs? */
        if (cUnit->numFPSpills) {
            newLIR1(cUnit, kThumb2VPushCS, cUnit->numFPSpills);
        }
        opRegImm(cUnit, kOpSub, rSP, cUnit->frameSize - (cUnit->numSpills * 4));
        storeBaseDisp(cUnit, rSP, 0, r0, kWord);
        flushIns(cUnit);
        oatFreeTemp(cUnit, r0);
        oatFreeTemp(cUnit, r1);
        oatFreeTemp(cUnit, r2);
        oatFreeTemp(cUnit, r3);
    } else if (bb->blockType == kExitBlock) {
        newLIR0(cUnit, kArmPseudoMethodExit);
        opRegImm(cUnit, kOpAdd, rSP, cUnit->frameSize - (cUnit->numSpills * 4));
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
        boundaryLIR = newLIR1(cUnit, kArmPseudoDalvikByteCodeBoundary,
                             (int) oatGetDalvikDisassembly(
                             &mir->dalvikInsn, ""));
        /* Remember the first LIR for this block */
        if (headLIR == NULL) {
            headLIR = boundaryLIR;
            /* Set the first boundaryLIR as a scheduling barrier */
            headLIR->defMask = ENCODE_ALL;
        }

        /* Don't generate the SSA annotation unless verbose mode is on */
        if (cUnit->printMe && mir->ssaRep) {
            char *ssaString = oatGetSSAString(cUnit, mir->ssaRep);
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

void oatMethodMIR2LIR(CompilationUnit* cUnit)
{
    /* Used to hold the labels of each block */
    cUnit->blockLabelList =
        (void *) oatNew(sizeof(ArmLIR) * cUnit->numBlocks, true);

    oatDataFlowAnalysisDispatcher(cUnit, methodBlockCodeGen,
                                  kPreOrderDFSTraversal, false /* Iterative */);
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
