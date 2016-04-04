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
  explicit IntrinsicSlowPathMIPS(HInvoke* invoke) : SlowPathCodeMIPS(invoke), invoke_(invoke) { }

  void EmitNativeCode(CodeGenerator* codegen_in) OVERRIDE {
    CodeGeneratorMIPS* codegen = down_cast<CodeGeneratorMIPS*>(codegen_in);

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
             /* reverseBits */ false,
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
             /* reverseBits */ false,
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
             /* reverseBits */ false,
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
  GenNumberOfLeadingZeroes(invoke->GetLocations(), /* is64bit */ false, IsR6(), GetAssembler());
}

// int java.lang.Long.numberOfLeadingZeros(long i)
void IntrinsicLocationsBuilderMIPS::VisitLongNumberOfLeadingZeros(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitLongNumberOfLeadingZeros(HInvoke* invoke) {
  GenNumberOfLeadingZeroes(invoke->GetLocations(), /* is64bit */ true, IsR6(), GetAssembler());
}

static void GenNumberOfTrailingZeroes(LocationSummary* locations,
                                      bool is64bit,
                                      bool isR6,
                                      MipsAssembler* assembler) {
  Register out = locations->Out().AsRegister<Register>();
  Register in_lo;
  Register in;

  if (is64bit) {
    Register in_hi = locations->InAt(0).AsRegisterPairHigh<Register>();

    in_lo = locations->InAt(0).AsRegisterPairLow<Register>();

    // If in_lo is zero then count the number of trailing zeroes in in_hi;
    // otherwise count the number of trailing zeroes in in_lo.
    // out = in_lo ? in_lo : in_hi;
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

  if (isR6) {
    // We don't have an instruction to count the number of trailing zeroes.
    // Start by flipping the bits end-for-end so we can count the number of
    // leading zeroes instead.
    __ Rotr(out, in, 16);
    __ Wsbh(out, out);
    __ Bitswap(out, out);
    __ ClzR6(out, out);
  } else {
    // Convert trailing zeroes to trailing ones, and bits to their left
    // to zeroes.
    __ Addiu(TMP, in, -1);
    __ Xor(out, TMP, in);
    __ And(out, out, TMP);
    // Count number of leading zeroes.
    __ ClzR2(out, out);
    // Subtract number of leading zeroes from 32 to get number of trailing ones.
    // Remember that the trailing ones were formerly trailing zeroes.
    __ LoadConst32(TMP, 32);
    __ Subu(out, TMP, out);
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
  GenNumberOfTrailingZeroes(invoke->GetLocations(), /* is64bit */ false, IsR6(), GetAssembler());
}

// int java.lang.Long.numberOfTrailingZeros(long i)
void IntrinsicLocationsBuilderMIPS::VisitLongNumberOfTrailingZeros(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke, Location::kOutputOverlap);
}

void IntrinsicCodeGeneratorMIPS::VisitLongNumberOfTrailingZeros(HInvoke* invoke) {
  GenNumberOfTrailingZeroes(invoke->GetLocations(), /* is64bit */ true, IsR6(), GetAssembler());
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
             /* reverseBits */ true,
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
             /* reverseBits */ true,
             GetAssembler());
}

static void CreateFPToFPLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
}

static void GenBitCount(LocationSummary* locations,
                        Primitive::Type type,
                        bool isR6,
                        MipsAssembler* assembler) {
  Register out = locations->Out().AsRegister<Register>();

  // https://graphics.stanford.edu/~seander/bithacks.html#CountBitsSetParallel
  //
  // A generalization of the best bit counting method to integers of
  // bit-widths up to 128 (parameterized by type T) is this:
  //
  // v = v - ((v >> 1) & (T)~(T)0/3);                           // temp
  // v = (v & (T)~(T)0/15*3) + ((v >> 2) & (T)~(T)0/15*3);      // temp
  // v = (v + (v >> 4)) & (T)~(T)0/255*15;                      // temp
  // c = (T)(v * ((T)~(T)0/255)) >> (sizeof(T) - 1) * BITS_PER_BYTE; // count
  //
  // For comparison, for 32-bit quantities, this algorithm can be executed
  // using 20 MIPS instructions (the calls to LoadConst32() generate two
  // machine instructions each for the values being used in this algorithm).
  // A(n unrolled) loop-based algorithm required 25 instructions.
  //
  // For 64-bit quantities, this algorithm gets executed twice, (once
  // for in_lo, and again for in_hi), but saves a few instructions
  // because the mask values only have to be loaded once.  Using this
  // algorithm the count for a 64-bit operand can be performed in 33
  // instructions compared to a loop-based algorithm which required 47
  // instructions.

  if (type == Primitive::kPrimInt) {
    Register in = locations->InAt(0).AsRegister<Register>();

    __ Srl(TMP, in, 1);
    __ LoadConst32(AT, 0x55555555);
    __ And(TMP, TMP, AT);
    __ Subu(TMP, in, TMP);
    __ LoadConst32(AT, 0x33333333);
    __ And(out, TMP, AT);
    __ Srl(TMP, TMP, 2);
    __ And(TMP, TMP, AT);
    __ Addu(TMP, out, TMP);
    __ Srl(out, TMP, 4);
    __ Addu(out, out, TMP);
    __ LoadConst32(AT, 0x0F0F0F0F);
    __ And(out, out, AT);
    __ LoadConst32(TMP, 0x01010101);
    if (isR6) {
      __ MulR6(out, out, TMP);
    } else {
      __ MulR2(out, out, TMP);
    }
    __ Srl(out, out, 24);
  } else {
    DCHECK_EQ(type, Primitive::kPrimLong);
    Register in_lo = locations->InAt(0).AsRegisterPairLow<Register>();
    Register in_hi = locations->InAt(0).AsRegisterPairHigh<Register>();
    Register tmp_hi = locations->GetTemp(0).AsRegister<Register>();
    Register out_hi = locations->GetTemp(1).AsRegister<Register>();
    Register tmp_lo = TMP;
    Register out_lo = out;

    __ Srl(tmp_lo, in_lo, 1);
    __ Srl(tmp_hi, in_hi, 1);

    __ LoadConst32(AT, 0x55555555);

    __ And(tmp_lo, tmp_lo, AT);
    __ Subu(tmp_lo, in_lo, tmp_lo);

    __ And(tmp_hi, tmp_hi, AT);
    __ Subu(tmp_hi, in_hi, tmp_hi);

    __ LoadConst32(AT, 0x33333333);

    __ And(out_lo, tmp_lo, AT);
    __ Srl(tmp_lo, tmp_lo, 2);
    __ And(tmp_lo, tmp_lo, AT);
    __ Addu(tmp_lo, out_lo, tmp_lo);
    __ Srl(out_lo, tmp_lo, 4);
    __ Addu(out_lo, out_lo, tmp_lo);

    __ And(out_hi, tmp_hi, AT);
    __ Srl(tmp_hi, tmp_hi, 2);
    __ And(tmp_hi, tmp_hi, AT);
    __ Addu(tmp_hi, out_hi, tmp_hi);
    __ Srl(out_hi, tmp_hi, 4);
    __ Addu(out_hi, out_hi, tmp_hi);

    __ LoadConst32(AT, 0x0F0F0F0F);

    __ And(out_lo, out_lo, AT);
    __ And(out_hi, out_hi, AT);

    __ LoadConst32(AT, 0x01010101);

    if (isR6) {
      __ MulR6(out_lo, out_lo, AT);

      __ MulR6(out_hi, out_hi, AT);
    } else {
      __ MulR2(out_lo, out_lo, AT);

      __ MulR2(out_hi, out_hi, AT);
    }

    __ Srl(out_lo, out_lo, 24);
    __ Srl(out_hi, out_hi, 24);

    __ Addu(out, out_hi, out_lo);
  }
}

