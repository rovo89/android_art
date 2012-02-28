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

#if defined(TARGET_ARM)
LIR* genIT(CompilationUnit* cUnit, ArmConditionCode cond, const char* guide);
#endif

LIR* callRuntimeHelper(CompilationUnit* cUnit, int reg)
{
    oatClobberCalleeSave(cUnit);
    return opReg(cUnit, kOpBlx, reg);
}

/*
 * Generate an kPseudoBarrier marker to indicate the boundary of special
 * blocks.
 */
void genBarrier(CompilationUnit* cUnit)
{
    LIR* barrier = newLIR0(cUnit, kPseudoBarrier);
    /* Mark all resources as being clobbered */
    barrier->defMask = -1;
}

/* Generate conditional branch instructions */
LIR* genConditionalBranch(CompilationUnit* cUnit, ConditionCode cond,
                          LIR* target)
{
    LIR* branch = opCondBranch(cUnit, cond);
    branch->target = (LIR*) target;
    return branch;
}

/* Generate unconditional branch instructions */
LIR* genUnconditionalBranch(CompilationUnit* cUnit, LIR* target)
{
    LIR* branch = opNone(cUnit, kOpUncondBr);
    branch->target = (LIR*) target;
    return branch;
}

LIR* genCheck(CompilationUnit* cUnit, ConditionCode cCode, MIR* mir,
              ThrowKind kind)
{
    LIR* tgt = (LIR*)oatNew(cUnit, sizeof(LIR), true, kAllocLIR);
    tgt->opcode = kPseudoThrowTarget;
    tgt->operands[0] = kind;
    tgt->operands[1] = mir ? mir->offset : 0;
    LIR* branch = genConditionalBranch(cUnit, cCode, tgt);
    // Remember branch target - will process later
    oatInsertGrowableList(cUnit, &cUnit->throwLaunchpads, (intptr_t)tgt);
    return branch;
}

LIR* genImmedCheck(CompilationUnit* cUnit, ConditionCode cCode,
                   int reg, int immVal, MIR* mir, ThrowKind kind)
{
    LIR* tgt = (LIR*)oatNew(cUnit, sizeof(LIR), true, kAllocLIR);
    tgt->opcode = kPseudoThrowTarget;
    tgt->operands[0] = kind;
    tgt->operands[1] = mir->offset;
    LIR* branch;
    if (cCode == kCondAl) {
        branch = genUnconditionalBranch(cUnit, tgt);
    } else {
        branch = genCmpImmBranch(cUnit, cCode, reg, immVal);
        branch->target = (LIR*)tgt;
    }
    // Remember branch target - will process later
    oatInsertGrowableList(cUnit, &cUnit->throwLaunchpads, (intptr_t)tgt);
    return branch;
}

/* Perform null-check on a register.  */
LIR* genNullCheck(CompilationUnit* cUnit, int sReg, int mReg, MIR* mir)
{
    if (!(cUnit->disableOpt & (1 << kNullCheckElimination)) &&
        mir->optimizationFlags & MIR_IGNORE_NULL_CHECK) {
        return NULL;
    }
    return genImmedCheck(cUnit, kCondEq, mReg, 0, mir, kThrowNullPointer);
}

/* Perform check on two registers */
LIR* genRegRegCheck(CompilationUnit* cUnit, ConditionCode cCode,
                        int reg1, int reg2, MIR* mir, ThrowKind kind)
{
    LIR* tgt = (LIR*)oatNew(cUnit, sizeof(LIR), true, kAllocLIR);
    tgt->opcode = kPseudoThrowTarget;
    tgt->operands[0] = kind;
    tgt->operands[1] = mir ? mir->offset : 0;
    tgt->operands[2] = reg1;
    tgt->operands[3] = reg2;
    opRegReg(cUnit, kOpCmp, reg1, reg2);
    LIR* branch = genConditionalBranch(cUnit, cCode, tgt);
    // Remember branch target - will process later
    oatInsertGrowableList(cUnit, &cUnit->throwLaunchpads, (intptr_t)tgt);
    return branch;
}

void genCompareAndBranch(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                         RegLocation rlSrc1, RegLocation rlSrc2, LIR* labelList)
{
    ConditionCode cond;
    rlSrc1 = loadValue(cUnit, rlSrc1, kCoreReg);
    rlSrc2 = loadValue(cUnit, rlSrc2, kCoreReg);
    opRegReg(cUnit, kOpCmp, rlSrc1.lowReg, rlSrc2.lowReg);
    Opcode opcode = mir->dalvikInsn.opcode;
    switch(opcode) {
        case OP_IF_EQ:
            cond = kCondEq;
            break;
        case OP_IF_NE:
            cond = kCondNe;
            break;
        case OP_IF_LT:
            cond = kCondLt;
            break;
        case OP_IF_GE:
            cond = kCondGe;
            break;
        case OP_IF_GT:
            cond = kCondGt;
            break;
        case OP_IF_LE:
            cond = kCondLe;
            break;
        default:
            cond = (ConditionCode)0;
            LOG(FATAL) << "Unexpected opcode " << (int)opcode;
    }
    genConditionalBranch(cUnit, cond, &labelList[bb->taken->id]);
    genUnconditionalBranch(cUnit, &labelList[bb->fallThrough->id]);
}

void genCompareZeroAndBranch(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                             RegLocation rlSrc, LIR* labelList)
{
    ConditionCode cond;
    rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
    opRegImm(cUnit, kOpCmp, rlSrc.lowReg, 0);
    Opcode opcode = mir->dalvikInsn.opcode;
    switch(opcode) {
        case OP_IF_EQZ:
            cond = kCondEq;
            break;
        case OP_IF_NEZ:
            cond = kCondNe;
            break;
        case OP_IF_LTZ:
            cond = kCondLt;
            break;
        case OP_IF_GEZ:
            cond = kCondGe;
            break;
        case OP_IF_GTZ:
            cond = kCondGt;
            break;
        case OP_IF_LEZ:
            cond = kCondLe;
            break;
        default:
            cond = (ConditionCode)0;
            LOG(FATAL) << "Unexpected opcode " << (int)opcode;
    }
    genConditionalBranch(cUnit, cond, &labelList[bb->taken->id]);
    genUnconditionalBranch(cUnit, &labelList[bb->fallThrough->id]);
}

void genIntToLong(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                  RegLocation rlSrc)
{
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    if (rlSrc.location == kLocPhysReg) {
        genRegCopy(cUnit, rlResult.lowReg, rlSrc.lowReg);
    } else {
        loadValueDirect(cUnit, rlSrc, rlResult.lowReg);
    }
    opRegRegImm(cUnit, kOpAsr, rlResult.highReg,
                rlResult.lowReg, 31);
    storeValueWide(cUnit, rlDest, rlResult);
}

void genIntNarrowing(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                     RegLocation rlSrc)
{
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
}

/*
 * Let helper function take care of everything.  Will call
 * Array::AllocFromCode(type_idx, method, count);
 * Note: AllocFromCode will handle checks for errNegativeArraySize.
 */
void genNewArray(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                 RegLocation rlSrc)
{
    oatFlushAllRegs(cUnit);    /* Everything to home location */
    uint32_t type_idx = mir->dalvikInsn.vC;
    int rTgt;
    if (cUnit->compiler->CanAccessTypeWithoutChecks(cUnit->method_idx,
                                                    cUnit->dex_cache,
                                                    *cUnit->dex_file,
                                                    type_idx)) {
        rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread, pAllocArrayFromCode));
    } else {
        rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread,
                          pAllocArrayFromCodeWithAccessCheck));
    }
    loadCurrMethodDirect(cUnit, rARG1);              // arg1 <- Method*
    loadConstant(cUnit, rARG0, type_idx);            // arg0 <- type_id
    loadValueDirectFixed(cUnit, rlSrc, rARG2);       // arg2 <- count
    callRuntimeHelper(cUnit, rTgt);
    RegLocation rlResult = oatGetReturn(cUnit);
    storeValue(cUnit, rlDest, rlResult);
}

/*
 * Similar to genNewArray, but with post-allocation initialization.
 * Verifier guarantees we're dealing with an array class.  Current
 * code throws runtime exception "bad Filled array req" for 'D' and 'J'.
 * Current code also throws internal unimp if not 'L', '[' or 'I'.
 */
void genFilledNewArray(CompilationUnit* cUnit, MIR* mir, bool isRange)
{
    DecodedInstruction* dInsn = &mir->dalvikInsn;
    int elems = dInsn->vA;
    int typeId = dInsn->vB;
    oatFlushAllRegs(cUnit);    /* Everything to home location */
    int rTgt;
    if (cUnit->compiler->CanAccessTypeWithoutChecks(cUnit->method_idx,
                                                    cUnit->dex_cache,
                                                    *cUnit->dex_file,
                                                    typeId)) {
        rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread,
                          pCheckAndAllocArrayFromCode));
    } else {
        rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread,
                          pCheckAndAllocArrayFromCodeWithAccessCheck));
    }
    loadCurrMethodDirect(cUnit, rARG1);              // arg1 <- Method*
    loadConstant(cUnit, rARG0, typeId);              // arg0 <- type_id
    loadConstant(cUnit, rARG2, elems);               // arg2 <- count
    callRuntimeHelper(cUnit, rTgt);
    /*
     * NOTE: the implicit target for OP_FILLED_NEW_ARRAY is the
     * return region.  Because AllocFromCode placed the new array
     * in rRET0, we'll just lock it into place.  When debugger support is
     * added, it may be necessary to additionally copy all return
     * values to a home location in thread-local storage
     */
    oatLockTemp(cUnit, rRET0);

    // TODO: use the correct component size, currently all supported types
    // share array alignment with ints (see comment at head of function)
    size_t component_size = sizeof(int32_t);

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
        opRegRegImm(cUnit, kOpAdd, rDst, rRET0,
                    Array::DataOffset(component_size).Int32Value());
        // Set up the loop counter (known to be > 0)
        loadConstant(cUnit, rIdx, dInsn->vA - 1);
        // Generate the copy loop.  Going backwards for convenience
        LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
        target->defMask = ENCODE_ALL;
        // Copy next element
        loadBaseIndexed(cUnit, rSrc, rIdx, rVal, 2, kWord);
        storeBaseIndexed(cUnit, rDst, rIdx, rVal, 2, kWord);
