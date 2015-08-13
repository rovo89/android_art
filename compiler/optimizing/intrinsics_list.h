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
  V(DoubleDoubleToRawLongBits, kStatic, kNeedsEnvironment) \
  V(DoubleLongBitsToDouble, kStatic, kNeedsEnvironment) \
  V(FloatFloatToRawIntBits, kStatic, kNeedsEnvironment) \
  V(FloatIntBitsToFloat, kStatic, kNeedsEnvironment) \
  V(IntegerReverse, kStatic, kNeedsEnvironment) \
  V(IntegerReverseBytes, kStatic, kNeedsEnvironment) \
  V(IntegerNumberOfLeadingZeros, kStatic, kNeedsEnvironment) \
  V(LongReverse, kStatic, kNeedsEnvironment) \
  V(LongReverseBytes, kStatic, kNeedsEnvironment) \
  V(LongNumberOfLeadingZeros, kStatic, kNeedsEnvironment) \
  V(ShortReverseBytes, kStatic, kNeedsEnvironment) \
  V(MathAbsDouble, kStatic, kNeedsEnvironment) \
  V(MathAbsFloat, kStatic, kNeedsEnvironment) \
  V(MathAbsLong, kStatic, kNeedsEnvironment) \
  V(MathAbsInt, kStatic, kNeedsEnvironment) \
  V(MathMinDoubleDouble, kStatic, kNeedsEnvironment) \
  V(MathMinFloatFloat, kStatic, kNeedsEnvironment) \
  V(MathMinLongLong, kStatic, kNeedsEnvironment) \
  V(MathMinIntInt, kStatic, kNeedsEnvironment) \
  V(MathMaxDoubleDouble, kStatic, kNeedsEnvironment) \
  V(MathMaxFloatFloat, kStatic, kNeedsEnvironment) \
  V(MathMaxLongLong, kStatic, kNeedsEnvironment) \
  V(MathMaxIntInt, kStatic, kNeedsEnvironment) \
  V(MathSqrt, kStatic, kNeedsEnvironment) \
  V(MathCeil, kStatic, kNeedsEnvironment) \
  V(MathFloor, kStatic, kNeedsEnvironment) \
  V(MathRint, kStatic, kNeedsEnvironment) \
  V(MathRoundDouble, kStatic, kNeedsEnvironment) \
  V(MathRoundFloat, kStatic, kNeedsEnvironment) \
  V(SystemArrayCopyChar, kStatic, kNeedsEnvironment) \
  V(ThreadCurrentThread, kStatic, kNeedsEnvironment) \
  V(MemoryPeekByte, kStatic, kNeedsEnvironment) \
  V(MemoryPeekIntNative, kStatic, kNeedsEnvironment) \
  V(MemoryPeekLongNative, kStatic, kNeedsEnvironment) \
  V(MemoryPeekShortNative, kStatic, kNeedsEnvironment) \
  V(MemoryPokeByte, kStatic, kNeedsEnvironment) \
  V(MemoryPokeIntNative, kStatic, kNeedsEnvironment) \
  V(MemoryPokeLongNative, kStatic, kNeedsEnvironment) \
  V(MemoryPokeShortNative, kStatic, kNeedsEnvironment) \
  V(StringCharAt, kDirect, kNeedsEnvironment) \
  V(StringCompareTo, kDirect, kNeedsEnvironment) \
  V(StringEquals, kDirect, kNeedsEnvironment) \
  V(StringGetCharsNoCheck, kDirect, kNeedsEnvironment) \
  V(StringIndexOf, kDirect, kNeedsEnvironment) \
  V(StringIndexOfAfter, kDirect, kNeedsEnvironment) \
  V(StringNewStringFromBytes, kStatic, kNeedsEnvironment) \
  V(StringNewStringFromChars, kStatic, kNeedsEnvironment) \
  V(StringNewStringFromString, kStatic, kNeedsEnvironment) \
  V(UnsafeCASInt, kDirect, kNeedsEnvironment) \
  V(UnsafeCASLong, kDirect, kNeedsEnvironment) \
  V(UnsafeCASObject, kDirect, kNeedsEnvironment) \
  V(UnsafeGet, kDirect, kNeedsEnvironment) \
  V(UnsafeGetVolatile, kDirect, kNeedsEnvironment) \
  V(UnsafeGetObject, kDirect, kNeedsEnvironment) \
  V(UnsafeGetObjectVolatile, kDirect, kNeedsEnvironment) \
  V(UnsafeGetLong, kDirect, kNeedsEnvironment) \
  V(UnsafeGetLongVolatile, kDirect, kNeedsEnvironment) \
  V(UnsafePut, kDirect, kNeedsEnvironment) \
  V(UnsafePutOrdered, kDirect, kNeedsEnvironment) \
  V(UnsafePutVolatile, kDirect, kNeedsEnvironment) \
  V(UnsafePutObject, kDirect, kNeedsEnvironment) \
  V(UnsafePutObjectOrdered, kDirect, kNeedsEnvironment) \
  V(UnsafePutObjectVolatile, kDirect, kNeedsEnvironment) \
  V(UnsafePutLong, kDirect, kNeedsEnvironment) \
  V(UnsafePutLongOrdered, kDirect, kNeedsEnvironment) \
  V(UnsafePutLongVolatile, kDirect, kNeedsEnvironment) \
  V(ReferenceGetReferent, kDirect, kNeedsEnvironment)

#endif  // ART_COMPILER_OPTIMIZING_INTRINSICS_LIST_H_
#undef ART_COMPILER_OPTIMIZING_INTRINSICS_LIST_H_   // #define is only for lint.
