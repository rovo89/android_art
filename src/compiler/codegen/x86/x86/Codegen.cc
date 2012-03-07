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

#define _CODEGEN_C
#define TARGET_X86

#include "../../../Dalvik.h"
#include "../../../CompilerInternals.h"
#include "../X86LIR.h"
#include "../../Ralloc.h"
#include "../Codegen.h"

/* X86 codegen building blocks */
#include "../../CodegenUtil.cc"

/* X86-specific factory utilities */
#include "../X86/Factory.cc"
/* Target independent factory utilities */
#include "../../CodegenFactory.cc"
/* X86-specific codegen routines */
#include "../X86/Gen.cc"
/* FP codegen routines */
#include "../FP/X86FP.cc"
/* Target independent gen routines */
#include "../../GenCommon.cc"
/* Shared invoke gen routines */
#include "../GenInvoke.cc"
/* X86-specific factory utilities */
#include "../ArchFactory.cc"

/* X86-specific register allocation */
#include "../X86/Ralloc.cc"

/* MIR2LIR dispatcher and architectural independent codegen routines */
#include "../../MethodCodegenDriver.cc"

/* Target-independent local optimizations */
#include "../../LocalOptimizations.cc"

/* Architecture manifest */
#include "ArchVariant.cc"