#if defined(TARGET_ARM)
        // Combine sub & test using sub setflags encoding here
        newLIR3(cUnit, kThumb2SubsRRI12, rIdx, rIdx, 1);
        LIR* branch = opCondBranch(cUnit, kCondGe);
#else
        opRegImm(cUnit, kOpSub, rIdx, 1);
        LIR* branch = opCompareBranchImm(cUnit, kCondGe, rIdx, 0);
#endif
        branch->target = (LIR*)target;
    } else if (!isRange) {
        // TUNING: interleave
        for (unsigned int i = 0; i < dInsn->vA; i++) {
            RegLocation rlArg = loadValue(cUnit,
                oatGetSrc(cUnit, mir, i), kCoreReg);
            storeBaseDisp(cUnit, rRET0,
                          Array::DataOffset(component_size).Int32Value() +
                          i * 4, rlArg.lowReg, kWord);
            // If the loadValue caused a temp to be allocated, free it
            if (oatIsTemp(cUnit, rlArg.lowReg)) {
                oatFreeTemp(cUnit, rlArg.lowReg);
            }
        }
    }
}

void genSput(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc,
             bool isLongOrDouble, bool isObject)
{
    int fieldOffset;
    int ssbIndex;
    bool isVolatile;
    bool isReferrersClass;
    uint32_t fieldIdx = mir->dalvikInsn.vB;

    OatCompilationUnit mUnit(cUnit->class_loader, cUnit->class_linker,
                             *cUnit->dex_file, *cUnit->dex_cache,
                             cUnit->code_item, cUnit->method_idx,
                             cUnit->access_flags);

    bool fastPath =
        cUnit->compiler->ComputeStaticFieldInfo(fieldIdx, &mUnit,
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
            rMethod = rARG1;
            oatLockTemp(cUnit, rMethod);
            loadCurrMethodDirect(cUnit, rMethod);
            rBase = rARG0;
            oatLockTemp(cUnit, rBase);
            loadWordDisp(cUnit, rMethod,
                Method::DexCacheInitializedStaticStorageOffset().Int32Value(),
                rBase);
            loadWordDisp(cUnit, rBase,
                         Array::DataOffset(sizeof(Object*)).Int32Value() + sizeof(int32_t*) *
                         ssbIndex, rBase);
            // rBase now points at appropriate static storage base (Class*)
            // or NULL if not initialized. Check for NULL and call helper if NULL.
            // TUNING: fast path should fall through
            LIR* branchOver = genCmpImmBranch(cUnit, kCondNe, rBase, 0);
            int rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread,
                                  pInitializeStaticStorage));
            loadConstant(cUnit, rARG0, ssbIndex);
            callRuntimeHelper(cUnit, rTgt);
#if defined(TARGET_MIPS)
            // For Arm, rRET0 = rARG0 = rBASE, for Mips, we need to copy
            genRegCopy(cUnit, rBase, rRET0);
#endif
            LIR* skipTarget = newLIR0(cUnit, kPseudoTargetLabel);
            skipTarget->defMask = ENCODE_ALL;
            branchOver->target = (LIR*)skipTarget;
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
//FIXME: need to generalize the barrier call
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
        int rTgt = loadHelper(cUnit, setterOffset);
        loadConstant(cUnit, rARG0, fieldIdx);
        if (isLongOrDouble) {
            loadValueDirectWideFixed(cUnit, rlSrc, rARG2, rARG3);
        } else {
            loadValueDirect(cUnit, rlSrc, rARG1);
        }
        callRuntimeHelper(cUnit, rTgt);
    }
}

void genSget(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
             bool isLongOrDouble, bool isObject)
{
    int fieldOffset;
    int ssbIndex;
    bool isVolatile;
    bool isReferrersClass;
    uint32_t fieldIdx = mir->dalvikInsn.vB;

    OatCompilationUnit mUnit(cUnit->class_loader, cUnit->class_linker,
                             *cUnit->dex_file, *cUnit->dex_cache,
                             cUnit->code_item, cUnit->method_idx,
                             cUnit->access_flags);

    bool fastPath =
        cUnit->compiler->ComputeStaticFieldInfo(fieldIdx, &mUnit,
                                                fieldOffset, ssbIndex,
                                                isReferrersClass, isVolatile,
                                                false);
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
            rMethod = rARG1;
            oatLockTemp(cUnit, rMethod);
            loadCurrMethodDirect(cUnit, rMethod);
            rBase = rARG0;
            oatLockTemp(cUnit, rBase);
            loadWordDisp(cUnit, rMethod,
                Method::DexCacheInitializedStaticStorageOffset().Int32Value(),
                rBase);
            loadWordDisp(cUnit, rBase,
                         Array::DataOffset(sizeof(Object*)).Int32Value() +
                         sizeof(int32_t*) * ssbIndex,
                         rBase);
            // rBase now points at appropriate static storage base (Class*)
            // or NULL if not initialized. Check for NULL and call helper if NULL.
            // TUNING: fast path should fall through
            LIR* branchOver = genCmpImmBranch(cUnit, kCondNe, rBase, 0);
            int rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread,
                                  pInitializeStaticStorage));
            loadConstant(cUnit, rARG0, ssbIndex);
            callRuntimeHelper(cUnit, rTgt);
#if defined(TARGET_MIPS)
            // For Arm, rRET0 = rARG0 = rBASE, for Mips, we need to copy
            genRegCopy(cUnit, rBase, rRET0);
#endif
            LIR* skipTarget = newLIR0(cUnit, kPseudoTargetLabel);
            skipTarget->defMask = ENCODE_ALL;
            branchOver->target = (LIR*)skipTarget;
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
        int rTgt = loadHelper(cUnit, getterOffset);
        loadConstant(cUnit, rARG0, fieldIdx);
        callRuntimeHelper(cUnit, rTgt);
        if (isLongOrDouble) {
            RegLocation rlResult = oatGetReturnWide(cUnit);
            storeValueWide(cUnit, rlDest, rlResult);
        } else {
            RegLocation rlResult = oatGetReturn(cUnit);
            storeValue(cUnit, rlDest, rlResult);
        }
    }
}


// Debugging routine - if null target, branch to DebugMe
void genShowTarget(CompilationUnit* cUnit)
{
    LIR* branchOver = genCmpImmBranch(cUnit, kCondNe, rLINK, 0);
    loadWordDisp(cUnit, rSELF,
                 OFFSETOF_MEMBER(Thread, pDebugMe), rLINK);
    LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
    target->defMask = -1;
    branchOver->target = (LIR*)target;
}

void genThrowVerificationError(CompilationUnit* cUnit, MIR* mir)
{
    int rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread,
                          pThrowVerificationErrorFromCode));
    loadConstant(cUnit, rARG0, mir->dalvikInsn.vA);
    loadConstant(cUnit, rARG1, mir->dalvikInsn.vB);
    callRuntimeHelper(cUnit, rTgt);
}

void handleSuspendLaunchpads(CompilationUnit *cUnit)
{
    LIR** suspendLabel =
        (LIR **) cUnit->suspendLaunchpads.elemList;
    int numElems = cUnit->suspendLaunchpads.numUsed;

    for (int i = 0; i < numElems; i++) {
        /* TUNING: move suspend count load into helper */
        LIR* lab = suspendLabel[i];
        LIR* resumeLab = (LIR*)lab->operands[0];
        cUnit->currentDalvikOffset = lab->operands[1];
        oatAppendLIR(cUnit, (LIR *)lab);
        int rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread,
                              pTestSuspendFromCode));
        if (!cUnit->genDebugger) {
            // use rSUSPEND for suspend count
            loadWordDisp(cUnit, rSELF,
                         Thread::SuspendCountOffset().Int32Value(), rSUSPEND);
        }
        opReg(cUnit, kOpBlx, rTgt);
        if ( cUnit->genDebugger) {
            // use rSUSPEND for update debugger
            loadWordDisp(cUnit, rSELF,
                         OFFSETOF_MEMBER(Thread, pUpdateDebuggerFromCode),
                         rSUSPEND);
        }
        genUnconditionalBranch(cUnit, resumeLab);
    }
}

void handleThrowLaunchpads(CompilationUnit *cUnit)
{
    LIR** throwLabel = (LIR **) cUnit->throwLaunchpads.elemList;
    int numElems = cUnit->throwLaunchpads.numUsed;
    int i;

    for (i = 0; i < numElems; i++) {
        LIR* lab = throwLabel[i];
        cUnit->currentDalvikOffset = lab->operands[1];
        oatAppendLIR(cUnit, (LIR *)lab);
        int funcOffset = 0;
        int v1 = lab->operands[2];
        int v2 = lab->operands[3];
        switch(lab->operands[0]) {
            case kThrowNullPointer:
                funcOffset = OFFSETOF_MEMBER(Thread, pThrowNullPointerFromCode);
                break;
            case kThrowArrayBounds:
                if (v2 != r0) {
                    genRegCopy(cUnit, rARG0, v1);
                    genRegCopy(cUnit, rARG1, v2);
                } else {
                    if (v1 == r1) {
#if defined(TARGET_ARM)
                        int rTmp = r12;
#else
                        int rTmp = oatAllocTemp(cUnit);
#endif
                        genRegCopy(cUnit, rTmp, v1);
                        genRegCopy(cUnit, rARG1, v2);
                        genRegCopy(cUnit, rARG0, rTmp);
#if !(defined(TARGET_ARM))
                        oatFreeTemp(cUnit, rTmp);
#endif
                    } else {
                        genRegCopy(cUnit, rARG1, v2);
                        genRegCopy(cUnit, rARG0, v1);
                    }
                }
                funcOffset = OFFSETOF_MEMBER(Thread, pThrowArrayBoundsFromCode);
                break;
            case kThrowDivZero:
                funcOffset = OFFSETOF_MEMBER(Thread, pThrowDivZeroFromCode);
                break;
            case kThrowVerificationError:
                loadConstant(cUnit, rARG0, v1);
                loadConstant(cUnit, rARG1, v2);
                funcOffset =
                    OFFSETOF_MEMBER(Thread, pThrowVerificationErrorFromCode);
                break;
            case kThrowNegArraySize:
                genRegCopy(cUnit, rARG0, v1);
                funcOffset =
                    OFFSETOF_MEMBER(Thread, pThrowNegArraySizeFromCode);
                break;
            case kThrowNoSuchMethod:
                genRegCopy(cUnit, rARG0, v1);
                funcOffset =
                    OFFSETOF_MEMBER(Thread, pThrowNoSuchMethodFromCode);
                break;
            case kThrowStackOverflow:
                funcOffset =
                    OFFSETOF_MEMBER(Thread, pThrowStackOverflowFromCode);
                // Restore stack alignment
                opRegImm(cUnit, kOpAdd, rSP,
                         (cUnit->numCoreSpills + cUnit->numFPSpills) * 4);
                break;
            default:
                LOG(FATAL) << "Unexpected throw kind: " << lab->operands[0];
        }
        int rTgt = loadHelper(cUnit, funcOffset);
        callRuntimeHelper(cUnit, rTgt);
    }
}

