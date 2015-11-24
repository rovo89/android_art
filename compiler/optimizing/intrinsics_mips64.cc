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

#define __ codegen->GetAssembler()->

static void MoveFromReturnRegister(Location trg,
                                   Primitive::Type type,
                                   CodeGeneratorMIPS64* codegen) {
  if (!trg.IsValid()) {
    DCHECK_EQ(type, Primitive::kPrimVoid);
    return;
  }

  DCHECK_NE(type, Primitive::kPrimVoid);

  if (Primitive::IsIntegralType(type) || type == Primitive::kPrimNot) {
    GpuRegister trg_reg = trg.AsRegister<GpuRegister>();
    if (trg_reg != V0) {
      __ Move(V0, trg_reg);
    }
  } else {
    FpuRegister trg_reg = trg.AsFpuRegister<FpuRegister>();
    if (trg_reg != F0) {
      if (type == Primitive::kPrimFloat) {
        __ MovS(F0, trg_reg);
      } else {
        __ MovD(F0, trg_reg);
      }
    }
  }
}

static void MoveArguments(HInvoke* invoke, CodeGeneratorMIPS64* codegen) {
  InvokeDexCallingConventionVisitorMIPS64 calling_convention_visitor;
  IntrinsicVisitor::MoveArguments(invoke, codegen, &calling_convention_visitor);
}

// Slow-path for fallback (calling the managed code to handle the
// intrinsic) in an intrinsified call. This will copy the arguments
// into the positions for a regular call.
//
// Note: The actual parameters are required to be in the locations
//       given by the invoke's location summary. If an intrinsic
//       modifies those locations before a slowpath call, they must be
//       restored!
class IntrinsicSlowPathMIPS64 : public SlowPathCodeMIPS64 {
 public:
  explicit IntrinsicSlowPathMIPS64(HInvoke* invoke) : invoke_(invoke) { }

