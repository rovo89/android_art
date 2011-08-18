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

#ifndef ART_SRC_COMPILER_COMPILER_OPTIMIZATION_H_
#define ART_SRC_COMPILER_COMPILER_OPTIMIZATION_H_

#include "../Dalvik.h"

#define STACK_ALIGN_WORDS 4
#define STACK_ALIGNMENT (STACK_ALIGN_WORDS * 4)

/* Supress optimization if corresponding bit set */
enum optControlVector {
    kLoadStoreElimination = 0,
    kLoadHoisting,
    kTrackLiveTemps,
    kSuppressLoads,
    kPromoteRegs,
};

/* Forward declarations */
struct CompilationUnit;
struct LIR;

void oatApplyLocalOptimizations(struct CompilationUnit* cUnit,
                                struct LIR* head, struct LIR* tail);

#endif  // ART_SRC_COMPILER_COMPILER_OPTIMIZATION_H_
