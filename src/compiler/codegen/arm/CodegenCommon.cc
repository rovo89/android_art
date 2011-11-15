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
 * This file contains codegen and support common to all supported
 * ARM variants.  It is included by:
 *
 *        Codegen-$(TARGET_ARCH_VARIANT).c
 *
 * which combines this common code with specific support found in the
 * applicable directory below this one.
 */

/* Track exercised opcodes */
static int opcodeCoverage[kNumPackedOpcodes];

STATIC void setMemRefType(ArmLIR* lir, bool isLoad, int memType)
{
    u8 *maskPtr;
    u8 mask = ENCODE_MEM;;
    DCHECK(EncodingMap[lir->opcode].flags & (IS_LOAD | IS_STORE));
    if (isLoad) {
        maskPtr = &lir->useMask;
    } else {
        maskPtr = &lir->defMask;
    }
    /* Clear out the memref flags */
    *maskPtr &= ~mask;
    /* ..and then add back the one we need */
    switch(memType) {
        case kLiteral:
            DCHECK(isLoad);
            *maskPtr |= ENCODE_LITERAL;
            break;
        case kDalvikReg:
            *maskPtr |= ENCODE_DALVIK_REG;
            break;
        case kHeapRef:
            *maskPtr |= ENCODE_HEAP_REF;
            break;
        case kMustNotAlias:
            /* Currently only loads can be marked as kMustNotAlias */
            DCHECK(!(EncodingMap[lir->opcode].flags & IS_STORE));
            *maskPtr |= ENCODE_MUST_NOT_ALIAS;
            break;
        default:
            LOG(FATAL) << "Oat: invalid memref kind - " << memType;
    }
}

/*
 * Mark load/store instructions that access Dalvik registers through r5FP +
 * offset.
 */
STATIC void annotateDalvikRegAccess(ArmLIR* lir, int regId, bool isLoad)
{
    setMemRefType(lir, isLoad, kDalvikReg);

    /*
     * Store the Dalvik register id in aliasInfo. Mark he MSB if it is a 64-bit
     * access.
     */
    lir->aliasInfo = regId;
    if (DOUBLEREG(lir->operands[0])) {
        lir->aliasInfo |= 0x80000000;
    }
}

/*
 * Decode the register id.
 */
STATIC inline u8 getRegMaskCommon(int reg)
{
    u8 seed;
    int shift;
    int regId = reg & 0x1f;

    /*
     * Each double register is equal to a pair of single-precision FP registers
     */
    seed = DOUBLEREG(reg) ? 3 : 1;
    /* FP register starts at bit position 16 */
    shift = FPREG(reg) ? kFPReg0 : 0;
    /* Expand the double register id into single offset */
    shift += regId;
    return (seed << shift);
}

/*
 * Mark the corresponding bit(s).
 */
STATIC inline void setupRegMask(u8* mask, int reg)
{
    *mask |= getRegMaskCommon(reg);
}

/*
 * Set up the proper fields in the resource mask
 */