/* Needed by the Assembler */
void oatSetupResourceMasks(LIR* lir)
{
    setupResourceMasks(lir);
}

void genIGet(CompilationUnit* cUnit, MIR* mir, OpSize size,
             RegLocation rlDest, RegLocation rlObj,
                    bool isLongOrDouble, bool isObject)
{
    int fieldOffset;
    bool isVolatile;
    uint32_t fieldIdx = mir->dalvikInsn.vC;

    OatCompilationUnit mUnit(cUnit->class_loader, cUnit->class_linker,
                             *cUnit->dex_file, *cUnit->dex_cache,
                             cUnit->code_item, cUnit->method_idx,
                             cUnit->access_flags);

    bool fastPath = cUnit->compiler->ComputeInstanceFieldInfo(fieldIdx, &mUnit,
                    fieldOffset, isVolatile, false);

    if (fastPath && !SLOW_FIELD_PATH) {
        RegLocation rlResult;
        RegisterClass regClass = oatRegClassBySize(size);
        DCHECK_GE(fieldOffset, 0);
        rlObj = loadValue(cUnit, rlObj, kCoreReg);
        if (isLongOrDouble) {
            DCHECK(rlDest.wide);
            genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, mir);/* null? */
            int regPtr = oatAllocTemp(cUnit);
            opRegRegImm(cUnit, kOpAdd, regPtr, rlObj.lowReg, fieldOffset);
            rlResult = oatEvalLoc(cUnit, rlDest, regClass, true);
            loadPair(cUnit, regPtr, rlResult.lowReg, rlResult.highReg);
            if (isVolatile) {
                oatGenMemBarrier(cUnit, kSY);
            }
            oatFreeTemp(cUnit, regPtr);
            storeValueWide(cUnit, rlDest, rlResult);
        } else {
            rlResult = oatEvalLoc(cUnit, rlDest, regClass, true);
            genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, mir);/* null? */
            loadBaseDisp(cUnit, mir, rlObj.lowReg, fieldOffset, rlResult.lowReg,
                         kWord, rlObj.sRegLow);
            if (isVolatile) {
                oatGenMemBarrier(cUnit, kSY);
            }
            storeValue(cUnit, rlDest, rlResult);
        }
    } else {
        int getterOffset = isLongOrDouble ? OFFSETOF_MEMBER(Thread, pGet64Instance) :
                           (isObject ? OFFSETOF_MEMBER(Thread, pGetObjInstance)
                                     : OFFSETOF_MEMBER(Thread, pGet32Instance));
        int rTgt = loadHelper(cUnit, getterOffset);
        loadValueDirect(cUnit, rlObj, rARG1);
        loadConstant(cUnit, rARG0, fieldIdx);
        callRuntimeHelper(cUnit, rTgt);
        if (isLongOrDouble) {
            RegLocation rlResult = oatGetReturnWide(cUnit);
            storeValueWide(cUnit, rlDest, rlResult);
        } else {
            RegLocation rlResult = oatGetReturn(cUnit);
            storeValue(cUnit, rlDest, rlResult);
        }
    }
}

void genIPut(CompilationUnit* cUnit, MIR* mir, OpSize size, RegLocation rlSrc,
             RegLocation rlObj, bool isLongOrDouble, bool isObject)
{
    int fieldOffset;
    bool isVolatile;
    uint32_t fieldIdx = mir->dalvikInsn.vC;

    OatCompilationUnit mUnit(cUnit->class_loader, cUnit->class_linker,
                             *cUnit->dex_file, *cUnit->dex_cache,
                             cUnit->code_item, cUnit->method_idx,
                             cUnit->access_flags);

    bool fastPath = cUnit->compiler->ComputeInstanceFieldInfo(fieldIdx, &mUnit,
                    fieldOffset, isVolatile, true);
    if (fastPath && !SLOW_FIELD_PATH) {
        RegisterClass regClass = oatRegClassBySize(size);
        DCHECK_GE(fieldOffset, 0);
        rlObj = loadValue(cUnit, rlObj, kCoreReg);
        if (isLongOrDouble) {
            int regPtr;
            rlSrc = loadValueWide(cUnit, rlSrc, kAnyReg);
            genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, mir);/* null? */
            regPtr = oatAllocTemp(cUnit);
            opRegRegImm(cUnit, kOpAdd, regPtr, rlObj.lowReg, fieldOffset);
            if (isVolatile) {
                oatGenMemBarrier(cUnit, kST);
            }
            storePair(cUnit, regPtr, rlSrc.lowReg, rlSrc.highReg);
            if (isVolatile) {
                oatGenMemBarrier(cUnit, kSY);
            }
            oatFreeTemp(cUnit, regPtr);
        } else {
            rlSrc = loadValue(cUnit, rlSrc, regClass);
            genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, mir);/* null? */
            if (isVolatile) {
                oatGenMemBarrier(cUnit, kST);
            }
            storeBaseDisp(cUnit, rlObj.lowReg, fieldOffset, rlSrc.lowReg, kWord);
            if (isVolatile) {
                oatGenMemBarrier(cUnit, kSY);
            }
        }
    } else {
        int setterOffset = isLongOrDouble ? OFFSETOF_MEMBER(Thread, pSet64Instance) :
                           (isObject ? OFFSETOF_MEMBER(Thread, pSetObjInstance)
                                     : OFFSETOF_MEMBER(Thread, pSet32Instance));
        int rTgt = loadHelper(cUnit, setterOffset);
        loadValueDirect(cUnit, rlObj, rARG1);
        if (isLongOrDouble) {
            loadValueDirectWide(cUnit, rlSrc, rARG2, rARG3);
        } else {
            loadValueDirect(cUnit, rlSrc, rARG2);
        }
        loadConstant(cUnit, rARG0, fieldIdx);
        callRuntimeHelper(cUnit, rTgt);
    }
}

void genConstClass(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                   RegLocation rlSrc)
{
    uint32_t type_idx = mir->dalvikInsn.vB;
    int mReg = loadCurrMethod(cUnit);
    int resReg = oatAllocTemp(cUnit);
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    if (!cUnit->compiler->CanAccessTypeWithoutChecks(cUnit->method_idx,
                                                     cUnit->dex_cache,
                                                     *cUnit->dex_file,
                                                     type_idx)) {
        // Call out to helper which resolves type and verifies access.
        // Resolved type returned in rRET0.
        int rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread,
                              pInitializeTypeAndVerifyAccessFromCode));
        genRegCopy(cUnit, rARG1, mReg);
        loadConstant(cUnit, rARG0, type_idx);
        callRuntimeHelper(cUnit, rTgt);
        RegLocation rlResult = oatGetReturn(cUnit);
        storeValue(cUnit, rlDest, rlResult);
    } else {
        // We're don't need access checks, load type from dex cache
        int32_t dex_cache_offset =
            Method::DexCacheResolvedTypesOffset().Int32Value();
        loadWordDisp(cUnit, mReg, dex_cache_offset, resReg);
        int32_t offset_of_type =
            Array::DataOffset(sizeof(Class*)).Int32Value() + (sizeof(Class*)
                              * type_idx);
        loadWordDisp(cUnit, resReg, offset_of_type, rlResult.lowReg);
        if (!cUnit->compiler->CanAssumeTypeIsPresentInDexCache(cUnit->dex_cache,
                type_idx) || SLOW_TYPE_PATH) {
            // Slow path, at runtime test if type is null and if so initialize
            oatFlushAllRegs(cUnit);
            LIR* branch1 = genCmpImmBranch(cUnit, kCondEq, rlResult.lowReg, 0);
            // Resolved, store and hop over following code
            storeValue(cUnit, rlDest, rlResult);
            LIR* branch2 = genUnconditionalBranch(cUnit,0);
            // TUNING: move slow path to end & remove unconditional branch
            LIR* target1 = newLIR0(cUnit, kPseudoTargetLabel);
            target1->defMask = ENCODE_ALL;
            // Call out to helper, which will return resolved type in r0
            int rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread,
                                  pInitializeTypeFromCode));
            genRegCopy(cUnit, rARG1, mReg);
            loadConstant(cUnit, rARG0, type_idx);
            callRuntimeHelper(cUnit, rTgt);
            RegLocation rlResult = oatGetReturn(cUnit);
            storeValue(cUnit, rlDest, rlResult);
            // Rejoin code paths
            LIR* target2 = newLIR0(cUnit, kPseudoTargetLabel);
            target2->defMask = ENCODE_ALL;
            branch1->target = (LIR*)target1;
            branch2->target = (LIR*)target2;
        } else {
            // Fast path, we're done - just store result
            storeValue(cUnit, rlDest, rlResult);
        }
    }
}
void genConstString(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                    RegLocation rlSrc)
{
    /* NOTE: Most strings should be available at compile time */
    uint32_t string_idx = mir->dalvikInsn.vB;
    int32_t offset_of_string = Array::DataOffset(sizeof(String*)).Int32Value() +
                               (sizeof(String*) * string_idx);
    if (!cUnit->compiler->CanAssumeStringIsPresentInDexCache(
            cUnit->dex_cache, string_idx) || SLOW_STRING_PATH) {
        // slow path, resolve string if not in dex cache
        oatFlushAllRegs(cUnit);
        oatLockCallTemps(cUnit); // Using explicit registers
        loadCurrMethodDirect(cUnit, rARG2);
        loadWordDisp(cUnit, rARG2,
                     Method::DexCacheStringsOffset().Int32Value(), rARG0);
        // Might call out to helper, which will return resolved string in rRET0
        int rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread,
                              pResolveStringFromCode));
        loadWordDisp(cUnit, rRET0, offset_of_string, rARG0);
        loadConstant(cUnit, rARG1, string_idx);
