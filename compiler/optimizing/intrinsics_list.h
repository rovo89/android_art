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
// to be a HInvokeStaticOrDirect node (compared to HInvokeVirtual), then whether it requires an
// environment.

#define INTRINSICS_LIST(V) \
  V(DoubleDoubleToRawLongBits, kStatic, kNeedsEnvironmentOrCache) \
  V(DoubleLongBitsToDouble, kStatic, kNeedsEnvironmentOrCache) \
  V(FloatFloatToRawIntBits, kStatic, kNeedsEnvironmentOrCache) \
  V(FloatIntBitsToFloat, kStatic, kNeedsEnvironmentOrCache) \
  V(IntegerReverse, kStatic, kNeedsEnvironmentOrCache) \
  V(IntegerReverseBytes, kStatic, kNeedsEnvironmentOrCache) \
  V(IntegerNumberOfLeadingZeros, kStatic, kNeedsEnvironmentOrCache) \
  V(IntegerNumberOfTrailingZeros, kStatic, kNeedsEnvironmentOrCache) \
  V(IntegerRotateRight, kStatic, kNeedsEnvironmentOrCache) \
  V(IntegerRotateLeft, kStatic, kNeedsEnvironmentOrCache) \
  V(LongReverse, kStatic, kNeedsEnvironmentOrCache) \
  V(LongReverseBytes, kStatic, kNeedsEnvironmentOrCache) \
  V(LongNumberOfLeadingZeros, kStatic, kNeedsEnvironmentOrCache) \
  V(LongNumberOfTrailingZeros, kStatic, kNeedsEnvironmentOrCache) \
  V(LongRotateRight, kStatic, kNeedsEnvironmentOrCache) \
  V(LongRotateLeft, kStatic, kNeedsEnvironmentOrCache) \
  V(ShortReverseBytes, kStatic, kNeedsEnvironmentOrCache) \
  V(MathAbsDouble, kStatic, kNeedsEnvironmentOrCache) \
  V(MathAbsFloat, kStatic, kNeedsEnvironmentOrCache) \
  V(MathAbsLong, kStatic, kNeedsEnvironmentOrCache) \
  V(MathAbsInt, kStatic, kNeedsEnvironmentOrCache) \
  V(MathMinDoubleDouble, kStatic, kNeedsEnvironmentOrCache) \
  V(MathMinFloatFloat, kStatic, kNeedsEnvironmentOrCache) \
  V(MathMinLongLong, kStatic, kNeedsEnvironmentOrCache) \
  V(MathMinIntInt, kStatic, kNeedsEnvironmentOrCache) \
  V(MathMaxDoubleDouble, kStatic, kNeedsEnvironmentOrCache) \
  V(MathMaxFloatFloat, kStatic, kNeedsEnvironmentOrCache) \
  V(MathMaxLongLong, kStatic, kNeedsEnvironmentOrCache) \
  V(MathMaxIntInt, kStatic, kNeedsEnvironmentOrCache) \
  V(MathCos, kStatic, kNeedsEnvironmentOrCache) \
  V(MathSin, kStatic, kNeedsEnvironmentOrCache) \
  V(MathAcos, kStatic, kNeedsEnvironmentOrCache) \
  V(MathAsin, kStatic, kNeedsEnvironmentOrCache) \
  V(MathAtan, kStatic, kNeedsEnvironmentOrCache) \
  V(MathAtan2, kStatic, kNeedsEnvironmentOrCache) \
  V(MathCbrt, kStatic, kNeedsEnvironmentOrCache) \
  V(MathCosh, kStatic, kNeedsEnvironmentOrCache) \
  V(MathExp, kStatic, kNeedsEnvironmentOrCache) \
  V(MathExpm1, kStatic, kNeedsEnvironmentOrCache) \
  V(MathHypot, kStatic, kNeedsEnvironmentOrCache) \
  V(MathLog, kStatic, kNeedsEnvironmentOrCache) \
  V(MathLog10, kStatic, kNeedsEnvironmentOrCache) \
  V(MathNextAfter, kStatic, kNeedsEnvironmentOrCache) \
  V(MathSinh, kStatic, kNeedsEnvironmentOrCache) \
  V(MathTan, kStatic, kNeedsEnvironmentOrCache) \
  V(MathTanh, kStatic, kNeedsEnvironmentOrCache) \
  V(MathSqrt, kStatic, kNeedsEnvironmentOrCache) \
  V(MathCeil, kStatic, kNeedsEnvironmentOrCache) \
  V(MathFloor, kStatic, kNeedsEnvironmentOrCache) \
  V(MathRint, kStatic, kNeedsEnvironmentOrCache) \
  V(MathRoundDouble, kStatic, kNeedsEnvironmentOrCache) \
  V(MathRoundFloat, kStatic, kNeedsEnvironmentOrCache) \
  V(SystemArrayCopyChar, kStatic, kNeedsEnvironmentOrCache) \
  V(SystemArrayCopy, kStatic, kNeedsEnvironmentOrCache) \
  V(ThreadCurrentThread, kStatic, kNeedsEnvironmentOrCache) \
  V(MemoryPeekByte, kStatic, kNeedsEnvironmentOrCache) \
  V(MemoryPeekIntNative, kStatic, kNeedsEnvironmentOrCache) \
  V(MemoryPeekLongNative, kStatic, kNeedsEnvironmentOrCache) \
  V(MemoryPeekShortNative, kStatic, kNeedsEnvironmentOrCache) \
  V(MemoryPokeByte, kStatic, kNeedsEnvironmentOrCache) \
  V(MemoryPokeIntNative, kStatic, kNeedsEnvironmentOrCache) \
  V(MemoryPokeLongNative, kStatic, kNeedsEnvironmentOrCache) \
  V(MemoryPokeShortNative, kStatic, kNeedsEnvironmentOrCache) \
  V(StringCharAt, kDirect, kNeedsEnvironmentOrCache) \
  V(StringCompareTo, kDirect, kNeedsEnvironmentOrCache) \
  V(StringEquals, kDirect, kNeedsEnvironmentOrCache) \
  V(StringGetCharsNoCheck, kDirect, kNeedsEnvironmentOrCache) \
  V(StringIndexOf, kDirect, kNeedsEnvironmentOrCache) \
  V(StringIndexOfAfter, kDirect, kNeedsEnvironmentOrCache) \
  V(StringNewStringFromBytes, kStatic, kNeedsEnvironmentOrCache) \
  V(StringNewStringFromChars, kStatic, kNeedsEnvironmentOrCache) \
  V(StringNewStringFromString, kStatic, kNeedsEnvironmentOrCache) \
  V(UnsafeCASInt, kDirect, kNeedsEnvironmentOrCache) \
  V(UnsafeCASLong, kDirect, kNeedsEnvironmentOrCache) \
  V(UnsafeCASObject, kDirect, kNeedsEnvironmentOrCache) \
  V(UnsafeGet, kDirect, kNeedsEnvironmentOrCache) \
  V(UnsafeGetVolatile, kDirect, kNeedsEnvironmentOrCache) \
  V(UnsafeGetObject, kDirect, kNeedsEnvironmentOrCache) \
  V(UnsafeGetObjectVolatile, kDirect, kNeedsEnvironmentOrCache) \
  V(UnsafeGetLong, kDirect, kNeedsEnvironmentOrCache) \
  V(UnsafeGetLongVolatile, kDirect, kNeedsEnvironmentOrCache) \
  V(UnsafePut, kDirect, kNeedsEnvironmentOrCache) \
  V(UnsafePutOrdered, kDirect, kNeedsEnvironmentOrCache) \
  V(UnsafePutVolatile, kDirect, kNeedsEnvironmentOrCache) \
  V(UnsafePutObject, kDirect, kNeedsEnvironmentOrCache) \
  V(UnsafePutObjectOrdered, kDirect, kNeedsEnvironmentOrCache) \
  V(UnsafePutObjectVolatile, kDirect, kNeedsEnvironmentOrCache) \
  V(UnsafePutLong, kDirect, kNeedsEnvironmentOrCache) \
  V(UnsafePutLongOrdered, kDirect, kNeedsEnvironmentOrCache) \
  V(UnsafePutLongVolatile, kDirect, kNeedsEnvironmentOrCache) \
  V(ReferenceGetReferent, kDirect, kNeedsEnvironmentOrCache)

#endif  // ART_COMPILER_OPTIMIZING_INTRINSICS_LIST_H_
#undef ART_COMPILER_OPTIMIZING_INTRINSICS_LIST_H_   // #define is only for lint.
