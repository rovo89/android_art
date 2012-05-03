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

#include "../../CompilerInternals.h"
#include "ArmLIR.h"
#include "../Ralloc.h"

#include <string>

namespace art {

ArmConditionCode oatArmConditionEncoding(ConditionCode code)
{
  ArmConditionCode res;
  switch (code) {
    case kCondEq: res = kArmCondEq; break;
    case kCondNe: res = kArmCondNe; break;
    case kCondCs: res = kArmCondCs; break;
    case kCondCc: res = kArmCondCc; break;
    case kCondMi: res = kArmCondMi; break;
    case kCondPl: res = kArmCondPl; break;
    case kCondVs: res = kArmCondVs; break;
    case kCondVc: res = kArmCondVc; break;
    case kCondHi: res = kArmCondHi; break;
    case kCondLs: res = kArmCondLs; break;
    case kCondGe: res = kArmCondGe; break;
    case kCondLt: res = kArmCondLt; break;
    case kCondGt: res = kArmCondGt; break;
    case kCondLe: res = kArmCondLe; break;
    case kCondAl: res = kArmCondAl; break;
    case kCondNv: res = kArmCondNv; break;
    default:
      LOG(FATAL) << "Bad condition code" << (int)code;
      res = (ArmConditionCode)0;  // Quiet gcc
  }
  return res;
}

static const char* coreRegNames[16] = {
  "r0",
  "r1",
  "r2",
  "r3",
  "r4",
  "r5",
  "r6",
  "r7",
  "r8",
  "rSELF",
  "r10",
  "r11",
  "r12",
  "sp",
  "lr",
  "pc",
};


static const char* shiftNames[4] = {
  "lsl",
  "lsr",
  "asr",
  "ror"};

/* Decode and print a ARM register name */
char* decodeRegList(int opcode, int vector, char* buf)
{
  int i;
  bool printed = false;
  buf[0] = 0;
  for (i = 0; i < 16; i++, vector >>= 1) {
    if (vector & 0x1) {
      int regId = i;
      if (opcode == kThumbPush && i == 8) {
        regId = r14lr;
      } else if (opcode == kThumbPop && i == 8) {
        regId = r15pc;
      }
      if (printed) {
        sprintf(buf + strlen(buf), ", r%d", regId);
      } else {
        printed = true;
        sprintf(buf, "r%d", regId);
      }
    }
  }
  return buf;
}

char*  decodeFPCSRegList(int count, int base, char* buf)
{
  sprintf(buf, "s%d", base);
  for (int i = 1; i < count; i++) {
    sprintf(buf + strlen(buf), ", s%d",base + i);
  }
  return buf;
}

int expandImmediate(int value)
{
  int mode = (value & 0xf00) >> 8;
  u4 bits = value & 0xff;
  switch (mode) {
    case 0:
      return bits;
     case 1:
      return (bits << 16) | bits;
     case 2:
      return (bits << 24) | (bits << 8);
     case 3:
      return (bits << 24) | (bits << 16) | (bits << 8) | bits;
    default:
      break;
  }
  bits = (bits | 0x80) << 24;
  return bits >> (((value & 0xf80) >> 7) - 8);
}

const char* ccNames[] = {"eq","ne","cs","cc","mi","pl","vs","vc",
                         "hi","ls","ge","lt","gt","le","al","nv"};
/*
 * Interpret a format string and build a string no longer than size
 * See format key in Assemble.c.
 */
std::string buildInsnString(const char* fmt, LIR* lir, unsigned char* baseAddr)
{
  std::string buf;
  int i;
  const char* fmtEnd = &fmt[strlen(fmt)];
  char tbuf[256];
  const char* name;
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
         DCHECK_LT((unsigned)(nc-'0'), 4U);
         operand = lir->operands[nc-'0'];
         switch (*fmt++) {
           case 'H':
             if (operand != 0) {
               sprintf(tbuf, ", %s %d",shiftNames[operand & 0x3], operand >> 2);
             } else {
               strcpy(tbuf,"");
             }
             break;
           case 'B':
             switch (operand) {
               case kSY:
                 name = "sy";
                 break;
               case kST:
                 name = "st";
                 break;
               case kISH:
                 name = "ish";
                 break;
               case kISHST:
                 name = "ishst";
                 break;
               case kNSH:
                 name = "nsh";
                 break;
               case kNSHST:
                 name = "shst";
                 break;
               default:
                 name = "DecodeError2";
                 break;
             }
             strcpy(tbuf, name);
             break;
           case 'b':
             strcpy(tbuf,"0000");
             for (i=3; i>= 0; i--) {
               tbuf[i] += operand & 1;
               operand >>= 1;
             }
             break;
           case 'n':
             operand = ~expandImmediate(operand);
             sprintf(tbuf,"%d [%#x]", operand, operand);
             break;
           case 'm':
             operand = expandImmediate(operand);
             sprintf(tbuf,"%d [%#x]", operand, operand);
             break;
           case 's':
             sprintf(tbuf,"s%d",operand & FP_REG_MASK);
             break;
           case 'S':
             sprintf(tbuf,"d%d",(operand & FP_REG_MASK) >> 1);
             break;
           case 'h':
             sprintf(tbuf,"%04x", operand);
             break;
           case 'M':
           case 'd':
             sprintf(tbuf,"%d", operand);
             break;
           case 'C':
             sprintf(tbuf,"%s",coreRegNames[operand]);
             break;
           case 'E':
             sprintf(tbuf,"%d", operand*4);
             break;
           case 'F':
             sprintf(tbuf,"%d", operand*2);
             break;
           case 'c':
             strcpy(tbuf, ccNames[operand]);
             break;
           case 't':
             sprintf(tbuf,"0x%08x (L%p)",
                 (int) baseAddr + lir->offset + 4 +
                 (operand << 1),
                 lir->target);
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
           case 'R':
             decodeRegList(lir->opcode, operand, tbuf);
             break;
           case 'P':
             decodeFPCSRegList(operand, 16, tbuf);
             break;
           case 'Q':
             decodeFPCSRegList(operand, 0, tbuf);
             break;
           default:
             strcpy(tbuf,"DecodeError1");
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

void oatDumpResourceMask(LIR* lir, u8 mask, const char* prefix)
{
  char buf[256];
  buf[0] = 0;
  LIR* armLIR = (LIR*) lir;

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
    if (armLIR && (mask & ENCODE_DALVIK_REG)) {
      sprintf(buf + strlen(buf), "dr%d%s", armLIR->aliasInfo & 0xffff,
              (armLIR->aliasInfo & 0x80000000) ? "(+1)" : "");
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
    LOG(INFO) << prefix << ": " << buf;
  }
}


}  // namespace art
