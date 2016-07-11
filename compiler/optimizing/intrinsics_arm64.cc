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

#include "intrinsics_arm64.h"

#include "arch/arm64/instruction_set_features_arm64.h"
#include "art_method.h"
#include "code_generator_arm64.h"
#include "common_arm64.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "intrinsics.h"
#include "mirror/array-inl.h"
#include "mirror/string.h"
#include "thread.h"
#include "utils/arm64/assembler_arm64.h"
#include "utils/arm64/constants_arm64.h"

#include "vixl/a64/disasm-a64.h"
#include "vixl/a64/macro-assembler-a64.h"

using namespace vixl;   // NOLINT(build/namespaces)

namespace art {

namespace arm64 {

using helpers::DRegisterFrom;
using helpers::FPRegisterFrom;
using helpers::HeapOperand;
using helpers::LocationFrom;
using helpers::OperandFrom;
using helpers::RegisterFrom;
using helpers::SRegisterFrom;
using helpers::WRegisterFrom;
using helpers::XRegisterFrom;
using helpers::InputRegisterAt;

namespace {

ALWAYS_INLINE inline MemOperand AbsoluteHeapOperandFrom(Location location, size_t offset = 0) {
  return MemOperand(XRegisterFrom(location), offset);
}

}  // namespace

vixl::MacroAssembler* IntrinsicCodeGeneratorARM64::GetVIXLAssembler() {
  return codegen_->GetAssembler()->vixl_masm_;
}

ArenaAllocator* IntrinsicCodeGeneratorARM64::GetAllocator() {
  return codegen_->GetGraph()->GetArena();
}

#define __ codegen->GetAssembler()->vixl_masm_->

static void MoveFromReturnRegister(Location trg,
                                   Primitive::Type type,
                                   CodeGeneratorARM64* codegen) {
  if (!trg.IsValid()) {
    DCHECK(type == Primitive::kPrimVoid);
    return;
  }

  DCHECK_NE(type, Primitive::kPrimVoid);

  if (Primitive::IsIntegralType(type) || type == Primitive::kPrimNot) {
    Register trg_reg = RegisterFrom(trg, type);
    Register res_reg = RegisterFrom(ARM64ReturnLocation(type), type);
    __ Mov(trg_reg, res_reg, kDiscardForSameWReg);
  } else {
    FPRegister trg_reg = FPRegisterFrom(trg, type);
    FPRegister res_reg = FPRegisterFrom(ARM64ReturnLocation(type), type);
    __ Fmov(trg_reg, res_reg);
  }
}

static void MoveArguments(HInvoke* invoke, CodeGeneratorARM64* codegen) {
  InvokeDexCallingConventionVisitorARM64 calling_convention_visitor;
  IntrinsicVisitor::MoveArguments(invoke, codegen, &calling_convention_visitor);
}

// Slow-path for fallback (calling the managed code to handle the intrinsic) in an intrinsified
// call. This will copy the arguments into the positions for a regular call.
//
// Note: The actual parameters are required to be in the locations given by the invoke's location
//       summary. If an intrinsic modifies those locations before a slowpath call, they must be
//       restored!
class IntrinsicSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit IntrinsicSlowPathARM64(HInvoke* invoke)
      : SlowPathCodeARM64(invoke), invoke_(invoke) { }

