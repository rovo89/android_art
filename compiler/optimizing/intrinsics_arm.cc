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

#include "intrinsics_arm.h"

#include "arch/arm/instruction_set_features_arm.h"
#include "art_method.h"
#include "code_generator_arm.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "intrinsics.h"
#include "mirror/array-inl.h"
#include "mirror/string.h"
#include "thread.h"
#include "utils/arm/assembler_arm.h"

namespace art {

namespace arm {

ArmAssembler* IntrinsicCodeGeneratorARM::GetAssembler() {
  return codegen_->GetAssembler();
}

ArenaAllocator* IntrinsicCodeGeneratorARM::GetAllocator() {
  return codegen_->GetGraph()->GetArena();
}

#define __ codegen->GetAssembler()->

static void MoveFromReturnRegister(Location trg, Primitive::Type type, CodeGeneratorARM* codegen) {
  if (!trg.IsValid()) {
    DCHECK(type == Primitive::kPrimVoid);
    return;
  }

  DCHECK_NE(type, Primitive::kPrimVoid);

  if (Primitive::IsIntegralType(type) || type == Primitive::kPrimNot) {
    if (type == Primitive::kPrimLong) {
      Register trg_reg_lo = trg.AsRegisterPairLow<Register>();
      Register trg_reg_hi = trg.AsRegisterPairHigh<Register>();
      Register res_reg_lo = R0;
      Register res_reg_hi = R1;
      if (trg_reg_lo != res_reg_hi) {
        if (trg_reg_lo != res_reg_lo) {
          __ mov(trg_reg_lo, ShifterOperand(res_reg_lo));
          __ mov(trg_reg_hi, ShifterOperand(res_reg_hi));
        } else {
          DCHECK_EQ(trg_reg_lo + 1, trg_reg_hi);
        }
      } else {
        __ mov(trg_reg_hi, ShifterOperand(res_reg_hi));
        __ mov(trg_reg_lo, ShifterOperand(res_reg_lo));
      }
    } else {
      Register trg_reg = trg.AsRegister<Register>();
      Register res_reg = R0;
      if (trg_reg != res_reg) {
        __ mov(trg_reg, ShifterOperand(res_reg));
      }
    }
  } else {
    UNIMPLEMENTED(FATAL) << "Floating-point return.";
  }
}

static void MoveArguments(HInvoke* invoke, CodeGeneratorARM* codegen) {
  InvokeDexCallingConventionVisitorARM calling_convention_visitor;
  IntrinsicVisitor::MoveArguments(invoke, codegen, &calling_convention_visitor);
}

// Slow-path for fallback (calling the managed code to handle the intrinsic) in an intrinsified
// call. This will copy the arguments into the positions for a regular call.
//
// Note: The actual parameters are required to be in the locations given by the invoke's location
//       summary. If an intrinsic modifies those locations before a slowpath call, they must be
//       restored!
class IntrinsicSlowPathARM : public SlowPathCodeARM {
 public:
  explicit IntrinsicSlowPathARM(HInvoke* invoke) : invoke_(invoke) { }

