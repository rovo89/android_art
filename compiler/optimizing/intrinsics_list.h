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
// environment, may have side effects, or may throw exceptions.

#define INTRINSICS_LIST(V) \
  V(DoubleDoubleToRawLongBits, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(DoubleDoubleToLongBits, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(DoubleIsInfinite, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(DoubleIsNaN, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(DoubleLongBitsToDouble, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(FloatFloatToRawIntBits, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(FloatFloatToIntBits, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(FloatIsInfinite, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(FloatIsNaN, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(FloatIntBitsToFloat, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(IntegerReverse, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(IntegerReverseBytes, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(IntegerBitCount, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(IntegerCompare, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(IntegerHighestOneBit, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(IntegerLowestOneBit, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(IntegerNumberOfLeadingZeros, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(IntegerNumberOfTrailingZeros, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(IntegerRotateRight, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(IntegerRotateLeft, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(IntegerSignum, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(LongReverse, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(LongReverseBytes, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(LongBitCount, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(LongCompare, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(LongHighestOneBit, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(LongLowestOneBit, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(LongNumberOfLeadingZeros, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(LongNumberOfTrailingZeros, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(LongRotateRight, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(LongRotateLeft, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(LongSignum, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(ShortReverseBytes, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathAbsDouble, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathAbsFloat, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathAbsLong, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathAbsInt, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathMinDoubleDouble, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathMinFloatFloat, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathMinLongLong, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathMinIntInt, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathMaxDoubleDouble, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathMaxFloatFloat, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathMaxLongLong, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathMaxIntInt, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathCos, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathSin, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathAcos, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathAsin, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathAtan, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathAtan2, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathCbrt, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathCosh, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathExp, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathExpm1, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathHypot, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathLog, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathLog10, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathNextAfter, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathSinh, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathTan, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathTanh, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathSqrt, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathCeil, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathFloor, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathRint, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathRoundDouble, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MathRoundFloat, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(SystemArrayCopyChar, kStatic, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(SystemArrayCopy, kStatic, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(ThreadCurrentThread, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow) \
  V(MemoryPeekByte, kStatic, kNeedsEnvironmentOrCache, kReadSideEffects, kCanThrow) \
  V(MemoryPeekIntNative, kStatic, kNeedsEnvironmentOrCache, kReadSideEffects, kCanThrow) \
  V(MemoryPeekLongNative, kStatic, kNeedsEnvironmentOrCache, kReadSideEffects, kCanThrow) \
  V(MemoryPeekShortNative, kStatic, kNeedsEnvironmentOrCache, kReadSideEffects, kCanThrow) \
  V(MemoryPokeByte, kStatic, kNeedsEnvironmentOrCache, kWriteSideEffects, kCanThrow) \
  V(MemoryPokeIntNative, kStatic, kNeedsEnvironmentOrCache, kWriteSideEffects, kCanThrow) \
  V(MemoryPokeLongNative, kStatic, kNeedsEnvironmentOrCache, kWriteSideEffects, kCanThrow) \
  V(MemoryPokeShortNative, kStatic, kNeedsEnvironmentOrCache, kWriteSideEffects, kCanThrow) \
  V(StringCharAt, kDirect, kNeedsEnvironmentOrCache, kReadSideEffects, kCanThrow) \
  V(StringCompareTo, kDirect, kNeedsEnvironmentOrCache, kReadSideEffects, kCanThrow) \
  V(StringEquals, kDirect, kNeedsEnvironmentOrCache, kReadSideEffects, kCanThrow) \
  V(StringGetCharsNoCheck, kDirect, kNeedsEnvironmentOrCache, kReadSideEffects, kCanThrow) \
  V(StringIndexOf, kDirect, kNeedsEnvironmentOrCache, kReadSideEffects, kCanThrow) \
  V(StringIndexOfAfter, kDirect, kNeedsEnvironmentOrCache, kReadSideEffects, kCanThrow) \
  V(StringNewStringFromBytes, kStatic, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(StringNewStringFromChars, kStatic, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(StringNewStringFromString, kStatic, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafeCASInt, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafeCASLong, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafeCASObject, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafeGet, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafeGetVolatile, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafeGetObject, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafeGetObjectVolatile, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafeGetLong, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafeGetLongVolatile, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafePut, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafePutOrdered, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafePutVolatile, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafePutObject, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafePutObjectOrdered, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafePutObjectVolatile, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafePutLong, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafePutLongOrdered, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafePutLongVolatile, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafeGetAndAddInt, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafeGetAndAddLong, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafeGetAndSetInt, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafeGetAndSetLong, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafeGetAndSetObject, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafeLoadFence, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafeStoreFence, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(UnsafeFullFence, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow) \
  V(ReferenceGetReferent, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow)

#endif  // ART_COMPILER_OPTIMIZING_INTRINSICS_LIST_H_
#undef ART_COMPILER_OPTIMIZING_INTRINSICS_LIST_H_   // #define is only for lint.
