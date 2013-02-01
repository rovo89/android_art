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

#include <stdint.h>

#include "compiler/compiler_enums.h"

namespace art {

class CompilationUnit;
struct LIR;

void MarkSafepointPC(CompilationUnit* cu, LIR* inst);
bool FastInstance(CompilationUnit* cu,  uint32_t field_idx,
                  int& field_offset, bool& is_volatile, bool is_put);
void SetupResourceMasks(CompilationUnit* cu, LIR* lir);
inline int32_t s4FromSwitchData(const void* switch_data) { return *reinterpret_cast<const int32_t*>(switch_data); }
inline RegisterClass oat_reg_class_by_size(OpSize size) { return (size == kUnsignedHalf || size == kSignedHalf || size == kUnsignedByte || size == kSignedByte ) ? kCoreReg : kAnyReg; }
void AssembleLIR(CompilationUnit* cu);
void SetMemRefType(CompilationUnit* cu, LIR* lir, bool is_load, int mem_type);
void AnnotateDalvikRegAccess(CompilationUnit* cu, LIR* lir, int reg_id, bool is_load, bool is64bit);
uint64_t GetRegMaskCommon(CompilationUnit* cu, int reg);
void SetupRegMask(CompilationUnit* cu, uint64_t* mask, int reg);
void SetupResourceMasks(CompilationUnit* cu, LIR* lir);
void DumpLIRInsn(CompilationUnit* cu, LIR* arg, unsigned char* base_addr);
void DumpPromotionMap(CompilationUnit *cu);
void CodegenDump(CompilationUnit* cu);
LIR* RawLIR(CompilationUnit* cu, int dalvik_offset, int opcode, int op0 = 0, int op1 = 0,
            int op2 = 0, int op3 = 0, int op4 = 0, LIR* target = NULL);
LIR* NewLIR0(CompilationUnit* cu, int opcode);
LIR* NewLIR1(CompilationUnit* cu, int opcode, int dest);
LIR* NewLIR2(CompilationUnit* cu, int opcode, int dest, int src1);
LIR* NewLIR3(CompilationUnit* cu, int opcode, int dest, int src1, int src2);
LIR* NewLIR4(CompilationUnit* cu, int opcode, int dest, int src1, int src2, int info);
LIR* NewLIR5(CompilationUnit* cu, int opcode, int dest, int src1, int src2, int info1, int info2);
LIR* ScanLiteralPool(LIR* data_target, int value, unsigned int delta);
LIR* ScanLiteralPoolWide(LIR* data_target, int val_lo, int val_hi);
LIR* AddWordData(CompilationUnit* cu, LIR* *constant_list_p, int value);
LIR* AddWideData(CompilationUnit* cu, LIR* *constant_list_p, int val_lo, int val_hi);
void ProcessSwitchTables(CompilationUnit* cu);
void DumpSparseSwitchTable(const uint16_t* table);
void DumpPackedSwitchTable(const uint16_t* table);
LIR* MarkBoundary(CompilationUnit* cu, int offset, const char* inst_str);
void NopLIR(LIR* lir);
bool EvaluateBranch(Instruction::Code opcode, int src1, int src2);

}  // namespace art

#endif // ART_SRC_COMPILER_CODEGEN_CODEGENUTIL_H_
