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

#include "intrinsics_x86_64.h"

#include <limits>

#include "arch/x86_64/instruction_set_features_x86_64.h"
#include "art_method-inl.h"
#include "base/bit_utils.h"
#include "code_generator_x86_64.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "intrinsics.h"
#include "intrinsics_utils.h"
#include "mirror/array-inl.h"
#include "mirror/string.h"
#include "thread.h"
#include "utils/x86_64/assembler_x86_64.h"
#include "utils/x86_64/constants_x86_64.h"

namespace art {

namespace x86_64 {

IntrinsicLocationsBuilderX86_64::IntrinsicLocationsBuilderX86_64(CodeGeneratorX86_64* codegen)
  : arena_(codegen->GetGraph()->GetArena()), codegen_(codegen) {
}


X86_64Assembler* IntrinsicCodeGeneratorX86_64::GetAssembler() {
  return down_cast<X86_64Assembler*>(codegen_->GetAssembler());
}

ArenaAllocator* IntrinsicCodeGeneratorX86_64::GetAllocator() {
  return codegen_->GetGraph()->GetArena();
}

bool IntrinsicLocationsBuilderX86_64::TryDispatch(HInvoke* invoke) {
  Dispatch(invoke);
  LocationSummary* res = invoke->GetLocations();
  if (res == nullptr) {
    return false;
  }
  if (kEmitCompilerReadBarrier && res->CanCall()) {
    // Generating an intrinsic for this HInvoke may produce an
    // IntrinsicSlowPathX86_64 slow path.  Currently this approach
    // does not work when using read barriers, as the emitted
    // calling sequence will make use of another slow path
    // (ReadBarrierForRootSlowPathX86_64 for HInvokeStaticOrDirect,
    // ReadBarrierSlowPathX86_64 for HInvokeVirtual).  So we bail
    // out in this case.
    //
    // TODO: Find a way to have intrinsics work with read barriers.
    invoke->SetLocations(nullptr);
    return false;
  }
  return res->Intrinsified();
}

static void MoveArguments(HInvoke* invoke, CodeGeneratorX86_64* codegen) {
  InvokeDexCallingConventionVisitorX86_64 calling_convention_visitor;
  IntrinsicVisitor::MoveArguments(invoke, codegen, &calling_convention_visitor);
}

using IntrinsicSlowPathX86_64 = IntrinsicSlowPath<InvokeDexCallingConventionVisitorX86_64>;

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

static void MoveFPToInt(LocationSummary* locations, bool is64bit, X86_64Assembler* assembler) {
  Location input = locations->InAt(0);
  Location output = locations->Out();
  __ movd(output.AsRegister<CpuRegister>(), input.AsFpuRegister<XmmRegister>(), is64bit);
}

static void MoveIntToFP(LocationSummary* locations, bool is64bit, X86_64Assembler* assembler) {
  Location input = locations->InAt(0);
  Location output = locations->Out();
  __ movd(output.AsFpuRegister<XmmRegister>(), input.AsRegister<CpuRegister>(), is64bit);
}

void IntrinsicLocationsBuilderX86_64::VisitDoubleDoubleToRawLongBits(HInvoke* invoke) {
  CreateFPToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitDoubleLongBitsToDouble(HInvoke* invoke) {
  CreateIntToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitDoubleDoubleToRawLongBits(HInvoke* invoke) {
  MoveFPToInt(invoke->GetLocations(), /* is64bit */ true, GetAssembler());
}
void IntrinsicCodeGeneratorX86_64::VisitDoubleLongBitsToDouble(HInvoke* invoke) {
  MoveIntToFP(invoke->GetLocations(), /* is64bit */ true, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitFloatFloatToRawIntBits(HInvoke* invoke) {
  CreateFPToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitFloatIntBitsToFloat(HInvoke* invoke) {
  CreateIntToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitFloatFloatToRawIntBits(HInvoke* invoke) {
  MoveFPToInt(invoke->GetLocations(), /* is64bit */ false, GetAssembler());
}
void IntrinsicCodeGeneratorX86_64::VisitFloatIntBitsToFloat(HInvoke* invoke) {
  MoveIntToFP(invoke->GetLocations(), /* is64bit */ false, GetAssembler());
}

static void CreateIntToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
}

static void GenReverseBytes(LocationSummary* locations,
                            Primitive::Type size,
                            X86_64Assembler* assembler) {
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();

  switch (size) {
    case Primitive::kPrimShort:
      // TODO: Can be done with an xchg of 8b registers. This is straight from Quick.
      __ bswapl(out);
      __ sarl(out, Immediate(16));
      break;
    case Primitive::kPrimInt:
      __ bswapl(out);
      break;
    case Primitive::kPrimLong:
      __ bswapq(out);
      break;
    default:
      LOG(FATAL) << "Unexpected size for reverse-bytes: " << size;
      UNREACHABLE();
  }
}

void IntrinsicLocationsBuilderX86_64::VisitIntegerReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitIntegerReverseBytes(HInvoke* invoke) {
  GenReverseBytes(invoke->GetLocations(), Primitive::kPrimInt, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitLongReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitLongReverseBytes(HInvoke* invoke) {
  GenReverseBytes(invoke->GetLocations(), Primitive::kPrimLong, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitShortReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitShortReverseBytes(HInvoke* invoke) {
  GenReverseBytes(invoke->GetLocations(), Primitive::kPrimShort, GetAssembler());
}


// TODO: Consider Quick's way of doing Double abs through integer operations, as the immediate we
//       need is 64b.

static void CreateFloatToFloatPlusTemps(ArenaAllocator* arena, HInvoke* invoke) {
  // TODO: Enable memory operations when the assembler supports them.
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetOut(Location::SameAsFirstInput());
  locations->AddTemp(Location::RequiresFpuRegister());  // FP reg to hold mask.
}

static void MathAbsFP(LocationSummary* locations,
                      bool is64bit,
                      X86_64Assembler* assembler,
                      CodeGeneratorX86_64* codegen) {
  Location output = locations->Out();

  DCHECK(output.IsFpuRegister());
  XmmRegister xmm_temp = locations->GetTemp(0).AsFpuRegister<XmmRegister>();

  // TODO: Can mask directly with constant area using pand if we can guarantee
  // that the literal is aligned on a 16 byte boundary.  This will avoid a
  // temporary.
  if (is64bit) {
    __ movsd(xmm_temp, codegen->LiteralInt64Address(INT64_C(0x7FFFFFFFFFFFFFFF)));
    __ andpd(output.AsFpuRegister<XmmRegister>(), xmm_temp);
  } else {
    __ movss(xmm_temp, codegen->LiteralInt32Address(INT32_C(0x7FFFFFFF)));
    __ andps(output.AsFpuRegister<XmmRegister>(), xmm_temp);
  }
}

void IntrinsicLocationsBuilderX86_64::VisitMathAbsDouble(HInvoke* invoke) {
  CreateFloatToFloatPlusTemps(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathAbsDouble(HInvoke* invoke) {
  MathAbsFP(invoke->GetLocations(), /* is64bit */ true, GetAssembler(), codegen_);
}

void IntrinsicLocationsBuilderX86_64::VisitMathAbsFloat(HInvoke* invoke) {
  CreateFloatToFloatPlusTemps(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathAbsFloat(HInvoke* invoke) {
  MathAbsFP(invoke->GetLocations(), /* is64bit */ false, GetAssembler(), codegen_);
}

static void CreateIntToIntPlusTemp(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
  locations->AddTemp(Location::RequiresRegister());
}

static void GenAbsInteger(LocationSummary* locations, bool is64bit, X86_64Assembler* assembler) {
  Location output = locations->Out();
  CpuRegister out = output.AsRegister<CpuRegister>();
  CpuRegister mask = locations->GetTemp(0).AsRegister<CpuRegister>();

  if (is64bit) {
    // Create mask.
    __ movq(mask, out);
    __ sarq(mask, Immediate(63));
    // Add mask.
    __ addq(out, mask);
    __ xorq(out, mask);
  } else {
    // Create mask.
    __ movl(mask, out);
    __ sarl(mask, Immediate(31));
    // Add mask.
    __ addl(out, mask);
    __ xorl(out, mask);
  }
}

void IntrinsicLocationsBuilderX86_64::VisitMathAbsInt(HInvoke* invoke) {
  CreateIntToIntPlusTemp(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathAbsInt(HInvoke* invoke) {
  GenAbsInteger(invoke->GetLocations(), /* is64bit */ false, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitMathAbsLong(HInvoke* invoke) {
  CreateIntToIntPlusTemp(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathAbsLong(HInvoke* invoke) {
  GenAbsInteger(invoke->GetLocations(), /* is64bit */ true, GetAssembler());
}

static void GenMinMaxFP(LocationSummary* locations,
                        bool is_min,
                        bool is_double,
                        X86_64Assembler* assembler,
                        CodeGeneratorX86_64* codegen) {
  Location op1_loc = locations->InAt(0);
  Location op2_loc = locations->InAt(1);
  Location out_loc = locations->Out();
  XmmRegister out = out_loc.AsFpuRegister<XmmRegister>();

  // Shortcut for same input locations.
  if (op1_loc.Equals(op2_loc)) {
    DCHECK(out_loc.Equals(op1_loc));
    return;
  }

  //  (out := op1)
  //  out <=? op2
  //  if Nan jmp Nan_label
  //  if out is min jmp done
  //  if op2 is min jmp op2_label
  //  handle -0/+0
  //  jmp done
  // Nan_label:
  //  out := NaN
  // op2_label:
  //  out := op2
  // done:
  //
  // This removes one jmp, but needs to copy one input (op1) to out.
  //
  // TODO: This is straight from Quick. Make NaN an out-of-line slowpath?

  XmmRegister op2 = op2_loc.AsFpuRegister<XmmRegister>();

  NearLabel nan, done, op2_label;
  if (is_double) {
    __ ucomisd(out, op2);
  } else {
    __ ucomiss(out, op2);
  }

  __ j(Condition::kParityEven, &nan);

  __ j(is_min ? Condition::kAbove : Condition::kBelow, &op2_label);
  __ j(is_min ? Condition::kBelow : Condition::kAbove, &done);

  // Handle 0.0/-0.0.
  if (is_min) {
    if (is_double) {
      __ orpd(out, op2);
    } else {
      __ orps(out, op2);
    }
  } else {
    if (is_double) {
      __ andpd(out, op2);
    } else {
      __ andps(out, op2);
    }
  }
  __ jmp(&done);

  // NaN handling.
  __ Bind(&nan);
  if (is_double) {
    __ movsd(out, codegen->LiteralInt64Address(INT64_C(0x7FF8000000000000)));
  } else {
    __ movss(out, codegen->LiteralInt32Address(INT32_C(0x7FC00000)));
  }
  __ jmp(&done);

  // out := op2;
  __ Bind(&op2_label);
  if (is_double) {
    __ movsd(out, op2);
  } else {
    __ movss(out, op2);
  }

  // Done.
  __ Bind(&done);
}

static void CreateFPFPToFP(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetInAt(1, Location::RequiresFpuRegister());
  // The following is sub-optimal, but all we can do for now. It would be fine to also accept
  // the second input to be the output (we can simply swap inputs).
  locations->SetOut(Location::SameAsFirstInput());
}

void IntrinsicLocationsBuilderX86_64::VisitMathMinDoubleDouble(HInvoke* invoke) {
  CreateFPFPToFP(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathMinDoubleDouble(HInvoke* invoke) {
  GenMinMaxFP(
      invoke->GetLocations(), /* is_min */ true, /* is_double */ true, GetAssembler(), codegen_);
}

void IntrinsicLocationsBuilderX86_64::VisitMathMinFloatFloat(HInvoke* invoke) {
  CreateFPFPToFP(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathMinFloatFloat(HInvoke* invoke) {
  GenMinMaxFP(
      invoke->GetLocations(), /* is_min */ true, /* is_double */ false, GetAssembler(), codegen_);
}

void IntrinsicLocationsBuilderX86_64::VisitMathMaxDoubleDouble(HInvoke* invoke) {
  CreateFPFPToFP(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathMaxDoubleDouble(HInvoke* invoke) {
  GenMinMaxFP(
      invoke->GetLocations(), /* is_min */ false, /* is_double */ true, GetAssembler(), codegen_);
}

void IntrinsicLocationsBuilderX86_64::VisitMathMaxFloatFloat(HInvoke* invoke) {
  CreateFPFPToFP(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathMaxFloatFloat(HInvoke* invoke) {
  GenMinMaxFP(
      invoke->GetLocations(), /* is_min */ false, /* is_double */ false, GetAssembler(), codegen_);
}

static void GenMinMax(LocationSummary* locations, bool is_min, bool is_long,
                      X86_64Assembler* assembler) {
  Location op1_loc = locations->InAt(0);
  Location op2_loc = locations->InAt(1);

  // Shortcut for same input locations.
  if (op1_loc.Equals(op2_loc)) {
    // Can return immediately, as op1_loc == out_loc.
    // Note: if we ever support separate registers, e.g., output into memory, we need to check for
    //       a copy here.
    DCHECK(locations->Out().Equals(op1_loc));
    return;
  }

  CpuRegister out = locations->Out().AsRegister<CpuRegister>();
  CpuRegister op2 = op2_loc.AsRegister<CpuRegister>();

  //  (out := op1)
  //  out <=? op2
  //  if out is min jmp done
  //  out := op2
  // done:

  if (is_long) {
    __ cmpq(out, op2);
  } else {
    __ cmpl(out, op2);
  }

  __ cmov(is_min ? Condition::kGreater : Condition::kLess, out, op2, is_long);
}

static void CreateIntIntToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
}

void IntrinsicLocationsBuilderX86_64::VisitMathMinIntInt(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathMinIntInt(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), /* is_min */ true, /* is_long */ false, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitMathMinLongLong(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathMinLongLong(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), /* is_min */ true, /* is_long */ true, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitMathMaxIntInt(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathMaxIntInt(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), /* is_min */ false, /* is_long */ false, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitMathMaxLongLong(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathMaxLongLong(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), /* is_min */ false, /* is_long */ true, GetAssembler());
}

static void CreateFPToFPLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetOut(Location::RequiresFpuRegister());
}

void IntrinsicLocationsBuilderX86_64::VisitMathSqrt(HInvoke* invoke) {
  CreateFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathSqrt(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  XmmRegister in = locations->InAt(0).AsFpuRegister<XmmRegister>();
  XmmRegister out = locations->Out().AsFpuRegister<XmmRegister>();

  GetAssembler()->sqrtsd(out, in);
}

static void InvokeOutOfLineIntrinsic(CodeGeneratorX86_64* codegen, HInvoke* invoke) {
  MoveArguments(invoke, codegen);

  DCHECK(invoke->IsInvokeStaticOrDirect());
  codegen->GenerateStaticOrDirectCall(
      invoke->AsInvokeStaticOrDirect(), Location::RegisterLocation(RDI));
  codegen->RecordPcInfo(invoke, invoke->GetDexPc());

  // Copy the result back to the expected output.
  Location out = invoke->GetLocations()->Out();
  if (out.IsValid()) {
    DCHECK(out.IsRegister());
    codegen->MoveFromReturnRegister(out, invoke->GetType());
  }
}

static void CreateSSE41FPToFPLocations(ArenaAllocator* arena,
                                      HInvoke* invoke,
                                      CodeGeneratorX86_64* codegen) {
  // Do we have instruction support?
  if (codegen->GetInstructionSetFeatures().HasSSE4_1()) {
    CreateFPToFPLocations(arena, invoke);
    return;
  }

  // We have to fall back to a call to the intrinsic.
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetFpuRegisterAt(0)));
  locations->SetOut(Location::FpuRegisterLocation(XMM0));
  // Needs to be RDI for the invoke.
  locations->AddTemp(Location::RegisterLocation(RDI));
}

static void GenSSE41FPToFPIntrinsic(CodeGeneratorX86_64* codegen,
                                   HInvoke* invoke,
                                   X86_64Assembler* assembler,
                                   int round_mode) {
  LocationSummary* locations = invoke->GetLocations();
  if (locations->WillCall()) {
    InvokeOutOfLineIntrinsic(codegen, invoke);
  } else {
    XmmRegister in = locations->InAt(0).AsFpuRegister<XmmRegister>();
    XmmRegister out = locations->Out().AsFpuRegister<XmmRegister>();
    __ roundsd(out, in, Immediate(round_mode));
  }
}

void IntrinsicLocationsBuilderX86_64::VisitMathCeil(HInvoke* invoke) {
  CreateSSE41FPToFPLocations(arena_, invoke, codegen_);
}

void IntrinsicCodeGeneratorX86_64::VisitMathCeil(HInvoke* invoke) {
  GenSSE41FPToFPIntrinsic(codegen_, invoke, GetAssembler(), 2);
}

void IntrinsicLocationsBuilderX86_64::VisitMathFloor(HInvoke* invoke) {
  CreateSSE41FPToFPLocations(arena_, invoke, codegen_);
}

void IntrinsicCodeGeneratorX86_64::VisitMathFloor(HInvoke* invoke) {
  GenSSE41FPToFPIntrinsic(codegen_, invoke, GetAssembler(), 1);
}

void IntrinsicLocationsBuilderX86_64::VisitMathRint(HInvoke* invoke) {
  CreateSSE41FPToFPLocations(arena_, invoke, codegen_);
}

void IntrinsicCodeGeneratorX86_64::VisitMathRint(HInvoke* invoke) {
  GenSSE41FPToFPIntrinsic(codegen_, invoke, GetAssembler(), 0);
}

static void CreateSSE41FPToIntLocations(ArenaAllocator* arena,
                                       HInvoke* invoke,
                                       CodeGeneratorX86_64* codegen) {
  // Do we have instruction support?
  if (codegen->GetInstructionSetFeatures().HasSSE4_1()) {
    LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                              LocationSummary::kNoCall,
                                                              kIntrinsified);
    locations->SetInAt(0, Location::RequiresFpuRegister());
    locations->SetOut(Location::RequiresRegister());
    locations->AddTemp(Location::RequiresFpuRegister());
    return;
  }

  // We have to fall back to a call to the intrinsic.
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetFpuRegisterAt(0)));
  locations->SetOut(Location::RegisterLocation(RAX));
  // Needs to be RDI for the invoke.
  locations->AddTemp(Location::RegisterLocation(RDI));
}

void IntrinsicLocationsBuilderX86_64::VisitMathRoundFloat(HInvoke* invoke) {
  // See intrinsics.h.
  if (kRoundIsPlusPointFive) {
    CreateSSE41FPToIntLocations(arena_, invoke, codegen_);
  }
}

void IntrinsicCodeGeneratorX86_64::VisitMathRoundFloat(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  if (locations->WillCall()) {
    InvokeOutOfLineIntrinsic(codegen_, invoke);
    return;
  }

  // Implement RoundFloat as t1 = floor(input + 0.5f);  convert to int.
  XmmRegister in = locations->InAt(0).AsFpuRegister<XmmRegister>();
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();
  XmmRegister inPlusPointFive = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
  NearLabel done, nan;
  X86_64Assembler* assembler = GetAssembler();

  // Load 0.5 into inPlusPointFive.
  __ movss(inPlusPointFive, codegen_->LiteralFloatAddress(0.5f));

  // Add in the input.
  __ addss(inPlusPointFive, in);

  // And truncate to an integer.
  __ roundss(inPlusPointFive, inPlusPointFive, Immediate(1));

  // Load maxInt into out.
  codegen_->Load64BitValue(out, kPrimIntMax);

  // if inPlusPointFive >= maxInt goto done
  __ comiss(inPlusPointFive, codegen_->LiteralFloatAddress(static_cast<float>(kPrimIntMax)));
  __ j(kAboveEqual, &done);

  // if input == NaN goto nan
  __ j(kUnordered, &nan);

  // output = float-to-int-truncate(input)
  __ cvttss2si(out, inPlusPointFive);
  __ jmp(&done);
  __ Bind(&nan);

  //  output = 0
  __ xorl(out, out);
  __ Bind(&done);
}

void IntrinsicLocationsBuilderX86_64::VisitMathRoundDouble(HInvoke* invoke) {
  // See intrinsics.h.
  if (kRoundIsPlusPointFive) {
    CreateSSE41FPToIntLocations(arena_, invoke, codegen_);
  }
}

void IntrinsicCodeGeneratorX86_64::VisitMathRoundDouble(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  if (locations->WillCall()) {
    InvokeOutOfLineIntrinsic(codegen_, invoke);
    return;
  }

  // Implement RoundDouble as t1 = floor(input + 0.5);  convert to long.
  XmmRegister in = locations->InAt(0).AsFpuRegister<XmmRegister>();
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();
  XmmRegister inPlusPointFive = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
  NearLabel done, nan;
  X86_64Assembler* assembler = GetAssembler();

  // Load 0.5 into inPlusPointFive.
  __ movsd(inPlusPointFive, codegen_->LiteralDoubleAddress(0.5));

  // Add in the input.
  __ addsd(inPlusPointFive, in);

  // And truncate to an integer.
  __ roundsd(inPlusPointFive, inPlusPointFive, Immediate(1));

  // Load maxLong into out.
  codegen_->Load64BitValue(out, kPrimLongMax);

  // if inPlusPointFive >= maxLong goto done
  __ comisd(inPlusPointFive, codegen_->LiteralDoubleAddress(static_cast<double>(kPrimLongMax)));
  __ j(kAboveEqual, &done);

  // if input == NaN goto nan
  __ j(kUnordered, &nan);

  // output = double-to-long-truncate(input)
  __ cvttsd2si(out, inPlusPointFive, /* is64bit */ true);
  __ jmp(&done);
  __ Bind(&nan);

  //  output = 0
  __ xorl(out, out);
  __ Bind(&done);
}

static void CreateFPToFPCallLocations(ArenaAllocator* arena,
                                      HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kCall,
                                                           kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(0)));
  locations->SetOut(Location::FpuRegisterLocation(XMM0));

  // We have to ensure that the native code doesn't clobber the XMM registers which are
  // non-volatile for ART, but volatile for Native calls.  This will ensure that they are
  // saved in the prologue and properly restored.
  for (auto fp_reg : non_volatile_xmm_regs) {
    locations->AddTemp(Location::FpuRegisterLocation(fp_reg));
  }
}

static void GenFPToFPCall(HInvoke* invoke, CodeGeneratorX86_64* codegen,
                          QuickEntrypointEnum entry) {
  LocationSummary* locations = invoke->GetLocations();
  DCHECK(locations->WillCall());
  DCHECK(invoke->IsInvokeStaticOrDirect());
  X86_64Assembler* assembler = codegen->GetAssembler();

  __ gs()->call(Address::Absolute(GetThreadOffset<kX86_64WordSize>(entry), true));
  codegen->RecordPcInfo(invoke, invoke->GetDexPc());
}

void IntrinsicLocationsBuilderX86_64::VisitMathCos(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathCos(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickCos);
}

void IntrinsicLocationsBuilderX86_64::VisitMathSin(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathSin(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickSin);
}

void IntrinsicLocationsBuilderX86_64::VisitMathAcos(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathAcos(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickAcos);
}

void IntrinsicLocationsBuilderX86_64::VisitMathAsin(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathAsin(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickAsin);
}

void IntrinsicLocationsBuilderX86_64::VisitMathAtan(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathAtan(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickAtan);
}

void IntrinsicLocationsBuilderX86_64::VisitMathCbrt(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathCbrt(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickCbrt);
}

void IntrinsicLocationsBuilderX86_64::VisitMathCosh(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathCosh(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickCosh);
}

void IntrinsicLocationsBuilderX86_64::VisitMathExp(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathExp(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickExp);
}

void IntrinsicLocationsBuilderX86_64::VisitMathExpm1(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathExpm1(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickExpm1);
}

void IntrinsicLocationsBuilderX86_64::VisitMathLog(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathLog(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickLog);
}

void IntrinsicLocationsBuilderX86_64::VisitMathLog10(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathLog10(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickLog10);
}

void IntrinsicLocationsBuilderX86_64::VisitMathSinh(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathSinh(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickSinh);
}

void IntrinsicLocationsBuilderX86_64::VisitMathTan(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathTan(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickTan);
}

void IntrinsicLocationsBuilderX86_64::VisitMathTanh(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathTanh(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickTanh);
}

static void CreateFPFPToFPCallLocations(ArenaAllocator* arena,
                                        HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kCall,
                                                           kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(0)));
  locations->SetInAt(1, Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(1)));
  locations->SetOut(Location::FpuRegisterLocation(XMM0));

  // We have to ensure that the native code doesn't clobber the XMM registers which are
  // non-volatile for ART, but volatile for Native calls.  This will ensure that they are
  // saved in the prologue and properly restored.
  for (auto fp_reg : non_volatile_xmm_regs) {
    locations->AddTemp(Location::FpuRegisterLocation(fp_reg));
  }
}

void IntrinsicLocationsBuilderX86_64::VisitMathAtan2(HInvoke* invoke) {
  CreateFPFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathAtan2(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickAtan2);
}

void IntrinsicLocationsBuilderX86_64::VisitMathHypot(HInvoke* invoke) {
  CreateFPFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathHypot(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickHypot);
}

void IntrinsicLocationsBuilderX86_64::VisitMathNextAfter(HInvoke* invoke) {
  CreateFPFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathNextAfter(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickNextAfter);
}

void IntrinsicLocationsBuilderX86_64::VisitStringCharAt(HInvoke* invoke) {
  // The inputs plus one temp.
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCallOnSlowPath,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
  locations->AddTemp(Location::RequiresRegister());
}

void IntrinsicCodeGeneratorX86_64::VisitStringCharAt(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();

  // Location of reference to data array.
  const int32_t value_offset = mirror::String::ValueOffset().Int32Value();
  // Location of count.
  const int32_t count_offset = mirror::String::CountOffset().Int32Value();

  CpuRegister obj = locations->InAt(0).AsRegister<CpuRegister>();
  CpuRegister idx = locations->InAt(1).AsRegister<CpuRegister>();
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();

  // TODO: Maybe we can support range check elimination. Overall, though, I think it's not worth
  //       the cost.
  // TODO: For simplicity, the index parameter is requested in a register, so different from Quick
  //       we will not optimize the code for constants (which would save a register).

  SlowPathCode* slow_path = new (GetAllocator()) IntrinsicSlowPathX86_64(invoke);
  codegen_->AddSlowPath(slow_path);

  X86_64Assembler* assembler = GetAssembler();

  __ cmpl(idx, Address(obj, count_offset));
  codegen_->MaybeRecordImplicitNullCheck(invoke);
  __ j(kAboveEqual, slow_path->GetEntryLabel());

  // out = out[2*idx].
  __ movzxw(out, Address(out, idx, ScaleFactor::TIMES_2, value_offset));

  __ Bind(slow_path->GetExitLabel());
}

void IntrinsicLocationsBuilderX86_64::VisitSystemArrayCopyChar(HInvoke* invoke) {
  // Check to see if we have known failures that will cause us to have to bail out
  // to the runtime, and just generate the runtime call directly.
  HIntConstant* src_pos = invoke->InputAt(1)->AsIntConstant();
  HIntConstant* dest_pos = invoke->InputAt(3)->AsIntConstant();

  // The positions must be non-negative.
  if ((src_pos != nullptr && src_pos->GetValue() < 0) ||
      (dest_pos != nullptr && dest_pos->GetValue() < 0)) {
    // We will have to fail anyways.
    return;
  }

  // The length must be > 0.
  HIntConstant* length = invoke->InputAt(4)->AsIntConstant();
  if (length != nullptr) {
    int32_t len = length->GetValue();
    if (len < 0) {
      // Just call as normal.
      return;
    }
  }

  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCallOnSlowPath,
                                                            kIntrinsified);
  // arraycopy(Object src, int src_pos, Object dest, int dest_pos, int length).
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(invoke->InputAt(1)));
  locations->SetInAt(2, Location::RequiresRegister());
  locations->SetInAt(3, Location::RegisterOrConstant(invoke->InputAt(3)));
  locations->SetInAt(4, Location::RegisterOrConstant(invoke->InputAt(4)));

  // And we need some temporaries.  We will use REP MOVSW, so we need fixed registers.
  locations->AddTemp(Location::RegisterLocation(RSI));
  locations->AddTemp(Location::RegisterLocation(RDI));
  locations->AddTemp(Location::RegisterLocation(RCX));
}

static void CheckPosition(X86_64Assembler* assembler,
                          Location pos,
                          CpuRegister input,
                          Location length,
                          SlowPathCode* slow_path,
                          CpuRegister input_len,
                          CpuRegister temp,
                          bool length_is_input_length = false) {
  // Where is the length in the Array?
  const uint32_t length_offset = mirror::Array::LengthOffset().Uint32Value();

  if (pos.IsConstant()) {
    int32_t pos_const = pos.GetConstant()->AsIntConstant()->GetValue();
    if (pos_const == 0) {
      if (!length_is_input_length) {
        // Check that length(input) >= length.
        if (length.IsConstant()) {
          __ cmpl(Address(input, length_offset),
                  Immediate(length.GetConstant()->AsIntConstant()->GetValue()));
        } else {
          __ cmpl(Address(input, length_offset), length.AsRegister<CpuRegister>());
        }
        __ j(kLess, slow_path->GetEntryLabel());
      }
    } else {
      // Check that length(input) >= pos.
      __ movl(input_len, Address(input, length_offset));
      __ cmpl(input_len, Immediate(pos_const));
      __ j(kLess, slow_path->GetEntryLabel());

      // Check that (length(input) - pos) >= length.
      __ leal(temp, Address(input_len, -pos_const));
      if (length.IsConstant()) {
        __ cmpl(temp, Immediate(length.GetConstant()->AsIntConstant()->GetValue()));
      } else {
        __ cmpl(temp, length.AsRegister<CpuRegister>());
      }
      __ j(kLess, slow_path->GetEntryLabel());
    }
  } else if (length_is_input_length) {
    // The only way the copy can succeed is if pos is zero.
    CpuRegister pos_reg = pos.AsRegister<CpuRegister>();
    __ testl(pos_reg, pos_reg);
    __ j(kNotEqual, slow_path->GetEntryLabel());
  } else {
    // Check that pos >= 0.
    CpuRegister pos_reg = pos.AsRegister<CpuRegister>();
    __ testl(pos_reg, pos_reg);
    __ j(kLess, slow_path->GetEntryLabel());

    // Check that pos <= length(input).
    __ cmpl(Address(input, length_offset), pos_reg);
    __ j(kLess, slow_path->GetEntryLabel());

    // Check that (length(input) - pos) >= length.
    __ movl(temp, Address(input, length_offset));
    __ subl(temp, pos_reg);
    if (length.IsConstant()) {
      __ cmpl(temp, Immediate(length.GetConstant()->AsIntConstant()->GetValue()));
    } else {
      __ cmpl(temp, length.AsRegister<CpuRegister>());
    }
    __ j(kLess, slow_path->GetEntryLabel());
  }
}

void IntrinsicCodeGeneratorX86_64::VisitSystemArrayCopyChar(HInvoke* invoke) {
  X86_64Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  CpuRegister src = locations->InAt(0).AsRegister<CpuRegister>();
  Location src_pos = locations->InAt(1);
  CpuRegister dest = locations->InAt(2).AsRegister<CpuRegister>();
  Location dest_pos = locations->InAt(3);
  Location length = locations->InAt(4);

  // Temporaries that we need for MOVSW.
  CpuRegister src_base = locations->GetTemp(0).AsRegister<CpuRegister>();
  DCHECK_EQ(src_base.AsRegister(), RSI);
  CpuRegister dest_base = locations->GetTemp(1).AsRegister<CpuRegister>();
  DCHECK_EQ(dest_base.AsRegister(), RDI);
  CpuRegister count = locations->GetTemp(2).AsRegister<CpuRegister>();
  DCHECK_EQ(count.AsRegister(), RCX);

  SlowPathCode* slow_path = new (GetAllocator()) IntrinsicSlowPathX86_64(invoke);
  codegen_->AddSlowPath(slow_path);

  // Bail out if the source and destination are the same.
  __ cmpl(src, dest);
  __ j(kEqual, slow_path->GetEntryLabel());

  // Bail out if the source is null.
  __ testl(src, src);
  __ j(kEqual, slow_path->GetEntryLabel());

  // Bail out if the destination is null.
  __ testl(dest, dest);
  __ j(kEqual, slow_path->GetEntryLabel());

  // If the length is negative, bail out.
  // We have already checked in the LocationsBuilder for the constant case.
  if (!length.IsConstant()) {
    __ testl(length.AsRegister<CpuRegister>(), length.AsRegister<CpuRegister>());
    __ j(kLess, slow_path->GetEntryLabel());
  }

  // Validity checks: source.
  CheckPosition(assembler, src_pos, src, length, slow_path, src_base, dest_base);

  // Validity checks: dest.
  CheckPosition(assembler, dest_pos, dest, length, slow_path, src_base, dest_base);

  // We need the count in RCX.
  if (length.IsConstant()) {
    __ movl(count, Immediate(length.GetConstant()->AsIntConstant()->GetValue()));
  } else {
    __ movl(count, length.AsRegister<CpuRegister>());
  }

  // Okay, everything checks out.  Finally time to do the copy.
  // Check assumption that sizeof(Char) is 2 (used in scaling below).
  const size_t char_size = Primitive::ComponentSize(Primitive::kPrimChar);
  DCHECK_EQ(char_size, 2u);

  const uint32_t data_offset = mirror::Array::DataOffset(char_size).Uint32Value();

  if (src_pos.IsConstant()) {
    int32_t src_pos_const = src_pos.GetConstant()->AsIntConstant()->GetValue();
    __ leal(src_base, Address(src, char_size * src_pos_const + data_offset));
  } else {
    __ leal(src_base, Address(src, src_pos.AsRegister<CpuRegister>(),
                              ScaleFactor::TIMES_2, data_offset));
  }
  if (dest_pos.IsConstant()) {
    int32_t dest_pos_const = dest_pos.GetConstant()->AsIntConstant()->GetValue();
    __ leal(dest_base, Address(dest, char_size * dest_pos_const + data_offset));
  } else {
    __ leal(dest_base, Address(dest, dest_pos.AsRegister<CpuRegister>(),
                               ScaleFactor::TIMES_2, data_offset));
  }

  // Do the move.
  __ rep_movsw();

  __ Bind(slow_path->GetExitLabel());
}


void IntrinsicLocationsBuilderX86_64::VisitSystemArrayCopy(HInvoke* invoke) {
  CodeGenerator::CreateSystemArrayCopyLocationSummary(invoke);
}

// TODO: Implement read barriers in the SystemArrayCopy intrinsic.
// Note that this code path is not used (yet) because we do not
// intrinsify methods that can go into the IntrinsicSlowPathX86_64
// slow path.
void IntrinsicCodeGeneratorX86_64::VisitSystemArrayCopy(HInvoke* invoke) {
  X86_64Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  uint32_t super_offset = mirror::Class::SuperClassOffset().Int32Value();
  uint32_t component_offset = mirror::Class::ComponentTypeOffset().Int32Value();
  uint32_t primitive_offset = mirror::Class::PrimitiveTypeOffset().Int32Value();

  CpuRegister src = locations->InAt(0).AsRegister<CpuRegister>();
  Location src_pos = locations->InAt(1);
  CpuRegister dest = locations->InAt(2).AsRegister<CpuRegister>();
  Location dest_pos = locations->InAt(3);
  Location length = locations->InAt(4);
  CpuRegister temp1 = locations->GetTemp(0).AsRegister<CpuRegister>();
  CpuRegister temp2 = locations->GetTemp(1).AsRegister<CpuRegister>();
  CpuRegister temp3 = locations->GetTemp(2).AsRegister<CpuRegister>();

  SlowPathCode* slow_path = new (GetAllocator()) IntrinsicSlowPathX86_64(invoke);
  codegen_->AddSlowPath(slow_path);

  NearLabel conditions_on_positions_validated;
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
        __ cmpl(src, dest);
        __ j(kEqual, slow_path->GetEntryLabel());
      }
    } else {
      if (!optimizations.GetDestinationIsSource()) {
        __ cmpl(src, dest);
        __ j(kNotEqual, &conditions_on_positions_validated);
      }
      __ cmpl(dest_pos.AsRegister<CpuRegister>(), Immediate(src_pos_constant));
      __ j(kGreater, slow_path->GetEntryLabel());
    }
  } else {
    if (!optimizations.GetDestinationIsSource()) {
      __ cmpl(src, dest);
      __ j(kNotEqual, &conditions_on_positions_validated);
    }
    if (dest_pos.IsConstant()) {
      int32_t dest_pos_constant = dest_pos.GetConstant()->AsIntConstant()->GetValue();
      __ cmpl(src_pos.AsRegister<CpuRegister>(), Immediate(dest_pos_constant));
      __ j(kLess, slow_path->GetEntryLabel());
    } else {
      __ cmpl(src_pos.AsRegister<CpuRegister>(), dest_pos.AsRegister<CpuRegister>());
      __ j(kLess, slow_path->GetEntryLabel());
    }
  }

  __ Bind(&conditions_on_positions_validated);

  if (!optimizations.GetSourceIsNotNull()) {
    // Bail out if the source is null.
    __ testl(src, src);
    __ j(kEqual, slow_path->GetEntryLabel());
  }

  if (!optimizations.GetDestinationIsNotNull() && !optimizations.GetDestinationIsSource()) {
    // Bail out if the destination is null.
    __ testl(dest, dest);
    __ j(kEqual, slow_path->GetEntryLabel());
  }

  // If the length is negative, bail out.
  // We have already checked in the LocationsBuilder for the constant case.
  if (!length.IsConstant() &&
      !optimizations.GetCountIsSourceLength() &&
      !optimizations.GetCountIsDestinationLength()) {
    __ testl(length.AsRegister<CpuRegister>(), length.AsRegister<CpuRegister>());
    __ j(kLess, slow_path->GetEntryLabel());
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
    __ movl(temp1, Address(dest, class_offset));
    __ movl(temp2, Address(src, class_offset));
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
      // /* HeapReference<Class> */ TMP = temp1->component_type_
      __ movl(CpuRegister(TMP), Address(temp1, component_offset));
      __ testl(CpuRegister(TMP), CpuRegister(TMP));
      __ j(kEqual, slow_path->GetEntryLabel());
      __ MaybeUnpoisonHeapReference(CpuRegister(TMP));
      __ cmpw(Address(CpuRegister(TMP), primitive_offset), Immediate(Primitive::kPrimNot));
      __ j(kNotEqual, slow_path->GetEntryLabel());
    }

    if (!optimizations.GetSourceIsNonPrimitiveArray()) {
      // Bail out if the source is not a non primitive array.
      // /* HeapReference<Class> */ TMP = temp2->component_type_
      __ movl(CpuRegister(TMP), Address(temp2, component_offset));
      __ testl(CpuRegister(TMP), CpuRegister(TMP));
      __ j(kEqual, slow_path->GetEntryLabel());
      __ MaybeUnpoisonHeapReference(CpuRegister(TMP));
      __ cmpw(Address(CpuRegister(TMP), primitive_offset), Immediate(Primitive::kPrimNot));
      __ j(kNotEqual, slow_path->GetEntryLabel());
    }

    __ cmpl(temp1, temp2);

    if (optimizations.GetDestinationIsTypedObjectArray()) {
      NearLabel do_copy;
      __ j(kEqual, &do_copy);
      if (!did_unpoison) {
        __ MaybeUnpoisonHeapReference(temp1);
      }
      // /* HeapReference<Class> */ temp1 = temp1->component_type_
      __ movl(temp1, Address(temp1, component_offset));
      __ MaybeUnpoisonHeapReference(temp1);
      // /* HeapReference<Class> */ temp1 = temp1->super_class_
      __ movl(temp1, Address(temp1, super_offset));
      // No need to unpoison the result, we're comparing against null.
      __ testl(temp1, temp1);
      __ j(kNotEqual, slow_path->GetEntryLabel());
      __ Bind(&do_copy);
    } else {
      __ j(kNotEqual, slow_path->GetEntryLabel());
    }
  } else if (!optimizations.GetSourceIsNonPrimitiveArray()) {
    DCHECK(optimizations.GetDestinationIsNonPrimitiveArray());
    // Bail out if the source is not a non primitive array.
    // /* HeapReference<Class> */ temp1 = src->klass_
    __ movl(temp1, Address(src, class_offset));
    __ MaybeUnpoisonHeapReference(temp1);
    // /* HeapReference<Class> */ TMP = temp1->component_type_
    __ movl(CpuRegister(TMP), Address(temp1, component_offset));
    __ testl(CpuRegister(TMP), CpuRegister(TMP));
    __ j(kEqual, slow_path->GetEntryLabel());
    __ MaybeUnpoisonHeapReference(CpuRegister(TMP));
    __ cmpw(Address(CpuRegister(TMP), primitive_offset), Immediate(Primitive::kPrimNot));
    __ j(kNotEqual, slow_path->GetEntryLabel());
  }

  // Compute base source address, base destination address, and end source address.

  uint32_t element_size = sizeof(int32_t);
  uint32_t offset = mirror::Array::DataOffset(element_size).Uint32Value();
  if (src_pos.IsConstant()) {
    int32_t constant = src_pos.GetConstant()->AsIntConstant()->GetValue();
    __ leal(temp1, Address(src, element_size * constant + offset));
  } else {
    __ leal(temp1, Address(src, src_pos.AsRegister<CpuRegister>(), ScaleFactor::TIMES_4, offset));
  }

  if (dest_pos.IsConstant()) {
    int32_t constant = dest_pos.GetConstant()->AsIntConstant()->GetValue();
    __ leal(temp2, Address(dest, element_size * constant + offset));
  } else {
    __ leal(temp2, Address(dest, dest_pos.AsRegister<CpuRegister>(), ScaleFactor::TIMES_4, offset));
  }

  if (length.IsConstant()) {
    int32_t constant = length.GetConstant()->AsIntConstant()->GetValue();
    __ leal(temp3, Address(temp1, element_size * constant));
  } else {
    __ leal(temp3, Address(temp1, length.AsRegister<CpuRegister>(), ScaleFactor::TIMES_4, 0));
  }

  // Iterate over the arrays and do a raw copy of the objects. We don't need to
  // poison/unpoison, nor do any read barrier as the next uses of the destination
  // array will do it.
  NearLabel loop, done;
  __ cmpl(temp1, temp3);
  __ j(kEqual, &done);
  __ Bind(&loop);
  __ movl(CpuRegister(TMP), Address(temp1, 0));
  __ movl(Address(temp2, 0), CpuRegister(TMP));
  __ addl(temp1, Immediate(element_size));
  __ addl(temp2, Immediate(element_size));
  __ cmpl(temp1, temp3);
  __ j(kNotEqual, &loop);
  __ Bind(&done);

  // We only need one card marking on the destination array.
  codegen_->MarkGCCard(temp1,
                       temp2,
                       dest,
                       CpuRegister(kNoRegister),
                       /* value_can_be_null */ false);

  __ Bind(slow_path->GetExitLabel());
}

void IntrinsicLocationsBuilderX86_64::VisitStringCompareTo(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetOut(Location::RegisterLocation(RAX));
}

void IntrinsicCodeGeneratorX86_64::VisitStringCompareTo(HInvoke* invoke) {
  X86_64Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  CpuRegister argument = locations->InAt(1).AsRegister<CpuRegister>();
  __ testl(argument, argument);
  SlowPathCode* slow_path = new (GetAllocator()) IntrinsicSlowPathX86_64(invoke);
  codegen_->AddSlowPath(slow_path);
  __ j(kEqual, slow_path->GetEntryLabel());

  __ gs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pStringCompareTo),
                                  /* no_rip */ true));
  __ Bind(slow_path->GetExitLabel());
}

void IntrinsicLocationsBuilderX86_64::VisitStringEquals(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());

  // Request temporary registers, RCX and RDI needed for repe_cmpsq instruction.
  locations->AddTemp(Location::RegisterLocation(RCX));
  locations->AddTemp(Location::RegisterLocation(RDI));

  // Set output, RSI needed for repe_cmpsq instruction anyways.
  locations->SetOut(Location::RegisterLocation(RSI), Location::kOutputOverlap);
}

void IntrinsicCodeGeneratorX86_64::VisitStringEquals(HInvoke* invoke) {
  X86_64Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  CpuRegister str = locations->InAt(0).AsRegister<CpuRegister>();
  CpuRegister arg = locations->InAt(1).AsRegister<CpuRegister>();
  CpuRegister rcx = locations->GetTemp(0).AsRegister<CpuRegister>();
  CpuRegister rdi = locations->GetTemp(1).AsRegister<CpuRegister>();
  CpuRegister rsi = locations->Out().AsRegister<CpuRegister>();

  NearLabel end, return_true, return_false;

  // Get offsets of count, value, and class fields within a string object.
  const uint32_t count_offset = mirror::String::CountOffset().Uint32Value();
  const uint32_t value_offset = mirror::String::ValueOffset().Uint32Value();
  const uint32_t class_offset = mirror::Object::ClassOffset().Uint32Value();

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  // Check if input is null, return false if it is.
  __ testl(arg, arg);
  __ j(kEqual, &return_false);

  // Instanceof check for the argument by comparing class fields.
  // All string objects must have the same type since String cannot be subclassed.
  // Receiver must be a string object, so its class field is equal to all strings' class fields.
  // If the argument is a string object, its class field must be equal to receiver's class field.
  __ movl(rcx, Address(str, class_offset));
  __ cmpl(rcx, Address(arg, class_offset));
  __ j(kNotEqual, &return_false);

  // Reference equality check, return true if same reference.
  __ cmpl(str, arg);
  __ j(kEqual, &return_true);

  // Load length of receiver string.
  __ movl(rcx, Address(str, count_offset));
  // Check if lengths are equal, return false if they're not.
  __ cmpl(rcx, Address(arg, count_offset));
  __ j(kNotEqual, &return_false);
  // Return true if both strings are empty.
  __ jrcxz(&return_true);

  // Load starting addresses of string values into RSI/RDI as required for repe_cmpsq instruction.
  __ leal(rsi, Address(str, value_offset));
  __ leal(rdi, Address(arg, value_offset));

  // Divide string length by 4 and adjust for lengths not divisible by 4.
  __ addl(rcx, Immediate(3));
  __ shrl(rcx, Immediate(2));

  // Assertions that must hold in order to compare strings 4 characters at a time.
  DCHECK_ALIGNED(value_offset, 8);
  static_assert(IsAligned<8>(kObjectAlignment), "String is not zero padded");

  // Loop to compare strings four characters at a time starting at the beginning of the string.
  __ repe_cmpsq();
  // If strings are not equal, zero flag will be cleared.
  __ j(kNotEqual, &return_false);

  // Return true and exit the function.
  // If loop does not result in returning false, we return true.
  __ Bind(&return_true);
  __ movl(rsi, Immediate(1));
  __ jmp(&end);

  // Return false and exit the function.
  __ Bind(&return_false);
  __ xorl(rsi, rsi);
  __ Bind(&end);
}

static void CreateStringIndexOfLocations(HInvoke* invoke,
                                         ArenaAllocator* allocator,
                                         bool start_at_zero) {
  LocationSummary* locations = new (allocator) LocationSummary(invoke,
                                                               LocationSummary::kCallOnSlowPath,
                                                               kIntrinsified);
  // The data needs to be in RDI for scasw. So request that the string is there, anyways.
  locations->SetInAt(0, Location::RegisterLocation(RDI));
  // If we look for a constant char, we'll still have to copy it into RAX. So just request the
  // allocator to do that, anyways. We can still do the constant check by checking the parameter
  // of the instruction explicitly.
  // Note: This works as we don't clobber RAX anywhere.
  locations->SetInAt(1, Location::RegisterLocation(RAX));
  if (!start_at_zero) {
    locations->SetInAt(2, Location::RequiresRegister());          // The starting index.
  }
  // As we clobber RDI during execution anyways, also use it as the output.
  locations->SetOut(Location::SameAsFirstInput());

  // repne scasw uses RCX as the counter.
  locations->AddTemp(Location::RegisterLocation(RCX));
  // Need another temporary to be able to compute the result.
  locations->AddTemp(Location::RequiresRegister());
}

static void GenerateStringIndexOf(HInvoke* invoke,
                                  X86_64Assembler* assembler,
                                  CodeGeneratorX86_64* codegen,
                                  ArenaAllocator* allocator,
                                  bool start_at_zero) {
  LocationSummary* locations = invoke->GetLocations();

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  CpuRegister string_obj = locations->InAt(0).AsRegister<CpuRegister>();
  CpuRegister search_value = locations->InAt(1).AsRegister<CpuRegister>();
  CpuRegister counter = locations->GetTemp(0).AsRegister<CpuRegister>();
  CpuRegister string_length = locations->GetTemp(1).AsRegister<CpuRegister>();
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();

  // Check our assumptions for registers.
  DCHECK_EQ(string_obj.AsRegister(), RDI);
  DCHECK_EQ(search_value.AsRegister(), RAX);
  DCHECK_EQ(counter.AsRegister(), RCX);
  DCHECK_EQ(out.AsRegister(), RDI);

  // Check for code points > 0xFFFF. Either a slow-path check when we don't know statically,
  // or directly dispatch if we have a constant.
  SlowPathCode* slow_path = nullptr;
  if (invoke->InputAt(1)->IsIntConstant()) {
    if (static_cast<uint32_t>(invoke->InputAt(1)->AsIntConstant()->GetValue()) >
    std::numeric_limits<uint16_t>::max()) {
      // Always needs the slow-path. We could directly dispatch to it, but this case should be
      // rare, so for simplicity just put the full slow-path down and branch unconditionally.
      slow_path = new (allocator) IntrinsicSlowPathX86_64(invoke);
      codegen->AddSlowPath(slow_path);
      __ jmp(slow_path->GetEntryLabel());
      __ Bind(slow_path->GetExitLabel());
      return;
    }
  } else {
    __ cmpl(search_value, Immediate(std::numeric_limits<uint16_t>::max()));
    slow_path = new (allocator) IntrinsicSlowPathX86_64(invoke);
    codegen->AddSlowPath(slow_path);
    __ j(kAbove, slow_path->GetEntryLabel());
  }

  // From here down, we know that we are looking for a char that fits in 16 bits.
  // Location of reference to data array within the String object.
  int32_t value_offset = mirror::String::ValueOffset().Int32Value();
  // Location of count within the String object.
  int32_t count_offset = mirror::String::CountOffset().Int32Value();

  // Load string length, i.e., the count field of the string.
  __ movl(string_length, Address(string_obj, count_offset));

  // Do a length check.
  // TODO: Support jecxz.
  NearLabel not_found_label;
  __ testl(string_length, string_length);
  __ j(kEqual, &not_found_label);

  if (start_at_zero) {
    // Number of chars to scan is the same as the string length.
    __ movl(counter, string_length);

    // Move to the start of the string.
    __ addq(string_obj, Immediate(value_offset));
  } else {
    CpuRegister start_index = locations->InAt(2).AsRegister<CpuRegister>();

    // Do a start_index check.
    __ cmpl(start_index, string_length);
    __ j(kGreaterEqual, &not_found_label);

    // Ensure we have a start index >= 0;
    __ xorl(counter, counter);
    __ cmpl(start_index, Immediate(0));
    __ cmov(kGreater, counter, start_index, /* is64bit */ false);  // 32-bit copy is enough.

    // Move to the start of the string: string_obj + value_offset + 2 * start_index.
    __ leaq(string_obj, Address(string_obj, counter, ScaleFactor::TIMES_2, value_offset));

    // Now update ecx, the work counter: it's gonna be string.length - start_index.
    __ negq(counter);  // Needs to be 64-bit negation, as the address computation is 64-bit.
    __ leaq(counter, Address(string_length, counter, ScaleFactor::TIMES_1, 0));
  }

  // Everything is set up for repne scasw:
  //   * Comparison address in RDI.
  //   * Counter in ECX.
  __ repne_scasw();

  // Did we find a match?
  __ j(kNotEqual, &not_found_label);

  // Yes, we matched.  Compute the index of the result.
  __ subl(string_length, counter);
  __ leal(out, Address(string_length, -1));

  NearLabel done;
  __ jmp(&done);

  // Failed to match; return -1.
  __ Bind(&not_found_label);
  __ movl(out, Immediate(-1));

  // And join up at the end.
  __ Bind(&done);
  if (slow_path != nullptr) {
    __ Bind(slow_path->GetExitLabel());
  }
}

void IntrinsicLocationsBuilderX86_64::VisitStringIndexOf(HInvoke* invoke) {
  CreateStringIndexOfLocations(invoke, arena_, /* start_at_zero */ true);
}

void IntrinsicCodeGeneratorX86_64::VisitStringIndexOf(HInvoke* invoke) {
  GenerateStringIndexOf(invoke, GetAssembler(), codegen_, GetAllocator(), /* start_at_zero */ true);
}

void IntrinsicLocationsBuilderX86_64::VisitStringIndexOfAfter(HInvoke* invoke) {
  CreateStringIndexOfLocations(invoke, arena_, /* start_at_zero */ false);
}

void IntrinsicCodeGeneratorX86_64::VisitStringIndexOfAfter(HInvoke* invoke) {
  GenerateStringIndexOf(
      invoke, GetAssembler(), codegen_, GetAllocator(), /* start_at_zero */ false);
}

void IntrinsicLocationsBuilderX86_64::VisitStringNewStringFromBytes(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(2, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  locations->SetInAt(3, Location::RegisterLocation(calling_convention.GetRegisterAt(3)));
  locations->SetOut(Location::RegisterLocation(RAX));
}

void IntrinsicCodeGeneratorX86_64::VisitStringNewStringFromBytes(HInvoke* invoke) {
  X86_64Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  CpuRegister byte_array = locations->InAt(0).AsRegister<CpuRegister>();
  __ testl(byte_array, byte_array);
  SlowPathCode* slow_path = new (GetAllocator()) IntrinsicSlowPathX86_64(invoke);
  codegen_->AddSlowPath(slow_path);
  __ j(kEqual, slow_path->GetEntryLabel());

  __ gs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pAllocStringFromBytes),
                                  /* no_rip */ true));
  CheckEntrypointTypes<kQuickAllocStringFromBytes, void*, void*, int32_t, int32_t, int32_t>();
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
  __ Bind(slow_path->GetExitLabel());
}

void IntrinsicLocationsBuilderX86_64::VisitStringNewStringFromChars(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(2, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  locations->SetOut(Location::RegisterLocation(RAX));
}

void IntrinsicCodeGeneratorX86_64::VisitStringNewStringFromChars(HInvoke* invoke) {
  X86_64Assembler* assembler = GetAssembler();

  // No need to emit code checking whether `locations->InAt(2)` is a null
  // pointer, as callers of the native method
  //
  //   java.lang.StringFactory.newStringFromChars(int offset, int charCount, char[] data)
  //
  // all include a null check on `data` before calling that method.
  __ gs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pAllocStringFromChars),
                                  /* no_rip */ true));
  CheckEntrypointTypes<kQuickAllocStringFromChars, void*, int32_t, int32_t, void*>();
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void IntrinsicLocationsBuilderX86_64::VisitStringNewStringFromString(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetOut(Location::RegisterLocation(RAX));
}

void IntrinsicCodeGeneratorX86_64::VisitStringNewStringFromString(HInvoke* invoke) {
  X86_64Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  CpuRegister string_to_copy = locations->InAt(0).AsRegister<CpuRegister>();
  __ testl(string_to_copy, string_to_copy);
  SlowPathCode* slow_path = new (GetAllocator()) IntrinsicSlowPathX86_64(invoke);
  codegen_->AddSlowPath(slow_path);
  __ j(kEqual, slow_path->GetEntryLabel());

  __ gs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pAllocStringFromString),
                                  /* no_rip */ true));
  CheckEntrypointTypes<kQuickAllocStringFromString, void*, void*>();
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
  __ Bind(slow_path->GetExitLabel());
}

void IntrinsicLocationsBuilderX86_64::VisitStringGetCharsNoCheck(HInvoke* invoke) {
  // public void getChars(int srcBegin, int srcEnd, char[] dst, int dstBegin);
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(invoke->InputAt(1)));
  locations->SetInAt(2, Location::RequiresRegister());
  locations->SetInAt(3, Location::RequiresRegister());
  locations->SetInAt(4, Location::RequiresRegister());

  // And we need some temporaries.  We will use REP MOVSW, so we need fixed registers.
  locations->AddTemp(Location::RegisterLocation(RSI));
  locations->AddTemp(Location::RegisterLocation(RDI));
  locations->AddTemp(Location::RegisterLocation(RCX));
}

void IntrinsicCodeGeneratorX86_64::VisitStringGetCharsNoCheck(HInvoke* invoke) {
  X86_64Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  size_t char_component_size = Primitive::ComponentSize(Primitive::kPrimChar);
  // Location of data in char array buffer.
  const uint32_t data_offset = mirror::Array::DataOffset(char_component_size).Uint32Value();
  // Location of char array data in string.
  const uint32_t value_offset = mirror::String::ValueOffset().Uint32Value();

  // public void getChars(int srcBegin, int srcEnd, char[] dst, int dstBegin);
  CpuRegister obj = locations->InAt(0).AsRegister<CpuRegister>();
  Location srcBegin = locations->InAt(1);
  int srcBegin_value =
    srcBegin.IsConstant() ? srcBegin.GetConstant()->AsIntConstant()->GetValue() : 0;
  CpuRegister srcEnd = locations->InAt(2).AsRegister<CpuRegister>();
  CpuRegister dst = locations->InAt(3).AsRegister<CpuRegister>();
  CpuRegister dstBegin = locations->InAt(4).AsRegister<CpuRegister>();

  // Check assumption that sizeof(Char) is 2 (used in scaling below).
  const size_t char_size = Primitive::ComponentSize(Primitive::kPrimChar);
  DCHECK_EQ(char_size, 2u);

  // Compute the address of the destination buffer.
  __ leaq(CpuRegister(RDI), Address(dst, dstBegin, ScaleFactor::TIMES_2, data_offset));

  // Compute the address of the source string.
  if (srcBegin.IsConstant()) {
    // Compute the address of the source string by adding the number of chars from
    // the source beginning to the value offset of a string.
    __ leaq(CpuRegister(RSI), Address(obj, srcBegin_value * char_size + value_offset));
  } else {
    __ leaq(CpuRegister(RSI), Address(obj, srcBegin.AsRegister<CpuRegister>(),
                                      ScaleFactor::TIMES_2, value_offset));
  }

  // Compute the number of chars (words) to move.
  __ movl(CpuRegister(RCX), srcEnd);
  if (srcBegin.IsConstant()) {
    if (srcBegin_value != 0) {
      __ subl(CpuRegister(RCX), Immediate(srcBegin_value));
    }
  } else {
    DCHECK(srcBegin.IsRegister());
    __ subl(CpuRegister(RCX), srcBegin.AsRegister<CpuRegister>());
  }

  // Do the move.
  __ rep_movsw();
}

static void GenPeek(LocationSummary* locations, Primitive::Type size, X86_64Assembler* assembler) {
  CpuRegister address = locations->InAt(0).AsRegister<CpuRegister>();
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();  // == address, here for clarity.
  // x86 allows unaligned access. We do not have to check the input or use specific instructions
  // to avoid a SIGBUS.
  switch (size) {
    case Primitive::kPrimByte:
      __ movsxb(out, Address(address, 0));
      break;
    case Primitive::kPrimShort:
      __ movsxw(out, Address(address, 0));
      break;
    case Primitive::kPrimInt:
      __ movl(out, Address(address, 0));
      break;
    case Primitive::kPrimLong:
      __ movq(out, Address(address, 0));
      break;
    default:
      LOG(FATAL) << "Type not recognized for peek: " << size;
      UNREACHABLE();
  }
}

void IntrinsicLocationsBuilderX86_64::VisitMemoryPeekByte(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMemoryPeekByte(HInvoke* invoke) {
  GenPeek(invoke->GetLocations(), Primitive::kPrimByte, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitMemoryPeekIntNative(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMemoryPeekIntNative(HInvoke* invoke) {
  GenPeek(invoke->GetLocations(), Primitive::kPrimInt, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitMemoryPeekLongNative(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMemoryPeekLongNative(HInvoke* invoke) {
  GenPeek(invoke->GetLocations(), Primitive::kPrimLong, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitMemoryPeekShortNative(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMemoryPeekShortNative(HInvoke* invoke) {
  GenPeek(invoke->GetLocations(), Primitive::kPrimShort, GetAssembler());
}

static void CreateIntIntToVoidLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrInt32Constant(invoke->InputAt(1)));
}

static void GenPoke(LocationSummary* locations, Primitive::Type size, X86_64Assembler* assembler) {
  CpuRegister address = locations->InAt(0).AsRegister<CpuRegister>();
  Location value = locations->InAt(1);
  // x86 allows unaligned access. We do not have to check the input or use specific instructions
  // to avoid a SIGBUS.
  switch (size) {
    case Primitive::kPrimByte:
      if (value.IsConstant()) {
        __ movb(Address(address, 0),
                Immediate(CodeGenerator::GetInt32ValueOf(value.GetConstant())));
      } else {
        __ movb(Address(address, 0), value.AsRegister<CpuRegister>());
      }
      break;
    case Primitive::kPrimShort:
      if (value.IsConstant()) {
        __ movw(Address(address, 0),
                Immediate(CodeGenerator::GetInt32ValueOf(value.GetConstant())));
      } else {
        __ movw(Address(address, 0), value.AsRegister<CpuRegister>());
      }
      break;
    case Primitive::kPrimInt:
      if (value.IsConstant()) {
        __ movl(Address(address, 0),
                Immediate(CodeGenerator::GetInt32ValueOf(value.GetConstant())));
      } else {
        __ movl(Address(address, 0), value.AsRegister<CpuRegister>());
      }
      break;
    case Primitive::kPrimLong:
      if (value.IsConstant()) {
        int64_t v = value.GetConstant()->AsLongConstant()->GetValue();
        DCHECK(IsInt<32>(v));
        int32_t v_32 = v;
        __ movq(Address(address, 0), Immediate(v_32));
      } else {
        __ movq(Address(address, 0), value.AsRegister<CpuRegister>());
      }
      break;
    default:
      LOG(FATAL) << "Type not recognized for poke: " << size;
      UNREACHABLE();
  }
}

void IntrinsicLocationsBuilderX86_64::VisitMemoryPokeByte(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMemoryPokeByte(HInvoke* invoke) {
  GenPoke(invoke->GetLocations(), Primitive::kPrimByte, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitMemoryPokeIntNative(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMemoryPokeIntNative(HInvoke* invoke) {
  GenPoke(invoke->GetLocations(), Primitive::kPrimInt, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitMemoryPokeLongNative(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMemoryPokeLongNative(HInvoke* invoke) {
  GenPoke(invoke->GetLocations(), Primitive::kPrimLong, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitMemoryPokeShortNative(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMemoryPokeShortNative(HInvoke* invoke) {
  GenPoke(invoke->GetLocations(), Primitive::kPrimShort, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitThreadCurrentThread(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetOut(Location::RequiresRegister());
}

void IntrinsicCodeGeneratorX86_64::VisitThreadCurrentThread(HInvoke* invoke) {
  CpuRegister out = invoke->GetLocations()->Out().AsRegister<CpuRegister>();
  GetAssembler()->gs()->movl(out, Address::Absolute(Thread::PeerOffset<kX86_64WordSize>(),
                                                    /* no_rip */ true));
}

static void GenUnsafeGet(HInvoke* invoke,
                         Primitive::Type type,
                         bool is_volatile ATTRIBUTE_UNUSED,
                         CodeGeneratorX86_64* codegen) {
  X86_64Assembler* assembler = down_cast<X86_64Assembler*>(codegen->GetAssembler());
  LocationSummary* locations = invoke->GetLocations();
  Location base_loc = locations->InAt(1);
  CpuRegister base = base_loc.AsRegister<CpuRegister>();
  Location offset_loc = locations->InAt(2);
  CpuRegister offset = offset_loc.AsRegister<CpuRegister>();
  Location output_loc = locations->Out();
  CpuRegister output = output_loc.AsRegister<CpuRegister>();

  switch (type) {
    case Primitive::kPrimInt:
      __ movl(output, Address(base, offset, ScaleFactor::TIMES_1, 0));
      break;

    case Primitive::kPrimNot: {
      if (kEmitCompilerReadBarrier) {
        if (kUseBakerReadBarrier) {
          Location temp = locations->GetTemp(0);
          codegen->GenerateArrayLoadWithBakerReadBarrier(
              invoke, output_loc, base, 0U, offset_loc, temp, /* needs_null_check */ false);
        } else {
          __ movl(output, Address(base, offset, ScaleFactor::TIMES_1, 0));
          codegen->GenerateReadBarrierSlow(
              invoke, output_loc, output_loc, base_loc, 0U, offset_loc);
        }
      } else {
        __ movl(output, Address(base, offset, ScaleFactor::TIMES_1, 0));
        __ MaybeUnpoisonHeapReference(output);
      }
      break;
    }

    case Primitive::kPrimLong:
      __ movq(output, Address(base, offset, ScaleFactor::TIMES_1, 0));
      break;

    default:
      LOG(FATAL) << "Unsupported op size " << type;
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
  locations->SetOut(Location::RequiresRegister());
  if (type == Primitive::kPrimNot && kEmitCompilerReadBarrier && kUseBakerReadBarrier) {
    // We need a temporary register for the read barrier marking slow
    // path in InstructionCodeGeneratorX86_64::GenerateArrayLoadWithBakerReadBarrier.
    locations->AddTemp(Location::RequiresRegister());
  }
}

void IntrinsicLocationsBuilderX86_64::VisitUnsafeGet(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, Primitive::kPrimInt);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafeGetVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, Primitive::kPrimInt);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafeGetLong(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, Primitive::kPrimLong);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafeGetLongVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, Primitive::kPrimLong);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafeGetObject(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, Primitive::kPrimNot);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafeGetObjectVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, Primitive::kPrimNot);
}


void IntrinsicCodeGeneratorX86_64::VisitUnsafeGet(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimInt, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafeGetVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimInt, /* is_volatile */ true, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafeGetLong(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimLong, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafeGetLongVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimLong, /* is_volatile */ true, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafeGetObject(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimNot, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafeGetObjectVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimNot, /* is_volatile */ true, codegen_);
}


static void CreateIntIntIntIntToVoidPlusTempsLocations(ArenaAllocator* arena,
                                                       Primitive::Type type,
                                                       HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::NoLocation());        // Unused receiver.
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetInAt(2, Location::RequiresRegister());
  locations->SetInAt(3, Location::RequiresRegister());
  if (type == Primitive::kPrimNot) {
    // Need temp registers for card-marking.
    locations->AddTemp(Location::RequiresRegister());  // Possibly used for reference poisoning too.
    locations->AddTemp(Location::RequiresRegister());
  }
}

void IntrinsicLocationsBuilderX86_64::VisitUnsafePut(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimInt, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafePutOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimInt, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafePutVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimInt, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafePutObject(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimNot, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafePutObjectOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimNot, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafePutObjectVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimNot, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafePutLong(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimLong, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafePutLongOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimLong, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafePutLongVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimLong, invoke);
}

// We don't care for ordered: it requires an AnyStore barrier, which is already given by the x86
// memory model.
static void GenUnsafePut(LocationSummary* locations, Primitive::Type type, bool is_volatile,
                         CodeGeneratorX86_64* codegen) {
  X86_64Assembler* assembler = down_cast<X86_64Assembler*>(codegen->GetAssembler());
  CpuRegister base = locations->InAt(1).AsRegister<CpuRegister>();
  CpuRegister offset = locations->InAt(2).AsRegister<CpuRegister>();
  CpuRegister value = locations->InAt(3).AsRegister<CpuRegister>();

  if (type == Primitive::kPrimLong) {
    __ movq(Address(base, offset, ScaleFactor::TIMES_1, 0), value);
  } else if (kPoisonHeapReferences && type == Primitive::kPrimNot) {
    CpuRegister temp = locations->GetTemp(0).AsRegister<CpuRegister>();
    __ movl(temp, value);
    __ PoisonHeapReference(temp);
    __ movl(Address(base, offset, ScaleFactor::TIMES_1, 0), temp);
  } else {
    __ movl(Address(base, offset, ScaleFactor::TIMES_1, 0), value);
  }

  if (is_volatile) {
    codegen->MemoryFence();
  }

  if (type == Primitive::kPrimNot) {
    bool value_can_be_null = true;  // TODO: Worth finding out this information?
    codegen->MarkGCCard(locations->GetTemp(0).AsRegister<CpuRegister>(),
                        locations->GetTemp(1).AsRegister<CpuRegister>(),
                        base,
                        value,
                        value_can_be_null);
  }
}

void IntrinsicCodeGeneratorX86_64::VisitUnsafePut(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimInt, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafePutOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimInt, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafePutVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimInt, /* is_volatile */ true, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafePutObject(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimNot, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafePutObjectOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimNot, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafePutObjectVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimNot, /* is_volatile */ true, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafePutLong(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimLong, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafePutLongOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimLong, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafePutLongVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimLong, /* is_volatile */ true, codegen_);
}

static void CreateIntIntIntIntIntToInt(ArenaAllocator* arena, Primitive::Type type,
                                       HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::NoLocation());        // Unused receiver.
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetInAt(2, Location::RequiresRegister());
  // expected value must be in EAX/RAX.
  locations->SetInAt(3, Location::RegisterLocation(RAX));
  locations->SetInAt(4, Location::RequiresRegister());

  locations->SetOut(Location::RequiresRegister());
  if (type == Primitive::kPrimNot) {
    // Need temp registers for card-marking.
    locations->AddTemp(Location::RequiresRegister());  // Possibly used for reference poisoning too.
    locations->AddTemp(Location::RequiresRegister());
  }
}

void IntrinsicLocationsBuilderX86_64::VisitUnsafeCASInt(HInvoke* invoke) {
  CreateIntIntIntIntIntToInt(arena_, Primitive::kPrimInt, invoke);
}

void IntrinsicLocationsBuilderX86_64::VisitUnsafeCASLong(HInvoke* invoke) {
  CreateIntIntIntIntIntToInt(arena_, Primitive::kPrimLong, invoke);
}

void IntrinsicLocationsBuilderX86_64::VisitUnsafeCASObject(HInvoke* invoke) {
  // The UnsafeCASObject intrinsic is missing a read barrier, and
  // therefore sometimes does not work as expected (b/25883050).
  // Turn it off temporarily as a quick fix, until the read barrier is
  // implemented.
  //
  // TODO(rpl): Implement a read barrier in GenCAS below and re-enable
  // this intrinsic.
  if (kEmitCompilerReadBarrier) {
    return;
  }

  CreateIntIntIntIntIntToInt(arena_, Primitive::kPrimNot, invoke);
}

static void GenCAS(Primitive::Type type, HInvoke* invoke, CodeGeneratorX86_64* codegen) {
  X86_64Assembler* assembler = down_cast<X86_64Assembler*>(codegen->GetAssembler());
  LocationSummary* locations = invoke->GetLocations();

  CpuRegister base = locations->InAt(1).AsRegister<CpuRegister>();
  CpuRegister offset = locations->InAt(2).AsRegister<CpuRegister>();
  CpuRegister expected = locations->InAt(3).AsRegister<CpuRegister>();
  // Ensure `expected` is in RAX (required by the CMPXCHG instruction).
  DCHECK_EQ(expected.AsRegister(), RAX);
  CpuRegister value = locations->InAt(4).AsRegister<CpuRegister>();
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();

  if (type == Primitive::kPrimNot) {
    // Mark card for object assuming new value is stored.
    bool value_can_be_null = true;  // TODO: Worth finding out this information?
    codegen->MarkGCCard(locations->GetTemp(0).AsRegister<CpuRegister>(),
                        locations->GetTemp(1).AsRegister<CpuRegister>(),
                        base,
                        value,
                        value_can_be_null);

    bool base_equals_value = (base.AsRegister() == value.AsRegister());
    Register value_reg = value.AsRegister();
    if (kPoisonHeapReferences) {
      if (base_equals_value) {
        // If `base` and `value` are the same register location, move
        // `value_reg` to a temporary register.  This way, poisoning
        // `value_reg` won't invalidate `base`.
        value_reg = locations->GetTemp(0).AsRegister<CpuRegister>().AsRegister();
        __ movl(CpuRegister(value_reg), base);
      }

      // Check that the register allocator did not assign the location
      // of `expected` (RAX) to `value` nor to `base`, so that heap
      // poisoning (when enabled) works as intended below.
      // - If `value` were equal to `expected`, both references would
      //   be poisoned twice, meaning they would not be poisoned at
      //   all, as heap poisoning uses address negation.
      // - If `base` were equal to `expected`, poisoning `expected`
      //   would invalidate `base`.
      DCHECK_NE(value_reg, expected.AsRegister());
      DCHECK_NE(base.AsRegister(), expected.AsRegister());

      __ PoisonHeapReference(expected);
      __ PoisonHeapReference(CpuRegister(value_reg));
    }

    // TODO: Add a read barrier for the reference stored in the object
    // before attempting the CAS, similar to the one in the
    // art::Unsafe_compareAndSwapObject JNI implementation.
    //
    // Note that this code is not (yet) used when read barriers are
    // enabled (see IntrinsicLocationsBuilderX86_64::VisitUnsafeCASObject).
    DCHECK(!kEmitCompilerReadBarrier);
    __ LockCmpxchgl(Address(base, offset, TIMES_1, 0), CpuRegister(value_reg));

    // LOCK CMPXCHG has full barrier semantics, and we don't need
    // scheduling barriers at this time.

    // Convert ZF into the boolean result.
    __ setcc(kZero, out);
    __ movzxb(out, out);

    // If heap poisoning is enabled, we need to unpoison the values
    // that were poisoned earlier.
    if (kPoisonHeapReferences) {
      if (base_equals_value) {
        // `value_reg` has been moved to a temporary register, no need
        // to unpoison it.
      } else {
        // Ensure `value` is different from `out`, so that unpoisoning
        // the former does not invalidate the latter.
        DCHECK_NE(value_reg, out.AsRegister());
        __ UnpoisonHeapReference(CpuRegister(value_reg));
      }
      // Ensure `expected` is different from `out`, so that unpoisoning
      // the former does not invalidate the latter.
      DCHECK_NE(expected.AsRegister(), out.AsRegister());
      __ UnpoisonHeapReference(expected);
    }
  } else {
    if (type == Primitive::kPrimInt) {
      __ LockCmpxchgl(Address(base, offset, TIMES_1, 0), value);
    } else if (type == Primitive::kPrimLong) {
      __ LockCmpxchgq(Address(base, offset, TIMES_1, 0), value);
    } else {
      LOG(FATAL) << "Unexpected CAS type " << type;
    }

    // LOCK CMPXCHG has full barrier semantics, and we don't need
    // scheduling barriers at this time.

    // Convert ZF into the boolean result.
    __ setcc(kZero, out);
    __ movzxb(out, out);
  }
}

void IntrinsicCodeGeneratorX86_64::VisitUnsafeCASInt(HInvoke* invoke) {
  GenCAS(Primitive::kPrimInt, invoke, codegen_);
}

void IntrinsicCodeGeneratorX86_64::VisitUnsafeCASLong(HInvoke* invoke) {
  GenCAS(Primitive::kPrimLong, invoke, codegen_);
}

void IntrinsicCodeGeneratorX86_64::VisitUnsafeCASObject(HInvoke* invoke) {
  GenCAS(Primitive::kPrimNot, invoke, codegen_);
}

void IntrinsicLocationsBuilderX86_64::VisitIntegerReverse(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
  locations->AddTemp(Location::RequiresRegister());
}

static void SwapBits(CpuRegister reg, CpuRegister temp, int32_t shift, int32_t mask,
                     X86_64Assembler* assembler) {
  Immediate imm_shift(shift);
  Immediate imm_mask(mask);
  __ movl(temp, reg);
  __ shrl(reg, imm_shift);
  __ andl(temp, imm_mask);
  __ andl(reg, imm_mask);
  __ shll(temp, imm_shift);
  __ orl(reg, temp);
}

void IntrinsicCodeGeneratorX86_64::VisitIntegerReverse(HInvoke* invoke) {
  X86_64Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  CpuRegister reg = locations->InAt(0).AsRegister<CpuRegister>();
  CpuRegister temp = locations->GetTemp(0).AsRegister<CpuRegister>();

  /*
   * Use one bswap instruction to reverse byte order first and then use 3 rounds of
   * swapping bits to reverse bits in a number x. Using bswap to save instructions
   * compared to generic luni implementation which has 5 rounds of swapping bits.
   * x = bswap x
   * x = (x & 0x55555555) << 1 | (x >> 1) & 0x55555555;
   * x = (x & 0x33333333) << 2 | (x >> 2) & 0x33333333;
   * x = (x & 0x0F0F0F0F) << 4 | (x >> 4) & 0x0F0F0F0F;
   */
  __ bswapl(reg);
  SwapBits(reg, temp, 1, 0x55555555, assembler);
  SwapBits(reg, temp, 2, 0x33333333, assembler);
  SwapBits(reg, temp, 4, 0x0f0f0f0f, assembler);
}

void IntrinsicLocationsBuilderX86_64::VisitLongReverse(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
  locations->AddTemp(Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
}

static void SwapBits64(CpuRegister reg, CpuRegister temp, CpuRegister temp_mask,
                       int32_t shift, int64_t mask, X86_64Assembler* assembler) {
  Immediate imm_shift(shift);
  __ movq(temp_mask, Immediate(mask));
  __ movq(temp, reg);
  __ shrq(reg, imm_shift);
  __ andq(temp, temp_mask);
  __ andq(reg, temp_mask);
  __ shlq(temp, imm_shift);
  __ orq(reg, temp);
}

void IntrinsicCodeGeneratorX86_64::VisitLongReverse(HInvoke* invoke) {
  X86_64Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  CpuRegister reg = locations->InAt(0).AsRegister<CpuRegister>();
  CpuRegister temp1 = locations->GetTemp(0).AsRegister<CpuRegister>();
  CpuRegister temp2 = locations->GetTemp(1).AsRegister<CpuRegister>();

  /*
   * Use one bswap instruction to reverse byte order first and then use 3 rounds of
   * swapping bits to reverse bits in a long number x. Using bswap to save instructions
   * compared to generic luni implementation which has 5 rounds of swapping bits.
   * x = bswap x
   * x = (x & 0x5555555555555555) << 1 | (x >> 1) & 0x5555555555555555;
   * x = (x & 0x3333333333333333) << 2 | (x >> 2) & 0x3333333333333333;
   * x = (x & 0x0F0F0F0F0F0F0F0F) << 4 | (x >> 4) & 0x0F0F0F0F0F0F0F0F;
   */
  __ bswapq(reg);
  SwapBits64(reg, temp1, temp2, 1, INT64_C(0x5555555555555555), assembler);
  SwapBits64(reg, temp1, temp2, 2, INT64_C(0x3333333333333333), assembler);
  SwapBits64(reg, temp1, temp2, 4, INT64_C(0x0f0f0f0f0f0f0f0f), assembler);
}

static void CreateBitCountLocations(
    ArenaAllocator* arena, CodeGeneratorX86_64* codegen, HInvoke* invoke) {
  if (!codegen->GetInstructionSetFeatures().HasPopCnt()) {
    // Do nothing if there is no popcnt support. This results in generating
    // a call for the intrinsic rather than direct code.
    return;
  }
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::Any());
  locations->SetOut(Location::RequiresRegister());
}

static void GenBitCount(X86_64Assembler* assembler,
                        CodeGeneratorX86_64* codegen,
                        HInvoke* invoke,
                        bool is_long) {
  LocationSummary* locations = invoke->GetLocations();
  Location src = locations->InAt(0);
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();

  if (invoke->InputAt(0)->IsConstant()) {
    // Evaluate this at compile time.
    int64_t value = Int64FromConstant(invoke->InputAt(0)->AsConstant());
    int32_t result = is_long
        ? POPCOUNT(static_cast<uint64_t>(value))
        : POPCOUNT(static_cast<uint32_t>(value));
    codegen->Load32BitValue(out, result);
    return;
  }

  if (src.IsRegister()) {
    if (is_long) {
      __ popcntq(out, src.AsRegister<CpuRegister>());
    } else {
      __ popcntl(out, src.AsRegister<CpuRegister>());
    }
  } else if (is_long) {
    DCHECK(src.IsDoubleStackSlot());
    __ popcntq(out, Address(CpuRegister(RSP), src.GetStackIndex()));
  } else {
    DCHECK(src.IsStackSlot());
    __ popcntl(out, Address(CpuRegister(RSP), src.GetStackIndex()));
  }
}

void IntrinsicLocationsBuilderX86_64::VisitIntegerBitCount(HInvoke* invoke) {
  CreateBitCountLocations(arena_, codegen_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitIntegerBitCount(HInvoke* invoke) {
  GenBitCount(GetAssembler(), codegen_, invoke, /* is_long */ false);
}

void IntrinsicLocationsBuilderX86_64::VisitLongBitCount(HInvoke* invoke) {
  CreateBitCountLocations(arena_, codegen_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitLongBitCount(HInvoke* invoke) {
  GenBitCount(GetAssembler(), codegen_, invoke, /* is_long */ true);
}

static void CreateOneBitLocations(ArenaAllocator* arena, HInvoke* invoke, bool is_high) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::Any());
  locations->SetOut(Location::RequiresRegister());
  locations->AddTemp(is_high ? Location::RegisterLocation(RCX)  // needs CL
                             : Location::RequiresRegister());  // any will do
}

static void GenOneBit(X86_64Assembler* assembler,
                      CodeGeneratorX86_64* codegen,
                      HInvoke* invoke,
                      bool is_high, bool is_long) {
  LocationSummary* locations = invoke->GetLocations();
  Location src = locations->InAt(0);
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();

  if (invoke->InputAt(0)->IsConstant()) {
    // Evaluate this at compile time.
    int64_t value = Int64FromConstant(invoke->InputAt(0)->AsConstant());
    if (value == 0) {
      __ xorl(out, out);  // Clears upper bits too.
      return;
    }
    // Nonzero value.
    if (is_high) {
      value = is_long ? 63 - CLZ(static_cast<uint64_t>(value))
                      : 31 - CLZ(static_cast<uint32_t>(value));
    } else {
      value = is_long ? CTZ(static_cast<uint64_t>(value))
                      : CTZ(static_cast<uint32_t>(value));
    }
    if (is_long) {
      codegen->Load64BitValue(out, 1L << value);
    } else {
      codegen->Load32BitValue(out, 1 << value);
    }
    return;
  }

  // Handle the non-constant cases.
  CpuRegister tmp = locations->GetTemp(0).AsRegister<CpuRegister>();
  if (is_high) {
    // Use architectural support: basically 1 << bsr.
    if (src.IsRegister()) {
      if (is_long) {
        __ bsrq(tmp, src.AsRegister<CpuRegister>());
      } else {
        __ bsrl(tmp, src.AsRegister<CpuRegister>());
      }
    } else if (is_long) {
      DCHECK(src.IsDoubleStackSlot());
      __ bsrq(tmp, Address(CpuRegister(RSP), src.GetStackIndex()));
    } else {
      DCHECK(src.IsStackSlot());
      __ bsrl(tmp, Address(CpuRegister(RSP), src.GetStackIndex()));
    }
    // BSR sets ZF if the input was zero.
    NearLabel is_zero, done;
    __ j(kEqual, &is_zero);
    __ movl(out, Immediate(1));  // Clears upper bits too.
    if (is_long) {
      __ shlq(out, tmp);
    } else {
      __ shll(out, tmp);
    }
    __ jmp(&done);
    __ Bind(&is_zero);
    __ xorl(out, out);  // Clears upper bits too.
    __ Bind(&done);
  } else  {
    // Copy input into temporary.
    if (src.IsRegister()) {
      if (is_long) {
        __ movq(tmp, src.AsRegister<CpuRegister>());
      } else {
        __ movl(tmp, src.AsRegister<CpuRegister>());
      }
    } else if (is_long) {
      DCHECK(src.IsDoubleStackSlot());
      __ movq(tmp, Address(CpuRegister(RSP), src.GetStackIndex()));
    } else {
      DCHECK(src.IsStackSlot());
      __ movl(tmp, Address(CpuRegister(RSP), src.GetStackIndex()));
    }
    // Do the bit twiddling: basically tmp & -tmp;
    if (is_long) {
      __ movq(out, tmp);
      __ negq(tmp);
      __ andq(out, tmp);
    } else {
      __ movl(out, tmp);
      __ negl(tmp);
      __ andl(out, tmp);
    }
  }
}

void IntrinsicLocationsBuilderX86_64::VisitIntegerHighestOneBit(HInvoke* invoke) {
  CreateOneBitLocations(arena_, invoke, /* is_high */ true);
}

void IntrinsicCodeGeneratorX86_64::VisitIntegerHighestOneBit(HInvoke* invoke) {
  GenOneBit(GetAssembler(), codegen_, invoke, /* is_high */ true, /* is_long */ false);
}

void IntrinsicLocationsBuilderX86_64::VisitLongHighestOneBit(HInvoke* invoke) {
  CreateOneBitLocations(arena_, invoke, /* is_high */ true);
}

void IntrinsicCodeGeneratorX86_64::VisitLongHighestOneBit(HInvoke* invoke) {
  GenOneBit(GetAssembler(), codegen_, invoke, /* is_high */ true, /* is_long */ true);
}

void IntrinsicLocationsBuilderX86_64::VisitIntegerLowestOneBit(HInvoke* invoke) {
  CreateOneBitLocations(arena_, invoke, /* is_high */ false);
}

void IntrinsicCodeGeneratorX86_64::VisitIntegerLowestOneBit(HInvoke* invoke) {
  GenOneBit(GetAssembler(), codegen_, invoke, /* is_high */ false, /* is_long */ false);
}

void IntrinsicLocationsBuilderX86_64::VisitLongLowestOneBit(HInvoke* invoke) {
  CreateOneBitLocations(arena_, invoke, /* is_high */ false);
}

void IntrinsicCodeGeneratorX86_64::VisitLongLowestOneBit(HInvoke* invoke) {
  GenOneBit(GetAssembler(), codegen_, invoke, /* is_high */ false, /* is_long */ true);
}

static void CreateLeadingZeroLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::Any());
  locations->SetOut(Location::RequiresRegister());
}

static void GenLeadingZeros(X86_64Assembler* assembler,
                            CodeGeneratorX86_64* codegen,
                            HInvoke* invoke, bool is_long) {
  LocationSummary* locations = invoke->GetLocations();
  Location src = locations->InAt(0);
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();

  int zero_value_result = is_long ? 64 : 32;
  if (invoke->InputAt(0)->IsConstant()) {
    // Evaluate this at compile time.
    int64_t value = Int64FromConstant(invoke->InputAt(0)->AsConstant());
    if (value == 0) {
      value = zero_value_result;
    } else {
      value = is_long ? CLZ(static_cast<uint64_t>(value)) : CLZ(static_cast<uint32_t>(value));
    }
    codegen->Load32BitValue(out, value);
    return;
  }

  // Handle the non-constant cases.
  if (src.IsRegister()) {
    if (is_long) {
      __ bsrq(out, src.AsRegister<CpuRegister>());
    } else {
      __ bsrl(out, src.AsRegister<CpuRegister>());
    }
  } else if (is_long) {
    DCHECK(src.IsDoubleStackSlot());
    __ bsrq(out, Address(CpuRegister(RSP), src.GetStackIndex()));
  } else {
    DCHECK(src.IsStackSlot());
    __ bsrl(out, Address(CpuRegister(RSP), src.GetStackIndex()));
  }

  // BSR sets ZF if the input was zero, and the output is undefined.
  NearLabel is_zero, done;
  __ j(kEqual, &is_zero);

  // Correct the result from BSR to get the CLZ result.
  __ xorl(out, Immediate(zero_value_result - 1));
  __ jmp(&done);

  // Fix the zero case with the expected result.
  __ Bind(&is_zero);
  __ movl(out, Immediate(zero_value_result));

  __ Bind(&done);
}

void IntrinsicLocationsBuilderX86_64::VisitIntegerNumberOfLeadingZeros(HInvoke* invoke) {
  CreateLeadingZeroLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitIntegerNumberOfLeadingZeros(HInvoke* invoke) {
  GenLeadingZeros(GetAssembler(), codegen_, invoke, /* is_long */ false);
}

void IntrinsicLocationsBuilderX86_64::VisitLongNumberOfLeadingZeros(HInvoke* invoke) {
  CreateLeadingZeroLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitLongNumberOfLeadingZeros(HInvoke* invoke) {
  GenLeadingZeros(GetAssembler(), codegen_, invoke, /* is_long */ true);
}

static void CreateTrailingZeroLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::Any());
  locations->SetOut(Location::RequiresRegister());
}

static void GenTrailingZeros(X86_64Assembler* assembler,
                             CodeGeneratorX86_64* codegen,
                             HInvoke* invoke, bool is_long) {
  LocationSummary* locations = invoke->GetLocations();
  Location src = locations->InAt(0);
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();

  int zero_value_result = is_long ? 64 : 32;
  if (invoke->InputAt(0)->IsConstant()) {
    // Evaluate this at compile time.
    int64_t value = Int64FromConstant(invoke->InputAt(0)->AsConstant());
    if (value == 0) {
      value = zero_value_result;
    } else {
      value = is_long ? CTZ(static_cast<uint64_t>(value)) : CTZ(static_cast<uint32_t>(value));
    }
    codegen->Load32BitValue(out, value);
    return;
  }

  // Handle the non-constant cases.
  if (src.IsRegister()) {
    if (is_long) {
      __ bsfq(out, src.AsRegister<CpuRegister>());
    } else {
      __ bsfl(out, src.AsRegister<CpuRegister>());
    }
  } else if (is_long) {
    DCHECK(src.IsDoubleStackSlot());
    __ bsfq(out, Address(CpuRegister(RSP), src.GetStackIndex()));
  } else {
    DCHECK(src.IsStackSlot());
    __ bsfl(out, Address(CpuRegister(RSP), src.GetStackIndex()));
  }

  // BSF sets ZF if the input was zero, and the output is undefined.
  NearLabel done;
  __ j(kNotEqual, &done);

  // Fix the zero case with the expected result.
  __ movl(out, Immediate(zero_value_result));

  __ Bind(&done);
}

void IntrinsicLocationsBuilderX86_64::VisitIntegerNumberOfTrailingZeros(HInvoke* invoke) {
  CreateTrailingZeroLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitIntegerNumberOfTrailingZeros(HInvoke* invoke) {
  GenTrailingZeros(GetAssembler(), codegen_, invoke, /* is_long */ false);
}

void IntrinsicLocationsBuilderX86_64::VisitLongNumberOfTrailingZeros(HInvoke* invoke) {
  CreateTrailingZeroLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitLongNumberOfTrailingZeros(HInvoke* invoke) {
  GenTrailingZeros(GetAssembler(), codegen_, invoke, /* is_long */ true);
}

UNIMPLEMENTED_INTRINSIC(X86_64, ReferenceGetReferent)
UNIMPLEMENTED_INTRINSIC(X86_64, FloatIsInfinite)
UNIMPLEMENTED_INTRINSIC(X86_64, DoubleIsInfinite)

// 1.8.
UNIMPLEMENTED_INTRINSIC(X86_64, UnsafeGetAndAddInt)
UNIMPLEMENTED_INTRINSIC(X86_64, UnsafeGetAndAddLong)
UNIMPLEMENTED_INTRINSIC(X86_64, UnsafeGetAndSetInt)
UNIMPLEMENTED_INTRINSIC(X86_64, UnsafeGetAndSetLong)
UNIMPLEMENTED_INTRINSIC(X86_64, UnsafeGetAndSetObject)

UNREACHABLE_INTRINSICS(X86_64)

#undef __

}  // namespace x86_64
}  // namespace art