  void EmitNativeCode(CodeGenerator* codegen_in) OVERRIDE {
    CodeGeneratorMIPS64* codegen = down_cast<CodeGeneratorMIPS64*>(codegen_in);

    __ Bind(GetEntryLabel());

    SaveLiveRegisters(codegen, invoke_->GetLocations());

    MoveArguments(invoke_, codegen);

    if (invoke_->IsInvokeStaticOrDirect()) {
      codegen->GenerateStaticOrDirectCall(invoke_->AsInvokeStaticOrDirect(),
                                          Location::RegisterLocation(A0));
    } else {
      codegen->GenerateVirtualCall(invoke_->AsInvokeVirtual(), Location::RegisterLocation(A0));
    }
    codegen->RecordPcInfo(invoke_, invoke_->GetDexPc(), this);

    // Copy the result back to the expected output.
    Location out = invoke_->GetLocations()->Out();
    if (out.IsValid()) {
      DCHECK(out.IsRegister());  // TODO: Replace this when we support output in memory.
      DCHECK(!invoke_->GetLocations()->GetLiveRegisters()->ContainsCoreRegister(out.reg()));
      MoveFromReturnRegister(out, invoke_->GetType(), codegen);
    }

    RestoreLiveRegisters(codegen, invoke_->GetLocations());
    __ Bc(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "IntrinsicSlowPathMIPS64"; }

 private:
  // The instruction where this slow path is happening.
  HInvoke* const invoke_;

  DISALLOW_COPY_AND_ASSIGN(IntrinsicSlowPathMIPS64);
};

#undef __

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

static void GenNumberOfLeadingZeroes(LocationSummary* locations,
                                     bool is64bit,
                                     Mips64Assembler* assembler) {
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
  GenNumberOfLeadingZeroes(invoke->GetLocations(), false, GetAssembler());
}

// int java.lang.Long.numberOfLeadingZeros(long i)
void IntrinsicLocationsBuilderMIPS64::VisitLongNumberOfLeadingZeros(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitLongNumberOfLeadingZeros(HInvoke* invoke) {
  GenNumberOfLeadingZeroes(invoke->GetLocations(), true, GetAssembler());
}

static void GenNumberOfTrailingZeroes(LocationSummary* locations,
                                      bool is64bit,
                                      Mips64Assembler* assembler) {
  Location in = locations->InAt(0);
  Location out = locations->Out();

  if (is64bit) {
    __ Dsbh(out.AsRegister<GpuRegister>(), in.AsRegister<GpuRegister>());
    __ Dshd(out.AsRegister<GpuRegister>(), out.AsRegister<GpuRegister>());
    __ Dbitswap(out.AsRegister<GpuRegister>(), out.AsRegister<GpuRegister>());
    __ Dclz(out.AsRegister<GpuRegister>(), out.AsRegister<GpuRegister>());
  } else {
    __ Rotr(out.AsRegister<GpuRegister>(), in.AsRegister<GpuRegister>(), 16);
    __ Wsbh(out.AsRegister<GpuRegister>(), out.AsRegister<GpuRegister>());
    __ Bitswap(out.AsRegister<GpuRegister>(), out.AsRegister<GpuRegister>());
    __ Clz(out.AsRegister<GpuRegister>(), out.AsRegister<GpuRegister>());
  }
}

// int java.lang.Integer.numberOfTrailingZeros(int i)
void IntrinsicLocationsBuilderMIPS64::VisitIntegerNumberOfTrailingZeros(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitIntegerNumberOfTrailingZeros(HInvoke* invoke) {
  GenNumberOfTrailingZeroes(invoke->GetLocations(), false, GetAssembler());
}

// int java.lang.Long.numberOfTrailingZeros(long i)
void IntrinsicLocationsBuilderMIPS64::VisitLongNumberOfTrailingZeros(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitLongNumberOfTrailingZeros(HInvoke* invoke) {
  GenNumberOfTrailingZeroes(invoke->GetLocations(), true, GetAssembler());
}

static void GenRotateRight(HInvoke* invoke,
                           Primitive::Type type,
                           Mips64Assembler* assembler) {
  DCHECK(type == Primitive::kPrimInt || type == Primitive::kPrimLong);

  LocationSummary* locations = invoke->GetLocations();
  GpuRegister in = locations->InAt(0).AsRegister<GpuRegister>();
  GpuRegister out = locations->Out().AsRegister<GpuRegister>();

  if (invoke->InputAt(1)->IsIntConstant()) {
    uint32_t shift = static_cast<uint32_t>(invoke->InputAt(1)->AsIntConstant()->GetValue());
    if (type == Primitive::kPrimInt) {
      shift &= 0x1f;
      __ Rotr(out, in, shift);
    } else {
      shift &= 0x3f;
      if (shift < 32) {
        __ Drotr(out, in, shift);
      } else {
        shift &= 0x1f;
        __ Drotr32(out, in, shift);
      }
    }
  } else {
    GpuRegister shamt = locations->InAt(1).AsRegister<GpuRegister>();
    if (type == Primitive::kPrimInt) {
      __ Rotrv(out, in, shamt);
    } else {
      __ Drotrv(out, in, shamt);
    }
  }
}

// int java.lang.Integer.rotateRight(int i, int distance)
void IntrinsicLocationsBuilderMIPS64::VisitIntegerRotateRight(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(invoke->InputAt(1)));
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void IntrinsicCodeGeneratorMIPS64::VisitIntegerRotateRight(HInvoke* invoke) {
  GenRotateRight(invoke, Primitive::kPrimInt, GetAssembler());
}

// long java.lang.Long.rotateRight(long i, int distance)
void IntrinsicLocationsBuilderMIPS64::VisitLongRotateRight(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(invoke->InputAt(1)));
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void IntrinsicCodeGeneratorMIPS64::VisitLongRotateRight(HInvoke* invoke) {
  GenRotateRight(invoke, Primitive::kPrimLong, GetAssembler());
}

static void GenRotateLeft(HInvoke* invoke,
                           Primitive::Type type,
                           Mips64Assembler* assembler) {
  DCHECK(type == Primitive::kPrimInt || type == Primitive::kPrimLong);

  LocationSummary* locations = invoke->GetLocations();
  GpuRegister in = locations->InAt(0).AsRegister<GpuRegister>();
  GpuRegister out = locations->Out().AsRegister<GpuRegister>();

  if (invoke->InputAt(1)->IsIntConstant()) {
    int32_t shift = -static_cast<int32_t>(invoke->InputAt(1)->AsIntConstant()->GetValue());
    if (type == Primitive::kPrimInt) {
      shift &= 0x1f;
      __ Rotr(out, in, shift);
    } else {
      shift &= 0x3f;
      if (shift < 32) {
        __ Drotr(out, in, shift);
      } else {
        shift &= 0x1f;
        __ Drotr32(out, in, shift);
      }
    }
  } else {
    GpuRegister shamt = locations->InAt(1).AsRegister<GpuRegister>();
    if (type == Primitive::kPrimInt) {
      __ Subu(TMP, ZERO, shamt);
      __ Rotrv(out, in, TMP);
    } else {
      __ Dsubu(TMP, ZERO, shamt);
      __ Drotrv(out, in, TMP);
    }
  }
}

// int java.lang.Integer.rotateLeft(int i, int distance)
void IntrinsicLocationsBuilderMIPS64::VisitIntegerRotateLeft(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(invoke->InputAt(1)));
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void IntrinsicCodeGeneratorMIPS64::VisitIntegerRotateLeft(HInvoke* invoke) {
  GenRotateLeft(invoke, Primitive::kPrimInt, GetAssembler());
}

// long java.lang.Long.rotateLeft(long i, int distance)
void IntrinsicLocationsBuilderMIPS64::VisitLongRotateLeft(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(invoke->InputAt(1)));
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void IntrinsicCodeGeneratorMIPS64::VisitLongRotateLeft(HInvoke* invoke) {
  GenRotateLeft(invoke, Primitive::kPrimLong, GetAssembler());
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

static void CreateFPToFPLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
}

static void MathAbsFP(LocationSummary* locations, bool is64bit, Mips64Assembler* assembler) {
  FpuRegister in = locations->InAt(0).AsFpuRegister<FpuRegister>();
  FpuRegister out = locations->Out().AsFpuRegister<FpuRegister>();

  if (is64bit) {
    __ AbsD(out, in);
  } else {
    __ AbsS(out, in);
  }
}

// double java.lang.Math.abs(double)
void IntrinsicLocationsBuilderMIPS64::VisitMathAbsDouble(HInvoke* invoke) {
  CreateFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMathAbsDouble(HInvoke* invoke) {
  MathAbsFP(invoke->GetLocations(), true, GetAssembler());
}

// float java.lang.Math.abs(float)
void IntrinsicLocationsBuilderMIPS64::VisitMathAbsFloat(HInvoke* invoke) {
  CreateFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMathAbsFloat(HInvoke* invoke) {
  MathAbsFP(invoke->GetLocations(), false, GetAssembler());
}

static void CreateIntToInt(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

static void GenAbsInteger(LocationSummary* locations, bool is64bit, Mips64Assembler* assembler) {
  GpuRegister in  = locations->InAt(0).AsRegister<GpuRegister>();
  GpuRegister out = locations->Out().AsRegister<GpuRegister>();

  if (is64bit) {
    __ Dsra32(AT, in, 31);
    __ Xor(out, in, AT);
    __ Dsubu(out, out, AT);
  } else {
    __ Sra(AT, in, 31);
    __ Xor(out, in, AT);
    __ Subu(out, out, AT);
  }
}

// int java.lang.Math.abs(int)
void IntrinsicLocationsBuilderMIPS64::VisitMathAbsInt(HInvoke* invoke) {
  CreateIntToInt(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMathAbsInt(HInvoke* invoke) {
  GenAbsInteger(invoke->GetLocations(), false, GetAssembler());
}

// long java.lang.Math.abs(long)
void IntrinsicLocationsBuilderMIPS64::VisitMathAbsLong(HInvoke* invoke) {
  CreateIntToInt(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMathAbsLong(HInvoke* invoke) {
  GenAbsInteger(invoke->GetLocations(), true, GetAssembler());
}

static void GenMinMaxFP(LocationSummary* locations,
                        bool is_min,
                        bool is_double,
                        Mips64Assembler* assembler) {
  FpuRegister lhs = locations->InAt(0).AsFpuRegister<FpuRegister>();
  FpuRegister rhs = locations->InAt(1).AsFpuRegister<FpuRegister>();
  FpuRegister out = locations->Out().AsFpuRegister<FpuRegister>();

  if (is_double) {
    if (is_min) {
      __ MinD(out, lhs, rhs);
    } else {
      __ MaxD(out, lhs, rhs);
    }
  } else {
    if (is_min) {
      __ MinS(out, lhs, rhs);
    } else {
      __ MaxS(out, lhs, rhs);
    }
  }
}

static void CreateFPFPToFPLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetInAt(1, Location::RequiresFpuRegister());
  locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
}

// double java.lang.Math.min(double, double)
void IntrinsicLocationsBuilderMIPS64::VisitMathMinDoubleDouble(HInvoke* invoke) {
  CreateFPFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMathMinDoubleDouble(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(), true, true, GetAssembler());
}

// float java.lang.Math.min(float, float)
void IntrinsicLocationsBuilderMIPS64::VisitMathMinFloatFloat(HInvoke* invoke) {
  CreateFPFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMathMinFloatFloat(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(), true, false, GetAssembler());
}

// double java.lang.Math.max(double, double)
void IntrinsicLocationsBuilderMIPS64::VisitMathMaxDoubleDouble(HInvoke* invoke) {
  CreateFPFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMathMaxDoubleDouble(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(), false, true, GetAssembler());
}

// float java.lang.Math.max(float, float)
void IntrinsicLocationsBuilderMIPS64::VisitMathMaxFloatFloat(HInvoke* invoke) {
  CreateFPFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMathMaxFloatFloat(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(), false, false, GetAssembler());
}

static void GenMinMax(LocationSummary* locations,
                      bool is_min,
                      Mips64Assembler* assembler) {
  GpuRegister lhs = locations->InAt(0).AsRegister<GpuRegister>();
  GpuRegister rhs = locations->InAt(1).AsRegister<GpuRegister>();
  GpuRegister out = locations->Out().AsRegister<GpuRegister>();

  // Some architectures, such as ARM and MIPS (prior to r6), have a
  // conditional move instruction which only changes the target
  // (output) register if the condition is true (MIPS prior to r6 had
  // MOVF, MOVT, and MOVZ). The SELEQZ and SELNEZ instructions always
  // change the target (output) register.  If the condition is true the
  // output register gets the contents of the "rs" register; otherwise,
  // the output register is set to zero. One consequence of this is
  // that to implement something like "rd = c==0 ? rs : rt" MIPS64r6
  // needs to use a pair of SELEQZ/SELNEZ instructions.  After
  // executing this pair of instructions one of the output registers
  // from the pair will necessarily contain zero. Then the code ORs the
  // output registers from the SELEQZ/SELNEZ instructions to get the
  // final result.
  //
  // The initial test to see if the output register is same as the
  // first input register is needed to make sure that value in the
  // first input register isn't clobbered before we've finished
  // computing the output value. The logic in the corresponding else
  // clause performs the same task but makes sure the second input
  // register isn't clobbered in the event that it's the same register
  // as the output register; the else clause also handles the case
  // where the output register is distinct from both the first, and the
  // second input registers.
  if (out == lhs) {
    __ Slt(AT, rhs, lhs);
    if (is_min) {
      __ Seleqz(out, lhs, AT);
      __ Selnez(AT, rhs, AT);
    } else {
      __ Selnez(out, lhs, AT);
      __ Seleqz(AT, rhs, AT);
    }
  } else {
    __ Slt(AT, lhs, rhs);
    if (is_min) {
      __ Seleqz(out, rhs, AT);
      __ Selnez(AT, lhs, AT);
    } else {
      __ Selnez(out, rhs, AT);
      __ Seleqz(AT, lhs, AT);
    }
  }
  __ Or(out, out, AT);
}

static void CreateIntIntToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

// int java.lang.Math.min(int, int)
void IntrinsicLocationsBuilderMIPS64::VisitMathMinIntInt(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMathMinIntInt(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), true, GetAssembler());
}

// long java.lang.Math.min(long, long)
void IntrinsicLocationsBuilderMIPS64::VisitMathMinLongLong(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMathMinLongLong(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), true, GetAssembler());
}

// int java.lang.Math.max(int, int)
void IntrinsicLocationsBuilderMIPS64::VisitMathMaxIntInt(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMathMaxIntInt(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), false, GetAssembler());
}

// long java.lang.Math.max(long, long)
void IntrinsicLocationsBuilderMIPS64::VisitMathMaxLongLong(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMathMaxLongLong(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), false, GetAssembler());
}

// double java.lang.Math.sqrt(double)
void IntrinsicLocationsBuilderMIPS64::VisitMathSqrt(HInvoke* invoke) {
  CreateFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMathSqrt(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  Mips64Assembler* assembler = GetAssembler();
  FpuRegister in = locations->InAt(0).AsFpuRegister<FpuRegister>();
  FpuRegister out = locations->Out().AsFpuRegister<FpuRegister>();

  __ SqrtD(out, in);
}

static void CreateFPToFP(ArenaAllocator* arena,
                         HInvoke* invoke,
                         Location::OutputOverlap overlaps = Location::kOutputOverlap) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetOut(Location::RequiresFpuRegister(), overlaps);
}

// double java.lang.Math.rint(double)
void IntrinsicLocationsBuilderMIPS64::VisitMathRint(HInvoke* invoke) {
  CreateFPToFP(arena_, invoke, Location::kNoOutputOverlap);
}

void IntrinsicCodeGeneratorMIPS64::VisitMathRint(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  Mips64Assembler* assembler = GetAssembler();
  FpuRegister in = locations->InAt(0).AsFpuRegister<FpuRegister>();
  FpuRegister out = locations->Out().AsFpuRegister<FpuRegister>();

  __ RintD(out, in);
}

// double java.lang.Math.floor(double)
void IntrinsicLocationsBuilderMIPS64::VisitMathFloor(HInvoke* invoke) {
  CreateFPToFP(arena_, invoke);
}

const constexpr uint16_t kFPLeaveUnchanged = kPositiveZero |
                                             kPositiveInfinity |
                                             kNegativeZero |
                                             kNegativeInfinity |
                                             kQuietNaN |
                                             kSignalingNaN;

enum FloatRoundingMode {
  kFloor,
  kCeil,
};

static void GenRoundingMode(LocationSummary* locations,
                            FloatRoundingMode mode,
                            Mips64Assembler* assembler) {
  FpuRegister in = locations->InAt(0).AsFpuRegister<FpuRegister>();
  FpuRegister out = locations->Out().AsFpuRegister<FpuRegister>();

  DCHECK_NE(in, out);

  Mips64Label done;

  // double floor/ceil(double in) {
  //     if in.isNaN || in.isInfinite || in.isZero {
  //         return in;
  //     }
  __ ClassD(out, in);
  __ Dmfc1(AT, out);
  __ Andi(AT, AT, kFPLeaveUnchanged);   // +0.0 | +Inf | -0.0 | -Inf | qNaN | sNaN
  __ MovD(out, in);
  __ Bnezc(AT, &done);

  //     Long outLong = floor/ceil(in);
  //     if outLong == Long.MAX_VALUE {
  //         // floor()/ceil() has almost certainly returned a value
  //         // which can't be successfully represented as a signed
  //         // 64-bit number.  Java expects that the input value will
  //         // be returned in these cases.
  //         // There is also a small probability that floor(in)/ceil(in)
  //         // correctly truncates/rounds up the input value to
  //         // Long.MAX_VALUE.  In that case, this exception handling
  //         // code still does the correct thing.
  //         return in;
  //     }
  if (mode == kFloor) {
    __ FloorLD(out, in);
  } else  if (mode == kCeil) {
    __ CeilLD(out, in);
  }
  __ Dmfc1(AT, out);
  __ MovD(out, in);
  __ LoadConst64(TMP, kPrimLongMax);
  __ Beqc(AT, TMP, &done);

  //     double out = outLong;
  //     return out;
  __ Dmtc1(AT, out);
  __ Cvtdl(out, out);
  __ Bind(&done);
  // }
}

void IntrinsicCodeGeneratorMIPS64::VisitMathFloor(HInvoke* invoke) {
  GenRoundingMode(invoke->GetLocations(), kFloor, GetAssembler());
}

// double java.lang.Math.ceil(double)
void IntrinsicLocationsBuilderMIPS64::VisitMathCeil(HInvoke* invoke) {
  CreateFPToFP(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMathCeil(HInvoke* invoke) {
  GenRoundingMode(invoke->GetLocations(), kCeil, GetAssembler());
}

// byte libcore.io.Memory.peekByte(long address)
void IntrinsicLocationsBuilderMIPS64::VisitMemoryPeekByte(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMemoryPeekByte(HInvoke* invoke) {
  Mips64Assembler* assembler = GetAssembler();
  GpuRegister adr = invoke->GetLocations()->InAt(0).AsRegister<GpuRegister>();
  GpuRegister out = invoke->GetLocations()->Out().AsRegister<GpuRegister>();

  __ Lb(out, adr, 0);
}

// short libcore.io.Memory.peekShort(long address)
void IntrinsicLocationsBuilderMIPS64::VisitMemoryPeekShortNative(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMemoryPeekShortNative(HInvoke* invoke) {
  Mips64Assembler* assembler = GetAssembler();
  GpuRegister adr = invoke->GetLocations()->InAt(0).AsRegister<GpuRegister>();
  GpuRegister out = invoke->GetLocations()->Out().AsRegister<GpuRegister>();

  __ Lh(out, adr, 0);
}

// int libcore.io.Memory.peekInt(long address)
void IntrinsicLocationsBuilderMIPS64::VisitMemoryPeekIntNative(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMemoryPeekIntNative(HInvoke* invoke) {
  Mips64Assembler* assembler = GetAssembler();
  GpuRegister adr = invoke->GetLocations()->InAt(0).AsRegister<GpuRegister>();
  GpuRegister out = invoke->GetLocations()->Out().AsRegister<GpuRegister>();

  __ Lw(out, adr, 0);
}

// long libcore.io.Memory.peekLong(long address)
void IntrinsicLocationsBuilderMIPS64::VisitMemoryPeekLongNative(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMemoryPeekLongNative(HInvoke* invoke) {
  Mips64Assembler* assembler = GetAssembler();
  GpuRegister adr = invoke->GetLocations()->InAt(0).AsRegister<GpuRegister>();
  GpuRegister out = invoke->GetLocations()->Out().AsRegister<GpuRegister>();

  __ Ld(out, adr, 0);
}

static void CreateIntIntToVoidLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
}

// void libcore.io.Memory.pokeByte(long address, byte value)
void IntrinsicLocationsBuilderMIPS64::VisitMemoryPokeByte(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMemoryPokeByte(HInvoke* invoke) {
  Mips64Assembler* assembler = GetAssembler();
  GpuRegister adr = invoke->GetLocations()->InAt(0).AsRegister<GpuRegister>();
  GpuRegister val = invoke->GetLocations()->InAt(1).AsRegister<GpuRegister>();

  __ Sb(val, adr, 0);
}

// void libcore.io.Memory.pokeShort(long address, short value)
void IntrinsicLocationsBuilderMIPS64::VisitMemoryPokeShortNative(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMemoryPokeShortNative(HInvoke* invoke) {
  Mips64Assembler* assembler = GetAssembler();
  GpuRegister adr = invoke->GetLocations()->InAt(0).AsRegister<GpuRegister>();
  GpuRegister val = invoke->GetLocations()->InAt(1).AsRegister<GpuRegister>();

  __ Sh(val, adr, 0);
}

// void libcore.io.Memory.pokeInt(long address, int value)
void IntrinsicLocationsBuilderMIPS64::VisitMemoryPokeIntNative(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMemoryPokeIntNative(HInvoke* invoke) {
  Mips64Assembler* assembler = GetAssembler();
  GpuRegister adr = invoke->GetLocations()->InAt(0).AsRegister<GpuRegister>();
  GpuRegister val = invoke->GetLocations()->InAt(1).AsRegister<GpuRegister>();

  __ Sw(val, adr, 00);
}

// void libcore.io.Memory.pokeLong(long address, long value)
void IntrinsicLocationsBuilderMIPS64::VisitMemoryPokeLongNative(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitMemoryPokeLongNative(HInvoke* invoke) {
  Mips64Assembler* assembler = GetAssembler();
  GpuRegister adr = invoke->GetLocations()->InAt(0).AsRegister<GpuRegister>();
  GpuRegister val = invoke->GetLocations()->InAt(1).AsRegister<GpuRegister>();

  __ Sd(val, adr, 0);
}

// Thread java.lang.Thread.currentThread()
void IntrinsicLocationsBuilderMIPS64::VisitThreadCurrentThread(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetOut(Location::RequiresRegister());
}

void IntrinsicCodeGeneratorMIPS64::VisitThreadCurrentThread(HInvoke* invoke) {
  Mips64Assembler* assembler = GetAssembler();
  GpuRegister out = invoke->GetLocations()->Out().AsRegister<GpuRegister>();

  __ LoadFromOffset(kLoadUnsignedWord,
                    out,
                    TR,
                    Thread::PeerOffset<kMips64PointerSize>().Int32Value());
}

static void CreateIntIntIntToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::NoLocation());        // Unused receiver.
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetInAt(2, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

static void GenUnsafeGet(HInvoke* invoke,
                         Primitive::Type type,
                         bool is_volatile,
                         CodeGeneratorMIPS64* codegen) {
  LocationSummary* locations = invoke->GetLocations();
  DCHECK((type == Primitive::kPrimInt) ||
         (type == Primitive::kPrimLong) ||
         (type == Primitive::kPrimNot));
  Mips64Assembler* assembler = codegen->GetAssembler();
  // Object pointer.
  GpuRegister base = locations->InAt(1).AsRegister<GpuRegister>();
  // Long offset.
  GpuRegister offset = locations->InAt(2).AsRegister<GpuRegister>();
  GpuRegister trg = locations->Out().AsRegister<GpuRegister>();

  __ Daddu(TMP, base, offset);
  if (is_volatile) {
    __ Sync(0);
  }
  switch (type) {
    case Primitive::kPrimInt:
      __ Lw(trg, TMP, 0);
      break;

    case Primitive::kPrimNot:
      __ Lwu(trg, TMP, 0);
      break;

    case Primitive::kPrimLong:
      __ Ld(trg, TMP, 0);
      break;

    default:
      LOG(FATAL) << "Unsupported op size " << type;
      UNREACHABLE();
  }
}

// int sun.misc.Unsafe.getInt(Object o, long offset)
void IntrinsicLocationsBuilderMIPS64::VisitUnsafeGet(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitUnsafeGet(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimInt, false, codegen_);
}

// int sun.misc.Unsafe.getIntVolatile(Object o, long offset)
void IntrinsicLocationsBuilderMIPS64::VisitUnsafeGetVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitUnsafeGetVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimInt, true, codegen_);
}

// long sun.misc.Unsafe.getLong(Object o, long offset)
void IntrinsicLocationsBuilderMIPS64::VisitUnsafeGetLong(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitUnsafeGetLong(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimLong, false, codegen_);
}

// long sun.misc.Unsafe.getLongVolatile(Object o, long offset)
void IntrinsicLocationsBuilderMIPS64::VisitUnsafeGetLongVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitUnsafeGetLongVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimLong, true, codegen_);
}

// Object sun.misc.Unsafe.getObject(Object o, long offset)
void IntrinsicLocationsBuilderMIPS64::VisitUnsafeGetObject(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitUnsafeGetObject(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimNot, false, codegen_);
}

// Object sun.misc.Unsafe.getObjectVolatile(Object o, long offset)
void IntrinsicLocationsBuilderMIPS64::VisitUnsafeGetObjectVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitUnsafeGetObjectVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimNot, true, codegen_);
}

static void CreateIntIntIntIntToVoid(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::NoLocation());        // Unused receiver.
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetInAt(2, Location::RequiresRegister());
  locations->SetInAt(3, Location::RequiresRegister());
}

static void GenUnsafePut(LocationSummary* locations,
                         Primitive::Type type,
                         bool is_volatile,
                         bool is_ordered,
                         CodeGeneratorMIPS64* codegen) {
  DCHECK((type == Primitive::kPrimInt) ||
         (type == Primitive::kPrimLong) ||
         (type == Primitive::kPrimNot));
  Mips64Assembler* assembler = codegen->GetAssembler();
  // Object pointer.
  GpuRegister base = locations->InAt(1).AsRegister<GpuRegister>();
  // Long offset.
  GpuRegister offset = locations->InAt(2).AsRegister<GpuRegister>();
  GpuRegister value = locations->InAt(3).AsRegister<GpuRegister>();

  __ Daddu(TMP, base, offset);
  if (is_volatile || is_ordered) {
    __ Sync(0);
  }
  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
      __ Sw(value, TMP, 0);
      break;

    case Primitive::kPrimLong:
      __ Sd(value, TMP, 0);
      break;

    default:
      LOG(FATAL) << "Unsupported op size " << type;
      UNREACHABLE();
  }
  if (is_volatile) {
    __ Sync(0);
  }

  if (type == Primitive::kPrimNot) {
    codegen->MarkGCCard(base, value);
  }
}

// void sun.misc.Unsafe.putInt(Object o, long offset, int x)
void IntrinsicLocationsBuilderMIPS64::VisitUnsafePut(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitUnsafePut(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimInt, false, false, codegen_);
}

// void sun.misc.Unsafe.putOrderedInt(Object o, long offset, int x)
void IntrinsicLocationsBuilderMIPS64::VisitUnsafePutOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitUnsafePutOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimInt, false, true, codegen_);
}

// void sun.misc.Unsafe.putIntVolatile(Object o, long offset, int x)
void IntrinsicLocationsBuilderMIPS64::VisitUnsafePutVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitUnsafePutVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimInt, true, false, codegen_);
}

// void sun.misc.Unsafe.putObject(Object o, long offset, Object x)
void IntrinsicLocationsBuilderMIPS64::VisitUnsafePutObject(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitUnsafePutObject(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimNot, false, false, codegen_);
}

// void sun.misc.Unsafe.putOrderedObject(Object o, long offset, Object x)
void IntrinsicLocationsBuilderMIPS64::VisitUnsafePutObjectOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitUnsafePutObjectOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimNot, false, true, codegen_);
}

// void sun.misc.Unsafe.putObjectVolatile(Object o, long offset, Object x)
void IntrinsicLocationsBuilderMIPS64::VisitUnsafePutObjectVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitUnsafePutObjectVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimNot, true, false, codegen_);
}

