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

#define BINARY_ENCODING_MAP(opcode, \
                            rm8_r8, rm32_r32, \
                            r8_rm8, r32_rm32, \
                            rax8_i8, rax32_i32, \
                            rm8_i8_opcode, rm8_i8_modrm, \
                            rm32_i32_opcode, rm32_i32_modrm, \
                            rm32_i8_opcode, rm32_i8_modrm) \
{ kOp ## opcode ## RI, \
  kRegImm, \
  0, \
  { RegMem_Immediate: { rax8_i8, rax32_i32, \
                       {rm8_i8_opcode, rm8_i8_modrm}, \
                       {rm32_i32_opcode, rm32_i32_modrm}, \
                       {rm32_i8_opcode, rm32_i8_modrm} } }, \
  #opcode "RI", "" \
}, \
{ kOp ## opcode ## MI, \
  kMemImm, \
  0, \
  { RegMem_Immediate: { rax8_i8, rax32_i32, \
                       {rm8_i8_opcode, rm8_i8_modrm}, \
                       {rm32_i32_opcode, rm32_i32_modrm}, \
                       {rm32_i8_opcode, rm32_i8_modrm} } }, \
  #opcode "MI", "" \
}, \
{ kOp ## opcode ## AI, \
  kArrayImm, \
  0, \
  { RegMem_Immediate: { rax8_i8, rax32_i32, \
                       {rm8_i8_opcode, rm8_i8_modrm}, \
                       {rm32_i32_opcode, rm32_i32_modrm}, \
                       {rm32_i8_opcode, rm32_i8_modrm} } }, \
  #opcode "AI", "" \
}, \
{ kOp ## opcode ## RR, \
  kRegReg, \
  0, \
  { Reg_RegMem: {r8_rm8, r32_rm32} }, \
  #opcode "RR", "" \
}, \
{ kOp ## opcode ## RM, \
  kRegMem, \
  0, \
  { Reg_RegMem: {r8_rm8, r32_rm32} }, \
  #opcode "RM", "" \
}, \
{ kOp ## opcode ## RA, \
  kRegArray, \
  0, \
  { Reg_RegMem: {r8_rm8, r32_rm32} }, \
  #opcode "RA", "" \
}, \
{ kOp ## opcode ## MR, \
  kMemReg, \
  0, \
  { RegMem_Reg: {rm8_r8, rm32_r32} }, \
  #opcode "MR", "" \
}, \
{ kOp ## opcode ## AR, \
  kArrayReg, \
  0, \
  { RegMem_Reg: {rm8_r8, rm32_r32} }, \
  #opcode "AR", "" \
}