// int java.lang.Integer.bitCount(int)
void IntrinsicLocationsBuilderMIPS::VisitIntegerBitCount(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitIntegerBitCount(HInvoke* invoke) {
  GenBitCount(invoke->GetLocations(), Primitive::kPrimInt, IsR6(), GetAssembler());
}

// int java.lang.Long.bitCount(int)
void IntrinsicLocationsBuilderMIPS::VisitLongBitCount(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
}

void IntrinsicCodeGeneratorMIPS::VisitLongBitCount(HInvoke* invoke) {
  GenBitCount(invoke->GetLocations(), Primitive::kPrimLong, IsR6(), GetAssembler());
}

static void MathAbsFP(LocationSummary* locations, bool is64bit, MipsAssembler* assembler) {
  FRegister in = locations->InAt(0).AsFpuRegister<FRegister>();
  FRegister out = locations->Out().AsFpuRegister<FRegister>();

  if (is64bit) {
    __ AbsD(out, in);
  } else {
    __ AbsS(out, in);
  }
}

// double java.lang.Math.abs(double)
void IntrinsicLocationsBuilderMIPS::VisitMathAbsDouble(HInvoke* invoke) {
  CreateFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitMathAbsDouble(HInvoke* invoke) {
  MathAbsFP(invoke->GetLocations(), /* is64bit */ true, GetAssembler());
}

// float java.lang.Math.abs(float)
void IntrinsicLocationsBuilderMIPS::VisitMathAbsFloat(HInvoke* invoke) {
  CreateFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitMathAbsFloat(HInvoke* invoke) {
  MathAbsFP(invoke->GetLocations(), /* is64bit */ false, GetAssembler());
}

static void GenAbsInteger(LocationSummary* locations, bool is64bit, MipsAssembler* assembler) {
  if (is64bit) {
    Register in_lo = locations->InAt(0).AsRegisterPairLow<Register>();
    Register in_hi = locations->InAt(0).AsRegisterPairHigh<Register>();
    Register out_lo = locations->Out().AsRegisterPairLow<Register>();
    Register out_hi = locations->Out().AsRegisterPairHigh<Register>();

    // The comments in this section show the analogous operations which would
    // be performed if we had 64-bit registers "in", and "out".
    // __ Dsra32(AT, in, 31);
    __ Sra(AT, in_hi, 31);
    // __ Xor(out, in, AT);
    __ Xor(TMP, in_lo, AT);
    __ Xor(out_hi, in_hi, AT);
    // __ Dsubu(out, out, AT);
    __ Subu(out_lo, TMP, AT);
    __ Sltu(TMP, out_lo, TMP);
    __ Addu(out_hi, out_hi, TMP);
  } else {
    Register in = locations->InAt(0).AsRegister<Register>();
    Register out = locations->Out().AsRegister<Register>();

    __ Sra(AT, in, 31);
    __ Xor(out, in, AT);
    __ Subu(out, out, AT);
  }
}

// int java.lang.Math.abs(int)
void IntrinsicLocationsBuilderMIPS::VisitMathAbsInt(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitMathAbsInt(HInvoke* invoke) {
  GenAbsInteger(invoke->GetLocations(), /* is64bit */ false, GetAssembler());
}

// long java.lang.Math.abs(long)
void IntrinsicLocationsBuilderMIPS::VisitMathAbsLong(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitMathAbsLong(HInvoke* invoke) {
  GenAbsInteger(invoke->GetLocations(), /* is64bit */ true, GetAssembler());
}

static void GenMinMaxFP(LocationSummary* locations,
                        bool is_min,
                        Primitive::Type type,
                        bool is_R6,
                        MipsAssembler* assembler) {
  FRegister out = locations->Out().AsFpuRegister<FRegister>();
  FRegister a = locations->InAt(0).AsFpuRegister<FRegister>();
  FRegister b = locations->InAt(1).AsFpuRegister<FRegister>();

  if (is_R6) {
    MipsLabel noNaNs;
    MipsLabel done;
    FRegister ftmp = ((out != a) && (out != b)) ? out : FTMP;

    // When Java computes min/max it prefers a NaN to a number; the
    // behavior of MIPSR6 is to prefer numbers to NaNs, i.e., if one of
    // the inputs is a NaN and the other is a valid number, the MIPS
    // instruction will return the number; Java wants the NaN value
    // returned. This is why there is extra logic preceding the use of
    // the MIPS min.fmt/max.fmt instructions. If either a, or b holds a
    // NaN, return the NaN, otherwise return the min/max.
    if (type == Primitive::kPrimDouble) {
      __ CmpUnD(FTMP, a, b);
      __ Bc1eqz(FTMP, &noNaNs);

      // One of the inputs is a NaN
      __ CmpEqD(ftmp, a, a);
      // If a == a then b is the NaN, otherwise a is the NaN.
      __ SelD(ftmp, a, b);

      if (ftmp != out) {
        __ MovD(out, ftmp);
      }

      __ B(&done);

      __ Bind(&noNaNs);

      if (is_min) {
        __ MinD(out, a, b);
      } else {
        __ MaxD(out, a, b);
      }
    } else {
      DCHECK_EQ(type, Primitive::kPrimFloat);
      __ CmpUnS(FTMP, a, b);
      __ Bc1eqz(FTMP, &noNaNs);

      // One of the inputs is a NaN
      __ CmpEqS(ftmp, a, a);
      // If a == a then b is the NaN, otherwise a is the NaN.
      __ SelS(ftmp, a, b);

      if (ftmp != out) {
        __ MovS(out, ftmp);
      }

      __ B(&done);

      __ Bind(&noNaNs);

      if (is_min) {
        __ MinS(out, a, b);
      } else {
        __ MaxS(out, a, b);
      }
    }

    __ Bind(&done);
  } else {
    MipsLabel ordered;
    MipsLabel compare;
    MipsLabel select;
    MipsLabel done;

    if (type == Primitive::kPrimDouble) {
      __ CunD(a, b);
    } else {
      DCHECK_EQ(type, Primitive::kPrimFloat);
      __ CunS(a, b);
    }
    __ Bc1f(&ordered);

    // a or b (or both) is a NaN. Return one, which is a NaN.
    if (type == Primitive::kPrimDouble) {
      __ CeqD(b, b);
    } else {
      __ CeqS(b, b);
    }
    __ B(&select);

    __ Bind(&ordered);

    // Neither is a NaN.
    // a == b? (-0.0 compares equal with +0.0)
    // If equal, handle zeroes, else compare further.
    if (type == Primitive::kPrimDouble) {
      __ CeqD(a, b);
    } else {
      __ CeqS(a, b);
    }
    __ Bc1f(&compare);

    // a == b either bit for bit or one is -0.0 and the other is +0.0.
    if (type == Primitive::kPrimDouble) {
      __ MoveFromFpuHigh(TMP, a);
      __ MoveFromFpuHigh(AT, b);
    } else {
      __ Mfc1(TMP, a);
      __ Mfc1(AT, b);
    }

    if (is_min) {
      // -0.0 prevails over +0.0.
      __ Or(TMP, TMP, AT);
    } else {
      // +0.0 prevails over -0.0.
      __ And(TMP, TMP, AT);
    }

    if (type == Primitive::kPrimDouble) {
      __ Mfc1(AT, a);
      __ Mtc1(AT, out);
      __ MoveToFpuHigh(TMP, out);
    } else {
      __ Mtc1(TMP, out);
    }
    __ B(&done);

    __ Bind(&compare);

    if (type == Primitive::kPrimDouble) {
      if (is_min) {
        // return (a <= b) ? a : b;
        __ ColeD(a, b);
      } else {
        // return (a >= b) ? a : b;
        __ ColeD(b, a);  // b <= a
      }
    } else {
      if (is_min) {
        // return (a <= b) ? a : b;
        __ ColeS(a, b);
      } else {
        // return (a >= b) ? a : b;
        __ ColeS(b, a);  // b <= a
      }
    }

    __ Bind(&select);

    if (type == Primitive::kPrimDouble) {
      __ MovtD(out, a);
      __ MovfD(out, b);
    } else {
      __ MovtS(out, a);
      __ MovfS(out, b);
    }

    __ Bind(&done);
  }
}

static void CreateFPFPToFPLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetInAt(1, Location::RequiresFpuRegister());
  locations->SetOut(Location::RequiresFpuRegister(), Location::kOutputOverlap);
}

// double java.lang.Math.min(double, double)
void IntrinsicLocationsBuilderMIPS::VisitMathMinDoubleDouble(HInvoke* invoke) {
  CreateFPFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitMathMinDoubleDouble(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(),
              /* is_min */ true,
              Primitive::kPrimDouble,
              IsR6(),
              GetAssembler());
}

// float java.lang.Math.min(float, float)
void IntrinsicLocationsBuilderMIPS::VisitMathMinFloatFloat(HInvoke* invoke) {
  CreateFPFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitMathMinFloatFloat(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(),
              /* is_min */ true,
              Primitive::kPrimFloat,
              IsR6(),
              GetAssembler());
}

// double java.lang.Math.max(double, double)
void IntrinsicLocationsBuilderMIPS::VisitMathMaxDoubleDouble(HInvoke* invoke) {
  CreateFPFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitMathMaxDoubleDouble(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(),
              /* is_min */ false,
              Primitive::kPrimDouble,
              IsR6(),
              GetAssembler());
}

// float java.lang.Math.max(float, float)
void IntrinsicLocationsBuilderMIPS::VisitMathMaxFloatFloat(HInvoke* invoke) {
  CreateFPFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitMathMaxFloatFloat(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(),
              /* is_min */ false,
              Primitive::kPrimFloat,
              IsR6(),
              GetAssembler());
}

static void CreateIntIntToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

static void GenMinMax(LocationSummary* locations,
                      bool is_min,
                      Primitive::Type type,
                      bool is_R6,
                      MipsAssembler* assembler) {
  if (is_R6) {
    // Some architectures, such as ARM and MIPS (prior to r6), have a
    // conditional move instruction which only changes the target
    // (output) register if the condition is true (MIPS prior to r6 had
    // MOVF, MOVT, MOVN, and MOVZ). The SELEQZ and SELNEZ instructions
    // always change the target (output) register.  If the condition is
    // true the output register gets the contents of the "rs" register;
    // otherwise, the output register is set to zero. One consequence
    // of this is that to implement something like "rd = c==0 ? rs : rt"
    // MIPS64r6 needs to use a pair of SELEQZ/SELNEZ instructions.
    // After executing this pair of instructions one of the output
    // registers from the pair will necessarily contain zero. Then the
    // code ORs the output registers from the SELEQZ/SELNEZ instructions
    // to get the final result.
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
    if (type == Primitive::kPrimLong) {
      Register a_lo = locations->InAt(0).AsRegisterPairLow<Register>();
      Register a_hi = locations->InAt(0).AsRegisterPairHigh<Register>();
      Register b_lo = locations->InAt(1).AsRegisterPairLow<Register>();
      Register b_hi = locations->InAt(1).AsRegisterPairHigh<Register>();
      Register out_lo = locations->Out().AsRegisterPairLow<Register>();
      Register out_hi = locations->Out().AsRegisterPairHigh<Register>();

      MipsLabel compare_done;

      if (a_lo == b_lo) {
        if (out_lo != a_lo) {
          __ Move(out_lo, a_lo);
          __ Move(out_hi, a_hi);
        }
      } else {
        __ Slt(TMP, b_hi, a_hi);
        __ Bne(b_hi, a_hi, &compare_done);

        __ Sltu(TMP, b_lo, a_lo);

        __ Bind(&compare_done);

        if (is_min) {
          __ Seleqz(AT, a_lo, TMP);
          __ Selnez(out_lo, b_lo, TMP);  // Safe even if out_lo == a_lo/b_lo
                                         // because at this point we're
                                         // done using a_lo/b_lo.
        } else {
          __ Selnez(AT, a_lo, TMP);
          __ Seleqz(out_lo, b_lo, TMP);  // ditto
        }
        __ Or(out_lo, out_lo, AT);
        if (is_min) {
          __ Seleqz(AT, a_hi, TMP);
          __ Selnez(out_hi, b_hi, TMP);  // ditto but for out_hi & a_hi/b_hi
        } else {
          __ Selnez(AT, a_hi, TMP);
          __ Seleqz(out_hi, b_hi, TMP);  // ditto but for out_hi & a_hi/b_hi
        }
        __ Or(out_hi, out_hi, AT);
      }
    } else {
      DCHECK_EQ(type, Primitive::kPrimInt);
      Register a = locations->InAt(0).AsRegister<Register>();
      Register b = locations->InAt(1).AsRegister<Register>();
      Register out = locations->Out().AsRegister<Register>();

      if (a == b) {
        if (out != a) {
          __ Move(out, a);
        }
      } else {
        __ Slt(AT, b, a);
        if (is_min) {
          __ Seleqz(TMP, a, AT);
          __ Selnez(AT, b, AT);
        } else {
          __ Selnez(TMP, a, AT);
          __ Seleqz(AT, b, AT);
        }
        __ Or(out, TMP, AT);
      }
    }
  } else {
    if (type == Primitive::kPrimLong) {
      Register a_lo = locations->InAt(0).AsRegisterPairLow<Register>();
      Register a_hi = locations->InAt(0).AsRegisterPairHigh<Register>();
      Register b_lo = locations->InAt(1).AsRegisterPairLow<Register>();
      Register b_hi = locations->InAt(1).AsRegisterPairHigh<Register>();
      Register out_lo = locations->Out().AsRegisterPairLow<Register>();
      Register out_hi = locations->Out().AsRegisterPairHigh<Register>();

      MipsLabel compare_done;

      if (a_lo == b_lo) {
        if (out_lo != a_lo) {
          __ Move(out_lo, a_lo);
          __ Move(out_hi, a_hi);
        }
      } else {
        __ Slt(TMP, a_hi, b_hi);
        __ Bne(a_hi, b_hi, &compare_done);

        __ Sltu(TMP, a_lo, b_lo);

        __ Bind(&compare_done);

        if (is_min) {
          if (out_lo != a_lo) {
            __ Movn(out_hi, a_hi, TMP);
            __ Movn(out_lo, a_lo, TMP);
          }
          if (out_lo != b_lo) {
            __ Movz(out_hi, b_hi, TMP);
            __ Movz(out_lo, b_lo, TMP);
          }
        } else {
          if (out_lo != a_lo) {
            __ Movz(out_hi, a_hi, TMP);
            __ Movz(out_lo, a_lo, TMP);
          }
          if (out_lo != b_lo) {
            __ Movn(out_hi, b_hi, TMP);
            __ Movn(out_lo, b_lo, TMP);
          }
        }
      }
    } else {
      DCHECK_EQ(type, Primitive::kPrimInt);
      Register a = locations->InAt(0).AsRegister<Register>();
      Register b = locations->InAt(1).AsRegister<Register>();
      Register out = locations->Out().AsRegister<Register>();

      if (a == b) {
        if (out != a) {
          __ Move(out, a);
        }
      } else {
        __ Slt(AT, a, b);
        if (is_min) {
          if (out != a) {
            __ Movn(out, a, AT);
          }
          if (out != b) {
            __ Movz(out, b, AT);
          }
        } else {
          if (out != a) {
            __ Movz(out, a, AT);
          }
          if (out != b) {
            __ Movn(out, b, AT);
          }
        }
      }
    }
  }
}

// int java.lang.Math.min(int, int)
void IntrinsicLocationsBuilderMIPS::VisitMathMinIntInt(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitMathMinIntInt(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(),
            /* is_min */ true,
            Primitive::kPrimInt,
            IsR6(),
            GetAssembler());
}

// long java.lang.Math.min(long, long)
void IntrinsicLocationsBuilderMIPS::VisitMathMinLongLong(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitMathMinLongLong(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(),
            /* is_min */ true,
            Primitive::kPrimLong,
            IsR6(),
            GetAssembler());
}

// int java.lang.Math.max(int, int)
void IntrinsicLocationsBuilderMIPS::VisitMathMaxIntInt(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitMathMaxIntInt(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(),
            /* is_min */ false,
            Primitive::kPrimInt,
            IsR6(),
            GetAssembler());
}

// long java.lang.Math.max(long, long)
void IntrinsicLocationsBuilderMIPS::VisitMathMaxLongLong(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitMathMaxLongLong(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(),
            /* is_min */ false,
            Primitive::kPrimLong,
            IsR6(),
            GetAssembler());
}

// double java.lang.Math.sqrt(double)
void IntrinsicLocationsBuilderMIPS::VisitMathSqrt(HInvoke* invoke) {
  CreateFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitMathSqrt(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  MipsAssembler* assembler = GetAssembler();
  FRegister in = locations->InAt(0).AsFpuRegister<FRegister>();
  FRegister out = locations->Out().AsFpuRegister<FRegister>();

  __ SqrtD(out, in);
}

// byte libcore.io.Memory.peekByte(long address)
void IntrinsicLocationsBuilderMIPS::VisitMemoryPeekByte(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitMemoryPeekByte(HInvoke* invoke) {
  MipsAssembler* assembler = GetAssembler();
  Register adr = invoke->GetLocations()->InAt(0).AsRegisterPairLow<Register>();
  Register out = invoke->GetLocations()->Out().AsRegister<Register>();

  __ Lb(out, adr, 0);
}

// short libcore.io.Memory.peekShort(long address)
void IntrinsicLocationsBuilderMIPS::VisitMemoryPeekShortNative(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitMemoryPeekShortNative(HInvoke* invoke) {
  MipsAssembler* assembler = GetAssembler();
  Register adr = invoke->GetLocations()->InAt(0).AsRegisterPairLow<Register>();
  Register out = invoke->GetLocations()->Out().AsRegister<Register>();

  if (IsR6()) {
    __ Lh(out, adr, 0);
  } else if (IsR2OrNewer()) {
    // Unlike for words, there are no lhl/lhr instructions to load
    // unaligned halfwords so the code loads individual bytes, in case
    // the address isn't halfword-aligned, and assembles them into a
    // signed halfword.
    __ Lb(AT, adr, 1);   // This byte must be sign-extended.
    __ Lb(out, adr, 0);  // This byte can be either sign-extended, or
                         // zero-extended because the following
                         // instruction overwrites the sign bits.
    __ Ins(out, AT, 8, 24);
  } else {
    __ Lbu(AT, adr, 0);  // This byte must be zero-extended.  If it's not
                         // the "or" instruction below will destroy the upper
                         // 24 bits of the final result.
    __ Lb(out, adr, 1);  // This byte must be sign-extended.
    __ Sll(out, out, 8);
    __ Or(out, out, AT);
  }
}

// int libcore.io.Memory.peekInt(long address)
void IntrinsicLocationsBuilderMIPS::VisitMemoryPeekIntNative(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke, Location::kOutputOverlap);
}

void IntrinsicCodeGeneratorMIPS::VisitMemoryPeekIntNative(HInvoke* invoke) {
  MipsAssembler* assembler = GetAssembler();
  Register adr = invoke->GetLocations()->InAt(0).AsRegisterPairLow<Register>();
  Register out = invoke->GetLocations()->Out().AsRegister<Register>();

  if (IsR6()) {
    __ Lw(out, adr, 0);
  } else {
    __ Lwr(out, adr, 0);
    __ Lwl(out, adr, 3);
  }
}

// long libcore.io.Memory.peekLong(long address)
void IntrinsicLocationsBuilderMIPS::VisitMemoryPeekLongNative(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke, Location::kOutputOverlap);
}

void IntrinsicCodeGeneratorMIPS::VisitMemoryPeekLongNative(HInvoke* invoke) {
  MipsAssembler* assembler = GetAssembler();
  Register adr = invoke->GetLocations()->InAt(0).AsRegisterPairLow<Register>();
  Register out_lo = invoke->GetLocations()->Out().AsRegisterPairLow<Register>();
  Register out_hi = invoke->GetLocations()->Out().AsRegisterPairHigh<Register>();

  if (IsR6()) {
    __ Lw(out_lo, adr, 0);
    __ Lw(out_hi, adr, 4);
  } else {
    __ Lwr(out_lo, adr, 0);
    __ Lwl(out_lo, adr, 3);
    __ Lwr(out_hi, adr, 4);
    __ Lwl(out_hi, adr, 7);
  }
}

static void CreateIntIntToVoidLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
}

// void libcore.io.Memory.pokeByte(long address, byte value)
void IntrinsicLocationsBuilderMIPS::VisitMemoryPokeByte(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitMemoryPokeByte(HInvoke* invoke) {
  MipsAssembler* assembler = GetAssembler();
  Register adr = invoke->GetLocations()->InAt(0).AsRegisterPairLow<Register>();
  Register val = invoke->GetLocations()->InAt(1).AsRegister<Register>();

  __ Sb(val, adr, 0);
}

// void libcore.io.Memory.pokeShort(long address, short value)
void IntrinsicLocationsBuilderMIPS::VisitMemoryPokeShortNative(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitMemoryPokeShortNative(HInvoke* invoke) {
  MipsAssembler* assembler = GetAssembler();
  Register adr = invoke->GetLocations()->InAt(0).AsRegisterPairLow<Register>();
  Register val = invoke->GetLocations()->InAt(1).AsRegister<Register>();

  if (IsR6()) {
    __ Sh(val, adr, 0);
  } else {
    // Unlike for words, there are no shl/shr instructions to store
    // unaligned halfwords so the code stores individual bytes, in case
    // the address isn't halfword-aligned.
    __ Sb(val, adr, 0);
    __ Srl(AT, val, 8);
    __ Sb(AT, adr, 1);
  }
}

// void libcore.io.Memory.pokeInt(long address, int value)
void IntrinsicLocationsBuilderMIPS::VisitMemoryPokeIntNative(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitMemoryPokeIntNative(HInvoke* invoke) {
  MipsAssembler* assembler = GetAssembler();
  Register adr = invoke->GetLocations()->InAt(0).AsRegisterPairLow<Register>();
  Register val = invoke->GetLocations()->InAt(1).AsRegister<Register>();

  if (IsR6()) {
    __ Sw(val, adr, 0);
  } else {
    __ Swr(val, adr, 0);
    __ Swl(val, adr, 3);
  }
}

// void libcore.io.Memory.pokeLong(long address, long value)
void IntrinsicLocationsBuilderMIPS::VisitMemoryPokeLongNative(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitMemoryPokeLongNative(HInvoke* invoke) {
  MipsAssembler* assembler = GetAssembler();
  Register adr = invoke->GetLocations()->InAt(0).AsRegisterPairLow<Register>();
  Register val_lo = invoke->GetLocations()->InAt(1).AsRegisterPairLow<Register>();
  Register val_hi = invoke->GetLocations()->InAt(1).AsRegisterPairHigh<Register>();

  if (IsR6()) {
    __ Sw(val_lo, adr, 0);
    __ Sw(val_hi, adr, 4);
  } else {
    __ Swr(val_lo, adr, 0);
    __ Swl(val_lo, adr, 3);
    __ Swr(val_hi, adr, 4);
    __ Swl(val_hi, adr, 7);
  }
}

// Thread java.lang.Thread.currentThread()
void IntrinsicLocationsBuilderMIPS::VisitThreadCurrentThread(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetOut(Location::RequiresRegister());
}

void IntrinsicCodeGeneratorMIPS::VisitThreadCurrentThread(HInvoke* invoke) {
  MipsAssembler* assembler = GetAssembler();
  Register out = invoke->GetLocations()->Out().AsRegister<Register>();

  __ LoadFromOffset(kLoadWord,
                    out,
                    TR,
                    Thread::PeerOffset<kMipsPointerSize>().Int32Value());
}

static void CreateIntIntIntToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  bool can_call =
       invoke->GetIntrinsic() == Intrinsics::kUnsafeGetObject ||
       invoke->GetIntrinsic() == Intrinsics::kUnsafeGetObjectVolatile;
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           can_call ?
                                                               LocationSummary::kCallOnSlowPath :
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
                         bool is_R6,
                         CodeGeneratorMIPS* codegen) {
  LocationSummary* locations = invoke->GetLocations();
  DCHECK((type == Primitive::kPrimInt) ||
         (type == Primitive::kPrimLong) ||
         (type == Primitive::kPrimNot)) << type;
  MipsAssembler* assembler = codegen->GetAssembler();
  // Object pointer.
  Register base = locations->InAt(1).AsRegister<Register>();
  // The "offset" argument is passed as a "long". Since this code is for
  // a 32-bit processor, we can only use 32-bit addresses, so we only
  // need the low 32-bits of offset.
  Register offset_lo = invoke->GetLocations()->InAt(2).AsRegisterPairLow<Register>();

  __ Addu(TMP, base, offset_lo);
  if (is_volatile) {
    __ Sync(0);
  }
  if (type == Primitive::kPrimLong) {
    Register trg_lo = locations->Out().AsRegisterPairLow<Register>();
    Register trg_hi = locations->Out().AsRegisterPairHigh<Register>();

    if (is_R6) {
      __ Lw(trg_lo, TMP, 0);
      __ Lw(trg_hi, TMP, 4);
    } else {
      __ Lwr(trg_lo, TMP, 0);
      __ Lwl(trg_lo, TMP, 3);
      __ Lwr(trg_hi, TMP, 4);
      __ Lwl(trg_hi, TMP, 7);
    }
  } else {
    Register trg = locations->Out().AsRegister<Register>();

    if (is_R6) {
      __ Lw(trg, TMP, 0);
    } else {
      __ Lwr(trg, TMP, 0);
      __ Lwl(trg, TMP, 3);
    }
  }
}

// int sun.misc.Unsafe.getInt(Object o, long offset)
void IntrinsicLocationsBuilderMIPS::VisitUnsafeGet(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitUnsafeGet(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimInt, /* is_volatile */ false, IsR6(), codegen_);
}

// int sun.misc.Unsafe.getIntVolatile(Object o, long offset)
void IntrinsicLocationsBuilderMIPS::VisitUnsafeGetVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitUnsafeGetVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimInt, /* is_volatile */ true, IsR6(), codegen_);
}

// long sun.misc.Unsafe.getLong(Object o, long offset)
void IntrinsicLocationsBuilderMIPS::VisitUnsafeGetLong(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitUnsafeGetLong(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimLong, /* is_volatile */ false, IsR6(), codegen_);
}

// long sun.misc.Unsafe.getLongVolatile(Object o, long offset)
void IntrinsicLocationsBuilderMIPS::VisitUnsafeGetLongVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitUnsafeGetLongVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimLong, /* is_volatile */ true, IsR6(), codegen_);
}

// Object sun.misc.Unsafe.getObject(Object o, long offset)
void IntrinsicLocationsBuilderMIPS::VisitUnsafeGetObject(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitUnsafeGetObject(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimNot, /* is_volatile */ false, IsR6(), codegen_);
}

// Object sun.misc.Unsafe.getObjectVolatile(Object o, long offset)
void IntrinsicLocationsBuilderMIPS::VisitUnsafeGetObjectVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitUnsafeGetObjectVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimNot, /* is_volatile */ true, IsR6(), codegen_);
}

static void CreateIntIntIntIntToVoidLocations(ArenaAllocator* arena, HInvoke* invoke) {
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
                         bool is_R6,
                         CodeGeneratorMIPS* codegen) {
  DCHECK((type == Primitive::kPrimInt) ||
         (type == Primitive::kPrimLong) ||
         (type == Primitive::kPrimNot)) << type;
  MipsAssembler* assembler = codegen->GetAssembler();
  // Object pointer.
  Register base = locations->InAt(1).AsRegister<Register>();
  // The "offset" argument is passed as a "long", i.e., it's 64-bits in
  // size. Since this code is for a 32-bit processor, we can only use
  // 32-bit addresses, so we only need the low 32-bits of offset.
  Register offset_lo = locations->InAt(2).AsRegisterPairLow<Register>();

  __ Addu(TMP, base, offset_lo);
  if (is_volatile || is_ordered) {
    __ Sync(0);
  }
  if ((type == Primitive::kPrimInt) || (type == Primitive::kPrimNot)) {
    Register value = locations->InAt(3).AsRegister<Register>();

    if (is_R6) {
      __ Sw(value, TMP, 0);
    } else {
      __ Swr(value, TMP, 0);
      __ Swl(value, TMP, 3);
    }
  } else {
    Register value_lo = locations->InAt(3).AsRegisterPairLow<Register>();
    Register value_hi = locations->InAt(3).AsRegisterPairHigh<Register>();

    if (is_R6) {
      __ Sw(value_lo, TMP, 0);
      __ Sw(value_hi, TMP, 4);
    } else {
      __ Swr(value_lo, TMP, 0);
      __ Swl(value_lo, TMP, 3);
      __ Swr(value_hi, TMP, 4);
      __ Swl(value_hi, TMP, 7);
    }
  }

  if (is_volatile) {
    __ Sync(0);
  }

  if (type == Primitive::kPrimNot) {
    codegen->MarkGCCard(base, locations->InAt(3).AsRegister<Register>());
  }
}

// void sun.misc.Unsafe.putInt(Object o, long offset, int x)
void IntrinsicLocationsBuilderMIPS::VisitUnsafePut(HInvoke* invoke) {
  CreateIntIntIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitUnsafePut(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimInt,
               /* is_volatile */ false,
               /* is_ordered */ false,
               IsR6(),
               codegen_);
}

// void sun.misc.Unsafe.putOrderedInt(Object o, long offset, int x)
void IntrinsicLocationsBuilderMIPS::VisitUnsafePutOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitUnsafePutOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimInt,
               /* is_volatile */ false,
               /* is_ordered */ true,
               IsR6(),
               codegen_);
}

