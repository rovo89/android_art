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
#include "intrinsics_utils.h"
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

using IntrinsicSlowPathARM = IntrinsicSlowPath<InvokeDexCallingConventionVisitorARM>;

bool IntrinsicLocationsBuilderARM::TryDispatch(HInvoke* invoke) {
  Dispatch(invoke);
  LocationSummary* res = invoke->GetLocations();
  if (res == nullptr) {
    return false;
  }
  if (kEmitCompilerReadBarrier && res->CanCall()) {
    // Generating an intrinsic for this HInvoke may produce an
    // IntrinsicSlowPathARM slow path.  Currently this approach
    // does not work when using read barriers, as the emitted
    // calling sequence will make use of another slow path
    // (ReadBarrierForRootSlowPathARM for HInvokeStaticOrDirect,
    // ReadBarrierSlowPathARM for HInvokeVirtual).  So we bail
    // out in this case.
    //
    // TODO: Find a way to have intrinsics work with read barriers.
    invoke->SetLocations(nullptr);
    return false;
  }
  return res->Intrinsified();
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
  MoveFPToInt(invoke->GetLocations(), /* is64bit */ true, GetAssembler());
}
void IntrinsicCodeGeneratorARM::VisitDoubleLongBitsToDouble(HInvoke* invoke) {
  MoveIntToFP(invoke->GetLocations(), /* is64bit */ true, GetAssembler());
}

void IntrinsicLocationsBuilderARM::VisitFloatFloatToRawIntBits(HInvoke* invoke) {
  CreateFPToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderARM::VisitFloatIntBitsToFloat(HInvoke* invoke) {
  CreateIntToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitFloatFloatToRawIntBits(HInvoke* invoke) {
  MoveFPToInt(invoke->GetLocations(), /* is64bit */ false, GetAssembler());
}
void IntrinsicCodeGeneratorARM::VisitFloatIntBitsToFloat(HInvoke* invoke) {
  MoveIntToFP(invoke->GetLocations(), /* is64bit */ false, GetAssembler());
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

static void GenNumberOfLeadingZeros(LocationSummary* locations,
                                    Primitive::Type type,
                                    ArmAssembler* assembler) {
  Location in = locations->InAt(0);
  Register out = locations->Out().AsRegister<Register>();

  DCHECK((type == Primitive::kPrimInt) || (type == Primitive::kPrimLong));

  if (type == Primitive::kPrimLong) {
    Register in_reg_lo = in.AsRegisterPairLow<Register>();
    Register in_reg_hi = in.AsRegisterPairHigh<Register>();
    Label end;
    __ clz(out, in_reg_hi);
    __ CompareAndBranchIfNonZero(in_reg_hi, &end);
    __ clz(out, in_reg_lo);
    __ AddConstant(out, 32);
    __ Bind(&end);
  } else {
    __ clz(out, in.AsRegister<Register>());
  }
}

void IntrinsicLocationsBuilderARM::VisitIntegerNumberOfLeadingZeros(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitIntegerNumberOfLeadingZeros(HInvoke* invoke) {
  GenNumberOfLeadingZeros(invoke->GetLocations(), Primitive::kPrimInt, GetAssembler());
}

void IntrinsicLocationsBuilderARM::VisitLongNumberOfLeadingZeros(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
}

void IntrinsicCodeGeneratorARM::VisitLongNumberOfLeadingZeros(HInvoke* invoke) {
  GenNumberOfLeadingZeros(invoke->GetLocations(), Primitive::kPrimLong, GetAssembler());
}

static void GenNumberOfTrailingZeros(LocationSummary* locations,
                                     Primitive::Type type,
                                     ArmAssembler* assembler) {
  DCHECK((type == Primitive::kPrimInt) || (type == Primitive::kPrimLong));

  Register out = locations->Out().AsRegister<Register>();

  if (type == Primitive::kPrimLong) {
    Register in_reg_lo = locations->InAt(0).AsRegisterPairLow<Register>();
    Register in_reg_hi = locations->InAt(0).AsRegisterPairHigh<Register>();
    Label end;
    __ rbit(out, in_reg_lo);
    __ clz(out, out);
    __ CompareAndBranchIfNonZero(in_reg_lo, &end);
    __ rbit(out, in_reg_hi);
    __ clz(out, out);
    __ AddConstant(out, 32);
    __ Bind(&end);
  } else {
    Register in = locations->InAt(0).AsRegister<Register>();
    __ rbit(out, in);
    __ clz(out, out);
  }
}

void IntrinsicLocationsBuilderARM::VisitIntegerNumberOfTrailingZeros(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void IntrinsicCodeGeneratorARM::VisitIntegerNumberOfTrailingZeros(HInvoke* invoke) {
  GenNumberOfTrailingZeros(invoke->GetLocations(), Primitive::kPrimInt, GetAssembler());
}

void IntrinsicLocationsBuilderARM::VisitLongNumberOfTrailingZeros(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
}

void IntrinsicCodeGeneratorARM::VisitLongNumberOfTrailingZeros(HInvoke* invoke) {
  GenNumberOfTrailingZeros(invoke->GetLocations(), Primitive::kPrimLong, GetAssembler());
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
  MathAbsFP(invoke->GetLocations(), /* is64bit */ true, GetAssembler());
}

void IntrinsicLocationsBuilderARM::VisitMathAbsFloat(HInvoke* invoke) {
  CreateFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathAbsFloat(HInvoke* invoke) {
  MathAbsFP(invoke->GetLocations(), /* is64bit */ false, GetAssembler());
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
  GenAbsInteger(invoke->GetLocations(), /* is64bit */ false, GetAssembler());
}


void IntrinsicLocationsBuilderARM::VisitMathAbsLong(HInvoke* invoke) {
  CreateIntToIntPlusTemp(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathAbsLong(HInvoke* invoke) {
  GenAbsInteger(invoke->GetLocations(), /* is64bit */ true, GetAssembler());
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
  GenMinMax(invoke->GetLocations(), /* is_min */ true, GetAssembler());
}

void IntrinsicLocationsBuilderARM::VisitMathMaxIntInt(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathMaxIntInt(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), /* is_min */ false, GetAssembler());
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
  ArmAssembler* assembler = codegen->GetAssembler();
  Location base_loc = locations->InAt(1);
  Register base = base_loc.AsRegister<Register>();             // Object pointer.
  Location offset_loc = locations->InAt(2);
  Register offset = offset_loc.AsRegisterPairLow<Register>();  // Long offset, lo part only.
  Location trg_loc = locations->Out();

  switch (type) {
    case Primitive::kPrimInt: {
      Register trg = trg_loc.AsRegister<Register>();
      __ ldr(trg, Address(base, offset));
      if (is_volatile) {
        __ dmb(ISH);
      }
      break;
    }

    case Primitive::kPrimNot: {
      Register trg = trg_loc.AsRegister<Register>();
      if (kEmitCompilerReadBarrier) {
        if (kUseBakerReadBarrier) {
          Location temp = locations->GetTemp(0);
          codegen->GenerateArrayLoadWithBakerReadBarrier(
              invoke, trg_loc, base, 0U, offset_loc, temp, /* needs_null_check */ false);
          if (is_volatile) {
            __ dmb(ISH);
          }
        } else {
          __ ldr(trg, Address(base, offset));
          if (is_volatile) {
            __ dmb(ISH);
          }
          codegen->GenerateReadBarrierSlow(invoke, trg_loc, trg_loc, base_loc, 0U, offset_loc);
        }
      } else {
        __ ldr(trg, Address(base, offset));
        if (is_volatile) {
          __ dmb(ISH);
        }
        __ MaybeUnpoisonHeapReference(trg);
      }
      break;
    }

    case Primitive::kPrimLong: {
      Register trg_lo = trg_loc.AsRegisterPairLow<Register>();
      __ add(IP, base, ShifterOperand(offset));
      if (is_volatile && !codegen->GetInstructionSetFeatures().HasAtomicLdrdAndStrd()) {
        Register trg_hi = trg_loc.AsRegisterPairHigh<Register>();
        __ ldrexd(trg_lo, trg_hi, IP);
      } else {
        __ ldrd(trg_lo, Address(IP));
      }
      if (is_volatile) {
        __ dmb(ISH);
      }
      break;
    }

    default:
      LOG(FATAL) << "Unexpected type " << type;
      UNREACHABLE();
  }
}

static void CreateIntIntIntToIntLocations(ArenaAllocator* arena,
                                          HInvoke* invoke,
                                          Primitive::Type type) {
  bool can_call = kEmitCompilerReadBarrier &&
      (invoke->GetIntrinsic() == Intrinsics::kUnsafeGetObject ||
       invoke->GetIntrinsic() == Intrinsics::kUnsafeGetObjectVolatile);
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           can_call ?
                                                               LocationSummary::kCallOnSlowPath :
                                                               LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::NoLocation());        // Unused receiver.
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetInAt(2, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
  if (type == Primitive::kPrimNot && kEmitCompilerReadBarrier && kUseBakerReadBarrier) {
    // We need a temporary register for the read barrier marking slow
    // path in InstructionCodeGeneratorARM::GenerateArrayLoadWithBakerReadBarrier.
    locations->AddTemp(Location::RequiresRegister());
  }
}

void IntrinsicLocationsBuilderARM::VisitUnsafeGet(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, Primitive::kPrimInt);
}
void IntrinsicLocationsBuilderARM::VisitUnsafeGetVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, Primitive::kPrimInt);
}
void IntrinsicLocationsBuilderARM::VisitUnsafeGetLong(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, Primitive::kPrimLong);
}
void IntrinsicLocationsBuilderARM::VisitUnsafeGetLongVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, Primitive::kPrimLong);
}
void IntrinsicLocationsBuilderARM::VisitUnsafeGetObject(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, Primitive::kPrimNot);
}
void IntrinsicLocationsBuilderARM::VisitUnsafeGetObjectVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, Primitive::kPrimNot);
}