STATIC void setupResourceMasks(ArmLIR* lir)
{
    int opcode = lir->opcode;
    int flags;

    if (opcode <= 0) {
        lir->useMask = lir->defMask = 0;
        return;
    }

    flags = EncodingMap[lir->opcode].flags;

    /* Set up the mask for resources that are updated */
    if (flags & (IS_LOAD | IS_STORE)) {
        /* Default to heap - will catch specialized classes later */
        setMemRefType(lir, flags & IS_LOAD, kHeapRef);
    }

    /*
     * Conservatively assume the branch here will call out a function that in
     * turn will trash everything.
     */
    if (flags & IS_BRANCH) {
        lir->defMask = lir->useMask = ENCODE_ALL;
        return;
    }

    if (flags & REG_DEF0) {
        setupRegMask(&lir->defMask, lir->operands[0]);
    }

    if (flags & REG_DEF1) {
        setupRegMask(&lir->defMask, lir->operands[1]);
    }

    if (flags & REG_DEF_SP) {
        lir->defMask |= ENCODE_REG_SP;
    }

    if (flags & REG_DEF_LR) {
        lir->defMask |= ENCODE_REG_LR;
    }

    if (flags & REG_DEF_LIST0) {
        lir->defMask |= ENCODE_REG_LIST(lir->operands[0]);
    }

    if (flags & REG_DEF_LIST1) {
        lir->defMask |= ENCODE_REG_LIST(lir->operands[1]);
    }

    if (flags & REG_DEF_FPCS_LIST0) {
        lir->defMask |= ENCODE_REG_FPCS_LIST(lir->operands[0]);
    }

    if (flags & REG_DEF_FPCS_LIST2) {
        for (int i = 0; i < lir->operands[2]; i++) {
            setupRegMask(&lir->defMask, lir->operands[1] + i);
        }
    }

    if (flags & SETS_CCODES) {
        lir->defMask |= ENCODE_CCODE;
    }

    /* Conservatively treat the IT block */
    if (flags & IS_IT) {
        lir->defMask = ENCODE_ALL;
    }

    if (flags & (REG_USE0 | REG_USE1 | REG_USE2 | REG_USE3)) {
        int i;

        for (i = 0; i < 4; i++) {
            if (flags & (1 << (kRegUse0 + i))) {
                setupRegMask(&lir->useMask, lir->operands[i]);
            }
        }
    }

    if (flags & REG_USE_PC) {
        lir->useMask |= ENCODE_REG_PC;
    }

    if (flags & REG_USE_SP) {
        lir->useMask |= ENCODE_REG_SP;
    }

    if (flags & REG_USE_LIST0) {
        lir->useMask |= ENCODE_REG_LIST(lir->operands[0]);
    }

    if (flags & REG_USE_LIST1) {
        lir->useMask |= ENCODE_REG_LIST(lir->operands[1]);
    }

    if (flags & REG_USE_FPCS_LIST0) {
        lir->useMask |= ENCODE_REG_FPCS_LIST(lir->operands[0]);
    }

    if (flags & REG_USE_FPCS_LIST2) {
        for (int i = 0; i < lir->operands[2]; i++) {
            setupRegMask(&lir->useMask, lir->operands[1] + i);
        }
    }

    if (flags & USES_CCODES) {
        lir->useMask |= ENCODE_CCODE;
    }

    /* Fixup for kThumbPush/lr and kThumbPop/pc */
    if (opcode == kThumbPush || opcode == kThumbPop) {
        u8 r8Mask = getRegMaskCommon(r8);
        if ((opcode == kThumbPush) && (lir->useMask & r8Mask)) {
            lir->useMask &= ~r8Mask;
            lir->useMask |= ENCODE_REG_LR;
        } else if ((opcode == kThumbPop) && (lir->defMask & r8Mask)) {
            lir->defMask &= ~r8Mask;
            lir->defMask |= ENCODE_REG_PC;
        }
    }
}

/*
 * The following are building blocks to construct low-level IRs with 0 - 4
 * operands.
 */
STATIC ArmLIR* newLIR0(CompilationUnit* cUnit, ArmOpcode opcode)
{
    ArmLIR* insn = (ArmLIR* ) oatNew(sizeof(ArmLIR), true);
    DCHECK(isPseudoOpcode(opcode) || (EncodingMap[opcode].flags & NO_OPERAND));
    insn->opcode = opcode;
    setupResourceMasks(insn);
    insn->generic.dalvikOffset = cUnit->currentDalvikOffset;
    oatAppendLIR(cUnit, (LIR*) insn);
    return insn;
}

STATIC ArmLIR* newLIR1(CompilationUnit* cUnit, ArmOpcode opcode,
                           int dest)
{
    ArmLIR* insn = (ArmLIR* ) oatNew(sizeof(ArmLIR), true);
    DCHECK(isPseudoOpcode(opcode) || (EncodingMap[opcode].flags & IS_UNARY_OP));
    insn->opcode = opcode;
    insn->operands[0] = dest;
    setupResourceMasks(insn);
    insn->generic.dalvikOffset = cUnit->currentDalvikOffset;
    oatAppendLIR(cUnit, (LIR*) insn);
    return insn;
}

STATIC ArmLIR* newLIR2(CompilationUnit* cUnit, ArmOpcode opcode,
                           int dest, int src1)
{
    ArmLIR* insn = (ArmLIR* ) oatNew(sizeof(ArmLIR), true);
    DCHECK(isPseudoOpcode(opcode) ||
           (EncodingMap[opcode].flags & IS_BINARY_OP));
    insn->opcode = opcode;
    insn->operands[0] = dest;
    insn->operands[1] = src1;
    setupResourceMasks(insn);
    insn->generic.dalvikOffset = cUnit->currentDalvikOffset;
    oatAppendLIR(cUnit, (LIR*) insn);
    return insn;
}

