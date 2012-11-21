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

#ifndef ART_SRC_COMPILER_CODEGEN_GENCOMMON_H_
#define ART_SRC_COMPILER_CODEGEN_GENCOMMON_H_

void MarkSafepointPC(CompilationUnit* cu, LIR* inst);
void CallRuntimeHelperImm(CompilationUnit* cu, int helper_offset, int arg0, bool safepoint_pc);
void CallRuntimeHelperReg(CompilationUnit* cu, int helper_offset, int arg0, bool safepoint_pc);
void CallRuntimeHelperRegLocation(CompilationUnit* cu, int helper_offset, RegLocation arg0, bool safepoint_pc);
void CallRuntimeHelperImmImm(CompilationUnit* cu, int helper_offset, int arg0, int arg1, bool safepoint_pc);
void CallRuntimeHelperImmRegLocation(CompilationUnit* cu, int helper_offset, int arg0, RegLocation arg1, bool safepoint_pc);
void CallRuntimeHelperRegLocationImm(CompilationUnit* cu, int helper_offset, RegLocation arg0, int arg1, bool safepoint_pc);
void CallRuntimeHelperImmReg(CompilationUnit* cu, int helper_offset, int arg0, int arg1, bool safepoint_pc);
void CallRuntimeHelperRegImm(CompilationUnit* cu, int helper_offset, int arg0, int arg1, bool safepoint_pc);
void CallRuntimeHelperImmMethod(CompilationUnit* cu, int helper_offset, int arg0, bool safepoint_pc);
void CallRuntimeHelperRegLocationRegLocation(CompilationUnit* cu, int helper_offset, RegLocation arg0, RegLocation arg1, bool safepoint_pc);
void CallRuntimeHelperRegReg(CompilationUnit* cu, int helper_offset, int arg0, int arg1, bool safepoint_pc);
void CallRuntimeHelperRegRegImm(CompilationUnit* cu, int helper_offset, int arg0, int arg1, int arg2, bool safepoint_pc);
void CallRuntimeHelperImmMethodRegLocation(CompilationUnit* cu, int helper_offset, int arg0, RegLocation arg2, bool safepoint_pc);
void CallRuntimeHelperImmMethodImm(CompilationUnit* cu, int helper_offset, int arg0, int arg2, bool safepoint_pc);
void CallRuntimeHelperImmRegLocationRegLocation(CompilationUnit* cu, int helper_offset, int arg0, RegLocation arg1, RegLocation arg2, bool safepoint_pc);
void GenBarrier(CompilationUnit* cu);
LIR* OpUnconditionalBranch(CompilationUnit* cu, LIR* target);
LIR* GenCheck(CompilationUnit* cu, ConditionCode c_code, ThrowKind kind);
LIR* GenImmedCheck(CompilationUnit* cu, ConditionCode c_code, int reg, int imm_val, ThrowKind kind);
LIR* GenNullCheck(CompilationUnit* cu, int s_reg, int m_reg, int opt_flags);
LIR* GenRegRegCheck(CompilationUnit* cu, ConditionCode c_code, int reg1, int reg2, ThrowKind kind);
void GenCompareAndBranch(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_src1, RegLocation rl_src2, LIR* taken, LIR* fall_through);
void GenCompareZeroAndBranch(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_src, LIR* taken, LIR* fall_through);
void GenIntToLong(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src);
void GenIntNarrowing(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src);
void GenNewArray(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_dest, RegLocation rl_src);
void GenFilledNewArray(CompilationUnit* cu, CallInfo* info);
void GenSput(CompilationUnit* cu, uint32_t field_idx, RegLocation rl_src, bool is_long_or_double, bool is_object);
void GenSget(CompilationUnit* cu, uint32_t field_idx, RegLocation rl_dest, bool is_long_or_double, bool is_object);
void GenShowTarget(CompilationUnit* cu);
void HandleSuspendLaunchPads(CompilationUnit *cu);
void HandleIntrinsicLaunchPads(CompilationUnit *cu);
void HandleThrowLaunchPads(CompilationUnit *cu);
void SetupResourceMasks(CompilationUnit* cu, LIR* lir);
bool FastInstance(CompilationUnit* cu,  uint32_t field_idx, int& field_offset, bool& is_volatile, bool is_put);
void GenIGet(CompilationUnit* cu, uint32_t field_idx, int opt_flags, OpSize size, RegLocation rl_dest, RegLocation rl_obj, bool is_long_or_double, bool is_object);
void GenIPut(CompilationUnit* cu, uint32_t field_idx, int opt_flags, OpSize size, RegLocation rl_src, RegLocation rl_obj, bool is_long_or_double, bool is_object);
void GenConstClass(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_dest);
void GenConstString(CompilationUnit* cu, uint32_t string_idx, RegLocation rl_dest);
void GenNewInstance(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_dest);
void GenMoveException(CompilationUnit* cu, RegLocation rl_dest);
void GenThrow(CompilationUnit* cu, RegLocation rl_src);
void GenInstanceof(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_dest, RegLocation rl_src);
void GenCheckCast(CompilationUnit* cu, uint32_t type_idx, RegLocation rl_src);
void GenArrayObjPut(CompilationUnit* cu, int opt_flags, RegLocation rl_array, RegLocation rl_index, RegLocation rl_src, int scale);
void GenArrayGet(CompilationUnit* cu, int opt_flags, OpSize size, RegLocation rl_array, RegLocation rl_index, RegLocation rl_dest, int scale);
void GenArrayPut(CompilationUnit* cu, int opt_flags, OpSize size, RegLocation rl_array, RegLocation rl_index, RegLocation rl_src, int scale);
void GenLong3Addr(CompilationUnit* cu, OpKind first_op, OpKind second_op, RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);
bool GenShiftOpLong(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_shift);
bool GenArithOpInt(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);
bool GenArithOpIntLit(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src, int lit);
bool GenArithOpLong(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);
bool GenConversionCall(CompilationUnit* cu, int func_offset, RegLocation rl_dest, RegLocation rl_src);
bool GenArithOpFloatPortable(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);
bool GenArithOpDoublePortable(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);
bool GenConversionPortable(CompilationUnit* cu, Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src);
void GenSuspendTest(CompilationUnit* cu, int opt_flags);
void GenSuspendTestAndBranch(CompilationUnit* cu, int opt_flags, LIR* target);

#endif // ART_SRC_COMPILER_CODEGEN_GENCOMMON_H_