  void EmitNativeCode(CodeGenerator* codegen_in) OVERRIDE {
    CodeGeneratorARM64* codegen = down_cast<CodeGeneratorARM64*>(codegen_in);
    __ Bind(GetEntryLabel());

    SaveLiveRegisters(codegen, invoke_->GetLocations());

    MoveArguments(invoke_, codegen);

    if (invoke_->IsInvokeStaticOrDirect()) {
      codegen->GenerateStaticOrDirectCall(invoke_->AsInvokeStaticOrDirect(),
                                          LocationFrom(kArtMethodRegister));
    } else {
      codegen->GenerateVirtualCall(invoke_->AsInvokeVirtual(), LocationFrom(kArtMethodRegister));
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

  const char* GetDescription() const OVERRIDE { return "IntrinsicSlowPathARM64"; }

 private:
  // The instruction where this slow path is happening.
  HInvoke* const invoke_;

  DISALLOW_COPY_AND_ASSIGN(IntrinsicSlowPathARM64);
};

#undef __

bool IntrinsicLocationsBuilderARM64::TryDispatch(HInvoke* invoke) {
  Dispatch(invoke);
  LocationSummary* res = invoke->GetLocations();
  if (res == nullptr) {
    return false;
  }
  if (kEmitCompilerReadBarrier && res->CanCall()) {
    // Generating an intrinsic for this HInvoke may produce an
    // IntrinsicSlowPathARM64 slow path.  Currently this approach
    // does not work when using read barriers, as the emitted
    // calling sequence will make use of another slow path
    // (ReadBarrierForRootSlowPathARM64 for HInvokeStaticOrDirect,
    // ReadBarrierSlowPathARM64 for HInvokeVirtual).  So we bail
    // out in this case.
    //
    // TODO: Find a way to have intrinsics work with read barriers.
    invoke->SetLocations(nullptr);
    return false;
  }
  return res->Intrinsified();
}

#define __ masm->

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

static void MoveFPToInt(LocationSummary* locations, bool is64bit, vixl::MacroAssembler* masm) {
  Location input = locations->InAt(0);
  Location output = locations->Out();
  __ Fmov(is64bit ? XRegisterFrom(output) : WRegisterFrom(output),
          is64bit ? DRegisterFrom(input) : SRegisterFrom(input));
}

static void MoveIntToFP(LocationSummary* locations, bool is64bit, vixl::MacroAssembler* masm) {
  Location input = locations->InAt(0);
  Location output = locations->Out();
  __ Fmov(is64bit ? DRegisterFrom(output) : SRegisterFrom(output),
          is64bit ? XRegisterFrom(input) : WRegisterFrom(input));
}

void IntrinsicLocationsBuilderARM64::VisitDoubleDoubleToRawLongBits(HInvoke* invoke) {
  CreateFPToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderARM64::VisitDoubleLongBitsToDouble(HInvoke* invoke) {
  CreateIntToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitDoubleDoubleToRawLongBits(HInvoke* invoke) {
  MoveFPToInt(invoke->GetLocations(), /* is64bit */ true, GetVIXLAssembler());
}
void IntrinsicCodeGeneratorARM64::VisitDoubleLongBitsToDouble(HInvoke* invoke) {
  MoveIntToFP(invoke->GetLocations(), /* is64bit */ true, GetVIXLAssembler());
}

void IntrinsicLocationsBuilderARM64::VisitFloatFloatToRawIntBits(HInvoke* invoke) {
  CreateFPToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderARM64::VisitFloatIntBitsToFloat(HInvoke* invoke) {
  CreateIntToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitFloatFloatToRawIntBits(HInvoke* invoke) {
  MoveFPToInt(invoke->GetLocations(), /* is64bit */ false, GetVIXLAssembler());
}
void IntrinsicCodeGeneratorARM64::VisitFloatIntBitsToFloat(HInvoke* invoke) {
  MoveIntToFP(invoke->GetLocations(), /* is64bit */ false, GetVIXLAssembler());
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
                            vixl::MacroAssembler* masm) {
  Location in = locations->InAt(0);
  Location out = locations->Out();

  switch (type) {
    case Primitive::kPrimShort:
      __ Rev16(WRegisterFrom(out), WRegisterFrom(in));
      __ Sxth(WRegisterFrom(out), WRegisterFrom(out));
      break;
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      __ Rev(RegisterFrom(out, type), RegisterFrom(in, type));
      break;
    default:
      LOG(FATAL) << "Unexpected size for reverse-bytes: " << type;
      UNREACHABLE();
  }
}

void IntrinsicLocationsBuilderARM64::VisitIntegerReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitIntegerReverseBytes(HInvoke* invoke) {
  GenReverseBytes(invoke->GetLocations(), Primitive::kPrimInt, GetVIXLAssembler());
}

void IntrinsicLocationsBuilderARM64::VisitLongReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitLongReverseBytes(HInvoke* invoke) {
  GenReverseBytes(invoke->GetLocations(), Primitive::kPrimLong, GetVIXLAssembler());
}

void IntrinsicLocationsBuilderARM64::VisitShortReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitShortReverseBytes(HInvoke* invoke) {
  GenReverseBytes(invoke->GetLocations(), Primitive::kPrimShort, GetVIXLAssembler());
}

static void CreateIntIntToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

static void GenNumberOfLeadingZeros(LocationSummary* locations,
                                    Primitive::Type type,
                                    vixl::MacroAssembler* masm) {
  DCHECK(type == Primitive::kPrimInt || type == Primitive::kPrimLong);

  Location in = locations->InAt(0);
  Location out = locations->Out();

  __ Clz(RegisterFrom(out, type), RegisterFrom(in, type));
}

void IntrinsicLocationsBuilderARM64::VisitIntegerNumberOfLeadingZeros(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitIntegerNumberOfLeadingZeros(HInvoke* invoke) {
  GenNumberOfLeadingZeros(invoke->GetLocations(), Primitive::kPrimInt, GetVIXLAssembler());
}

void IntrinsicLocationsBuilderARM64::VisitLongNumberOfLeadingZeros(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitLongNumberOfLeadingZeros(HInvoke* invoke) {
  GenNumberOfLeadingZeros(invoke->GetLocations(), Primitive::kPrimLong, GetVIXLAssembler());
}

static void GenNumberOfTrailingZeros(LocationSummary* locations,
                                     Primitive::Type type,
                                     vixl::MacroAssembler* masm) {
  DCHECK(type == Primitive::kPrimInt || type == Primitive::kPrimLong);

  Location in = locations->InAt(0);
  Location out = locations->Out();

  __ Rbit(RegisterFrom(out, type), RegisterFrom(in, type));
  __ Clz(RegisterFrom(out, type), RegisterFrom(out, type));
}

void IntrinsicLocationsBuilderARM64::VisitIntegerNumberOfTrailingZeros(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitIntegerNumberOfTrailingZeros(HInvoke* invoke) {
  GenNumberOfTrailingZeros(invoke->GetLocations(), Primitive::kPrimInt, GetVIXLAssembler());
}

void IntrinsicLocationsBuilderARM64::VisitLongNumberOfTrailingZeros(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitLongNumberOfTrailingZeros(HInvoke* invoke) {
  GenNumberOfTrailingZeros(invoke->GetLocations(), Primitive::kPrimLong, GetVIXLAssembler());
}

static void GenReverse(LocationSummary* locations,
                       Primitive::Type type,
                       vixl::MacroAssembler* masm) {
  DCHECK(type == Primitive::kPrimInt || type == Primitive::kPrimLong);

  Location in = locations->InAt(0);
  Location out = locations->Out();

  __ Rbit(RegisterFrom(out, type), RegisterFrom(in, type));
}

void IntrinsicLocationsBuilderARM64::VisitIntegerReverse(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitIntegerReverse(HInvoke* invoke) {
  GenReverse(invoke->GetLocations(), Primitive::kPrimInt, GetVIXLAssembler());
}

void IntrinsicLocationsBuilderARM64::VisitLongReverse(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitLongReverse(HInvoke* invoke) {
  GenReverse(invoke->GetLocations(), Primitive::kPrimLong, GetVIXLAssembler());
}

static void GenBitCount(HInvoke* instr, Primitive::Type type, vixl::MacroAssembler* masm) {
  DCHECK(Primitive::IsIntOrLongType(type)) << type;
  DCHECK_EQ(instr->GetType(), Primitive::kPrimInt);
  DCHECK_EQ(Primitive::PrimitiveKind(instr->InputAt(0)->GetType()), type);

  UseScratchRegisterScope temps(masm);

  Register src = InputRegisterAt(instr, 0);
  Register dst = RegisterFrom(instr->GetLocations()->Out(), type);
  FPRegister fpr = (type == Primitive::kPrimLong) ? temps.AcquireD() : temps.AcquireS();

  __ Fmov(fpr, src);
  __ Cnt(fpr.V8B(), fpr.V8B());
  __ Addv(fpr.B(), fpr.V8B());
  __ Fmov(dst, fpr);
}

void IntrinsicLocationsBuilderARM64::VisitLongBitCount(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitLongBitCount(HInvoke* invoke) {
  GenBitCount(invoke, Primitive::kPrimLong, GetVIXLAssembler());
}

void IntrinsicLocationsBuilderARM64::VisitIntegerBitCount(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitIntegerBitCount(HInvoke* invoke) {
  GenBitCount(invoke, Primitive::kPrimInt, GetVIXLAssembler());
}

static void CreateFPToFPLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
}

static void MathAbsFP(LocationSummary* locations, bool is64bit, vixl::MacroAssembler* masm) {
  Location in = locations->InAt(0);
  Location out = locations->Out();

  FPRegister in_reg = is64bit ? DRegisterFrom(in) : SRegisterFrom(in);
  FPRegister out_reg = is64bit ? DRegisterFrom(out) : SRegisterFrom(out);

  __ Fabs(out_reg, in_reg);
}

void IntrinsicLocationsBuilderARM64::VisitMathAbsDouble(HInvoke* invoke) {
  CreateFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathAbsDouble(HInvoke* invoke) {
  MathAbsFP(invoke->GetLocations(), /* is64bit */ true, GetVIXLAssembler());
}

void IntrinsicLocationsBuilderARM64::VisitMathAbsFloat(HInvoke* invoke) {
  CreateFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathAbsFloat(HInvoke* invoke) {
  MathAbsFP(invoke->GetLocations(), /* is64bit */ false, GetVIXLAssembler());
}

static void CreateIntToInt(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

static void GenAbsInteger(LocationSummary* locations,
                          bool is64bit,
                          vixl::MacroAssembler* masm) {
  Location in = locations->InAt(0);
  Location output = locations->Out();

  Register in_reg = is64bit ? XRegisterFrom(in) : WRegisterFrom(in);
  Register out_reg = is64bit ? XRegisterFrom(output) : WRegisterFrom(output);

  __ Cmp(in_reg, Operand(0));
  __ Cneg(out_reg, in_reg, lt);
}

void IntrinsicLocationsBuilderARM64::VisitMathAbsInt(HInvoke* invoke) {
  CreateIntToInt(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathAbsInt(HInvoke* invoke) {
  GenAbsInteger(invoke->GetLocations(), /* is64bit */ false, GetVIXLAssembler());
}

void IntrinsicLocationsBuilderARM64::VisitMathAbsLong(HInvoke* invoke) {
  CreateIntToInt(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathAbsLong(HInvoke* invoke) {
  GenAbsInteger(invoke->GetLocations(), /* is64bit */ true, GetVIXLAssembler());
}

static void GenMinMaxFP(LocationSummary* locations,
                        bool is_min,
                        bool is_double,
                        vixl::MacroAssembler* masm) {
  Location op1 = locations->InAt(0);
  Location op2 = locations->InAt(1);
  Location out = locations->Out();

  FPRegister op1_reg = is_double ? DRegisterFrom(op1) : SRegisterFrom(op1);
  FPRegister op2_reg = is_double ? DRegisterFrom(op2) : SRegisterFrom(op2);
  FPRegister out_reg = is_double ? DRegisterFrom(out) : SRegisterFrom(out);
  if (is_min) {
    __ Fmin(out_reg, op1_reg, op2_reg);
  } else {
    __ Fmax(out_reg, op1_reg, op2_reg);
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

void IntrinsicLocationsBuilderARM64::VisitMathMinDoubleDouble(HInvoke* invoke) {
  CreateFPFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathMinDoubleDouble(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(), /* is_min */ true, /* is_double */ true, GetVIXLAssembler());
}

void IntrinsicLocationsBuilderARM64::VisitMathMinFloatFloat(HInvoke* invoke) {
  CreateFPFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathMinFloatFloat(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(), /* is_min */ true, /* is_double */ false, GetVIXLAssembler());
}

void IntrinsicLocationsBuilderARM64::VisitMathMaxDoubleDouble(HInvoke* invoke) {
  CreateFPFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathMaxDoubleDouble(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(), /* is_min */ false, /* is_double */ true, GetVIXLAssembler());
}

void IntrinsicLocationsBuilderARM64::VisitMathMaxFloatFloat(HInvoke* invoke) {
  CreateFPFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathMaxFloatFloat(HInvoke* invoke) {
  GenMinMaxFP(
      invoke->GetLocations(), /* is_min */ false, /* is_double */ false, GetVIXLAssembler());
}

static void GenMinMax(LocationSummary* locations,
                      bool is_min,
                      bool is_long,
                      vixl::MacroAssembler* masm) {
  Location op1 = locations->InAt(0);
  Location op2 = locations->InAt(1);
  Location out = locations->Out();

  Register op1_reg = is_long ? XRegisterFrom(op1) : WRegisterFrom(op1);
  Register op2_reg = is_long ? XRegisterFrom(op2) : WRegisterFrom(op2);
  Register out_reg = is_long ? XRegisterFrom(out) : WRegisterFrom(out);

  __ Cmp(op1_reg, op2_reg);
  __ Csel(out_reg, op1_reg, op2_reg, is_min ? lt : gt);
}

void IntrinsicLocationsBuilderARM64::VisitMathMinIntInt(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathMinIntInt(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), /* is_min */ true, /* is_long */ false, GetVIXLAssembler());
}

void IntrinsicLocationsBuilderARM64::VisitMathMinLongLong(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathMinLongLong(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), /* is_min */ true, /* is_long */ true, GetVIXLAssembler());
}

void IntrinsicLocationsBuilderARM64::VisitMathMaxIntInt(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathMaxIntInt(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), /* is_min */ false, /* is_long */ false, GetVIXLAssembler());
}

void IntrinsicLocationsBuilderARM64::VisitMathMaxLongLong(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathMaxLongLong(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), /* is_min */ false, /* is_long */ true, GetVIXLAssembler());
}

void IntrinsicLocationsBuilderARM64::VisitMathSqrt(HInvoke* invoke) {
  CreateFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathSqrt(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  vixl::MacroAssembler* masm = GetVIXLAssembler();
  __ Fsqrt(DRegisterFrom(locations->Out()), DRegisterFrom(locations->InAt(0)));
}

void IntrinsicLocationsBuilderARM64::VisitMathCeil(HInvoke* invoke) {
  CreateFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathCeil(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  vixl::MacroAssembler* masm = GetVIXLAssembler();
  __ Frintp(DRegisterFrom(locations->Out()), DRegisterFrom(locations->InAt(0)));
}

void IntrinsicLocationsBuilderARM64::VisitMathFloor(HInvoke* invoke) {
  CreateFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathFloor(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  vixl::MacroAssembler* masm = GetVIXLAssembler();
  __ Frintm(DRegisterFrom(locations->Out()), DRegisterFrom(locations->InAt(0)));
}

void IntrinsicLocationsBuilderARM64::VisitMathRint(HInvoke* invoke) {
  CreateFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathRint(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  vixl::MacroAssembler* masm = GetVIXLAssembler();
  __ Frintn(DRegisterFrom(locations->Out()), DRegisterFrom(locations->InAt(0)));
}

static void CreateFPToIntPlusTempLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetOut(Location::RequiresRegister());
}

static void GenMathRound(LocationSummary* locations,
                         bool is_double,
                         vixl::MacroAssembler* masm) {
  FPRegister in_reg = is_double ?
      DRegisterFrom(locations->InAt(0)) : SRegisterFrom(locations->InAt(0));
  Register out_reg = is_double ?
      XRegisterFrom(locations->Out()) : WRegisterFrom(locations->Out());
  UseScratchRegisterScope temps(masm);
  FPRegister temp1_reg = temps.AcquireSameSizeAs(in_reg);

  // 0.5 can be encoded as an immediate, so use fmov.
  if (is_double) {
    __ Fmov(temp1_reg, static_cast<double>(0.5));
  } else {
    __ Fmov(temp1_reg, static_cast<float>(0.5));
  }
  __ Fadd(temp1_reg, in_reg, temp1_reg);
  __ Fcvtms(out_reg, temp1_reg);
}

void IntrinsicLocationsBuilderARM64::VisitMathRoundDouble(HInvoke* invoke) {
  // See intrinsics.h.
  if (kRoundIsPlusPointFive) {
    CreateFPToIntPlusTempLocations(arena_, invoke);
  }
}

void IntrinsicCodeGeneratorARM64::VisitMathRoundDouble(HInvoke* invoke) {
  GenMathRound(invoke->GetLocations(), /* is_double */ true, GetVIXLAssembler());
}

void IntrinsicLocationsBuilderARM64::VisitMathRoundFloat(HInvoke* invoke) {
  // See intrinsics.h.
  if (kRoundIsPlusPointFive) {
    CreateFPToIntPlusTempLocations(arena_, invoke);
  }
}

void IntrinsicCodeGeneratorARM64::VisitMathRoundFloat(HInvoke* invoke) {
  GenMathRound(invoke->GetLocations(), /* is_double */ false, GetVIXLAssembler());
}

void IntrinsicLocationsBuilderARM64::VisitMemoryPeekByte(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMemoryPeekByte(HInvoke* invoke) {
  vixl::MacroAssembler* masm = GetVIXLAssembler();
  __ Ldrsb(WRegisterFrom(invoke->GetLocations()->Out()),
          AbsoluteHeapOperandFrom(invoke->GetLocations()->InAt(0), 0));
}

void IntrinsicLocationsBuilderARM64::VisitMemoryPeekIntNative(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMemoryPeekIntNative(HInvoke* invoke) {
  vixl::MacroAssembler* masm = GetVIXLAssembler();
  __ Ldr(WRegisterFrom(invoke->GetLocations()->Out()),
         AbsoluteHeapOperandFrom(invoke->GetLocations()->InAt(0), 0));
}

void IntrinsicLocationsBuilderARM64::VisitMemoryPeekLongNative(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMemoryPeekLongNative(HInvoke* invoke) {
  vixl::MacroAssembler* masm = GetVIXLAssembler();
  __ Ldr(XRegisterFrom(invoke->GetLocations()->Out()),
         AbsoluteHeapOperandFrom(invoke->GetLocations()->InAt(0), 0));
}

void IntrinsicLocationsBuilderARM64::VisitMemoryPeekShortNative(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMemoryPeekShortNative(HInvoke* invoke) {
  vixl::MacroAssembler* masm = GetVIXLAssembler();
  __ Ldrsh(WRegisterFrom(invoke->GetLocations()->Out()),
           AbsoluteHeapOperandFrom(invoke->GetLocations()->InAt(0), 0));
}

static void CreateIntIntToVoidLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
}

void IntrinsicLocationsBuilderARM64::VisitMemoryPokeByte(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMemoryPokeByte(HInvoke* invoke) {
  vixl::MacroAssembler* masm = GetVIXLAssembler();
  __ Strb(WRegisterFrom(invoke->GetLocations()->InAt(1)),
          AbsoluteHeapOperandFrom(invoke->GetLocations()->InAt(0), 0));
}

void IntrinsicLocationsBuilderARM64::VisitMemoryPokeIntNative(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMemoryPokeIntNative(HInvoke* invoke) {
  vixl::MacroAssembler* masm = GetVIXLAssembler();
  __ Str(WRegisterFrom(invoke->GetLocations()->InAt(1)),
         AbsoluteHeapOperandFrom(invoke->GetLocations()->InAt(0), 0));
}

void IntrinsicLocationsBuilderARM64::VisitMemoryPokeLongNative(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMemoryPokeLongNative(HInvoke* invoke) {
  vixl::MacroAssembler* masm = GetVIXLAssembler();
  __ Str(XRegisterFrom(invoke->GetLocations()->InAt(1)),
         AbsoluteHeapOperandFrom(invoke->GetLocations()->InAt(0), 0));
}

void IntrinsicLocationsBuilderARM64::VisitMemoryPokeShortNative(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMemoryPokeShortNative(HInvoke* invoke) {
  vixl::MacroAssembler* masm = GetVIXLAssembler();
  __ Strh(WRegisterFrom(invoke->GetLocations()->InAt(1)),
          AbsoluteHeapOperandFrom(invoke->GetLocations()->InAt(0), 0));
}

void IntrinsicLocationsBuilderARM64::VisitThreadCurrentThread(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetOut(Location::RequiresRegister());
}

void IntrinsicCodeGeneratorARM64::VisitThreadCurrentThread(HInvoke* invoke) {
  codegen_->Load(Primitive::kPrimNot, WRegisterFrom(invoke->GetLocations()->Out()),
                 MemOperand(tr, Thread::PeerOffset<8>().Int32Value()));
}

static void GenUnsafeGet(HInvoke* invoke,
                         Primitive::Type type,
                         bool is_volatile,
                         CodeGeneratorARM64* codegen) {
  LocationSummary* locations = invoke->GetLocations();
  DCHECK((type == Primitive::kPrimInt) ||
         (type == Primitive::kPrimLong) ||
         (type == Primitive::kPrimNot));
  vixl::MacroAssembler* masm = codegen->GetAssembler()->vixl_masm_;
  Location base_loc = locations->InAt(1);
  Register base = WRegisterFrom(base_loc);      // Object pointer.
  Location offset_loc = locations->InAt(2);
  Register offset = XRegisterFrom(offset_loc);  // Long offset.
  Location trg_loc = locations->Out();
  Register trg = RegisterFrom(trg_loc, type);

  if (type == Primitive::kPrimNot && kEmitCompilerReadBarrier && kUseBakerReadBarrier) {
    // UnsafeGetObject/UnsafeGetObjectVolatile with Baker's read barrier case.
    UseScratchRegisterScope temps(masm);
    Register temp = temps.AcquireW();
    codegen->GenerateArrayLoadWithBakerReadBarrier(
        invoke, trg_loc, base, 0U, offset_loc, temp, /* needs_null_check */ false);
  } else {
    // Other cases.
    MemOperand mem_op(base.X(), offset);
    if (is_volatile) {
      codegen->LoadAcquire(invoke, trg, mem_op, /* needs_null_check */ true);
    } else {
      codegen->Load(type, trg, mem_op);
    }

    if (type == Primitive::kPrimNot) {
      DCHECK(trg.IsW());
      codegen->MaybeGenerateReadBarrierSlow(invoke, trg_loc, trg_loc, base_loc, 0U, offset_loc);
    }
  }
}

static void CreateIntIntIntToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
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
}

void IntrinsicLocationsBuilderARM64::VisitUnsafeGet(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderARM64::VisitUnsafeGetVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderARM64::VisitUnsafeGetLong(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderARM64::VisitUnsafeGetLongVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderARM64::VisitUnsafeGetObject(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderARM64::VisitUnsafeGetObjectVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitUnsafeGet(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimInt, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorARM64::VisitUnsafeGetVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimInt, /* is_volatile */ true, codegen_);
}
void IntrinsicCodeGeneratorARM64::VisitUnsafeGetLong(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimLong, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorARM64::VisitUnsafeGetLongVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimLong, /* is_volatile */ true, codegen_);
}
void IntrinsicCodeGeneratorARM64::VisitUnsafeGetObject(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimNot, /* is_volatile */ false, codegen_);
}
void IntrinsicCodeGeneratorARM64::VisitUnsafeGetObjectVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke, Primitive::kPrimNot, /* is_volatile */ true, codegen_);
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

void IntrinsicLocationsBuilderARM64::VisitUnsafePut(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, invoke);
}
void IntrinsicLocationsBuilderARM64::VisitUnsafePutOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, invoke);
}
void IntrinsicLocationsBuilderARM64::VisitUnsafePutVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, invoke);
}
void IntrinsicLocationsBuilderARM64::VisitUnsafePutObject(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, invoke);
}
void IntrinsicLocationsBuilderARM64::VisitUnsafePutObjectOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, invoke);
}
void IntrinsicLocationsBuilderARM64::VisitUnsafePutObjectVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, invoke);
}
void IntrinsicLocationsBuilderARM64::VisitUnsafePutLong(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, invoke);
}
void IntrinsicLocationsBuilderARM64::VisitUnsafePutLongOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, invoke);
}
void IntrinsicLocationsBuilderARM64::VisitUnsafePutLongVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoid(arena_, invoke);
}

static void GenUnsafePut(LocationSummary* locations,
                         Primitive::Type type,
                         bool is_volatile,
                         bool is_ordered,
                         CodeGeneratorARM64* codegen) {
  vixl::MacroAssembler* masm = codegen->GetAssembler()->vixl_masm_;

  Register base = WRegisterFrom(locations->InAt(1));    // Object pointer.
  Register offset = XRegisterFrom(locations->InAt(2));  // Long offset.
  Register value = RegisterFrom(locations->InAt(3), type);
  Register source = value;
  MemOperand mem_op(base.X(), offset);

  {
    // We use a block to end the scratch scope before the write barrier, thus
    // freeing the temporary registers so they can be used in `MarkGCCard`.
    UseScratchRegisterScope temps(masm);

    if (kPoisonHeapReferences && type == Primitive::kPrimNot) {
      DCHECK(value.IsW());
      Register temp = temps.AcquireW();
      __ Mov(temp.W(), value.W());
      codegen->GetAssembler()->PoisonHeapReference(temp.W());
      source = temp;
    }

    if (is_volatile || is_ordered) {
      codegen->StoreRelease(type, source, mem_op);
    } else {
      codegen->Store(type, source, mem_op);
    }
  }

  if (type == Primitive::kPrimNot) {
    bool value_can_be_null = true;  // TODO: Worth finding out this information?
    codegen->MarkGCCard(base, value, value_can_be_null);
  }
}

void IntrinsicCodeGeneratorARM64::VisitUnsafePut(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimInt,
               /* is_volatile */ false,
               /* is_ordered */ false,
               codegen_);
}
void IntrinsicCodeGeneratorARM64::VisitUnsafePutOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimInt,
               /* is_volatile */ false,
               /* is_ordered */ true,
               codegen_);
}
void IntrinsicCodeGeneratorARM64::VisitUnsafePutVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimInt,
               /* is_volatile */ true,
               /* is_ordered */ false,
               codegen_);
}
void IntrinsicCodeGeneratorARM64::VisitUnsafePutObject(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimNot,
               /* is_volatile */ false,
               /* is_ordered */ false,
               codegen_);
}
void IntrinsicCodeGeneratorARM64::VisitUnsafePutObjectOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimNot,
               /* is_volatile */ false,
               /* is_ordered */ true,
               codegen_);
}
void IntrinsicCodeGeneratorARM64::VisitUnsafePutObjectVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimNot,
               /* is_volatile */ true,
               /* is_ordered */ false,
               codegen_);
}
void IntrinsicCodeGeneratorARM64::VisitUnsafePutLong(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimLong,
               /* is_volatile */ false,
               /* is_ordered */ false,
               codegen_);
}
void IntrinsicCodeGeneratorARM64::VisitUnsafePutLongOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimLong,
               /* is_volatile */ false,
               /* is_ordered */ true,
               codegen_);
}
void IntrinsicCodeGeneratorARM64::VisitUnsafePutLongVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(),
               Primitive::kPrimLong,
               /* is_volatile */ true,
               /* is_ordered */ false,
               codegen_);
}

