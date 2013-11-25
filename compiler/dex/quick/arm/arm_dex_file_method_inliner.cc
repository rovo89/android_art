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

#include "base/logging.h"
#include "base/macros.h"
#include "dex/compiler_enums.h"

#include "arm_dex_file_method_inliner.h"

namespace art {

const DexFileMethodInliner::IntrinsicDef ArmDexFileMethodInliner::kIntrinsicMethods[] = {
#define INTRINSIC(c, n, p, o, d) \
    { { kClassCache ## c, kNameCache ## n, kProtoCache ## p }, { o, d } }

    INTRINSIC(JavaLangDouble, DoubleToRawLongBits, D_J, kIntrinsicDoubleCvt, 0),
    INTRINSIC(JavaLangDouble, LongBitsToDouble, J_D, kIntrinsicDoubleCvt, 0),
    INTRINSIC(JavaLangFloat, FloatToRawIntBits, F_I, kIntrinsicFloatCvt, 0),
    INTRINSIC(JavaLangFloat, IntBitsToFloat, I_F, kIntrinsicFloatCvt, 0),

    INTRINSIC(JavaLangInteger, ReverseBytes, I_I, kIntrinsicReverseBytes, kWord),
    INTRINSIC(JavaLangLong, ReverseBytes, J_J, kIntrinsicReverseBytes, kLong),
    INTRINSIC(JavaLangShort, ReverseBytes, S_S, kIntrinsicReverseBytes, kSignedHalf),

    INTRINSIC(JavaLangMath,       Abs, I_I, kIntrinsicAbsInt, 0),
    INTRINSIC(JavaLangStrictMath, Abs, I_I, kIntrinsicAbsInt, 0),
    INTRINSIC(JavaLangMath,       Abs, J_J, kIntrinsicAbsLong, 0),
    INTRINSIC(JavaLangStrictMath, Abs, J_J, kIntrinsicAbsLong, 0),
    INTRINSIC(JavaLangMath,       Min, II_I, kIntrinsicMinMaxInt, kIntrinsicFlagMin),
    INTRINSIC(JavaLangStrictMath, Min, II_I, kIntrinsicMinMaxInt, kIntrinsicFlagMin),
    INTRINSIC(JavaLangMath,       Max, II_I, kIntrinsicMinMaxInt, kIntrinsicFlagMax),
    INTRINSIC(JavaLangStrictMath, Max, II_I, kIntrinsicMinMaxInt, kIntrinsicFlagMax),
    INTRINSIC(JavaLangMath,       Sqrt, D_D, kIntrinsicSqrt, 0),
    INTRINSIC(JavaLangStrictMath, Sqrt, D_D, kIntrinsicSqrt, 0),

    INTRINSIC(JavaLangString, CharAt, I_C, kIntrinsicCharAt, 0),
    INTRINSIC(JavaLangString, CompareTo, String_I, kIntrinsicCompareTo, 0),
    INTRINSIC(JavaLangString, IsEmpty, _Z, kIntrinsicIsEmptyOrLength, kIntrinsicFlagIsEmpty),
    INTRINSIC(JavaLangString, IndexOf, II_I, kIntrinsicIndexOf, kIntrinsicFlagNone),
    INTRINSIC(JavaLangString, IndexOf, I_I, kIntrinsicIndexOf, kIntrinsicFlagBase0),
    INTRINSIC(JavaLangString, Length, _I, kIntrinsicIsEmptyOrLength, kIntrinsicFlagLength),

    INTRINSIC(JavaLangThread, CurrentThread, _Thread, kIntrinsicCurrentThread, 0),

    INTRINSIC(LibcoreIoMemory, PeekByte, J_B, kIntrinsicPeek, kSignedByte),
    INTRINSIC(LibcoreIoMemory, PeekIntNative, J_I, kIntrinsicPeek, kWord),
    INTRINSIC(LibcoreIoMemory, PeekLongNative, J_J, kIntrinsicPeek, kLong),
    INTRINSIC(LibcoreIoMemory, PeekShortNative, J_S, kIntrinsicPeek, kSignedHalf),
    INTRINSIC(LibcoreIoMemory, PokeByte, JB_V, kIntrinsicPoke, kSignedByte),
    INTRINSIC(LibcoreIoMemory, PokeIntNative, JI_V, kIntrinsicPoke, kWord),
    INTRINSIC(LibcoreIoMemory, PokeLongNative, JJ_V, kIntrinsicPoke, kLong),
    INTRINSIC(LibcoreIoMemory, PokeShortNative, JS_V, kIntrinsicPoke, kSignedHalf),

    INTRINSIC(SunMiscUnsafe, CompareAndSwapInt, ObjectJII_Z, kIntrinsicCas,
              kIntrinsicFlagNone),
    // INTRINSIC(SunMiscUnsafe, CompareAndSwapLong, ObjectJJJ_Z, kIntrinsicCas,
    //           kIntrinsicFlagIsLong),
    INTRINSIC(SunMiscUnsafe, CompareAndSwapObject, ObjectJObjectObject_Z, kIntrinsicCas,
              kIntrinsicFlagIsObject),

#define UNSAFE_GET_PUT(type, code, type_flags) \
    INTRINSIC(SunMiscUnsafe, Get ## type, ObjectJ_ ## code, kIntrinsicUnsafeGet, \
              type_flags & ~kIntrinsicFlagIsObject), \
    INTRINSIC(SunMiscUnsafe, Get ## type ## Volatile, ObjectJ_ ## code, kIntrinsicUnsafeGet, \
              (type_flags | kIntrinsicFlagIsVolatile) & ~kIntrinsicFlagIsObject), \
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

#undef INTRINSIC
};

ArmDexFileMethodInliner::ArmDexFileMethodInliner() {
}

ArmDexFileMethodInliner::~ArmDexFileMethodInliner() {
}

void ArmDexFileMethodInliner::FindIntrinsics(const DexFile* dex_file) {
  IndexCache cache;
  DoFindIntrinsics(dex_file, &cache, kIntrinsicMethods, arraysize(kIntrinsicMethods));
}

}  // namespace art