#if defined(TARGET_ARM)
        opRegImm(cUnit, kOpCmp, rRET0, 0);  // Is resolved?
        genBarrier(cUnit);
        // For testing, always force through helper
        if (!EXERCISE_SLOWEST_STRING_PATH) {
            genIT(cUnit, kArmCondEq, "T");
        }
        genRegCopy(cUnit, rARG0, rARG2);   // .eq
        opReg(cUnit, kOpBlx, rTgt);        // .eq, helper(Method*, string_idx)
#else
        LIR* branch = genCmpImmBranch(cUnit, kCondNe, 0);
        genRegCopy(cUnit, rARG0, rARG2);   // .eq
        opReg(cUnit, kOpBlx, rTgt);
        LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
        target->defMask = ENCODE_ALL;
        branch->target = target;
#endif
        genBarrier(cUnit);
        storeValue(cUnit, rlDest, getRetLoc(cUnit));
    } else {
        int mReg = loadCurrMethod(cUnit);
        int resReg = oatAllocTemp(cUnit);
        RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
        loadWordDisp(cUnit, mReg,
                     Method::DexCacheStringsOffset().Int32Value(), resReg);
        loadWordDisp(cUnit, resReg, offset_of_string, rlResult.lowReg);
        storeValue(cUnit, rlDest, rlResult);
    }
}

/*
 * Let helper function take care of everything.  Will
 * call Class::NewInstanceFromCode(type_idx, method);
 */
void genNewInstance(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest)
{
    oatFlushAllRegs(cUnit);    /* Everything to home location */
    uint32_t type_idx = mir->dalvikInsn.vB;
    // alloc will always check for resolution, do we also need to verify
    // access because the verifier was unable to?
    int rTgt;
    if (cUnit->compiler->CanAccessInstantiableTypeWithoutChecks(
            cUnit->method_idx, cUnit->dex_cache, *cUnit->dex_file, type_idx)) {
        rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread, pAllocObjectFromCode));
    } else {
        rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread,
                          pAllocObjectFromCodeWithAccessCheck));
    }
    loadCurrMethodDirect(cUnit, rARG1);    // arg1 <= Method*
    loadConstant(cUnit, rARG0, type_idx);  // arg0 <- type_idx
    callRuntimeHelper(cUnit, rTgt);
    RegLocation rlResult = oatGetReturn(cUnit);
    storeValue(cUnit, rlDest, rlResult);
}

void genInstanceof(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                   RegLocation rlSrc)
{
    oatFlushAllRegs(cUnit);
    // May generate a call - use explicit registers
    oatLockCallTemps(cUnit);
    uint32_t type_idx = mir->dalvikInsn.vC;
    loadCurrMethodDirect(cUnit, rARG1);  // r1 <= current Method*
    int classReg = rARG2;  // rARG2 will hold the Class*
    if (!cUnit->compiler->CanAccessTypeWithoutChecks(cUnit->method_idx,
                                                     cUnit->dex_cache,
                                                     *cUnit->dex_file,
                                                     type_idx)) {
        // Check we have access to type_idx and if not throw IllegalAccessError,
        // returns Class* in r0
        int rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread,
                              pInitializeTypeAndVerifyAccessFromCode));
        loadConstant(cUnit, rARG0, type_idx);
        callRuntimeHelper(cUnit, rTgt);  // InitializeTypeAndVerifyAccess(idx, method)
        genRegCopy(cUnit, classReg, rRET0);  // Align usage with fast path
        loadValueDirectFixed(cUnit, rlSrc, rARG0);  // r0 <= ref
    } else {
        // Load dex cache entry into classReg (r2)
        loadValueDirectFixed(cUnit, rlSrc, rARG0);  // rARG0 <= ref
        loadWordDisp(cUnit, rARG1,
                     Method::DexCacheResolvedTypesOffset().Int32Value(),
                     classReg);
        int32_t offset_of_type =
            Array::DataOffset(sizeof(Class*)).Int32Value() + (sizeof(Class*)
            * type_idx);
        loadWordDisp(cUnit, classReg, offset_of_type, classReg);
        if (!cUnit->compiler->CanAssumeTypeIsPresentInDexCache(
                cUnit->dex_cache, type_idx)) {
            // Need to test presence of type in dex cache at runtime
            LIR* hopBranch = genCmpImmBranch(cUnit, kCondNe, classReg, 0);
            // Not resolved
            // Call out to helper, which will return resolved type in rRET0
            int rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread,
                                  pInitializeTypeFromCode));
            loadConstant(cUnit, rARG0, type_idx);
            callRuntimeHelper(cUnit, rTgt);  // InitializeTypeFromCode(idx, method)
            genRegCopy(cUnit, r2, rRET0); // Align usage with fast path
            loadValueDirectFixed(cUnit, rlSrc, rARG0);  /* reload Ref */
            // Rejoin code paths
            LIR* hopTarget = newLIR0(cUnit, kPseudoTargetLabel);
            hopTarget->defMask = ENCODE_ALL;
            hopBranch->target = (LIR*)hopTarget;
        }
    }
    /* rARG0 is ref, rARG2 is class. If ref==null, use directly as bool result */
    LIR* branch1 = genCmpImmBranch(cUnit, kCondEq, rARG0, 0);
    /* load object->clazz */
    DCHECK_EQ(Object::ClassOffset().Int32Value(), 0);
    loadWordDisp(cUnit, rARG0,  Object::ClassOffset().Int32Value(), rARG1);
    /* rARG0 is ref, rARG1 is ref->clazz, rARG2 is class */
    int rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread,
                          pInstanceofNonTrivialFromCode));
#if defined(TARGET_ARM)
    opRegReg(cUnit, kOpCmp, rARG1, rARG2);  // Same?
    genBarrier(cUnit);
    genIT(cUnit, kArmCondEq, "EE");   // if-convert the test
    loadConstant(cUnit, rARG0, 1);       // .eq case - load true
    genRegCopy(cUnit, rARG0, rARG2);        // .ne case - arg0 <= class
    opReg(cUnit, kOpBlx, rTgt);        // .ne case: helper(class, ref->class)
    genBarrier(cUnit);
    oatClobberCalleeSave(cUnit);
#else
    // Perhaps a general-purpose kOpSelect operator?
    UNIMPLEMENTED(FATAL) << "Need non IT implementation";
#endif
    /* branch target here */
    LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
    target->defMask = ENCODE_ALL;
    RegLocation rlResult = oatGetReturn(cUnit);
    storeValue(cUnit, rlDest, rlResult);
    branch1->target = (LIR*)target;
}

void genCheckCast(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc)
{
    oatFlushAllRegs(cUnit);
    // May generate a call - use explicit registers
    oatLockCallTemps(cUnit);
    uint32_t type_idx = mir->dalvikInsn.vB;
    loadCurrMethodDirect(cUnit, rARG1);  // rARG1 <= current Method*
    int classReg = rARG2;  // rARG2 will hold the Class*
    if (!cUnit->compiler->CanAccessTypeWithoutChecks(cUnit->method_idx,
                                                     cUnit->dex_cache,
                                                     *cUnit->dex_file,
                                                     type_idx)) {
        // Check we have access to type_idx and if not throw IllegalAccessError,
        // returns Class* in rRET0
        int rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread,
                              pInitializeTypeAndVerifyAccessFromCode));
        loadConstant(cUnit, rARG0, type_idx);
        callRuntimeHelper(cUnit, rTgt);  // InitializeTypeAndVerifyAccess(idx, method)
        genRegCopy(cUnit, classReg, rRET0);  // Align usage with fast path
    } else {
        // Load dex cache entry into classReg (rARG2)
        loadWordDisp(cUnit, rARG1,
                     Method::DexCacheResolvedTypesOffset().Int32Value(),
                     classReg);
        int32_t offset_of_type =
                Array::DataOffset(sizeof(Class*)).Int32Value() +
                (sizeof(Class*) * type_idx);
        loadWordDisp(cUnit, classReg, offset_of_type, classReg);
        if (!cUnit->compiler->CanAssumeTypeIsPresentInDexCache(
                cUnit->dex_cache, type_idx)) {
            // Need to test presence of type in dex cache at runtime
            LIR* hopBranch = genCmpImmBranch(cUnit, kCondNe, classReg, 0);
            // Not resolved
            // Call out to helper, which will return resolved type in r0
            loadWordDisp(cUnit, rSELF, OFFSETOF_MEMBER(Thread, pInitializeTypeFromCode), rLR);
            loadConstant(cUnit, r0, type_idx);
            callRuntimeHelper(cUnit, rLR);  // InitializeTypeFromCode(idx, method)
            genRegCopy(cUnit, classReg, r0); // Align usage with fast path
            // Rejoin code paths
            LIR* hopTarget = newLIR0(cUnit, kPseudoTargetLabel);
            hopTarget->defMask = ENCODE_ALL;
            hopBranch->target = (LIR*)hopTarget;
        }
    }
    // At this point, classReg (r2) has class
    loadValueDirectFixed(cUnit, rlSrc, rARG0);  // rARG0 <= ref
    /* Null is OK - continue */
    LIR* branch1 = genCmpImmBranch(cUnit, kCondEq, rARG0, 0);
    /* load object->clazz */
    DCHECK_EQ(Object::ClassOffset().Int32Value(), 0);
    loadWordDisp(cUnit, rARG0,  Object::ClassOffset().Int32Value(), rARG1);
    /* rARG1 now contains object->clazz */
    int rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread,
                          pCheckCastFromCode));
    opRegReg(cUnit, kOpCmp, rARG1, classReg);
    LIR* branch2 = opCondBranch(cUnit, kCondEq); /* If equal, trivial yes */
    genRegCopy(cUnit, rARG0, rARG1);
    genRegCopy(cUnit, rARG1, rARG2);
    callRuntimeHelper(cUnit, rTgt);
    /* branch target here */
    LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
    target->defMask = ENCODE_ALL;
    branch1->target = (LIR*)target;
    branch2->target = (LIR*)target;
}


void genThrow(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc)
{
    oatFlushAllRegs(cUnit);
    int rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread, pDeliverException));
    loadValueDirectFixed(cUnit, rlSrc, rARG0);  // Get exception object
    callRuntimeHelper(cUnit, rTgt);  // art_deliver_exception(exception);
}

/*
 * Generate array store
 *
 */
