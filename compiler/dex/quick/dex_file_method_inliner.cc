/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include "dex_file_method_inliner.h"

#include <algorithm>

#include "base/logging.h"
#include "base/macros.h"
#include "base/mutex-inl.h"
#include "dex/compiler_ir.h"
#include "thread-inl.h"
#include "dex/mir_graph.h"
#include "dex/quick/mir_to_lir.h"
#include "dex_instruction-inl.h"
#include "driver/dex_compilation_unit.h"
#include "verifier/method_verifier-inl.h"

namespace art {

namespace {  // anonymous namespace

static constexpr bool kIntrinsicIsStatic[] = {
    true,   // kIntrinsicDoubleCvt
    true,   // kIntrinsicFloatCvt
    true,   // kIntrinsicReverseBits
    true,   // kIntrinsicReverseBytes
    true,   // kIntrinsicAbsInt
    true,   // kIntrinsicAbsLong
    true,   // kIntrinsicAbsFloat
    true,   // kIntrinsicAbsDouble
    true,   // kIntrinsicMinMaxInt
    true,   // kIntrinsicMinMaxLong
    true,   // kIntrinsicMinMaxFloat
    true,   // kIntrinsicMinMaxDouble
    true,   // kIntrinsicSqrt
    true,   // kIntrinsicCeil
    true,   // kIntrinsicFloor
    true,   // kIntrinsicRint
    true,   // kIntrinsicRoundFloat
    true,   // kIntrinsicRoundDouble
    false,  // kIntrinsicReferenceGetReferent
    false,  // kIntrinsicCharAt
    false,  // kIntrinsicCompareTo
    false,  // kIntrinsicGetCharsNoCheck
    false,  // kIntrinsicIsEmptyOrLength
    false,  // kIntrinsicIndexOf
    true,   // kIntrinsicNewStringFromBytes
    true,   // kIntrinsicNewStringFromChars
    true,   // kIntrinsicNewStringFromString
    true,   // kIntrinsicCurrentThread
    true,   // kIntrinsicPeek
    true,   // kIntrinsicPoke
    false,  // kIntrinsicCas
    false,  // kIntrinsicUnsafeGet
    false,  // kIntrinsicUnsafePut
    true,   // kIntrinsicSystemArrayCopyCharArray
};
static_assert(arraysize(kIntrinsicIsStatic) == kInlineOpNop,
              "arraysize of kIntrinsicIsStatic unexpected");
static_assert(kIntrinsicIsStatic[kIntrinsicDoubleCvt], "DoubleCvt must be static");
static_assert(kIntrinsicIsStatic[kIntrinsicFloatCvt], "FloatCvt must be static");
static_assert(kIntrinsicIsStatic[kIntrinsicReverseBits], "ReverseBits must be static");
static_assert(kIntrinsicIsStatic[kIntrinsicReverseBytes], "ReverseBytes must be static");
static_assert(kIntrinsicIsStatic[kIntrinsicAbsInt], "AbsInt must be static");
static_assert(kIntrinsicIsStatic[kIntrinsicAbsLong], "AbsLong must be static");
static_assert(kIntrinsicIsStatic[kIntrinsicAbsFloat], "AbsFloat must be static");
static_assert(kIntrinsicIsStatic[kIntrinsicAbsDouble], "AbsDouble must be static");
static_assert(kIntrinsicIsStatic[kIntrinsicMinMaxInt], "MinMaxInt must be static");
static_assert(kIntrinsicIsStatic[kIntrinsicMinMaxLong], "MinMaxLong_must_be_static");
static_assert(kIntrinsicIsStatic[kIntrinsicMinMaxFloat], "MinMaxFloat_must_be_static");
static_assert(kIntrinsicIsStatic[kIntrinsicMinMaxDouble], "MinMaxDouble_must_be_static");
static_assert(kIntrinsicIsStatic[kIntrinsicSqrt], "Sqrt must be static");
static_assert(kIntrinsicIsStatic[kIntrinsicCeil], "Ceil must be static");
static_assert(kIntrinsicIsStatic[kIntrinsicFloor], "Floor must be static");
static_assert(kIntrinsicIsStatic[kIntrinsicRint], "Rint must be static");
static_assert(kIntrinsicIsStatic[kIntrinsicRoundFloat], "RoundFloat must be static");
static_assert(kIntrinsicIsStatic[kIntrinsicRoundDouble], "RoundDouble must be static");
static_assert(!kIntrinsicIsStatic[kIntrinsicReferenceGetReferent], "Get must not be static");
static_assert(!kIntrinsicIsStatic[kIntrinsicCharAt], "CharAt must not be static");
static_assert(!kIntrinsicIsStatic[kIntrinsicCompareTo], "CompareTo must not be static");
static_assert(!kIntrinsicIsStatic[kIntrinsicGetCharsNoCheck], "GetCharsNoCheck must not be static");
static_assert(!kIntrinsicIsStatic[kIntrinsicIsEmptyOrLength], "IsEmptyOrLength must not be static");
static_assert(!kIntrinsicIsStatic[kIntrinsicIndexOf], "IndexOf must not be static");
static_assert(kIntrinsicIsStatic[kIntrinsicNewStringFromBytes],
              "NewStringFromBytes must be static");
static_assert(kIntrinsicIsStatic[kIntrinsicNewStringFromChars],
              "NewStringFromChars must be static");
static_assert(kIntrinsicIsStatic[kIntrinsicNewStringFromString],
              "NewStringFromString must be static");
static_assert(kIntrinsicIsStatic[kIntrinsicCurrentThread], "CurrentThread must be static");
static_assert(kIntrinsicIsStatic[kIntrinsicPeek], "Peek must be static");
static_assert(kIntrinsicIsStatic[kIntrinsicPoke], "Poke must be static");
static_assert(!kIntrinsicIsStatic[kIntrinsicCas], "Cas must not be static");
static_assert(!kIntrinsicIsStatic[kIntrinsicUnsafeGet], "UnsafeGet_must_not_be_static");
static_assert(!kIntrinsicIsStatic[kIntrinsicUnsafePut], "UnsafePut must not be static");
static_assert(kIntrinsicIsStatic[kIntrinsicSystemArrayCopyCharArray],
              "SystemArrayCopyCharArray must be static");

MIR* AllocReplacementMIR(MIRGraph* mir_graph, MIR* invoke) {
  MIR* insn = mir_graph->NewMIR();
  insn->offset = invoke->offset;
  insn->optimization_flags = MIR_CALLEE;
  return insn;
}

uint32_t GetInvokeReg(MIR* invoke, uint32_t arg) {
  DCHECK_LT(arg, invoke->dalvikInsn.vA);
  DCHECK(!MIR::DecodedInstruction::IsPseudoMirOp(invoke->dalvikInsn.opcode));
  if (IsInvokeInstructionRange(invoke->dalvikInsn.opcode)) {
    return invoke->dalvikInsn.vC + arg;  // Range invoke.
  } else {
    DCHECK_EQ(Instruction::FormatOf(invoke->dalvikInsn.opcode), Instruction::k35c);
    return invoke->dalvikInsn.arg[arg];  // Non-range invoke.
  }
}

bool WideArgIsInConsecutiveDalvikRegs(MIR* invoke, uint32_t arg) {
  DCHECK_LT(arg + 1, invoke->dalvikInsn.vA);
  DCHECK(!MIR::DecodedInstruction::IsPseudoMirOp(invoke->dalvikInsn.opcode));
  return IsInvokeInstructionRange(invoke->dalvikInsn.opcode) ||
      invoke->dalvikInsn.arg[arg + 1u] == invoke->dalvikInsn.arg[arg] + 1u;
}

}  // anonymous namespace

const uint32_t DexFileMethodInliner::kIndexUnresolved;
const char* const DexFileMethodInliner::kClassCacheNames[] = {
    "Z",                       // kClassCacheBoolean
    "B",                       // kClassCacheByte
    "C",                       // kClassCacheChar
    "S",                       // kClassCacheShort
    "I",                       // kClassCacheInt
    "J",                       // kClassCacheLong
    "F",                       // kClassCacheFloat
    "D",                       // kClassCacheDouble
    "V",                       // kClassCacheVoid
    "[B",                      // kClassCacheJavaLangByteArray
    "[C",                      // kClassCacheJavaLangCharArray
    "[I",                      // kClassCacheJavaLangIntArray
    "Ljava/lang/Object;",      // kClassCacheJavaLangObject
    "Ljava/lang/ref/Reference;",   // kClassCacheJavaLangRefReference
    "Ljava/lang/String;",      // kClassCacheJavaLangString
    "Ljava/lang/StringBuffer;",    // kClassCacheJavaLangStringBuffer
    "Ljava/lang/StringBuilder;",   // kClassCacheJavaLangStringBuilder
    "Ljava/lang/StringFactory;",   // kClassCacheJavaLangStringFactory
    "Ljava/lang/Double;",      // kClassCacheJavaLangDouble
    "Ljava/lang/Float;",       // kClassCacheJavaLangFloat
    "Ljava/lang/Integer;",     // kClassCacheJavaLangInteger
    "Ljava/lang/Long;",        // kClassCacheJavaLangLong
    "Ljava/lang/Short;",       // kClassCacheJavaLangShort
    "Ljava/lang/Math;",        // kClassCacheJavaLangMath
    "Ljava/lang/StrictMath;",  // kClassCacheJavaLangStrictMath
    "Ljava/lang/Thread;",      // kClassCacheJavaLangThread
    "Ljava/nio/charset/Charset;",  // kClassCacheJavaNioCharsetCharset
    "Llibcore/io/Memory;",     // kClassCacheLibcoreIoMemory
    "Lsun/misc/Unsafe;",       // kClassCacheSunMiscUnsafe
    "Ljava/lang/System;",      // kClassCacheJavaLangSystem
};

const char* const DexFileMethodInliner::kNameCacheNames[] = {
    "reverse",               // kNameCacheReverse
    "reverseBytes",          // kNameCacheReverseBytes
    "doubleToRawLongBits",   // kNameCacheDoubleToRawLongBits
    "longBitsToDouble",      // kNameCacheLongBitsToDouble
    "floatToRawIntBits",     // kNameCacheFloatToRawIntBits
    "intBitsToFloat",        // kNameCacheIntBitsToFloat
    "abs",                   // kNameCacheAbs
    "max",                   // kNameCacheMax
    "min",                   // kNameCacheMin
    "sqrt",                  // kNameCacheSqrt
    "ceil",                  // kNameCacheCeil
    "floor",                 // kNameCacheFloor
    "rint",                  // kNameCacheRint
    "round",                 // kNameCacheRound
    "getReferent",           // kNameCacheReferenceGet
    "charAt",                // kNameCacheCharAt
    "compareTo",             // kNameCacheCompareTo
    "getCharsNoCheck",       // kNameCacheGetCharsNoCheck
    "isEmpty",               // kNameCacheIsEmpty
    "indexOf",               // kNameCacheIndexOf
    "length",                // kNameCacheLength
    "<init>",                // kNameCacheInit
    "newStringFromBytes",    // kNameCacheNewStringFromBytes
    "newStringFromChars",    // kNameCacheNewStringFromChars
    "newStringFromString",   // kNameCacheNewStringFromString
    "currentThread",         // kNameCacheCurrentThread
    "peekByte",              // kNameCachePeekByte
    "peekIntNative",         // kNameCachePeekIntNative
    "peekLongNative",        // kNameCachePeekLongNative
    "peekShortNative",       // kNameCachePeekShortNative
    "pokeByte",              // kNameCachePokeByte
    "pokeIntNative",         // kNameCachePokeIntNative
    "pokeLongNative",        // kNameCachePokeLongNative
    "pokeShortNative",       // kNameCachePokeShortNative
    "compareAndSwapInt",     // kNameCacheCompareAndSwapInt
    "compareAndSwapLong",    // kNameCacheCompareAndSwapLong
    "compareAndSwapObject",  // kNameCacheCompareAndSwapObject
    "getInt",                // kNameCacheGetInt
    "getIntVolatile",        // kNameCacheGetIntVolatile
    "putInt",                // kNameCachePutInt
    "putIntVolatile",        // kNameCachePutIntVolatile
    "putOrderedInt",         // kNameCachePutOrderedInt
    "getLong",               // kNameCacheGetLong
    "getLongVolatile",       // kNameCacheGetLongVolatile
    "putLong",               // kNameCachePutLong
    "putLongVolatile",       // kNameCachePutLongVolatile
    "putOrderedLong",        // kNameCachePutOrderedLong
    "getObject",             // kNameCacheGetObject
    "getObjectVolatile",     // kNameCacheGetObjectVolatile
    "putObject",             // kNameCachePutObject
    "putObjectVolatile",     // kNameCachePutObjectVolatile
    "putOrderedObject",      // kNameCachePutOrderedObject
    "arraycopy",             // kNameCacheArrayCopy
};

const DexFileMethodInliner::ProtoDef DexFileMethodInliner::kProtoCacheDefs[] = {
    // kProtoCacheI_I
    { kClassCacheInt, 1, { kClassCacheInt } },
    // kProtoCacheJ_J
    { kClassCacheLong, 1, { kClassCacheLong } },
    // kProtoCacheS_S
    { kClassCacheShort, 1, { kClassCacheShort } },
    // kProtoCacheD_D
    { kClassCacheDouble, 1, { kClassCacheDouble } },
    // kProtoCacheDD_D
    { kClassCacheDouble, 2, { kClassCacheDouble, kClassCacheDouble } },
    // kProtoCacheF_F
    { kClassCacheFloat, 1, { kClassCacheFloat } },
    // kProtoCacheFF_F
    { kClassCacheFloat, 2, { kClassCacheFloat, kClassCacheFloat } },
    // kProtoCacheD_J
    { kClassCacheLong, 1, { kClassCacheDouble } },
    // kProtoCacheJ_D
    { kClassCacheDouble, 1, { kClassCacheLong } },
    // kProtoCacheF_I
    { kClassCacheInt, 1, { kClassCacheFloat } },
    // kProtoCacheI_F
    { kClassCacheFloat, 1, { kClassCacheInt } },
    // kProtoCacheII_I
    { kClassCacheInt, 2, { kClassCacheInt, kClassCacheInt } },
    // kProtoCacheI_C
    { kClassCacheChar, 1, { kClassCacheInt } },
    // kProtoCacheString_I
    { kClassCacheInt, 1, { kClassCacheJavaLangString } },
    // kProtoCache_Z
    { kClassCacheBoolean, 0, { } },
    // kProtoCache_I
    { kClassCacheInt, 0, { } },
    // kProtoCache_Object
    { kClassCacheJavaLangObject, 0, { } },
    // kProtoCache_Thread
    { kClassCacheJavaLangThread, 0, { } },
    // kProtoCacheJ_B
    { kClassCacheByte, 1, { kClassCacheLong } },
    // kProtoCacheJ_I
    { kClassCacheInt, 1, { kClassCacheLong } },
    // kProtoCacheJ_S
    { kClassCacheShort, 1, { kClassCacheLong } },
    // kProtoCacheJB_V
    { kClassCacheVoid, 2, { kClassCacheLong, kClassCacheByte } },
    // kProtoCacheJI_V
    { kClassCacheVoid, 2, { kClassCacheLong, kClassCacheInt } },
    // kProtoCacheJJ_J
    { kClassCacheLong, 2, { kClassCacheLong, kClassCacheLong } },
    // kProtoCacheJJ_V
    { kClassCacheVoid, 2, { kClassCacheLong, kClassCacheLong } },
    // kProtoCacheJS_V
    { kClassCacheVoid, 2, { kClassCacheLong, kClassCacheShort } },
    // kProtoCacheObjectJII_Z
    { kClassCacheBoolean, 4, { kClassCacheJavaLangObject, kClassCacheLong,
        kClassCacheInt, kClassCacheInt } },
    // kProtoCacheObjectJJJ_Z
    { kClassCacheBoolean, 4, { kClassCacheJavaLangObject, kClassCacheLong,
        kClassCacheLong, kClassCacheLong } },
    // kProtoCacheObjectJObjectObject_Z
    { kClassCacheBoolean, 4, { kClassCacheJavaLangObject, kClassCacheLong,
        kClassCacheJavaLangObject, kClassCacheJavaLangObject } },
    // kProtoCacheObjectJ_I
    { kClassCacheInt, 2, { kClassCacheJavaLangObject, kClassCacheLong } },
    // kProtoCacheObjectJI_V
    { kClassCacheVoid, 3, { kClassCacheJavaLangObject, kClassCacheLong, kClassCacheInt } },
    // kProtoCacheObjectJ_J
    { kClassCacheLong, 2, { kClassCacheJavaLangObject, kClassCacheLong } },
    // kProtoCacheObjectJJ_V
    { kClassCacheVoid, 3, { kClassCacheJavaLangObject, kClassCacheLong, kClassCacheLong } },
    // kProtoCacheObjectJ_Object
    { kClassCacheJavaLangObject, 2, { kClassCacheJavaLangObject, kClassCacheLong } },
    // kProtoCacheObjectJObject_V
    { kClassCacheVoid, 3, { kClassCacheJavaLangObject, kClassCacheLong,
        kClassCacheJavaLangObject } },
    // kProtoCacheCharArrayICharArrayII_V
    { kClassCacheVoid, 5, {kClassCacheJavaLangCharArray, kClassCacheInt,
        kClassCacheJavaLangCharArray, kClassCacheInt, kClassCacheInt} },
    // kProtoCacheIICharArrayI_V
    { kClassCacheVoid, 4, { kClassCacheInt, kClassCacheInt, kClassCacheJavaLangCharArray,
        kClassCacheInt } },
    // kProtoCacheByteArrayIII_String
    { kClassCacheJavaLangString, 4, { kClassCacheJavaLangByteArray, kClassCacheInt, kClassCacheInt,
        kClassCacheInt } },
    // kProtoCacheIICharArray_String
    { kClassCacheJavaLangString, 3, { kClassCacheInt, kClassCacheInt,
        kClassCacheJavaLangCharArray } },
    // kProtoCacheString_String
    { kClassCacheJavaLangString, 1, { kClassCacheJavaLangString } },
    // kProtoCache_V
    { kClassCacheVoid, 0, { } },
    // kProtoCacheByteArray_V
    { kClassCacheVoid, 1, { kClassCacheJavaLangByteArray } },
    // kProtoCacheByteArrayI_V
    { kClassCacheVoid, 2, { kClassCacheJavaLangByteArray, kClassCacheInt } },
    // kProtoCacheByteArrayII_V
    { kClassCacheVoid, 3, { kClassCacheJavaLangByteArray, kClassCacheInt, kClassCacheInt } },
    // kProtoCacheByteArrayIII_V
    { kClassCacheVoid, 4, { kClassCacheJavaLangByteArray, kClassCacheInt, kClassCacheInt,
        kClassCacheInt } },
    // kProtoCacheByteArrayIIString_V
    { kClassCacheVoid, 4, { kClassCacheJavaLangByteArray, kClassCacheInt, kClassCacheInt,
        kClassCacheJavaLangString } },
    // kProtoCacheByteArrayString_V
    { kClassCacheVoid, 2, { kClassCacheJavaLangByteArray, kClassCacheJavaLangString } },
    // kProtoCacheByteArrayIICharset_V
    { kClassCacheVoid, 4, { kClassCacheJavaLangByteArray, kClassCacheInt, kClassCacheInt,
        kClassCacheJavaNioCharsetCharset } },
    // kProtoCacheByteArrayCharset_V
    { kClassCacheVoid, 2, { kClassCacheJavaLangByteArray, kClassCacheJavaNioCharsetCharset } },
    // kProtoCacheCharArray_V
    { kClassCacheVoid, 1, { kClassCacheJavaLangCharArray } },
    // kProtoCacheCharArrayII_V
    { kClassCacheVoid, 3, { kClassCacheJavaLangCharArray, kClassCacheInt, kClassCacheInt } },
    // kProtoCacheIICharArray_V
    { kClassCacheVoid, 3, { kClassCacheInt, kClassCacheInt, kClassCacheJavaLangCharArray } },
    // kProtoCacheIntArrayII_V
    { kClassCacheVoid, 3, { kClassCacheJavaLangIntArray, kClassCacheInt, kClassCacheInt } },
    // kProtoCacheString_V
    { kClassCacheVoid, 1, { kClassCacheJavaLangString } },
    // kProtoCacheStringBuffer_V
    { kClassCacheVoid, 1, { kClassCacheJavaLangStringBuffer } },
    // kProtoCacheStringBuilder_V
    { kClassCacheVoid, 1, { kClassCacheJavaLangStringBuilder } },
};

const DexFileMethodInliner::IntrinsicDef DexFileMethodInliner::kIntrinsicMethods[] = {
#define INTRINSIC(c, n, p, o, d) \
    { { kClassCache ## c, kNameCache ## n, kProtoCache ## p }, { o, kInlineIntrinsic, { d } } }

    INTRINSIC(JavaLangDouble, DoubleToRawLongBits, D_J, kIntrinsicDoubleCvt, 0),
    INTRINSIC(JavaLangDouble, LongBitsToDouble, J_D, kIntrinsicDoubleCvt, kIntrinsicFlagToFloatingPoint),
    INTRINSIC(JavaLangFloat, FloatToRawIntBits, F_I, kIntrinsicFloatCvt, 0),
    INTRINSIC(JavaLangFloat, IntBitsToFloat, I_F, kIntrinsicFloatCvt, kIntrinsicFlagToFloatingPoint),

    INTRINSIC(JavaLangInteger, ReverseBytes, I_I, kIntrinsicReverseBytes, k32),
    INTRINSIC(JavaLangLong, ReverseBytes, J_J, kIntrinsicReverseBytes, k64),
    INTRINSIC(JavaLangShort, ReverseBytes, S_S, kIntrinsicReverseBytes, kSignedHalf),
    INTRINSIC(JavaLangInteger, Reverse, I_I, kIntrinsicReverseBits, k32),
    INTRINSIC(JavaLangLong, Reverse, J_J, kIntrinsicReverseBits, k64),

    INTRINSIC(JavaLangMath,       Abs, I_I, kIntrinsicAbsInt, 0),
    INTRINSIC(JavaLangStrictMath, Abs, I_I, kIntrinsicAbsInt, 0),
    INTRINSIC(JavaLangMath,       Abs, J_J, kIntrinsicAbsLong, 0),
    INTRINSIC(JavaLangStrictMath, Abs, J_J, kIntrinsicAbsLong, 0),
    INTRINSIC(JavaLangMath,       Abs, F_F, kIntrinsicAbsFloat, 0),
    INTRINSIC(JavaLangStrictMath, Abs, F_F, kIntrinsicAbsFloat, 0),
    INTRINSIC(JavaLangMath,       Abs, D_D, kIntrinsicAbsDouble, 0),
    INTRINSIC(JavaLangStrictMath, Abs, D_D, kIntrinsicAbsDouble, 0),
    INTRINSIC(JavaLangMath,       Min, II_I, kIntrinsicMinMaxInt, kIntrinsicFlagMin),
    INTRINSIC(JavaLangStrictMath, Min, II_I, kIntrinsicMinMaxInt, kIntrinsicFlagMin),
    INTRINSIC(JavaLangMath,       Max, II_I, kIntrinsicMinMaxInt, kIntrinsicFlagMax),
    INTRINSIC(JavaLangStrictMath, Max, II_I, kIntrinsicMinMaxInt, kIntrinsicFlagMax),
    INTRINSIC(JavaLangMath,       Min, JJ_J, kIntrinsicMinMaxLong, kIntrinsicFlagMin),
    INTRINSIC(JavaLangStrictMath, Min, JJ_J, kIntrinsicMinMaxLong, kIntrinsicFlagMin),
    INTRINSIC(JavaLangMath,       Max, JJ_J, kIntrinsicMinMaxLong, kIntrinsicFlagMax),
    INTRINSIC(JavaLangStrictMath, Max, JJ_J, kIntrinsicMinMaxLong, kIntrinsicFlagMax),
    INTRINSIC(JavaLangMath,       Min, FF_F, kIntrinsicMinMaxFloat, kIntrinsicFlagMin),
    INTRINSIC(JavaLangStrictMath, Min, FF_F, kIntrinsicMinMaxFloat, kIntrinsicFlagMin),
    INTRINSIC(JavaLangMath,       Max, FF_F, kIntrinsicMinMaxFloat, kIntrinsicFlagMax),
    INTRINSIC(JavaLangStrictMath, Max, FF_F, kIntrinsicMinMaxFloat, kIntrinsicFlagMax),
    INTRINSIC(JavaLangMath,       Min, DD_D, kIntrinsicMinMaxDouble, kIntrinsicFlagMin),
    INTRINSIC(JavaLangStrictMath, Min, DD_D, kIntrinsicMinMaxDouble, kIntrinsicFlagMin),
    INTRINSIC(JavaLangMath,       Max, DD_D, kIntrinsicMinMaxDouble, kIntrinsicFlagMax),
    INTRINSIC(JavaLangStrictMath, Max, DD_D, kIntrinsicMinMaxDouble, kIntrinsicFlagMax),

    INTRINSIC(JavaLangMath,       Sqrt, D_D, kIntrinsicSqrt, 0),
    INTRINSIC(JavaLangStrictMath, Sqrt, D_D, kIntrinsicSqrt, 0),

    INTRINSIC(JavaLangMath,       Ceil, D_D, kIntrinsicCeil, 0),
    INTRINSIC(JavaLangStrictMath, Ceil, D_D, kIntrinsicCeil, 0),
    INTRINSIC(JavaLangMath,       Floor, D_D, kIntrinsicFloor, 0),
    INTRINSIC(JavaLangStrictMath, Floor, D_D, kIntrinsicFloor, 0),
    INTRINSIC(JavaLangMath,       Rint, D_D, kIntrinsicRint, 0),
    INTRINSIC(JavaLangStrictMath, Rint, D_D, kIntrinsicRint, 0),
    INTRINSIC(JavaLangMath,       Round, F_I, kIntrinsicRoundFloat, 0),
    INTRINSIC(JavaLangStrictMath, Round, F_I, kIntrinsicRoundFloat, 0),
    INTRINSIC(JavaLangMath,       Round, D_J, kIntrinsicRoundDouble, 0),
    INTRINSIC(JavaLangStrictMath, Round, D_J, kIntrinsicRoundDouble, 0),

    INTRINSIC(JavaLangRefReference, ReferenceGetReferent, _Object, kIntrinsicReferenceGetReferent, 0),

    INTRINSIC(JavaLangString, CharAt, I_C, kIntrinsicCharAt, 0),
    INTRINSIC(JavaLangString, CompareTo, String_I, kIntrinsicCompareTo, 0),
    INTRINSIC(JavaLangString, GetCharsNoCheck, IICharArrayI_V, kIntrinsicGetCharsNoCheck, 0),
    INTRINSIC(JavaLangString, IsEmpty, _Z, kIntrinsicIsEmptyOrLength, kIntrinsicFlagIsEmpty),
    INTRINSIC(JavaLangString, IndexOf, II_I, kIntrinsicIndexOf, kIntrinsicFlagNone),
    INTRINSIC(JavaLangString, IndexOf, I_I, kIntrinsicIndexOf, kIntrinsicFlagBase0),
    INTRINSIC(JavaLangString, Length, _I, kIntrinsicIsEmptyOrLength, kIntrinsicFlagLength),

    INTRINSIC(JavaLangThread, CurrentThread, _Thread, kIntrinsicCurrentThread, 0),

    INTRINSIC(LibcoreIoMemory, PeekByte, J_B, kIntrinsicPeek, kSignedByte),
    INTRINSIC(LibcoreIoMemory, PeekIntNative, J_I, kIntrinsicPeek, k32),
    INTRINSIC(LibcoreIoMemory, PeekLongNative, J_J, kIntrinsicPeek, k64),
    INTRINSIC(LibcoreIoMemory, PeekShortNative, J_S, kIntrinsicPeek, kSignedHalf),
    INTRINSIC(LibcoreIoMemory, PokeByte, JB_V, kIntrinsicPoke, kSignedByte),
    INTRINSIC(LibcoreIoMemory, PokeIntNative, JI_V, kIntrinsicPoke, k32),
    INTRINSIC(LibcoreIoMemory, PokeLongNative, JJ_V, kIntrinsicPoke, k64),
    INTRINSIC(LibcoreIoMemory, PokeShortNative, JS_V, kIntrinsicPoke, kSignedHalf),

    INTRINSIC(SunMiscUnsafe, CompareAndSwapInt, ObjectJII_Z, kIntrinsicCas,
              kIntrinsicFlagNone),
    INTRINSIC(SunMiscUnsafe, CompareAndSwapLong, ObjectJJJ_Z, kIntrinsicCas,
              kIntrinsicFlagIsLong),
    INTRINSIC(SunMiscUnsafe, CompareAndSwapObject, ObjectJObjectObject_Z, kIntrinsicCas,
              kIntrinsicFlagIsObject),

#define UNSAFE_GET_PUT(type, code, type_flags) \
    INTRINSIC(SunMiscUnsafe, Get ## type, ObjectJ_ ## code, kIntrinsicUnsafeGet, \
              type_flags), \
    INTRINSIC(SunMiscUnsafe, Get ## type ## Volatile, ObjectJ_ ## code, kIntrinsicUnsafeGet, \
              type_flags | kIntrinsicFlagIsVolatile), \
    INTRINSIC(SunMiscUnsafe, Put ## type, ObjectJ ## code ## _V, kIntrinsicUnsafePut, \
              type_flags), \
    INTRINSIC(SunMiscUnsafe, Put ## type ## Volatile, ObjectJ ## code ## _V, kIntrinsicUnsafePut, \
              type_flags | kIntrinsicFlagIsVolatile), \
    INTRINSIC(SunMiscUnsafe, PutOrdered ## type, ObjectJ ## code ## _V, kIntrinsicUnsafePut, \
              type_flags | kIntrinsicFlagIsOrdered)

    UNSAFE_GET_PUT(Int, I, kIntrinsicFlagNone),
    UNSAFE_GET_PUT(Long, J, kIntrinsicFlagIsLong),
    UNSAFE_GET_PUT(Object, Object, kIntrinsicFlagIsObject),
#undef UNSAFE_GET_PUT

    INTRINSIC(JavaLangSystem, ArrayCopy, CharArrayICharArrayII_V , kIntrinsicSystemArrayCopyCharArray,
              0),

#undef INTRINSIC

#define SPECIAL(c, n, p, o, d) \
    { { kClassCache ## c, kNameCache ## n, kProtoCache ## p }, { o, kInlineSpecial, { d } } }

    SPECIAL(JavaLangString, Init, _V, kInlineStringInit, 0),
    SPECIAL(JavaLangString, Init, ByteArray_V, kInlineStringInit, 1),
    SPECIAL(JavaLangString, Init, ByteArrayI_V, kInlineStringInit, 2),
    SPECIAL(JavaLangString, Init, ByteArrayII_V, kInlineStringInit, 3),
    SPECIAL(JavaLangString, Init, ByteArrayIII_V, kInlineStringInit, 4),
    SPECIAL(JavaLangString, Init, ByteArrayIIString_V, kInlineStringInit, 5),
    SPECIAL(JavaLangString, Init, ByteArrayString_V, kInlineStringInit, 6),
    SPECIAL(JavaLangString, Init, ByteArrayIICharset_V, kInlineStringInit, 7),
    SPECIAL(JavaLangString, Init, ByteArrayCharset_V, kInlineStringInit, 8),
    SPECIAL(JavaLangString, Init, CharArray_V, kInlineStringInit, 9),
    SPECIAL(JavaLangString, Init, CharArrayII_V, kInlineStringInit, 10),
    SPECIAL(JavaLangString, Init, IICharArray_V, kInlineStringInit, 11),
    SPECIAL(JavaLangString, Init, IntArrayII_V, kInlineStringInit, 12),
    SPECIAL(JavaLangString, Init, String_V, kInlineStringInit, 13),
    SPECIAL(JavaLangString, Init, StringBuffer_V, kInlineStringInit, 14),
    SPECIAL(JavaLangString, Init, StringBuilder_V, kInlineStringInit, 15),

#undef SPECIAL
};

DexFileMethodInliner::DexFileMethodInliner()
    : lock_("DexFileMethodInliner lock", kDexFileMethodInlinerLock),
      dex_file_(nullptr) {
  static_assert(kClassCacheFirst == 0, "kClassCacheFirst not 0");
  static_assert(arraysize(kClassCacheNames) == kClassCacheLast,
                "bad arraysize for kClassCacheNames");
  static_assert(kNameCacheFirst == 0, "kNameCacheFirst not 0");
  static_assert(arraysize(kNameCacheNames) == kNameCacheLast,
                "bad arraysize for kNameCacheNames");
  static_assert(kProtoCacheFirst == 0, "kProtoCacheFirst not 0");
  static_assert(arraysize(kProtoCacheDefs) == kProtoCacheLast,
                "bad arraysize kProtoCacheNames");
}

DexFileMethodInliner::~DexFileMethodInliner() {
}

bool DexFileMethodInliner::AnalyseMethodCode(verifier::MethodVerifier* verifier) {
  InlineMethod method;
  bool success = InlineMethodAnalyser::AnalyseMethodCode(verifier, &method);
  return success && AddInlineMethod(verifier->GetMethodReference().dex_method_index, method);
}

InlineMethodFlags DexFileMethodInliner::IsIntrinsicOrSpecial(uint32_t method_index) {
  ReaderMutexLock mu(Thread::Current(), lock_);
  auto it = inline_methods_.find(method_index);
  if (it != inline_methods_.end()) {
    DCHECK_NE(it->second.flags & (kInlineIntrinsic | kInlineSpecial), 0);
    return it->second.flags;
  } else {
    return kNoInlineMethodFlags;
  }
}

bool DexFileMethodInliner::IsIntrinsic(uint32_t method_index, InlineMethod* intrinsic) {
  ReaderMutexLock mu(Thread::Current(), lock_);
  auto it = inline_methods_.find(method_index);
  bool res = (it != inline_methods_.end() && (it->second.flags & kInlineIntrinsic) != 0);
  if (res && intrinsic != nullptr) {
    *intrinsic = it->second;
  }
  return res;
}

bool DexFileMethodInliner::GenIntrinsic(Mir2Lir* backend, CallInfo* info) {
  InlineMethod intrinsic;
  {
    ReaderMutexLock mu(Thread::Current(), lock_);
    auto it = inline_methods_.find(info->method_ref.dex_method_index);
    if (it == inline_methods_.end() || (it->second.flags & kInlineIntrinsic) == 0) {
      return false;
    }
    intrinsic = it->second;
  }
  if (kIntrinsicIsStatic[intrinsic.opcode] != (info->type == kStatic)) {
    // Invoke type mismatch.
    return false;
  }
  switch (intrinsic.opcode) {
    case kIntrinsicDoubleCvt:
      return backend->GenInlinedDoubleCvt(info);
    case kIntrinsicFloatCvt:
      return backend->GenInlinedFloatCvt(info);
    case kIntrinsicReverseBytes:
      return backend->GenInlinedReverseBytes(info, static_cast<OpSize>(intrinsic.d.data));
    case kIntrinsicReverseBits:
      return backend->GenInlinedReverseBits(info, static_cast<OpSize>(intrinsic.d.data));
    case kIntrinsicAbsInt:
      return backend->GenInlinedAbsInt(info);
    case kIntrinsicAbsLong:
      return backend->GenInlinedAbsLong(info);
    case kIntrinsicAbsFloat:
      return backend->GenInlinedAbsFloat(info);
    case kIntrinsicAbsDouble:
      return backend->GenInlinedAbsDouble(info);
    case kIntrinsicMinMaxInt:
      return backend->GenInlinedMinMax(info, intrinsic.d.data & kIntrinsicFlagMin, false /* is_long */);
    case kIntrinsicMinMaxLong:
      return backend->GenInlinedMinMax(info, intrinsic.d.data & kIntrinsicFlagMin, true /* is_long */);
    case kIntrinsicMinMaxFloat:
      return backend->GenInlinedMinMaxFP(info, intrinsic.d.data & kIntrinsicFlagMin, false /* is_double */);
    case kIntrinsicMinMaxDouble:
      return backend->GenInlinedMinMaxFP(info, intrinsic.d.data & kIntrinsicFlagMin, true /* is_double */);
    case kIntrinsicSqrt:
      return backend->GenInlinedSqrt(info);
    case kIntrinsicCeil:
      return backend->GenInlinedCeil(info);
    case kIntrinsicFloor:
      return backend->GenInlinedFloor(info);
    case kIntrinsicRint:
      return backend->GenInlinedRint(info);
    case kIntrinsicRoundFloat:
      return backend->GenInlinedRound(info, false /* is_double */);
    case kIntrinsicRoundDouble:
      return backend->GenInlinedRound(info, true /* is_double */);
    case kIntrinsicReferenceGetReferent:
      return backend->GenInlinedReferenceGetReferent(info);
    case kIntrinsicCharAt:
      return backend->GenInlinedCharAt(info);
    case kIntrinsicCompareTo:
      return backend->GenInlinedStringCompareTo(info);
    case kIntrinsicGetCharsNoCheck:
      return backend->GenInlinedStringGetCharsNoCheck(info);
    case kIntrinsicIsEmptyOrLength:
      return backend->GenInlinedStringIsEmptyOrLength(
          info, intrinsic.d.data & kIntrinsicFlagIsEmpty);
    case kIntrinsicIndexOf:
      return backend->GenInlinedIndexOf(info, intrinsic.d.data & kIntrinsicFlagBase0);
    case kIntrinsicNewStringFromBytes:
      return backend->GenInlinedStringFactoryNewStringFromBytes(info);
    case kIntrinsicNewStringFromChars:
      return backend->GenInlinedStringFactoryNewStringFromChars(info);
    case kIntrinsicNewStringFromString:
      return backend->GenInlinedStringFactoryNewStringFromString(info);
    case kIntrinsicCurrentThread:
      return backend->GenInlinedCurrentThread(info);
    case kIntrinsicPeek:
      return backend->GenInlinedPeek(info, static_cast<OpSize>(intrinsic.d.data));
    case kIntrinsicPoke:
      return backend->GenInlinedPoke(info, static_cast<OpSize>(intrinsic.d.data));
    case kIntrinsicCas:
      return backend->GenInlinedCas(info, intrinsic.d.data & kIntrinsicFlagIsLong,
                                    intrinsic.d.data & kIntrinsicFlagIsObject);
    case kIntrinsicUnsafeGet:
      return backend->GenInlinedUnsafeGet(info, intrinsic.d.data & kIntrinsicFlagIsLong,
                                          intrinsic.d.data & kIntrinsicFlagIsObject,
                                          intrinsic.d.data & kIntrinsicFlagIsVolatile);
    case kIntrinsicUnsafePut:
      return backend->GenInlinedUnsafePut(info, intrinsic.d.data & kIntrinsicFlagIsLong,
                                          intrinsic.d.data & kIntrinsicFlagIsObject,
                                          intrinsic.d.data & kIntrinsicFlagIsVolatile,
                                          intrinsic.d.data & kIntrinsicFlagIsOrdered);
    case kIntrinsicSystemArrayCopyCharArray:
      return backend->GenInlinedArrayCopyCharArray(info);
    default:
      LOG(FATAL) << "Unexpected intrinsic opcode: " << intrinsic.opcode;
      return false;  // avoid warning "control reaches end of non-void function"
  }
}

bool DexFileMethodInliner::IsSpecial(uint32_t method_index) {
  ReaderMutexLock mu(Thread::Current(), lock_);
  auto it = inline_methods_.find(method_index);
  return it != inline_methods_.end() && (it->second.flags & kInlineSpecial) != 0;
}

bool DexFileMethodInliner::GenSpecial(Mir2Lir* backend, uint32_t method_idx) {
  InlineMethod special;
  {
    ReaderMutexLock mu(Thread::Current(), lock_);
    auto it = inline_methods_.find(method_idx);
    if (it == inline_methods_.end() || (it->second.flags & kInlineSpecial) == 0) {
      return false;
    }
    special = it->second;
  }
  return backend->SpecialMIR2LIR(special);
}

bool DexFileMethodInliner::GenInline(MIRGraph* mir_graph, BasicBlock* bb, MIR* invoke,
                                     uint32_t method_idx) {
  InlineMethod method;
  {
    ReaderMutexLock mu(Thread::Current(), lock_);
    auto it = inline_methods_.find(method_idx);
    if (it == inline_methods_.end() || (it->second.flags & kInlineSpecial) == 0) {
      return false;
    }
    method = it->second;
  }

  MIR* move_result = nullptr;
  bool result = true;
  switch (method.opcode) {
    case kInlineOpNop:
      break;
    case kInlineOpNonWideConst:
      move_result = mir_graph->FindMoveResult(bb, invoke);
      result = GenInlineConst(mir_graph, bb, invoke, move_result, method);
      break;
    case kInlineOpReturnArg:
      move_result = mir_graph->FindMoveResult(bb, invoke);
      result = GenInlineReturnArg(mir_graph, bb, invoke, move_result, method);
      break;
    case kInlineOpIGet:
      move_result = mir_graph->FindMoveResult(bb, invoke);
      result = GenInlineIGet(mir_graph, bb, invoke, move_result, method);
      break;
    case kInlineOpIPut:
      move_result = mir_graph->FindMoveResult(bb, invoke);
      result = GenInlineIPut(mir_graph, bb, invoke, move_result, method);
      break;
    case kInlineStringInit:
      return false;
    default:
      LOG(FATAL) << "Unexpected inline op: " << method.opcode;
      break;
  }
  if (result) {
    // If the invoke has not been eliminated yet, check now whether we should do it.
    // This is done so that dataflow analysis does not get tripped up seeing nop invoke.
    if (static_cast<int>(invoke->dalvikInsn.opcode) != kMirOpNop) {
      bool is_static = IsInstructionInvokeStatic(invoke->dalvikInsn.opcode);
      if (is_static || (invoke->optimization_flags & MIR_IGNORE_NULL_CHECK) != 0) {
        // No null object register involved here so we can eliminate the invoke.
        invoke->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
      } else {
        // Invoke was kept around because null check needed to be done.
        invoke->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNullCheck);
        // For invokes, the object register is in vC. For null check mir, it is in vA.
        invoke->dalvikInsn.vA = invoke->dalvikInsn.vC;
      }
    }
    if (move_result != nullptr) {
      move_result->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
    }
  }
  return result;
}

uint32_t DexFileMethodInliner::FindClassIndex(const DexFile* dex_file, IndexCache* cache,
                                              ClassCacheIndex index) {
  uint32_t* class_index = &cache->class_indexes[index];
  if (*class_index != kIndexUnresolved) {
    return *class_index;
  }

  const DexFile::StringId* string_id = dex_file->FindStringId(kClassCacheNames[index]);
  if (string_id == nullptr) {
    *class_index = kIndexNotFound;
    return *class_index;
  }
  uint32_t string_index = dex_file->GetIndexForStringId(*string_id);

  const DexFile::TypeId* type_id = dex_file->FindTypeId(string_index);
  if (type_id == nullptr) {
    *class_index = kIndexNotFound;
    return *class_index;
  }
  *class_index = dex_file->GetIndexForTypeId(*type_id);
  return *class_index;
}

uint32_t DexFileMethodInliner::FindNameIndex(const DexFile* dex_file, IndexCache* cache,
                                             NameCacheIndex index) {
  uint32_t* name_index = &cache->name_indexes[index];
  if (*name_index != kIndexUnresolved) {
    return *name_index;
  }

  const DexFile::StringId* string_id = dex_file->FindStringId(kNameCacheNames[index]);
  if (string_id == nullptr) {
    *name_index = kIndexNotFound;
    return *name_index;
  }
  *name_index = dex_file->GetIndexForStringId(*string_id);
  return *name_index;
}

uint32_t DexFileMethodInliner::FindProtoIndex(const DexFile* dex_file, IndexCache* cache,
                                              ProtoCacheIndex index) {
  uint32_t* proto_index = &cache->proto_indexes[index];
  if (*proto_index != kIndexUnresolved) {
    return *proto_index;
  }

  const ProtoDef& proto_def = kProtoCacheDefs[index];
  uint32_t return_index = FindClassIndex(dex_file, cache, proto_def.return_type);
  if (return_index == kIndexNotFound) {
    *proto_index = kIndexNotFound;
    return *proto_index;
  }
  uint16_t return_type = static_cast<uint16_t>(return_index);
  DCHECK_EQ(static_cast<uint32_t>(return_type), return_index);

  uint32_t signature_length = proto_def.param_count;
  uint16_t signature_type_idxs[kProtoMaxParams];
  for (uint32_t i = 0; i != signature_length; ++i) {
    uint32_t param_index = FindClassIndex(dex_file, cache, proto_def.params[i]);
    if (param_index == kIndexNotFound) {
      *proto_index = kIndexNotFound;
      return *proto_index;
    }
    signature_type_idxs[i] = static_cast<uint16_t>(param_index);
    DCHECK_EQ(static_cast<uint32_t>(signature_type_idxs[i]), param_index);
  }

  const DexFile::ProtoId* proto_id = dex_file->FindProtoId(return_type, signature_type_idxs,
                                                           signature_length);
  if (proto_id == nullptr) {
    *proto_index = kIndexNotFound;
    return *proto_index;
  }
  *proto_index = dex_file->GetIndexForProtoId(*proto_id);
  return *proto_index;
}

uint32_t DexFileMethodInliner::FindMethodIndex(const DexFile* dex_file, IndexCache* cache,
                                               const MethodDef& method_def) {
  uint32_t declaring_class_index = FindClassIndex(dex_file, cache, method_def.declaring_class);
  if (declaring_class_index == kIndexNotFound) {
    return kIndexNotFound;
  }
  uint32_t name_index = FindNameIndex(dex_file, cache, method_def.name);
  if (name_index == kIndexNotFound) {
    return kIndexNotFound;
  }
  uint32_t proto_index = FindProtoIndex(dex_file, cache, method_def.proto);
  if (proto_index == kIndexNotFound) {
    return kIndexNotFound;
  }
  const DexFile::MethodId* method_id =
      dex_file->FindMethodId(dex_file->GetTypeId(declaring_class_index),
                             dex_file->GetStringId(name_index),
                             dex_file->GetProtoId(proto_index));
  if (method_id == nullptr) {
    return kIndexNotFound;
  }
  return dex_file->GetIndexForMethodId(*method_id);
}

DexFileMethodInliner::IndexCache::IndexCache() {
  std::fill_n(class_indexes, arraysize(class_indexes), kIndexUnresolved);
  std::fill_n(name_indexes, arraysize(name_indexes), kIndexUnresolved);
  std::fill_n(proto_indexes, arraysize(proto_indexes), kIndexUnresolved);
}

void DexFileMethodInliner::FindIntrinsics(const DexFile* dex_file) {
  DCHECK(dex_file != nullptr);
  DCHECK(dex_file_ == nullptr);
  IndexCache cache;
  for (const IntrinsicDef& def : kIntrinsicMethods) {
    uint32_t method_idx = FindMethodIndex(dex_file, &cache, def.method_def);
    if (method_idx != kIndexNotFound) {
      DCHECK(inline_methods_.find(method_idx) == inline_methods_.end());
      inline_methods_.Put(method_idx, def.intrinsic);
    }
  }
  dex_file_ = dex_file;
}

bool DexFileMethodInliner::AddInlineMethod(int32_t method_idx, const InlineMethod& method) {
  WriterMutexLock mu(Thread::Current(), lock_);
  if (LIKELY(inline_methods_.find(method_idx) == inline_methods_.end())) {
    inline_methods_.Put(method_idx, method);
    return true;
  } else {
    if (PrettyMethod(method_idx, *dex_file_) == "int java.lang.String.length()") {
      // TODO: String.length is both kIntrinsicIsEmptyOrLength and kInlineOpIGet.
    } else {
      LOG(WARNING) << "Inliner: " << PrettyMethod(method_idx, *dex_file_) << " already inline";
    }
    return false;
  }
}

bool DexFileMethodInliner::GenInlineConst(MIRGraph* mir_graph, BasicBlock* bb, MIR* invoke,
                                          MIR* move_result, const InlineMethod& method) {
  if (move_result == nullptr) {
    // Result is unused.
    return true;
  }

  // Check the opcode and for MOVE_RESULT_OBJECT check also that the constant is null.
  DCHECK(move_result->dalvikInsn.opcode == Instruction::MOVE_RESULT ||
         (move_result->dalvikInsn.opcode == Instruction::MOVE_RESULT_OBJECT &&
             method.d.data == 0u));

  // Insert the CONST instruction.
  MIR* insn = AllocReplacementMIR(mir_graph, invoke);
  insn->dalvikInsn.opcode = Instruction::CONST;
  insn->dalvikInsn.vA = move_result->dalvikInsn.vA;
  insn->dalvikInsn.vB = method.d.data;
  insn->meta.method_lowering_info = invoke->meta.method_lowering_info;  // Preserve type info.
  bb->InsertMIRAfter(move_result, insn);
  return true;
}

bool DexFileMethodInliner::GenInlineReturnArg(MIRGraph* mir_graph, BasicBlock* bb, MIR* invoke,
                                              MIR* move_result, const InlineMethod& method) {
  if (move_result == nullptr) {
    // Result is unused.
    return true;
  }

  // Select opcode and argument.
  const InlineReturnArgData& data = method.d.return_data;
  Instruction::Code opcode = Instruction::MOVE_FROM16;
  uint32_t arg = GetInvokeReg(invoke, data.arg);
  if (move_result->dalvikInsn.opcode == Instruction::MOVE_RESULT_OBJECT) {
    DCHECK_EQ(data.is_object, 1u);
    DCHECK_EQ(data.is_wide, 0u);
    opcode = Instruction::MOVE_OBJECT_FROM16;
  } else if (move_result->dalvikInsn.opcode == Instruction::MOVE_RESULT_WIDE) {
    DCHECK_EQ(data.is_wide, 1u);
    DCHECK_EQ(data.is_object, 0u);
    opcode = Instruction::MOVE_WIDE_FROM16;
    if (!WideArgIsInConsecutiveDalvikRegs(invoke, data.arg)) {
      // The two halfs of the source value are not in consecutive dalvik registers in INVOKE.
      return false;
    }
  } else {
    DCHECK(move_result->dalvikInsn.opcode == Instruction::MOVE_RESULT);
    DCHECK_EQ(data.is_wide, 0u);
    DCHECK_EQ(data.is_object, 0u);
  }

  // Insert the move instruction
  MIR* insn = AllocReplacementMIR(mir_graph, invoke);
  insn->dalvikInsn.opcode = opcode;
  insn->dalvikInsn.vA = move_result->dalvikInsn.vA;
  insn->dalvikInsn.vB = arg;
  insn->meta.method_lowering_info = invoke->meta.method_lowering_info;  // Preserve type info.
  bb->InsertMIRAfter(move_result, insn);
  return true;
}

bool DexFileMethodInliner::GenInlineIGet(MIRGraph* mir_graph, BasicBlock* bb, MIR* invoke,
                                         MIR* move_result, const InlineMethod& method) {
  CompilationUnit* cu = mir_graph->GetCurrentDexCompilationUnit()->GetCompilationUnit();
  if (cu->enable_debug & (1 << kDebugSlowFieldPath)) {
    return false;
  }

  const InlineIGetIPutData& data = method.d.ifield_data;
  Instruction::Code opcode = static_cast<Instruction::Code>(Instruction::IGET + data.op_variant);
  DCHECK_EQ(InlineMethodAnalyser::IGetVariant(opcode), data.op_variant);
  uint32_t object_reg = GetInvokeReg(invoke, data.object_arg);

  if (move_result == nullptr) {
    // Result is unused. If volatile, we still need to emit the IGET but we have no destination.
    return !data.is_volatile;
  }

  DCHECK_EQ(data.method_is_static != 0u, IsInstructionInvokeStatic(invoke->dalvikInsn.opcode));
  bool object_is_this = (data.method_is_static == 0u && data.object_arg == 0u);
  if (!object_is_this) {
    // TODO: Implement inlining of IGET on non-"this" registers (needs correct stack trace for NPE).
    // Allow synthetic accessors. We don't care about losing their stack frame in NPE.
    if (!InlineMethodAnalyser::IsSyntheticAccessor(
        mir_graph->GetMethodLoweringInfo(invoke).GetTargetMethod())) {
      return false;
    }
  }

  if (object_is_this) {
    // Mark invoke as NOP, null-check is done on IGET. No aborts after this.
    invoke->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
  }

  MIR* insn = AllocReplacementMIR(mir_graph, invoke);
  insn->offset = invoke->offset;
  insn->dalvikInsn.opcode = opcode;
  insn->dalvikInsn.vA = move_result->dalvikInsn.vA;
  insn->dalvikInsn.vB = object_reg;
  mir_graph->ComputeInlineIFieldLoweringInfo(data.field_idx, invoke, insn);

  DCHECK(mir_graph->GetIFieldLoweringInfo(insn).IsResolved());
  DCHECK(mir_graph->GetIFieldLoweringInfo(insn).FastGet());
  DCHECK_EQ(data.field_offset, mir_graph->GetIFieldLoweringInfo(insn).FieldOffset().Uint32Value());
  DCHECK_EQ(data.is_volatile, mir_graph->GetIFieldLoweringInfo(insn).IsVolatile() ? 1u : 0u);

  bb->InsertMIRAfter(move_result, insn);
  return true;
}

bool DexFileMethodInliner::GenInlineIPut(MIRGraph* mir_graph, BasicBlock* bb, MIR* invoke,
                                         MIR* move_result, const InlineMethod& method) {
  CompilationUnit* cu = mir_graph->GetCurrentDexCompilationUnit()->GetCompilationUnit();
  if (cu->enable_debug & (1 << kDebugSlowFieldPath)) {
    return false;
  }

  const InlineIGetIPutData& data = method.d.ifield_data;
  Instruction::Code opcode = static_cast<Instruction::Code>(Instruction::IPUT + data.op_variant);
  DCHECK_EQ(InlineMethodAnalyser::IPutVariant(opcode), data.op_variant);
  uint32_t object_reg = GetInvokeReg(invoke, data.object_arg);
  uint32_t src_reg = GetInvokeReg(invoke, data.src_arg);
  uint32_t return_reg =
      data.return_arg_plus1 != 0u ? GetInvokeReg(invoke, data.return_arg_plus1 - 1u) : 0u;

  if (opcode == Instruction::IPUT_WIDE && !WideArgIsInConsecutiveDalvikRegs(invoke, data.src_arg)) {
    // The two halfs of the source value are not in consecutive dalvik registers in INVOKE.
    return false;
  }

  DCHECK(move_result == nullptr || data.return_arg_plus1 != 0u);
  if (move_result != nullptr && move_result->dalvikInsn.opcode == Instruction::MOVE_RESULT_WIDE &&
      !WideArgIsInConsecutiveDalvikRegs(invoke, data.return_arg_plus1 - 1u)) {
    // The two halfs of the return value are not in consecutive dalvik registers in INVOKE.
    return false;
  }

  DCHECK_EQ(data.method_is_static != 0u, IsInstructionInvokeStatic(invoke->dalvikInsn.opcode));
  bool object_is_this = (data.method_is_static == 0u && data.object_arg == 0u);
  if (!object_is_this) {
    // TODO: Implement inlining of IPUT on non-"this" registers (needs correct stack trace for NPE).
    // Allow synthetic accessors. We don't care about losing their stack frame in NPE.
    if (!InlineMethodAnalyser::IsSyntheticAccessor(
        mir_graph->GetMethodLoweringInfo(invoke).GetTargetMethod())) {
      return false;
    }
  }

  if (object_is_this) {
    // Mark invoke as NOP, null-check is done on IPUT. No aborts after this.
    invoke->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
  }

  MIR* insn = AllocReplacementMIR(mir_graph, invoke);
  insn->dalvikInsn.opcode = opcode;
  insn->dalvikInsn.vA = src_reg;
  insn->dalvikInsn.vB = object_reg;
  mir_graph->ComputeInlineIFieldLoweringInfo(data.field_idx, invoke, insn);

  DCHECK(mir_graph->GetIFieldLoweringInfo(insn).IsResolved());
  DCHECK(mir_graph->GetIFieldLoweringInfo(insn).FastPut());
  DCHECK_EQ(data.field_offset, mir_graph->GetIFieldLoweringInfo(insn).FieldOffset().Uint32Value());
  DCHECK_EQ(data.is_volatile, mir_graph->GetIFieldLoweringInfo(insn).IsVolatile() ? 1u : 0u);

  bb->InsertMIRAfter(invoke, insn);

  if (move_result != nullptr) {
    MIR* move = AllocReplacementMIR(mir_graph, invoke);
    move->offset = move_result->offset;
    if (move_result->dalvikInsn.opcode == Instruction::MOVE_RESULT) {
      move->dalvikInsn.opcode = Instruction::MOVE_FROM16;
    } else if (move_result->dalvikInsn.opcode == Instruction::MOVE_RESULT_OBJECT) {
      move->dalvikInsn.opcode = Instruction::MOVE_OBJECT_FROM16;
    } else {
      DCHECK_EQ(move_result->dalvikInsn.opcode, Instruction::MOVE_RESULT_WIDE);
      move->dalvikInsn.opcode = Instruction::MOVE_WIDE_FROM16;
    }
    move->dalvikInsn.vA = move_result->dalvikInsn.vA;
    move->dalvikInsn.vB = return_reg;
    move->meta.method_lowering_info = invoke->meta.method_lowering_info;  // Preserve type info.
    bb->InsertMIRAfter(insn, move);
  }
  return true;
}

uint32_t DexFileMethodInliner::GetOffsetForStringInit(uint32_t method_index, size_t pointer_size) {
  ReaderMutexLock mu(Thread::Current(), lock_);
  auto it = inline_methods_.find(method_index);
  if (it != inline_methods_.end() && (it->second.opcode == kInlineStringInit)) {
    uint32_t string_init_base_offset = Thread::QuickEntryPointOffsetWithSize(
              OFFSETOF_MEMBER(QuickEntryPoints, pNewEmptyString), pointer_size);
    return string_init_base_offset + it->second.d.data * pointer_size;
  }
  return 0;
}

bool DexFileMethodInliner::IsStringInitMethodIndex(uint32_t method_index) {
  ReaderMutexLock mu(Thread::Current(), lock_);
  auto it = inline_methods_.find(method_index);
  return (it != inline_methods_.end()) && (it->second.opcode == kInlineStringInit);
}

}  // namespace art