void IntrinsicCodeGeneratorARM::VisitUnsafeGet(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimInt, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafeGetVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimInt, /* is_volatile */ true, codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafeGetLong(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimLong, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafeGetLongVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimLong, /* is_volatile */ true, codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafeGetObject(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimNot, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafeGetObjectVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimNot, /* is_volatile */ true, codegen_);
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
  CreateIntIntIntIntToVoid(arena_, features_, Primitive::kPrimInt, /* is_volatile */ false, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafePutOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, features_, Primitive::kPrimInt, /* is_volatile */ false, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafePutVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, features_, Primitive::kPrimInt, /* is_volatile */ true, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafePutObject(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, features_, Primitive::kPrimNot, /* is_volatile */ false, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafePutObjectOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, features_, Primitive::kPrimNot, /* is_volatile */ false, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafePutObjectVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, features_, Primitive::kPrimNot, /* is_volatile */ true, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafePutLong(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(
      arena_, features_, Primitive::kPrimLong, /* is_volatile */ false, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafePutLongOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(
      arena_, features_, Primitive::kPrimLong, /* is_volatile */ false, invoke);
}
void IntrinsicLocationsBuilderARM::VisitUnsafePutLongVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(
      arena_, features_, Primitive::kPrimLong, /* is_volatile */ true, invoke);
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
    value = locations->InAt(3).AsRegister<Register>();
    Register source = value;
    if (kPoisonHeapReferences && type == Primitive::kPrimNot) {
      Register temp = locations->GetTemp(0).AsRegister<Register>();
      __ Mov(temp, value);
      __ PoisonHeapReference(temp);
      source = temp;
    }
    __ str(source, Address(base, offset));
  }

  if (is_volatile) {
    __ dmb(ISH);
  }

  if (type == Primitive::kPrimNot) {
    Register temp = locations->GetTemp(0).AsRegister<Register>();
    Register card = locations->GetTemp(1).AsRegister<Register>();
    bool value_can_be_null = true;  // TODO: Worth finding out this information?
    codegen->MarkGCCard(temp, card, base, value, value_can_be_null);
  }
}

void IntrinsicCodeGeneratorARM::VisitUnsafePut(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimInt,
               /* is_volatile */ false,
               /* is_ordered */ false,
               codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafePutOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimInt,
               /* is_volatile */ false,
               /* is_ordered */ true,
               codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafePutVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimInt,
               /* is_volatile */ true,
               /* is_ordered */ false,
               codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafePutObject(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimNot,
               /* is_volatile */ false,
               /* is_ordered */ false,
               codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafePutObjectOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimNot,
               /* is_volatile */ false,
               /* is_ordered */ true,
               codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafePutObjectVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimNot,
               /* is_volatile */ true,
               /* is_ordered */ false,
               codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafePutLong(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimLong,
               /* is_volatile */ false,
               /* is_ordered */ false,
               codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafePutLongOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimLong,
               /* is_volatile */ false,
               /* is_ordered */ true,
               codegen_);
}
void IntrinsicCodeGeneratorARM::VisitUnsafePutLongVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimLong,
               /* is_volatile */ true,
               /* is_ordered */ false,
               codegen_);
}

static void CreateIntIntIntIntIntToIntPlusTemps(ArenaAllocator* arena,
                                                HInvoke* invoke,
                                                Primitive::Type type) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::NoLocation());        // Unused receiver.
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetInAt(2, Location::RequiresRegister());
  locations->SetInAt(3, Location::RequiresRegister());
  locations->SetInAt(4, Location::RequiresRegister());

  // If heap poisoning is enabled, we don't want the unpoisoning
  // operations to potentially clobber the output.
  Location::OutputOverlap overlaps = (kPoisonHeapReferences && type == Primitive::kPrimNot)
      ? Location::kOutputOverlap
      : Location::kNoOutputOverlap;
  locations->SetOut(Location::RequiresRegister(), overlaps);

  locations->AddTemp(Location::RequiresRegister());  // Pointer.
  locations->AddTemp(Location::RequiresRegister());  // Temp 1.
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
    bool value_can_be_null = true;  // TODO: Worth finding out this information?
    codegen->MarkGCCard(tmp_ptr, tmp_lo, base, value_lo, value_can_be_null);
  }

  // Prevent reordering with prior memory operations.
  // Emit a DMB ISH instruction instead of an DMB ISHST one, as the
  // latter allows a preceding load to be delayed past the STXR
  // instruction below.
  __ dmb(ISH);

  __ add(tmp_ptr, base, ShifterOperand(offset));

  if (kPoisonHeapReferences && type == Primitive::kPrimNot) {
    codegen->GetAssembler()->PoisonHeapReference(expected_lo);
    if (value_lo == expected_lo) {
      // Do not poison `value_lo`, as it is the same register as
      // `expected_lo`, which has just been poisoned.
    } else {
      codegen->GetAssembler()->PoisonHeapReference(value_lo);
    }
  }

  // do {
  //   tmp = [r_ptr] - expected;
  // } while (tmp == 0 && failure([r_ptr] <- r_new_value));
  // result = tmp != 0;

  Label loop_head;
  __ Bind(&loop_head);

  // TODO: When `type == Primitive::kPrimNot`, add a read barrier for
  // the reference stored in the object before attempting the CAS,
  // similar to the one in the art::Unsafe_compareAndSwapObject JNI
  // implementation.
  //
  // Note that this code is not (yet) used when read barriers are
  // enabled (see IntrinsicLocationsBuilderARM::VisitUnsafeCASObject).
  DCHECK(!(type == Primitive::kPrimNot && kEmitCompilerReadBarrier));
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

  if (kPoisonHeapReferences && type == Primitive::kPrimNot) {
    codegen->GetAssembler()->UnpoisonHeapReference(expected_lo);
    if (value_lo == expected_lo) {
      // Do not unpoison `value_lo`, as it is the same register as
      // `expected_lo`, which has just been unpoisoned.
    } else {
      codegen->GetAssembler()->UnpoisonHeapReference(value_lo);
    }
  }
}