  void EmitNativeCode(CodeGenerator* codegen_in) OVERRIDE {
    CodeGeneratorARM* codegen = down_cast<CodeGeneratorARM*>(codegen_in);
    __ Bind(GetEntryLabel());

    SaveLiveRegisters(codegen, invoke_->GetLocations());

    MoveArguments(invoke_, codegen);

    if (invoke_->IsInvokeStaticOrDirect()) {
      codegen->GenerateStaticOrDirectCall(invoke_->AsInvokeStaticOrDirect(), kArtMethodRegister);
      RecordPcInfo(codegen, invoke_, invoke_->GetDexPc());
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
    __ b(GetExitLabel());
  }

 private:
  // The instruction where this slow path is happening.
  HInvoke* const invoke_;

  DISALLOW_COPY_AND_ASSIGN(IntrinsicSlowPathARM);
};

#undef __

bool IntrinsicLocationsBuilderARM::TryDispatch(HInvoke* invoke) {
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

static void CreateIntToFPLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresFpuRegister());
}

static void MoveFPToInt(LocationSummary* locations, bool is64bit, ArmAssembler* assembler) {
  Location input = locations->InAt(0);
  Location output = locations->Out();
  if (is64bit) {
    __ vmovrrd(output.AsRegisterPairLow<Register>(),
               output.AsRegisterPairHigh<Register>(),
               FromLowSToD(input.AsFpuRegisterPairLow<SRegister>()));
  } else {
    __ vmovrs(output.AsRegister<Register>(), input.AsFpuRegister<SRegister>());
  }
}

static void MoveIntToFP(LocationSummary* locations, bool is64bit, ArmAssembler* assembler) {
  Location input = locations->InAt(0);
  Location output = locations->Out();
  if (is64bit) {
    __ vmovdrr(FromLowSToD(output.AsFpuRegisterPairLow<SRegister>()),
               input.AsRegisterPairLow<Register>(),
               input.AsRegisterPairHigh<Register>());
  } else {
    __ vmovsr(output.AsFpuRegister<SRegister>(), input.AsRegister<Register>());
  }
}

void IntrinsicLocationsBuilderARM::VisitDoubleDoubleToRawLongBits(HInvoke* invoke) {
  CreateFPToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderARM::VisitDoubleLongBitsToDouble(HInvoke* invoke) {
  CreateIntToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitDoubleDoubleToRawLongBits(HInvoke* invoke) {
  MoveFPToInt(invoke->GetLocations(), true, GetAssembler());
}
void IntrinsicCodeGeneratorARM::VisitDoubleLongBitsToDouble(HInvoke* invoke) {
  MoveIntToFP(invoke->GetLocations(), true, GetAssembler());
}

void IntrinsicLocationsBuilderARM::VisitFloatFloatToRawIntBits(HInvoke* invoke) {
  CreateFPToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderARM::VisitFloatIntBitsToFloat(HInvoke* invoke) {
  CreateIntToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitFloatFloatToRawIntBits(HInvoke* invoke) {
  MoveFPToInt(invoke->GetLocations(), false, GetAssembler());
}
void IntrinsicCodeGeneratorARM::VisitFloatIntBitsToFloat(HInvoke* invoke) {
  MoveIntToFP(invoke->GetLocations(), false, GetAssembler());
}

static void CreateIntToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

static void CreateFPToFPLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
}

static void MathAbsFP(LocationSummary* locations, bool is64bit, ArmAssembler* assembler) {
  Location in = locations->InAt(0);
  Location out = locations->Out();

  if (is64bit) {
    __ vabsd(FromLowSToD(out.AsFpuRegisterPairLow<SRegister>()),
             FromLowSToD(in.AsFpuRegisterPairLow<SRegister>()));
  } else {
    __ vabss(out.AsFpuRegister<SRegister>(), in.AsFpuRegister<SRegister>());
  }
}

void IntrinsicLocationsBuilderARM::VisitMathAbsDouble(HInvoke* invoke) {
  CreateFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathAbsDouble(HInvoke* invoke) {
  MathAbsFP(invoke->GetLocations(), true, GetAssembler());
}

void IntrinsicLocationsBuilderARM::VisitMathAbsFloat(HInvoke* invoke) {
  CreateFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathAbsFloat(HInvoke* invoke) {
  MathAbsFP(invoke->GetLocations(), false, GetAssembler());
}

static void CreateIntToIntPlusTemp(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);

  locations->AddTemp(Location::RequiresRegister());
}

static void GenAbsInteger(LocationSummary* locations,
                          bool is64bit,
                          ArmAssembler* assembler) {
  Location in = locations->InAt(0);
  Location output = locations->Out();

  Register mask = locations->GetTemp(0).AsRegister<Register>();

  if (is64bit) {
    Register in_reg_lo = in.AsRegisterPairLow<Register>();
    Register in_reg_hi = in.AsRegisterPairHigh<Register>();
    Register out_reg_lo = output.AsRegisterPairLow<Register>();
    Register out_reg_hi = output.AsRegisterPairHigh<Register>();

    DCHECK_NE(out_reg_lo, in_reg_hi) << "Diagonal overlap unexpected.";

    __ Asr(mask, in_reg_hi, 31);
    __ adds(out_reg_lo, in_reg_lo, ShifterOperand(mask));
    __ adc(out_reg_hi, in_reg_hi, ShifterOperand(mask));
    __ eor(out_reg_lo, mask, ShifterOperand(out_reg_lo));
    __ eor(out_reg_hi, mask, ShifterOperand(out_reg_hi));
  } else {
    Register in_reg = in.AsRegister<Register>();
    Register out_reg = output.AsRegister<Register>();

    __ Asr(mask, in_reg, 31);
    __ add(out_reg, in_reg, ShifterOperand(mask));
    __ eor(out_reg, mask, ShifterOperand(out_reg));
  }
}

void IntrinsicLocationsBuilderARM::VisitMathAbsInt(HInvoke* invoke) {
  CreateIntToIntPlusTemp(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathAbsInt(HInvoke* invoke) {
  GenAbsInteger(invoke->GetLocations(), false, GetAssembler());
}


void IntrinsicLocationsBuilderARM::VisitMathAbsLong(HInvoke* invoke) {
  CreateIntToIntPlusTemp(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathAbsLong(HInvoke* invoke) {
  GenAbsInteger(invoke->GetLocations(), true, GetAssembler());
}

static void GenMinMax(LocationSummary* locations,
                      bool is_min,
                      ArmAssembler* assembler) {
  Register op1 = locations->InAt(0).AsRegister<Register>();
  Register op2 = locations->InAt(1).AsRegister<Register>();
  Register out = locations->Out().AsRegister<Register>();

  __ cmp(op1, ShifterOperand(op2));

  __ it((is_min) ? Condition::LT : Condition::GT, kItElse);
  __ mov(out, ShifterOperand(op1), is_min ? Condition::LT : Condition::GT);
  __ mov(out, ShifterOperand(op2), is_min ? Condition::GE : Condition::LE);
}

static void CreateIntIntToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void IntrinsicLocationsBuilderARM::VisitMathMinIntInt(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathMinIntInt(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), true, GetAssembler());
}

void IntrinsicLocationsBuilderARM::VisitMathMaxIntInt(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathMaxIntInt(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), false, GetAssembler());
}

void IntrinsicLocationsBuilderARM::VisitMathSqrt(HInvoke* invoke) {
  CreateFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathSqrt(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  ArmAssembler* assembler = GetAssembler();
  __ vsqrtd(FromLowSToD(locations->Out().AsFpuRegisterPairLow<SRegister>()),
            FromLowSToD(locations->InAt(0).AsFpuRegisterPairLow<SRegister>()));
}

void IntrinsicLocationsBuilderARM::VisitMemoryPeekByte(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMemoryPeekByte(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  // Ignore upper 4B of long address.
  __ ldrsb(invoke->GetLocations()->Out().AsRegister<Register>(),
           Address(invoke->GetLocations()->InAt(0).AsRegisterPairLow<Register>()));
}

void IntrinsicLocationsBuilderARM::VisitMemoryPeekIntNative(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMemoryPeekIntNative(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  // Ignore upper 4B of long address.
  __ ldr(invoke->GetLocations()->Out().AsRegister<Register>(),
         Address(invoke->GetLocations()->InAt(0).AsRegisterPairLow<Register>()));
}

void IntrinsicLocationsBuilderARM::VisitMemoryPeekLongNative(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMemoryPeekLongNative(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  // Ignore upper 4B of long address.
  Register addr = invoke->GetLocations()->InAt(0).AsRegisterPairLow<Register>();
  // Worst case: Control register bit SCTLR.A = 0. Then unaligned accesses throw a processor
  // exception. So we can't use ldrd as addr may be unaligned.
  Register lo = invoke->GetLocations()->Out().AsRegisterPairLow<Register>();
  Register hi = invoke->GetLocations()->Out().AsRegisterPairHigh<Register>();
  if (addr == lo) {
    __ ldr(hi, Address(addr, 4));
    __ ldr(lo, Address(addr, 0));
  } else {
    __ ldr(lo, Address(addr, 0));
    __ ldr(hi, Address(addr, 4));
  }
}

void IntrinsicLocationsBuilderARM::VisitMemoryPeekShortNative(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMemoryPeekShortNative(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  // Ignore upper 4B of long address.
  __ ldrsh(invoke->GetLocations()->Out().AsRegister<Register>(),
           Address(invoke->GetLocations()->InAt(0).AsRegisterPairLow<Register>()));
}

static void CreateIntIntToVoidLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
}

void IntrinsicLocationsBuilderARM::VisitMemoryPokeByte(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMemoryPokeByte(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  __ strb(invoke->GetLocations()->InAt(1).AsRegister<Register>(),
          Address(invoke->GetLocations()->InAt(0).AsRegisterPairLow<Register>()));
}

void IntrinsicLocationsBuilderARM::VisitMemoryPokeIntNative(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMemoryPokeIntNative(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  __ str(invoke->GetLocations()->InAt(1).AsRegister<Register>(),
         Address(invoke->GetLocations()->InAt(0).AsRegisterPairLow<Register>()));
}

void IntrinsicLocationsBuilderARM::VisitMemoryPokeLongNative(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMemoryPokeLongNative(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  // Ignore upper 4B of long address.
  Register addr = invoke->GetLocations()->InAt(0).AsRegisterPairLow<Register>();
  // Worst case: Control register bit SCTLR.A = 0. Then unaligned accesses throw a processor
  // exception. So we can't use ldrd as addr may be unaligned.
  __ str(invoke->GetLocations()->InAt(1).AsRegisterPairLow<Register>(), Address(addr, 0));
  __ str(invoke->GetLocations()->InAt(1).AsRegisterPairHigh<Register>(), Address(addr, 4));
}

void IntrinsicLocationsBuilderARM::VisitMemoryPokeShortNative(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMemoryPokeShortNative(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  __ strh(invoke->GetLocations()->InAt(1).AsRegister<Register>(),
          Address(invoke->GetLocations()->InAt(0).AsRegisterPairLow<Register>()));
}

void IntrinsicLocationsBuilderARM::VisitThreadCurrentThread(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetOut(Location::RequiresRegister());
}

void IntrinsicCodeGeneratorARM::VisitThreadCurrentThread(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  __ LoadFromOffset(kLoadWord,
                    invoke->GetLocations()->Out().AsRegister<Register>(),
                    TR,
                    Thread::PeerOffset<kArmPointerSize>().Int32Value());
}

static void GenUnsafeGet(HInvoke* invoke,
                         Primitive::Type type,
                         bool is_volatile,
                         CodeGeneratorARM* codegen) {
  LocationSummary* locations = invoke->GetLocations();
  DCHECK((type == Primitive::kPrimInt) ||
         (type == Primitive::kPrimLong) ||
         (type == Primitive::kPrimNot));
  ArmAssembler* assembler = codegen->GetAssembler();
  Register base = locations->InAt(1).AsRegister<Register>();           // Object pointer.
  Register offset = locations->InAt(2).AsRegisterPairLow<Register>();  // Long offset, lo part only.

  if (type == Primitive::kPrimLong) {
    Register trg_lo = locations->Out().AsRegisterPairLow<Register>();
    __ add(IP, base, ShifterOperand(offset));
    if (is_volatile && !codegen->GetInstructionSetFeatures().HasAtomicLdrdAndStrd()) {
      Register trg_hi = locations->Out().AsRegisterPairHigh<Register>();
      __ ldrexd(trg_lo, trg_hi, IP);
    } else {
      __ ldrd(trg_lo, Address(IP));
    }
  } else {
    Register trg = locations->Out().AsRegister<Register>();
    __ ldr(trg, Address(base, offset));
  }

  if (is_volatile) {
    __ dmb(ISH);
  }
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

void IntrinsicLocationsBuilderARM::VisitUnsafeGet(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafeGetVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafeGetLong(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafeGetLongVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafeGetObject(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafeGetObjectVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitUnsafeGet(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimInt, false, codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafeGetVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimInt, true, codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafeGetLong(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimLong, false, codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafeGetLongVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimLong, true, codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafeGetObject(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimNot, false, codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafeGetObjectVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimNot, true, codegen_);
}

static void CreateIntIntIntIntToVoid(ArenaAllocator* arena,
                                     const ArmInstructionSetFeatures& features,
                                     Primitive::Type type,
                                     bool is_volatile,
                                     HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::NoLocation());        // Unused receiver.
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetInAt(2, Location::RequiresRegister());
  locations->SetInAt(3, Location::RequiresRegister());

  if (type == Primitive::kPrimLong) {
    // Potentially need temps for ldrexd-strexd loop.
    if (is_volatile && !features.HasAtomicLdrdAndStrd()) {
      locations->AddTemp(Location::RequiresRegister());  // Temp_lo.
      locations->AddTemp(Location::RequiresRegister());  // Temp_hi.
    }
  } else if (type == Primitive::kPrimNot) {
    // Temps for card-marking.
    locations->AddTemp(Location::RequiresRegister());  // Temp.
    locations->AddTemp(Location::RequiresRegister());  // Card.
  }
}

void IntrinsicLocationsBuilderARM::VisitUnsafePut(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, features_, Primitive::kPrimInt, false, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafePutOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, features_, Primitive::kPrimInt, false, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafePutVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, features_, Primitive::kPrimInt, true, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafePutObject(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, features_, Primitive::kPrimNot, false, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafePutObjectOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, features_, Primitive::kPrimNot, false, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafePutObjectVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, features_, Primitive::kPrimNot, true, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafePutLong(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, features_, Primitive::kPrimLong, false, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafePutLongOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, features_, Primitive::kPrimLong, false, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafePutLongVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, features_, Primitive::kPrimLong, true, invoke);
}

static void GenUnsafePut(LocationSummary* locations,
                         Primitive::Type type,
                         bool is_volatile,
                         bool is_ordered,
                         CodeGeneratorARM* codegen) {
  ArmAssembler* assembler = codegen->GetAssembler();

  Register base = locations->InAt(1).AsRegister<Register>();           // Object pointer.
  Register offset = locations->InAt(2).AsRegisterPairLow<Register>();  // Long offset, lo part only.
  Register value;

  if (is_volatile || is_ordered) {
    __ dmb(ISH);
  }

  if (type == Primitive::kPrimLong) {
    Register value_lo = locations->InAt(3).AsRegisterPairLow<Register>();
    value = value_lo;
    if (is_volatile && !codegen->GetInstructionSetFeatures().HasAtomicLdrdAndStrd()) {
      Register temp_lo = locations->GetTemp(0).AsRegister<Register>();
      Register temp_hi = locations->GetTemp(1).AsRegister<Register>();
      Register value_hi = locations->InAt(3).AsRegisterPairHigh<Register>();

      __ add(IP, base, ShifterOperand(offset));
      Label loop_head;
      __ Bind(&loop_head);
      __ ldrexd(temp_lo, temp_hi, IP);
      __ strexd(temp_lo, value_lo, value_hi, IP);
      __ cmp(temp_lo, ShifterOperand(0));
      __ b(&loop_head, NE);
    } else {
      __ add(IP, base, ShifterOperand(offset));
      __ strd(value_lo, Address(IP));
    }
  } else {
    value =  locations->InAt(3).AsRegister<Register>();
    __ str(value, Address(base, offset));
  }

  if (is_volatile) {
    __ dmb(ISH);
  }

  if (type == Primitive::kPrimNot) {
    Register temp = locations->GetTemp(0).AsRegister<Register>();
    Register card = locations->GetTemp(1).AsRegister<Register>();
    codegen->MarkGCCard(temp, card, base, value);
  }
}

void IntrinsicCodeGeneratorARM::VisitUnsafePut(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimInt, false, false, codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafePutOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimInt, false, true, codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafePutVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimInt, true, false, codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafePutObject(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimNot, false, false, codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafePutObjectOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimNot, false, true, codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafePutObjectVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimNot, true, false, codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafePutLong(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimLong, false, false, codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafePutLongOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimLong, false, true, codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafePutLongVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimLong, true, false, codegen_);
}

static void CreateIntIntIntIntIntToIntPlusTemps(ArenaAllocator* arena,
                                                HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::NoLocation());        // Unused receiver.
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetInAt(2, Location::RequiresRegister());
  locations->SetInAt(3, Location::RequiresRegister());
  locations->SetInAt(4, Location::RequiresRegister());

  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);

  locations->AddTemp(Location::RequiresRegister());  // Pointer.
  locations->AddTemp(Location::RequiresRegister());  // Temp 1.
  locations->AddTemp(Location::RequiresRegister());  // Temp 2.
}

static void GenCas(LocationSummary* locations, Primitive::Type type, CodeGeneratorARM* codegen) {
  DCHECK_NE(type, Primitive::kPrimLong);

  ArmAssembler* assembler = codegen->GetAssembler();

  Register out = locations->Out().AsRegister<Register>();              // Boolean result.

  Register base = locations->InAt(1).AsRegister<Register>();           // Object pointer.
  Register offset = locations->InAt(2).AsRegisterPairLow<Register>();  // Offset (discard high 4B).
  Register expected_lo = locations->InAt(3).AsRegister<Register>();    // Expected.
  Register value_lo = locations->InAt(4).AsRegister<Register>();       // Value.

  Register tmp_ptr = locations->GetTemp(0).AsRegister<Register>();     // Pointer to actual memory.
  Register tmp_lo = locations->GetTemp(1).AsRegister<Register>();      // Value in memory.

  if (type == Primitive::kPrimNot) {
    // Mark card for object assuming new value is stored. Worst case we will mark an unchanged
    // object and scan the receiver at the next GC for nothing.
    codegen->MarkGCCard(tmp_ptr, tmp_lo, base, value_lo);
  }

  // Prevent reordering with prior memory operations.
  __ dmb(ISH);

  __ add(tmp_ptr, base, ShifterOperand(offset));

  // do {
  //   tmp = [r_ptr] - expected;
  // } while (tmp == 0 && failure([r_ptr] <- r_new_value));
  // result = tmp != 0;

  Label loop_head;
  __ Bind(&loop_head);

  __ ldrex(tmp_lo, tmp_ptr);

  __ subs(tmp_lo, tmp_lo, ShifterOperand(expected_lo));

  __ it(EQ, ItState::kItT);
  __ strex(tmp_lo, value_lo, tmp_ptr, EQ);
  __ cmp(tmp_lo, ShifterOperand(1), EQ);

  __ b(&loop_head, EQ);

  __ dmb(ISH);

  __ rsbs(out, tmp_lo, ShifterOperand(1));
  __ it(CC);
  __ mov(out, ShifterOperand(0), CC);
}

void IntrinsicLocationsBuilderARM::VisitUnsafeCASInt(HInvoke* invoke) {
  CreateIntIntIntIntIntToIntPlusTemps(arena_, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafeCASObject(HInvoke* invoke) {
  CreateIntIntIntIntIntToIntPlusTemps(arena_, invoke);
}
void IntrinsicCodeGeneratorARM::VisitUnsafeCASInt(HInvoke* invoke) {
  GenCas(invoke->GetLocations(), Primitive::kPrimInt, codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafeCASObject(HInvoke* invoke) {
  GenCas(invoke->GetLocations(), Primitive::kPrimNot, codegen_);
}

void IntrinsicLocationsBuilderARM::VisitStringCharAt(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCallOnSlowPath,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);

  locations->AddTemp(Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
}

void IntrinsicCodeGeneratorARM::VisitStringCharAt(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  // Location of reference to data array
  const MemberOffset value_offset = mirror::String::ValueOffset();
  // Location of count
  const MemberOffset count_offset = mirror::String::CountOffset();

  Register obj = locations->InAt(0).AsRegister<Register>();  // String object pointer.
  Register idx = locations->InAt(1).AsRegister<Register>();  // Index of character.
  Register out = locations->Out().AsRegister<Register>();    // Result character.

  Register temp = locations->GetTemp(0).AsRegister<Register>();
  Register array_temp = locations->GetTemp(1).AsRegister<Register>();

  // TODO: Maybe we can support range check elimination. Overall, though, I think it's not worth
  //       the cost.
  // TODO: For simplicity, the index parameter is requested in a register, so different from Quick
  //       we will not optimize the code for constants (which would save a register).

  SlowPathCodeARM* slow_path = new (GetAllocator()) IntrinsicSlowPathARM(invoke);
  codegen_->AddSlowPath(slow_path);

  __ ldr(temp, Address(obj, count_offset.Int32Value()));          // temp = str.length.
  codegen_->MaybeRecordImplicitNullCheck(invoke);
  __ cmp(idx, ShifterOperand(temp));
  __ b(slow_path->GetEntryLabel(), CS);

  __ add(array_temp, obj, ShifterOperand(value_offset.Int32Value()));  // array_temp := str.value.

  // Load the value.
  __ ldrh(out, Address(array_temp, idx, LSL, 1));                 // out := array_temp[idx].

  __ Bind(slow_path->GetExitLabel());
}

void IntrinsicLocationsBuilderARM::VisitStringCompareTo(HInvoke* invoke) {
  // The inputs plus one temp.
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetOut(Location::RegisterLocation(R0));
}

void IntrinsicCodeGeneratorARM::VisitStringCompareTo(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  Register argument = locations->InAt(1).AsRegister<Register>();
  __ cmp(argument, ShifterOperand(0));
  SlowPathCodeARM* slow_path = new (GetAllocator()) IntrinsicSlowPathARM(invoke);
  codegen_->AddSlowPath(slow_path);
  __ b(slow_path->GetEntryLabel(), EQ);

  __ LoadFromOffset(
      kLoadWord, LR, TR, QUICK_ENTRYPOINT_OFFSET(kArmWordSize, pStringCompareTo).Int32Value());
  __ blx(LR);
  __ Bind(slow_path->GetExitLabel());
}

static void GenerateVisitStringIndexOf(HInvoke* invoke,
                                       ArmAssembler* assembler,
                                       CodeGeneratorARM* codegen,
                                       ArenaAllocator* allocator,
                                       bool start_at_zero) {
  LocationSummary* locations = invoke->GetLocations();
  Register tmp_reg = locations->GetTemp(0).AsRegister<Register>();

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  // Check for code points > 0xFFFF. Either a slow-path check when we don't know statically,
  // or directly dispatch if we have a constant.
  SlowPathCodeARM* slow_path = nullptr;
  if (invoke->InputAt(1)->IsIntConstant()) {
    if (static_cast<uint32_t>(invoke->InputAt(1)->AsIntConstant()->GetValue()) >
        std::numeric_limits<uint16_t>::max()) {
      // Always needs the slow-path. We could directly dispatch to it, but this case should be
      // rare, so for simplicity just put the full slow-path down and branch unconditionally.
      slow_path = new (allocator) IntrinsicSlowPathARM(invoke);
      codegen->AddSlowPath(slow_path);
      __ b(slow_path->GetEntryLabel());
      __ Bind(slow_path->GetExitLabel());
      return;
    }
  } else {
    Register char_reg = locations->InAt(1).AsRegister<Register>();
    __ LoadImmediate(tmp_reg, std::numeric_limits<uint16_t>::max());
    __ cmp(char_reg, ShifterOperand(tmp_reg));
    slow_path = new (allocator) IntrinsicSlowPathARM(invoke);
    codegen->AddSlowPath(slow_path);
    __ b(slow_path->GetEntryLabel(), HI);
  }

  if (start_at_zero) {
    DCHECK_EQ(tmp_reg, R2);
    // Start-index = 0.
    __ LoadImmediate(tmp_reg, 0);
  }

  __ LoadFromOffset(kLoadWord, LR, TR,
                    QUICK_ENTRYPOINT_OFFSET(kArmWordSize, pIndexOf).Int32Value());
  __ blx(LR);

  if (slow_path != nullptr) {
    __ Bind(slow_path->GetExitLabel());
  }
}

void IntrinsicLocationsBuilderARM::VisitStringIndexOf(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  // We have a hand-crafted assembly stub that follows the runtime calling convention. So it's
  // best to align the inputs accordingly.
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetOut(Location::RegisterLocation(R0));

  // Need a temp for slow-path codepoint compare, and need to send start-index=0.
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
}

void IntrinsicCodeGeneratorARM::VisitStringIndexOf(HInvoke* invoke) {
  GenerateVisitStringIndexOf(invoke, GetAssembler(), codegen_, GetAllocator(), true);
}

void IntrinsicLocationsBuilderARM::VisitStringIndexOfAfter(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  // We have a hand-crafted assembly stub that follows the runtime calling convention. So it's
  // best to align the inputs accordingly.
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(2, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  locations->SetOut(Location::RegisterLocation(R0));

  // Need a temp for slow-path codepoint compare.
  locations->AddTemp(Location::RequiresRegister());
}

void IntrinsicCodeGeneratorARM::VisitStringIndexOfAfter(HInvoke* invoke) {
  GenerateVisitStringIndexOf(invoke, GetAssembler(), codegen_, GetAllocator(), false);
}

void IntrinsicLocationsBuilderARM::VisitStringNewStringFromBytes(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(2, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  locations->SetInAt(3, Location::RegisterLocation(calling_convention.GetRegisterAt(3)));
  locations->SetOut(Location::RegisterLocation(R0));
}

void IntrinsicCodeGeneratorARM::VisitStringNewStringFromBytes(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  Register byte_array = locations->InAt(0).AsRegister<Register>();
  __ cmp(byte_array, ShifterOperand(0));
  SlowPathCodeARM* slow_path = new (GetAllocator()) IntrinsicSlowPathARM(invoke);
  codegen_->AddSlowPath(slow_path);
  __ b(slow_path->GetEntryLabel(), EQ);

  __ LoadFromOffset(
      kLoadWord, LR, TR, QUICK_ENTRYPOINT_OFFSET(kArmWordSize, pAllocStringFromBytes).Int32Value());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
  __ blx(LR);
  __ Bind(slow_path->GetExitLabel());
}

void IntrinsicLocationsBuilderARM::VisitStringNewStringFromChars(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(2, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  locations->SetOut(Location::RegisterLocation(R0));
}

void IntrinsicCodeGeneratorARM::VisitStringNewStringFromChars(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();

  __ LoadFromOffset(
      kLoadWord, LR, TR, QUICK_ENTRYPOINT_OFFSET(kArmWordSize, pAllocStringFromChars).Int32Value());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
  __ blx(LR);
}

void IntrinsicLocationsBuilderARM::VisitStringNewStringFromString(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetOut(Location::RegisterLocation(R0));
}

void IntrinsicCodeGeneratorARM::VisitStringNewStringFromString(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  Register string_to_copy = locations->InAt(0).AsRegister<Register>();
  __ cmp(string_to_copy, ShifterOperand(0));
  SlowPathCodeARM* slow_path = new (GetAllocator()) IntrinsicSlowPathARM(invoke);
  codegen_->AddSlowPath(slow_path);
  __ b(slow_path->GetEntryLabel(), EQ);

  __ LoadFromOffset(kLoadWord,
      LR, TR, QUICK_ENTRYPOINT_OFFSET(kArmWordSize, pAllocStringFromString).Int32Value());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
  __ blx(LR);
  __ Bind(slow_path->GetExitLabel());
}

// Unimplemented intrinsics.

#define UNIMPLEMENTED_INTRINSIC(Name)                                                  \
void IntrinsicLocationsBuilderARM::Visit ## Name(HInvoke* invoke ATTRIBUTE_UNUSED) { \
}                                                                                      \
void IntrinsicCodeGeneratorARM::Visit ## Name(HInvoke* invoke ATTRIBUTE_UNUSED) {    \
}

UNIMPLEMENTED_INTRINSIC(IntegerReverse)
UNIMPLEMENTED_INTRINSIC(IntegerReverseBytes)
UNIMPLEMENTED_INTRINSIC(LongReverse)
UNIMPLEMENTED_INTRINSIC(LongReverseBytes)
UNIMPLEMENTED_INTRINSIC(ShortReverseBytes)
UNIMPLEMENTED_INTRINSIC(MathMinDoubleDouble)
UNIMPLEMENTED_INTRINSIC(MathMinFloatFloat)
UNIMPLEMENTED_INTRINSIC(MathMaxDoubleDouble)
UNIMPLEMENTED_INTRINSIC(MathMaxFloatFloat)
UNIMPLEMENTED_INTRINSIC(MathMinLongLong)
UNIMPLEMENTED_INTRINSIC(MathMaxLongLong)
UNIMPLEMENTED_INTRINSIC(MathCeil)          // Could be done by changing rounding mode, maybe?
UNIMPLEMENTED_INTRINSIC(MathFloor)         // Could be done by changing rounding mode, maybe?
UNIMPLEMENTED_INTRINSIC(MathRint)
UNIMPLEMENTED_INTRINSIC(MathRoundDouble)   // Could be done by changing rounding mode, maybe?
UNIMPLEMENTED_INTRINSIC(MathRoundFloat)    // Could be done by changing rounding mode, maybe?
UNIMPLEMENTED_INTRINSIC(UnsafeCASLong)     // High register pressure.
UNIMPLEMENTED_INTRINSIC(SystemArrayCopyChar)
UNIMPLEMENTED_INTRINSIC(ReferenceGetReferent)
UNIMPLEMENTED_INTRINSIC(StringGetCharsNoCheck)

}  // namespace arm
}  // namespace art