// void sun.misc.Unsafe.putLong(Object o, long offset, long x)
void IntrinsicLocationsBuilderMIPS64::VisitUnsafePutLong(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitUnsafePutLong(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimLong, false, false, codegen_);
}

// void sun.misc.Unsafe.putOrderedLong(Object o, long offset, long x)
void IntrinsicLocationsBuilderMIPS64::VisitUnsafePutLongOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitUnsafePutLongOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimLong, false, true, codegen_);
}

// void sun.misc.Unsafe.putLongVolatile(Object o, long offset, long x)
void IntrinsicLocationsBuilderMIPS64::VisitUnsafePutLongVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitUnsafePutLongVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimLong, true, false, codegen_);
}

static void CreateIntIntIntIntIntToInt(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::NoLocation());        // Unused receiver.
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetInAt(2, Location::RequiresRegister());
  locations->SetInAt(3, Location::RequiresRegister());
  locations->SetInAt(4, Location::RequiresRegister());

  locations->SetOut(Location::RequiresRegister());
}

static void GenCas(LocationSummary* locations, Primitive::Type type, CodeGeneratorMIPS64* codegen) {
  Mips64Assembler* assembler = codegen->GetAssembler();
  GpuRegister base = locations->InAt(1).AsRegister<GpuRegister>();
  GpuRegister offset = locations->InAt(2).AsRegister<GpuRegister>();
  GpuRegister expected = locations->InAt(3).AsRegister<GpuRegister>();
  GpuRegister value = locations->InAt(4).AsRegister<GpuRegister>();
  GpuRegister out = locations->Out().AsRegister<GpuRegister>();

  DCHECK_NE(base, out);
  DCHECK_NE(offset, out);
  DCHECK_NE(expected, out);

  // do {
  //   tmp_value = [tmp_ptr] - expected;
  // } while (tmp_value == 0 && failure([tmp_ptr] <- r_new_value));
  // result = tmp_value != 0;

  Mips64Label loop_head, exit_loop;
  __ Daddu(TMP, base, offset);
  __ Sync(0);
  __ Bind(&loop_head);
  if (type == Primitive::kPrimLong) {
    __ Lld(out, TMP);
  } else {
    __ Ll(out, TMP);
  }
  __ Dsubu(out, out, expected);         // If we didn't get the 'expected'
  __ Sltiu(out, out, 1);                // value, set 'out' to false, and
  __ Beqzc(out, &exit_loop);            // return.
  __ Move(out, value);  // Use 'out' for the 'store conditional' instruction.
                        // If we use 'value' directly, we would lose 'value'
                        // in the case that the store fails.  Whether the
                        // store succeeds, or fails, it will load the
                        // correct boolean value into the 'out' register.
  if (type == Primitive::kPrimLong) {
    __ Scd(out, TMP);
  } else {
    __ Sc(out, TMP);
  }
  __ Beqzc(out, &loop_head);    // If we couldn't do the read-modify-write
                                // cycle atomically then retry.
  __ Bind(&exit_loop);
  __ Sync(0);
}