void IntrinsicLocationsBuilderARM::VisitUnsafeCASInt(HInvoke* invoke) {
  CreateIntIntIntIntIntToIntPlusTemps(arena_, invoke, Primitive::kPrimInt);
}
void IntrinsicLocationsBuilderARM::VisitUnsafeCASObject(HInvoke* invoke) {
  // The UnsafeCASObject intrinsic is missing a read barrier, and
  // therefore sometimes does not work as expected (b/25883050).
  // Turn it off temporarily as a quick fix, until the read barrier is
  // implemented (see TODO in GenCAS below).
  //
  // TODO(rpl): Fix this issue and re-enable this intrinsic with read barriers.
  if (kEmitCompilerReadBarrier) {
    return;
  }

  CreateIntIntIntIntIntToIntPlusTemps(arena_, invoke, Primitive::kPrimNot);
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

  SlowPathCode* slow_path = new (GetAllocator()) IntrinsicSlowPathARM(invoke);
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
  SlowPathCode* slow_path = new (GetAllocator()) IntrinsicSlowPathARM(invoke);
  codegen_->AddSlowPath(slow_path);
  __ b(slow_path->GetEntryLabel(), EQ);

  __ LoadFromOffset(
      kLoadWord, LR, TR, QUICK_ENTRYPOINT_OFFSET(kArmWordSize, pStringCompareTo).Int32Value());
  __ blx(LR);
  __ Bind(slow_path->GetExitLabel());
}

void IntrinsicLocationsBuilderARM::VisitStringEquals(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  // Temporary registers to store lengths of strings and for calculations.
  // Using instruction cbz requires a low register, so explicitly set a temp to be R0.
  locations->AddTemp(Location::RegisterLocation(R0));
  locations->AddTemp(Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());

  locations->SetOut(Location::RequiresRegister());
}