static void CreateIntIntIntIntIntToInt(ArenaAllocator* arena,
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
}

static void GenCas(LocationSummary* locations, Primitive::Type type, CodeGeneratorARM64* codegen) {
  vixl::MacroAssembler* masm = codegen->GetAssembler()->vixl_masm_;

  Register out = WRegisterFrom(locations->Out());                  // Boolean result.

  Register base = WRegisterFrom(locations->InAt(1));               // Object pointer.
  Register offset = XRegisterFrom(locations->InAt(2));             // Long offset.
  Register expected = RegisterFrom(locations->InAt(3), type);      // Expected.
  Register value = RegisterFrom(locations->InAt(4), type);         // Value.

  // This needs to be before the temp registers, as MarkGCCard also uses VIXL temps.
  if (type == Primitive::kPrimNot) {
    // Mark card for object assuming new value is stored.
    bool value_can_be_null = true;  // TODO: Worth finding out this information?
    codegen->MarkGCCard(base, value, value_can_be_null);
  }

  UseScratchRegisterScope temps(masm);
  Register tmp_ptr = temps.AcquireX();                             // Pointer to actual memory.
  Register tmp_value = temps.AcquireSameSizeAs(value);             // Value in memory.

  Register tmp_32 = tmp_value.W();

  __ Add(tmp_ptr, base.X(), Operand(offset));

  if (kPoisonHeapReferences && type == Primitive::kPrimNot) {
    codegen->GetAssembler()->PoisonHeapReference(expected);
    if (value.Is(expected)) {
      // Do not poison `value`, as it is the same register as
      // `expected`, which has just been poisoned.
    } else {
      codegen->GetAssembler()->PoisonHeapReference(value);
    }
  }

  // do {
  //   tmp_value = [tmp_ptr] - expected;
  // } while (tmp_value == 0 && failure([tmp_ptr] <- r_new_value));
  // result = tmp_value != 0;

  vixl::Label loop_head, exit_loop;
  __ Bind(&loop_head);
  // TODO: When `type == Primitive::kPrimNot`, add a read barrier for
  // the reference stored in the object before attempting the CAS,
  // similar to the one in the art::Unsafe_compareAndSwapObject JNI
  // implementation.
  //
  // Note that this code is not (yet) used when read barriers are
  // enabled (see IntrinsicLocationsBuilderARM64::VisitUnsafeCASObject).
  DCHECK(!(type == Primitive::kPrimNot && kEmitCompilerReadBarrier));
  __ Ldaxr(tmp_value, MemOperand(tmp_ptr));
  __ Cmp(tmp_value, expected);
  __ B(&exit_loop, ne);
  __ Stlxr(tmp_32, value, MemOperand(tmp_ptr));
  __ Cbnz(tmp_32, &loop_head);
  __ Bind(&exit_loop);
  __ Cset(out, eq);

  if (kPoisonHeapReferences && type == Primitive::kPrimNot) {
    codegen->GetAssembler()->UnpoisonHeapReference(expected);
    if (value.Is(expected)) {
      // Do not unpoison `value`, as it is the same register as
      // `expected`, which has just been unpoisoned.
    } else {
      codegen->GetAssembler()->UnpoisonHeapReference(value);
    }
  }
}