void genArrayObjPut(CompilationUnit* cUnit, MIR* mir, RegLocation rlArray,
                    RegLocation rlIndex, RegLocation rlSrc, int scale)
{
    RegisterClass regClass = oatRegClassBySize(kWord);
    int lenOffset = Array::LengthOffset().Int32Value();
    int dataOffset = Array::DataOffset(sizeof(Object*)).Int32Value();

    oatFlushAllRegs(cUnit);
    /* Make sure it's a legal object Put. Use direct regs at first */
    loadValueDirectFixed(cUnit, rlArray, rARG1);
    loadValueDirectFixed(cUnit, rlSrc, rARG0);

    /* null array object? */
    genNullCheck(cUnit, rlArray.sRegLow, rARG1, mir);
    int rTgt = loadHelper(cUnit, OFFSETOF_MEMBER(Thread,
                          pCanPutArrayElementFromCode));
    /* Get the array's clazz */
    loadWordDisp(cUnit, rARG1, Object::ClassOffset().Int32Value(), rARG1);
    callRuntimeHelper(cUnit, rTgt);
    oatFreeTemp(cUnit, rARG0);
    oatFreeTemp(cUnit, rARG1);

    // Now, redo loadValues in case they didn't survive the call

    int regPtr;
    rlArray = loadValue(cUnit, rlArray, kCoreReg);
    rlIndex = loadValue(cUnit, rlIndex, kCoreReg);

    if (oatIsTemp(cUnit, rlArray.lowReg)) {
        oatClobber(cUnit, rlArray.lowReg);
        regPtr = rlArray.lowReg;
    } else {
        regPtr = oatAllocTemp(cUnit);
        genRegCopy(cUnit, regPtr, rlArray.lowReg);
    }

    if (!(mir->optimizationFlags & MIR_IGNORE_RANGE_CHECK)) {
        int regLen = oatAllocTemp(cUnit);
        //NOTE: max live temps(4) here.
        /* Get len */
        loadWordDisp(cUnit, rlArray.lowReg, lenOffset, regLen);
        /* regPtr -> array data */
        opRegImm(cUnit, kOpAdd, regPtr, dataOffset);
        genRegRegCheck(cUnit, kCondCs, rlIndex.lowReg, regLen, mir,
                       kThrowArrayBounds);
        oatFreeTemp(cUnit, regLen);
    } else {
        /* regPtr -> array data */
        opRegImm(cUnit, kOpAdd, regPtr, dataOffset);
    }
    /* at this point, regPtr points to array, 2 live temps */
    rlSrc = loadValue(cUnit, rlSrc, regClass);
    storeBaseIndexed(cUnit, regPtr, rlIndex.lowReg, rlSrc.lowReg,
                     scale, kWord);
}

/*
 * Generate array load
 */
void genArrayGet(CompilationUnit* cUnit, MIR* mir, OpSize size,
                 RegLocation rlArray, RegLocation rlIndex,
                 RegLocation rlDest, int scale)
{
    RegisterClass regClass = oatRegClassBySize(size);
    int lenOffset = Array::LengthOffset().Int32Value();
    int dataOffset;
    RegLocation rlResult;
    rlArray = loadValue(cUnit, rlArray, kCoreReg);
    rlIndex = loadValue(cUnit, rlIndex, kCoreReg);
    int regPtr;

    if (size == kLong || size == kDouble) {
      dataOffset = Array::DataOffset(sizeof(int64_t)).Int32Value();
    } else {
      dataOffset = Array::DataOffset(sizeof(int32_t)).Int32Value();
    }

    /* null object? */
    genNullCheck(cUnit, rlArray.sRegLow, rlArray.lowReg, mir);

    regPtr = oatAllocTemp(cUnit);

    if (!(mir->optimizationFlags & MIR_IGNORE_RANGE_CHECK)) {
        int regLen = oatAllocTemp(cUnit);
        /* Get len */
        loadWordDisp(cUnit, rlArray.lowReg, lenOffset, regLen);
        /* regPtr -> array data */
        opRegRegImm(cUnit, kOpAdd, regPtr, rlArray.lowReg, dataOffset);
        genRegRegCheck(cUnit, kCondCs, rlIndex.lowReg, regLen, mir,
                       kThrowArrayBounds);
        oatFreeTemp(cUnit, regLen);
    } else {
        /* regPtr -> array data */
        opRegRegImm(cUnit, kOpAdd, regPtr, rlArray.lowReg, dataOffset);
    }
    oatFreeTemp(cUnit, rlArray.lowReg);
    if ((size == kLong) || (size == kDouble)) {
        if (scale) {
            int rNewIndex = oatAllocTemp(cUnit);
            opRegRegImm(cUnit, kOpLsl, rNewIndex, rlIndex.lowReg, scale);
            opRegReg(cUnit, kOpAdd, regPtr, rNewIndex);
            oatFreeTemp(cUnit, rNewIndex);
        } else {
            opRegReg(cUnit, kOpAdd, regPtr, rlIndex.lowReg);
        }
        oatFreeTemp(cUnit, rlIndex.lowReg);
        rlResult = oatEvalLoc(cUnit, rlDest, regClass, true);

        loadPair(cUnit, regPtr, rlResult.lowReg, rlResult.highReg);

        oatFreeTemp(cUnit, regPtr);
        storeValueWide(cUnit, rlDest, rlResult);
    } else {
        rlResult = oatEvalLoc(cUnit, rlDest, regClass, true);

        loadBaseIndexed(cUnit, regPtr, rlIndex.lowReg, rlResult.lowReg,
                        scale, size);

        oatFreeTemp(cUnit, regPtr);
        storeValue(cUnit, rlDest, rlResult);
    }
}

/*
 * Generate array store
 *
 */
void genArrayPut(CompilationUnit* cUnit, MIR* mir, OpSize size,
                 RegLocation rlArray, RegLocation rlIndex,
                 RegLocation rlSrc, int scale)
{
    RegisterClass regClass = oatRegClassBySize(size);
    int lenOffset = Array::LengthOffset().Int32Value();
    int dataOffset;

    if (size == kLong || size == kDouble) {
      dataOffset = Array::DataOffset(sizeof(int64_t)).Int32Value();
    } else {
      dataOffset = Array::DataOffset(sizeof(int32_t)).Int32Value();
    }

    int regPtr;
    rlArray = loadValue(cUnit, rlArray, kCoreReg);
    rlIndex = loadValue(cUnit, rlIndex, kCoreReg);

    if (oatIsTemp(cUnit, rlArray.lowReg)) {
        oatClobber(cUnit, rlArray.lowReg);
        regPtr = rlArray.lowReg;
    } else {
        regPtr = oatAllocTemp(cUnit);
        genRegCopy(cUnit, regPtr, rlArray.lowReg);
    }

    /* null object? */
    genNullCheck(cUnit, rlArray.sRegLow, rlArray.lowReg, mir);

    if (!(mir->optimizationFlags & MIR_IGNORE_RANGE_CHECK)) {
        int regLen = oatAllocTemp(cUnit);
        //NOTE: max live temps(4) here.
        /* Get len */
        loadWordDisp(cUnit, rlArray.lowReg, lenOffset, regLen);
        /* regPtr -> array data */
        opRegImm(cUnit, kOpAdd, regPtr, dataOffset);
        genRegRegCheck(cUnit, kCondCs, rlIndex.lowReg, regLen, mir,
                       kThrowArrayBounds);
        oatFreeTemp(cUnit, regLen);
    } else {
        /* regPtr -> array data */
        opRegImm(cUnit, kOpAdd, regPtr, dataOffset);
    }
    /* at this point, regPtr points to array, 2 live temps */
    if ((size == kLong) || (size == kDouble)) {
        //TUNING: specific wide routine that can handle fp regs
        if (scale) {
            int rNewIndex = oatAllocTemp(cUnit);
            opRegRegImm(cUnit, kOpLsl, rNewIndex, rlIndex.lowReg, scale);
            opRegReg(cUnit, kOpAdd, regPtr, rNewIndex);
            oatFreeTemp(cUnit, rNewIndex);
        } else {
            opRegReg(cUnit, kOpAdd, regPtr, rlIndex.lowReg);
        }
        rlSrc = loadValueWide(cUnit, rlSrc, regClass);

        storePair(cUnit, regPtr, rlSrc.lowReg, rlSrc.highReg);

        oatFreeTemp(cUnit, regPtr);
    } else {
        rlSrc = loadValue(cUnit, rlSrc, regClass);

        storeBaseIndexed(cUnit, regPtr, rlIndex.lowReg, rlSrc.lowReg,
                         scale, size);
    }
}

void genLong3Addr(CompilationUnit* cUnit, MIR* mir, OpKind firstOp,
                  OpKind secondOp, RegLocation rlDest,
                         RegLocation rlSrc1, RegLocation rlSrc2)
{
    RegLocation rlResult;
#if defined(TARGET_ARM)
    /*
     * NOTE:  This is the one place in the code in which we might have
     * as many as six live temporary registers.  There are 5 in the normal
     * set for Arm.  Until we have spill capabilities, temporarily add
     * lr to the temp set.  It is safe to do this locally, but note that
     * lr is used explicitly elsewhere in the code generator and cannot
     * normally be used as a general temp register.
     */
    oatMarkTemp(cUnit, rLR);   // Add lr to the temp pool
    oatFreeTemp(cUnit, rLR);   // and make it available
#endif
    rlSrc1 = loadValueWide(cUnit, rlSrc1, kCoreReg);
    rlSrc2 = loadValueWide(cUnit, rlSrc2, kCoreReg);
    rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    // The longs may overlap - use intermediate temp if so
    if (rlResult.lowReg == rlSrc1.highReg) {
        int tReg = oatAllocTemp(cUnit);
        genRegCopy(cUnit, tReg, rlSrc1.highReg);
        opRegRegReg(cUnit, firstOp, rlResult.lowReg, rlSrc1.lowReg,
                    rlSrc2.lowReg);
        opRegRegReg(cUnit, secondOp, rlResult.highReg, tReg,
                    rlSrc2.highReg);
        oatFreeTemp(cUnit, tReg);
    } else {
        opRegRegReg(cUnit, firstOp, rlResult.lowReg, rlSrc1.lowReg,
                    rlSrc2.lowReg);
        opRegRegReg(cUnit, secondOp, rlResult.highReg, rlSrc1.highReg,
                    rlSrc2.highReg);
    }
    /*
     * NOTE: If rlDest refers to a frame variable in a large frame, the
     * following storeValueWide might need to allocate a temp register.
     * To further work around the lack of a spill capability, explicitly
     * free any temps from rlSrc1 & rlSrc2 that aren't still live in rlResult.
     * Remove when spill is functional.
     */
    freeRegLocTemps(cUnit, rlResult, rlSrc1);
    freeRegLocTemps(cUnit, rlResult, rlSrc2);
    storeValueWide(cUnit, rlDest, rlResult);
#if defined(TARGET_ARM)
    oatClobber(cUnit, rLR);
    oatUnmarkTemp(cUnit, rLR);  // Remove lr from the temp pool
#endif
}


