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

#include "intrinsics_mips.h"

#include "arch/mips/instruction_set_features_mips.h"
#include "art_method.h"
#include "code_generator_mips.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "intrinsics.h"
#include "mirror/array-inl.h"
#include "mirror/string.h"
#include "thread.h"
#include "utils/mips/assembler_mips.h"
#include "utils/mips/constants_mips.h"

namespace art {

namespace mips {

IntrinsicLocationsBuilderMIPS::IntrinsicLocationsBuilderMIPS(CodeGeneratorMIPS* codegen)
  : arena_(codegen->GetGraph()->GetArena()) {
}

MipsAssembler* IntrinsicCodeGeneratorMIPS::GetAssembler() {
  return reinterpret_cast<MipsAssembler*>(codegen_->GetAssembler());
}

ArenaAllocator* IntrinsicCodeGeneratorMIPS::GetAllocator() {
  return codegen_->GetGraph()->GetArena();
}

inline bool IntrinsicCodeGeneratorMIPS::IsR2OrNewer() const {
  return codegen_->GetInstructionSetFeatures().IsMipsIsaRevGreaterThanEqual2();
}

inline bool IntrinsicCodeGeneratorMIPS::IsR6() const {
  return codegen_->GetInstructionSetFeatures().IsR6();
}

inline bool IntrinsicCodeGeneratorMIPS::Is32BitFPU() const {
  return codegen_->GetInstructionSetFeatures().Is32BitFloatingPoint();
}

#define __ codegen->GetAssembler()->

static void MoveFromReturnRegister(Location trg,
                                   Primitive::Type type,
                                   CodeGeneratorMIPS* codegen) {
  if (!trg.IsValid()) {
    DCHECK_EQ(type, Primitive::kPrimVoid);
    return;
  }

  DCHECK_NE(type, Primitive::kPrimVoid);

  if (Primitive::IsIntegralType(type) || type == Primitive::kPrimNot) {
    Register trg_reg = trg.AsRegister<Register>();
    if (trg_reg != V0) {
      __ Move(V0, trg_reg);
    }
  } else {
    FRegister trg_reg = trg.AsFpuRegister<FRegister>();
    if (trg_reg != F0) {
      if (type == Primitive::kPrimFloat) {
        __ MovS(F0, trg_reg);
      } else {
        __ MovD(F0, trg_reg);
      }
    }
  }
}

static void MoveArguments(HInvoke* invoke, CodeGeneratorMIPS* codegen) {
  InvokeDexCallingConventionVisitorMIPS calling_convention_visitor;
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
class IntrinsicSlowPathMIPS : public SlowPathCodeMIPS {
 public:
  explicit IntrinsicSlowPathMIPS(HInvoke* invoke) : invoke_(invoke) { }

  void EmitNativeCode(CodeGenerator* codegen_in) OVERRIDE {
    CodeGeneratorMIPS* codegen = down_cast<CodeGeneratorMIPS*>(codegen_in);

    __ Bind(GetEntryLabel());

    SaveLiveRegisters(codegen, invoke_->GetLocations());

    MoveArguments(invoke_, codegen);

    if (invoke_->IsInvokeStaticOrDirect()) {
      codegen->GenerateStaticOrDirectCall(invoke_->AsInvokeStaticOrDirect(),
                                          Location::RegisterLocation(A0));
      codegen->RecordPcInfo(invoke_, invoke_->GetDexPc(), this);
    } else {
      UNIMPLEMENTED(FATAL) << "Non-direct intrinsic slow-path not yet implemented";
      UNREACHABLE();
    }

    // Copy the result back to the expected output.
    Location out = invoke_->GetLocations()->Out();
    if (out.IsValid()) {
      DCHECK(out.IsRegister());  // TODO: Replace this when we support output in memory.
      DCHECK(!invoke_->GetLocations()->GetLiveRegisters()->ContainsCoreRegister(out.reg()));
      MoveFromReturnRegister(out, invoke_->GetType(), codegen);
    }

    RestoreLiveRegisters(codegen, invoke_->GetLocations());
    __ B(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "IntrinsicSlowPathMIPS"; }

 private:
  // The instruction where this slow path is happening.
  HInvoke* const invoke_;

  DISALLOW_COPY_AND_ASSIGN(IntrinsicSlowPathMIPS);
};

#undef __

bool IntrinsicLocationsBuilderMIPS::TryDispatch(HInvoke* invoke) {
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

static void MoveFPToInt(LocationSummary* locations, bool is64bit, MipsAssembler* assembler) {
  FRegister in = locations->InAt(0).AsFpuRegister<FRegister>();

  if (is64bit) {
    Register out_lo = locations->Out().AsRegisterPairLow<Register>();
    Register out_hi = locations->Out().AsRegisterPairHigh<Register>();

    __ Mfc1(out_lo, in);
    __ MoveFromFpuHigh(out_hi, in);
  } else {
    Register out = locations->Out().AsRegister<Register>();

    __ Mfc1(out, in);
  }
}

// long java.lang.Double.doubleToRawLongBits(double)
void IntrinsicLocationsBuilderMIPS::VisitDoubleDoubleToRawLongBits(HInvoke* invoke) {
  CreateFPToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitDoubleDoubleToRawLongBits(HInvoke* invoke) {
  MoveFPToInt(invoke->GetLocations(), /* is64bit */ true, GetAssembler());
}

// int java.lang.Float.floatToRawIntBits(float)
void IntrinsicLocationsBuilderMIPS::VisitFloatFloatToRawIntBits(HInvoke* invoke) {
  CreateFPToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitFloatFloatToRawIntBits(HInvoke* invoke) {
  MoveFPToInt(invoke->GetLocations(), /* is64bit */ false, GetAssembler());
}

static void CreateIntToFPLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresFpuRegister());
}

static void MoveIntToFP(LocationSummary* locations, bool is64bit, MipsAssembler* assembler) {
  FRegister out = locations->Out().AsFpuRegister<FRegister>();

  if (is64bit) {
    Register in_lo = locations->InAt(0).AsRegisterPairLow<Register>();
    Register in_hi = locations->InAt(0).AsRegisterPairHigh<Register>();

    __ Mtc1(in_lo, out);
    __ MoveToFpuHigh(in_hi, out);
  } else {
    Register in = locations->InAt(0).AsRegister<Register>();

    __ Mtc1(in, out);
  }
}

// double java.lang.Double.longBitsToDouble(long)
void IntrinsicLocationsBuilderMIPS::VisitDoubleLongBitsToDouble(HInvoke* invoke) {
  CreateIntToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitDoubleLongBitsToDouble(HInvoke* invoke) {
  MoveIntToFP(invoke->GetLocations(), /* is64bit */ true, GetAssembler());
}

// float java.lang.Float.intBitsToFloat(int)
void IntrinsicLocationsBuilderMIPS::VisitFloatIntBitsToFloat(HInvoke* invoke) {
  CreateIntToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitFloatIntBitsToFloat(HInvoke* invoke) {
  MoveIntToFP(invoke->GetLocations(), /* is64bit */ false, GetAssembler());
}

static void CreateIntToIntLocations(ArenaAllocator* arena,
                                    HInvoke* invoke,
                                    Location::OutputOverlap overlaps = Location::kNoOutputOverlap) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), overlaps);
}

static void GenReverse(LocationSummary* locations,
                       Primitive::Type type,
                       bool isR2OrNewer,
                       bool isR6,
                       bool reverseBits,
                       MipsAssembler* assembler) {
  DCHECK(type == Primitive::kPrimShort ||
         type == Primitive::kPrimInt ||
         type == Primitive::kPrimLong);
  DCHECK(type != Primitive::kPrimShort || !reverseBits);

  if (type == Primitive::kPrimShort) {
    Register in = locations->InAt(0).AsRegister<Register>();
    Register out = locations->Out().AsRegister<Register>();

    if (isR2OrNewer) {
      __ Wsbh(out, in);
      __ Seh(out, out);
    } else {
      __ Sll(TMP, in, 24);
      __ Sra(TMP, TMP, 16);
      __ Sll(out, in, 16);
      __ Srl(out, out, 24);
      __ Or(out, out, TMP);
    }
  } else if (type == Primitive::kPrimInt) {
    Register in = locations->InAt(0).AsRegister<Register>();
    Register out = locations->Out().AsRegister<Register>();

    if (isR2OrNewer) {
      __ Rotr(out, in, 16);
      __ Wsbh(out, out);
    } else {
      // MIPS32r1
      // __ Rotr(out, in, 16);
      __ Sll(TMP, in, 16);
      __ Srl(out, in, 16);
      __ Or(out, out, TMP);
      // __ Wsbh(out, out);
      __ LoadConst32(AT, 0x00FF00FF);
      __ And(TMP, out, AT);
      __ Sll(TMP, TMP, 8);
      __ Srl(out, out, 8);
      __ And(out, out, AT);
      __ Or(out, out, TMP);
    }
    if (reverseBits) {
      if (isR6) {
        __ Bitswap(out, out);
      } else {
        __ LoadConst32(AT, 0x0F0F0F0F);
        __ And(TMP, out, AT);
        __ Sll(TMP, TMP, 4);
        __ Srl(out, out, 4);
        __ And(out, out, AT);
        __ Or(out, TMP, out);
        __ LoadConst32(AT, 0x33333333);
        __ And(TMP, out, AT);
        __ Sll(TMP, TMP, 2);
        __ Srl(out, out, 2);
        __ And(out, out, AT);
        __ Or(out, TMP, out);
        __ LoadConst32(AT, 0x55555555);
        __ And(TMP, out, AT);
        __ Sll(TMP, TMP, 1);
        __ Srl(out, out, 1);
        __ And(out, out, AT);
        __ Or(out, TMP, out);
      }
    }
  } else if (type == Primitive::kPrimLong) {
    Register in_lo = locations->InAt(0).AsRegisterPairLow<Register>();
    Register in_hi = locations->InAt(0).AsRegisterPairHigh<Register>();
    Register out_lo = locations->Out().AsRegisterPairLow<Register>();
    Register out_hi = locations->Out().AsRegisterPairHigh<Register>();

    if (isR2OrNewer) {
      __ Rotr(AT, in_hi, 16);
      __ Rotr(TMP, in_lo, 16);
      __ Wsbh(out_lo, AT);
      __ Wsbh(out_hi, TMP);
    } else {
      // When calling CreateIntToIntLocations() we promised that the
      // use of the out_lo/out_hi wouldn't overlap with the use of
      // in_lo/in_hi. Be very careful not to write to out_lo/out_hi
      // until we're completely done reading from in_lo/in_hi.
      // __ Rotr(TMP, in_lo, 16);
      __ Sll(TMP, in_lo, 16);
      __ Srl(AT, in_lo, 16);
      __ Or(TMP, TMP, AT);             // Hold in TMP until it's safe
                                       // to write to out_hi.
      // __ Rotr(out_lo, in_hi, 16);
      __ Sll(AT, in_hi, 16);
      __ Srl(out_lo, in_hi, 16);        // Here we are finally done reading
                                        // from in_lo/in_hi so it's okay to
                                        // write to out_lo/out_hi.
      __ Or(out_lo, out_lo, AT);
      // __ Wsbh(out_hi, out_hi);
      __ LoadConst32(AT, 0x00FF00FF);
      __ And(out_hi, TMP, AT);
      __ Sll(out_hi, out_hi, 8);
      __ Srl(TMP, TMP, 8);
      __ And(TMP, TMP, AT);
      __ Or(out_hi, out_hi, TMP);
      // __ Wsbh(out_lo, out_lo);
      __ And(TMP, out_lo, AT);  // AT already holds the correct mask value
      __ Sll(TMP, TMP, 8);
      __ Srl(out_lo, out_lo, 8);
      __ And(out_lo, out_lo, AT);
      __ Or(out_lo, out_lo, TMP);
    }
    if (reverseBits) {
      if (isR6) {
        __ Bitswap(out_hi, out_hi);
        __ Bitswap(out_lo, out_lo);
      } else {
        __ LoadConst32(AT, 0x0F0F0F0F);
        __ And(TMP, out_hi, AT);
        __ Sll(TMP, TMP, 4);
        __ Srl(out_hi, out_hi, 4);
        __ And(out_hi, out_hi, AT);
        __ Or(out_hi, TMP, out_hi);
        __ And(TMP, out_lo, AT);
        __ Sll(TMP, TMP, 4);
        __ Srl(out_lo, out_lo, 4);
        __ And(out_lo, out_lo, AT);
        __ Or(out_lo, TMP, out_lo);
        __ LoadConst32(AT, 0x33333333);
        __ And(TMP, out_hi, AT);
        __ Sll(TMP, TMP, 2);
        __ Srl(out_hi, out_hi, 2);
        __ And(out_hi, out_hi, AT);
        __ Or(out_hi, TMP, out_hi);
        __ And(TMP, out_lo, AT);
        __ Sll(TMP, TMP, 2);
        __ Srl(out_lo, out_lo, 2);
        __ And(out_lo, out_lo, AT);
        __ Or(out_lo, TMP, out_lo);
        __ LoadConst32(AT, 0x55555555);
        __ And(TMP, out_hi, AT);
        __ Sll(TMP, TMP, 1);
        __ Srl(out_hi, out_hi, 1);
        __ And(out_hi, out_hi, AT);
        __ Or(out_hi, TMP, out_hi);
        __ And(TMP, out_lo, AT);
        __ Sll(TMP, TMP, 1);
        __ Srl(out_lo, out_lo, 1);
        __ And(out_lo, out_lo, AT);
        __ Or(out_lo, TMP, out_lo);
      }
    }
  }
}

// int java.lang.Integer.reverseBytes(int)
void IntrinsicLocationsBuilderMIPS::VisitIntegerReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitIntegerReverseBytes(HInvoke* invoke) {
  GenReverse(invoke->GetLocations(),
             Primitive::kPrimInt,
             IsR2OrNewer(),
             IsR6(),
             false,
             GetAssembler());
}

// long java.lang.Long.reverseBytes(long)
void IntrinsicLocationsBuilderMIPS::VisitLongReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitLongReverseBytes(HInvoke* invoke) {
  GenReverse(invoke->GetLocations(),
             Primitive::kPrimLong,
             IsR2OrNewer(),
             IsR6(),
             false,
             GetAssembler());
}

// short java.lang.Short.reverseBytes(short)
void IntrinsicLocationsBuilderMIPS::VisitShortReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitShortReverseBytes(HInvoke* invoke) {
  GenReverse(invoke->GetLocations(),
             Primitive::kPrimShort,
             IsR2OrNewer(),
             IsR6(),
             false,
             GetAssembler());
}

static void GenNumberOfLeadingZeroes(LocationSummary* locations,
                                     bool is64bit,
                                     bool isR6,
                                     MipsAssembler* assembler) {
  Register out = locations->Out().AsRegister<Register>();
  if (is64bit) {
    Register in_lo = locations->InAt(0).AsRegisterPairLow<Register>();
    Register in_hi = locations->InAt(0).AsRegisterPairHigh<Register>();

    if (isR6) {
      __ ClzR6(AT, in_hi);
      __ ClzR6(TMP, in_lo);
      __ Seleqz(TMP, TMP, in_hi);
    } else {
      __ ClzR2(AT, in_hi);
      __ ClzR2(TMP, in_lo);
      __ Movn(TMP, ZERO, in_hi);
    }
    __ Addu(out, AT, TMP);
  } else {
    Register in = locations->InAt(0).AsRegister<Register>();

    if (isR6) {
      __ ClzR6(out, in);
    } else {
      __ ClzR2(out, in);
    }
  }
}

// int java.lang.Integer.numberOfLeadingZeros(int i)
void IntrinsicLocationsBuilderMIPS::VisitIntegerNumberOfLeadingZeros(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitIntegerNumberOfLeadingZeros(HInvoke* invoke) {
  GenNumberOfLeadingZeroes(invoke->GetLocations(), false, IsR6(), GetAssembler());
}

// int java.lang.Long.numberOfLeadingZeros(long i)
void IntrinsicLocationsBuilderMIPS::VisitLongNumberOfLeadingZeros(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitLongNumberOfLeadingZeros(HInvoke* invoke) {
  GenNumberOfLeadingZeroes(invoke->GetLocations(), true, IsR6(), GetAssembler());
}

static void GenNumberOfTrailingZeroes(LocationSummary* locations,
                                      bool is64bit,
                                      bool isR6,
                                      bool isR2OrNewer,
                                      MipsAssembler* assembler) {
  Register out = locations->Out().AsRegister<Register>();
  Register in_lo;
  Register in;

  if (is64bit) {
    MipsLabel done;
    Register in_hi = locations->InAt(0).AsRegisterPairHigh<Register>();

    in_lo = locations->InAt(0).AsRegisterPairLow<Register>();

    // If in_lo is zero then count the number of trailing zeroes in in_hi;
    // otherwise count the number of trailing zeroes in in_lo.
    // AT = in_lo ? in_lo : in_hi;
    if (isR6) {
      __ Seleqz(out, in_hi, in_lo);
      __ Selnez(TMP, in_lo, in_lo);
      __ Or(out, out, TMP);
    } else {
      __ Movz(out, in_hi, in_lo);
      __ Movn(out, in_lo, in_lo);
    }

    in = out;
  } else {
    in = locations->InAt(0).AsRegister<Register>();
    // Give in_lo a dummy value to keep the compiler from complaining.
    // Since we only get here in the 32-bit case, this value will never
    // be used.
    in_lo = in;
  }

  // We don't have an instruction to count the number of trailing zeroes.
  // Start by flipping the bits end-for-end so we can count the number of
  // leading zeroes instead.
  if (isR2OrNewer) {
    __ Rotr(out, in, 16);
    __ Wsbh(out, out);
  } else {
    // MIPS32r1
    // __ Rotr(out, in, 16);
    __ Sll(TMP, in, 16);
    __ Srl(out, in, 16);
    __ Or(out, out, TMP);
    // __ Wsbh(out, out);
    __ LoadConst32(AT, 0x00FF00FF);
    __ And(TMP, out, AT);
    __ Sll(TMP, TMP, 8);
    __ Srl(out, out, 8);
    __ And(out, out, AT);
    __ Or(out, out, TMP);
  }

  if (isR6) {
    __ Bitswap(out, out);
    __ ClzR6(out, out);
  } else {
    __ LoadConst32(AT, 0x0F0F0F0F);
    __ And(TMP, out, AT);
    __ Sll(TMP, TMP, 4);
    __ Srl(out, out, 4);
    __ And(out, out, AT);
    __ Or(out, TMP, out);
    __ LoadConst32(AT, 0x33333333);
    __ And(TMP, out, AT);
    __ Sll(TMP, TMP, 2);
    __ Srl(out, out, 2);
    __ And(out, out, AT);
    __ Or(out, TMP, out);
    __ LoadConst32(AT, 0x55555555);
    __ And(TMP, out, AT);
    __ Sll(TMP, TMP, 1);
    __ Srl(out, out, 1);
    __ And(out, out, AT);
    __ Or(out, TMP, out);
    __ ClzR2(out, out);
  }

  if (is64bit) {
    // If in_lo is zero, then we counted the number of trailing zeroes in in_hi so we must add the
    // number of trailing zeroes in in_lo (32) to get the correct final count
    __ LoadConst32(TMP, 32);
    if (isR6) {
      __ Seleqz(TMP, TMP, in_lo);
    } else {
      __ Movn(TMP, ZERO, in_lo);
    }
    __ Addu(out, out, TMP);
  }
}

// int java.lang.Integer.numberOfTrailingZeros(int i)
void IntrinsicLocationsBuilderMIPS::VisitIntegerNumberOfTrailingZeros(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke, Location::kOutputOverlap);
}

void IntrinsicCodeGeneratorMIPS::VisitIntegerNumberOfTrailingZeros(HInvoke* invoke) {
  GenNumberOfTrailingZeroes(invoke->GetLocations(), false, IsR6(), IsR2OrNewer(), GetAssembler());
}

// int java.lang.Long.numberOfTrailingZeros(long i)
void IntrinsicLocationsBuilderMIPS::VisitLongNumberOfTrailingZeros(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke, Location::kOutputOverlap);
}

void IntrinsicCodeGeneratorMIPS::VisitLongNumberOfTrailingZeros(HInvoke* invoke) {
  GenNumberOfTrailingZeroes(invoke->GetLocations(), true, IsR6(), IsR2OrNewer(), GetAssembler());
}

enum RotationDirection {
  kRotateRight,
  kRotateLeft,
};

static void GenRotate(HInvoke* invoke,
                      Primitive::Type type,
                      bool isR2OrNewer,
                      RotationDirection direction,
                      MipsAssembler* assembler) {
  DCHECK(type == Primitive::kPrimInt || type == Primitive::kPrimLong);

  LocationSummary* locations = invoke->GetLocations();
  if (invoke->InputAt(1)->IsIntConstant()) {
    int32_t shift = static_cast<int32_t>(invoke->InputAt(1)->AsIntConstant()->GetValue());
    if (type == Primitive::kPrimInt) {
      Register in = locations->InAt(0).AsRegister<Register>();
      Register out = locations->Out().AsRegister<Register>();

      shift &= 0x1f;
      if (direction == kRotateLeft) {
        shift = (32 - shift) & 0x1F;
      }

      if (isR2OrNewer) {
        if ((shift != 0) || (out != in)) {
          __ Rotr(out, in, shift);
        }
      } else {
        if (shift == 0) {
          if (out != in) {
            __ Move(out, in);
          }
        } else {
          __ Srl(AT, in, shift);
          __ Sll(out, in, 32 - shift);
          __ Or(out, out, AT);
        }
      }
    } else {    // Primitive::kPrimLong
      Register in_lo = locations->InAt(0).AsRegisterPairLow<Register>();
      Register in_hi = locations->InAt(0).AsRegisterPairHigh<Register>();
      Register out_lo = locations->Out().AsRegisterPairLow<Register>();
      Register out_hi = locations->Out().AsRegisterPairHigh<Register>();

      shift &= 0x3f;
      if (direction == kRotateLeft) {
        shift = (64 - shift) & 0x3F;
      }

      if (shift == 0) {
        __ Move(out_lo, in_lo);
        __ Move(out_hi, in_hi);
      } else if (shift == 32) {
        __ Move(out_lo, in_hi);
        __ Move(out_hi, in_lo);
      } else if (shift < 32) {
        __ Srl(AT, in_lo, shift);
        __ Sll(out_lo, in_hi, 32 - shift);
        __ Or(out_lo, out_lo, AT);
        __ Srl(AT, in_hi, shift);
        __ Sll(out_hi, in_lo, 32 - shift);
        __ Or(out_hi, out_hi, AT);
      } else {
        __ Sll(AT, in_lo, 64 - shift);
        __ Srl(out_lo, in_hi, shift - 32);
        __ Or(out_lo, out_lo, AT);
        __ Sll(AT, in_hi, 64 - shift);
        __ Srl(out_hi, in_lo, shift - 32);
        __ Or(out_hi, out_hi, AT);
      }
    }
  } else {      // !invoke->InputAt(1)->IsIntConstant()
    Register shamt = locations->InAt(1).AsRegister<Register>();
    if (type == Primitive::kPrimInt) {
      Register in = locations->InAt(0).AsRegister<Register>();
      Register out = locations->Out().AsRegister<Register>();

      if (isR2OrNewer) {
        if (direction == kRotateRight) {
          __ Rotrv(out, in, shamt);
        } else {
          // negu tmp, shamt
          __ Subu(TMP, ZERO, shamt);
          __ Rotrv(out, in, TMP);
        }
      } else {
        if (direction == kRotateRight) {
          __ Srlv(AT, in, shamt);
          __ Subu(TMP, ZERO, shamt);
          __ Sllv(out, in, TMP);
          __ Or(out, out, AT);
        } else {
          __ Sllv(AT, in, shamt);
          __ Subu(TMP, ZERO, shamt);
          __ Srlv(out, in, TMP);
          __ Or(out, out, AT);
        }
      }
    } else {    // Primitive::kPrimLong
      Register in_lo = locations->InAt(0).AsRegisterPairLow<Register>();
      Register in_hi = locations->InAt(0).AsRegisterPairHigh<Register>();
      Register out_lo = locations->Out().AsRegisterPairLow<Register>();
      Register out_hi = locations->Out().AsRegisterPairHigh<Register>();

      MipsLabel done;

      if (direction == kRotateRight) {
        __ Nor(TMP, ZERO, shamt);
        __ Srlv(AT, in_lo, shamt);
        __ Sll(out_lo, in_hi, 1);
        __ Sllv(out_lo, out_lo, TMP);
        __ Or(out_lo, out_lo, AT);
        __ Srlv(AT, in_hi, shamt);
        __ Sll(out_hi, in_lo, 1);
        __ Sllv(out_hi, out_hi, TMP);
        __ Or(out_hi, out_hi, AT);
      } else {
        __ Nor(TMP, ZERO, shamt);
        __ Sllv(AT, in_lo, shamt);
        __ Srl(out_lo, in_hi, 1);
        __ Srlv(out_lo, out_lo, TMP);
        __ Or(out_lo, out_lo, AT);
        __ Sllv(AT, in_hi, shamt);
        __ Srl(out_hi, in_lo, 1);
        __ Srlv(out_hi, out_hi, TMP);
        __ Or(out_hi, out_hi, AT);
      }

      __ Andi(TMP, shamt, 32);
      __ Beqz(TMP, &done);
      __ Move(TMP, out_hi);
      __ Move(out_hi, out_lo);
      __ Move(out_lo, TMP);

      __ Bind(&done);
    }
  }
}

// int java.lang.Integer.rotateRight(int i, int distance)
void IntrinsicLocationsBuilderMIPS::VisitIntegerRotateRight(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(invoke->InputAt(1)));
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void IntrinsicCodeGeneratorMIPS::VisitIntegerRotateRight(HInvoke* invoke) {
  GenRotate(invoke, Primitive::kPrimInt, IsR2OrNewer(), kRotateRight, GetAssembler());
}

// long java.lang.Long.rotateRight(long i, int distance)
void IntrinsicLocationsBuilderMIPS::VisitLongRotateRight(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(invoke->InputAt(1)));
  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
}

void IntrinsicCodeGeneratorMIPS::VisitLongRotateRight(HInvoke* invoke) {
  GenRotate(invoke, Primitive::kPrimLong, IsR2OrNewer(), kRotateRight, GetAssembler());
}

// int java.lang.Integer.rotateLeft(int i, int distance)
void IntrinsicLocationsBuilderMIPS::VisitIntegerRotateLeft(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(invoke->InputAt(1)));
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void IntrinsicCodeGeneratorMIPS::VisitIntegerRotateLeft(HInvoke* invoke) {
  GenRotate(invoke, Primitive::kPrimInt, IsR2OrNewer(), kRotateLeft, GetAssembler());
}

// long java.lang.Long.rotateLeft(long i, int distance)
void IntrinsicLocationsBuilderMIPS::VisitLongRotateLeft(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(invoke->InputAt(1)));
  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
}

void IntrinsicCodeGeneratorMIPS::VisitLongRotateLeft(HInvoke* invoke) {
  GenRotate(invoke, Primitive::kPrimLong, IsR2OrNewer(), kRotateLeft, GetAssembler());
}

// int java.lang.Integer.reverse(int)
void IntrinsicLocationsBuilderMIPS::VisitIntegerReverse(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitIntegerReverse(HInvoke* invoke) {
  GenReverse(invoke->GetLocations(),
             Primitive::kPrimInt,
             IsR2OrNewer(),
             IsR6(),
             true,
             GetAssembler());
}

// long java.lang.Long.reverse(long)
void IntrinsicLocationsBuilderMIPS::VisitLongReverse(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitLongReverse(HInvoke* invoke) {
  GenReverse(invoke->GetLocations(),
             Primitive::kPrimLong,
             IsR2OrNewer(),
             IsR6(),
             true,
             GetAssembler());
}

// boolean java.lang.String.equals(Object anObject)
void IntrinsicLocationsBuilderMIPS::VisitStringEquals(HInvoke* invoke) {
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

void IntrinsicCodeGeneratorMIPS::VisitStringEquals(HInvoke* invoke) {
  MipsAssembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  Register str = locations->InAt(0).AsRegister<Register>();
  Register arg = locations->InAt(1).AsRegister<Register>();
  Register out = locations->Out().AsRegister<Register>();

  Register temp1 = locations->GetTemp(0).AsRegister<Register>();
  Register temp2 = locations->GetTemp(1).AsRegister<Register>();
  Register temp3 = locations->GetTemp(2).AsRegister<Register>();

  MipsLabel loop;
  MipsLabel end;
  MipsLabel return_true;
  MipsLabel return_false;

  // Get offsets of count, value, and class fields within a string object.
  const uint32_t count_offset = mirror::String::CountOffset().Uint32Value();
  const uint32_t value_offset = mirror::String::ValueOffset().Uint32Value();
  const uint32_t class_offset = mirror::Object::ClassOffset().Uint32Value();

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  // If the register containing the pointer to "this", and the register
  // containing the pointer to "anObject" are the same register then
  // "this", and "anObject" are the same object and we can
  // short-circuit the logic to a true result.
  if (str == arg) {
    __ LoadConst32(out, 1);
    return;
  }

  // Check if input is null, return false if it is.
  __ Beqz(arg, &return_false);

  // Reference equality check, return true if same reference.
  __ Beq(str, arg, &return_true);

  // Instanceof check for the argument by comparing class fields.
  // All string objects must have the same type since String cannot be subclassed.
  // Receiver must be a string object, so its class field is equal to all strings' class fields.
  // If the argument is a string object, its class field must be equal to receiver's class field.
  __ Lw(temp1, str, class_offset);
  __ Lw(temp2, arg, class_offset);
  __ Bne(temp1, temp2, &return_false);

  // Load lengths of this and argument strings.
  __ Lw(temp1, str, count_offset);
  __ Lw(temp2, arg, count_offset);
  // Check if lengths are equal, return false if they're not.
  __ Bne(temp1, temp2, &return_false);
  // Return true if both strings are empty.
  __ Beqz(temp1, &return_true);

  // Don't overwrite input registers
  __ Move(TMP, str);
  __ Move(temp3, arg);

  // Assertions that must hold in order to compare strings 2 characters at a time.
  DCHECK_ALIGNED(value_offset, 4);
  static_assert(IsAligned<4>(kObjectAlignment), "String of odd length is not zero padded");

  // Loop to compare strings 2 characters at a time starting at the beginning of the string.
  // Ok to do this because strings are zero-padded.
  __ Bind(&loop);
  __ Lw(out, TMP, value_offset);
  __ Lw(temp2, temp3, value_offset);
  __ Bne(out, temp2, &return_false);
  __ Addiu(TMP, TMP, 4);
  __ Addiu(temp3, temp3, 4);
  __ Addiu(temp1, temp1, -2);
  __ Bgtz(temp1, &loop);

  // Return true and exit the function.
  // If loop does not result in returning false, we return true.
  __ Bind(&return_true);
  __ LoadConst32(out, 1);
  __ B(&end);

  // Return false and exit the function.
  __ Bind(&return_false);
  __ LoadConst32(out, 0);
  __ Bind(&end);
}

// Unimplemented intrinsics.

#define UNIMPLEMENTED_INTRINSIC(Name)                                                  \
void IntrinsicLocationsBuilderMIPS::Visit ## Name(HInvoke* invoke ATTRIBUTE_UNUSED) { \
}                                                                                      \
void IntrinsicCodeGeneratorMIPS::Visit ## Name(HInvoke* invoke ATTRIBUTE_UNUSED) {    \
}

UNIMPLEMENTED_INTRINSIC(IntegerBitCount)
UNIMPLEMENTED_INTRINSIC(LongBitCount)

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
UNIMPLEMENTED_INTRINSIC(StringIndexOf)
UNIMPLEMENTED_INTRINSIC(StringIndexOfAfter)
UNIMPLEMENTED_INTRINSIC(StringNewStringFromBytes)
UNIMPLEMENTED_INTRINSIC(StringNewStringFromChars)
UNIMPLEMENTED_INTRINSIC(StringNewStringFromString)

UNIMPLEMENTED_INTRINSIC(ReferenceGetReferent)
UNIMPLEMENTED_INTRINSIC(StringGetCharsNoCheck)
UNIMPLEMENTED_INTRINSIC(SystemArrayCopyChar)
UNIMPLEMENTED_INTRINSIC(SystemArrayCopy)

UNIMPLEMENTED_INTRINSIC(MathCos)
UNIMPLEMENTED_INTRINSIC(MathSin)
UNIMPLEMENTED_INTRINSIC(MathAcos)
UNIMPLEMENTED_INTRINSIC(MathAsin)
UNIMPLEMENTED_INTRINSIC(MathAtan)
UNIMPLEMENTED_INTRINSIC(MathAtan2)
UNIMPLEMENTED_INTRINSIC(MathCbrt)
UNIMPLEMENTED_INTRINSIC(MathCosh)
UNIMPLEMENTED_INTRINSIC(MathExp)
UNIMPLEMENTED_INTRINSIC(MathExpm1)
UNIMPLEMENTED_INTRINSIC(MathHypot)
UNIMPLEMENTED_INTRINSIC(MathLog)
UNIMPLEMENTED_INTRINSIC(MathLog10)
UNIMPLEMENTED_INTRINSIC(MathNextAfter)
UNIMPLEMENTED_INTRINSIC(MathSinh)
UNIMPLEMENTED_INTRINSIC(MathTan)
UNIMPLEMENTED_INTRINSIC(MathTanh)
#undef UNIMPLEMENTED_INTRINSIC

#undef __

}  // namespace mips
}  // namespace art