void IntrinsicCodeGeneratorARM::VisitStringEquals(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  Register str = locations->InAt(0).AsRegister<Register>();
  Register arg = locations->InAt(1).AsRegister<Register>();
  Register out = locations->Out().AsRegister<Register>();

  Register temp = locations->GetTemp(0).AsRegister<Register>();
  Register temp1 = locations->GetTemp(1).AsRegister<Register>();
  Register temp2 = locations->GetTemp(2).AsRegister<Register>();

  Label loop;
  Label end;
  Label return_true;
  Label return_false;

  // Get offsets of count, value, and class fields within a string object.
  const uint32_t count_offset = mirror::String::CountOffset().Uint32Value();
  const uint32_t value_offset = mirror::String::ValueOffset().Uint32Value();
  const uint32_t class_offset = mirror::Object::ClassOffset().Uint32Value();

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  // Check if input is null, return false if it is.
  __ CompareAndBranchIfZero(arg, &return_false);

  // Instanceof check for the argument by comparing class fields.
  // All string objects must have the same type since String cannot be subclassed.
  // Receiver must be a string object, so its class field is equal to all strings' class fields.
  // If the argument is a string object, its class field must be equal to receiver's class field.
  __ ldr(temp, Address(str, class_offset));
  __ ldr(temp1, Address(arg, class_offset));
  __ cmp(temp, ShifterOperand(temp1));
  __ b(&return_false, NE);

  // Load lengths of this and argument strings.
  __ ldr(temp, Address(str, count_offset));
  __ ldr(temp1, Address(arg, count_offset));
  // Check if lengths are equal, return false if they're not.
  __ cmp(temp, ShifterOperand(temp1));
  __ b(&return_false, NE);
  // Return true if both strings are empty.
  __ cbz(temp, &return_true);

  // Reference equality check, return true if same reference.
  __ cmp(str, ShifterOperand(arg));
  __ b(&return_true, EQ);

  // Assertions that must hold in order to compare strings 2 characters at a time.
  DCHECK_ALIGNED(value_offset, 4);
  static_assert(IsAligned<4>(kObjectAlignment), "String of odd length is not zero padded");

  __ LoadImmediate(temp1, value_offset);

  // Loop to compare strings 2 characters at a time starting at the front of the string.
  // Ok to do this because strings with an odd length are zero-padded.
  __ Bind(&loop);
  __ ldr(out, Address(str, temp1));
  __ ldr(temp2, Address(arg, temp1));
  __ cmp(out, ShifterOperand(temp2));
  __ b(&return_false, NE);
  __ add(temp1, temp1, ShifterOperand(sizeof(uint32_t)));
  __ subs(temp, temp, ShifterOperand(sizeof(uint32_t) /  sizeof(uint16_t)));
  __ b(&loop, GT);

  // Return true and exit the function.
  // If loop does not result in returning false, we return true.
  __ Bind(&return_true);
  __ LoadImmediate(out, 1);
  __ b(&end);

  // Return false and exit the function.
  __ Bind(&return_false);
  __ LoadImmediate(out, 0);
  __ Bind(&end);
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
  SlowPathCode* slow_path = nullptr;
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
  CheckEntrypointTypes<kQuickIndexOf, int32_t, void*, uint32_t, uint32_t>();
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
  GenerateVisitStringIndexOf(
      invoke, GetAssembler(), codegen_, GetAllocator(), /* start_at_zero */ true);
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
  GenerateVisitStringIndexOf(
      invoke, GetAssembler(), codegen_, GetAllocator(), /* start_at_zero */ false);
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
  SlowPathCode* slow_path = new (GetAllocator()) IntrinsicSlowPathARM(invoke);
  codegen_->AddSlowPath(slow_path);
  __ b(slow_path->GetEntryLabel(), EQ);

  __ LoadFromOffset(
      kLoadWord, LR, TR, QUICK_ENTRYPOINT_OFFSET(kArmWordSize, pAllocStringFromBytes).Int32Value());
  CheckEntrypointTypes<kQuickAllocStringFromBytes, void*, void*, int32_t, int32_t, int32_t>();
  __ blx(LR);
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
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

  // No need to emit code checking whether `locations->InAt(2)` is a null
  // pointer, as callers of the native method
  //
  //   java.lang.StringFactory.newStringFromChars(int offset, int charCount, char[] data)
  //
  // all include a null check on `data` before calling that method.
  __ LoadFromOffset(
      kLoadWord, LR, TR, QUICK_ENTRYPOINT_OFFSET(kArmWordSize, pAllocStringFromChars).Int32Value());
  CheckEntrypointTypes<kQuickAllocStringFromChars, void*, int32_t, int32_t, void*>();
  __ blx(LR);
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
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
  SlowPathCode* slow_path = new (GetAllocator()) IntrinsicSlowPathARM(invoke);
  codegen_->AddSlowPath(slow_path);
  __ b(slow_path->GetEntryLabel(), EQ);

  __ LoadFromOffset(kLoadWord,
      LR, TR, QUICK_ENTRYPOINT_OFFSET(kArmWordSize, pAllocStringFromString).Int32Value());
  CheckEntrypointTypes<kQuickAllocStringFromString, void*, void*>();
  __ blx(LR);
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
  __ Bind(slow_path->GetExitLabel());
}

void IntrinsicLocationsBuilderARM::VisitSystemArrayCopy(HInvoke* invoke) {
  CodeGenerator::CreateSystemArrayCopyLocationSummary(invoke);
  LocationSummary* locations = invoke->GetLocations();
  if (locations == nullptr) {
    return;
  }

  HIntConstant* src_pos = invoke->InputAt(1)->AsIntConstant();
  HIntConstant* dest_pos = invoke->InputAt(3)->AsIntConstant();
  HIntConstant* length = invoke->InputAt(4)->AsIntConstant();

  if (src_pos != nullptr && !assembler_->ShifterOperandCanAlwaysHold(src_pos->GetValue())) {
    locations->SetInAt(1, Location::RequiresRegister());
  }
  if (dest_pos != nullptr && !assembler_->ShifterOperandCanAlwaysHold(dest_pos->GetValue())) {
    locations->SetInAt(3, Location::RequiresRegister());
  }
  if (length != nullptr && !assembler_->ShifterOperandCanAlwaysHold(length->GetValue())) {
    locations->SetInAt(4, Location::RequiresRegister());
  }
}

static void CheckPosition(ArmAssembler* assembler,
                          Location pos,
                          Register input,
                          Location length,
                          SlowPathCode* slow_path,
                          Register input_len,
                          Register temp,
                          bool length_is_input_length = false) {
  // Where is the length in the Array?
  const uint32_t length_offset = mirror::Array::LengthOffset().Uint32Value();

  if (pos.IsConstant()) {
    int32_t pos_const = pos.GetConstant()->AsIntConstant()->GetValue();
    if (pos_const == 0) {
      if (!length_is_input_length) {
        // Check that length(input) >= length.
        __ LoadFromOffset(kLoadWord, temp, input, length_offset);
        if (length.IsConstant()) {
          __ cmp(temp, ShifterOperand(length.GetConstant()->AsIntConstant()->GetValue()));
        } else {
          __ cmp(temp, ShifterOperand(length.AsRegister<Register>()));
        }
        __ b(slow_path->GetEntryLabel(), LT);
      }
    } else {
      // Check that length(input) >= pos.
      __ LoadFromOffset(kLoadWord, input_len, input, length_offset);
      __ subs(temp, input_len, ShifterOperand(pos_const));
      __ b(slow_path->GetEntryLabel(), LT);

      // Check that (length(input) - pos) >= length.
      if (length.IsConstant()) {
        __ cmp(temp, ShifterOperand(length.GetConstant()->AsIntConstant()->GetValue()));
      } else {
        __ cmp(temp, ShifterOperand(length.AsRegister<Register>()));
      }
      __ b(slow_path->GetEntryLabel(), LT);
    }
  } else if (length_is_input_length) {
    // The only way the copy can succeed is if pos is zero.
    Register pos_reg = pos.AsRegister<Register>();
    __ CompareAndBranchIfNonZero(pos_reg, slow_path->GetEntryLabel());
  } else {
    // Check that pos >= 0.
    Register pos_reg = pos.AsRegister<Register>();
    __ cmp(pos_reg, ShifterOperand(0));
    __ b(slow_path->GetEntryLabel(), LT);

    // Check that pos <= length(input).
    __ LoadFromOffset(kLoadWord, temp, input, length_offset);
    __ subs(temp, temp, ShifterOperand(pos_reg));
    __ b(slow_path->GetEntryLabel(), LT);

    // Check that (length(input) - pos) >= length.
    if (length.IsConstant()) {
      __ cmp(temp, ShifterOperand(length.GetConstant()->AsIntConstant()->GetValue()));
    } else {
      __ cmp(temp, ShifterOperand(length.AsRegister<Register>()));
    }
    __ b(slow_path->GetEntryLabel(), LT);
  }
}