bool genShiftOpLong(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                    RegLocation rlSrc1, RegLocation rlShift)
{
    int funcOffset;

    switch( mir->dalvikInsn.opcode) {
        case OP_SHL_LONG:
        case OP_SHL_LONG_2ADDR:
            funcOffset = OFFSETOF_MEMBER(Thread, pShlLong);
            break;
        case OP_SHR_LONG:
        case OP_SHR_LONG_2ADDR:
            funcOffset = OFFSETOF_MEMBER(Thread, pShrLong);
            break;
        case OP_USHR_LONG:
        case OP_USHR_LONG_2ADDR:
            funcOffset = OFFSETOF_MEMBER(Thread, pUshrLong);
            break;
        default:
            LOG(FATAL) << "Unexpected case";
            return true;
    }
    oatFlushAllRegs(cUnit);   /* Send everything to home location */
    int rTgt = loadHelper(cUnit, funcOffset);
    loadValueDirectWideFixed(cUnit, rlSrc1, rARG0, rARG1);
    loadValueDirect(cUnit, rlShift, rARG2);
    callRuntimeHelper(cUnit, rTgt);
    RegLocation rlResult = oatGetReturnWide(cUnit);
    storeValueWide(cUnit, rlDest, rlResult);
    return false;
}


bool genArithOpInt(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                   RegLocation rlSrc1, RegLocation rlSrc2)
{
    OpKind op = kOpBkpt;
    bool callOut = false;
    bool checkZero = false;
    bool unary = false;
    int retReg = rRET0;
    int funcOffset;
    RegLocation rlResult;
    bool shiftOp = false;

    switch (mir->dalvikInsn.opcode) {
        case OP_NEG_INT:
            op = kOpNeg;
            unary = true;
            break;
        case OP_NOT_INT:
            op = kOpMvn;
            unary = true;
            break;
        case OP_ADD_INT:
        case OP_ADD_INT_2ADDR:
            op = kOpAdd;
            break;
        case OP_SUB_INT:
        case OP_SUB_INT_2ADDR:
            op = kOpSub;
            break;
        case OP_MUL_INT:
        case OP_MUL_INT_2ADDR:
            op = kOpMul;
            break;
        case OP_DIV_INT:
        case OP_DIV_INT_2ADDR:
            callOut = true;
            checkZero = true;
            funcOffset = OFFSETOF_MEMBER(Thread, pIdiv);
            retReg = rRET0;
            break;
        /* NOTE: returns in r1 */
        case OP_REM_INT:
        case OP_REM_INT_2ADDR:
            callOut = true;
            checkZero = true;
            funcOffset = OFFSETOF_MEMBER(Thread, pIdivmod);
            retReg = rRET1;
            break;
        case OP_AND_INT:
        case OP_AND_INT_2ADDR:
            op = kOpAnd;
            break;
        case OP_OR_INT:
        case OP_OR_INT_2ADDR:
            op = kOpOr;
            break;
        case OP_XOR_INT:
        case OP_XOR_INT_2ADDR:
            op = kOpXor;
            break;
        case OP_SHL_INT:
        case OP_SHL_INT_2ADDR:
            shiftOp = true;
            op = kOpLsl;
            break;
        case OP_SHR_INT:
        case OP_SHR_INT_2ADDR:
            shiftOp = true;
            op = kOpAsr;
            break;
        case OP_USHR_INT:
        case OP_USHR_INT_2ADDR:
            shiftOp = true;
            op = kOpLsr;
            break;
        default:
            LOG(FATAL) << "Invalid word arith op: " <<
                (int)mir->dalvikInsn.opcode;
    }
    if (!callOut) {
        rlSrc1 = loadValue(cUnit, rlSrc1, kCoreReg);
        if (unary) {
            rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
            opRegReg(cUnit, op, rlResult.lowReg,
                     rlSrc1.lowReg);
        } else {
            rlSrc2 = loadValue(cUnit, rlSrc2, kCoreReg);
            if (shiftOp) {
                int tReg = oatAllocTemp(cUnit);
                opRegRegImm(cUnit, kOpAnd, tReg, rlSrc2.lowReg, 31);
                rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
                opRegRegReg(cUnit, op, rlResult.lowReg,
                            rlSrc1.lowReg, tReg);
                oatFreeTemp(cUnit, tReg);
            } else {
                rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
                opRegRegReg(cUnit, op, rlResult.lowReg,
                            rlSrc1.lowReg, rlSrc2.lowReg);
            }
        }
        storeValue(cUnit, rlDest, rlResult);
    } else {
        RegLocation rlResult;
        oatFlushAllRegs(cUnit);   /* Send everything to home location */
        loadValueDirectFixed(cUnit, rlSrc2, rRET1);
        int rTgt = loadHelper(cUnit, funcOffset);
        loadValueDirectFixed(cUnit, rlSrc1, rARG0);
        if (checkZero) {
            genImmedCheck(cUnit, kCondEq, rARG1, 0, mir, kThrowDivZero);
        }
        callRuntimeHelper(cUnit, rTgt);
        if (retReg == rRET0)
            rlResult = oatGetReturn(cUnit);
        else
            rlResult = oatGetReturnAlt(cUnit);
        storeValue(cUnit, rlDest, rlResult);
    }
    return false;
}

/*
 * The following are the first-level codegen routines that analyze the format
 * of each bytecode then either dispatch special purpose codegen routines
 * or produce corresponding Thumb instructions directly.
 */

bool isPowerOfTwo(int x)
{
    return (x & (x - 1)) == 0;
}

// Returns true if no more than two bits are set in 'x'.
bool isPopCountLE2(unsigned int x)
{
    x &= x - 1;
    return (x & (x - 1)) == 0;
}

// Returns the index of the lowest set bit in 'x'.
int lowestSetBit(unsigned int x) {
    int bit_posn = 0;
    while ((x & 0xf) == 0) {
        bit_posn += 4;
        x >>= 4;
    }
    while ((x & 1) == 0) {
        bit_posn++;
        x >>= 1;
    }
    return bit_posn;
}

// Returns true if it added instructions to 'cUnit' to divide 'rlSrc' by 'lit'
// and store the result in 'rlDest'.
bool handleEasyDivide(CompilationUnit* cUnit, Opcode dalvikOpcode,
                      RegLocation rlSrc, RegLocation rlDest, int lit)
{
    if (lit < 2 || !isPowerOfTwo(lit)) {
        return false;
    }
    int k = lowestSetBit(lit);
    if (k >= 30) {
        // Avoid special cases.
        return false;
    }
    bool div = (dalvikOpcode == OP_DIV_INT_LIT8 ||
                dalvikOpcode == OP_DIV_INT_LIT16);
    rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    if (div) {
        int tReg = oatAllocTemp(cUnit);
        if (lit == 2) {
            // Division by 2 is by far the most common division by constant.
            opRegRegImm(cUnit, kOpLsr, tReg, rlSrc.lowReg, 32 - k);
            opRegRegReg(cUnit, kOpAdd, tReg, tReg, rlSrc.lowReg);
            opRegRegImm(cUnit, kOpAsr, rlResult.lowReg, tReg, k);
        } else {
            opRegRegImm(cUnit, kOpAsr, tReg, rlSrc.lowReg, 31);
            opRegRegImm(cUnit, kOpLsr, tReg, tReg, 32 - k);
            opRegRegReg(cUnit, kOpAdd, tReg, tReg, rlSrc.lowReg);
            opRegRegImm(cUnit, kOpAsr, rlResult.lowReg, tReg, k);
        }
    } else {
        int cReg = oatAllocTemp(cUnit);
        loadConstant(cUnit, cReg, lit - 1);
        int tReg1 = oatAllocTemp(cUnit);
        int tReg2 = oatAllocTemp(cUnit);
        if (lit == 2) {
            opRegRegImm(cUnit, kOpLsr, tReg1, rlSrc.lowReg, 32 - k);
            opRegRegReg(cUnit, kOpAdd, tReg2, tReg1, rlSrc.lowReg);
            opRegRegReg(cUnit, kOpAnd, tReg2, tReg2, cReg);
            opRegRegReg(cUnit, kOpSub, rlResult.lowReg, tReg2, tReg1);
        } else {
            opRegRegImm(cUnit, kOpAsr, tReg1, rlSrc.lowReg, 31);
            opRegRegImm(cUnit, kOpLsr, tReg1, tReg1, 32 - k);
            opRegRegReg(cUnit, kOpAdd, tReg2, tReg1, rlSrc.lowReg);
            opRegRegReg(cUnit, kOpAnd, tReg2, tReg2, cReg);
            opRegRegReg(cUnit, kOpSub, rlResult.lowReg, tReg2, tReg1);
        }
    }
    storeValue(cUnit, rlDest, rlResult);
    return true;
}

void genMultiplyByTwoBitMultiplier(CompilationUnit* cUnit, RegLocation rlSrc,
                                   RegLocation rlResult, int lit,
                                   int firstBit, int secondBit)
{
    opRegRegRegShift(cUnit, kOpAdd, rlResult.lowReg, rlSrc.lowReg, rlSrc.lowReg,
                     encodeShift(kArmLsl, secondBit - firstBit));
    if (firstBit != 0) {
        opRegRegImm(cUnit, kOpLsl, rlResult.lowReg, rlResult.lowReg, firstBit);
    }
}