void IntrinsicLocationsBuilderARM64::VisitUnsafeCASInt(HInvoke* invoke) {
  CreateIntIntIntIntIntToInt(arena_, invoke, Primitive::kPrimInt);
}
void IntrinsicLocationsBuilderARM64::VisitUnsafeCASLong(HInvoke* invoke) {
  CreateIntIntIntIntIntToInt(arena_, invoke, Primitive::kPrimLong);
}
void IntrinsicLocationsBuilderARM64::VisitUnsafeCASObject(HInvoke* invoke) {
  // The UnsafeCASObject intrinsic is missing a read barrier, and
  // therefore sometimes does not work as expected (b/25883050).
  // Turn it off temporarily as a quick fix, until the read barrier is
  // implemented (see TODO in GenCAS below).
  //
  // TODO(rpl): Fix this issue and re-enable this intrinsic with read barriers.
  if (kEmitCompilerReadBarrier) {
    return;
  }

  CreateIntIntIntIntIntToInt(arena_, invoke, Primitive::kPrimNot);
}

void IntrinsicCodeGeneratorARM64::VisitUnsafeCASInt(HInvoke* invoke) {
  GenCas(invoke->GetLocations(), Primitive::kPrimInt, codegen_);
}
void IntrinsicCodeGeneratorARM64::VisitUnsafeCASLong(HInvoke* invoke) {
  GenCas(invoke->GetLocations(), Primitive::kPrimLong, codegen_);
}
void IntrinsicCodeGeneratorARM64::VisitUnsafeCASObject(HInvoke* invoke) {
  GenCas(invoke->GetLocations(), Primitive::kPrimNot, codegen_);
}

void IntrinsicLocationsBuilderARM64::VisitStringCharAt(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCallOnSlowPath,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  // In case we need to go in the slow path, we can't have the output be the same
  // as the input: the current liveness analysis considers the input to be live
  // at the point of the call.
  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
}

void IntrinsicCodeGeneratorARM64::VisitStringCharAt(HInvoke* invoke) {
  vixl::MacroAssembler* masm = GetVIXLAssembler();
  LocationSummary* locations = invoke->GetLocations();

  // Location of reference to data array
  const MemberOffset value_offset = mirror::String::ValueOffset();
  // Location of count
  const MemberOffset count_offset = mirror::String::CountOffset();

  Register obj = WRegisterFrom(locations->InAt(0));  // String object pointer.
  Register idx = WRegisterFrom(locations->InAt(1));  // Index of character.
  Register out = WRegisterFrom(locations->Out());    // Result character.

  UseScratchRegisterScope temps(masm);
  Register temp = temps.AcquireW();
  Register array_temp = temps.AcquireW();            // We can trade this for worse scheduling.

  // TODO: Maybe we can support range check elimination. Overall, though, I think it's not worth
  //       the cost.
  // TODO: For simplicity, the index parameter is requested in a register, so different from Quick
  //       we will not optimize the code for constants (which would save a register).

  SlowPathCodeARM64* slow_path = new (GetAllocator()) IntrinsicSlowPathARM64(invoke);
  codegen_->AddSlowPath(slow_path);

  __ Ldr(temp, HeapOperand(obj, count_offset));          // temp = str.length.
  codegen_->MaybeRecordImplicitNullCheck(invoke);
  __ Cmp(idx, temp);
  __ B(hs, slow_path->GetEntryLabel());

  __ Add(array_temp, obj, Operand(value_offset.Int32Value()));  // array_temp := str.value.

  // Load the value.
  __ Ldrh(out, MemOperand(array_temp.X(), idx, UXTW, 1));  // out := array_temp[idx].

  __ Bind(slow_path->GetExitLabel());
}

void IntrinsicLocationsBuilderARM64::VisitStringCompareTo(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, LocationFrom(calling_convention.GetRegisterAt(1)));
  locations->SetOut(calling_convention.GetReturnLocation(Primitive::kPrimInt));
}

void IntrinsicCodeGeneratorARM64::VisitStringCompareTo(HInvoke* invoke) {
  vixl::MacroAssembler* masm = GetVIXLAssembler();
  LocationSummary* locations = invoke->GetLocations();

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  Register argument = WRegisterFrom(locations->InAt(1));
  __ Cmp(argument, 0);
  SlowPathCodeARM64* slow_path = new (GetAllocator()) IntrinsicSlowPathARM64(invoke);
  codegen_->AddSlowPath(slow_path);
  __ B(eq, slow_path->GetEntryLabel());

  __ Ldr(
      lr, MemOperand(tr, QUICK_ENTRYPOINT_OFFSET(kArm64WordSize, pStringCompareTo).Int32Value()));
  __ Blr(lr);
  __ Bind(slow_path->GetExitLabel());
}

void IntrinsicLocationsBuilderARM64::VisitStringEquals(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  // Temporary registers to store lengths of strings and for calculations.
  locations->AddTemp(Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());

  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
}

