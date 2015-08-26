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

#include "intrinsics_mips64.h"

#include "arch/mips64/instruction_set_features_mips64.h"
#include "art_method.h"
#include "code_generator_mips64.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "intrinsics.h"
#include "mirror/array-inl.h"
#include "mirror/string.h"
#include "thread.h"
#include "utils/mips64/assembler_mips64.h"
#include "utils/mips64/constants_mips64.h"

namespace art {

namespace mips64 {

IntrinsicLocationsBuilderMIPS64::IntrinsicLocationsBuilderMIPS64(CodeGeneratorMIPS64* codegen)
  : arena_(codegen->GetGraph()->GetArena()) {
}

Mips64Assembler* IntrinsicCodeGeneratorMIPS64::GetAssembler() {
  return reinterpret_cast<Mips64Assembler*>(codegen_->GetAssembler());
}

ArenaAllocator* IntrinsicCodeGeneratorMIPS64::GetAllocator() {
  return codegen_->GetGraph()->GetArena();
}

bool IntrinsicLocationsBuilderMIPS64::TryDispatch(HInvoke* invoke) {
  Dispatch(invoke);
  LocationSummary* res = invoke->GetLocations();
  return res != nullptr && res->Intrinsified();
}

#define __ assembler->

static void CreateFPToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetOut(Location::RequiresRegister());
}

static void MoveFPToInt(LocationSummary* locations, bool is64bit, Mips64Assembler* assembler) {
  FpuRegister in  = locations->InAt(0).AsFpuRegister<FpuRegister>();
  GpuRegister out = locations->Out().AsRegister<GpuRegister>();

  if (is64bit) {
    __ Dmfc1(out, in);
  } else {
    __ Mfc1(out, in);
  }
}

// long java.lang.Double.doubleToRawLongBits(double)
void IntrinsicLocationsBuilderMIPS64::VisitDoubleDoubleToRawLongBits(HInvoke* invoke) {
  CreateFPToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitDoubleDoubleToRawLongBits(HInvoke* invoke) {
  MoveFPToInt(invoke->GetLocations(), true, GetAssembler());
}

// int java.lang.Float.floatToRawIntBits(float)
void IntrinsicLocationsBuilderMIPS64::VisitFloatFloatToRawIntBits(HInvoke* invoke) {
  CreateFPToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitFloatFloatToRawIntBits(HInvoke* invoke) {
  MoveFPToInt(invoke->GetLocations(), false, GetAssembler());
}

static void CreateIntToFPLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresFpuRegister());
}

static void MoveIntToFP(LocationSummary* locations, bool is64bit, Mips64Assembler* assembler) {
  GpuRegister in  = locations->InAt(0).AsRegister<GpuRegister>();
  FpuRegister out = locations->Out().AsFpuRegister<FpuRegister>();

  if (is64bit) {
    __ Dmtc1(in, out);
  } else {
    __ Mtc1(in, out);
  }
}

// double java.lang.Double.longBitsToDouble(long)
void IntrinsicLocationsBuilderMIPS64::VisitDoubleLongBitsToDouble(HInvoke* invoke) {
  CreateIntToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitDoubleLongBitsToDouble(HInvoke* invoke) {
  MoveIntToFP(invoke->GetLocations(), true, GetAssembler());
}

// float java.lang.Float.intBitsToFloat(int)
void IntrinsicLocationsBuilderMIPS64::VisitFloatIntBitsToFloat(HInvoke* invoke) {
  CreateIntToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitFloatIntBitsToFloat(HInvoke* invoke) {
  MoveIntToFP(invoke->GetLocations(), false, GetAssembler());
}

static void CreateIntToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

static void GenReverseBytes(LocationSummary* locations,
                            Primitive::Type type,
                            Mips64Assembler* assembler) {
  GpuRegister in  = locations->InAt(0).AsRegister<GpuRegister>();
  GpuRegister out = locations->Out().AsRegister<GpuRegister>();

  switch (type) {
    case Primitive::kPrimShort:
      __ Dsbh(out, in);
      __ Seh(out, out);
      break;
    case Primitive::kPrimInt:
      __ Rotr(out, in, 16);
      __ Wsbh(out, out);
      break;
    case Primitive::kPrimLong:
      __ Dsbh(out, in);
      __ Dshd(out, out);
      break;
    default:
      LOG(FATAL) << "Unexpected size for reverse-bytes: " << type;
      UNREACHABLE();
  }
}

// int java.lang.Integer.reverseBytes(int)
void IntrinsicLocationsBuilderMIPS64::VisitIntegerReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitIntegerReverseBytes(HInvoke* invoke) {
  GenReverseBytes(invoke->GetLocations(), Primitive::kPrimInt, GetAssembler());
}

// long java.lang.Long.reverseBytes(long)
void IntrinsicLocationsBuilderMIPS64::VisitLongReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitLongReverseBytes(HInvoke* invoke) {
  GenReverseBytes(invoke->GetLocations(), Primitive::kPrimLong, GetAssembler());
}

// short java.lang.Short.reverseBytes(short)
void IntrinsicLocationsBuilderMIPS64::VisitShortReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitShortReverseBytes(HInvoke* invoke) {
  GenReverseBytes(invoke->GetLocations(), Primitive::kPrimShort, GetAssembler());
}

static void GenCountZeroes(LocationSummary* locations, bool is64bit, Mips64Assembler* assembler) {
  GpuRegister in  = locations->InAt(0).AsRegister<GpuRegister>();
  GpuRegister out = locations->Out().AsRegister<GpuRegister>();

  if (is64bit) {
    __ Dclz(out, in);
  } else {
    __ Clz(out, in);
  }
}

// int java.lang.Integer.numberOfLeadingZeros(int i)
void IntrinsicLocationsBuilderMIPS64::VisitIntegerNumberOfLeadingZeros(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitIntegerNumberOfLeadingZeros(HInvoke* invoke) {
  GenCountZeroes(invoke->GetLocations(), false, GetAssembler());
}

// int java.lang.Long.numberOfLeadingZeros(long i)
void IntrinsicLocationsBuilderMIPS64::VisitLongNumberOfLeadingZeros(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitLongNumberOfLeadingZeros(HInvoke* invoke) {
  GenCountZeroes(invoke->GetLocations(), true, GetAssembler());
}

static void GenReverse(LocationSummary* locations,
                       Primitive::Type type,
                       Mips64Assembler* assembler) {
  DCHECK(type == Primitive::kPrimInt || type == Primitive::kPrimLong);

  GpuRegister in  = locations->InAt(0).AsRegister<GpuRegister>();
  GpuRegister out = locations->Out().AsRegister<GpuRegister>();

  if (type == Primitive::kPrimInt) {
    __ Rotr(out, in, 16);
    __ Wsbh(out, out);
    __ Bitswap(out, out);
  } else {
    __ Dsbh(out, in);
    __ Dshd(out, out);
    __ Dbitswap(out, out);
  }
}

// int java.lang.Integer.reverse(int)
void IntrinsicLocationsBuilderMIPS64::VisitIntegerReverse(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitIntegerReverse(HInvoke* invoke) {
  GenReverse(invoke->GetLocations(), Primitive::kPrimInt, GetAssembler());
}

// long java.lang.Long.reverse(long)
void IntrinsicLocationsBuilderMIPS64::VisitLongReverse(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitLongReverse(HInvoke* invoke) {
  GenReverse(invoke->GetLocations(), Primitive::kPrimLong, GetAssembler());
}

// Unimplemented intrinsics.

#define UNIMPLEMENTED_INTRINSIC(Name)                                                  \
void IntrinsicLocationsBuilderMIPS64::Visit ## Name(HInvoke* invoke ATTRIBUTE_UNUSED) { \
}                                                                                      \
void IntrinsicCodeGeneratorMIPS64::Visit ## Name(HInvoke* invoke ATTRIBUTE_UNUSED) {    \
}

UNIMPLEMENTED_INTRINSIC(MathAbsDouble)
UNIMPLEMENTED_INTRINSIC(MathAbsFloat)
UNIMPLEMENTED_INTRINSIC(MathAbsInt)
UNIMPLEMENTED_INTRINSIC(MathAbsLong)
UNIMPLEMENTED_INTRINSIC(MathMinDoubleDouble)
UNIMPLEMENTED_INTRINSIC(MathMinFloatFloat)
UNIMPLEMENTED_INTRINSIC(MathMaxDoubleDouble)
UNIMPLEMENTED_INTRINSIC(MathMaxFloatFloat)
UNIMPLEMENTED_INTRINSIC(MathMinIntInt)
UNIMPLEMENTED_INTRINSIC(MathMinLongLong)
UNIMPLEMENTED_INTRINSIC(MathMaxIntInt)
UNIMPLEMENTED_INTRINSIC(MathMaxLongLong)
UNIMPLEMENTED_INTRINSIC(MathSqrt)
UNIMPLEMENTED_INTRINSIC(MathCeil)
UNIMPLEMENTED_INTRINSIC(MathFloor)
UNIMPLEMENTED_INTRINSIC(MathRint)
UNIMPLEMENTED_INTRINSIC(MathRoundDouble)
UNIMPLEMENTED_INTRINSIC(MathRoundFloat)
UNIMPLEMENTED_INTRINSIC(MemoryPeekByte)
UNIMPLEMENTED_INTRINSIC(MemoryPeekIntNative)
UNIMPLEMENTED_INTRINSIC(MemoryPeekLongNative)
UNIMPLEMENTED_INTRINSIC(MemoryPeekShortNative)
UNIMPLEMENTED_INTRINSIC(MemoryPokeByte)
UNIMPLEMENTED_INTRINSIC(MemoryPokeIntNative)
UNIMPLEMENTED_INTRINSIC(MemoryPokeLongNative)
UNIMPLEMENTED_INTRINSIC(MemoryPokeShortNative)
UNIMPLEMENTED_INTRINSIC(ThreadCurrentThread)
UNIMPLEMENTED_INTRINSIC(UnsafeGet)
UNIMPLEMENTED_INTRINSIC(UnsafeGetVolatile)
UNIMPLEMENTED_INTRINSIC(UnsafeGetLong)
UNIMPLEMENTED_INTRINSIC(UnsafeGetLongVolatile)
UNIMPLEMENTED_INTRINSIC(UnsafeGetObject)
UNIMPLEMENTED_INTRINSIC(UnsafeGetObjectVolatile)
UNIMPLEMENTED_INTRINSIC(UnsafePut)
UNIMPLEMENTED_INTRINSIC(UnsafePutOrdered)
UNIMPLEMENTED_INTRINSIC(UnsafePutVolatile)
UNIMPLEMENTED_INTRINSIC(UnsafePutObject)
UNIMPLEMENTED_INTRINSIC(UnsafePutObjectOrdered)
UNIMPLEMENTED_INTRINSIC(UnsafePutObjectVolatile)
UNIMPLEMENTED_INTRINSIC(UnsafePutLong)
UNIMPLEMENTED_INTRINSIC(UnsafePutLongOrdered)
UNIMPLEMENTED_INTRINSIC(UnsafePutLongVolatile)
UNIMPLEMENTED_INTRINSIC(UnsafeCASInt)
UNIMPLEMENTED_INTRINSIC(UnsafeCASLong)
UNIMPLEMENTED_INTRINSIC(UnsafeCASObject)
UNIMPLEMENTED_INTRINSIC(StringCharAt)
UNIMPLEMENTED_INTRINSIC(StringCompareTo)
UNIMPLEMENTED_INTRINSIC(StringEquals)
UNIMPLEMENTED_INTRINSIC(StringIndexOf)
UNIMPLEMENTED_INTRINSIC(StringIndexOfAfter)
UNIMPLEMENTED_INTRINSIC(StringNewStringFromBytes)
UNIMPLEMENTED_INTRINSIC(StringNewStringFromChars)
UNIMPLEMENTED_INTRINSIC(StringNewStringFromString)
UNIMPLEMENTED_INTRINSIC(LongRotateLeft)
UNIMPLEMENTED_INTRINSIC(LongRotateRight)
UNIMPLEMENTED_INTRINSIC(LongNumberOfTrailingZeros)
UNIMPLEMENTED_INTRINSIC(IntegerRotateLeft)
UNIMPLEMENTED_INTRINSIC(IntegerRotateRight)
UNIMPLEMENTED_INTRINSIC(IntegerNumberOfTrailingZeros)

UNIMPLEMENTED_INTRINSIC(ReferenceGetReferent)
UNIMPLEMENTED_INTRINSIC(StringGetCharsNoCheck)
UNIMPLEMENTED_INTRINSIC(SystemArrayCopyChar)

#undef UNIMPLEMENTED_INTRINSIC

#undef __

}  // namespace mips64
}  // namespace art