// void sun.misc.Unsafe.putIntVolatile(Object o, long offset, int x)
void IntrinsicLocationsBuilderMIPS::VisitUnsafePutVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitUnsafePutVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimInt,
               /* is_volatile */ true,
               /* is_ordered */ false,
               IsR6(),
               codegen_);
}

// void sun.misc.Unsafe.putObject(Object o, long offset, Object x)
void IntrinsicLocationsBuilderMIPS::VisitUnsafePutObject(HInvoke* invoke) {
  CreateIntIntIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitUnsafePutObject(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimNot,
               /* is_volatile */ false,
               /* is_ordered */ false,
               IsR6(),
               codegen_);
}

// void sun.misc.Unsafe.putOrderedObject(Object o, long offset, Object x)
void IntrinsicLocationsBuilderMIPS::VisitUnsafePutObjectOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitUnsafePutObjectOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimNot,
               /* is_volatile */ false,
               /* is_ordered */ true,
               IsR6(),
               codegen_);
}

// void sun.misc.Unsafe.putObjectVolatile(Object o, long offset, Object x)
void IntrinsicLocationsBuilderMIPS::VisitUnsafePutObjectVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitUnsafePutObjectVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimNot,
               /* is_volatile */ true,
               /* is_ordered */ false,
               IsR6(),
               codegen_);
}