// TODO: Implement read barriers in the SystemArrayCopy intrinsic.
// Note that this code path is not used (yet) because we do not
// intrinsify methods that can go into the IntrinsicSlowPathARM
// slow path.
void IntrinsicCodeGeneratorARM::VisitSystemArrayCopy(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  uint32_t super_offset = mirror::Class::SuperClassOffset().Int32Value();
  uint32_t component_offset = mirror::Class::ComponentTypeOffset().Int32Value();
  uint32_t primitive_offset = mirror::Class::PrimitiveTypeOffset().Int32Value();

  Register src = locations->InAt(0).AsRegister<Register>();
  Location src_pos = locations->InAt(1);
  Register dest = locations->InAt(2).AsRegister<Register>();
  Location dest_pos = locations->InAt(3);
  Location length = locations->InAt(4);
  Register temp1 = locations->GetTemp(0).AsRegister<Register>();
  Register temp2 = locations->GetTemp(1).AsRegister<Register>();
  Register temp3 = locations->GetTemp(2).AsRegister<Register>();

  SlowPathCode* slow_path = new (GetAllocator()) IntrinsicSlowPathARM(invoke);
  codegen_->AddSlowPath(slow_path);

  Label conditions_on_positions_validated;
  SystemArrayCopyOptimizations optimizations(invoke);

  // If source and destination are the same, we go to slow path if we need to do
  // forward copying.
  if (src_pos.IsConstant()) {
    int32_t src_pos_constant = src_pos.GetConstant()->AsIntConstant()->GetValue();
    if (dest_pos.IsConstant()) {
      int32_t dest_pos_constant = dest_pos.GetConstant()->AsIntConstant()->GetValue();
      if (optimizations.GetDestinationIsSource()) {
        // Checked when building locations.
        DCHECK_GE(src_pos_constant, dest_pos_constant);
      } else if (src_pos_constant < dest_pos_constant) {
        __ cmp(src, ShifterOperand(dest));
        __ b(slow_path->GetEntryLabel(), EQ);
      }

      // Checked when building locations.
      DCHECK(!optimizations.GetDestinationIsSource()
             || (src_pos_constant >= dest_pos.GetConstant()->AsIntConstant()->GetValue()));
    } else {
      if (!optimizations.GetDestinationIsSource()) {
        __ cmp(src, ShifterOperand(dest));
        __ b(&conditions_on_positions_validated, NE);
      }
      __ cmp(dest_pos.AsRegister<Register>(), ShifterOperand(src_pos_constant));
      __ b(slow_path->GetEntryLabel(), GT);
    }
  } else {
    if (!optimizations.GetDestinationIsSource()) {
      __ cmp(src, ShifterOperand(dest));
      __ b(&conditions_on_positions_validated, NE);
    }
    if (dest_pos.IsConstant()) {
      int32_t dest_pos_constant = dest_pos.GetConstant()->AsIntConstant()->GetValue();
      __ cmp(src_pos.AsRegister<Register>(), ShifterOperand(dest_pos_constant));
    } else {
      __ cmp(src_pos.AsRegister<Register>(), ShifterOperand(dest_pos.AsRegister<Register>()));
    }
    __ b(slow_path->GetEntryLabel(), LT);
  }

  __ Bind(&conditions_on_positions_validated);

  if (!optimizations.GetSourceIsNotNull()) {
    // Bail out if the source is null.
    __ CompareAndBranchIfZero(src, slow_path->GetEntryLabel());
  }

  if (!optimizations.GetDestinationIsNotNull() && !optimizations.GetDestinationIsSource()) {
    // Bail out if the destination is null.
    __ CompareAndBranchIfZero(dest, slow_path->GetEntryLabel());
  }

  // If the length is negative, bail out.
  // We have already checked in the LocationsBuilder for the constant case.
  if (!length.IsConstant() &&
      !optimizations.GetCountIsSourceLength() &&
      !optimizations.GetCountIsDestinationLength()) {
    __ cmp(length.AsRegister<Register>(), ShifterOperand(0));
    __ b(slow_path->GetEntryLabel(), LT);
  }

  // Validity checks: source.
  CheckPosition(assembler,
                src_pos,
                src,
                length,
                slow_path,
                temp1,
                temp2,
                optimizations.GetCountIsSourceLength());

  // Validity checks: dest.
  CheckPosition(assembler,
                dest_pos,
                dest,
                length,
                slow_path,
                temp1,
                temp2,
                optimizations.GetCountIsDestinationLength());

  if (!optimizations.GetDoesNotNeedTypeCheck()) {
    // Check whether all elements of the source array are assignable to the component
    // type of the destination array. We do two checks: the classes are the same,
    // or the destination is Object[]. If none of these checks succeed, we go to the
    // slow path.
    __ LoadFromOffset(kLoadWord, temp1, dest, class_offset);
    __ LoadFromOffset(kLoadWord, temp2, src, class_offset);
    bool did_unpoison = false;
    if (!optimizations.GetDestinationIsNonPrimitiveArray() ||
        !optimizations.GetSourceIsNonPrimitiveArray()) {
      // One or two of the references need to be unpoisoned. Unpoison them
      // both to make the identity check valid.
      __ MaybeUnpoisonHeapReference(temp1);
      __ MaybeUnpoisonHeapReference(temp2);
      did_unpoison = true;
    }

    if (!optimizations.GetDestinationIsNonPrimitiveArray()) {
      // Bail out if the destination is not a non primitive array.
      // /* HeapReference<Class> */ temp3 = temp1->component_type_
      __ LoadFromOffset(kLoadWord, temp3, temp1, component_offset);
      __ CompareAndBranchIfZero(temp3, slow_path->GetEntryLabel());
      __ MaybeUnpoisonHeapReference(temp3);
      __ LoadFromOffset(kLoadUnsignedHalfword, temp3, temp3, primitive_offset);
      static_assert(Primitive::kPrimNot == 0, "Expected 0 for kPrimNot");
      __ CompareAndBranchIfNonZero(temp3, slow_path->GetEntryLabel());
    }

    if (!optimizations.GetSourceIsNonPrimitiveArray()) {
      // Bail out if the source is not a non primitive array.
      // /* HeapReference<Class> */ temp3 = temp2->component_type_
      __ LoadFromOffset(kLoadWord, temp3, temp2, component_offset);
      __ CompareAndBranchIfZero(temp3, slow_path->GetEntryLabel());
      __ MaybeUnpoisonHeapReference(temp3);
      __ LoadFromOffset(kLoadUnsignedHalfword, temp3, temp3, primitive_offset);
      static_assert(Primitive::kPrimNot == 0, "Expected 0 for kPrimNot");
      __ CompareAndBranchIfNonZero(temp3, slow_path->GetEntryLabel());
    }

    __ cmp(temp1, ShifterOperand(temp2));

    if (optimizations.GetDestinationIsTypedObjectArray()) {
      Label do_copy;
      __ b(&do_copy, EQ);
      if (!did_unpoison) {
        __ MaybeUnpoisonHeapReference(temp1);
      }
      // /* HeapReference<Class> */ temp1 = temp1->component_type_
      __ LoadFromOffset(kLoadWord, temp1, temp1, component_offset);
      __ MaybeUnpoisonHeapReference(temp1);
      // /* HeapReference<Class> */ temp1 = temp1->super_class_
      __ LoadFromOffset(kLoadWord, temp1, temp1, super_offset);
      // No need to unpoison the result, we're comparing against null.
      __ CompareAndBranchIfNonZero(temp1, slow_path->GetEntryLabel());
      __ Bind(&do_copy);
    } else {
      __ b(slow_path->GetEntryLabel(), NE);
    }
  } else if (!optimizations.GetSourceIsNonPrimitiveArray()) {
    DCHECK(optimizations.GetDestinationIsNonPrimitiveArray());
    // Bail out if the source is not a non primitive array.
    // /* HeapReference<Class> */ temp1 = src->klass_
    __ LoadFromOffset(kLoadWord, temp1, src, class_offset);
    __ MaybeUnpoisonHeapReference(temp1);
    // /* HeapReference<Class> */ temp3 = temp1->component_type_
    __ LoadFromOffset(kLoadWord, temp3, temp1, component_offset);
    __ CompareAndBranchIfZero(temp3, slow_path->GetEntryLabel());
    __ MaybeUnpoisonHeapReference(temp3);
    __ LoadFromOffset(kLoadUnsignedHalfword, temp3, temp3, primitive_offset);
    static_assert(Primitive::kPrimNot == 0, "Expected 0 for kPrimNot");
    __ CompareAndBranchIfNonZero(temp3, slow_path->GetEntryLabel());
  }

  // Compute base source address, base destination address, and end source address.

  uint32_t element_size = sizeof(int32_t);
  uint32_t offset = mirror::Array::DataOffset(element_size).Uint32Value();
  if (src_pos.IsConstant()) {
    int32_t constant = src_pos.GetConstant()->AsIntConstant()->GetValue();
    __ AddConstant(temp1, src, element_size * constant + offset);
  } else {
    __ add(temp1, src, ShifterOperand(src_pos.AsRegister<Register>(), LSL, 2));
    __ AddConstant(temp1, offset);
  }

  if (dest_pos.IsConstant()) {
    int32_t constant = dest_pos.GetConstant()->AsIntConstant()->GetValue();
    __ AddConstant(temp2, dest, element_size * constant + offset);
  } else {
    __ add(temp2, dest, ShifterOperand(dest_pos.AsRegister<Register>(), LSL, 2));
    __ AddConstant(temp2, offset);
  }

  if (length.IsConstant()) {
    int32_t constant = length.GetConstant()->AsIntConstant()->GetValue();
    __ AddConstant(temp3, temp1, element_size * constant);
  } else {
    __ add(temp3, temp1, ShifterOperand(length.AsRegister<Register>(), LSL, 2));
  }

  // Iterate over the arrays and do a raw copy of the objects. We don't need to
  // poison/unpoison, nor do any read barrier as the next uses of the destination
  // array will do it.
  Label loop, done;
  __ cmp(temp1, ShifterOperand(temp3));
  __ b(&done, EQ);
  __ Bind(&loop);
  __ ldr(IP, Address(temp1, element_size, Address::PostIndex));
  __ str(IP, Address(temp2, element_size, Address::PostIndex));
  __ cmp(temp1, ShifterOperand(temp3));
  __ b(&loop, NE);
  __ Bind(&done);

  // We only need one card marking on the destination array.
  codegen_->MarkGCCard(temp1,
                       temp2,
                       dest,
                       Register(kNoRegister),
                       /* value_can_be_null */ false);

  __ Bind(slow_path->GetExitLabel());
}