X86EncodingMap EncodingMap[kX86Last] = {
  { kX8632BitData, kData, 0 /* flags - TODO */, { unused: 0 }, "data", "" },
BINARY_ENCODING_MAP(Add,
  0x00 /* RegMem8/Reg8 */,     0x01 /* RegMem32/Reg32 */,
  0x02 /* Reg8/RegMem8 */,     0x03 /* Reg32/RegMem32 */,
  0x04 /* Rax8/imm8 opcode */, 0x05 /* Rax32/imm32 */,
  0x80, 0x0 /* RegMem8/imm8 */,
  0x81, 0x0 /* RegMem32/imm32 */, 0x83, 0x0 /* RegMem32/imm8 */),
BINARY_ENCODING_MAP(Or,
  0x08 /* RegMem8/Reg8 */,     0x09 /* RegMem32/Reg32 */,
  0x0A /* Reg8/RegMem8 */,     0x0B /* Reg32/RegMem32 */,
  0x0C /* Rax8/imm8 opcode */, 0x0D /* Rax32/imm32 */,
  0x80, 0x1 /* RegMem8/imm8 */,
  0x81, 0x1 /* RegMem32/imm32 */, 0x83, 0x1 /* RegMem32/imm8 */),
BINARY_ENCODING_MAP(Adc,
  0x10 /* RegMem8/Reg8 */,     0x11 /* RegMem32/Reg32 */,
  0x12 /* Reg8/RegMem8 */,     0x13 /* Reg32/RegMem32 */,
  0x14 /* Rax8/imm8 opcode */, 0x15 /* Rax32/imm32 */,
  0x80, 0x2 /* RegMem8/imm8 */,
  0x81, 0x2 /* RegMem32/imm32 */, 0x83, 0x2 /* RegMem32/imm8 */),
BINARY_ENCODING_MAP(Sbb,
  0x18 /* RegMem8/Reg8 */,     0x19 /* RegMem32/Reg32 */,
  0x1A /* Reg8/RegMem8 */,     0x1B /* Reg32/RegMem32 */,
  0x1C /* Rax8/imm8 opcode */, 0x1D /* Rax32/imm32 */,
  0x80, 0x3 /* RegMem8/imm8 */,
  0x81, 0x3 /* RegMem32/imm32 */, 0x83, 0x3 /* RegMem32/imm8 */),
BINARY_ENCODING_MAP(And,
  0x20 /* RegMem8/Reg8 */,     0x21 /* RegMem32/Reg32 */,
  0x22 /* Reg8/RegMem8 */,     0x23 /* Reg32/RegMem32 */,
  0x24 /* Rax8/imm8 opcode */, 0x25 /* Rax32/imm32 */,
  0x80, 0x4 /* RegMem8/imm8 */,
  0x81, 0x4 /* RegMem32/imm32 */, 0x83, 0x4 /* RegMem32/imm8 */),
BINARY_ENCODING_MAP(Sub,
  0x28 /* RegMem8/Reg8 */,     0x29 /* RegMem32/Reg32 */,
  0x2A /* Reg8/RegMem8 */,     0x2B /* Reg32/RegMem32 */,
  0x2C /* Rax8/imm8 opcode */, 0x2D /* Rax32/imm32 */,
  0x80, 0x5 /* RegMem8/imm8 */,
  0x81, 0x5 /* RegMem32/imm32 */, 0x83, 0x5 /* RegMem32/imm8 */),
BINARY_ENCODING_MAP(Xor,
  0x30 /* RegMem8/Reg8 */,     0x31 /* RegMem32/Reg32 */,
  0x32 /* Reg8/RegMem8 */,     0x33 /* Reg32/RegMem32 */,
  0x34 /* Rax8/imm8 opcode */, 0x35 /* Rax32/imm32 */,
  0x80, 0x6 /* RegMem8/imm8 */,
  0x81, 0x6 /* RegMem32/imm32 */, 0x83, 0x6 /* RegMem32/imm8 */),
BINARY_ENCODING_MAP(Cmp,
  0x38 /* RegMem8/Reg8 */,     0x39 /* RegMem32/Reg32 */,
  0x3A /* Reg8/RegMem8 */,     0x3B /* Reg32/RegMem32 */,
  0x3C /* Rax8/imm8 opcode */, 0x3D /* Rax32/imm32 */,
  0x80, 0x7 /* RegMem8/imm8 */,
  0x81, 0x7 /* RegMem32/imm32 */, 0x83, 0x7 /* RegMem32/imm8 */)
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
            if (lir->opcode == kX86Delta) {
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
                    // Doesn't fit - must expand to kX86Delta[Hi|Lo] pair
                    LIR *newDeltaHi =
                          rawLIR(cUnit, lir->dalvikOffset, kX86DeltaHi,
                                 lir->operands[0], 0, lir->operands[2],
                                 lir->operands[3], lir->target);
                    oatInsertLIRBefore((LIR*)lir, (LIR*)newDeltaHi);
                    LIR *newDeltaLo =
                          rawLIR(cUnit, lir->dalvikOffset, kX86DeltaLo,
                                 lir->operands[0], 0, lir->operands[2],
                                 lir->operands[3], lir->target);
                    oatInsertLIRBefore((LIR*)lir, (LIR*)newDeltaLo);
                    lir->flags.isNop = true;
                    res = kRetryAll;
                }
            } else if (lir->opcode == kX86DeltaLo) {
                int offset1 = ((LIR*)lir->operands[2])->offset;
                SwitchTable *tabRec = (SwitchTable*)lir->operands[3];
                int offset2 = tabRec ? tabRec->offset : lir->target->offset;
                int delta = offset2 - offset1;
                lir->operands[1] = delta & 0xffff;
            } else if (lir->opcode == kX86DeltaHi) {
                int offset1 = ((LIR*)lir->operands[2])->offset;
                SwitchTable *tabRec = (SwitchTable*)lir->operands[3];
                int offset2 = tabRec ? tabRec->offset : lir->target->offset;
                int delta = offset2 - offset1;
                lir->operands[1] = (delta >> 16) & 0xffff;
            } else if (lir->opcode == kX86B || lir->opcode == kX86Bal) {
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
            } else if (lir->opcode >= kX86Beqz && lir->opcode <= kX86Bnez) {
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
            } else if (lir->opcode == kX86Beq || lir->opcode == kX86Bne) {
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
            } else if (lir->opcode == kX86Jal) {
                intptr_t curPC = (startAddr + lir->offset + 4) & ~3;
                intptr_t target = lir->operands[0];
                /* ensure PC-region branch can be used */
                DCHECK_EQ((curPC & 0xF0000000), (target & 0xF0000000));
                if (target & 0x3) {
                    LOG(FATAL) << "Jump target not multiple of 4: " << target;
                }
                lir->operands[0] =  target >> 2;
            } else if (lir->opcode == kX86Lahi) { /* ld address hi (via lui) */
                LIR *targetLIR = (LIR *) lir->target;
                intptr_t target = startAddr + targetLIR->offset;
                lir->operands[1] = target >> 16;
            } else if (lir->opcode == kX86Lalo) { /* ld address lo (via ori) */
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
        const X86EncodingMap *encoder = &EncodingMap[lir->opcode];
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
            const X86EncodingMap *encoder = &EncodingMap[kX86Nop];
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
  switch (EncodingMap[lir->opcode].kind) {
    case kData:
      return 4;
    case kRegImm: {
      int reg = lir->operands[0];
      int imm = lir->operands[1];
      return (reg == rAX ? 1 : 2) +  // AX opcodes don't require the modrm byte
             (IS_SIMM8(imm) ? 1 : 4);  // 1 or 4 byte immediate
      break;
    }
    case kMemImm: {
      // int base = lir->operands[0];
      int disp = lir->operands[1];
      int imm  = lir->operands[2];
      return 2 +  // opcode and modrm bytes
          (disp == 0 ? 0 : (IS_SIMM8(disp) ? 1 : 4)) +  // 0, 1 or 4 byte displacement
          (IS_SIMM8(imm) ? 1 : 4);  // 1 or 4 byte immediate
      break;
    }
    case kArrayImm:
      UNIMPLEMENTED(FATAL);
      return 0;
    case kRegReg:
      return 2;  // opcode and modrm
    case kRegMem: {
      // int reg =  lir->operands[0];
      // int base = lir->operands[1];
      int disp = lir->operands[2];
      return 2 +  // opcode and modrm bytes
          (disp == 0 ? 0 : (IS_SIMM8(disp) ? 1 : 4));  // 0, 1 or 4 byte displacement
      break;
    }
    case kRegArray:
      UNIMPLEMENTED(FATAL);
      return 0;
    case kMemReg: {
      // int base =  lir->operands[0];
      int disp = lir->operands[1];
      // int reg = lir->operands[2];
      return 2 +  // opcode and modrm bytes
          (disp == 0 ? 0 : (IS_SIMM8(disp) ? 1 : 4));  // 0, 1 or 4 byte displacement
      break;
    }
    case kArrayReg:
      UNIMPLEMENTED(FATAL);
      return 0;
  }
  UNIMPLEMENTED(FATAL);  // unreachable
  return 0;
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
