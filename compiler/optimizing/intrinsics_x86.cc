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

#include "intrinsics_x86.h"

#include <limits>

#include "arch/x86/instruction_set_features_x86.h"
#include "art_method.h"
#include "base/bit_utils.h"
#include "code_generator_x86.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "intrinsics.h"
#include "intrinsics_utils.h"
#include "mirror/array-inl.h"
#include "mirror/string.h"
#include "thread.h"
#include "utils/x86/assembler_x86.h"
#include "utils/x86/constants_x86.h"

namespace art {

namespace x86 {

static constexpr int kDoubleNaNHigh = 0x7FF80000;
static constexpr int kDoubleNaNLow = 0x00000000;
static constexpr int64_t kDoubleNaN = INT64_C(0x7FF8000000000000);
static constexpr int32_t kFloatNaN = INT32_C(0x7FC00000);

IntrinsicLocationsBuilderX86::IntrinsicLocationsBuilderX86(CodeGeneratorX86* codegen)
  : arena_(codegen->GetGraph()->GetArena()),
    codegen_(codegen) {
}


X86Assembler* IntrinsicCodeGeneratorX86::GetAssembler() {
  return down_cast<X86Assembler*>(codegen_->GetAssembler());
}

ArenaAllocator* IntrinsicCodeGeneratorX86::GetAllocator() {
  return codegen_->GetGraph()->GetArena();
}

bool IntrinsicLocationsBuilderX86::TryDispatch(HInvoke* invoke) {
  Dispatch(invoke);
  LocationSummary* res = invoke->GetLocations();
  if (res == nullptr) {
    return false;
  }
  if (kEmitCompilerReadBarrier && res->CanCall()) {
    // Generating an intrinsic for this HInvoke may produce an
    // IntrinsicSlowPathX86 slow path.  Currently this approach
    // does not work when using read barriers, as the emitted
    // calling sequence will make use of another slow path
    // (ReadBarrierForRootSlowPathX86 for HInvokeStaticOrDirect,
    // ReadBarrierSlowPathX86 for HInvokeVirtual).  So we bail
    // out in this case.
    //
    // TODO: Find a way to have intrinsics work with read barriers.
    invoke->SetLocations(nullptr);
    return false;
  }
  return res->Intrinsified();
}

static void MoveArguments(HInvoke* invoke, CodeGeneratorX86* codegen) {
  InvokeDexCallingConventionVisitorX86 calling_convention_visitor;
  IntrinsicVisitor::MoveArguments(invoke, codegen, &calling_convention_visitor);
}

using IntrinsicSlowPathX86 = IntrinsicSlowPath<InvokeDexCallingConventionVisitorX86>;

#define __ assembler->

static void CreateFPToIntLocations(ArenaAllocator* arena, HInvoke* invoke, bool is64bit) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetOut(Location::RequiresRegister());
  if (is64bit) {
    locations->AddTemp(Location::RequiresFpuRegister());
  }
}

static void CreateIntToFPLocations(ArenaAllocator* arena, HInvoke* invoke, bool is64bit) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresFpuRegister());
  if (is64bit) {
    locations->AddTemp(Location::RequiresFpuRegister());
    locations->AddTemp(Location::RequiresFpuRegister());
  }
}

static void MoveFPToInt(LocationSummary* locations, bool is64bit, X86Assembler* assembler) {
  Location input = locations->InAt(0);
  Location output = locations->Out();
  if (is64bit) {
    // Need to use the temporary.
    XmmRegister temp = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
    __ movsd(temp, input.AsFpuRegister<XmmRegister>());
    __ movd(output.AsRegisterPairLow<Register>(), temp);
    __ psrlq(temp, Immediate(32));
    __ movd(output.AsRegisterPairHigh<Register>(), temp);
  } else {
    __ movd(output.AsRegister<Register>(), input.AsFpuRegister<XmmRegister>());
  }
}

static void MoveIntToFP(LocationSummary* locations, bool is64bit, X86Assembler* assembler) {
  Location input = locations->InAt(0);
  Location output = locations->Out();
  if (is64bit) {
    // Need to use the temporary.
    XmmRegister temp1 = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
    XmmRegister temp2 = locations->GetTemp(1).AsFpuRegister<XmmRegister>();
    __ movd(temp1, input.AsRegisterPairLow<Register>());
    __ movd(temp2, input.AsRegisterPairHigh<Register>());
    __ punpckldq(temp1, temp2);
    __ movsd(output.AsFpuRegister<XmmRegister>(), temp1);
  } else {
    __ movd(output.AsFpuRegister<XmmRegister>(), input.AsRegister<Register>());
  }
}

void IntrinsicLocationsBuilderX86::VisitDoubleDoubleToRawLongBits(HInvoke* invoke) {
  CreateFPToIntLocations(arena_, invoke, /* is64bit */ true);
}
void IntrinsicLocationsBuilderX86::VisitDoubleLongBitsToDouble(HInvoke* invoke) {
  CreateIntToFPLocations(arena_, invoke, /* is64bit */ true);
}

void IntrinsicCodeGeneratorX86::VisitDoubleDoubleToRawLongBits(HInvoke* invoke) {
  MoveFPToInt(invoke->GetLocations(), /* is64bit */ true, GetAssembler());
}
void IntrinsicCodeGeneratorX86::VisitDoubleLongBitsToDouble(HInvoke* invoke) {
  MoveIntToFP(invoke->GetLocations(), /* is64bit */ true, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitFloatFloatToRawIntBits(HInvoke* invoke) {
  CreateFPToIntLocations(arena_, invoke, /* is64bit */ false);
}
void IntrinsicLocationsBuilderX86::VisitFloatIntBitsToFloat(HInvoke* invoke) {
  CreateIntToFPLocations(arena_, invoke, /* is64bit */ false);
}

void IntrinsicCodeGeneratorX86::VisitFloatFloatToRawIntBits(HInvoke* invoke) {
  MoveFPToInt(invoke->GetLocations(), /* is64bit */ false, GetAssembler());
}
void IntrinsicCodeGeneratorX86::VisitFloatIntBitsToFloat(HInvoke* invoke) {
  MoveIntToFP(invoke->GetLocations(), /* is64bit */ false, GetAssembler());
}

static void CreateIntToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
}

static void CreateLongToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
}

static void CreateLongToLongLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
}

static void GenReverseBytes(LocationSummary* locations,
                            Primitive::Type size,
                            X86Assembler* assembler) {
  Register out = locations->Out().AsRegister<Register>();

  switch (size) {
    case Primitive::kPrimShort:
      // TODO: Can be done with an xchg of 8b registers. This is straight from Quick.
      __ bswapl(out);
      __ sarl(out, Immediate(16));
      break;
    case Primitive::kPrimInt:
      __ bswapl(out);
      break;
    default:
      LOG(FATAL) << "Unexpected size for reverse-bytes: " << size;
      UNREACHABLE();
  }
}