// void sun.misc.Unsafe.putLong(Object o, long offset, long x)
void IntrinsicLocationsBuilderMIPS::VisitUnsafePutLong(HInvoke* invoke) {
  CreateIntIntIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitUnsafePutLong(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimLong,
               /* is_volatile */ false,
               /* is_ordered */ false,
               IsR6(),
               codegen_);
}

// void sun.misc.Unsafe.putOrderedLong(Object o, long offset, long x)
void IntrinsicLocationsBuilderMIPS::VisitUnsafePutLongOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitUnsafePutLongOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimLong,
               /* is_volatile */ false,
               /* is_ordered */ true,
               IsR6(),
               codegen_);
}

// void sun.misc.Unsafe.putLongVolatile(Object o, long offset, long x)
void IntrinsicLocationsBuilderMIPS::VisitUnsafePutLongVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitUnsafePutLongVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimLong,
               /* is_volatile */ true,
               /* is_ordered */ false,
               IsR6(),
               codegen_);
}

static void CreateIntIntIntIntIntToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
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

static void GenCas(LocationSummary* locations, Primitive::Type type, CodeGeneratorMIPS* codegen) {
  MipsAssembler* assembler = codegen->GetAssembler();
  bool isR6 = codegen->GetInstructionSetFeatures().IsR6();
  Register base = locations->InAt(1).AsRegister<Register>();
  Register offset_lo = locations->InAt(2).AsRegisterPairLow<Register>();
  Register expected = locations->InAt(3).AsRegister<Register>();
  Register value = locations->InAt(4).AsRegister<Register>();
  Register out = locations->Out().AsRegister<Register>();

  DCHECK_NE(base, out);
  DCHECK_NE(offset_lo, out);
  DCHECK_NE(expected, out);

  if (type == Primitive::kPrimNot) {
    // Mark card for object assuming new value is stored.
    codegen->MarkGCCard(base, value);
  }

  // do {
  //   tmp_value = [tmp_ptr] - expected;
  // } while (tmp_value == 0 && failure([tmp_ptr] <- r_new_value));
  // result = tmp_value != 0;

  MipsLabel loop_head, exit_loop;
  __ Addu(TMP, base, offset_lo);
  __ Sync(0);
  __ Bind(&loop_head);
  if ((type == Primitive::kPrimInt) || (type == Primitive::kPrimNot)) {
    if (isR6) {
      __ LlR6(out, TMP);
    } else {
      __ LlR2(out, TMP);
    }
  } else {
      LOG(FATAL) << "Unsupported op size " << type;
      UNREACHABLE();
  }
  __ Subu(out, out, expected);          // If we didn't get the 'expected'
  __ Sltiu(out, out, 1);                // value, set 'out' to false, and
  __ Beqz(out, &exit_loop);             // return.
  __ Move(out, value);  // Use 'out' for the 'store conditional' instruction.
                        // If we use 'value' directly, we would lose 'value'
                        // in the case that the store fails.  Whether the
                        // store succeeds, or fails, it will load the
                        // correct boolean value into the 'out' register.
  // This test isn't really necessary. We only support Primitive::kPrimInt,
  // Primitive::kPrimNot, and we already verified that we're working on one
  // of those two types. It's left here in case the code needs to support
  // other types in the future.
  if ((type == Primitive::kPrimInt) || (type == Primitive::kPrimNot)) {
    if (isR6) {
      __ ScR6(out, TMP);
    } else {
      __ ScR2(out, TMP);
    }
  }
  __ Beqz(out, &loop_head);     // If we couldn't do the read-modify-write
                                // cycle atomically then retry.
  __ Bind(&exit_loop);
  __ Sync(0);
}

