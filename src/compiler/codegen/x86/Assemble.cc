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

#include "../../Dalvik.h"
#include "../../CompilerInternals.h"
#include "X86LIR.h"
#include "Codegen.h"
#include <sys/mman.h>           /* for protection change */

namespace art {

#define MAX_ASSEMBLER_RETRIES 50

/*
 * opcode: MipsOpCode enum
 * skeleton: pre-designated bit-pattern for this opcode
 * k0: key to applying ds/de
 * ds: dest start bit position
 * de: dest end bit position
 * k1: key to applying s1s/s1e
 * s1s: src1 start bit position
 * s1e: src1 end bit position
 * k2: key to applying s2s/s2e
 * s2s: src2 start bit position
 * s2e: src2 end bit position
 * operands: number of operands (for sanity check purposes)
 * name: mnemonic name
 * fmt: for pretty-printing
 */
#define ENCODING_MAP(opcode, skeleton, k0, ds, de, k1, s1s, s1e, k2, s2s, s2e, \
                     k3, k3s, k3e, flags, name, fmt, size) \
        {skeleton, {{k0, ds, de}, {k1, s1s, s1e}, {k2, s2s, s2e}, \
                    {k3, k3s, k3e}}, opcode, flags, name, fmt, size}

/* Instruction dump string format keys: !pf, where "!" is the start
 * of the key, "p" is which numeric operand to use and "f" is the
 * print format.
 *
 * [p]ositions:
 *     0 -> operands[0] (dest)
 *     1 -> operands[1] (src1)
 *     2 -> operands[2] (src2)
 *     3 -> operands[3] (extra)
 *
 * [f]ormats:
 *     h -> 4-digit hex
 *     d -> decimal
 *     E -> decimal*4
 *     F -> decimal*2
 *     c -> branch condition (beq, bne, etc.)
 *     t -> pc-relative target
 *     T -> pc-region target
 *     u -> 1st half of bl[x] target
 *     v -> 2nd half ob bl[x] target
 *     R -> register list
 *     s -> single precision floating point register
 *     S -> double precision floating point register
 *     m -> Thumb2 modified immediate
 *     n -> complimented Thumb2 modified immediate
 *     M -> Thumb2 16-bit zero-extended immediate
 *     b -> 4-digit binary
 *     N -> append a NOP
 *
 *  [!] escape.  To insert "!", use "!!"
 */
/* NOTE: must be kept in sync with enum MipsOpcode from LIR.h */
/*
 * TUNING: We're currently punting on the branch delay slots.  All branch
 * instructions in this map are given a size of 8, which during assembly
 * is expanded to include a nop.  This scheme should be replaced with
 * an assembler pass to fill those slots when possible.
 */
MipsEncodingMap EncodingMap[kX86Last] = {
};


/*
 * Assemble the LIR into binary instruction format.  Note that we may
 * discover that pc-relative displacements may not fit the selected
 * instruction.  In those cases we will try to substitute a new code
 * sequence or request that the trace be shortened and retried.
 */
