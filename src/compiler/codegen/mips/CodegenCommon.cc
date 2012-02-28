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
 * This file contains codegen and support common to all supported
 * Mips variants.  It is included by:
 *
 *        Codegen-$(TARGET_ARCH_VARIANT).c
 *
 * which combines this common code with specific support found in the
 * applicable directory below this one.
 */

static void setMemRefType(MipsLIR *lir, bool isLoad, int memType)
{
    /* MIPSTODO simplify setMemRefType() */
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
 * Mark load/store instructions that access Dalvik registers through rFP +
 * offset.
 */
STATIC void annotateDalvikRegAccess(MipsLIR *lir, int regId, bool isLoad)
{
    /* MIPSTODO simplify annotateDalvikRegAccess() */
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
 * Decode the register id
 */
STATIC inline u8 getRegMaskCommon(int reg)
{
    u8 seed;
    int shift;
    int regId = reg & 0x1f;

    /*
     * Each double register is equal to a pair of single-precision FP registers
     */
    if (!DOUBLEREG(reg)) {
        seed = 1;
    } else {
        DCHECK_EQ((regId & 1), 0); /* double registers must be even */
        seed = 3;
    }

    if (FPREG(reg)) {
       DCHECK_LT(regId, 16); /* only 16 fp regs */
       shift = kFPReg0;
    } else if (EXTRAREG(reg)) {
       DCHECK_LT(regId, 3); /* only 3 extra regs */
       shift = kFPRegEnd;
    } else {
       shift = 0;
    }

    /* Expand the double register id into single offset */
    shift += regId;
    return (seed << shift);
}

/*
 * Mark the corresponding bit(s).
 */
STATIC inline void setupRegMask(u8 *mask, int reg)
{
    *mask |= getRegMaskCommon(reg);
}

/*
 * Set up the proper fields in the resource mask
 */
STATIC void setupResourceMasks(MipsLIR *lir)
{
    /* MIPSTODO simplify setupResourceMasks() */
    int opcode = lir->opcode;
    int flags;

    if (opcode <= 0) {
        lir->useMask = lir->defMask = 0;
        return;
    }

    flags = EncodingMap[lir->opcode].flags;

    // TODO: do we need this for MIPS?  if so, add to inst table defs
#if 0
    if (flags & NEEDS_FIXUP) {
        lir->flags.pcRelFixup = true;
    }
#endif

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

    if (flags & SETS_CCODES) {
        lir->defMask |= ENCODE_CCODE;
    }

    // TODO: needed for MIPS?
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

    if (flags & USES_CCODES) {
        lir->useMask |= ENCODE_CCODE;
    }
}

/*
 * The following are building blocks to construct low-level IRs with 0 - 4
 * operands.
 */
MipsLIR *newLIR0(CompilationUnit *cUnit, MipsOpCode opcode)
{
    MipsLIR *insn = (MipsLIR *) oatNew(cUnit, sizeof(MipsLIR), true, kAllocLIR);
    DCHECK(isPseudoOpcode(opcode) || (EncodingMap[opcode].flags & NO_OPERAND));
    insn->opcode = opcode;
    setupResourceMasks(insn);
    insn->generic.dalvikOffset = cUnit->currentDalvikOffset;
    oatAppendLIR(cUnit, (LIR *) insn);
    return insn;
}

MipsLIR *newLIR1(CompilationUnit *cUnit, MipsOpCode opcode,
                           int dest)
{
    MipsLIR *insn = (MipsLIR *) oatNew(cUnit, sizeof(MipsLIR), true, kAllocLIR);
    DCHECK(isPseudoOpcode(opcode) || (EncodingMap[opcode].flags & IS_UNARY_OP));
    insn->opcode = opcode;
    insn->operands[0] = dest;
    setupResourceMasks(insn);
    insn->generic.dalvikOffset = cUnit->currentDalvikOffset;
    oatAppendLIR(cUnit, (LIR *) insn);
    return insn;
}

MipsLIR *newLIR2(CompilationUnit *cUnit, MipsOpCode opcode,
                           int dest, int src1)
{
    MipsLIR *insn = (MipsLIR *) oatNew(cUnit, sizeof(MipsLIR), true, kAllocLIR);
    DCHECK(isPseudoOpcode(opcode) ||
           (EncodingMap[opcode].flags & IS_BINARY_OP));
    insn->opcode = opcode;
    insn->operands[0] = dest;
    insn->operands[1] = src1;
    setupResourceMasks(insn);
    insn->generic.dalvikOffset = cUnit->currentDalvikOffset;
    oatAppendLIR(cUnit, (LIR *) insn);
    return insn;
}

MipsLIR *newLIR3(CompilationUnit *cUnit, MipsOpCode opcode,
                           int dest, int src1, int src2)
{
    MipsLIR *insn = (MipsLIR *) oatNew(cUnit, sizeof(MipsLIR), true, kAllocLIR);
    if (!(EncodingMap[opcode].flags & IS_TERTIARY_OP)) {
        LOG(FATAL) << "Bad LIR3: " << EncodingMap[opcode].name;
    }
    DCHECK(isPseudoOpcode(opcode) ||
           (EncodingMap[opcode].flags & IS_TERTIARY_OP));
    insn->opcode = opcode;
    insn->operands[0] = dest;
    insn->operands[1] = src1;
    insn->operands[2] = src2;
    setupResourceMasks(insn);
    insn->generic.dalvikOffset = cUnit->currentDalvikOffset;
    oatAppendLIR(cUnit, (LIR *) insn);
    return insn;
}

MipsLIR *newLIR4(CompilationUnit *cUnit, MipsOpCode opcode,
                           int dest, int src1, int src2, int info)
{
    MipsLIR *insn = (MipsLIR *) oatNew(cUnit, sizeof(MipsLIR), true, kAllocLIR);
    DCHECK(isPseudoOpcode(opcode) ||
           (EncodingMap[opcode].flags & IS_QUAD_OP));
    insn->opcode = opcode;
    insn->operands[0] = dest;
    insn->operands[1] = src1;
    insn->operands[2] = src2;
    insn->operands[3] = info;
    setupResourceMasks(insn);
    insn->generic.dalvikOffset = cUnit->currentDalvikOffset;
    oatAppendLIR(cUnit, (LIR *) insn);
    return insn;
}

/*
 * Generate an kPseudoBarrier marker to indicate the boundary of special
 * blocks.
 */
static void genBarrier(CompilationUnit *cUnit)
{
    MipsLIR *barrier = newLIR0(cUnit, kPseudoBarrier);
    /* Mark all resources as being clobbered */
    barrier->defMask = -1;
}

} // namespace art