// boolean sun.misc.Unsafe.compareAndSwapInt(Object o, long offset, int expected, int x)
void IntrinsicLocationsBuilderMIPS::VisitUnsafeCASInt(HInvoke* invoke) {
  CreateIntIntIntIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitUnsafeCASInt(HInvoke* invoke) {
  GenCas(invoke->GetLocations(), Primitive::kPrimInt, codegen_);
}

// boolean sun.misc.Unsafe.compareAndSwapObject(Object o, long offset, Object expected, Object x)
void IntrinsicLocationsBuilderMIPS::VisitUnsafeCASObject(HInvoke* invoke) {
  CreateIntIntIntIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitUnsafeCASObject(HInvoke* invoke) {
  GenCas(invoke->GetLocations(), Primitive::kPrimNot, codegen_);
}

// char java.lang.String.charAt(int index)
void IntrinsicLocationsBuilderMIPS::VisitStringCharAt(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCallOnSlowPath,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  // The inputs will be considered live at the last instruction and restored. This would overwrite
  // the output with kNoOutputOverlap.
  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
}

void IntrinsicCodeGeneratorMIPS::VisitStringCharAt(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  MipsAssembler* assembler = GetAssembler();

  // Location of reference to data array
  const int32_t value_offset = mirror::String::ValueOffset().Int32Value();
  // Location of count
  const int32_t count_offset = mirror::String::CountOffset().Int32Value();

  Register obj = locations->InAt(0).AsRegister<Register>();
  Register idx = locations->InAt(1).AsRegister<Register>();
  Register out = locations->Out().AsRegister<Register>();

  // TODO: Maybe we can support range check elimination. Overall,
  //       though, I think it's not worth the cost.
  // TODO: For simplicity, the index parameter is requested in a
  //       register, so different from Quick we will not optimize the
  //       code for constants (which would save a register).

  SlowPathCodeMIPS* slow_path = new (GetAllocator()) IntrinsicSlowPathMIPS(invoke);
  codegen_->AddSlowPath(slow_path);

  // Load the string size
  __ Lw(TMP, obj, count_offset);
  codegen_->MaybeRecordImplicitNullCheck(invoke);
  // Revert to slow path if idx is too large, or negative
  __ Bgeu(idx, TMP, slow_path->GetEntryLabel());

  // out = obj[2*idx].
  __ Sll(TMP, idx, 1);                  // idx * 2
  __ Addu(TMP, TMP, obj);               // Address of char at location idx
  __ Lhu(out, TMP, value_offset);       // Load char at location idx

  __ Bind(slow_path->GetExitLabel());
}

// int java.lang.String.compareTo(String anotherString)
void IntrinsicLocationsBuilderMIPS::VisitStringCompareTo(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  Location outLocation = calling_convention.GetReturnLocation(Primitive::kPrimInt);
  locations->SetOut(Location::RegisterLocation(outLocation.AsRegister<Register>()));
}

void IntrinsicCodeGeneratorMIPS::VisitStringCompareTo(HInvoke* invoke) {
  MipsAssembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  Register argument = locations->InAt(1).AsRegister<Register>();
  SlowPathCodeMIPS* slow_path = new (GetAllocator()) IntrinsicSlowPathMIPS(invoke);
  codegen_->AddSlowPath(slow_path);
  __ Beqz(argument, slow_path->GetEntryLabel());

  __ LoadFromOffset(kLoadWord,
                    T9,
                    TR,
                    QUICK_ENTRYPOINT_OFFSET(kMipsWordSize,
                                            pStringCompareTo).Int32Value());
  __ Jalr(T9);
  __ Nop();
  __ Bind(slow_path->GetExitLabel());
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

static void GenerateStringIndexOf(HInvoke* invoke,
                                  bool start_at_zero,
                                  MipsAssembler* assembler,
                                  CodeGeneratorMIPS* codegen,
                                  ArenaAllocator* allocator) {
  LocationSummary* locations = invoke->GetLocations();
  Register tmp_reg = start_at_zero ? locations->GetTemp(0).AsRegister<Register>() : TMP;

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  // Check for code points > 0xFFFF. Either a slow-path check when we
  // don't know statically, or directly dispatch if we have a constant.
  SlowPathCodeMIPS* slow_path = nullptr;
  if (invoke->InputAt(1)->IsIntConstant()) {
    if (!IsUint<16>(invoke->InputAt(1)->AsIntConstant()->GetValue())) {
      // Always needs the slow-path. We could directly dispatch to it,
      // but this case should be rare, so for simplicity just put the
      // full slow-path down and branch unconditionally.
      slow_path = new (allocator) IntrinsicSlowPathMIPS(invoke);
      codegen->AddSlowPath(slow_path);
      __ B(slow_path->GetEntryLabel());
      __ Bind(slow_path->GetExitLabel());
      return;
    }
  } else {
    Register char_reg = locations->InAt(1).AsRegister<Register>();
    // The "bltu" conditional branch tests to see if the character value
    // fits in a valid 16-bit (MIPS halfword) value. If it doesn't then
    // the character being searched for, if it exists in the string, is
    // encoded using UTF-16 and stored in the string as two (16-bit)
    // halfwords. Currently the assembly code used to implement this
    // intrinsic doesn't support searching for a character stored as
    // two halfwords so we fallback to using the generic implementation
    // of indexOf().
    __ LoadConst32(tmp_reg, std::numeric_limits<uint16_t>::max());
    slow_path = new (allocator) IntrinsicSlowPathMIPS(invoke);
    codegen->AddSlowPath(slow_path);
    __ Bltu(tmp_reg, char_reg, slow_path->GetEntryLabel());
  }

  if (start_at_zero) {
    DCHECK_EQ(tmp_reg, A2);
    // Start-index = 0.
    __ Clear(tmp_reg);
  }

  __ LoadFromOffset(kLoadWord,
                    T9,
                    TR,
                    QUICK_ENTRYPOINT_OFFSET(kMipsWordSize, pIndexOf).Int32Value());
  __ Jalr(T9);
  __ Nop();

  if (slow_path != nullptr) {
    __ Bind(slow_path->GetExitLabel());
  }
}

// int java.lang.String.indexOf(int ch)
void IntrinsicLocationsBuilderMIPS::VisitStringIndexOf(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  // We have a hand-crafted assembly stub that follows the runtime
  // calling convention. So it's best to align the inputs accordingly.
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  Location outLocation = calling_convention.GetReturnLocation(Primitive::kPrimInt);
  locations->SetOut(Location::RegisterLocation(outLocation.AsRegister<Register>()));

  // Need a temp for slow-path codepoint compare, and need to send start-index=0.
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
}

void IntrinsicCodeGeneratorMIPS::VisitStringIndexOf(HInvoke* invoke) {
  GenerateStringIndexOf(invoke,
                        /* start_at_zero */ true,
                        GetAssembler(),
                        codegen_,
                        GetAllocator());
}

// int java.lang.String.indexOf(int ch, int fromIndex)
void IntrinsicLocationsBuilderMIPS::VisitStringIndexOfAfter(HInvoke* invoke) {
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
  locations->SetOut(Location::RegisterLocation(outLocation.AsRegister<Register>()));

  // Need a temp for slow-path codepoint compare.
  locations->AddTemp(Location::RequiresRegister());
}

void IntrinsicCodeGeneratorMIPS::VisitStringIndexOfAfter(HInvoke* invoke) {
  GenerateStringIndexOf(invoke,
                        /* start_at_zero */ false,
                        GetAssembler(),
                        codegen_,
                        GetAllocator());
}

// java.lang.StringFactory.newStringFromBytes(byte[] data, int high, int offset, int byteCount)
void IntrinsicLocationsBuilderMIPS::VisitStringNewStringFromBytes(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(2, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  locations->SetInAt(3, Location::RegisterLocation(calling_convention.GetRegisterAt(3)));
  Location outLocation = calling_convention.GetReturnLocation(Primitive::kPrimInt);
  locations->SetOut(Location::RegisterLocation(outLocation.AsRegister<Register>()));
}

void IntrinsicCodeGeneratorMIPS::VisitStringNewStringFromBytes(HInvoke* invoke) {
  MipsAssembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  Register byte_array = locations->InAt(0).AsRegister<Register>();
  SlowPathCodeMIPS* slow_path = new (GetAllocator()) IntrinsicSlowPathMIPS(invoke);
  codegen_->AddSlowPath(slow_path);
  __ Beqz(byte_array, slow_path->GetEntryLabel());

  __ LoadFromOffset(kLoadWord,
                    T9,
                    TR,
                    QUICK_ENTRYPOINT_OFFSET(kMipsWordSize, pAllocStringFromBytes).Int32Value());
  __ Jalr(T9);
  __ Nop();
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
  __ Bind(slow_path->GetExitLabel());
}

// java.lang.StringFactory.newStringFromChars(int offset, int charCount, char[] data)
void IntrinsicLocationsBuilderMIPS::VisitStringNewStringFromChars(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(2, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  Location outLocation = calling_convention.GetReturnLocation(Primitive::kPrimInt);
  locations->SetOut(Location::RegisterLocation(outLocation.AsRegister<Register>()));
}

void IntrinsicCodeGeneratorMIPS::VisitStringNewStringFromChars(HInvoke* invoke) {
  MipsAssembler* assembler = GetAssembler();

  // No need to emit code checking whether `locations->InAt(2)` is a null
  // pointer, as callers of the native method
  //
  //   java.lang.StringFactory.newStringFromChars(int offset, int charCount, char[] data)
  //
  // all include a null check on `data` before calling that method.

  __ LoadFromOffset(kLoadWord,
                    T9,
                    TR,
                    QUICK_ENTRYPOINT_OFFSET(kMipsWordSize, pAllocStringFromChars).Int32Value());
  __ Jalr(T9);
  __ Nop();
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

// java.lang.StringFactory.newStringFromString(String toCopy)
void IntrinsicLocationsBuilderMIPS::VisitStringNewStringFromString(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  Location outLocation = calling_convention.GetReturnLocation(Primitive::kPrimInt);
  locations->SetOut(Location::RegisterLocation(outLocation.AsRegister<Register>()));
}

void IntrinsicCodeGeneratorMIPS::VisitStringNewStringFromString(HInvoke* invoke) {
  MipsAssembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  Register string_to_copy = locations->InAt(0).AsRegister<Register>();
  SlowPathCodeMIPS* slow_path = new (GetAllocator()) IntrinsicSlowPathMIPS(invoke);
  codegen_->AddSlowPath(slow_path);
  __ Beqz(string_to_copy, slow_path->GetEntryLabel());

  __ LoadFromOffset(kLoadWord,
                    T9,
                    TR,
                    QUICK_ENTRYPOINT_OFFSET(kMipsWordSize, pAllocStringFromString).Int32Value());
  __ Jalr(T9);
  __ Nop();
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
  __ Bind(slow_path->GetExitLabel());
}

static void GenIsInfinite(LocationSummary* locations,
                          const Primitive::Type type,
                          const bool isR6,
                          MipsAssembler* assembler) {
  FRegister in = locations->InAt(0).AsFpuRegister<FRegister>();
  Register out = locations->Out().AsRegister<Register>();

  DCHECK(type == Primitive::kPrimFloat || type == Primitive::kPrimDouble);

  if (isR6) {
    if (type == Primitive::kPrimDouble) {
        __ ClassD(FTMP, in);
    } else {
        __ ClassS(FTMP, in);
    }
    __ Mfc1(out, FTMP);
    __ Andi(out, out, kPositiveInfinity | kNegativeInfinity);
    __ Sltu(out, ZERO, out);
  } else {
    // If one, or more, of the exponent bits is zero, then the number can't be infinite.
    if (type == Primitive::kPrimDouble) {
      __ MoveFromFpuHigh(TMP, in);
      __ LoadConst32(AT, 0x7FF00000);
    } else {
      __ Mfc1(TMP, in);
      __ LoadConst32(AT, 0x7F800000);
    }
    __ Xor(TMP, TMP, AT);

    __ Sll(TMP, TMP, 1);

    if (type == Primitive::kPrimDouble) {
      __ Mfc1(AT, in);
      __ Or(TMP, TMP, AT);
    }
    // If any of the significand bits are one, then the number is not infinite.
    __ Sltiu(out, TMP, 1);
  }
}

// boolean java.lang.Float.isInfinite(float)
void IntrinsicLocationsBuilderMIPS::VisitFloatIsInfinite(HInvoke* invoke) {
  CreateFPToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitFloatIsInfinite(HInvoke* invoke) {
  GenIsInfinite(invoke->GetLocations(), Primitive::kPrimFloat, IsR6(), GetAssembler());
}

// boolean java.lang.Double.isInfinite(double)
void IntrinsicLocationsBuilderMIPS::VisitDoubleIsInfinite(HInvoke* invoke) {
  CreateFPToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitDoubleIsInfinite(HInvoke* invoke) {
  GenIsInfinite(invoke->GetLocations(), Primitive::kPrimDouble, IsR6(), GetAssembler());
}

static void GenHighestOneBit(LocationSummary* locations,
                             const Primitive::Type type,
                             bool isR6,
                             MipsAssembler* assembler) {
  DCHECK(type == Primitive::kPrimInt || type == Primitive::kPrimLong);

  if (type == Primitive::kPrimLong) {
    Register in_lo = locations->InAt(0).AsRegisterPairLow<Register>();
    Register in_hi = locations->InAt(0).AsRegisterPairHigh<Register>();
    Register out_lo = locations->Out().AsRegisterPairLow<Register>();
    Register out_hi = locations->Out().AsRegisterPairHigh<Register>();

    if (isR6) {
      __ ClzR6(TMP, in_hi);
    } else {
      __ ClzR2(TMP, in_hi);
    }
    __ LoadConst32(AT, 0x80000000);
    __ Srlv(out_hi, AT, TMP);
    __ And(out_hi, out_hi, in_hi);
    if (isR6) {
      __ ClzR6(TMP, in_lo);
    } else {
      __ ClzR2(TMP, in_lo);
    }
    __ Srlv(out_lo, AT, TMP);
    __ And(out_lo, out_lo, in_lo);
    if (isR6) {
      __ Seleqz(out_lo, out_lo, out_hi);
    } else {
      __ Movn(out_lo, ZERO, out_hi);
    }
  } else {
    Register in = locations->InAt(0).AsRegister<Register>();
    Register out = locations->Out().AsRegister<Register>();

    if (isR6) {
      __ ClzR6(TMP, in);
    } else {
      __ ClzR2(TMP, in);
    }
    __ LoadConst32(AT, 0x80000000);
    __ Srlv(AT, AT, TMP);  // Srlv shifts in the range of [0;31] bits (lower 5 bits of arg).
    __ And(out, AT, in);   // So this is required for 0 (=shift by 32).
  }
}

// int java.lang.Integer.highestOneBit(int)
void IntrinsicLocationsBuilderMIPS::VisitIntegerHighestOneBit(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitIntegerHighestOneBit(HInvoke* invoke) {
  GenHighestOneBit(invoke->GetLocations(), Primitive::kPrimInt, IsR6(), GetAssembler());
}

// long java.lang.Long.highestOneBit(long)
void IntrinsicLocationsBuilderMIPS::VisitLongHighestOneBit(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke, Location::kOutputOverlap);
}

void IntrinsicCodeGeneratorMIPS::VisitLongHighestOneBit(HInvoke* invoke) {
  GenHighestOneBit(invoke->GetLocations(), Primitive::kPrimLong, IsR6(), GetAssembler());
}

static void GenLowestOneBit(LocationSummary* locations,
                            const Primitive::Type type,
                            bool isR6,
                            MipsAssembler* assembler) {
  DCHECK(type == Primitive::kPrimInt || type == Primitive::kPrimLong);

  if (type == Primitive::kPrimLong) {
    Register in_lo = locations->InAt(0).AsRegisterPairLow<Register>();
    Register in_hi = locations->InAt(0).AsRegisterPairHigh<Register>();
    Register out_lo = locations->Out().AsRegisterPairLow<Register>();
    Register out_hi = locations->Out().AsRegisterPairHigh<Register>();

    __ Subu(TMP, ZERO, in_lo);
    __ And(out_lo, TMP, in_lo);
    __ Subu(TMP, ZERO, in_hi);
    __ And(out_hi, TMP, in_hi);
    if (isR6) {
      __ Seleqz(out_hi, out_hi, out_lo);
    } else {
      __ Movn(out_hi, ZERO, out_lo);
    }
  } else {
    Register in = locations->InAt(0).AsRegister<Register>();
    Register out = locations->Out().AsRegister<Register>();

    __ Subu(TMP, ZERO, in);
    __ And(out, TMP, in);
  }
}

// int java.lang.Integer.lowestOneBit(int)
void IntrinsicLocationsBuilderMIPS::VisitIntegerLowestOneBit(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitIntegerLowestOneBit(HInvoke* invoke) {
  GenLowestOneBit(invoke->GetLocations(), Primitive::kPrimInt, IsR6(), GetAssembler());
}

// long java.lang.Long.lowestOneBit(long)
void IntrinsicLocationsBuilderMIPS::VisitLongLowestOneBit(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorMIPS::VisitLongLowestOneBit(HInvoke* invoke) {
  GenLowestOneBit(invoke->GetLocations(), Primitive::kPrimLong, IsR6(), GetAssembler());
}

// Unimplemented intrinsics.

UNIMPLEMENTED_INTRINSIC(MIPS, MathCeil)
UNIMPLEMENTED_INTRINSIC(MIPS, MathFloor)
UNIMPLEMENTED_INTRINSIC(MIPS, MathRint)
UNIMPLEMENTED_INTRINSIC(MIPS, MathRoundDouble)
UNIMPLEMENTED_INTRINSIC(MIPS, MathRoundFloat)
UNIMPLEMENTED_INTRINSIC(MIPS, UnsafeCASLong)

UNIMPLEMENTED_INTRINSIC(MIPS, ReferenceGetReferent)
UNIMPLEMENTED_INTRINSIC(MIPS, StringGetCharsNoCheck)
UNIMPLEMENTED_INTRINSIC(MIPS, SystemArrayCopyChar)
UNIMPLEMENTED_INTRINSIC(MIPS, SystemArrayCopy)

UNIMPLEMENTED_INTRINSIC(MIPS, MathCos)
UNIMPLEMENTED_INTRINSIC(MIPS, MathSin)
UNIMPLEMENTED_INTRINSIC(MIPS, MathAcos)
UNIMPLEMENTED_INTRINSIC(MIPS, MathAsin)
UNIMPLEMENTED_INTRINSIC(MIPS, MathAtan)
UNIMPLEMENTED_INTRINSIC(MIPS, MathAtan2)
UNIMPLEMENTED_INTRINSIC(MIPS, MathCbrt)
UNIMPLEMENTED_INTRINSIC(MIPS, MathCosh)
UNIMPLEMENTED_INTRINSIC(MIPS, MathExp)
UNIMPLEMENTED_INTRINSIC(MIPS, MathExpm1)
UNIMPLEMENTED_INTRINSIC(MIPS, MathHypot)
UNIMPLEMENTED_INTRINSIC(MIPS, MathLog)
UNIMPLEMENTED_INTRINSIC(MIPS, MathLog10)
UNIMPLEMENTED_INTRINSIC(MIPS, MathNextAfter)
UNIMPLEMENTED_INTRINSIC(MIPS, MathSinh)
UNIMPLEMENTED_INTRINSIC(MIPS, MathTan)
UNIMPLEMENTED_INTRINSIC(MIPS, MathTanh)

// 1.8.
UNIMPLEMENTED_INTRINSIC(MIPS, UnsafeGetAndAddInt)
UNIMPLEMENTED_INTRINSIC(MIPS, UnsafeGetAndAddLong)
UNIMPLEMENTED_INTRINSIC(MIPS, UnsafeGetAndSetInt)
UNIMPLEMENTED_INTRINSIC(MIPS, UnsafeGetAndSetLong)
UNIMPLEMENTED_INTRINSIC(MIPS, UnsafeGetAndSetObject)

UNREACHABLE_INTRINSICS(MIPS)

#undef __

}  // namespace mips
}  // namespace art