AssemblerStatus oatAssembleInstructions(CompilationUnit *cUnit,
                                        intptr_t startAddr)
{
    UNIMPLEMENTED(WARNING) << "oatAssembleInstructions";
    return kSuccess;
#if 0
    LIR *lir;
    AssemblerStatus res = kSuccess;  // Assume success

    for (lir = (LIR *) cUnit->firstLIRInsn; lir; lir = NEXT_LIR(lir)) {
        if (lir->opcode < 0) {
            continue;
        }


        if (lir->flags.isNop) {
            continue;
        }

        if (lir->flags.pcRelFixup) {
            if (lir->opcode == kMipsDelta) {
                /*
                 * The "Delta" pseudo-ops load the difference between
                 * two pc-relative locations into a the target register
                 * found in operands[0].  The delta is determined by
                 * (label2 - label1), where label1 is a standard
                 * kPseudoTargetLabel and is stored in operands[2].
                 * If operands[3] is null, then label2 is a kPseudoTargetLabel
                 * and is found in lir->target.  If operands[3] is non-NULL,
                 * then it is a Switch/Data table.
                 */
                int offset1 = ((LIR*)lir->operands[2])->offset;
                SwitchTable *tabRec = (SwitchTable*)lir->operands[3];
                int offset2 = tabRec ? tabRec->offset : lir->target->offset;
                int delta = offset2 - offset1;
                if ((delta & 0xffff) == delta) {
                    // Fits
                    lir->operands[1] = delta;
                } else {
                    // Doesn't fit - must expand to kMipsDelta[Hi|Lo] pair
                    LIR *newDeltaHi =
                          rawLIR(cUnit, lir->dalvikOffset, kMipsDeltaHi,
                                 lir->operands[0], 0, lir->operands[2],
                                 lir->operands[3], lir->target);
                    oatInsertLIRBefore((LIR*)lir, (LIR*)newDeltaHi);
                    LIR *newDeltaLo =
                          rawLIR(cUnit, lir->dalvikOffset, kMipsDeltaLo,
                                 lir->operands[0], 0, lir->operands[2],
                                 lir->operands[3], lir->target);
                    oatInsertLIRBefore((LIR*)lir, (LIR*)newDeltaLo);
                    lir->flags.isNop = true;
                    res = kRetryAll;
                }
            } else if (lir->opcode == kMipsDeltaLo) {
                int offset1 = ((LIR*)lir->operands[2])->offset;
                SwitchTable *tabRec = (SwitchTable*)lir->operands[3];
                int offset2 = tabRec ? tabRec->offset : lir->target->offset;
                int delta = offset2 - offset1;
                lir->operands[1] = delta & 0xffff;
            } else if (lir->opcode == kMipsDeltaHi) {
                int offset1 = ((LIR*)lir->operands[2])->offset;
                SwitchTable *tabRec = (SwitchTable*)lir->operands[3];
                int offset2 = tabRec ? tabRec->offset : lir->target->offset;
                int delta = offset2 - offset1;
                lir->operands[1] = (delta >> 16) & 0xffff;
            } else if (lir->opcode == kMipsB || lir->opcode == kMipsBal) {
                LIR *targetLIR = (LIR *) lir->target;
                intptr_t pc = lir->offset + 4;
                intptr_t target = targetLIR->offset;
                int delta = target - pc;
                if (delta & 0x3) {
                    LOG(FATAL) << "PC-rel offset not multiple of 4: " << delta;
                }
                if (delta > 131068 || delta < -131069) {
                    res = kRetryAll;
                    convertShortToLongBranch(cUnit, lir);
                } else {
                    lir->operands[0] = delta >> 2;
                }
            } else if (lir->opcode >= kMipsBeqz && lir->opcode <= kMipsBnez) {
                LIR *targetLIR = (LIR *) lir->target;
                intptr_t pc = lir->offset + 4;
                intptr_t target = targetLIR->offset;
                int delta = target - pc;
                if (delta & 0x3) {
                    LOG(FATAL) << "PC-rel offset not multiple of 4: " << delta;
                }
                if (delta > 131068 || delta < -131069) {
                    res = kRetryAll;
                    convertShortToLongBranch(cUnit, lir);
                } else {
                    lir->operands[1] = delta >> 2;
                }
            } else if (lir->opcode == kMipsBeq || lir->opcode == kMipsBne) {
                LIR *targetLIR = (LIR *) lir->target;
                intptr_t pc = lir->offset + 4;
                intptr_t target = targetLIR->offset;
                int delta = target - pc;
                if (delta & 0x3) {
                    LOG(FATAL) << "PC-rel offset not multiple of 4: " << delta;
                }
                if (delta > 131068 || delta < -131069) {
                    res = kRetryAll;
                    convertShortToLongBranch(cUnit, lir);
                } else {
                    lir->operands[2] = delta >> 2;
                }
            } else if (lir->opcode == kMipsJal) {
                intptr_t curPC = (startAddr + lir->offset + 4) & ~3;
                intptr_t target = lir->operands[0];
                /* ensure PC-region branch can be used */
                DCHECK_EQ((curPC & 0xF0000000), (target & 0xF0000000));
                if (target & 0x3) {
                    LOG(FATAL) << "Jump target not multiple of 4: " << target;
                }
                lir->operands[0] =  target >> 2;
            } else if (lir->opcode == kMipsLahi) { /* ld address hi (via lui) */
                LIR *targetLIR = (LIR *) lir->target;
                intptr_t target = startAddr + targetLIR->offset;
                lir->operands[1] = target >> 16;
            } else if (lir->opcode == kMipsLalo) { /* ld address lo (via ori) */
                LIR *targetLIR = (LIR *) lir->target;
                intptr_t target = startAddr + targetLIR->offset;
                lir->operands[2] = lir->operands[2] + target;
            }
        }

        /*
         * If one of the pc-relative instructions expanded we'll have
         * to make another pass.  Don't bother to fully assemble the
         * instruction.
         */
        if (res != kSuccess) {
            continue;
        }
        const MipsEncodingMap *encoder = &EncodingMap[lir->opcode];
        u4 bits = encoder->skeleton;
        int i;
        for (i = 0; i < 4; i++) {
            u4 operand;
            u4 value;
            operand = lir->operands[i];
            switch(encoder->fieldLoc[i].kind) {
                case kFmtUnused:
                    break;
                case kFmtBitBlt:
                    if (encoder->fieldLoc[i].start == 0 && encoder->fieldLoc[i].end == 31) {
                        value = operand;
                    } else {
                        value = (operand << encoder->fieldLoc[i].start) &
                                ((1 << (encoder->fieldLoc[i].end + 1)) - 1);
                    }
                    bits |= value;
                    break;
                case kFmtBlt5_2:
                    value = (operand & 0x1f);
                    bits |= (value << encoder->fieldLoc[i].start);
                    bits |= (value << encoder->fieldLoc[i].end);
                    break;
                case kFmtDfp: {
                    DCHECK(DOUBLEREG(operand));
                    DCHECK((operand & 0x1) == 0);
                    value = ((operand & FP_REG_MASK) << encoder->fieldLoc[i].start) &
                            ((1 << (encoder->fieldLoc[i].end + 1)) - 1);
                    bits |= value;
                    break;
                }
                case kFmtSfp:
                    DCHECK(SINGLEREG(operand));
                    value = ((operand & FP_REG_MASK) << encoder->fieldLoc[i].start) &
                            ((1 << (encoder->fieldLoc[i].end + 1)) - 1);
                    bits |= value;
                    break;
                default:
                    LOG(FATAL) << "Bad encoder format: "
                               << (int)encoder->fieldLoc[i].kind;
            }
        }
        // FIXME: need multi-endian handling here
        cUnit->codeBuffer.push_back((bits >> 16) & 0xffff);
        cUnit->codeBuffer.push_back(bits & 0xffff);
        // TUNING: replace with proper delay slot handling
        if (encoder->size == 8) {
            const MipsEncodingMap *encoder = &EncodingMap[kMipsNop];
            u4 bits = encoder->skeleton;
            cUnit->codeBuffer.push_back((bits >> 16) & 0xffff);
            cUnit->codeBuffer.push_back(bits & 0xffff);
        }
    }
    return res;
#endif
}

int oatGetInsnSize(LIR* lir)
{
    return EncodingMap[lir->opcode].size;
}
/*
 * Target-dependent offset assignment.
 * independent.
 */
int oatAssignInsnOffsets(CompilationUnit* cUnit)
{
    LIR* x86LIR;
    int offset = 0;

    for (x86LIR = (LIR *) cUnit->firstLIRInsn;
        x86LIR;
        x86LIR = NEXT_LIR(x86LIR)) {
        x86LIR->offset = offset;
        if (x86LIR->opcode >= 0) {
            if (!x86LIR->flags.isNop) {
                offset += x86LIR->flags.size;
            }
        } else if (x86LIR->opcode == kPseudoPseudoAlign4) {
            if (offset & 0x2) {
                offset += 2;
                x86LIR->operands[0] = 1;
            } else {
                x86LIR->operands[0] = 0;
            }
        }
        /* Pseudo opcodes don't consume space */
    }

    return offset;
}

}  // namespace art
