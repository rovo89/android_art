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

/*
 * This file contains register alloction support and is intended to be
 * included by:
 *
 *        Codegen-$(TARGET_ARCH_VARIANT).c
 *
 */

#include "../../CompilerIR.h"

namespace art {

#if defined(_CODEGEN_C)
bool genAddLong(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2);
bool genSubLong(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2);
bool genNegLong(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                RegLocation rlSrc);
LIR *opRegImm(CompilationUnit* cUnit, OpKind op, int rDestSrc1, int value);
LIR *opRegReg(CompilationUnit* cUnit, OpKind op, int rDestSrc1, int rSrc2);
LIR* opCmpBranch(CompilationUnit* cUnit, ConditionCode cond, int src1,
                 int src2, LIR* target);
LIR* opCmpImmBranch(CompilationUnit* cUnit, ConditionCode cond, int reg,
                    int checkValue, LIR* target);

/* Forward declaraton the portable versions due to circular dependency */
bool genArithOpFloatPortable(CompilationUnit* cUnit, MIR* mir,
                                    RegLocation rlDest, RegLocation rlSrc1,
                                    RegLocation rlSrc2);

bool genArithOpDoublePortable(CompilationUnit* cUnit, MIR* mir,
                                     RegLocation rlDest, RegLocation rlSrc1,
                                     RegLocation rlSrc2);

bool genConversionPortable(CompilationUnit* cUnit, MIR* mir);

int loadHelper(CompilationUnit* cUnit, int offset);
LIR* callRuntimeHelper(CompilationUnit* cUnit, int reg);
RegLocation getRetLoc(CompilationUnit* cUnit);
LIR* loadConstant(CompilationUnit* cUnit, int reg, int immVal);
void opRegCopyWide(CompilationUnit* cUnit, int destLo, int destHi,
                   int srcLo, int srcHi);
LIR* opRegCopy(CompilationUnit* cUnit, int rDest, int rSrc);
void freeRegLocTemps(CompilationUnit* cUnit, RegLocation rlKeep,
                     RegLocation rlFree);


/*
 * Return most flexible allowed register class based on size.
 * Bug: 2813841
 * Must use a core register for data types narrower than word (due
 * to possible unaligned load/store.
 */
inline RegisterClass oatRegClassBySize(OpSize size)
{
    return (size == kUnsignedHalf ||
            size == kSignedHalf ||
            size == kUnsignedByte ||
            size == kSignedByte ) ? kCoreReg : kAnyReg;
}

/*
 * Construct an s4 from two consecutive half-words of switch data.
 * This needs to check endianness because the DEX optimizer only swaps
 * half-words in instruction stream.
 *
 * "switchData" must be 32-bit aligned.
 */
#if __BYTE_ORDER == __LITTLE_ENDIAN
inline s4 s4FromSwitchData(const void* switchData) {
    return *(s4*) switchData;
}
#else
inline s4 s4FromSwitchData(const void* switchData) {
    u2* data = switchData;
    return data[0] | (((s4) data[1]) << 16);
}
#endif

#endif

extern void oatSetupResourceMasks(LIR* lir);

extern LIR* oatRegCopyNoInsert(CompilationUnit* cUnit, int rDest,
                                          int rSrc);

}  // namespace art