void IntrinsicCodeGeneratorARM64::VisitStringEquals(HInvoke* invoke) {
  vixl::MacroAssembler* masm = GetVIXLAssembler();
  LocationSummary* locations = invoke->GetLocations();

  Register str = WRegisterFrom(locations->InAt(0));
  Register arg = WRegisterFrom(locations->InAt(1));
  Register out = XRegisterFrom(locations->Out());

  UseScratchRegisterScope scratch_scope(masm);
  Register temp = scratch_scope.AcquireW();
  Register temp1 = WRegisterFrom(locations->GetTemp(0));
  Register temp2 = WRegisterFrom(locations->GetTemp(1));

  vixl::Label loop;
  vixl::Label end;
  vixl::Label return_true;
  vixl::Label return_false;

  // Get offsets of count, value, and class fields within a string object.
  const int32_t count_offset = mirror::String::CountOffset().Int32Value();
  const int32_t value_offset = mirror::String::ValueOffset().Int32Value();
  const int32_t class_offset = mirror::Object::ClassOffset().Int32Value();

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  // Check if input is null, return false if it is.
  __ Cbz(arg, &return_false);

  // Reference equality check, return true if same reference.
  __ Cmp(str, arg);
  __ B(&return_true, eq);

  // Instanceof check for the argument by comparing class fields.
  // All string objects must have the same type since String cannot be subclassed.
  // Receiver must be a string object, so its class field is equal to all strings' class fields.
  // If the argument is a string object, its class field must be equal to receiver's class field.
  __ Ldr(temp, MemOperand(str.X(), class_offset));
  __ Ldr(temp1, MemOperand(arg.X(), class_offset));
  __ Cmp(temp, temp1);
  __ B(&return_false, ne);

  // Load lengths of this and argument strings.
  __ Ldr(temp, MemOperand(str.X(), count_offset));
  __ Ldr(temp1, MemOperand(arg.X(), count_offset));
  // Check if lengths are equal, return false if they're not.
  __ Cmp(temp, temp1);
  __ B(&return_false, ne);
  // Store offset of string value in preparation for comparison loop
  __ Mov(temp1, value_offset);
  // Return true if both strings are empty.
  __ Cbz(temp, &return_true);

  // Assertions that must hold in order to compare strings 4 characters at a time.
  DCHECK_ALIGNED(value_offset, 8);
  static_assert(IsAligned<8>(kObjectAlignment), "String of odd length is not zero padded");

  temp1 = temp1.X();
  temp2 = temp2.X();

  // Loop to compare strings 4 characters at a time starting at the beginning of the string.
  // Ok to do this because strings are zero-padded to be 8-byte aligned.
  __ Bind(&loop);
  __ Ldr(out, MemOperand(str.X(), temp1));
  __ Ldr(temp2, MemOperand(arg.X(), temp1));
  __ Add(temp1, temp1, Operand(sizeof(uint64_t)));
  __ Cmp(out, temp2);
  __ B(&return_false, ne);
  __ Sub(temp, temp, Operand(4), SetFlags);
  __ B(&loop, gt);

  // Return true and exit the function.
  // If loop does not result in returning false, we return true.
  __ Bind(&return_true);
  __ Mov(out, 1);
  __ B(&end);

  // Return false and exit the function.
  __ Bind(&return_false);
  __ Mov(out, 0);
  __ Bind(&end);
}

static void GenerateVisitStringIndexOf(HInvoke* invoke,
                                       vixl::MacroAssembler* masm,
                                       CodeGeneratorARM64* codegen,
                                       ArenaAllocator* allocator,
                                       bool start_at_zero) {
  LocationSummary* locations = invoke->GetLocations();
  Register tmp_reg = WRegisterFrom(locations->GetTemp(0));

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  // Check for code points > 0xFFFF. Either a slow-path check when we don't know statically,
  // or directly dispatch if we have a constant.
  SlowPathCodeARM64* slow_path = nullptr;
  if (invoke->InputAt(1)->IsIntConstant()) {
    if (static_cast<uint32_t>(invoke->InputAt(1)->AsIntConstant()->GetValue()) > 0xFFFFU) {
      // Always needs the slow-path. We could directly dispatch to it, but this case should be
      // rare, so for simplicity just put the full slow-path down and branch unconditionally.
      slow_path = new (allocator) IntrinsicSlowPathARM64(invoke);
      codegen->AddSlowPath(slow_path);
      __ B(slow_path->GetEntryLabel());
      __ Bind(slow_path->GetExitLabel());
      return;
    }
  } else {
    Register char_reg = WRegisterFrom(locations->InAt(1));
    __ Mov(tmp_reg, 0xFFFF);
    __ Cmp(char_reg, Operand(tmp_reg));
    slow_path = new (allocator) IntrinsicSlowPathARM64(invoke);
    codegen->AddSlowPath(slow_path);
    __ B(hi, slow_path->GetEntryLabel());
  }

  if (start_at_zero) {
    // Start-index = 0.
    __ Mov(tmp_reg, 0);
  }

  __ Ldr(lr, MemOperand(tr, QUICK_ENTRYPOINT_OFFSET(kArm64WordSize, pIndexOf).Int32Value()));
  CheckEntrypointTypes<kQuickIndexOf, int32_t, void*, uint32_t, uint32_t>();
  __ Blr(lr);

  if (slow_path != nullptr) {
    __ Bind(slow_path->GetExitLabel());
  }
}

void IntrinsicLocationsBuilderARM64::VisitStringIndexOf(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  // We have a hand-crafted assembly stub that follows the runtime calling convention. So it's
  // best to align the inputs accordingly.
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, LocationFrom(calling_convention.GetRegisterAt(1)));
  locations->SetOut(calling_convention.GetReturnLocation(Primitive::kPrimInt));

  // Need a temp for slow-path codepoint compare, and need to send start_index=0.
  locations->AddTemp(LocationFrom(calling_convention.GetRegisterAt(2)));
}

void IntrinsicCodeGeneratorARM64::VisitStringIndexOf(HInvoke* invoke) {
  GenerateVisitStringIndexOf(
      invoke, GetVIXLAssembler(), codegen_, GetAllocator(), /* start_at_zero */ true);
}