// Returns true if it added instructions to 'cUnit' to multiply 'rlSrc' by 'lit'
// and store the result in 'rlDest'.
bool handleEasyMultiply(CompilationUnit* cUnit, RegLocation rlSrc,
                        RegLocation rlDest, int lit)
{
    // Can we simplify this multiplication?
    bool powerOfTwo = false;
    bool popCountLE2 = false;
    bool powerOfTwoMinusOne = false;
    if (lit < 2) {
        // Avoid special cases.
        return false;
    } else if (isPowerOfTwo(lit)) {
        powerOfTwo = true;
    } else if (isPopCountLE2(lit)) {
        popCountLE2 = true;
    } else if (isPowerOfTwo(lit + 1)) {
        powerOfTwoMinusOne = true;
    } else {
        return false;
    }
    rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    if (powerOfTwo) {
        // Shift.
        opRegRegImm(cUnit, kOpLsl, rlResult.lowReg, rlSrc.lowReg,
                    lowestSetBit(lit));
    } else if (popCountLE2) {
        // Shift and add and shift.
        int firstBit = lowestSetBit(lit);
        int secondBit = lowestSetBit(lit ^ (1 << firstBit));
        genMultiplyByTwoBitMultiplier(cUnit, rlSrc, rlResult, lit,
                                      firstBit, secondBit);
    } else {
        // Reverse subtract: (src << (shift + 1)) - src.
        DCHECK(powerOfTwoMinusOne);
        // TUNING: rsb dst, src, src lsl#lowestSetBit(lit + 1)
        int tReg = oatAllocTemp(cUnit);
        opRegRegImm(cUnit, kOpLsl, tReg, rlSrc.lowReg, lowestSetBit(lit + 1));
        opRegRegReg(cUnit, kOpSub, rlResult.lowReg, tReg, rlSrc.lowReg);
    }
    storeValue(cUnit, rlDest, rlResult);
    return true;
}

bool genArithOpIntLit(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                      RegLocation rlSrc, int lit)
{
    Opcode dalvikOpcode = mir->dalvikInsn.opcode;
    RegLocation rlResult;
    OpKind op = (OpKind)0;      /* Make gcc happy */
    int shiftOp = false;
    bool isDiv = false;
    int funcOffset;
    int rTgt;

    switch (dalvikOpcode) {
        case OP_RSUB_INT_LIT8:
        case OP_RSUB_INT: {
            int tReg;
            //TUNING: add support for use of Arm rsub op
            rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
            tReg = oatAllocTemp(cUnit);
            loadConstant(cUnit, tReg, lit);
            rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
            opRegRegReg(cUnit, kOpSub, rlResult.lowReg,
                        tReg, rlSrc.lowReg);
            storeValue(cUnit, rlDest, rlResult);
            return false;
            break;
        }

        case OP_ADD_INT_LIT8:
        case OP_ADD_INT_LIT16:
            op = kOpAdd;
            break;
        case OP_MUL_INT_LIT8:
        case OP_MUL_INT_LIT16: {
            if (handleEasyMultiply(cUnit, rlSrc, rlDest, lit)) {
                return false;
            }
            op = kOpMul;
            break;
        }
        case OP_AND_INT_LIT8:
        case OP_AND_INT_LIT16:
            op = kOpAnd;
            break;
        case OP_OR_INT_LIT8:
        case OP_OR_INT_LIT16:
            op = kOpOr;
            break;
        case OP_XOR_INT_LIT8:
        case OP_XOR_INT_LIT16:
            op = kOpXor;
            break;
        case OP_SHL_INT_LIT8:
            lit &= 31;
            shiftOp = true;
            op = kOpLsl;
            break;
        case OP_SHR_INT_LIT8:
            lit &= 31;
            shiftOp = true;
            op = kOpAsr;
            break;
        case OP_USHR_INT_LIT8:
            lit &= 31;
            shiftOp = true;
            op = kOpLsr;
            break;

        case OP_DIV_INT_LIT8:
        case OP_DIV_INT_LIT16:
        case OP_REM_INT_LIT8:
        case OP_REM_INT_LIT16:
            if (lit == 0) {
                genImmedCheck(cUnit, kCondAl, 0, 0, mir, kThrowDivZero);
                return false;
            }
            if (handleEasyDivide(cUnit, dalvikOpcode, rlSrc, rlDest, lit)) {
                return false;
            }
            oatFlushAllRegs(cUnit);   /* Everything to home location */
            loadValueDirectFixed(cUnit, rlSrc, rARG0);
            oatClobber(cUnit, rARG0);
            if ((dalvikOpcode == OP_DIV_INT_LIT8) ||
                (dalvikOpcode == OP_DIV_INT_LIT16)) {
                funcOffset = OFFSETOF_MEMBER(Thread, pIdiv);
                isDiv = true;
            } else {
                funcOffset = OFFSETOF_MEMBER(Thread, pIdivmod);
                isDiv = false;
            }
            rTgt = loadHelper(cUnit, funcOffset);
            loadConstant(cUnit, rARG1, lit);
            callRuntimeHelper(cUnit, rTgt);
            if (isDiv)
                rlResult = oatGetReturn(cUnit);
            else
                rlResult = oatGetReturnAlt(cUnit);
            storeValue(cUnit, rlDest, rlResult);
            return false;
            break;
        default:
            return true;
    }
    rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
    rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    // Avoid shifts by literal 0 - no support in Thumb.  Change to copy
    if (shiftOp && (lit == 0)) {
        genRegCopy(cUnit, rlResult.lowReg, rlSrc.lowReg);
    } else {
        opRegRegImm(cUnit, op, rlResult.lowReg, rlSrc.lowReg, lit);
    }
    storeValue(cUnit, rlDest, rlResult);
    return false;
}

bool genArithOpLong(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                    RegLocation rlSrc1, RegLocation rlSrc2)
{
    RegLocation rlResult;
    OpKind firstOp = kOpBkpt;
    OpKind secondOp = kOpBkpt;
    bool callOut = false;
    bool checkZero = false;
    int funcOffset;
    int retReg = rRET0;

    switch (mir->dalvikInsn.opcode) {
        case OP_NOT_LONG:
            rlSrc2 = loadValueWide(cUnit, rlSrc2, kCoreReg);
            rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
            // Check for destructive overlap
            if (rlResult.lowReg == rlSrc2.highReg) {
                int tReg = oatAllocTemp(cUnit);
                genRegCopy(cUnit, tReg, rlSrc2.highReg);
                opRegReg(cUnit, kOpMvn, rlResult.lowReg, rlSrc2.lowReg);
                opRegReg(cUnit, kOpMvn, rlResult.highReg, tReg);
                oatFreeTemp(cUnit, tReg);
            } else {
                opRegReg(cUnit, kOpMvn, rlResult.lowReg, rlSrc2.lowReg);
                opRegReg(cUnit, kOpMvn, rlResult.highReg, rlSrc2.highReg);
            }
            storeValueWide(cUnit, rlDest, rlResult);
            return false;
            break;
        case OP_ADD_LONG:
        case OP_ADD_LONG_2ADDR:
            firstOp = kOpAdd;
            secondOp = kOpAdc;
            break;
        case OP_SUB_LONG:
        case OP_SUB_LONG_2ADDR:
            firstOp = kOpSub;
            secondOp = kOpSbc;
            break;
        case OP_MUL_LONG:
        case OP_MUL_LONG_2ADDR:
            callOut = true;
            retReg = rRET0;
            funcOffset = OFFSETOF_MEMBER(Thread, pLmul);
            break;
        case OP_DIV_LONG:
        case OP_DIV_LONG_2ADDR:
            callOut = true;
            checkZero = true;
            retReg = rRET0;
            funcOffset = OFFSETOF_MEMBER(Thread, pLdivmod);
            break;
        /* NOTE - result is in rARG2/rARG3 instead of rRET0/rRET1 */
        // FIXME: is true, or could be made true, or other targets?
        case OP_REM_LONG:
        case OP_REM_LONG_2ADDR:
            callOut = true;
            checkZero = true;
            funcOffset = OFFSETOF_MEMBER(Thread, pLdivmod);
            retReg = rARG2;
            break;
        case OP_AND_LONG_2ADDR:
        case OP_AND_LONG:
            firstOp = kOpAnd;
            secondOp = kOpAnd;
            break;
        case OP_OR_LONG:
        case OP_OR_LONG_2ADDR:
            firstOp = kOpOr;
            secondOp = kOpOr;
            break;
        case OP_XOR_LONG:
        case OP_XOR_LONG_2ADDR:
            firstOp = kOpXor;
            secondOp = kOpXor;
            break;
        case OP_NEG_LONG: {
            rlSrc2 = loadValueWide(cUnit, rlSrc2, kCoreReg);
            rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
            int zReg = oatAllocTemp(cUnit);
            loadConstantNoClobber(cUnit, zReg, 0);
            // Check for destructive overlap
            if (rlResult.lowReg == rlSrc2.highReg) {
                int tReg = oatAllocTemp(cUnit);
                opRegRegReg(cUnit, kOpSub, rlResult.lowReg,
                            zReg, rlSrc2.lowReg);
                opRegRegReg(cUnit, kOpSbc, rlResult.highReg,
                            zReg, tReg);
                oatFreeTemp(cUnit, tReg);
            } else {
                opRegRegReg(cUnit, kOpSub, rlResult.lowReg,
                            zReg, rlSrc2.lowReg);
                opRegRegReg(cUnit, kOpSbc, rlResult.highReg,
                            zReg, rlSrc2.highReg);
            }
            oatFreeTemp(cUnit, zReg);
            storeValueWide(cUnit, rlDest, rlResult);
            return false;
        }
        default:
            LOG(FATAL) << "Invalid long arith op";
    }
    if (!callOut) {
        genLong3Addr(cUnit, mir, firstOp, secondOp, rlDest, rlSrc1, rlSrc2);
    } else {
        int rTgt;
        oatFlushAllRegs(cUnit);   /* Send everything to home location */
        if (checkZero) {
            loadValueDirectWideFixed(cUnit, rlSrc2, rARG2, rARG3);
            rTgt = loadHelper(cUnit, funcOffset);
            loadValueDirectWideFixed(cUnit, rlSrc1, rARG0, rARG1);
            int tReg = oatAllocTemp(cUnit);
#if defined(TARGET_ARM)
            newLIR4(cUnit, kThumb2OrrRRRs, tReg, rARG2, rARG3, 0);
            oatFreeTemp(cUnit, tReg);
            genCheck(cUnit, kCondEq, mir, kThrowDivZero);
#else
            opRegRegReg(cUnit, kOpOr, tReg, rARG2, rARG3);
            genImmedCheck(cUnit, kCondEq, mir, tReg, 0, mir, kThrowDivZero);
            oatFreeTemp(cUnit, tReg);
#endif
        } else {
            rTgt = loadHelper(cUnit, funcOffset);
            loadValueDirectWideFixed(cUnit, rlSrc1, rARG0, rARG1);
            loadValueDirectWideFixed(cUnit, rlSrc2, rARG2, rARG3);
        }
        callRuntimeHelper(cUnit, rTgt);
        // Adjust return regs in to handle case of rem returning rARG2/rARG3
        if (retReg == rRET0)
            rlResult = oatGetReturnWide(cUnit);
        else
            rlResult = oatGetReturnWideAlt(cUnit);
        storeValueWide(cUnit, rlDest, rlResult);
    }
    return false;
}