STATIC ArmLIR* newLIR3(CompilationUnit* cUnit, ArmOpcode opcode,
                           int dest, int src1, int src2)
{
    ArmLIR* insn = (ArmLIR* ) oatNew(sizeof(ArmLIR), true);
    DCHECK(isPseudoOpcode(opcode) ||
           (EncodingMap[opcode].flags & IS_TERTIARY_OP))
            << (int)opcode << " "
            << PrettyMethod(cUnit->method_idx, *cUnit->dex_file) << " "
            << cUnit->currentDalvikOffset;
    insn->opcode = opcode;
    insn->operands[0] = dest;
    insn->operands[1] = src1;
    insn->operands[2] = src2;
    setupResourceMasks(insn);
    insn->generic.dalvikOffset = cUnit->currentDalvikOffset;
    oatAppendLIR(cUnit, (LIR*) insn);
    return insn;
}

#if defined(_ARMV7_A) || defined(_ARMV7_A_NEON)
STATIC ArmLIR* newLIR4(CompilationUnit* cUnit, ArmOpcode opcode,
                           int dest, int src1, int src2, int info)
{
    ArmLIR* insn = (ArmLIR* ) oatNew(sizeof(ArmLIR), true);
    DCHECK(isPseudoOpcode(opcode) ||
           (EncodingMap[opcode].flags & IS_QUAD_OP));
    insn->opcode = opcode;
    insn->operands[0] = dest;
    insn->operands[1] = src1;
    insn->operands[2] = src2;
    insn->operands[3] = info;
    setupResourceMasks(insn);
    insn->generic.dalvikOffset = cUnit->currentDalvikOffset;
    oatAppendLIR(cUnit, (LIR*) insn);
    return insn;
}
#endif

/*
 * Search the existing constants in the literal pool for an exact or close match
 * within specified delta (greater or equal to 0).
 */
STATIC ArmLIR* scanLiteralPool(LIR* dataTarget, int value, unsigned int delta)
{
    while (dataTarget) {
        if (((unsigned) (value - ((ArmLIR* ) dataTarget)->operands[0])) <=
            delta)
            return (ArmLIR* ) dataTarget;
        dataTarget = dataTarget->next;
    }
    return NULL;
}

/* Search the existing constants in the literal pool for an exact wide match */
STATIC ArmLIR* scanLiteralPoolWide(LIR* dataTarget, int valLo, int valHi)
{
    bool loMatch = false;
    LIR* loTarget = NULL;
    while (dataTarget) {
        if (loMatch && (((ArmLIR*)dataTarget)->operands[0] == valHi)) {
            return (ArmLIR*)loTarget;
        }
        loMatch = false;
        if (((ArmLIR*)dataTarget)->operands[0] == valLo) {
            loMatch = true;
            loTarget = dataTarget;
        }
        dataTarget = dataTarget->next;
    }
    return NULL;
}

/*
 * The following are building blocks to insert constants into the pool or
 * instruction streams.
 */

/* Add a 32-bit constant either in the constant pool or mixed with code */
STATIC ArmLIR* addWordData(CompilationUnit* cUnit, LIR* *constantListP,
                           int value)
{
    /* Add the constant to the literal pool */
    if (constantListP) {
        ArmLIR* newValue = (ArmLIR* ) oatNew(sizeof(ArmLIR), true);
        newValue->operands[0] = value;
        newValue->generic.next = *constantListP;
        *constantListP = (LIR*) newValue;
        return newValue;
    } else {
        /* Add the constant in the middle of code stream */
        newLIR1(cUnit, kArm16BitData, (value & 0xffff));
        newLIR1(cUnit, kArm16BitData, (value >> 16));
    }
    return NULL;
}

/* Add a 64-bit constant to the constant pool or mixed with code */
STATIC ArmLIR* addWideData(CompilationUnit* cUnit, LIR* *constantListP,
                           int valLo, int valHi)
{
    ArmLIR* res;
    //NOTE: hard-coded little endian
    if (constantListP == NULL) {
        res = addWordData(cUnit, NULL, valLo);
        addWordData(cUnit, NULL, valHi);
    } else {
        // Insert high word into list first
        addWordData(cUnit, constantListP, valHi);
        res = addWordData(cUnit, constantListP, valLo);
    }
    return res;
}

/*
 * Generate an kArmPseudoBarrier marker to indicate the boundary of special
 * blocks.
 */
STATIC void genBarrier(CompilationUnit* cUnit)
{
    ArmLIR* barrier = newLIR0(cUnit, kArmPseudoBarrier);
    /* Mark all resources as being clobbered */
    barrier->defMask = -1;
}
