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

#ifndef ART_SRC_COMPILER_CODEGEN_GENLOADSTORE_H_
#define ART_SRC_COMPILER_CODEGEN_GENLOADSTORE_H_

LIR* LoadConstant(CompilationUnit* cu, int r_dest, int value);
LIR* LoadWordDisp(CompilationUnit* cu, int rBase, int displacement, int r_dest);
LIR* StoreWordDisp(CompilationUnit* cu, int rBase, int displacement, int r_src);
void LoadValueDirect(CompilationUnit* cu, RegLocation rl_src, int r_dest);
void LoadValueDirectFixed(CompilationUnit* cu, RegLocation rl_src, int r_dest);
void LoadValueDirectWide(CompilationUnit* cu, RegLocation rl_src, int reg_lo, int reg_hi);
void LoadValueDirectWideFixed(CompilationUnit* cu, RegLocation rl_src, int reg_lo, int reg_hi);
RegLocation LoadValue(CompilationUnit* cu, RegLocation rl_src, RegisterClass op_kind);
void StoreValue(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src);
RegLocation LoadValueWide(CompilationUnit* cu, RegLocation rl_src, RegisterClass op_kind);
void StoreValueWide(CompilationUnit* cu, RegLocation rl_dest, RegLocation rl_src);
void LoadCurrMethodDirect(CompilationUnit *cu, int r_tgt);
RegLocation LoadCurrMethod(CompilationUnit *cu);
bool MethodStarInReg(CompilationUnit* cu);

#endif // ART_SRC_COMPILER_CODEGEN_GENLOADSTORE_H_