static void CreateFPToFPCallLocations(ArenaAllocator* arena, HInvoke* invoke) {
  // If the graph is debuggable, all callee-saved floating-point registers are blocked by
  // the code generator. Furthermore, the register allocator creates fixed live intervals
  // for all caller-saved registers because we are doing a function call. As a result, if
  // the input and output locations are unallocated, the register allocator runs out of
  // registers and fails; however, a debuggable graph is not the common case.
  if (invoke->GetBlock()->GetGraph()->IsDebuggable()) {
    return;
  }

  DCHECK_EQ(invoke->GetNumberOfArguments(), 1U);
  DCHECK_EQ(invoke->InputAt(0)->GetType(), Primitive::kPrimDouble);
  DCHECK_EQ(invoke->GetType(), Primitive::kPrimDouble);

  LocationSummary* const locations = new (arena) LocationSummary(invoke,
                                                                 LocationSummary::kCall,
                                                                 kIntrinsified);
  const InvokeRuntimeCallingConvention calling_convention;

  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetOut(Location::RequiresFpuRegister());
  // Native code uses the soft float ABI.
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
}

static void CreateFPFPToFPCallLocations(ArenaAllocator* arena, HInvoke* invoke) {
  // If the graph is debuggable, all callee-saved floating-point registers are blocked by
  // the code generator. Furthermore, the register allocator creates fixed live intervals
  // for all caller-saved registers because we are doing a function call. As a result, if
  // the input and output locations are unallocated, the register allocator runs out of
  // registers and fails; however, a debuggable graph is not the common case.
  if (invoke->GetBlock()->GetGraph()->IsDebuggable()) {
    return;
  }

  DCHECK_EQ(invoke->GetNumberOfArguments(), 2U);
  DCHECK_EQ(invoke->InputAt(0)->GetType(), Primitive::kPrimDouble);
  DCHECK_EQ(invoke->InputAt(1)->GetType(), Primitive::kPrimDouble);
  DCHECK_EQ(invoke->GetType(), Primitive::kPrimDouble);

  LocationSummary* const locations = new (arena) LocationSummary(invoke,
                                                                 LocationSummary::kCall,
                                                                 kIntrinsified);
  const InvokeRuntimeCallingConvention calling_convention;

  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetInAt(1, Location::RequiresFpuRegister());
  locations->SetOut(Location::RequiresFpuRegister());
  // Native code uses the soft float ABI.
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(3)));
}