bool genConversionCall(CompilationUnit* cUnit, MIR* mir, int funcOffset,
                       int srcSize, int tgtSize)
{
    /*
     * Don't optimize the register usage since it calls out to support
     * functions
     */
    RegLocation rlSrc;
    RegLocation rlDest;
    oatFlushAllRegs(cUnit);   /* Send everything to home location */
    int rTgt = loadHelper(cUnit, funcOffset);
    if (srcSize == 1) {
        rlSrc = oatGetSrc(cUnit, mir, 0);
        loadValueDirectFixed(cUnit, rlSrc, rARG0);
    } else {
        rlSrc = oatGetSrcWide(cUnit, mir, 0, 1);
        loadValueDirectWideFixed(cUnit, rlSrc, rARG0, rARG1);
    }
    callRuntimeHelper(cUnit, rTgt);
    if (tgtSize == 1) {
        RegLocation rlResult;
        rlDest = oatGetDest(cUnit, mir, 0);
        rlResult = oatGetReturn(cUnit);
        storeValue(cUnit, rlDest, rlResult);
    } else {
        RegLocation rlResult;
        rlDest = oatGetDestWide(cUnit, mir, 0, 1);
        rlResult = oatGetReturnWide(cUnit);
        storeValueWide(cUnit, rlDest, rlResult);
    }
    return false;
}

void genNegFloat(CompilationUnit* cUnit, RegLocation rlDest, RegLocation rlSrc);
bool genArithOpFloatPortable(CompilationUnit* cUnit, MIR* mir,
                             RegLocation rlDest, RegLocation rlSrc1,
                             RegLocation rlSrc2)
{
    RegLocation rlResult;
    int funcOffset;

    switch (mir->dalvikInsn.opcode) {
        case OP_ADD_FLOAT_2ADDR:
        case OP_ADD_FLOAT:
            funcOffset = OFFSETOF_MEMBER(Thread, pFadd);
            break;
        case OP_SUB_FLOAT_2ADDR:
        case OP_SUB_FLOAT:
            funcOffset = OFFSETOF_MEMBER(Thread, pFsub);
            break;
        case OP_DIV_FLOAT_2ADDR:
        case OP_DIV_FLOAT:
            funcOffset = OFFSETOF_MEMBER(Thread, pFdiv);
            break;
        case OP_MUL_FLOAT_2ADDR:
        case OP_MUL_FLOAT:
            funcOffset = OFFSETOF_MEMBER(Thread, pFmul);
            break;
        case OP_REM_FLOAT_2ADDR:
        case OP_REM_FLOAT:
            funcOffset = OFFSETOF_MEMBER(Thread, pFmodf);
            break;
        case OP_NEG_FLOAT: {
            genNegFloat(cUnit, rlDest, rlSrc1);
            return false;
        }
        default:
            return true;
    }
    oatFlushAllRegs(cUnit);   /* Send everything to home location */
    int rTgt = loadHelper(cUnit, funcOffset);
    loadValueDirectFixed(cUnit, rlSrc1, rARG0);
    loadValueDirectFixed(cUnit, rlSrc2, rARG1);
    callRuntimeHelper(cUnit, rTgt);
    rlResult = oatGetReturn(cUnit);
    storeValue(cUnit, rlDest, rlResult);
    return false;
}

void genNegDouble(CompilationUnit* cUnit, RegLocation rlDst, RegLocation rlSrc);
bool genArithOpDoublePortable(CompilationUnit* cUnit, MIR* mir,
                              RegLocation rlDest, RegLocation rlSrc1,
                              RegLocation rlSrc2)
{
    RegLocation rlResult;
    int funcOffset;

    switch (mir->dalvikInsn.opcode) {
        case OP_ADD_DOUBLE_2ADDR:
        case OP_ADD_DOUBLE:
            funcOffset = OFFSETOF_MEMBER(Thread, pDadd);
            break;
        case OP_SUB_DOUBLE_2ADDR:
        case OP_SUB_DOUBLE:
            funcOffset = OFFSETOF_MEMBER(Thread, pDsub);
            break;
        case OP_DIV_DOUBLE_2ADDR:
        case OP_DIV_DOUBLE:
            funcOffset = OFFSETOF_MEMBER(Thread, pDdiv);
            break;
        case OP_MUL_DOUBLE_2ADDR:
        case OP_MUL_DOUBLE:
            funcOffset = OFFSETOF_MEMBER(Thread, pDmul);
            break;
        case OP_REM_DOUBLE_2ADDR:
        case OP_REM_DOUBLE:
            funcOffset = OFFSETOF_MEMBER(Thread, pFmod);
            break;
        case OP_NEG_DOUBLE: {
            genNegDouble(cUnit, rlDest, rlSrc1);
            return false;
        }
        default:
            return true;
    }
    oatFlushAllRegs(cUnit);   /* Send everything to home location */
    int rTgt = loadHelper(cUnit, funcOffset);
    loadValueDirectWideFixed(cUnit, rlSrc1, rARG0, rARG1);
    loadValueDirectWideFixed(cUnit, rlSrc2, rARG2, rARG3);
    callRuntimeHelper(cUnit, rTgt);
    rlResult = oatGetReturnWide(cUnit);
    storeValueWide(cUnit, rlDest, rlResult);
    return false;
}

bool genConversionPortable(CompilationUnit* cUnit, MIR* mir)
{
    Opcode opcode = mir->dalvikInsn.opcode;

    switch (opcode) {
        case OP_INT_TO_FLOAT:
            return genConversionCall(cUnit, mir, OFFSETOF_MEMBER(Thread, pI2f),
                                     1, 1);
        case OP_FLOAT_TO_INT:
            return genConversionCall(cUnit, mir, OFFSETOF_MEMBER(Thread, pF2iz),
                                     1, 1);
        case OP_DOUBLE_TO_FLOAT:
            return genConversionCall(cUnit, mir, OFFSETOF_MEMBER(Thread, pD2f),
                                     2, 1);
        case OP_FLOAT_TO_DOUBLE:
            return genConversionCall(cUnit, mir, OFFSETOF_MEMBER(Thread, pF2d),
                                     1, 2);
        case OP_INT_TO_DOUBLE:
            return genConversionCall(cUnit, mir, OFFSETOF_MEMBER(Thread, pI2d),
                                     1, 2);
        case OP_DOUBLE_TO_INT:
            return genConversionCall(cUnit, mir, OFFSETOF_MEMBER(Thread, pD2iz),
                                     2, 1);
        case OP_FLOAT_TO_LONG:
            return genConversionCall(cUnit, mir, OFFSETOF_MEMBER(Thread,
                                     pF2l), 1, 2);
        case OP_LONG_TO_FLOAT:
            return genConversionCall(cUnit, mir, OFFSETOF_MEMBER(Thread, pL2f),
                                     2, 1);
        case OP_DOUBLE_TO_LONG:
            return genConversionCall(cUnit, mir, OFFSETOF_MEMBER(Thread,
                                     pD2l), 2, 2);
        case OP_LONG_TO_DOUBLE:
            return genConversionCall(cUnit, mir, OFFSETOF_MEMBER(Thread, pL2d),
                                     2, 2);
        default:
            return true;
    }
    return false;
}

/*
 * Generate callout to updateDebugger. Note that we're overloading
 * the use of rSUSPEND here.  When the debugger is active, this
 * register holds the address of the update function.  So, if it's
 * non-null, we call out to it.
 *
 * Note also that rRET0 and rRET1 must be preserved across this
 * code.  This must be handled by the stub.
 */
void genDebuggerUpdate(CompilationUnit* cUnit, int32_t offset)
{
    // Following DCHECK verifies that dPC is in range of single load immediate
    DCHECK((offset == DEBUGGER_METHOD_ENTRY) ||
           (offset == DEBUGGER_METHOD_EXIT) || ((offset & 0xffff) == offset));
    oatClobberCalleeSave(cUnit);
#if defined(TARGET_ARM)
    opRegImm(cUnit, kOpCmp, rSUSPEND, 0);
    genIT(cUnit, kArmCondNe, "T");
    loadConstant(cUnit, rARG2, offset);     // arg2 <- Entry code
    opReg(cUnit, kOpBlx, rSUSPEND);
#else
    LIR* branch = genCmpImmBranch(cUnit, kCondEq, rSUSPEND, 0);
    loadConstant(cUnit, rARG2, offset);
    opReg(cUnit, kOpBlx, rSUSPEND);
    LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
    target->defMask = ENCODE_ALL;
    branch->target = (LIR*)target;
#endif
    oatFreeTemp(cUnit, rARG2);
}

/* Check if we need to check for pending suspend request */
void genSuspendTest(CompilationUnit* cUnit, MIR* mir)
{
    if (NO_SUSPEND || (mir->optimizationFlags & MIR_IGNORE_SUSPEND_CHECK)) {
        return;
    }
    oatFlushAllRegs(cUnit);
    LIR* branch;
    if (cUnit->genDebugger) {
        // If generating code for the debugger, always check for suspension
        branch = genUnconditionalBranch(cUnit, NULL);
    } else {
#if defined(TARGET_ARM)
        // In non-debug case, only check periodically
        newLIR2(cUnit, kThumbSubRI8, rSUSPEND, 1);
        branch = opCondBranch(cUnit, kCondEq);
#else
        opRegImm(cUnit, kOpSub, rSUSPEND, 1);
        branch = opCompareBranchImm(cUnit, kCondEq, rSUSPEND, 0);
#endif
    }
    LIR* retLab = newLIR0(cUnit, kPseudoTargetLabel);
    retLab->defMask = ENCODE_ALL;
    LIR* target = (LIR*)oatNew(cUnit, sizeof(LIR), true, kAllocLIR);
    target->dalvikOffset = cUnit->currentDalvikOffset;
    target->opcode = kPseudoSuspendTarget;
    target->operands[0] = (intptr_t)retLab;
    target->operands[1] = mir->offset;
    branch->target = (LIR*)target;
    oatInsertGrowableList(cUnit, &cUnit->suspendLaunchpads, (intptr_t)target);
}

}  // namespace art
