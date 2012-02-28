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

#include "../../CompilerInternals.h"
#include "MipsLIR.h"
#include "../Ralloc.h"

#include <string>

namespace art {

/* For dumping instructions */
#define MIPS_REG_COUNT 32
static const char *mipsRegName[MIPS_REG_COUNT] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

/*
 * Interpret a format string and build a string no longer than size
 * See format key in Assemble.c.
 */
STATIC std::string buildInsnString(const char *fmt, MipsLIR *lir, unsigned char* baseAddr)
{
    std::string buf;
    int i;
    const char *fmtEnd = &fmt[strlen(fmt)];
    char tbuf[256];
    char nc;
    while (fmt < fmtEnd) {
        int operand;
        if (*fmt == '!') {
            fmt++;
            DCHECK_LT(fmt, fmtEnd);
            nc = *fmt++;
            if (nc=='!') {
                strcpy(tbuf, "!");
            } else {
               DCHECK_LT(fmt, fmtEnd);
               DCHECK_LT((unsigned)(nc-'0'), 4u);
               operand = lir->operands[nc-'0'];
               switch(*fmt++) {
                   case 'b':
                       strcpy(tbuf,"0000");
                       for (i=3; i>= 0; i--) {
                           tbuf[i] += operand & 1;
                           operand >>= 1;
                       }
                       break;
                   case 's':
                       sprintf(tbuf,"$f%d",operand & FP_REG_MASK);
                       break;
                   case 'S':
                       DCHECK_EQ(((operand & FP_REG_MASK) & 1), 0);
                       sprintf(tbuf,"$f%d",operand & FP_REG_MASK);
                       break;
                   case 'h':
                       sprintf(tbuf,"%04x", operand);
                       break;
                   case 'M':
                   case 'd':
                       sprintf(tbuf,"%d", operand);
                       break;
                   case 'D':
                       sprintf(tbuf,"%d", operand+1);
                       break;
                   case 'E':
                       sprintf(tbuf,"%d", operand*4);
                       break;
                   case 'F':
                       sprintf(tbuf,"%d", operand*2);
                       break;
                   case 'c':
                       switch (operand) {
                           case kMipsCondEq:
                               strcpy(tbuf, "eq");
                               break;
                           case kMipsCondNe:
                               strcpy(tbuf, "ne");
                               break;
                           case kMipsCondLt:
                               strcpy(tbuf, "lt");
                               break;
                           case kMipsCondGe:
                               strcpy(tbuf, "ge");
                               break;
                           case kMipsCondGt:
                               strcpy(tbuf, "gt");
                               break;
                           case kMipsCondLe:
                               strcpy(tbuf, "le");
                               break;
                           case kMipsCondCs:
                               strcpy(tbuf, "cs");
                               break;
                           case kMipsCondMi:
                               strcpy(tbuf, "mi");
                               break;
                           default:
                               strcpy(tbuf, "");
                               break;
                       }
                       break;
                   case 't':
                       sprintf(tbuf,"0x%08x (L%p)",
                               (int) baseAddr + lir->generic.offset + 4 +
                               (operand << 2),
                               lir->generic.target);
                       break;
                   case 'T':
                       sprintf(tbuf,"0x%08x",
                               (int) (operand << 2));
                       break;
                   case 'u': {
                       int offset_1 = lir->operands[0];
                       int offset_2 = NEXT_LIR(lir)->operands[0];
                       intptr_t target =
                           ((((intptr_t) baseAddr + lir->generic.offset + 4) &
                            ~3) + (offset_1 << 21 >> 9) + (offset_2 << 1)) &
                           0xfffffffc;
                       sprintf(tbuf, "%p", (void *) target);
                       break;
                    }

                   /* Nothing to print for BLX_2 */
                   case 'v':
                       strcpy(tbuf, "see above");
                       break;
                   case 'r':
                       DCHECK(operand >= 0 && operand < MIPS_REG_COUNT);
                       strcpy(tbuf, mipsRegName[operand]);
                       break;
                   default:
                       strcpy(tbuf,"DecodeError");
                       break;
               }
               buf += tbuf;
            }
        } else {
           buf += *fmt++;
        }
    }
    return buf;
}

// FIXME: need to redo resourse maps for MIPS - fix this at that time
void oatDumpResourceMask(LIR *lir, u8 mask, const char *prefix)
{
    char buf[256];
    buf[0] = 0;
    MipsLIR *mipsLIR = (MipsLIR *) lir;

    if (mask == ENCODE_ALL) {
        strcpy(buf, "all");
    } else {
        char num[8];
        int i;

        for (i = 0; i < kRegEnd; i++) {
            if (mask & (1ULL << i)) {
                sprintf(num, "%d ", i);
                strcat(buf, num);
            }
        }

        if (mask & ENCODE_CCODE) {
            strcat(buf, "cc ");
        }
        if (mask & ENCODE_FP_STATUS) {
            strcat(buf, "fpcc ");
        }
        /* Memory bits */
        if (mipsLIR && (mask & ENCODE_DALVIK_REG)) {
            sprintf(buf + strlen(buf), "dr%d%s", mipsLIR->aliasInfo & 0xffff,
                    (mipsLIR->aliasInfo & 0x80000000) ? "(+1)" : "");
        }
        if (mask & ENCODE_LITERAL) {
            strcat(buf, "lit ");
        }

        if (mask & ENCODE_HEAP_REF) {
            strcat(buf, "heap ");
        }
        if (mask & ENCODE_MUST_NOT_ALIAS) {
            strcat(buf, "noalias ");
        }
    }
    if (buf[0]) {
        LOG(INFO) << prefix << ": " <<  buf;
    }
}

/*
 * Debugging macros
 */
#define DUMP_RESOURCE_MASK(X)
#define DUMP_SSA_REP(X)

/* Pretty-print a LIR instruction */
void oatDumpLIRInsn(CompilationUnit* cUnit, LIR* arg, unsigned char* baseAddr)
{
    MipsLIR* lir = (MipsLIR*) arg;
    int offset = lir->generic.offset;
    int dest = lir->operands[0];
    const bool dumpNop = false;

    /* Handle pseudo-ops individually, and all regular insns as a group */
    switch(lir->opcode) {
        case kPseudoMethodEntry:
            LOG(INFO) << "-------- method entry " <<
                PrettyMethod(cUnit->method_idx, *cUnit->dex_file);
            break;
        case kPseudoMethodExit:
            LOG(INFO) << "-------- Method_Exit";
            break;
        case kPseudoBarrier:
            LOG(INFO) << "-------- BARRIER";
            break;
        case kPseudoExtended:
            LOG(INFO) << "-------- " << (char* ) dest;
            break;
        case kPseudoSSARep:
            DUMP_SSA_REP(LOG(INFO) << "-------- kMirOpPhi: " <<  (char* ) dest);
            break;
        case kPseudoEntryBlock:
            LOG(INFO) << "-------- entry offset: 0x" << std::hex << dest;
            break;
        case kPseudoDalvikByteCodeBoundary:
            LOG(INFO) << "-------- dalvik offset: 0x" << std::hex <<
                 lir->generic.dalvikOffset << " @ " << (char* )lir->operands[0];
            break;
        case kPseudoExitBlock:
            LOG(INFO) << "-------- exit offset: 0x" << std::hex << dest;
            break;
        case kPseudoPseudoAlign4:
            LOG(INFO) << (intptr_t)baseAddr + offset << " (0x" << std::hex <<
                offset << "): .align4";
            break;
        case kPseudoEHBlockLabel:
            LOG(INFO) << "Exception_Handling:";
            break;
        case kPseudoTargetLabel:
        case kPseudoNormalBlockLabel:
            LOG(INFO) << "L" << (intptr_t)lir << ":";
            break;
        case kPseudoThrowTarget:
            LOG(INFO) << "LT" << (intptr_t)lir << ":";
            break;
        case kPseudoSuspendTarget:
            LOG(INFO) << "LS" << (intptr_t)lir << ":";
            break;
        case kPseudoCaseLabel:
            LOG(INFO) << "LC" << (intptr_t)lir << ": Case target 0x" <<
                std::hex << lir->operands[0] << "|" << std::dec <<
                lir->operands[0];
            break;
        default:
            if (lir->flags.isNop && !dumpNop) {
                break;
            } else {
                std::string op_name(buildInsnString(EncodingMap[lir->opcode].name, lir, baseAddr));
                std::string op_operands(buildInsnString(EncodingMap[lir->opcode].fmt, lir, baseAddr));
                LOG(INFO) << StringPrintf("%p (%04x): %-9s%s%s", baseAddr + offset, offset,
                    op_name.c_str(), op_operands.c_str(), lir->flags.isNop ? "(nop)" : "");
            }
            break;
    }

    if (lir->useMask && (!lir->flags.isNop || dumpNop)) {
        DUMP_RESOURCE_MASK(oatDumpResourceMask((LIR* ) lir,
                                               lir->useMask, "use"));
    }
    if (lir->defMask && (!lir->flags.isNop || dumpNop)) {
        DUMP_RESOURCE_MASK(oatDumpResourceMask((LIR* ) lir,
                                               lir->defMask, "def"));
    }
}

void oatDumpPromotionMap(CompilationUnit *cUnit)
{
    for (int i = 0; i < cUnit->numDalvikRegisters; i++) {
        PromotionMap vRegMap = cUnit->promotionMap[i];
        char buf[100];
        if (vRegMap.fpLocation == kLocPhysReg) {
            snprintf(buf, 100, " : s%d", vRegMap.fpReg & FP_REG_MASK);
        } else {
            buf[0] = 0;
        }
        char buf2[100];
        snprintf(buf2, 100, "V[%02d] -> %s%d%s", i,
                 vRegMap.coreLocation == kLocPhysReg ?
                 "r" : "SP+", vRegMap.coreLocation == kLocPhysReg ?
                 vRegMap.coreReg : oatSRegOffset(cUnit, i), buf);
        LOG(INFO) << buf2;
    }
}

void oatDumpFullPromotionMap(CompilationUnit *cUnit)
{
    for (int i = 0; i < cUnit->numDalvikRegisters; i++) {
        PromotionMap vRegMap = cUnit->promotionMap[i];
        LOG(INFO) << i << " -> " << "CL:" << (int)vRegMap.coreLocation <<
            ", CR:" << (int)vRegMap.coreReg << ", FL:" <<
            (int)vRegMap.fpLocation << ", FR:" << (int)vRegMap.fpReg <<
            ", - " << (int)vRegMap.firstInPair;
    }
}

/* Dump instructions and constant pool contents */
void oatCodegenDump(CompilationUnit* cUnit)
{
    LOG(INFO) << "/*";
    LOG(INFO) << "Dumping LIR insns for "
        << PrettyMethod(cUnit->method_idx, *cUnit->dex_file);
    LIR* lirInsn;
    MipsLIR* mipsLIR;
    int insnsSize = cUnit->insnsSize;

    LOG(INFO) << "Regs (excluding ins) : " << cUnit->numRegs;
    LOG(INFO) << "Ins                  : " << cUnit->numIns;
    LOG(INFO) << "Outs                 : " << cUnit->numOuts;
    LOG(INFO) << "CoreSpills           : " << cUnit->numCoreSpills;
    LOG(INFO) << "FPSpills             : " << cUnit->numFPSpills;
    LOG(INFO) << "Padding              : " << cUnit->numPadding;
    LOG(INFO) << "Frame size           : " << cUnit->frameSize;
    LOG(INFO) << "Start of ins         : " << cUnit->insOffset;
    LOG(INFO) << "Start of regs        : " << cUnit->regsOffset;
    LOG(INFO) << "code size is " << cUnit->totalSize <<
        " bytes, Dalvik size is " << insnsSize * 2;
    LOG(INFO) << "expansion factor: " <<
         (float)cUnit->totalSize / (float)(insnsSize * 2);
    oatDumpPromotionMap(cUnit);
    for (lirInsn = cUnit->firstLIRInsn; lirInsn; lirInsn = lirInsn->next) {
        oatDumpLIRInsn(cUnit, lirInsn, 0);
    }
    for (lirInsn = cUnit->classPointerList; lirInsn; lirInsn = lirInsn->next) {
        mipsLIR = (MipsLIR*) lirInsn;
        LOG(INFO) << StringPrintf("%x (%04x): .class (%s)",
            mipsLIR->generic.offset, mipsLIR->generic.offset,
            ((CallsiteInfo *) mipsLIR->operands[0])->classDescriptor);
    }
    for (lirInsn = cUnit->literalList; lirInsn; lirInsn = lirInsn->next) {
        mipsLIR = (MipsLIR*) lirInsn;
        LOG(INFO) << StringPrintf("%x (%04x): .word (%#x)",
            mipsLIR->generic.offset, mipsLIR->generic.offset, mipsLIR->operands[0]);
    }

    const DexFile::MethodId& method_id =
        cUnit->dex_file->GetMethodId(cUnit->method_idx);
    std::string signature(cUnit->dex_file->GetMethodSignature(method_id));
    std::string name(cUnit->dex_file->GetMethodName(method_id));
    std::string descriptor(cUnit->dex_file->GetMethodDeclaringClassDescriptor(method_id));

    // Dump mapping table
    if (cUnit->mappingTable.size() > 0) {
        std::string line(StringPrintf("\n    MappingTable %s%s_%s_mappingTable[%zu] = {",
            descriptor.c_str(), name.c_str(), signature.c_str(), cUnit->mappingTable.size()));
        std::replace(line.begin(), line.end(), ';', '_');
        LOG(INFO) << line;
        for (uint32_t i = 0; i < cUnit->mappingTable.size(); i+=2) {
            line = StringPrintf("        {0x%08x, 0x%04x},",
                cUnit->mappingTable[i], cUnit->mappingTable[i+1]);
            LOG(INFO) << line;
        }
        LOG(INFO) <<"    };\n\n";
    }
}

} // namespace art