static void GenFPToFPCall(HInvoke* invoke,
                          ArmAssembler* assembler,
                          CodeGeneratorARM* codegen,
                          QuickEntrypointEnum entry) {
  LocationSummary* const locations = invoke->GetLocations();
  const InvokeRuntimeCallingConvention calling_convention;

  DCHECK_EQ(invoke->GetNumberOfArguments(), 1U);
  DCHECK(locations->WillCall() && locations->Intrinsified());
  DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(calling_convention.GetRegisterAt(0)));
  DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(calling_convention.GetRegisterAt(1)));

  __ LoadFromOffset(kLoadWord, LR, TR, GetThreadOffset<kArmWordSize>(entry).Int32Value());
  // Native code uses the soft float ABI.
  __ vmovrrd(calling_convention.GetRegisterAt(0),
             calling_convention.GetRegisterAt(1),
             FromLowSToD(locations->InAt(0).AsFpuRegisterPairLow<SRegister>()));
  __ blx(LR);
  codegen->RecordPcInfo(invoke, invoke->GetDexPc());
  __ vmovdrr(FromLowSToD(locations->Out().AsFpuRegisterPairLow<SRegister>()),
             calling_convention.GetRegisterAt(0),
             calling_convention.GetRegisterAt(1));
}

static void GenFPFPToFPCall(HInvoke* invoke,
                          ArmAssembler* assembler,
                          CodeGeneratorARM* codegen,
                          QuickEntrypointEnum entry) {
  LocationSummary* const locations = invoke->GetLocations();
  const InvokeRuntimeCallingConvention calling_convention;

  DCHECK_EQ(invoke->GetNumberOfArguments(), 2U);
  DCHECK(locations->WillCall() && locations->Intrinsified());
  DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(calling_convention.GetRegisterAt(0)));
  DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(calling_convention.GetRegisterAt(1)));
  DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(calling_convention.GetRegisterAt(2)));
  DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(calling_convention.GetRegisterAt(3)));

  __ LoadFromOffset(kLoadWord, LR, TR, GetThreadOffset<kArmWordSize>(entry).Int32Value());
  // Native code uses the soft float ABI.
  __ vmovrrd(calling_convention.GetRegisterAt(0),
             calling_convention.GetRegisterAt(1),
             FromLowSToD(locations->InAt(0).AsFpuRegisterPairLow<SRegister>()));
  __ vmovrrd(calling_convention.GetRegisterAt(2),
             calling_convention.GetRegisterAt(3),
             FromLowSToD(locations->InAt(1).AsFpuRegisterPairLow<SRegister>()));
  __ blx(LR);
  codegen->RecordPcInfo(invoke, invoke->GetDexPc());
  __ vmovdrr(FromLowSToD(locations->Out().AsFpuRegisterPairLow<SRegister>()),
             calling_convention.GetRegisterAt(0),
             calling_convention.GetRegisterAt(1));
}

void IntrinsicLocationsBuilderARM::VisitMathCos(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathCos(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetAssembler(), codegen_, kQuickCos);
}

void IntrinsicLocationsBuilderARM::VisitMathSin(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathSin(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetAssembler(), codegen_, kQuickSin);
}

void IntrinsicLocationsBuilderARM::VisitMathAcos(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathAcos(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetAssembler(), codegen_, kQuickAcos);
}

void IntrinsicLocationsBuilderARM::VisitMathAsin(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathAsin(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetAssembler(), codegen_, kQuickAsin);
}

void IntrinsicLocationsBuilderARM::VisitMathAtan(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathAtan(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetAssembler(), codegen_, kQuickAtan);
}

void IntrinsicLocationsBuilderARM::VisitMathCbrt(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathCbrt(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetAssembler(), codegen_, kQuickCbrt);
}

void IntrinsicLocationsBuilderARM::VisitMathCosh(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathCosh(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetAssembler(), codegen_, kQuickCosh);
}

void IntrinsicLocationsBuilderARM::VisitMathExp(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathExp(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetAssembler(), codegen_, kQuickExp);
}

void IntrinsicLocationsBuilderARM::VisitMathExpm1(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathExpm1(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetAssembler(), codegen_, kQuickExpm1);
}

void IntrinsicLocationsBuilderARM::VisitMathLog(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathLog(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetAssembler(), codegen_, kQuickLog);
}

void IntrinsicLocationsBuilderARM::VisitMathLog10(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathLog10(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetAssembler(), codegen_, kQuickLog10);
}

void IntrinsicLocationsBuilderARM::VisitMathSinh(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathSinh(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetAssembler(), codegen_, kQuickSinh);
}

void IntrinsicLocationsBuilderARM::VisitMathTan(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathTan(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetAssembler(), codegen_, kQuickTan);
}

void IntrinsicLocationsBuilderARM::VisitMathTanh(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathTanh(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetAssembler(), codegen_, kQuickTanh);
}

