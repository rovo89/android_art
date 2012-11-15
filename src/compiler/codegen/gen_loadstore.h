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

LIR* loadConstant(CompilationUnit* cUnit, int rDest, int value);
LIR* loadWordDisp(CompilationUnit* cUnit, int rBase, int displacement, int rDest);
LIR* storeWordDisp(CompilationUnit* cUnit, int rBase, int displacement, int rSrc);
void loadValueDirect(CompilationUnit* cUnit, RegLocation rlSrc, int rDest);
void loadValueDirectFixed(CompilationUnit* cUnit, RegLocation rlSrc, int rDest);
void loadValueDirectWide(CompilationUnit* cUnit, RegLocation rlSrc, int regLo, int regHi);
void loadValueDirectWideFixed(CompilationUnit* cUnit, RegLocation rlSrc, int regLo, int regHi);
RegLocation loadValue(CompilationUnit* cUnit, RegLocation rlSrc, RegisterClass opKind);
void storeValue(CompilationUnit* cUnit, RegLocation rlDest, RegLocation rlSrc);
RegLocation loadValueWide(CompilationUnit* cUnit, RegLocation rlSrc, RegisterClass opKind);
void storeValueWide(CompilationUnit* cUnit, RegLocation rlDest, RegLocation rlSrc);
void loadCurrMethodDirect(CompilationUnit *cUnit, int rTgt);
RegLocation loadCurrMethod(CompilationUnit *cUnit);
bool methodStarInReg(CompilationUnit* cUnit);

#endif // ART_SRC_COMPILER_CODEGEN_GENLOADSTORE_H_
