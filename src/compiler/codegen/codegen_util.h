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

inline int32_t s4FromSwitchData(const void* switchData) { return *(int32_t*) switchData; }
inline RegisterClass oatRegClassBySize(OpSize size) { return (size == kUnsignedHalf || size == kSignedHalf || size == kUnsignedByte || size == kSignedByte ) ? kCoreReg : kAnyReg; }
void oatAssembleLIR(CompilationUnit* cUnit);
void setMemRefType(LIR* lir, bool isLoad, int memType);
void annotateDalvikRegAccess(LIR* lir, int regId, bool isLoad, bool is64bit);
uint64_t oatGetRegMaskCommon(CompilationUnit* cUnit, int reg);
void oatSetupRegMask(CompilationUnit* cUnit, uint64_t* mask, int reg);
void setupResourceMasks(CompilationUnit* cUnit, LIR* lir);
void oatDumpLIRInsn(CompilationUnit* cUnit, LIR* arg, unsigned char* baseAddr);
void oatDumpPromotionMap(CompilationUnit *cUnit);
void dumpMappingTable(const char* table_name, const std::string& descriptor, const std::string& name, const std::string& signature, const std::vector<uint32_t>& v);
void oatCodegenDump(CompilationUnit* cUnit);
// TODO: remove default parameters
LIR* rawLIR(CompilationUnit* cUnit, int dalvikOffset, int opcode, int op0 = 0, int op1 = 0, int op2 = 0, int op3 = 0, int op4 = 0, LIR* target = NULL);
LIR* newLIR0(CompilationUnit* cUnit, int opcode);
LIR* newLIR1(CompilationUnit* cUnit, int opcode, int dest);
LIR* newLIR2(CompilationUnit* cUnit, int opcode, int dest, int src1);
LIR* newLIR3(CompilationUnit* cUnit, int opcode, int dest, int src1, int src2);
LIR* newLIR4(CompilationUnit* cUnit, int opcode, int dest, int src1, int src2, int info);
LIR* newLIR5(CompilationUnit* cUnit, int opcode, int dest, int src1, int src2, int info1, int info2);
LIR* scanLiteralPool(LIR* dataTarget, int value, unsigned int delta);
LIR* scanLiteralPoolWide(LIR* dataTarget, int valLo, int valHi);
LIR* addWordData(CompilationUnit* cUnit, LIR* *constantListP, int value);
LIR* addWideData(CompilationUnit* cUnit, LIR* *constantListP, int valLo, int valHi);
void oatProcessSwitchTables(CompilationUnit* cUnit);
void dumpSparseSwitchTable(const uint16_t* table);
void dumpPackedSwitchTable(const uint16_t* table);
LIR* markBoundary(CompilationUnit* cUnit, int offset, const char* instStr);

}  // namespace art

#endif // ART_SRC_COMPILER_CODEGEN_CODEGENUTIL_H_