void IntrinsicLocationsBuilderX86::VisitIntegerReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitIntegerReverseBytes(HInvoke* invoke) {
  GenReverseBytes(invoke->GetLocations(), Primitive::kPrimInt, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitLongReverseBytes(HInvoke* invoke) {
  CreateLongToLongLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitLongReverseBytes(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  Location input = locations->InAt(0);
  Register input_lo = input.AsRegisterPairLow<Register>();
  Register input_hi = input.AsRegisterPairHigh<Register>();
  Location output = locations->Out();
  Register output_lo = output.AsRegisterPairLow<Register>();
  Register output_hi = output.AsRegisterPairHigh<Register>();

  X86Assembler* assembler = GetAssembler();
  // Assign the inputs to the outputs, mixing low/high.
  __ movl(output_lo, input_hi);
  __ movl(output_hi, input_lo);
  __ bswapl(output_lo);
  __ bswapl(output_hi);
}

void IntrinsicLocationsBuilderX86::VisitShortReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitShortReverseBytes(HInvoke* invoke) {
  GenReverseBytes(invoke->GetLocations(), Primitive::kPrimShort, GetAssembler());
}


// TODO: Consider Quick's way of doing Double abs through integer operations, as the immediate we
//       need is 64b.

static void CreateFloatToFloat(ArenaAllocator* arena, HInvoke* invoke) {
  // TODO: Enable memory operations when the assembler supports them.
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetOut(Location::SameAsFirstInput());
  HInvokeStaticOrDirect* static_or_direct = invoke->AsInvokeStaticOrDirect();
  DCHECK(static_or_direct != nullptr);
  if (static_or_direct->HasSpecialInput() &&
      invoke->InputAt(static_or_direct->GetSpecialInputIndex())->IsX86ComputeBaseMethodAddress()) {
    // We need addressibility for the constant area.
    locations->SetInAt(1, Location::RequiresRegister());
    // We need a temporary to hold the constant.
    locations->AddTemp(Location::RequiresFpuRegister());
  }
}

static void MathAbsFP(LocationSummary* locations,
                      bool is64bit,
                      X86Assembler* assembler,
                      CodeGeneratorX86* codegen) {
  Location output = locations->Out();

  DCHECK(output.IsFpuRegister());
  if (locations->GetInputCount() == 2 && locations->InAt(1).IsValid()) {
    DCHECK(locations->InAt(1).IsRegister());
    // We also have a constant area pointer.
    Register constant_area = locations->InAt(1).AsRegister<Register>();
    XmmRegister temp = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
    if (is64bit) {
      __ movsd(temp, codegen->LiteralInt64Address(INT64_C(0x7FFFFFFFFFFFFFFF), constant_area));
      __ andpd(output.AsFpuRegister<XmmRegister>(), temp);
    } else {
      __ movss(temp, codegen->LiteralInt32Address(INT32_C(0x7FFFFFFF), constant_area));
      __ andps(output.AsFpuRegister<XmmRegister>(), temp);
    }
  } else {
    // Create the right constant on an aligned stack.
    if (is64bit) {
      __ subl(ESP, Immediate(8));
      __ pushl(Immediate(0x7FFFFFFF));
      __ pushl(Immediate(0xFFFFFFFF));
      __ andpd(output.AsFpuRegister<XmmRegister>(), Address(ESP, 0));
    } else {
      __ subl(ESP, Immediate(12));
      __ pushl(Immediate(0x7FFFFFFF));
      __ andps(output.AsFpuRegister<XmmRegister>(), Address(ESP, 0));
    }
    __ addl(ESP, Immediate(16));
  }
}

void IntrinsicLocationsBuilderX86::VisitMathAbsDouble(HInvoke* invoke) {
  CreateFloatToFloat(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathAbsDouble(HInvoke* invoke) {
  MathAbsFP(invoke->GetLocations(), /* is64bit */ true, GetAssembler(), codegen_);
}

void IntrinsicLocationsBuilderX86::VisitMathAbsFloat(HInvoke* invoke) {
  CreateFloatToFloat(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathAbsFloat(HInvoke* invoke) {
  MathAbsFP(invoke->GetLocations(), /* is64bit */ false, GetAssembler(), codegen_);
}

static void CreateAbsIntLocation(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RegisterLocation(EAX));
  locations->SetOut(Location::SameAsFirstInput());
  locations->AddTemp(Location::RegisterLocation(EDX));
}

static void GenAbsInteger(LocationSummary* locations, X86Assembler* assembler) {
  Location output = locations->Out();
  Register out = output.AsRegister<Register>();
  DCHECK_EQ(out, EAX);
  Register temp = locations->GetTemp(0).AsRegister<Register>();
  DCHECK_EQ(temp, EDX);

  // Sign extend EAX into EDX.
  __ cdq();

  // XOR EAX with sign.
  __ xorl(EAX, EDX);

  // Subtract out sign to correct.
  __ subl(EAX, EDX);

  // The result is in EAX.
}

static void CreateAbsLongLocation(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
  locations->AddTemp(Location::RequiresRegister());
}

static void GenAbsLong(LocationSummary* locations, X86Assembler* assembler) {
  Location input = locations->InAt(0);
  Register input_lo = input.AsRegisterPairLow<Register>();
  Register input_hi = input.AsRegisterPairHigh<Register>();
  Location output = locations->Out();
  Register output_lo = output.AsRegisterPairLow<Register>();
  Register output_hi = output.AsRegisterPairHigh<Register>();
  Register temp = locations->GetTemp(0).AsRegister<Register>();

  // Compute the sign into the temporary.
  __ movl(temp, input_hi);
  __ sarl(temp, Immediate(31));

  // Store the sign into the output.
  __ movl(output_lo, temp);
  __ movl(output_hi, temp);

  // XOR the input to the output.
  __ xorl(output_lo, input_lo);
  __ xorl(output_hi, input_hi);

  // Subtract the sign.
  __ subl(output_lo, temp);
  __ sbbl(output_hi, temp);
}

void IntrinsicLocationsBuilderX86::VisitMathAbsInt(HInvoke* invoke) {
  CreateAbsIntLocation(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathAbsInt(HInvoke* invoke) {
  GenAbsInteger(invoke->GetLocations(), GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMathAbsLong(HInvoke* invoke) {
  CreateAbsLongLocation(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathAbsLong(HInvoke* invoke) {
  GenAbsLong(invoke->GetLocations(), GetAssembler());
}

static void GenMinMaxFP(LocationSummary* locations,
                        bool is_min,
                        bool is_double,
                        X86Assembler* assembler,
                        CodeGeneratorX86* codegen) {
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
  // TODO: This is straight from Quick (except literal pool). Make NaN an out-of-line slowpath?

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
  // Do we have a constant area pointer?
  if (locations->GetInputCount() == 3 && locations->InAt(2).IsValid()) {
    DCHECK(locations->InAt(2).IsRegister());
    Register constant_area = locations->InAt(2).AsRegister<Register>();
    if (is_double) {
      __ movsd(out, codegen->LiteralInt64Address(kDoubleNaN, constant_area));
    } else {
      __ movss(out, codegen->LiteralInt32Address(kFloatNaN, constant_area));
    }
  } else {
    if (is_double) {
      __ pushl(Immediate(kDoubleNaNHigh));
      __ pushl(Immediate(kDoubleNaNLow));
      __ movsd(out, Address(ESP, 0));
      __ addl(ESP, Immediate(8));
    } else {
      __ pushl(Immediate(kFloatNaN));
      __ movss(out, Address(ESP, 0));
      __ addl(ESP, Immediate(4));
    }
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

static void CreateFPFPToFPLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetInAt(1, Location::RequiresFpuRegister());
  // The following is sub-optimal, but all we can do for now. It would be fine to also accept
  // the second input to be the output (we can simply swap inputs).
  locations->SetOut(Location::SameAsFirstInput());
  HInvokeStaticOrDirect* static_or_direct = invoke->AsInvokeStaticOrDirect();
  DCHECK(static_or_direct != nullptr);
  if (static_or_direct->HasSpecialInput() &&
      invoke->InputAt(static_or_direct->GetSpecialInputIndex())->IsX86ComputeBaseMethodAddress()) {
    locations->SetInAt(2, Location::RequiresRegister());
  }
}

void IntrinsicLocationsBuilderX86::VisitMathMinDoubleDouble(HInvoke* invoke) {
  CreateFPFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathMinDoubleDouble(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(),
              /* is_min */ true,
              /* is_double */ true,
              GetAssembler(),
              codegen_);
}

void IntrinsicLocationsBuilderX86::VisitMathMinFloatFloat(HInvoke* invoke) {
  CreateFPFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathMinFloatFloat(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(),
              /* is_min */ true,
              /* is_double */ false,
              GetAssembler(),
              codegen_);
}

void IntrinsicLocationsBuilderX86::VisitMathMaxDoubleDouble(HInvoke* invoke) {
  CreateFPFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathMaxDoubleDouble(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(),
              /* is_min */ false,
              /* is_double */ true,
              GetAssembler(),
              codegen_);
}

void IntrinsicLocationsBuilderX86::VisitMathMaxFloatFloat(HInvoke* invoke) {
  CreateFPFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathMaxFloatFloat(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(),
              /* is_min */ false,
              /* is_double */ false,
              GetAssembler(),
              codegen_);
}

static void GenMinMax(LocationSummary* locations, bool is_min, bool is_long,
                      X86Assembler* assembler) {
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

  if (is_long) {
    // Need to perform a subtract to get the sign right.
    // op1 is already in the same location as the output.
    Location output = locations->Out();
    Register output_lo = output.AsRegisterPairLow<Register>();
    Register output_hi = output.AsRegisterPairHigh<Register>();

    Register op2_lo = op2_loc.AsRegisterPairLow<Register>();
    Register op2_hi = op2_loc.AsRegisterPairHigh<Register>();

    // Spare register to compute the subtraction to set condition code.
    Register temp = locations->GetTemp(0).AsRegister<Register>();

    // Subtract off op2_low.
    __ movl(temp, output_lo);
    __ subl(temp, op2_lo);

    // Now use the same tempo and the borrow to finish the subtraction of op2_hi.
    __ movl(temp, output_hi);
    __ sbbl(temp, op2_hi);

    // Now the condition code is correct.
    Condition cond = is_min ? Condition::kGreaterEqual : Condition::kLess;
    __ cmovl(cond, output_lo, op2_lo);
    __ cmovl(cond, output_hi, op2_hi);
  } else {
    Register out = locations->Out().AsRegister<Register>();
    Register op2 = op2_loc.AsRegister<Register>();

    //  (out := op1)
    //  out <=? op2
    //  if out is min jmp done
    //  out := op2
    // done:

    __ cmpl(out, op2);
    Condition cond = is_min ? Condition::kGreater : Condition::kLess;
    __ cmovl(cond, out, op2);
  }
}

static void CreateIntIntToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
}

static void CreateLongLongToLongLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
  // Register to use to perform a long subtract to set cc.
  locations->AddTemp(Location::RequiresRegister());
}

void IntrinsicLocationsBuilderX86::VisitMathMinIntInt(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathMinIntInt(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), /* is_min */ true, /* is_long */ false, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMathMinLongLong(HInvoke* invoke) {
  CreateLongLongToLongLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathMinLongLong(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), /* is_min */ true, /* is_long */ true, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMathMaxIntInt(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathMaxIntInt(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), /* is_min */ false, /* is_long */ false, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMathMaxLongLong(HInvoke* invoke) {
  CreateLongLongToLongLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathMaxLongLong(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), /* is_min */ false, /* is_long */ true, GetAssembler());
}

static void CreateFPToFPLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetOut(Location::RequiresFpuRegister());
}

void IntrinsicLocationsBuilderX86::VisitMathSqrt(HInvoke* invoke) {
  CreateFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathSqrt(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  XmmRegister in = locations->InAt(0).AsFpuRegister<XmmRegister>();
  XmmRegister out = locations->Out().AsFpuRegister<XmmRegister>();

  GetAssembler()->sqrtsd(out, in);
}

static void InvokeOutOfLineIntrinsic(CodeGeneratorX86* codegen, HInvoke* invoke) {
  MoveArguments(invoke, codegen);

  DCHECK(invoke->IsInvokeStaticOrDirect());
  codegen->GenerateStaticOrDirectCall(invoke->AsInvokeStaticOrDirect(),
                                      Location::RegisterLocation(EAX));
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
                                      CodeGeneratorX86* codegen) {
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
  // Needs to be EAX for the invoke.
  locations->AddTemp(Location::RegisterLocation(EAX));
}

static void GenSSE41FPToFPIntrinsic(CodeGeneratorX86* codegen,
                                   HInvoke* invoke,
                                   X86Assembler* assembler,
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

void IntrinsicLocationsBuilderX86::VisitMathCeil(HInvoke* invoke) {
  CreateSSE41FPToFPLocations(arena_, invoke, codegen_);
}

void IntrinsicCodeGeneratorX86::VisitMathCeil(HInvoke* invoke) {
  GenSSE41FPToFPIntrinsic(codegen_, invoke, GetAssembler(), 2);
}

void IntrinsicLocationsBuilderX86::VisitMathFloor(HInvoke* invoke) {
  CreateSSE41FPToFPLocations(arena_, invoke, codegen_);
}

void IntrinsicCodeGeneratorX86::VisitMathFloor(HInvoke* invoke) {
  GenSSE41FPToFPIntrinsic(codegen_, invoke, GetAssembler(), 1);
}

void IntrinsicLocationsBuilderX86::VisitMathRint(HInvoke* invoke) {
  CreateSSE41FPToFPLocations(arena_, invoke, codegen_);
}

void IntrinsicCodeGeneratorX86::VisitMathRint(HInvoke* invoke) {
  GenSSE41FPToFPIntrinsic(codegen_, invoke, GetAssembler(), 0);
}

// Note that 32 bit x86 doesn't have the capability to inline MathRoundDouble,
// as it needs 64 bit instructions.
void IntrinsicLocationsBuilderX86::VisitMathRoundFloat(HInvoke* invoke) {
  // See intrinsics.h.
  if (!kRoundIsPlusPointFive) {
    return;
  }

  // Do we have instruction support?
  if (codegen_->GetInstructionSetFeatures().HasSSE4_1()) {
    LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                              LocationSummary::kNoCall,
                                                              kIntrinsified);
    locations->SetInAt(0, Location::RequiresFpuRegister());
    locations->SetOut(Location::RequiresRegister());
    locations->AddTemp(Location::RequiresFpuRegister());
    locations->AddTemp(Location::RequiresFpuRegister());
    return;
  }

  // We have to fall back to a call to the intrinsic.
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                           LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetFpuRegisterAt(0)));
  locations->SetOut(Location::RegisterLocation(EAX));
  // Needs to be EAX for the invoke.
  locations->AddTemp(Location::RegisterLocation(EAX));
}

void IntrinsicCodeGeneratorX86::VisitMathRoundFloat(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  if (locations->WillCall()) {
    InvokeOutOfLineIntrinsic(codegen_, invoke);
    return;
  }

  // Implement RoundFloat as t1 = floor(input + 0.5f);  convert to int.
  XmmRegister in = locations->InAt(0).AsFpuRegister<XmmRegister>();
  Register out = locations->Out().AsRegister<Register>();
  XmmRegister maxInt = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
  XmmRegister inPlusPointFive = locations->GetTemp(1).AsFpuRegister<XmmRegister>();
  NearLabel done, nan;
  X86Assembler* assembler = GetAssembler();

  // Generate 0.5 into inPlusPointFive.
  __ movl(out, Immediate(bit_cast<int32_t, float>(0.5f)));
  __ movd(inPlusPointFive, out);

  // Add in the input.
  __ addss(inPlusPointFive, in);

  // And truncate to an integer.
  __ roundss(inPlusPointFive, inPlusPointFive, Immediate(1));

  __ movl(out, Immediate(kPrimIntMax));
  // maxInt = int-to-float(out)
  __ cvtsi2ss(maxInt, out);

  // if inPlusPointFive >= maxInt goto done
  __ comiss(inPlusPointFive, maxInt);
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

static void CreateFPToFPCallLocations(ArenaAllocator* arena,
                                      HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kCall,
                                                           kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(0)));
  locations->SetOut(Location::FpuRegisterLocation(XMM0));
}

static void GenFPToFPCall(HInvoke* invoke, CodeGeneratorX86* codegen, QuickEntrypointEnum entry) {
  LocationSummary* locations = invoke->GetLocations();
  DCHECK(locations->WillCall());
  DCHECK(invoke->IsInvokeStaticOrDirect());
  X86Assembler* assembler = codegen->GetAssembler();

  // We need some place to pass the parameters.
  __ subl(ESP, Immediate(16));
  __ cfi().AdjustCFAOffset(16);

  // Pass the parameters at the bottom of the stack.
  __ movsd(Address(ESP, 0), XMM0);

  // If we have a second parameter, pass it next.
  if (invoke->GetNumberOfArguments() == 2) {
    __ movsd(Address(ESP, 8), XMM1);
  }

  // Now do the actual call.
  __ fs()->call(Address::Absolute(GetThreadOffset<kX86WordSize>(entry)));

  // Extract the return value from the FP stack.
  __ fstpl(Address(ESP, 0));
  __ movsd(XMM0, Address(ESP, 0));

  // And clean up the stack.
  __ addl(ESP, Immediate(16));
  __ cfi().AdjustCFAOffset(-16);

  codegen->RecordPcInfo(invoke, invoke->GetDexPc());
}

void IntrinsicLocationsBuilderX86::VisitMathCos(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathCos(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickCos);
}

void IntrinsicLocationsBuilderX86::VisitMathSin(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathSin(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickSin);
}

void IntrinsicLocationsBuilderX86::VisitMathAcos(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathAcos(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickAcos);
}

void IntrinsicLocationsBuilderX86::VisitMathAsin(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathAsin(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickAsin);
}

void IntrinsicLocationsBuilderX86::VisitMathAtan(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathAtan(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickAtan);
}

void IntrinsicLocationsBuilderX86::VisitMathCbrt(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathCbrt(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickCbrt);
}

void IntrinsicLocationsBuilderX86::VisitMathCosh(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathCosh(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickCosh);
}

void IntrinsicLocationsBuilderX86::VisitMathExp(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathExp(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickExp);
}

void IntrinsicLocationsBuilderX86::VisitMathExpm1(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathExpm1(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickExpm1);
}

void IntrinsicLocationsBuilderX86::VisitMathLog(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathLog(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickLog);
}

void IntrinsicLocationsBuilderX86::VisitMathLog10(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathLog10(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickLog10);
}

void IntrinsicLocationsBuilderX86::VisitMathSinh(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathSinh(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickSinh);
}

void IntrinsicLocationsBuilderX86::VisitMathTan(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathTan(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickTan);
}

void IntrinsicLocationsBuilderX86::VisitMathTanh(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathTanh(HInvoke* invoke) {
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
}

void IntrinsicLocationsBuilderX86::VisitMathAtan2(HInvoke* invoke) {
  CreateFPFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathAtan2(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickAtan2);
}

void IntrinsicLocationsBuilderX86::VisitMathHypot(HInvoke* invoke) {
  CreateFPFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathHypot(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickHypot);
}

void IntrinsicLocationsBuilderX86::VisitMathNextAfter(HInvoke* invoke) {
  CreateFPFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMathNextAfter(HInvoke* invoke) {
  GenFPToFPCall(invoke, codegen_, kQuickNextAfter);
}

void IntrinsicLocationsBuilderX86::VisitStringCharAt(HInvoke* invoke) {
  // The inputs plus one temp.
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCallOnSlowPath,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
}

void IntrinsicCodeGeneratorX86::VisitStringCharAt(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();

  // Location of reference to data array.
  const int32_t value_offset = mirror::String::ValueOffset().Int32Value();
  // Location of count.
  const int32_t count_offset = mirror::String::CountOffset().Int32Value();

  Register obj = locations->InAt(0).AsRegister<Register>();
  Register idx = locations->InAt(1).AsRegister<Register>();
  Register out = locations->Out().AsRegister<Register>();

  // TODO: Maybe we can support range check elimination. Overall, though, I think it's not worth
  //       the cost.
  // TODO: For simplicity, the index parameter is requested in a register, so different from Quick
  //       we will not optimize the code for constants (which would save a register).

  SlowPathCode* slow_path = new (GetAllocator()) IntrinsicSlowPathX86(invoke);
  codegen_->AddSlowPath(slow_path);

  X86Assembler* assembler = GetAssembler();

  __ cmpl(idx, Address(obj, count_offset));
  codegen_->MaybeRecordImplicitNullCheck(invoke);
  __ j(kAboveEqual, slow_path->GetEntryLabel());

  // out = out[2*idx].
  __ movzxw(out, Address(out, idx, ScaleFactor::TIMES_2, value_offset));

  __ Bind(slow_path->GetExitLabel());
}

void IntrinsicLocationsBuilderX86::VisitSystemArrayCopyChar(HInvoke* invoke) {
  // We need at least two of the positions or length to be an integer constant,
  // or else we won't have enough free registers.
  HIntConstant* src_pos = invoke->InputAt(1)->AsIntConstant();
  HIntConstant* dest_pos = invoke->InputAt(3)->AsIntConstant();
  HIntConstant* length = invoke->InputAt(4)->AsIntConstant();

  int num_constants =
      ((src_pos != nullptr) ? 1 : 0)
      + ((dest_pos != nullptr) ? 1 : 0)
      + ((length != nullptr) ? 1 : 0);

  if (num_constants < 2) {
    // Not enough free registers.
    return;
  }

  // As long as we are checking, we might as well check to see if the src and dest
  // positions are >= 0.
  if ((src_pos != nullptr && src_pos->GetValue() < 0) ||
      (dest_pos != nullptr && dest_pos->GetValue() < 0)) {
    // We will have to fail anyways.
    return;
  }

  // And since we are already checking, check the length too.
  if (length != nullptr) {
    int32_t len = length->GetValue();
    if (len < 0) {
      // Just call as normal.
      return;
    }
  }

  // Okay, it is safe to generate inline code.
  LocationSummary* locations =
    new (arena_) LocationSummary(invoke, LocationSummary::kCallOnSlowPath, kIntrinsified);
  // arraycopy(Object src, int srcPos, Object dest, int destPos, int length).
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(invoke->InputAt(1)));
  locations->SetInAt(2, Location::RequiresRegister());
  locations->SetInAt(3, Location::RegisterOrConstant(invoke->InputAt(3)));
  locations->SetInAt(4, Location::RegisterOrConstant(invoke->InputAt(4)));

  // And we need some temporaries.  We will use REP MOVSW, so we need fixed registers.
  locations->AddTemp(Location::RegisterLocation(ESI));
  locations->AddTemp(Location::RegisterLocation(EDI));
  locations->AddTemp(Location::RegisterLocation(ECX));
}

static void CheckPosition(X86Assembler* assembler,
                          Location pos,
                          Register input,
                          Register length,
                          SlowPathCode* slow_path,
                          Register input_len,
                          Register temp) {
  // Where is the length in the String?
  const uint32_t length_offset = mirror::Array::LengthOffset().Uint32Value();

  if (pos.IsConstant()) {
    int32_t pos_const = pos.GetConstant()->AsIntConstant()->GetValue();
    if (pos_const == 0) {
      // Check that length(input) >= length.
      __ cmpl(Address(input, length_offset), length);
      __ j(kLess, slow_path->GetEntryLabel());
    } else {
      // Check that length(input) >= pos.
      __ movl(input_len, Address(input, length_offset));
      __ cmpl(input_len, Immediate(pos_const));
      __ j(kLess, slow_path->GetEntryLabel());

      // Check that (length(input) - pos) >= length.
      __ leal(temp, Address(input_len, -pos_const));
      __ cmpl(temp, length);
      __ j(kLess, slow_path->GetEntryLabel());
    }
  } else {
    // Check that pos >= 0.
    Register pos_reg = pos.AsRegister<Register>();
    __ testl(pos_reg, pos_reg);
    __ j(kLess, slow_path->GetEntryLabel());

    // Check that pos <= length(input).
    __ cmpl(Address(input, length_offset), pos_reg);
    __ j(kLess, slow_path->GetEntryLabel());

    // Check that (length(input) - pos) >= length.
    __ movl(temp, Address(input, length_offset));
    __ subl(temp, pos_reg);
    __ cmpl(temp, length);
    __ j(kLess, slow_path->GetEntryLabel());
  }
}

void IntrinsicCodeGeneratorX86::VisitSystemArrayCopyChar(HInvoke* invoke) {
  X86Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  Register src = locations->InAt(0).AsRegister<Register>();
  Location srcPos = locations->InAt(1);
  Register dest = locations->InAt(2).AsRegister<Register>();
  Location destPos = locations->InAt(3);
  Location length = locations->InAt(4);

  // Temporaries that we need for MOVSW.
  Register src_base = locations->GetTemp(0).AsRegister<Register>();
  DCHECK_EQ(src_base, ESI);
  Register dest_base = locations->GetTemp(1).AsRegister<Register>();
  DCHECK_EQ(dest_base, EDI);
  Register count = locations->GetTemp(2).AsRegister<Register>();
  DCHECK_EQ(count, ECX);

  SlowPathCode* slow_path = new (GetAllocator()) IntrinsicSlowPathX86(invoke);
  codegen_->AddSlowPath(slow_path);

  // Bail out if the source and destination are the same (to handle overlap).
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
    __ cmpl(length.AsRegister<Register>(), length.AsRegister<Register>());
    __ j(kLess, slow_path->GetEntryLabel());
  }

  // We need the count in ECX.
  if (length.IsConstant()) {
    __ movl(count, Immediate(length.GetConstant()->AsIntConstant()->GetValue()));
  } else {
    __ movl(count, length.AsRegister<Register>());
  }

  // Validity checks: source.
  CheckPosition(assembler, srcPos, src, count, slow_path, src_base, dest_base);

  // Validity checks: dest.
  CheckPosition(assembler, destPos, dest, count, slow_path, src_base, dest_base);

  // Okay, everything checks out.  Finally time to do the copy.
  // Check assumption that sizeof(Char) is 2 (used in scaling below).
  const size_t char_size = Primitive::ComponentSize(Primitive::kPrimChar);
  DCHECK_EQ(char_size, 2u);

  const uint32_t data_offset = mirror::Array::DataOffset(char_size).Uint32Value();

  if (srcPos.IsConstant()) {
    int32_t srcPos_const = srcPos.GetConstant()->AsIntConstant()->GetValue();
    __ leal(src_base, Address(src, char_size * srcPos_const + data_offset));
  } else {
    __ leal(src_base, Address(src, srcPos.AsRegister<Register>(),
                              ScaleFactor::TIMES_2, data_offset));
  }
  if (destPos.IsConstant()) {
    int32_t destPos_const = destPos.GetConstant()->AsIntConstant()->GetValue();

    __ leal(dest_base, Address(dest, char_size * destPos_const + data_offset));
  } else {
    __ leal(dest_base, Address(dest, destPos.AsRegister<Register>(),
                               ScaleFactor::TIMES_2, data_offset));
  }

  // Do the move.
  __ rep_movsw();

  __ Bind(slow_path->GetExitLabel());
}

void IntrinsicLocationsBuilderX86::VisitStringCompareTo(HInvoke* invoke) {
  // The inputs plus one temp.
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetOut(Location::RegisterLocation(EAX));
}

void IntrinsicCodeGeneratorX86::VisitStringCompareTo(HInvoke* invoke) {
  X86Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  Register argument = locations->InAt(1).AsRegister<Register>();
  __ testl(argument, argument);
  SlowPathCode* slow_path = new (GetAllocator()) IntrinsicSlowPathX86(invoke);
  codegen_->AddSlowPath(slow_path);
  __ j(kEqual, slow_path->GetEntryLabel());

  __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pStringCompareTo)));
  __ Bind(slow_path->GetExitLabel());
}

void IntrinsicLocationsBuilderX86::VisitStringEquals(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());

  // Request temporary registers, ECX and EDI needed for repe_cmpsl instruction.
  locations->AddTemp(Location::RegisterLocation(ECX));
  locations->AddTemp(Location::RegisterLocation(EDI));

  // Set output, ESI needed for repe_cmpsl instruction anyways.
  locations->SetOut(Location::RegisterLocation(ESI), Location::kOutputOverlap);
}

void IntrinsicCodeGeneratorX86::VisitStringEquals(HInvoke* invoke) {
  X86Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  Register str = locations->InAt(0).AsRegister<Register>();
  Register arg = locations->InAt(1).AsRegister<Register>();
  Register ecx = locations->GetTemp(0).AsRegister<Register>();
  Register edi = locations->GetTemp(1).AsRegister<Register>();
  Register esi = locations->Out().AsRegister<Register>();

  NearLabel end, return_true, return_false;

  // Get offsets of count, value, and class fields within a string object.
  const uint32_t count_offset = mirror::String::CountOffset().Uint32Value();
  const uint32_t value_offset = mirror::String::ValueOffset().Uint32Value();
  const uint32_t class_offset = mirror::Object::ClassOffset().Uint32Value();

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  StringEqualsOptimizations optimizations(invoke);
  if (!optimizations.GetArgumentNotNull()) {
    // Check if input is null, return false if it is.
    __ testl(arg, arg);
    __ j(kEqual, &return_false);
  }

  // Instanceof check for the argument by comparing class fields.
  // All string objects must have the same type since String cannot be subclassed.
  // Receiver must be a string object, so its class field is equal to all strings' class fields.
  // If the argument is a string object, its class field must be equal to receiver's class field.
  if (!optimizations.GetArgumentIsString()) {
    __ movl(ecx, Address(str, class_offset));
    __ cmpl(ecx, Address(arg, class_offset));
    __ j(kNotEqual, &return_false);
  }

  // Reference equality check, return true if same reference.
  __ cmpl(str, arg);
  __ j(kEqual, &return_true);

  // Load length of receiver string.
  __ movl(ecx, Address(str, count_offset));
  // Check if lengths are equal, return false if they're not.
  __ cmpl(ecx, Address(arg, count_offset));
  __ j(kNotEqual, &return_false);
  // Return true if both strings are empty.
  __ jecxz(&return_true);

  // Load starting addresses of string values into ESI/EDI as required for repe_cmpsl instruction.
  __ leal(esi, Address(str, value_offset));
  __ leal(edi, Address(arg, value_offset));

  // Divide string length by 2 to compare characters 2 at a time and adjust for odd lengths.
  __ addl(ecx, Immediate(1));
  __ shrl(ecx, Immediate(1));

  // Assertions that must hold in order to compare strings 2 characters at a time.
  DCHECK_ALIGNED(value_offset, 4);
  static_assert(IsAligned<4>(kObjectAlignment), "String of odd length is not zero padded");

  // Loop to compare strings two characters at a time starting at the beginning of the string.
  __ repe_cmpsl();
  // If strings are not equal, zero flag will be cleared.
  __ j(kNotEqual, &return_false);

  // Return true and exit the function.
  // If loop does not result in returning false, we return true.
  __ Bind(&return_true);
  __ movl(esi, Immediate(1));
  __ jmp(&end);

  // Return false and exit the function.
  __ Bind(&return_false);
  __ xorl(esi, esi);
  __ Bind(&end);
}

static void CreateStringIndexOfLocations(HInvoke* invoke,
                                         ArenaAllocator* allocator,
                                         bool start_at_zero) {
  LocationSummary* locations = new (allocator) LocationSummary(invoke,
                                                               LocationSummary::kCallOnSlowPath,
                                                               kIntrinsified);
  // The data needs to be in EDI for scasw. So request that the string is there, anyways.
  locations->SetInAt(0, Location::RegisterLocation(EDI));
  // If we look for a constant char, we'll still have to copy it into EAX. So just request the
  // allocator to do that, anyways. We can still do the constant check by checking the parameter
  // of the instruction explicitly.
  // Note: This works as we don't clobber EAX anywhere.
  locations->SetInAt(1, Location::RegisterLocation(EAX));
  if (!start_at_zero) {
    locations->SetInAt(2, Location::RequiresRegister());          // The starting index.
  }
  // As we clobber EDI during execution anyways, also use it as the output.
  locations->SetOut(Location::SameAsFirstInput());

  // repne scasw uses ECX as the counter.
  locations->AddTemp(Location::RegisterLocation(ECX));
  // Need another temporary to be able to compute the result.
  locations->AddTemp(Location::RequiresRegister());
}

static void GenerateStringIndexOf(HInvoke* invoke,
                                  X86Assembler* assembler,
                                  CodeGeneratorX86* codegen,
                                  ArenaAllocator* allocator,
                                  bool start_at_zero) {
  LocationSummary* locations = invoke->GetLocations();

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  Register string_obj = locations->InAt(0).AsRegister<Register>();
  Register search_value = locations->InAt(1).AsRegister<Register>();
  Register counter = locations->GetTemp(0).AsRegister<Register>();
  Register string_length = locations->GetTemp(1).AsRegister<Register>();
  Register out = locations->Out().AsRegister<Register>();

  // Check our assumptions for registers.
  DCHECK_EQ(string_obj, EDI);
  DCHECK_EQ(search_value, EAX);
  DCHECK_EQ(counter, ECX);
  DCHECK_EQ(out, EDI);

  // Check for code points > 0xFFFF. Either a slow-path check when we don't know statically,
  // or directly dispatch if we have a constant.
  SlowPathCode* slow_path = nullptr;
  if (invoke->InputAt(1)->IsIntConstant()) {
    if (static_cast<uint32_t>(invoke->InputAt(1)->AsIntConstant()->GetValue()) >
    std::numeric_limits<uint16_t>::max()) {
      // Always needs the slow-path. We could directly dispatch to it, but this case should be
      // rare, so for simplicity just put the full slow-path down and branch unconditionally.
      slow_path = new (allocator) IntrinsicSlowPathX86(invoke);
      codegen->AddSlowPath(slow_path);
      __ jmp(slow_path->GetEntryLabel());
      __ Bind(slow_path->GetExitLabel());
      return;
    }
  } else {
    __ cmpl(search_value, Immediate(std::numeric_limits<uint16_t>::max()));
    slow_path = new (allocator) IntrinsicSlowPathX86(invoke);
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

  // Do a zero-length check.
  // TODO: Support jecxz.
  NearLabel not_found_label;
  __ testl(string_length, string_length);
  __ j(kEqual, &not_found_label);

  if (start_at_zero) {
    // Number of chars to scan is the same as the string length.
    __ movl(counter, string_length);

    // Move to the start of the string.
    __ addl(string_obj, Immediate(value_offset));
  } else {
    Register start_index = locations->InAt(2).AsRegister<Register>();

    // Do a start_index check.
    __ cmpl(start_index, string_length);
    __ j(kGreaterEqual, &not_found_label);

    // Ensure we have a start index >= 0;
    __ xorl(counter, counter);
    __ cmpl(start_index, Immediate(0));
    __ cmovl(kGreater, counter, start_index);

    // Move to the start of the string: string_obj + value_offset + 2 * start_index.
    __ leal(string_obj, Address(string_obj, counter, ScaleFactor::TIMES_2, value_offset));

    // Now update ecx (the repne scasw work counter). We have string.length - start_index left to
    // compare.
    __ negl(counter);
    __ leal(counter, Address(string_length, counter, ScaleFactor::TIMES_1, 0));
  }

  // Everything is set up for repne scasw:
  //   * Comparison address in EDI.
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

void IntrinsicLocationsBuilderX86::VisitStringIndexOf(HInvoke* invoke) {
  CreateStringIndexOfLocations(invoke, arena_, /* start_at_zero */ true);
}

void IntrinsicCodeGeneratorX86::VisitStringIndexOf(HInvoke* invoke) {
  GenerateStringIndexOf(invoke, GetAssembler(), codegen_, GetAllocator(), /* start_at_zero */ true);
}

void IntrinsicLocationsBuilderX86::VisitStringIndexOfAfter(HInvoke* invoke) {
  CreateStringIndexOfLocations(invoke, arena_, /* start_at_zero */ false);
}

void IntrinsicCodeGeneratorX86::VisitStringIndexOfAfter(HInvoke* invoke) {
  GenerateStringIndexOf(
      invoke, GetAssembler(), codegen_, GetAllocator(), /* start_at_zero */ false);
}

void IntrinsicLocationsBuilderX86::VisitStringNewStringFromBytes(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(2, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  locations->SetInAt(3, Location::RegisterLocation(calling_convention.GetRegisterAt(3)));
  locations->SetOut(Location::RegisterLocation(EAX));
}

void IntrinsicCodeGeneratorX86::VisitStringNewStringFromBytes(HInvoke* invoke) {
  X86Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  Register byte_array = locations->InAt(0).AsRegister<Register>();
  __ testl(byte_array, byte_array);
  SlowPathCode* slow_path = new (GetAllocator()) IntrinsicSlowPathX86(invoke);
  codegen_->AddSlowPath(slow_path);
  __ j(kEqual, slow_path->GetEntryLabel());

  __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pAllocStringFromBytes)));
  CheckEntrypointTypes<kQuickAllocStringFromBytes, void*, void*, int32_t, int32_t, int32_t>();
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
  __ Bind(slow_path->GetExitLabel());
}

void IntrinsicLocationsBuilderX86::VisitStringNewStringFromChars(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(2, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  locations->SetOut(Location::RegisterLocation(EAX));
}

void IntrinsicCodeGeneratorX86::VisitStringNewStringFromChars(HInvoke* invoke) {
  X86Assembler* assembler = GetAssembler();

  // No need to emit code checking whether `locations->InAt(2)` is a null
  // pointer, as callers of the native method
  //
  //   java.lang.StringFactory.newStringFromChars(int offset, int charCount, char[] data)
  //
  // all include a null check on `data` before calling that method.
  __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pAllocStringFromChars)));
  CheckEntrypointTypes<kQuickAllocStringFromChars, void*, int32_t, int32_t, void*>();
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void IntrinsicLocationsBuilderX86::VisitStringNewStringFromString(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetOut(Location::RegisterLocation(EAX));
}

void IntrinsicCodeGeneratorX86::VisitStringNewStringFromString(HInvoke* invoke) {
  X86Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  Register string_to_copy = locations->InAt(0).AsRegister<Register>();
  __ testl(string_to_copy, string_to_copy);
  SlowPathCode* slow_path = new (GetAllocator()) IntrinsicSlowPathX86(invoke);
  codegen_->AddSlowPath(slow_path);
  __ j(kEqual, slow_path->GetEntryLabel());

  __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pAllocStringFromString)));
  CheckEntrypointTypes<kQuickAllocStringFromString, void*, void*>();
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
  __ Bind(slow_path->GetExitLabel());
}

void IntrinsicLocationsBuilderX86::VisitStringGetCharsNoCheck(HInvoke* invoke) {
  // public void getChars(int srcBegin, int srcEnd, char[] dst, int dstBegin);
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(invoke->InputAt(1)));
  // Place srcEnd in ECX to save a move below.
  locations->SetInAt(2, Location::RegisterLocation(ECX));
  locations->SetInAt(3, Location::RequiresRegister());
  locations->SetInAt(4, Location::RequiresRegister());

  // And we need some temporaries.  We will use REP MOVSW, so we need fixed registers.
  // We don't have enough registers to also grab ECX, so handle below.
  locations->AddTemp(Location::RegisterLocation(ESI));
  locations->AddTemp(Location::RegisterLocation(EDI));
}

void IntrinsicCodeGeneratorX86::VisitStringGetCharsNoCheck(HInvoke* invoke) {
  X86Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  size_t char_component_size = Primitive::ComponentSize(Primitive::kPrimChar);
  // Location of data in char array buffer.
  const uint32_t data_offset = mirror::Array::DataOffset(char_component_size).Uint32Value();
  // Location of char array data in string.
  const uint32_t value_offset = mirror::String::ValueOffset().Uint32Value();

  // public void getChars(int srcBegin, int srcEnd, char[] dst, int dstBegin);
  Register obj = locations->InAt(0).AsRegister<Register>();
  Location srcBegin = locations->InAt(1);
  int srcBegin_value =
    srcBegin.IsConstant() ? srcBegin.GetConstant()->AsIntConstant()->GetValue() : 0;
  Register srcEnd = locations->InAt(2).AsRegister<Register>();
  Register dst = locations->InAt(3).AsRegister<Register>();
  Register dstBegin = locations->InAt(4).AsRegister<Register>();

  // Check assumption that sizeof(Char) is 2 (used in scaling below).
  const size_t char_size = Primitive::ComponentSize(Primitive::kPrimChar);
  DCHECK_EQ(char_size, 2u);

  // Compute the address of the destination buffer.
  __ leal(EDI, Address(dst, dstBegin, ScaleFactor::TIMES_2, data_offset));

  // Compute the address of the source string.
  if (srcBegin.IsConstant()) {
    // Compute the address of the source string by adding the number of chars from
    // the source beginning to the value offset of a string.
    __ leal(ESI, Address(obj, srcBegin_value * char_size + value_offset));
  } else {
    __ leal(ESI, Address(obj, srcBegin.AsRegister<Register>(),
                         ScaleFactor::TIMES_2, value_offset));
  }

  // Compute the number of chars (words) to move.
  // Now is the time to save ECX, since we don't know if it will be used later.
  __ pushl(ECX);
  int stack_adjust = kX86WordSize;
  __ cfi().AdjustCFAOffset(stack_adjust);
  DCHECK_EQ(srcEnd, ECX);
  if (srcBegin.IsConstant()) {
    if (srcBegin_value != 0) {
      __ subl(ECX, Immediate(srcBegin_value));
    }
  } else {
    DCHECK(srcBegin.IsRegister());
    __ subl(ECX, srcBegin.AsRegister<Register>());
  }

  // Do the move.
  __ rep_movsw();

  // And restore ECX.
  __ popl(ECX);
  __ cfi().AdjustCFAOffset(-stack_adjust);
}

static void GenPeek(LocationSummary* locations, Primitive::Type size, X86Assembler* assembler) {
  Register address = locations->InAt(0).AsRegisterPairLow<Register>();
  Location out_loc = locations->Out();
  // x86 allows unaligned access. We do not have to check the input or use specific instructions
  // to avoid a SIGBUS.
  switch (size) {
    case Primitive::kPrimByte:
      __ movsxb(out_loc.AsRegister<Register>(), Address(address, 0));
      break;
    case Primitive::kPrimShort:
      __ movsxw(out_loc.AsRegister<Register>(), Address(address, 0));
      break;
    case Primitive::kPrimInt:
      __ movl(out_loc.AsRegister<Register>(), Address(address, 0));
      break;
    case Primitive::kPrimLong:
      __ movl(out_loc.AsRegisterPairLow<Register>(), Address(address, 0));
      __ movl(out_loc.AsRegisterPairHigh<Register>(), Address(address, 4));
      break;
    default:
      LOG(FATAL) << "Type not recognized for peek: " << size;
      UNREACHABLE();
  }
}

void IntrinsicLocationsBuilderX86::VisitMemoryPeekByte(HInvoke* invoke) {
  CreateLongToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMemoryPeekByte(HInvoke* invoke) {
  GenPeek(invoke->GetLocations(), Primitive::kPrimByte, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMemoryPeekIntNative(HInvoke* invoke) {
  CreateLongToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMemoryPeekIntNative(HInvoke* invoke) {
  GenPeek(invoke->GetLocations(), Primitive::kPrimInt, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMemoryPeekLongNative(HInvoke* invoke) {
  CreateLongToLongLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMemoryPeekLongNative(HInvoke* invoke) {
  GenPeek(invoke->GetLocations(), Primitive::kPrimLong, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMemoryPeekShortNative(HInvoke* invoke) {
  CreateLongToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMemoryPeekShortNative(HInvoke* invoke) {
  GenPeek(invoke->GetLocations(), Primitive::kPrimShort, GetAssembler());
}

static void CreateLongIntToVoidLocations(ArenaAllocator* arena, Primitive::Type size,
                                         HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  HInstruction* value = invoke->InputAt(1);
  if (size == Primitive::kPrimByte) {
    locations->SetInAt(1, Location::ByteRegisterOrConstant(EDX, value));
  } else {
    locations->SetInAt(1, Location::RegisterOrConstant(value));
  }
}

static void GenPoke(LocationSummary* locations, Primitive::Type size, X86Assembler* assembler) {
  Register address = locations->InAt(0).AsRegisterPairLow<Register>();
  Location value_loc = locations->InAt(1);
  // x86 allows unaligned access. We do not have to check the input or use specific instructions
  // to avoid a SIGBUS.
  switch (size) {
    case Primitive::kPrimByte:
      if (value_loc.IsConstant()) {
        __ movb(Address(address, 0),
                Immediate(value_loc.GetConstant()->AsIntConstant()->GetValue()));
      } else {
        __ movb(Address(address, 0), value_loc.AsRegister<ByteRegister>());
      }
      break;
    case Primitive::kPrimShort:
      if (value_loc.IsConstant()) {
        __ movw(Address(address, 0),
                Immediate(value_loc.GetConstant()->AsIntConstant()->GetValue()));
      } else {
        __ movw(Address(address, 0), value_loc.AsRegister<Register>());
      }
      break;
    case Primitive::kPrimInt:
      if (value_loc.IsConstant()) {
        __ movl(Address(address, 0),
                Immediate(value_loc.GetConstant()->AsIntConstant()->GetValue()));
      } else {
        __ movl(Address(address, 0), value_loc.AsRegister<Register>());
      }
      break;
    case Primitive::kPrimLong:
      if (value_loc.IsConstant()) {
        int64_t value = value_loc.GetConstant()->AsLongConstant()->GetValue();
        __ movl(Address(address, 0), Immediate(Low32Bits(value)));
        __ movl(Address(address, 4), Immediate(High32Bits(value)));
      } else {
        __ movl(Address(address, 0), value_loc.AsRegisterPairLow<Register>());
        __ movl(Address(address, 4), value_loc.AsRegisterPairHigh<Register>());
      }
      break;
    default:
      LOG(FATAL) << "Type not recognized for poke: " << size;
      UNREACHABLE();
  }
}

void IntrinsicLocationsBuilderX86::VisitMemoryPokeByte(HInvoke* invoke) {
  CreateLongIntToVoidLocations(arena_, Primitive::kPrimByte, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMemoryPokeByte(HInvoke* invoke) {
  GenPoke(invoke->GetLocations(), Primitive::kPrimByte, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMemoryPokeIntNative(HInvoke* invoke) {
  CreateLongIntToVoidLocations(arena_, Primitive::kPrimInt, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMemoryPokeIntNative(HInvoke* invoke) {
  GenPoke(invoke->GetLocations(), Primitive::kPrimInt, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMemoryPokeLongNative(HInvoke* invoke) {
  CreateLongIntToVoidLocations(arena_, Primitive::kPrimLong, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMemoryPokeLongNative(HInvoke* invoke) {
  GenPoke(invoke->GetLocations(), Primitive::kPrimLong, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitMemoryPokeShortNative(HInvoke* invoke) {
  CreateLongIntToVoidLocations(arena_, Primitive::kPrimShort, invoke);
}

void IntrinsicCodeGeneratorX86::VisitMemoryPokeShortNative(HInvoke* invoke) {
  GenPoke(invoke->GetLocations(), Primitive::kPrimShort, GetAssembler());
}

void IntrinsicLocationsBuilderX86::VisitThreadCurrentThread(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetOut(Location::RequiresRegister());
}

void IntrinsicCodeGeneratorX86::VisitThreadCurrentThread(HInvoke* invoke) {
  Register out = invoke->GetLocations()->Out().AsRegister<Register>();
  GetAssembler()->fs()->movl(out, Address::Absolute(Thread::PeerOffset<kX86WordSize>()));
}

static void GenUnsafeGet(HInvoke* invoke,
                         Primitive::Type type,
                         bool is_volatile,
                         CodeGeneratorX86* codegen) {
  X86Assembler* assembler = down_cast<X86Assembler*>(codegen->GetAssembler());
  LocationSummary* locations = invoke->GetLocations();
  Location base_loc = locations->InAt(1);
  Register base = base_loc.AsRegister<Register>();
  Location offset_loc = locations->InAt(2);
  Register offset = offset_loc.AsRegisterPairLow<Register>();
  Location output_loc = locations->Out();

  switch (type) {
    case Primitive::kPrimInt: {
      Register output = output_loc.AsRegister<Register>();
      __ movl(output, Address(base, offset, ScaleFactor::TIMES_1, 0));
      break;
    }

    case Primitive::kPrimNot: {
      Register output = output_loc.AsRegister<Register>();
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

    case Primitive::kPrimLong: {
        Register output_lo = output_loc.AsRegisterPairLow<Register>();
        Register output_hi = output_loc.AsRegisterPairHigh<Register>();
        if (is_volatile) {
          // Need to use a XMM to read atomically.
          XmmRegister temp = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
          __ movsd(temp, Address(base, offset, ScaleFactor::TIMES_1, 0));
          __ movd(output_lo, temp);
          __ psrlq(temp, Immediate(32));
          __ movd(output_hi, temp);
        } else {
          __ movl(output_lo, Address(base, offset, ScaleFactor::TIMES_1, 0));
          __ movl(output_hi, Address(base, offset, ScaleFactor::TIMES_1, 4));
        }
      }
      break;

    default:
      LOG(FATAL) << "Unsupported op size " << type;
      UNREACHABLE();
  }
}

static void CreateIntIntIntToIntLocations(ArenaAllocator* arena,
                                          HInvoke* invoke,
                                          Primitive::Type type,
                                          bool is_volatile) {
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
  if (type == Primitive::kPrimLong) {
    if (is_volatile) {
      // Need to use XMM to read volatile.
      locations->AddTemp(Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresRegister());
    } else {
      locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
    }
  } else {
    locations->SetOut(Location::RequiresRegister());
  }
  if (type == Primitive::kPrimNot && kEmitCompilerReadBarrier && kUseBakerReadBarrier) {
    // We need a temporary register for the read barrier marking slow
    // path in InstructionCodeGeneratorX86::GenerateArrayLoadWithBakerReadBarrier.
    locations->AddTemp(Location::RequiresRegister());
  }
}

void IntrinsicLocationsBuilderX86::VisitUnsafeGet(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, Primitive::kPrimInt, /* is_volatile */ false);
}
void IntrinsicLocationsBuilderX86::VisitUnsafeGetVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, Primitive::kPrimInt, /* is_volatile */ true);
}
void IntrinsicLocationsBuilderX86::VisitUnsafeGetLong(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, Primitive::kPrimLong, /* is_volatile */ false);
}
void IntrinsicLocationsBuilderX86::VisitUnsafeGetLongVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, Primitive::kPrimLong, /* is_volatile */ true);
}
void IntrinsicLocationsBuilderX86::VisitUnsafeGetObject(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, Primitive::kPrimNot, /* is_volatile */ false);
}
void IntrinsicLocationsBuilderX86::VisitUnsafeGetObjectVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke, Primitive::kPrimNot, /* is_volatile */ true);
}


void IntrinsicCodeGeneratorX86::VisitUnsafeGet(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimInt, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafeGetVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimInt, /* is_volatile */ true, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafeGetLong(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimLong, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafeGetLongVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimLong, /* is_volatile */ true, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafeGetObject(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimNot, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafeGetObjectVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimNot, /* is_volatile */ true, codegen_);
}


static void CreateIntIntIntIntToVoidPlusTempsLocations(ArenaAllocator* arena,
                                                       Primitive::Type type,
                                                       HInvoke* invoke,
                                                       bool is_volatile) {
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
    // Ensure the value is in a byte register.
    locations->AddTemp(Location::RegisterLocation(ECX));
  } else if (type == Primitive::kPrimLong && is_volatile) {
    locations->AddTemp(Location::RequiresFpuRegister());
    locations->AddTemp(Location::RequiresFpuRegister());
  }
}

void IntrinsicLocationsBuilderX86::VisitUnsafePut(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(
      arena_, Primitive::kPrimInt, invoke, /* is_volatile */ false);
}
void IntrinsicLocationsBuilderX86::VisitUnsafePutOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(
      arena_, Primitive::kPrimInt, invoke, /* is_volatile */ false);
}
void IntrinsicLocationsBuilderX86::VisitUnsafePutVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(
      arena_, Primitive::kPrimInt, invoke, /* is_volatile */ true);
}
void IntrinsicLocationsBuilderX86::VisitUnsafePutObject(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(
      arena_, Primitive::kPrimNot, invoke, /* is_volatile */ false);
}
void IntrinsicLocationsBuilderX86::VisitUnsafePutObjectOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(
      arena_, Primitive::kPrimNot, invoke, /* is_volatile */ false);
}
void IntrinsicLocationsBuilderX86::VisitUnsafePutObjectVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(
      arena_, Primitive::kPrimNot, invoke, /* is_volatile */ true);
}
void IntrinsicLocationsBuilderX86::VisitUnsafePutLong(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(
      arena_, Primitive::kPrimLong, invoke, /* is_volatile */ false);
}
void IntrinsicLocationsBuilderX86::VisitUnsafePutLongOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(
      arena_, Primitive::kPrimLong, invoke, /* is_volatile */ false);
}
void IntrinsicLocationsBuilderX86::VisitUnsafePutLongVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(
      arena_, Primitive::kPrimLong, invoke, /* is_volatile */ true);
}

// We don't care for ordered: it requires an AnyStore barrier, which is already given by the x86
// memory model.
static void GenUnsafePut(LocationSummary* locations,
                         Primitive::Type type,
                         bool is_volatile,
                         CodeGeneratorX86* codegen) {
  X86Assembler* assembler = down_cast<X86Assembler*>(codegen->GetAssembler());
  Register base = locations->InAt(1).AsRegister<Register>();
  Register offset = locations->InAt(2).AsRegisterPairLow<Register>();
  Location value_loc = locations->InAt(3);

  if (type == Primitive::kPrimLong) {
    Register value_lo = value_loc.AsRegisterPairLow<Register>();
    Register value_hi = value_loc.AsRegisterPairHigh<Register>();
    if (is_volatile) {
      XmmRegister temp1 = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
      XmmRegister temp2 = locations->GetTemp(1).AsFpuRegister<XmmRegister>();
      __ movd(temp1, value_lo);
      __ movd(temp2, value_hi);
      __ punpckldq(temp1, temp2);
      __ movsd(Address(base, offset, ScaleFactor::TIMES_1, 0), temp1);
    } else {
      __ movl(Address(base, offset, ScaleFactor::TIMES_1, 0), value_lo);
      __ movl(Address(base, offset, ScaleFactor::TIMES_1, 4), value_hi);
    }
  } else if (kPoisonHeapReferences && type == Primitive::kPrimNot) {
    Register temp = locations->GetTemp(0).AsRegister<Register>();
    __ movl(temp, value_loc.AsRegister<Register>());
    __ PoisonHeapReference(temp);
    __ movl(Address(base, offset, ScaleFactor::TIMES_1, 0), temp);
  } else {
    __ movl(Address(base, offset, ScaleFactor::TIMES_1, 0), value_loc.AsRegister<Register>());
  }

  if (is_volatile) {
    codegen->MemoryFence();
  }

  if (type == Primitive::kPrimNot) {
    bool value_can_be_null = true;  // TODO: Worth finding out this information?
    codegen->MarkGCCard(locations->GetTemp(0).AsRegister<Register>(),
                        locations->GetTemp(1).AsRegister<Register>(),
                        base,
                        value_loc.AsRegister<Register>(),
                        value_can_be_null);
  }
}

void IntrinsicCodeGeneratorX86::VisitUnsafePut(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimInt, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafePutOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimInt, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafePutVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimInt, /* is_volatile */ true, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafePutObject(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimNot, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafePutObjectOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimNot, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafePutObjectVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimNot, /* is_volatile */ true, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafePutLong(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimLong, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafePutLongOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimLong, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorX86::VisitUnsafePutLongVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimLong, /* is_volatile */ true, codegen_);
}

static void CreateIntIntIntIntIntToInt(ArenaAllocator* arena, Primitive::Type type,
                                       HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::NoLocation());        // Unused receiver.
  locations->SetInAt(1, Location::RequiresRegister());
  // Offset is a long, but in 32 bit mode, we only need the low word.
  // Can we update the invoke here to remove a TypeConvert to Long?
  locations->SetInAt(2, Location::RequiresRegister());
  // Expected value must be in EAX or EDX:EAX.
  // For long, new value must be in ECX:EBX.
  if (type == Primitive::kPrimLong) {
    locations->SetInAt(3, Location::RegisterPairLocation(EAX, EDX));
    locations->SetInAt(4, Location::RegisterPairLocation(EBX, ECX));
  } else {
    locations->SetInAt(3, Location::RegisterLocation(EAX));
    locations->SetInAt(4, Location::RequiresRegister());
  }

  // Force a byte register for the output.
  locations->SetOut(Location::RegisterLocation(EAX));
  if (type == Primitive::kPrimNot) {
    // Need temp registers for card-marking.
    locations->AddTemp(Location::RequiresRegister());  // Possibly used for reference poisoning too.
    // Need a byte register for marking.
    locations->AddTemp(Location::RegisterLocation(ECX));
  }
}

void IntrinsicLocationsBuilderX86::VisitUnsafeCASInt(HInvoke* invoke) {
  CreateIntIntIntIntIntToInt(arena_, Primitive::kPrimInt, invoke);
}

void IntrinsicLocationsBuilderX86::VisitUnsafeCASLong(HInvoke* invoke) {
  CreateIntIntIntIntIntToInt(arena_, Primitive::kPrimLong, invoke);
}

void IntrinsicLocationsBuilderX86::VisitUnsafeCASObject(HInvoke* invoke) {
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

static void GenCAS(Primitive::Type type, HInvoke* invoke, CodeGeneratorX86* codegen) {
  X86Assembler* assembler = down_cast<X86Assembler*>(codegen->GetAssembler());
  LocationSummary* locations = invoke->GetLocations();

  Register base = locations->InAt(1).AsRegister<Register>();
  Register offset = locations->InAt(2).AsRegisterPairLow<Register>();
  Location out = locations->Out();
  DCHECK_EQ(out.AsRegister<Register>(), EAX);

  if (type == Primitive::kPrimNot) {
    Register expected = locations->InAt(3).AsRegister<Register>();
    // Ensure `expected` is in EAX (required by the CMPXCHG instruction).
    DCHECK_EQ(expected, EAX);
    Register value = locations->InAt(4).AsRegister<Register>();

    // Mark card for object assuming new value is stored.
    bool value_can_be_null = true;  // TODO: Worth finding out this information?
    codegen->MarkGCCard(locations->GetTemp(0).AsRegister<Register>(),
                        locations->GetTemp(1).AsRegister<Register>(),
                        base,
                        value,
                        value_can_be_null);

    bool base_equals_value = (base == value);
    if (kPoisonHeapReferences) {
      if (base_equals_value) {
        // If `base` and `value` are the same register location, move
        // `value` to a temporary register.  This way, poisoning
        // `value` won't invalidate `base`.
        value = locations->GetTemp(0).AsRegister<Register>();
        __ movl(value, base);
      }

      // Check that the register allocator did not assign the location
      // of `expected` (EAX) to `value` nor to `base`, so that heap
      // poisoning (when enabled) works as intended below.
      // - If `value` were equal to `expected`, both references would
      //   be poisoned twice, meaning they would not be poisoned at
      //   all, as heap poisoning uses address negation.
      // - If `base` were equal to `expected`, poisoning `expected`
      //   would invalidate `base`.
      DCHECK_NE(value, expected);
      DCHECK_NE(base, expected);

      __ PoisonHeapReference(expected);
      __ PoisonHeapReference(value);
    }

    // TODO: Add a read barrier for the reference stored in the object
    // before attempting the CAS, similar to the one in the
    // art::Unsafe_compareAndSwapObject JNI implementation.
    //
    // Note that this code is not (yet) used when read barriers are
    // enabled (see IntrinsicLocationsBuilderX86::VisitUnsafeCASObject).
    DCHECK(!kEmitCompilerReadBarrier);
    __ LockCmpxchgl(Address(base, offset, TIMES_1, 0), value);

    // LOCK CMPXCHG has full barrier semantics, and we don't need
    // scheduling barriers at this time.

    // Convert ZF into the boolean result.
    __ setb(kZero, out.AsRegister<Register>());
    __ movzxb(out.AsRegister<Register>(), out.AsRegister<ByteRegister>());

    // If heap poisoning is enabled, we need to unpoison the values
    // that were poisoned earlier.
    if (kPoisonHeapReferences) {
      if (base_equals_value) {
        // `value` has been moved to a temporary register, no need to
        // unpoison it.
      } else {
        // Ensure `value` is different from `out`, so that unpoisoning
        // the former does not invalidate the latter.
        DCHECK_NE(value, out.AsRegister<Register>());
        __ UnpoisonHeapReference(value);
      }
      // Do not unpoison the reference contained in register
      // `expected`, as it is the same as register `out` (EAX).
    }
  } else {
    if (type == Primitive::kPrimInt) {
      // Ensure the expected value is in EAX (required by the CMPXCHG
      // instruction).
      DCHECK_EQ(locations->InAt(3).AsRegister<Register>(), EAX);
      __ LockCmpxchgl(Address(base, offset, TIMES_1, 0),
                      locations->InAt(4).AsRegister<Register>());
    } else if (type == Primitive::kPrimLong) {
      // Ensure the expected value is in EAX:EDX and that the new
      // value is in EBX:ECX (required by the CMPXCHG8B instruction).
      DCHECK_EQ(locations->InAt(3).AsRegisterPairLow<Register>(), EAX);
      DCHECK_EQ(locations->InAt(3).AsRegisterPairHigh<Register>(), EDX);
      DCHECK_EQ(locations->InAt(4).AsRegisterPairLow<Register>(), EBX);
      DCHECK_EQ(locations->InAt(4).AsRegisterPairHigh<Register>(), ECX);
      __ LockCmpxchg8b(Address(base, offset, TIMES_1, 0));
    } else {
      LOG(FATAL) << "Unexpected CAS type " << type;
    }

    // LOCK CMPXCHG/LOCK CMPXCHG8B have full barrier semantics, and we
    // don't need scheduling barriers at this time.

    // Convert ZF into the boolean result.
    __ setb(kZero, out.AsRegister<Register>());
    __ movzxb(out.AsRegister<Register>(), out.AsRegister<ByteRegister>());
  }
}

void IntrinsicCodeGeneratorX86::VisitUnsafeCASInt(HInvoke* invoke) {
  GenCAS(Primitive::kPrimInt, invoke, codegen_);
}

void IntrinsicCodeGeneratorX86::VisitUnsafeCASLong(HInvoke* invoke) {
  GenCAS(Primitive::kPrimLong, invoke, codegen_);
}

void IntrinsicCodeGeneratorX86::VisitUnsafeCASObject(HInvoke* invoke) {
  GenCAS(Primitive::kPrimNot, invoke, codegen_);
}

void IntrinsicLocationsBuilderX86::VisitIntegerReverse(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
  locations->AddTemp(Location::RequiresRegister());
}

static void SwapBits(Register reg, Register temp, int32_t shift, int32_t mask,
                     X86Assembler* assembler) {
  Immediate imm_shift(shift);
  Immediate imm_mask(mask);
  __ movl(temp, reg);
  __ shrl(reg, imm_shift);
  __ andl(temp, imm_mask);
  __ andl(reg, imm_mask);
  __ shll(temp, imm_shift);
  __ orl(reg, temp);
}

void IntrinsicCodeGeneratorX86::VisitIntegerReverse(HInvoke* invoke) {
  X86Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  Register reg = locations->InAt(0).AsRegister<Register>();
  Register temp = locations->GetTemp(0).AsRegister<Register>();

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

void IntrinsicLocationsBuilderX86::VisitLongReverse(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
  locations->AddTemp(Location::RequiresRegister());
}

void IntrinsicCodeGeneratorX86::VisitLongReverse(HInvoke* invoke) {
  X86Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  Register reg_low = locations->InAt(0).AsRegisterPairLow<Register>();
  Register reg_high = locations->InAt(0).AsRegisterPairHigh<Register>();
  Register temp = locations->GetTemp(0).AsRegister<Register>();

  // We want to swap high/low, then bswap each one, and then do the same
  // as a 32 bit reverse.
  // Exchange high and low.
  __ movl(temp, reg_low);
  __ movl(reg_low, reg_high);
  __ movl(reg_high, temp);

  // bit-reverse low
  __ bswapl(reg_low);
  SwapBits(reg_low, temp, 1, 0x55555555, assembler);
  SwapBits(reg_low, temp, 2, 0x33333333, assembler);
  SwapBits(reg_low, temp, 4, 0x0f0f0f0f, assembler);

  // bit-reverse high
  __ bswapl(reg_high);
  SwapBits(reg_high, temp, 1, 0x55555555, assembler);
  SwapBits(reg_high, temp, 2, 0x33333333, assembler);
  SwapBits(reg_high, temp, 4, 0x0f0f0f0f, assembler);
}

static void CreateBitCountLocations(
    ArenaAllocator* arena, CodeGeneratorX86* codegen, HInvoke* invoke, bool is_long) {
  if (!codegen->GetInstructionSetFeatures().HasPopCnt()) {
    // Do nothing if there is no popcnt support. This results in generating
    // a call for the intrinsic rather than direct code.
    return;
  }
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  if (is_long) {
    locations->AddTemp(Location::RequiresRegister());
  }
  locations->SetInAt(0, Location::Any());
  locations->SetOut(Location::RequiresRegister());
}

static void GenBitCount(X86Assembler* assembler,
                        CodeGeneratorX86* codegen,
                        HInvoke* invoke, bool is_long) {
  LocationSummary* locations = invoke->GetLocations();
  Location src = locations->InAt(0);
  Register out = locations->Out().AsRegister<Register>();

  if (invoke->InputAt(0)->IsConstant()) {
    // Evaluate this at compile time.
    int64_t value = Int64FromConstant(invoke->InputAt(0)->AsConstant());
    int32_t result = is_long
        ? POPCOUNT(static_cast<uint64_t>(value))
        : POPCOUNT(static_cast<uint32_t>(value));
    codegen->Load32BitValue(out, result);
    return;
  }

  // Handle the non-constant cases.
  if (!is_long) {
    if (src.IsRegister()) {
      __ popcntl(out, src.AsRegister<Register>());
    } else {
      DCHECK(src.IsStackSlot());
      __ popcntl(out, Address(ESP, src.GetStackIndex()));
    }
  } else {
    // The 64-bit case needs to worry about two parts.
    Register temp = locations->GetTemp(0).AsRegister<Register>();
    if (src.IsRegisterPair()) {
      __ popcntl(temp, src.AsRegisterPairLow<Register>());
      __ popcntl(out, src.AsRegisterPairHigh<Register>());
    } else {
      DCHECK(src.IsDoubleStackSlot());
      __ popcntl(temp, Address(ESP, src.GetStackIndex()));
      __ popcntl(out, Address(ESP, src.GetHighStackIndex(kX86WordSize)));
    }
    __ addl(out, temp);
  }
}

void IntrinsicLocationsBuilderX86::VisitIntegerBitCount(HInvoke* invoke) {
  CreateBitCountLocations(arena_, codegen_, invoke, /* is_long */ false);
}

void IntrinsicCodeGeneratorX86::VisitIntegerBitCount(HInvoke* invoke) {
  GenBitCount(GetAssembler(), codegen_, invoke, /* is_long */ false);
}

void IntrinsicLocationsBuilderX86::VisitLongBitCount(HInvoke* invoke) {
  CreateBitCountLocations(arena_, codegen_, invoke, /* is_long */ true);
}

void IntrinsicCodeGeneratorX86::VisitLongBitCount(HInvoke* invoke) {
  GenBitCount(GetAssembler(), codegen_, invoke, /* is_long */ true);
}

static void CreateLeadingZeroLocations(ArenaAllocator* arena, HInvoke* invoke, bool is_long) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  if (is_long) {
    locations->SetInAt(0, Location::RequiresRegister());
  } else {
    locations->SetInAt(0, Location::Any());
  }
  locations->SetOut(Location::RequiresRegister());
}

static void GenLeadingZeros(X86Assembler* assembler,
                            CodeGeneratorX86* codegen,
                            HInvoke* invoke, bool is_long) {
  LocationSummary* locations = invoke->GetLocations();
  Location src = locations->InAt(0);
  Register out = locations->Out().AsRegister<Register>();

  if (invoke->InputAt(0)->IsConstant()) {
    // Evaluate this at compile time.
    int64_t value = Int64FromConstant(invoke->InputAt(0)->AsConstant());
    if (value == 0) {
      value = is_long ? 64 : 32;
    } else {
      value = is_long ? CLZ(static_cast<uint64_t>(value)) : CLZ(static_cast<uint32_t>(value));
    }
    codegen->Load32BitValue(out, value);
    return;
  }

  // Handle the non-constant cases.
  if (!is_long) {
    if (src.IsRegister()) {
      __ bsrl(out, src.AsRegister<Register>());
    } else {
      DCHECK(src.IsStackSlot());
      __ bsrl(out, Address(ESP, src.GetStackIndex()));
    }

    // BSR sets ZF if the input was zero, and the output is undefined.
    NearLabel all_zeroes, done;
    __ j(kEqual, &all_zeroes);

    // Correct the result from BSR to get the final CLZ result.
    __ xorl(out, Immediate(31));
    __ jmp(&done);

    // Fix the zero case with the expected result.
    __ Bind(&all_zeroes);
    __ movl(out, Immediate(32));

    __ Bind(&done);
    return;
  }

  // 64 bit case needs to worry about both parts of the register.
  DCHECK(src.IsRegisterPair());
  Register src_lo = src.AsRegisterPairLow<Register>();
  Register src_hi = src.AsRegisterPairHigh<Register>();
  NearLabel handle_low, done, all_zeroes;

  // Is the high word zero?
  __ testl(src_hi, src_hi);
  __ j(kEqual, &handle_low);

  // High word is not zero. We know that the BSR result is defined in this case.
  __ bsrl(out, src_hi);

  // Correct the result from BSR to get the final CLZ result.
  __ xorl(out, Immediate(31));
  __ jmp(&done);

  // High word was zero.  We have to compute the low word count and add 32.
  __ Bind(&handle_low);
  __ bsrl(out, src_lo);
  __ j(kEqual, &all_zeroes);

  // We had a valid result.  Use an XOR to both correct the result and add 32.
  __ xorl(out, Immediate(63));
  __ jmp(&done);

  // All zero case.
  __ Bind(&all_zeroes);
  __ movl(out, Immediate(64));

  __ Bind(&done);
}

void IntrinsicLocationsBuilderX86::VisitIntegerNumberOfLeadingZeros(HInvoke* invoke) {
  CreateLeadingZeroLocations(arena_, invoke, /* is_long */ false);
}

void IntrinsicCodeGeneratorX86::VisitIntegerNumberOfLeadingZeros(HInvoke* invoke) {
  GenLeadingZeros(GetAssembler(), codegen_, invoke, /* is_long */ false);
}

void IntrinsicLocationsBuilderX86::VisitLongNumberOfLeadingZeros(HInvoke* invoke) {
  CreateLeadingZeroLocations(arena_, invoke, /* is_long */ true);
}

void IntrinsicCodeGeneratorX86::VisitLongNumberOfLeadingZeros(HInvoke* invoke) {
  GenLeadingZeros(GetAssembler(), codegen_, invoke, /* is_long */ true);
}

static void CreateTrailingZeroLocations(ArenaAllocator* arena, HInvoke* invoke, bool is_long) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  if (is_long) {
    locations->SetInAt(0, Location::RequiresRegister());
  } else {
    locations->SetInAt(0, Location::Any());
  }
  locations->SetOut(Location::RequiresRegister());
}

static void GenTrailingZeros(X86Assembler* assembler,
                             CodeGeneratorX86* codegen,
                             HInvoke* invoke, bool is_long) {
  LocationSummary* locations = invoke->GetLocations();
  Location src = locations->InAt(0);
  Register out = locations->Out().AsRegister<Register>();

  if (invoke->InputAt(0)->IsConstant()) {
    // Evaluate this at compile time.
    int64_t value = Int64FromConstant(invoke->InputAt(0)->AsConstant());
    if (value == 0) {
      value = is_long ? 64 : 32;
    } else {
      value = is_long ? CTZ(static_cast<uint64_t>(value)) : CTZ(static_cast<uint32_t>(value));
    }
    codegen->Load32BitValue(out, value);
    return;
  }

  // Handle the non-constant cases.
  if (!is_long) {
    if (src.IsRegister()) {
      __ bsfl(out, src.AsRegister<Register>());
    } else {
      DCHECK(src.IsStackSlot());
      __ bsfl(out, Address(ESP, src.GetStackIndex()));
    }

    // BSF sets ZF if the input was zero, and the output is undefined.
    NearLabel done;
    __ j(kNotEqual, &done);

    // Fix the zero case with the expected result.
    __ movl(out, Immediate(32));

    __ Bind(&done);
    return;
  }

  // 64 bit case needs to worry about both parts of the register.
  DCHECK(src.IsRegisterPair());
  Register src_lo = src.AsRegisterPairLow<Register>();
  Register src_hi = src.AsRegisterPairHigh<Register>();
  NearLabel done, all_zeroes;

  // If the low word is zero, then ZF will be set.  If not, we have the answer.
  __ bsfl(out, src_lo);
  __ j(kNotEqual, &done);

  // Low word was zero.  We have to compute the high word count and add 32.
  __ bsfl(out, src_hi);
  __ j(kEqual, &all_zeroes);

  // We had a valid result.  Add 32 to account for the low word being zero.
  __ addl(out, Immediate(32));
  __ jmp(&done);

  // All zero case.
  __ Bind(&all_zeroes);
  __ movl(out, Immediate(64));

  __ Bind(&done);
}

void IntrinsicLocationsBuilderX86::VisitIntegerNumberOfTrailingZeros(HInvoke* invoke) {
  CreateTrailingZeroLocations(arena_, invoke, /* is_long */ false);
}

void IntrinsicCodeGeneratorX86::VisitIntegerNumberOfTrailingZeros(HInvoke* invoke) {
  GenTrailingZeros(GetAssembler(), codegen_, invoke, /* is_long */ false);
}

void IntrinsicLocationsBuilderX86::VisitLongNumberOfTrailingZeros(HInvoke* invoke) {
  CreateTrailingZeroLocations(arena_, invoke, /* is_long */ true);
}

void IntrinsicCodeGeneratorX86::VisitLongNumberOfTrailingZeros(HInvoke* invoke) {
  GenTrailingZeros(GetAssembler(), codegen_, invoke, /* is_long */ true);
}

UNIMPLEMENTED_INTRINSIC(X86, MathRoundDouble)
UNIMPLEMENTED_INTRINSIC(X86, ReferenceGetReferent)
UNIMPLEMENTED_INTRINSIC(X86, SystemArrayCopy)
UNIMPLEMENTED_INTRINSIC(X86, FloatIsInfinite)
UNIMPLEMENTED_INTRINSIC(X86, DoubleIsInfinite)
UNIMPLEMENTED_INTRINSIC(X86, IntegerHighestOneBit)
UNIMPLEMENTED_INTRINSIC(X86, LongHighestOneBit)
UNIMPLEMENTED_INTRINSIC(X86, IntegerLowestOneBit)
UNIMPLEMENTED_INTRINSIC(X86, LongLowestOneBit)

// 1.8.
UNIMPLEMENTED_INTRINSIC(X86, UnsafeGetAndAddInt)
UNIMPLEMENTED_INTRINSIC(X86, UnsafeGetAndAddLong)
UNIMPLEMENTED_INTRINSIC(X86, UnsafeGetAndSetInt)
UNIMPLEMENTED_INTRINSIC(X86, UnsafeGetAndSetLong)
UNIMPLEMENTED_INTRINSIC(X86, UnsafeGetAndSetObject)

UNREACHABLE_INTRINSICS(X86)

#undef __

}  // namespace x86
}  // namespace art
