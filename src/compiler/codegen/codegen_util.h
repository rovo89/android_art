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

#ifndef ART_SRC_COMPILER_CODEGEN_CODEGENUTIL_H_
#define ART_SRC_COMPILER_CODEGEN_CODEGENUTIL_H_

namespace art {

inline int32_t s4FromSwitchData(const void* switchData) { return *reinterpret_cast<const int32_t*>(switchData); }
inline RegisterClass oatRegClassBySize(OpSize size) { return (size == kUnsignedHalf || size == kSignedHalf || size == kUnsignedByte || size == kSignedByte ) ? kCoreReg : kAnyReg; }
void AssembleLIR(CompilationUnit* cUnit);
void SetMemRefType(LIR* lir, bool isLoad, int memType);
void AnnotateDalvikRegAccess(LIR* lir, int regId, bool isLoad, bool is64bit);
uint64_t GetRegMaskCommon(CompilationUnit* cUnit, int reg);
void SetupRegMask(CompilationUnit* cUnit, uint64_t* mask, int reg);
void SetupResourceMasks(CompilationUnit* cUnit, LIR* lir);
void DumpLIRInsn(CompilationUnit* cUnit, LIR* arg, unsigned char* baseAddr);
void DumpPromotionMap(CompilationUnit *cUnit);
void CodegenDump(CompilationUnit* cUnit);
// TODO: remove default parameters
LIR* RawLIR(CompilationUnit* cUnit, int dalvikOffset, int opcode, int op0 = 0, int op1 = 0, int op2 = 0, int op3 = 0, int op4 = 0, LIR* target = NULL);
LIR* NewLIR0(CompilationUnit* cUnit, int opcode);
LIR* NewLIR1(CompilationUnit* cUnit, int opcode, int dest);
LIR* NewLIR2(CompilationUnit* cUnit, int opcode, int dest, int src1);
LIR* NewLIR3(CompilationUnit* cUnit, int opcode, int dest, int src1, int src2);
LIR* NewLIR4(CompilationUnit* cUnit, int opcode, int dest, int src1, int src2, int info);
LIR* NewLIR5(CompilationUnit* cUnit, int opcode, int dest, int src1, int src2, int info1, int info2);
LIR* ScanLiteralPool(LIR* dataTarget, int value, unsigned int delta);
LIR* ScanLiteralPoolWide(LIR* dataTarget, int valLo, int valHi);
LIR* AddWordData(CompilationUnit* cUnit, LIR* *constantListP, int value);
LIR* AddWideData(CompilationUnit* cUnit, LIR* *constantListP, int valLo, int valHi);
void ProcessSwitchTables(CompilationUnit* cUnit);
void DumpSparseSwitchTable(const uint16_t* table);
void DumpPackedSwitchTable(const uint16_t* table);
LIR* MarkBoundary(CompilationUnit* cUnit, int offset, const char* instStr);

}  // namespace art

#endif // ART_SRC_COMPILER_CODEGEN_CODEGENUTIL_H_
