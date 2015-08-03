/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ART_COMPILER_OPTIMIZING_INTRINSICS_LIST_H_
#define ART_COMPILER_OPTIMIZING_INTRINSICS_LIST_H_

// All intrinsics supported by the optimizing compiler. Format is name, then whether it is expected
// to be a HInvokeStaticOrDirect node (compared to HInvokeVirtual).

#define INTRINSICS_LIST(V) \
  V(DoubleDoubleToRawLongBits, kStatic) \
  V(DoubleLongBitsToDouble, kStatic) \
  V(FloatFloatToRawIntBits, kStatic) \
  V(FloatIntBitsToFloat, kStatic) \
  V(IntegerReverse, kStatic) \
  V(IntegerReverseBytes, kStatic) \
  V(LongReverse, kStatic) \
  V(LongReverseBytes, kStatic) \
  V(ShortReverseBytes, kStatic) \
  V(MathAbsDouble, kStatic) \
  V(MathAbsFloat, kStatic) \
  V(MathAbsLong, kStatic) \
  V(MathAbsInt, kStatic) \
  V(MathMinDoubleDouble, kStatic) \
  V(MathMinFloatFloat, kStatic) \
  V(MathMinLongLong, kStatic) \
  V(MathMinIntInt, kStatic) \
  V(MathMaxDoubleDouble, kStatic) \
  V(MathMaxFloatFloat, kStatic) \
  V(MathMaxLongLong, kStatic) \
  V(MathMaxIntInt, kStatic) \
  V(MathSqrt, kStatic) \
  V(MathCeil, kStatic) \
  V(MathFloor, kStatic) \
  V(MathRint, kStatic) \
  V(MathRoundDouble, kStatic) \
  V(MathRoundFloat, kStatic) \
  V(SystemArrayCopyChar, kStatic) \
  V(ThreadCurrentThread, kStatic) \
  V(MemoryPeekByte, kStatic) \
  V(MemoryPeekIntNative, kStatic) \
  V(MemoryPeekLongNative, kStatic) \
  V(MemoryPeekShortNative, kStatic) \
  V(MemoryPokeByte, kStatic) \
  V(MemoryPokeIntNative, kStatic) \
  V(MemoryPokeLongNative, kStatic) \
  V(MemoryPokeShortNative, kStatic) \
  V(StringCharAt, kDirect) \
  V(StringCompareTo, kDirect) \
  V(StringGetCharsNoCheck, kDirect) \
  V(StringIndexOf, kDirect) \
  V(StringIndexOfAfter, kDirect) \
  V(StringNewStringFromBytes, kStatic) \
  V(StringNewStringFromChars, kStatic) \
  V(StringNewStringFromString, kStatic) \
  V(UnsafeCASInt, kDirect) \
  V(UnsafeCASLong, kDirect) \
  V(UnsafeCASObject, kDirect) \
  V(UnsafeGet, kDirect) \
  V(UnsafeGetVolatile, kDirect) \
  V(UnsafeGetObject, kDirect) \
  V(UnsafeGetObjectVolatile, kDirect) \
  V(UnsafeGetLong, kDirect) \
  V(UnsafeGetLongVolatile, kDirect) \
  V(UnsafePut, kDirect) \
  V(UnsafePutOrdered, kDirect) \
  V(UnsafePutVolatile, kDirect) \
  V(UnsafePutObject, kDirect) \
  V(UnsafePutObjectOrdered, kDirect) \
  V(UnsafePutObjectVolatile, kDirect) \
  V(UnsafePutLong, kDirect) \
  V(UnsafePutLongOrdered, kDirect) \
  V(UnsafePutLongVolatile, kDirect) \
  V(ReferenceGetReferent, kDirect)

#endif  // ART_COMPILER_OPTIMIZING_INTRINSICS_LIST_H_
#undef ART_COMPILER_OPTIMIZING_INTRINSICS_LIST_H_   // #define is only for lint.