void IntrinsicLocationsBuilderARM::VisitMathAtan2(HInvoke* invoke) {
  CreateFPFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathAtan2(HInvoke* invoke) {
  GenFPFPToFPCall(invoke, GetAssembler(), codegen_, kQuickAtan2);
}

void IntrinsicLocationsBuilderARM::VisitMathHypot(HInvoke* invoke) {
  CreateFPFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathHypot(HInvoke* invoke) {
  GenFPFPToFPCall(invoke, GetAssembler(), codegen_, kQuickHypot);
}

void IntrinsicLocationsBuilderARM::VisitMathNextAfter(HInvoke* invoke) {
  CreateFPFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitMathNextAfter(HInvoke* invoke) {
  GenFPFPToFPCall(invoke, GetAssembler(), codegen_, kQuickNextAfter);
}

void IntrinsicLocationsBuilderARM::VisitIntegerReverse(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitIntegerReverse(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  Register out = locations->Out().AsRegister<Register>();
  Register in  = locations->InAt(0).AsRegister<Register>();

  __ rbit(out, in);
}

void IntrinsicLocationsBuilderARM::VisitLongReverse(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
}

void IntrinsicCodeGeneratorARM::VisitLongReverse(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  Register in_reg_lo  = locations->InAt(0).AsRegisterPairLow<Register>();
  Register in_reg_hi  = locations->InAt(0).AsRegisterPairHigh<Register>();
  Register out_reg_lo = locations->Out().AsRegisterPairLow<Register>();
  Register out_reg_hi = locations->Out().AsRegisterPairHigh<Register>();

  __ rbit(out_reg_lo, in_reg_hi);
  __ rbit(out_reg_hi, in_reg_lo);
}

void IntrinsicLocationsBuilderARM::VisitIntegerReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitIntegerReverseBytes(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  Register out = locations->Out().AsRegister<Register>();
  Register in  = locations->InAt(0).AsRegister<Register>();

  __ rev(out, in);
}

void IntrinsicLocationsBuilderARM::VisitLongReverseBytes(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
}

void IntrinsicCodeGeneratorARM::VisitLongReverseBytes(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  Register in_reg_lo  = locations->InAt(0).AsRegisterPairLow<Register>();
  Register in_reg_hi  = locations->InAt(0).AsRegisterPairHigh<Register>();
  Register out_reg_lo = locations->Out().AsRegisterPairLow<Register>();
  Register out_reg_hi = locations->Out().AsRegisterPairHigh<Register>();

  __ rev(out_reg_lo, in_reg_hi);
  __ rev(out_reg_hi, in_reg_lo);
}

void IntrinsicLocationsBuilderARM::VisitShortReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM::VisitShortReverseBytes(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  Register out = locations->Out().AsRegister<Register>();
  Register in  = locations->InAt(0).AsRegister<Register>();

  __ revsh(out, in);
}

void IntrinsicLocationsBuilderARM::VisitStringGetCharsNoCheck(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetInAt(2, Location::RequiresRegister());
  locations->SetInAt(3, Location::RequiresRegister());
  locations->SetInAt(4, Location::RequiresRegister());

  locations->AddTemp(Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
}

void IntrinsicCodeGeneratorARM::VisitStringGetCharsNoCheck(HInvoke* invoke) {
  ArmAssembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  // Check assumption that sizeof(Char) is 2 (used in scaling below).
  const size_t char_size = Primitive::ComponentSize(Primitive::kPrimChar);
  DCHECK_EQ(char_size, 2u);

  // Location of data in char array buffer.
  const uint32_t data_offset = mirror::Array::DataOffset(char_size).Uint32Value();

  // Location of char array data in string.
  const uint32_t value_offset = mirror::String::ValueOffset().Uint32Value();

  // void getCharsNoCheck(int srcBegin, int srcEnd, char[] dst, int dstBegin);
  // Since getChars() calls getCharsNoCheck() - we use registers rather than constants.
  Register srcObj = locations->InAt(0).AsRegister<Register>();
  Register srcBegin = locations->InAt(1).AsRegister<Register>();
  Register srcEnd = locations->InAt(2).AsRegister<Register>();
  Register dstObj = locations->InAt(3).AsRegister<Register>();
  Register dstBegin = locations->InAt(4).AsRegister<Register>();

  Register src_ptr = locations->GetTemp(0).AsRegister<Register>();
  Register src_ptr_end = locations->GetTemp(1).AsRegister<Register>();
  Register dst_ptr = locations->GetTemp(2).AsRegister<Register>();
  Register tmp = locations->GetTemp(3).AsRegister<Register>();

  // src range to copy.
  __ add(src_ptr, srcObj, ShifterOperand(value_offset));
  __ add(src_ptr_end, src_ptr, ShifterOperand(srcEnd, LSL, 1));
  __ add(src_ptr, src_ptr, ShifterOperand(srcBegin, LSL, 1));

  // dst to be copied.
  __ add(dst_ptr, dstObj, ShifterOperand(data_offset));
  __ add(dst_ptr, dst_ptr, ShifterOperand(dstBegin, LSL, 1));

  // Do the copy.
  Label loop, done;
  __ Bind(&loop);
  __ cmp(src_ptr, ShifterOperand(src_ptr_end));
  __ b(&done, EQ);
  __ ldrh(tmp, Address(src_ptr, char_size, Address::PostIndex));
  __ strh(tmp, Address(dst_ptr, char_size, Address::PostIndex));
  __ b(&loop);
  __ Bind(&done);
}

UNIMPLEMENTED_INTRINSIC(ARM, IntegerBitCount)
UNIMPLEMENTED_INTRINSIC(ARM, LongBitCount)
UNIMPLEMENTED_INTRINSIC(ARM, MathMinDoubleDouble)
UNIMPLEMENTED_INTRINSIC(ARM, MathMinFloatFloat)
UNIMPLEMENTED_INTRINSIC(ARM, MathMaxDoubleDouble)
UNIMPLEMENTED_INTRINSIC(ARM, MathMaxFloatFloat)
UNIMPLEMENTED_INTRINSIC(ARM, MathMinLongLong)
UNIMPLEMENTED_INTRINSIC(ARM, MathMaxLongLong)
UNIMPLEMENTED_INTRINSIC(ARM, MathCeil)          // Could be done by changing rounding mode, maybe?
UNIMPLEMENTED_INTRINSIC(ARM, MathFloor)         // Could be done by changing rounding mode, maybe?
UNIMPLEMENTED_INTRINSIC(ARM, MathRint)
UNIMPLEMENTED_INTRINSIC(ARM, MathRoundDouble)   // Could be done by changing rounding mode, maybe?
UNIMPLEMENTED_INTRINSIC(ARM, MathRoundFloat)    // Could be done by changing rounding mode, maybe?
UNIMPLEMENTED_INTRINSIC(ARM, UnsafeCASLong)     // High register pressure.
UNIMPLEMENTED_INTRINSIC(ARM, SystemArrayCopyChar)
UNIMPLEMENTED_INTRINSIC(ARM, ReferenceGetReferent)
UNIMPLEMENTED_INTRINSIC(ARM, FloatIsInfinite)
UNIMPLEMENTED_INTRINSIC(ARM, DoubleIsInfinite)
UNIMPLEMENTED_INTRINSIC(ARM, IntegerHighestOneBit)
UNIMPLEMENTED_INTRINSIC(ARM, LongHighestOneBit)
UNIMPLEMENTED_INTRINSIC(ARM, IntegerLowestOneBit)
UNIMPLEMENTED_INTRINSIC(ARM, LongLowestOneBit)

// 1.8.
UNIMPLEMENTED_INTRINSIC(ARM, UnsafeGetAndAddInt)
UNIMPLEMENTED_INTRINSIC(ARM, UnsafeGetAndAddLong)
UNIMPLEMENTED_INTRINSIC(ARM, UnsafeGetAndSetInt)
UNIMPLEMENTED_INTRINSIC(ARM, UnsafeGetAndSetLong)
UNIMPLEMENTED_INTRINSIC(ARM, UnsafeGetAndSetObject)

UNREACHABLE_INTRINSICS(ARM)

#undef __

}  // namespace arm
}  // namespace art