void IntrinsicLocationsBuilderARM64::VisitStringIndexOfAfter(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  // We have a hand-crafted assembly stub that follows the runtime calling convention. So it's
  // best to align the inputs accordingly.
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, LocationFrom(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(2, LocationFrom(calling_convention.GetRegisterAt(2)));
  locations->SetOut(calling_convention.GetReturnLocation(Primitive::kPrimInt));

  // Need a temp for slow-path codepoint compare.
  locations->AddTemp(Location::RequiresRegister());
}

void IntrinsicCodeGeneratorARM64::VisitStringIndexOfAfter(HInvoke* invoke) {
  GenerateVisitStringIndexOf(
      invoke, GetVIXLAssembler(), codegen_, GetAllocator(), /* start_at_zero */ false);
}

void IntrinsicLocationsBuilderARM64::VisitStringNewStringFromBytes(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, LocationFrom(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(2, LocationFrom(calling_convention.GetRegisterAt(2)));
  locations->SetInAt(3, LocationFrom(calling_convention.GetRegisterAt(3)));
  locations->SetOut(calling_convention.GetReturnLocation(Primitive::kPrimNot));
}

void IntrinsicCodeGeneratorARM64::VisitStringNewStringFromBytes(HInvoke* invoke) {
  vixl::MacroAssembler* masm = GetVIXLAssembler();
  LocationSummary* locations = invoke->GetLocations();

  Register byte_array = WRegisterFrom(locations->InAt(0));
  __ Cmp(byte_array, 0);
  SlowPathCodeARM64* slow_path = new (GetAllocator()) IntrinsicSlowPathARM64(invoke);
  codegen_->AddSlowPath(slow_path);
  __ B(eq, slow_path->GetEntryLabel());

  __ Ldr(lr,
      MemOperand(tr, QUICK_ENTRYPOINT_OFFSET(kArm64WordSize, pAllocStringFromBytes).Int32Value()));
  CheckEntrypointTypes<kQuickAllocStringFromBytes, void*, void*, int32_t, int32_t, int32_t>();
  __ Blr(lr);
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
  __ Bind(slow_path->GetExitLabel());
}

void IntrinsicLocationsBuilderARM64::VisitStringNewStringFromChars(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, LocationFrom(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(2, LocationFrom(calling_convention.GetRegisterAt(2)));
  locations->SetOut(calling_convention.GetReturnLocation(Primitive::kPrimNot));
}

void IntrinsicCodeGeneratorARM64::VisitStringNewStringFromChars(HInvoke* invoke) {
  vixl::MacroAssembler* masm = GetVIXLAssembler();

  // No need to emit code checking whether `locations->InAt(2)` is a null
  // pointer, as callers of the native method
  //
  //   java.lang.StringFactory.newStringFromChars(int offset, int charCount, char[] data)
  //
  // all include a null check on `data` before calling that method.
  __ Ldr(lr,
      MemOperand(tr, QUICK_ENTRYPOINT_OFFSET(kArm64WordSize, pAllocStringFromChars).Int32Value()));
  CheckEntrypointTypes<kQuickAllocStringFromChars, void*, int32_t, int32_t, void*>();
  __ Blr(lr);
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void IntrinsicLocationsBuilderARM64::VisitStringNewStringFromString(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(0)));
  locations->SetOut(calling_convention.GetReturnLocation(Primitive::kPrimNot));
}

void IntrinsicCodeGeneratorARM64::VisitStringNewStringFromString(HInvoke* invoke) {
  vixl::MacroAssembler* masm = GetVIXLAssembler();
  LocationSummary* locations = invoke->GetLocations();

  Register string_to_copy = WRegisterFrom(locations->InAt(0));
  __ Cmp(string_to_copy, 0);
  SlowPathCodeARM64* slow_path = new (GetAllocator()) IntrinsicSlowPathARM64(invoke);
  codegen_->AddSlowPath(slow_path);
  __ B(eq, slow_path->GetEntryLabel());

  __ Ldr(lr,
      MemOperand(tr, QUICK_ENTRYPOINT_OFFSET(kArm64WordSize, pAllocStringFromString).Int32Value()));
  CheckEntrypointTypes<kQuickAllocStringFromString, void*, void*>();
  __ Blr(lr);
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
  __ Bind(slow_path->GetExitLabel());
}

static void CreateFPToFPCallLocations(ArenaAllocator* arena, HInvoke* invoke) {
  DCHECK_EQ(invoke->GetNumberOfArguments(), 1U);
  DCHECK(Primitive::IsFloatingPointType(invoke->InputAt(0)->GetType()));
  DCHECK(Primitive::IsFloatingPointType(invoke->GetType()));

  LocationSummary* const locations = new (arena) LocationSummary(invoke,
                                                                 LocationSummary::kCall,
                                                                 kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;

  locations->SetInAt(0, LocationFrom(calling_convention.GetFpuRegisterAt(0)));
  locations->SetOut(calling_convention.GetReturnLocation(invoke->GetType()));
}

static void CreateFPFPToFPCallLocations(ArenaAllocator* arena, HInvoke* invoke) {
  DCHECK_EQ(invoke->GetNumberOfArguments(), 2U);
  DCHECK(Primitive::IsFloatingPointType(invoke->InputAt(0)->GetType()));
  DCHECK(Primitive::IsFloatingPointType(invoke->InputAt(1)->GetType()));
  DCHECK(Primitive::IsFloatingPointType(invoke->GetType()));

  LocationSummary* const locations = new (arena) LocationSummary(invoke,
                                                                 LocationSummary::kCall,
                                                                 kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;

  locations->SetInAt(0, LocationFrom(calling_convention.GetFpuRegisterAt(0)));
  locations->SetInAt(1, LocationFrom(calling_convention.GetFpuRegisterAt(1)));
  locations->SetOut(calling_convention.GetReturnLocation(invoke->GetType()));
}

static void GenFPToFPCall(HInvoke* invoke,
                          vixl::MacroAssembler* masm,
                          CodeGeneratorARM64* codegen,
                          QuickEntrypointEnum entry) {
  __ Ldr(lr, MemOperand(tr, GetThreadOffset<kArm64WordSize>(entry).Int32Value()));
  __ Blr(lr);
  codegen->RecordPcInfo(invoke, invoke->GetDexPc());
}

void IntrinsicLocationsBuilderARM64::VisitMathCos(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathCos(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetVIXLAssembler(), codegen_, kQuickCos);
}

void IntrinsicLocationsBuilderARM64::VisitMathSin(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathSin(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetVIXLAssembler(), codegen_, kQuickSin);
}

void IntrinsicLocationsBuilderARM64::VisitMathAcos(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathAcos(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetVIXLAssembler(), codegen_, kQuickAcos);
}

void IntrinsicLocationsBuilderARM64::VisitMathAsin(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathAsin(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetVIXLAssembler(), codegen_, kQuickAsin);
}

void IntrinsicLocationsBuilderARM64::VisitMathAtan(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathAtan(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetVIXLAssembler(), codegen_, kQuickAtan);
}

void IntrinsicLocationsBuilderARM64::VisitMathCbrt(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathCbrt(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetVIXLAssembler(), codegen_, kQuickCbrt);
}

void IntrinsicLocationsBuilderARM64::VisitMathCosh(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathCosh(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetVIXLAssembler(), codegen_, kQuickCosh);
}

void IntrinsicLocationsBuilderARM64::VisitMathExp(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathExp(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetVIXLAssembler(), codegen_, kQuickExp);
}

void IntrinsicLocationsBuilderARM64::VisitMathExpm1(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathExpm1(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetVIXLAssembler(), codegen_, kQuickExpm1);
}

void IntrinsicLocationsBuilderARM64::VisitMathLog(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathLog(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetVIXLAssembler(), codegen_, kQuickLog);
}

void IntrinsicLocationsBuilderARM64::VisitMathLog10(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathLog10(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetVIXLAssembler(), codegen_, kQuickLog10);
}

void IntrinsicLocationsBuilderARM64::VisitMathSinh(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathSinh(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetVIXLAssembler(), codegen_, kQuickSinh);
}

void IntrinsicLocationsBuilderARM64::VisitMathTan(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathTan(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetVIXLAssembler(), codegen_, kQuickTan);
}

void IntrinsicLocationsBuilderARM64::VisitMathTanh(HInvoke* invoke) {
  CreateFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathTanh(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetVIXLAssembler(), codegen_, kQuickTanh);
}

void IntrinsicLocationsBuilderARM64::VisitMathAtan2(HInvoke* invoke) {
  CreateFPFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathAtan2(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetVIXLAssembler(), codegen_, kQuickAtan2);
}

void IntrinsicLocationsBuilderARM64::VisitMathHypot(HInvoke* invoke) {
  CreateFPFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathHypot(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetVIXLAssembler(), codegen_, kQuickHypot);
}

void IntrinsicLocationsBuilderARM64::VisitMathNextAfter(HInvoke* invoke) {
  CreateFPFPToFPCallLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorARM64::VisitMathNextAfter(HInvoke* invoke) {
  GenFPToFPCall(invoke, GetVIXLAssembler(), codegen_, kQuickNextAfter);
}

void IntrinsicLocationsBuilderARM64::VisitStringGetCharsNoCheck(HInvoke* invoke) {
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
}

void IntrinsicCodeGeneratorARM64::VisitStringGetCharsNoCheck(HInvoke* invoke) {
  vixl::MacroAssembler* masm = GetVIXLAssembler();
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
  Register srcObj = XRegisterFrom(locations->InAt(0));
  Register srcBegin = XRegisterFrom(locations->InAt(1));
  Register srcEnd = XRegisterFrom(locations->InAt(2));
  Register dstObj = XRegisterFrom(locations->InAt(3));
  Register dstBegin = XRegisterFrom(locations->InAt(4));

  Register src_ptr = XRegisterFrom(locations->GetTemp(0));
  Register src_ptr_end = XRegisterFrom(locations->GetTemp(1));

  UseScratchRegisterScope temps(masm);
  Register dst_ptr = temps.AcquireX();
  Register tmp = temps.AcquireW();

  // src range to copy.
  __ Add(src_ptr, srcObj, Operand(value_offset));
  __ Add(src_ptr_end, src_ptr, Operand(srcEnd, LSL, 1));
  __ Add(src_ptr, src_ptr, Operand(srcBegin, LSL, 1));

  // dst to be copied.
  __ Add(dst_ptr, dstObj, Operand(data_offset));
  __ Add(dst_ptr, dst_ptr, Operand(dstBegin, LSL, 1));

  // Do the copy.
  vixl::Label loop, done;
  __ Bind(&loop);
  __ Cmp(src_ptr, src_ptr_end);
  __ B(&done, eq);
  __ Ldrh(tmp, MemOperand(src_ptr, char_size, vixl::PostIndex));
  __ Strh(tmp, MemOperand(dst_ptr, char_size, vixl::PostIndex));
  __ B(&loop);
  __ Bind(&done);
}

// Mirrors ARRAYCOPY_SHORT_CHAR_ARRAY_THRESHOLD in libcore, so we can choose to use the native
// implementation there for longer copy lengths.
static constexpr int32_t kSystemArrayCopyCharThreshold = 32;

static void SetSystemArrayCopyLocationRequires(LocationSummary* locations,
                                               uint32_t at,
                                               HInstruction* input) {
  HIntConstant* const_input = input->AsIntConstant();
  if (const_input != nullptr && !vixl::Assembler::IsImmAddSub(const_input->GetValue())) {
    locations->SetInAt(at, Location::RequiresRegister());
  } else {
    locations->SetInAt(at, Location::RegisterOrConstant(input));
  }
}

void IntrinsicLocationsBuilderARM64::VisitSystemArrayCopyChar(HInvoke* invoke) {
  // Check to see if we have known failures that will cause us to have to bail out
  // to the runtime, and just generate the runtime call directly.
  HIntConstant* src_pos = invoke->InputAt(1)->AsIntConstant();
  HIntConstant* dst_pos = invoke->InputAt(3)->AsIntConstant();

  // The positions must be non-negative.
  if ((src_pos != nullptr && src_pos->GetValue() < 0) ||
      (dst_pos != nullptr && dst_pos->GetValue() < 0)) {
    // We will have to fail anyways.
    return;
  }

  // The length must be >= 0 and not so long that we would (currently) prefer libcore's
  // native implementation.
  HIntConstant* length = invoke->InputAt(4)->AsIntConstant();
  if (length != nullptr) {
    int32_t len = length->GetValue();
    if (len < 0 || len > kSystemArrayCopyCharThreshold) {
      // Just call as normal.
      return;
    }
  }

  ArenaAllocator* allocator = invoke->GetBlock()->GetGraph()->GetArena();
  LocationSummary* locations = new (allocator) LocationSummary(invoke,
                                                               LocationSummary::kCallOnSlowPath,
                                                               kIntrinsified);
  // arraycopy(char[] src, int src_pos, char[] dst, int dst_pos, int length).
  locations->SetInAt(0, Location::RequiresRegister());
  SetSystemArrayCopyLocationRequires(locations, 1, invoke->InputAt(1));
  locations->SetInAt(2, Location::RequiresRegister());
  SetSystemArrayCopyLocationRequires(locations, 3, invoke->InputAt(3));
  SetSystemArrayCopyLocationRequires(locations, 4, invoke->InputAt(4));

  locations->AddTemp(Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
}

static void CheckSystemArrayCopyPosition(vixl::MacroAssembler* masm,
                                         const Location& pos,
                                         const Register& input,
                                         const Location& length,
                                         SlowPathCodeARM64* slow_path,
                                         const Register& input_len,
                                         const Register& temp,
                                         bool length_is_input_length = false) {
  const int32_t length_offset = mirror::Array::LengthOffset().Int32Value();
  if (pos.IsConstant()) {
    int32_t pos_const = pos.GetConstant()->AsIntConstant()->GetValue();
    if (pos_const == 0) {
      if (!length_is_input_length) {
        // Check that length(input) >= length.
        __ Ldr(temp, MemOperand(input, length_offset));
        __ Cmp(temp, OperandFrom(length, Primitive::kPrimInt));
        __ B(slow_path->GetEntryLabel(), lt);
      }
    } else {
      // Check that length(input) >= pos.
      __ Ldr(input_len, MemOperand(input, length_offset));
      __ Subs(temp, input_len, pos_const);
      __ B(slow_path->GetEntryLabel(), lt);

      // Check that (length(input) - pos) >= length.
      __ Cmp(temp, OperandFrom(length, Primitive::kPrimInt));
      __ B(slow_path->GetEntryLabel(), lt);
    }
  } else if (length_is_input_length) {
    // The only way the copy can succeed is if pos is zero.
    __ Cbnz(WRegisterFrom(pos), slow_path->GetEntryLabel());
  } else {
    // Check that pos >= 0.
    Register pos_reg = WRegisterFrom(pos);
    __ Tbnz(pos_reg, pos_reg.size() - 1, slow_path->GetEntryLabel());

    // Check that pos <= length(input) && (length(input) - pos) >= length.
    __ Ldr(temp, MemOperand(input, length_offset));
    __ Subs(temp, temp, pos_reg);
    // Ccmp if length(input) >= pos, else definitely bail to slow path (N!=V == lt).
    __ Ccmp(temp, OperandFrom(length, Primitive::kPrimInt), NFlag, ge);
    __ B(slow_path->GetEntryLabel(), lt);
  }
}

// Compute base source address, base destination address, and end source address
// for System.arraycopy* intrinsics.
static void GenSystemArrayCopyAddresses(vixl::MacroAssembler* masm,
                                        Primitive::Type type,
                                        const Register& src,
                                        const Location& src_pos,
                                        const Register& dst,
                                        const Location& dst_pos,
                                        const Location& copy_length,
                                        const Register& src_base,
                                        const Register& dst_base,
                                        const Register& src_end) {
  DCHECK(type == Primitive::kPrimNot || type == Primitive::kPrimChar)
      << "Unexpected element type: " << type;
  const int32_t element_size = Primitive::ComponentSize(type);
  const int32_t element_size_shift = Primitive::ComponentSizeShift(type);

  uint32_t data_offset = mirror::Array::DataOffset(element_size).Uint32Value();
  if (src_pos.IsConstant()) {
    int32_t constant = src_pos.GetConstant()->AsIntConstant()->GetValue();
    __ Add(src_base, src, element_size * constant + data_offset);
  } else {
    __ Add(src_base, src, data_offset);
    __ Add(src_base, src_base, Operand(XRegisterFrom(src_pos), LSL, element_size_shift));
  }

  if (dst_pos.IsConstant()) {
    int32_t constant = dst_pos.GetConstant()->AsIntConstant()->GetValue();
    __ Add(dst_base, dst, element_size * constant + data_offset);
  } else {
    __ Add(dst_base, dst, data_offset);
    __ Add(dst_base, dst_base, Operand(XRegisterFrom(dst_pos), LSL, element_size_shift));
  }

  if (copy_length.IsConstant()) {
    int32_t constant = copy_length.GetConstant()->AsIntConstant()->GetValue();
    __ Add(src_end, src_base, element_size * constant);
  } else {
    __ Add(src_end, src_base, Operand(XRegisterFrom(copy_length), LSL, element_size_shift));
  }
}

void IntrinsicCodeGeneratorARM64::VisitSystemArrayCopyChar(HInvoke* invoke) {
  vixl::MacroAssembler* masm = GetVIXLAssembler();
  LocationSummary* locations = invoke->GetLocations();
  Register src = XRegisterFrom(locations->InAt(0));
  Location src_pos = locations->InAt(1);
  Register dst = XRegisterFrom(locations->InAt(2));
  Location dst_pos = locations->InAt(3);
  Location length = locations->InAt(4);

  SlowPathCodeARM64* slow_path = new (GetAllocator()) IntrinsicSlowPathARM64(invoke);
  codegen_->AddSlowPath(slow_path);

  // If source and destination are the same, take the slow path. Overlapping copy regions must be
  // copied in reverse and we can't know in all cases if it's needed.
  __ Cmp(src, dst);
  __ B(slow_path->GetEntryLabel(), eq);

  // Bail out if the source is null.
  __ Cbz(src, slow_path->GetEntryLabel());

  // Bail out if the destination is null.
  __ Cbz(dst, slow_path->GetEntryLabel());

  if (!length.IsConstant()) {
    // If the length is negative, bail out.
    __ Tbnz(WRegisterFrom(length), kWRegSize - 1, slow_path->GetEntryLabel());
    // If the length > 32 then (currently) prefer libcore's native implementation.
    __ Cmp(WRegisterFrom(length), kSystemArrayCopyCharThreshold);
    __ B(slow_path->GetEntryLabel(), gt);
  } else {
    // We have already checked in the LocationsBuilder for the constant case.
    DCHECK_GE(length.GetConstant()->AsIntConstant()->GetValue(), 0);
    DCHECK_LE(length.GetConstant()->AsIntConstant()->GetValue(), 32);
  }

  Register src_curr_addr = WRegisterFrom(locations->GetTemp(0));
  Register dst_curr_addr = WRegisterFrom(locations->GetTemp(1));
  Register src_stop_addr = WRegisterFrom(locations->GetTemp(2));

  CheckSystemArrayCopyPosition(masm,
                               src_pos,
                               src,
                               length,
                               slow_path,
                               src_curr_addr,
                               dst_curr_addr,
                               false);

  CheckSystemArrayCopyPosition(masm,
                               dst_pos,
                               dst,
                               length,
                               slow_path,
                               src_curr_addr,
                               dst_curr_addr,
                               false);

  src_curr_addr = src_curr_addr.X();
  dst_curr_addr = dst_curr_addr.X();
  src_stop_addr = src_stop_addr.X();

  GenSystemArrayCopyAddresses(masm,
                              Primitive::kPrimChar,
                              src,
                              src_pos,
                              dst,
                              dst_pos,
                              length,
                              src_curr_addr,
                              dst_curr_addr,
                              src_stop_addr);

  // Iterate over the arrays and do a raw copy of the chars.
  const int32_t char_size = Primitive::ComponentSize(Primitive::kPrimChar);
  UseScratchRegisterScope temps(masm);
  Register tmp = temps.AcquireW();
  vixl::Label loop, done;
  __ Bind(&loop);
  __ Cmp(src_curr_addr, src_stop_addr);
  __ B(&done, eq);
  __ Ldrh(tmp, MemOperand(src_curr_addr, char_size, vixl::PostIndex));
  __ Strh(tmp, MemOperand(dst_curr_addr, char_size, vixl::PostIndex));
  __ B(&loop);
  __ Bind(&done);

  __ Bind(slow_path->GetExitLabel());
}

// We can choose to use the native implementation there for longer copy lengths.
static constexpr int32_t kSystemArrayCopyThreshold = 128;

// CodeGenerator::CreateSystemArrayCopyLocationSummary use three temporary registers.
// We want to use two temporary registers in order to reduce the register pressure in arm64.
// So we don't use the CodeGenerator::CreateSystemArrayCopyLocationSummary.
void IntrinsicLocationsBuilderARM64::VisitSystemArrayCopy(HInvoke* invoke) {
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

  // The length must be >= 0.
  HIntConstant* length = invoke->InputAt(4)->AsIntConstant();
  if (length != nullptr) {
    int32_t len = length->GetValue();
    if (len < 0 || len >= kSystemArrayCopyThreshold) {
      // Just call as normal.
      return;
    }
  }

  SystemArrayCopyOptimizations optimizations(invoke);

  if (optimizations.GetDestinationIsSource()) {
    if (src_pos != nullptr && dest_pos != nullptr && src_pos->GetValue() < dest_pos->GetValue()) {
      // We only support backward copying if source and destination are the same.
      return;
    }
  }

  if (optimizations.GetDestinationIsPrimitiveArray() || optimizations.GetSourceIsPrimitiveArray()) {
    // We currently don't intrinsify primitive copying.
    return;
  }

  ArenaAllocator* allocator = invoke->GetBlock()->GetGraph()->GetArena();
  LocationSummary* locations = new (allocator) LocationSummary(invoke,
                                                               LocationSummary::kCallOnSlowPath,
                                                               kIntrinsified);
  // arraycopy(Object src, int src_pos, Object dest, int dest_pos, int length).
  locations->SetInAt(0, Location::RequiresRegister());
  SetSystemArrayCopyLocationRequires(locations, 1, invoke->InputAt(1));
  locations->SetInAt(2, Location::RequiresRegister());
  SetSystemArrayCopyLocationRequires(locations, 3, invoke->InputAt(3));
  SetSystemArrayCopyLocationRequires(locations, 4, invoke->InputAt(4));

  locations->AddTemp(Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
}

void IntrinsicCodeGeneratorARM64::VisitSystemArrayCopy(HInvoke* invoke) {
  vixl::MacroAssembler* masm = GetVIXLAssembler();
  LocationSummary* locations = invoke->GetLocations();

  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  uint32_t super_offset = mirror::Class::SuperClassOffset().Int32Value();
  uint32_t component_offset = mirror::Class::ComponentTypeOffset().Int32Value();
  uint32_t primitive_offset = mirror::Class::PrimitiveTypeOffset().Int32Value();

  Register src = XRegisterFrom(locations->InAt(0));
  Location src_pos = locations->InAt(1);
  Register dest = XRegisterFrom(locations->InAt(2));
  Location dest_pos = locations->InAt(3);
  Location length = locations->InAt(4);
  Register temp1 = WRegisterFrom(locations->GetTemp(0));
  Register temp2 = WRegisterFrom(locations->GetTemp(1));

  SlowPathCodeARM64* slow_path = new (GetAllocator()) IntrinsicSlowPathARM64(invoke);
  codegen_->AddSlowPath(slow_path);

  vixl::Label conditions_on_positions_validated;
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
        __ Cmp(src, dest);
        __ B(slow_path->GetEntryLabel(), eq);
      }
      // Checked when building locations.
      DCHECK(!optimizations.GetDestinationIsSource()
             || (src_pos_constant >= dest_pos.GetConstant()->AsIntConstant()->GetValue()));
    } else {
      if (!optimizations.GetDestinationIsSource()) {
        __ Cmp(src, dest);
        __ B(&conditions_on_positions_validated, ne);
      }
      __ Cmp(WRegisterFrom(dest_pos), src_pos_constant);
      __ B(slow_path->GetEntryLabel(), gt);
    }
  } else {
    if (!optimizations.GetDestinationIsSource()) {
      __ Cmp(src, dest);
      __ B(&conditions_on_positions_validated, ne);
    }
    __ Cmp(RegisterFrom(src_pos, invoke->InputAt(1)->GetType()),
           OperandFrom(dest_pos, invoke->InputAt(3)->GetType()));
    __ B(slow_path->GetEntryLabel(), lt);
  }

  __ Bind(&conditions_on_positions_validated);

  if (!optimizations.GetSourceIsNotNull()) {
    // Bail out if the source is null.
    __ Cbz(src, slow_path->GetEntryLabel());
  }

  if (!optimizations.GetDestinationIsNotNull() && !optimizations.GetDestinationIsSource()) {
    // Bail out if the destination is null.
    __ Cbz(dest, slow_path->GetEntryLabel());
  }

  // We have already checked in the LocationsBuilder for the constant case.
  if (!length.IsConstant() &&
      !optimizations.GetCountIsSourceLength() &&
      !optimizations.GetCountIsDestinationLength()) {
    // If the length is negative, bail out.
    __ Tbnz(WRegisterFrom(length), kWRegSize - 1, slow_path->GetEntryLabel());
    // If the length >= 128 then (currently) prefer native implementation.
    __ Cmp(WRegisterFrom(length), kSystemArrayCopyThreshold);
    __ B(slow_path->GetEntryLabel(), ge);
  }
  // Validity checks: source.
  CheckSystemArrayCopyPosition(masm,
                               src_pos,
                               src,
                               length,
                               slow_path,
                               temp1,
                               temp2,
                               optimizations.GetCountIsSourceLength());

  // Validity checks: dest.
  CheckSystemArrayCopyPosition(masm,
                               dest_pos,
                               dest,
                               length,
                               slow_path,
                               temp1,
                               temp2,
                               optimizations.GetCountIsDestinationLength());
  {
    // We use a block to end the scratch scope before the write barrier, thus
    // freeing the temporary registers so they can be used in `MarkGCCard`.
    UseScratchRegisterScope temps(masm);
    Register temp3 = temps.AcquireW();
    if (!optimizations.GetDoesNotNeedTypeCheck()) {
      // Check whether all elements of the source array are assignable to the component
      // type of the destination array. We do two checks: the classes are the same,
      // or the destination is Object[]. If none of these checks succeed, we go to the
      // slow path.
      __ Ldr(temp1, MemOperand(dest, class_offset));
      __ Ldr(temp2, MemOperand(src, class_offset));
      bool did_unpoison = false;
      if (!optimizations.GetDestinationIsNonPrimitiveArray() ||
          !optimizations.GetSourceIsNonPrimitiveArray()) {
        // One or two of the references need to be unpoisoned. Unpoison them
        // both to make the identity check valid.
        codegen_->GetAssembler()->MaybeUnpoisonHeapReference(temp1);
        codegen_->GetAssembler()->MaybeUnpoisonHeapReference(temp2);
        did_unpoison = true;
      }

      if (!optimizations.GetDestinationIsNonPrimitiveArray()) {
        // Bail out if the destination is not a non primitive array.
        // /* HeapReference<Class> */ temp3 = temp1->component_type_
        __ Ldr(temp3, HeapOperand(temp1, component_offset));
        __ Cbz(temp3, slow_path->GetEntryLabel());
        codegen_->GetAssembler()->MaybeUnpoisonHeapReference(temp3);
        __ Ldrh(temp3, HeapOperand(temp3, primitive_offset));
        static_assert(Primitive::kPrimNot == 0, "Expected 0 for kPrimNot");
        __ Cbnz(temp3, slow_path->GetEntryLabel());
      }

      if (!optimizations.GetSourceIsNonPrimitiveArray()) {
        // Bail out if the source is not a non primitive array.
        // /* HeapReference<Class> */ temp3 = temp2->component_type_
        __ Ldr(temp3, HeapOperand(temp2, component_offset));
        __ Cbz(temp3, slow_path->GetEntryLabel());
        codegen_->GetAssembler()->MaybeUnpoisonHeapReference(temp3);
        __ Ldrh(temp3, HeapOperand(temp3, primitive_offset));
        static_assert(Primitive::kPrimNot == 0, "Expected 0 for kPrimNot");
        __ Cbnz(temp3, slow_path->GetEntryLabel());
      }

      __ Cmp(temp1, temp2);

      if (optimizations.GetDestinationIsTypedObjectArray()) {
        vixl::Label do_copy;
        __ B(&do_copy, eq);
        if (!did_unpoison) {
          codegen_->GetAssembler()->MaybeUnpoisonHeapReference(temp1);
        }
        // /* HeapReference<Class> */ temp1 = temp1->component_type_
        __ Ldr(temp1, HeapOperand(temp1, component_offset));
        codegen_->GetAssembler()->MaybeUnpoisonHeapReference(temp1);
        // /* HeapReference<Class> */ temp1 = temp1->super_class_
        __ Ldr(temp1, HeapOperand(temp1, super_offset));
        // No need to unpoison the result, we're comparing against null.
        __ Cbnz(temp1, slow_path->GetEntryLabel());
        __ Bind(&do_copy);
      } else {
        __ B(slow_path->GetEntryLabel(), ne);
      }
    } else if (!optimizations.GetSourceIsNonPrimitiveArray()) {
      DCHECK(optimizations.GetDestinationIsNonPrimitiveArray());
      // Bail out if the source is not a non primitive array.
      // /* HeapReference<Class> */ temp1 = src->klass_
      __ Ldr(temp1, HeapOperand(src.W(), class_offset));
      codegen_->GetAssembler()->MaybeUnpoisonHeapReference(temp1);
      // /* HeapReference<Class> */ temp3 = temp1->component_type_
      __ Ldr(temp3, HeapOperand(temp1, component_offset));
      __ Cbz(temp3, slow_path->GetEntryLabel());
      codegen_->GetAssembler()->MaybeUnpoisonHeapReference(temp3);
      __ Ldrh(temp3, HeapOperand(temp3, primitive_offset));
      static_assert(Primitive::kPrimNot == 0, "Expected 0 for kPrimNot");
      __ Cbnz(temp3, slow_path->GetEntryLabel());
    }

    Register src_curr_addr = temp1.X();
    Register dst_curr_addr = temp2.X();
    Register src_stop_addr = temp3.X();

    GenSystemArrayCopyAddresses(masm,
                                Primitive::kPrimNot,
                                src,
                                src_pos,
                                dest,
                                dest_pos,
                                length,
                                src_curr_addr,
                                dst_curr_addr,
                                src_stop_addr);

    // Iterate over the arrays and do a raw copy of the objects. We don't need to
    // poison/unpoison, nor do any read barrier as the next uses of the destination
    // array will do it.
    vixl::Label loop, done;
    const int32_t element_size = Primitive::ComponentSize(Primitive::kPrimNot);
    __ Bind(&loop);
    __ Cmp(src_curr_addr, src_stop_addr);
    __ B(&done, eq);
    {
      Register tmp = temps.AcquireW();
      __ Ldr(tmp, MemOperand(src_curr_addr, element_size, vixl::PostIndex));
      __ Str(tmp, MemOperand(dst_curr_addr, element_size, vixl::PostIndex));
    }
    __ B(&loop);
    __ Bind(&done);
  }
  // We only need one card marking on the destination array.
  codegen_->MarkGCCard(dest.W(), Register(), /* value_can_be_null */ false);

  __ Bind(slow_path->GetExitLabel());
}

UNIMPLEMENTED_INTRINSIC(ARM64, ReferenceGetReferent)
UNIMPLEMENTED_INTRINSIC(ARM64, FloatIsInfinite)
UNIMPLEMENTED_INTRINSIC(ARM64, DoubleIsInfinite)
UNIMPLEMENTED_INTRINSIC(ARM64, IntegerHighestOneBit)
UNIMPLEMENTED_INTRINSIC(ARM64, LongHighestOneBit)
UNIMPLEMENTED_INTRINSIC(ARM64, IntegerLowestOneBit)
UNIMPLEMENTED_INTRINSIC(ARM64, LongLowestOneBit)

// 1.8.
UNIMPLEMENTED_INTRINSIC(ARM64, UnsafeGetAndAddInt)
UNIMPLEMENTED_INTRINSIC(ARM64, UnsafeGetAndAddLong)
UNIMPLEMENTED_INTRINSIC(ARM64, UnsafeGetAndSetInt)
UNIMPLEMENTED_INTRINSIC(ARM64, UnsafeGetAndSetLong)
UNIMPLEMENTED_INTRINSIC(ARM64, UnsafeGetAndSetObject)

UNREACHABLE_INTRINSICS(ARM64)

#undef __

}  // namespace arm64
}  // namespace art
