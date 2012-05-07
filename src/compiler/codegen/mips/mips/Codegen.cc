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
#define TARGET_MIPS

#include "../../../Dalvik.h"
#include "../../../CompilerInternals.h"
#include "../MipsLIR.h"
#include "../../Ralloc.h"
#include "../Codegen.h"

/* Mips codegen building blocks */
#include "../../CodegenUtil.cc"

/* Mips-specific factory utilities */
#include "../Mips32/Factory.cc"
/* Target independent factory utilities */
#include "../../CodegenFactory.cc"
/* Target independent gen routines */
#include "../../GenCommon.cc"
/* Shared invoke gen routines */
#include "../../GenInvoke.cc"
/* Mips-specific factory utilities */
#include "../ArchFactory.cc"

/* Mips32-specific codegen routines */
#include "../Mips32/Gen.cc"
/* FP codegen routines */
#include "../FP/MipsFP.cc"

/* Mips32-specific register allocation */
#include "../Mips32/Ralloc.cc"

/* Bitcode conversion */
#include "../../MethodBitcode.cc"

/* MIR2LIR dispatcher and architectural independent codegen routines */
#include "../../MethodCodegenDriver.cc"

/* Target-independent local optimizations */
#include "../../LocalOptimizations.cc"

/* Architecture manifest */
#include "ArchVariant.cc"
