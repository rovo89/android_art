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

MipsConditionCode oatMipsConditionEncoding(ConditionCode code)
{
    MipsConditionCode res;
    switch(code) {
        case kCondEq: res = kMipsCondEq; break;
        case kCondNe: res = kMipsCondNe; break;
        case kCondCs: res = kMipsCondCs; break;
        case kCondCc: res = kMipsCondCc; break;
        case kCondMi: res = kMipsCondMi; break;
        case kCondPl: res = kMipsCondPl; break;
        case kCondVs: res = kMipsCondVs; break;
        case kCondVc: res = kMipsCondVc; break;
        case kCondHi: res = kMipsCondHi; break;
        case kCondLs: res = kMipsCondLs; break;
        case kCondGe: res = kMipsCondGe; break;
        case kCondLt: res = kMipsCondLt; break;
        case kCondGt: res = kMipsCondGt; break;
        case kCondLe: res = kMipsCondLe; break;
        case kCondAl: res = kMipsCondAl; break;
        case kCondNv: res = kMipsCondNv; break;
        default:
            LOG(FATAL) << "Bad condition code" << (int)code;
            res = (MipsConditionCode)0;  // Quiet gcc
    }
    return res;
}

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
std::string buildInsnString(const char *fmt, LIR *lir, unsigned char* baseAddr)
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
                               (int) baseAddr + lir->offset + 4 +
                               (operand << 2),
                               lir->target);
                       break;
                   case 'T':
                       sprintf(tbuf,"0x%08x",
                               (int) (operand << 2));
                       break;
                   case 'u': {
                       int offset_1 = lir->operands[0];
                       int offset_2 = NEXT_LIR(lir)->operands[0];
                       intptr_t target =
                           ((((intptr_t) baseAddr + lir->offset + 4) &
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
    LIR *mipsLIR = (LIR *) lir;

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

} // namespace art