// boolean sun.misc.Unsafe.compareAndSwapInt(Object o, long offset, int expected, int x)
void IntrinsicLocationsBuilderMIPS64::VisitUnsafeCASInt(HInvoke* invoke) {
  CreateIntIntIntIntIntToInt(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitUnsafeCASInt(HInvoke* invoke) {
  GenCas(invoke->GetLocations(), Primitive::kPrimInt, codegen_);
}

// boolean sun.misc.Unsafe.compareAndSwapLong(Object o, long offset, long expected, long x)
void IntrinsicLocationsBuilderMIPS64::VisitUnsafeCASLong(HInvoke* invoke) {
  CreateIntIntIntIntIntToInt(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitUnsafeCASLong(HInvoke* invoke) {
  GenCas(invoke->GetLocations(), Primitive::kPrimLong, codegen_);
}

// boolean sun.misc.Unsafe.compareAndSwapObject(Object o, long offset, Object expected, Object x)
void IntrinsicLocationsBuilderMIPS64::VisitUnsafeCASObject(HInvoke* invoke) {
  CreateIntIntIntIntIntToInt(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS64::VisitUnsafeCASObject(HInvoke* invoke) {
  GenCas(invoke->GetLocations(), Primitive::kPrimNot, codegen_);
}

// char java.lang.String.charAt(int index)
void IntrinsicLocationsBuilderMIPS64::VisitStringCharAt(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCallOnSlowPath,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
}

void IntrinsicCodeGeneratorMIPS64::VisitStringCharAt(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  Mips64Assembler* assembler = GetAssembler();

  // Location of reference to data array
  const int32_t value_offset = mirror::String::ValueOffset().Int32Value();
  // Location of count
  const int32_t count_offset = mirror::String::CountOffset().Int32Value();

  GpuRegister obj = locations->InAt(0).AsRegister<GpuRegister>();
  GpuRegister idx = locations->InAt(1).AsRegister<GpuRegister>();
  GpuRegister out = locations->Out().AsRegister<GpuRegister>();

  // TODO: Maybe we can support range check elimination. Overall,
  //       though, I think it's not worth the cost.
  // TODO: For simplicity, the index parameter is requested in a
  //       register, so different from Quick we will not optimize the
  //       code for constants (which would save a register).

  SlowPathCodeMIPS64* slow_path = new (GetAllocator()) IntrinsicSlowPathMIPS64(invoke);
  codegen_->AddSlowPath(slow_path);

  // Load the string size
  __ Lw(TMP, obj, count_offset);
  codegen_->MaybeRecordImplicitNullCheck(invoke);
  // Revert to slow path if idx is too large, or negative
  __ Bgeuc(idx, TMP, slow_path->GetEntryLabel());

  // out = obj[2*idx].
  __ Sll(TMP, idx, 1);                  // idx * 2
  __ Daddu(TMP, TMP, obj);              // Address of char at location idx
  __ Lhu(out, TMP, value_offset);       // Load char at location idx

  __ Bind(slow_path->GetExitLabel());
}

// int java.lang.String.compareTo(String anotherString)
void IntrinsicLocationsBuilderMIPS64::VisitStringCompareTo(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  Location outLocation = calling_convention.GetReturnLocation(Primitive::kPrimInt);
  locations->SetOut(Location::RegisterLocation(outLocation.AsRegister<GpuRegister>()));
}

void IntrinsicCodeGeneratorMIPS64::VisitStringCompareTo(HInvoke* invoke) {
  Mips64Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  GpuRegister argument = locations->InAt(1).AsRegister<GpuRegister>();
  SlowPathCodeMIPS64* slow_path = new (GetAllocator()) IntrinsicSlowPathMIPS64(invoke);
  codegen_->AddSlowPath(slow_path);
  __ Beqzc(argument, slow_path->GetEntryLabel());

  __ LoadFromOffset(kLoadDoubleword,
                    TMP,
                    TR,
                    QUICK_ENTRYPOINT_OFFSET(kMips64WordSize,
                                            pStringCompareTo).Int32Value());
  __ Jalr(TMP);
  __ Nop();
  __ Bind(slow_path->GetExitLabel());
}

// boolean java.lang.String.equals(Object anObject)
void IntrinsicLocationsBuilderMIPS64::VisitStringEquals(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());

  // Temporary registers to store lengths of strings and for calculations.
  locations->AddTemp(Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
}

void IntrinsicCodeGeneratorMIPS64::VisitStringEquals(HInvoke* invoke) {
  Mips64Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  GpuRegister str = locations->InAt(0).AsRegister<GpuRegister>();
  GpuRegister arg = locations->InAt(1).AsRegister<GpuRegister>();
  GpuRegister out = locations->Out().AsRegister<GpuRegister>();

  GpuRegister temp1 = locations->GetTemp(0).AsRegister<GpuRegister>();
  GpuRegister temp2 = locations->GetTemp(1).AsRegister<GpuRegister>();
  GpuRegister temp3 = locations->GetTemp(2).AsRegister<GpuRegister>();

  Mips64Label loop;
  Mips64Label end;
  Mips64Label return_true;
  Mips64Label return_false;

  // Get offsets of count, value, and class fields within a string object.
  const int32_t count_offset = mirror::String::CountOffset().Int32Value();
  const int32_t value_offset = mirror::String::ValueOffset().Int32Value();
  const int32_t class_offset = mirror::Object::ClassOffset().Int32Value();

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  // If the register containing the pointer to "this", and the register
  // containing the pointer to "anObject" are the same register then
  // "this", and "anObject" are the same object and we can
  // short-circuit the logic to a true result.
  if (str == arg) {
    __ LoadConst64(out, 1);
    return;
  }

  // Check if input is null, return false if it is.
  __ Beqzc(arg, &return_false);

  // Reference equality check, return true if same reference.
  __ Beqc(str, arg, &return_true);

  // Instanceof check for the argument by comparing class fields.
  // All string objects must have the same type since String cannot be subclassed.
  // Receiver must be a string object, so its class field is equal to all strings' class fields.
  // If the argument is a string object, its class field must be equal to receiver's class field.
  __ Lw(temp1, str, class_offset);
  __ Lw(temp2, arg, class_offset);
  __ Bnec(temp1, temp2, &return_false);

  // Load lengths of this and argument strings.
  __ Lw(temp1, str, count_offset);
  __ Lw(temp2, arg, count_offset);
  // Check if lengths are equal, return false if they're not.
  __ Bnec(temp1, temp2, &return_false);
  // Return true if both strings are empty.
  __ Beqzc(temp1, &return_true);

  // Don't overwrite input registers
  __ Move(TMP, str);
  __ Move(temp3, arg);

  // Assertions that must hold in order to compare strings 4 characters at a time.
  DCHECK_ALIGNED(value_offset, 8);
  static_assert(IsAligned<8>(kObjectAlignment), "String of odd length is not zero padded");

  // Loop to compare strings 4 characters at a time starting at the beginning of the string.
  // Ok to do this because strings are zero-padded to be 8-byte aligned.
  __ Bind(&loop);
  __ Ld(out, TMP, value_offset);
  __ Ld(temp2, temp3, value_offset);
  __ Bnec(out, temp2, &return_false);
  __ Daddiu(TMP, TMP, 8);
  __ Daddiu(temp3, temp3, 8);
  __ Addiu(temp1, temp1, -4);
  __ Bgtzc(temp1, &loop);

  // Return true and exit the function.
  // If loop does not result in returning false, we return true.
  __ Bind(&return_true);
  __ LoadConst64(out, 1);
  __ Bc(&end);

  // Return false and exit the function.
  __ Bind(&return_false);
  __ LoadConst64(out, 0);
  __ Bind(&end);
}

static void GenerateStringIndexOf(HInvoke* invoke,
                                  Mips64Assembler* assembler,
                                  CodeGeneratorMIPS64* codegen,
                                  ArenaAllocator* allocator,
                                  bool start_at_zero) {
  LocationSummary* locations = invoke->GetLocations();
  GpuRegister tmp_reg = start_at_zero ? locations->GetTemp(0).AsRegister<GpuRegister>() : TMP;

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  // Check for code points > 0xFFFF. Either a slow-path check when we
  // don't know statically, or directly dispatch if we have a constant.
  SlowPathCodeMIPS64* slow_path = nullptr;
  if (invoke->InputAt(1)->IsIntConstant()) {
    if (!IsUint<16>(invoke->InputAt(1)->AsIntConstant()->GetValue())) {
      // Always needs the slow-path. We could directly dispatch to it,
      // but this case should be rare, so for simplicity just put the
      // full slow-path down and branch unconditionally.
      slow_path = new (allocator) IntrinsicSlowPathMIPS64(invoke);
      codegen->AddSlowPath(slow_path);
      __ Bc(slow_path->GetEntryLabel());
      __ Bind(slow_path->GetExitLabel());
      return;
    }
  } else {
    GpuRegister char_reg = locations->InAt(1).AsRegister<GpuRegister>();
    __ LoadConst32(tmp_reg, std::numeric_limits<uint16_t>::max());
    slow_path = new (allocator) IntrinsicSlowPathMIPS64(invoke);
    codegen->AddSlowPath(slow_path);
    __ Bltuc(tmp_reg, char_reg, slow_path->GetEntryLabel());    // UTF-16 required
  }

  if (start_at_zero) {
    DCHECK_EQ(tmp_reg, A2);
    // Start-index = 0.
    __ Clear(tmp_reg);
  } else {
    __ Slt(TMP, A2, ZERO);      // if fromIndex < 0
    __ Seleqz(A2, A2, TMP);     //     fromIndex = 0
  }

  __ LoadFromOffset(kLoadDoubleword,
                    TMP,
                    TR,
                    QUICK_ENTRYPOINT_OFFSET(kMips64WordSize, pIndexOf).Int32Value());
  __ Jalr(TMP);
  __ Nop();

  if (slow_path != nullptr) {
    __ Bind(slow_path->GetExitLabel());
  }
}

// int java.lang.String.indexOf(int ch)
void IntrinsicLocationsBuilderMIPS64::VisitStringIndexOf(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  // We have a hand-crafted assembly stub that follows the runtime
  // calling convention. So it's best to align the inputs accordingly.
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  Location outLocation = calling_convention.GetReturnLocation(Primitive::kPrimInt);
  locations->SetOut(Location::RegisterLocation(outLocation.AsRegister<GpuRegister>()));

  // Need a temp for slow-path codepoint compare, and need to send start-index=0.
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
}

void IntrinsicCodeGeneratorMIPS64::VisitStringIndexOf(HInvoke* invoke) {
  GenerateStringIndexOf(invoke, GetAssembler(), codegen_, GetAllocator(), true);
}

// int java.lang.String.indexOf(int ch, int fromIndex)
void IntrinsicLocationsBuilderMIPS64::VisitStringIndexOfAfter(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  // We have a hand-crafted assembly stub that follows the runtime
  // calling convention. So it's best to align the inputs accordingly.
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(2, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  Location outLocation = calling_convention.GetReturnLocation(Primitive::kPrimInt);
  locations->SetOut(Location::RegisterLocation(outLocation.AsRegister<GpuRegister>()));
}

void IntrinsicCodeGeneratorMIPS64::VisitStringIndexOfAfter(HInvoke* invoke) {
  GenerateStringIndexOf(invoke, GetAssembler(), codegen_, GetAllocator(), false);
}

// java.lang.String.String(byte[] bytes)
void IntrinsicLocationsBuilderMIPS64::VisitStringNewStringFromBytes(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(2, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  locations->SetInAt(3, Location::RegisterLocation(calling_convention.GetRegisterAt(3)));
  Location outLocation = calling_convention.GetReturnLocation(Primitive::kPrimInt);
  locations->SetOut(Location::RegisterLocation(outLocation.AsRegister<GpuRegister>()));
}

void IntrinsicCodeGeneratorMIPS64::VisitStringNewStringFromBytes(HInvoke* invoke) {
  Mips64Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  GpuRegister byte_array = locations->InAt(0).AsRegister<GpuRegister>();
  SlowPathCodeMIPS64* slow_path = new (GetAllocator()) IntrinsicSlowPathMIPS64(invoke);
  codegen_->AddSlowPath(slow_path);
  __ Beqzc(byte_array, slow_path->GetEntryLabel());

  __ LoadFromOffset(kLoadDoubleword,
                    TMP,
                    TR,
                    QUICK_ENTRYPOINT_OFFSET(kMips64WordSize, pAllocStringFromBytes).Int32Value());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
  __ Jalr(TMP);
  __ Nop();
  __ Bind(slow_path->GetExitLabel());
}

// java.lang.String.String(char[] value)
void IntrinsicLocationsBuilderMIPS64::VisitStringNewStringFromChars(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(2, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  Location outLocation = calling_convention.GetReturnLocation(Primitive::kPrimInt);
  locations->SetOut(Location::RegisterLocation(outLocation.AsRegister<GpuRegister>()));
}

void IntrinsicCodeGeneratorMIPS64::VisitStringNewStringFromChars(HInvoke* invoke) {
  Mips64Assembler* assembler = GetAssembler();

  __ LoadFromOffset(kLoadDoubleword,
                    TMP,
                    TR,
                    QUICK_ENTRYPOINT_OFFSET(kMips64WordSize, pAllocStringFromChars).Int32Value());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
  __ Jalr(TMP);
  __ Nop();
}

// java.lang.String.String(String original)
void IntrinsicLocationsBuilderMIPS64::VisitStringNewStringFromString(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(2, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  Location outLocation = calling_convention.GetReturnLocation(Primitive::kPrimInt);
  locations->SetOut(Location::RegisterLocation(outLocation.AsRegister<GpuRegister>()));
}

void IntrinsicCodeGeneratorMIPS64::VisitStringNewStringFromString(HInvoke* invoke) {
  Mips64Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  GpuRegister string_to_copy = locations->InAt(0).AsRegister<GpuRegister>();
  SlowPathCodeMIPS64* slow_path = new (GetAllocator()) IntrinsicSlowPathMIPS64(invoke);
  codegen_->AddSlowPath(slow_path);
  __ Beqzc(string_to_copy, slow_path->GetEntryLabel());

  __ LoadFromOffset(kLoadDoubleword,
                    TMP,
                    TR,
                    QUICK_ENTRYPOINT_OFFSET(kMips64WordSize, pAllocStringFromString).Int32Value());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
  __ Jalr(TMP);
  __ Nop();
  __ Bind(slow_path->GetExitLabel());
}

// Unimplemented intrinsics.

#define UNIMPLEMENTED_INTRINSIC(Name)                                                  \
void IntrinsicLocationsBuilderMIPS64::Visit ## Name(HInvoke* invoke ATTRIBUTE_UNUSED) { \
}                                                                                      \
void IntrinsicCodeGeneratorMIPS64::Visit ## Name(HInvoke* invoke ATTRIBUTE_UNUSED) {    \
}

UNIMPLEMENTED_INTRINSIC(MathRoundDouble)
UNIMPLEMENTED_INTRINSIC(MathRoundFloat)

UNIMPLEMENTED_INTRINSIC(ReferenceGetReferent)
UNIMPLEMENTED_INTRINSIC(StringGetCharsNoCheck)
UNIMPLEMENTED_INTRINSIC(SystemArrayCopyChar)
UNIMPLEMENTED_INTRINSIC(SystemArrayCopy)

#undef UNIMPLEMENTED_INTRINSIC

#undef __

}  // namespace mips64
}  // namespace art
