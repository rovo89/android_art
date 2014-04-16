/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "code_generator_arm.h"
#include "utils/assembler.h"
#include "utils/arm/assembler_arm.h"
#include "utils/arm/managed_register_arm.h"

#include "mirror/array.h"
#include "mirror/art_method.h"

#define __ reinterpret_cast<ArmAssembler*>(GetAssembler())->

namespace art {

arm::ArmManagedRegister Location::AsArm() const {
  return reg().AsArm();
}

namespace arm {

static constexpr int kNumberOfPushedRegistersAtEntry = 1;
static constexpr int kCurrentMethodStackOffset = 0;

static Location ArmCoreLocation(Register reg) {
  return Location::RegisterLocation(ArmManagedRegister::FromCoreRegister(reg));
}

InstructionCodeGeneratorARM::InstructionCodeGeneratorARM(HGraph* graph, CodeGeneratorARM* codegen)
      : HGraphVisitor(graph),
        assembler_(codegen->GetAssembler()),
        codegen_(codegen) {}

void CodeGeneratorARM::GenerateFrameEntry() {
  core_spill_mask_ |= (1 << LR);
  __ PushList((1 << LR));

  // Add the current ART method to the frame size, the return PC, and the filler.
  SetFrameSize(RoundUp((
      GetGraph()->GetMaximumNumberOfOutVRegs() + GetGraph()->GetNumberOfVRegs() + 3) * kArmWordSize,
      kStackAlignment));
  // The return PC has already been pushed on the stack.
  __ AddConstant(SP, -(GetFrameSize() - kNumberOfPushedRegistersAtEntry * kArmWordSize));
  __ str(R0, Address(SP, 0));
}

void CodeGeneratorARM::GenerateFrameExit() {
  __ AddConstant(SP, GetFrameSize() - kNumberOfPushedRegistersAtEntry * kArmWordSize);
  __ PopList((1 << PC));
}

void CodeGeneratorARM::Bind(Label* label) {
  __ Bind(label);
}

int32_t CodeGeneratorARM::GetStackSlot(HLocal* local) const {
  uint16_t reg_number = local->GetRegNumber();
  uint16_t number_of_vregs = GetGraph()->GetNumberOfVRegs();
  uint16_t number_of_in_vregs = GetGraph()->GetNumberOfInVRegs();
  if (reg_number >= number_of_vregs - number_of_in_vregs) {
    // Local is a parameter of the method. It is stored in the caller's frame.
    return GetFrameSize() + kArmWordSize  // ART method
                          + (reg_number - number_of_vregs + number_of_in_vregs) * kArmWordSize;
  } else {
    // Local is a temporary in this method. It is stored in this method's frame.
    return GetFrameSize() - (kNumberOfPushedRegistersAtEntry * kArmWordSize)
                          - kArmWordSize  // filler.
                          - (number_of_vregs * kArmWordSize)
                          + (reg_number * kArmWordSize);
  }
}

static constexpr Register kParameterCoreRegisters[] = { R1, R2, R3 };
static constexpr RegisterPair kParameterCorePairRegisters[] = { R1_R2, R2_R3 };
static constexpr size_t kParameterCoreRegistersLength = arraysize(kParameterCoreRegisters);

class InvokeDexCallingConvention : public CallingConvention<Register> {
 public:
  InvokeDexCallingConvention()
      : CallingConvention(kParameterCoreRegisters, kParameterCoreRegistersLength) {}

  RegisterPair GetRegisterPairAt(size_t argument_index) {
    DCHECK_LT(argument_index + 1, GetNumberOfRegisters());
    return kParameterCorePairRegisters[argument_index];
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConvention);
};

void CodeGeneratorARM::Move32(Location destination, Location source) {
  if (source.Equals(destination)) {
    return;
  }
  if (destination.IsRegister()) {
    if (source.IsRegister()) {
      __ Mov(destination.AsArm().AsCoreRegister(), source.AsArm().AsCoreRegister());
    } else {
      __ ldr(destination.AsArm().AsCoreRegister(), Address(SP, source.GetStackIndex()));
    }
  } else {
    DCHECK(destination.IsStackSlot());
    if (source.IsRegister()) {
      __ str(source.AsArm().AsCoreRegister(), Address(SP, destination.GetStackIndex()));
    } else {
      __ ldr(R0, Address(SP, source.GetStackIndex()));
      __ str(R0, Address(SP, destination.GetStackIndex()));
    }
  }
}

void CodeGeneratorARM::Move64(Location destination, Location source) {
  if (source.Equals(destination)) {
    return;
  }
  if (destination.IsRegister()) {
    if (source.IsRegister()) {
      __ Mov(destination.AsArm().AsRegisterPairLow(), source.AsArm().AsRegisterPairLow());
      __ Mov(destination.AsArm().AsRegisterPairHigh(), source.AsArm().AsRegisterPairHigh());
    } else if (source.IsQuickParameter()) {
      uint32_t argument_index = source.GetQuickParameterIndex();
      InvokeDexCallingConvention calling_convention;
      __ Mov(destination.AsArm().AsRegisterPairLow(),
             calling_convention.GetRegisterAt(argument_index));
      __ ldr(destination.AsArm().AsRegisterPairHigh(),
             Address(SP, calling_convention.GetStackOffsetOf(argument_index + 1) + GetFrameSize()));
    } else {
      DCHECK(source.IsDoubleStackSlot());
      if (destination.AsArm().AsRegisterPair() == R1_R2) {
        __ ldr(R1, Address(SP, source.GetStackIndex()));
        __ ldr(R2, Address(SP, source.GetHighStackIndex(kArmWordSize)));
      } else {
        __ LoadFromOffset(kLoadWordPair, destination.AsArm().AsRegisterPairLow(),
                          SP, source.GetStackIndex());
      }
    }
  } else if (destination.IsQuickParameter()) {
    InvokeDexCallingConvention calling_convention;
    uint32_t argument_index = destination.GetQuickParameterIndex();
    if (source.IsRegister()) {
      __ Mov(calling_convention.GetRegisterAt(argument_index), source.AsArm().AsRegisterPairLow());
      __ str(source.AsArm().AsRegisterPairHigh(),
             Address(SP, calling_convention.GetStackOffsetOf(argument_index + 1)));
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ ldr(calling_convention.GetRegisterAt(argument_index), Address(SP, source.GetStackIndex()));
      __ ldr(R0, Address(SP, source.GetHighStackIndex(kArmWordSize)));
      __ str(R0, Address(SP, calling_convention.GetStackOffsetOf(argument_index + 1)));
    }
  } else {
    DCHECK(destination.IsDoubleStackSlot());
    if (source.IsRegister()) {
      if (source.AsArm().AsRegisterPair() == R1_R2) {
        __ str(R1, Address(SP, destination.GetStackIndex()));
        __ str(R2, Address(SP, destination.GetHighStackIndex(kArmWordSize)));
      } else {
        __ StoreToOffset(kStoreWordPair, source.AsArm().AsRegisterPairLow(),
                         SP, destination.GetStackIndex());
      }
    } else if (source.IsQuickParameter()) {
      InvokeDexCallingConvention calling_convention;
      uint32_t argument_index = source.GetQuickParameterIndex();
      __ str(calling_convention.GetRegisterAt(argument_index),
             Address(SP, destination.GetStackIndex()));
      __ ldr(R0,
             Address(SP, calling_convention.GetStackOffsetOf(argument_index + 1) + GetFrameSize()));
      __ str(R0, Address(SP, destination.GetHighStackIndex(kArmWordSize)));
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ ldr(R0, Address(SP, source.GetStackIndex()));
      __ str(R0, Address(SP, destination.GetStackIndex()));
      __ ldr(R0, Address(SP, source.GetHighStackIndex(kArmWordSize)));
      __ str(R0, Address(SP, destination.GetHighStackIndex(kArmWordSize)));
    }
  }
}

void CodeGeneratorARM::Move(HInstruction* instruction, Location location, HInstruction* move_for) {
  if (instruction->AsIntConstant() != nullptr) {
    int32_t value = instruction->AsIntConstant()->GetValue();
    if (location.IsRegister()) {
      __ LoadImmediate(location.AsArm().AsCoreRegister(), value);
    } else {
      __ LoadImmediate(R0, value);
      __ str(R0, Address(SP, location.GetStackIndex()));
    }
  } else if (instruction->AsLongConstant() != nullptr) {
    int64_t value = instruction->AsLongConstant()->GetValue();
    if (location.IsRegister()) {
      __ LoadImmediate(location.AsArm().AsRegisterPairLow(), Low32Bits(value));
      __ LoadImmediate(location.AsArm().AsRegisterPairHigh(), High32Bits(value));
    } else {
      __ LoadImmediate(R0, Low32Bits(value));
      __ str(R0, Address(SP, location.GetStackIndex()));
      __ LoadImmediate(R0, High32Bits(value));
      __ str(R0, Address(SP, location.GetHighStackIndex(kArmWordSize)));
    }
  } else if (instruction->AsLoadLocal() != nullptr) {
    uint32_t stack_slot = GetStackSlot(instruction->AsLoadLocal()->GetLocal());
    switch (instruction->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
      case Primitive::kPrimNot:
        Move32(location, Location::StackSlot(stack_slot));
        break;

      case Primitive::kPrimLong:
        Move64(location, Location::DoubleStackSlot(stack_slot));
        break;

      default:
        LOG(FATAL) << "Unimplemented type " << instruction->GetType();
    }
  } else {
    // This can currently only happen when the instruction that requests the move
    // is the next to be compiled.
    DCHECK_EQ(instruction->GetNext(), move_for);
    switch (instruction->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimNot:
      case Primitive::kPrimInt:
        Move32(location, instruction->GetLocations()->Out());
        break;

      case Primitive::kPrimLong:
        Move64(location, instruction->GetLocations()->Out());
        break;

      default:
        LOG(FATAL) << "Unimplemented type " << instruction->GetType();
    }
  }
}

void LocationsBuilderARM::VisitGoto(HGoto* got) {
  got->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitGoto(HGoto* got) {
  HBasicBlock* successor = got->GetSuccessor();
  if (GetGraph()->GetExitBlock() == successor) {
    codegen_->GenerateFrameExit();
  } else if (!codegen_->GoesToNextBlock(got->GetBlock(), successor)) {
    __ b(codegen_->GetLabelOf(successor));
  }
}

void LocationsBuilderARM::VisitExit(HExit* exit) {
  exit->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitExit(HExit* exit) {
  if (kIsDebugBuild) {
    __ Comment("Unreachable");
    __ bkpt(0);
  }
}

void LocationsBuilderARM::VisitIf(HIf* if_instr) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(if_instr);
  locations->SetInAt(0, ArmCoreLocation(R0));
  if_instr->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitIf(HIf* if_instr) {
  // TODO: Generate the input as a condition, instead of materializing in a register.
  __ cmp(if_instr->GetLocations()->InAt(0).AsArm().AsCoreRegister(), ShifterOperand(0));
  __ b(codegen_->GetLabelOf(if_instr->IfFalseSuccessor()), EQ);
  if (!codegen_->GoesToNextBlock(if_instr->GetBlock(), if_instr->IfTrueSuccessor())) {
    __ b(codegen_->GetLabelOf(if_instr->IfTrueSuccessor()));
  }
}

void LocationsBuilderARM::VisitEqual(HEqual* equal) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(equal);
  locations->SetInAt(0, ArmCoreLocation(R0));
  locations->SetInAt(1, ArmCoreLocation(R1));
  locations->SetOut(ArmCoreLocation(R0));
  equal->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitEqual(HEqual* equal) {
  LocationSummary* locations = equal->GetLocations();
  __ teq(locations->InAt(0).AsArm().AsCoreRegister(),
         ShifterOperand(locations->InAt(1).AsArm().AsCoreRegister()));
  __ mov(locations->Out().AsArm().AsCoreRegister(), ShifterOperand(1), EQ);
  __ mov(locations->Out().AsArm().AsCoreRegister(), ShifterOperand(0), NE);
}

void LocationsBuilderARM::VisitLocal(HLocal* local) {
  local->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitLocal(HLocal* local) {
  DCHECK_EQ(local->GetBlock(), GetGraph()->GetEntryBlock());
}

void LocationsBuilderARM::VisitLoadLocal(HLoadLocal* load) {
  load->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitLoadLocal(HLoadLocal* load) {
  // Nothing to do, this is driven by the code generator.
}

void LocationsBuilderARM::VisitStoreLocal(HStoreLocal* store) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(store);
  switch (store->InputAt(1)->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
      locations->SetInAt(1, Location::StackSlot(codegen_->GetStackSlot(store->GetLocal())));
      break;

    case Primitive::kPrimLong:
      locations->SetInAt(1, Location::DoubleStackSlot(codegen_->GetStackSlot(store->GetLocal())));
      break;

    default:
      LOG(FATAL) << "Unimplemented local type " << store->InputAt(1)->GetType();
  }
  store->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitStoreLocal(HStoreLocal* store) {
}

void LocationsBuilderARM::VisitIntConstant(HIntConstant* constant) {
  constant->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitIntConstant(HIntConstant* constant) {
  // Will be generated at use site.
}

void LocationsBuilderARM::VisitLongConstant(HLongConstant* constant) {
  constant->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitLongConstant(HLongConstant* constant) {
  // Will be generated at use site.
}

void LocationsBuilderARM::VisitReturnVoid(HReturnVoid* ret) {
  ret->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitReturnVoid(HReturnVoid* ret) {
  codegen_->GenerateFrameExit();
}

void LocationsBuilderARM::VisitReturn(HReturn* ret) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(ret);
  switch (ret->InputAt(0)->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
      locations->SetInAt(0, ArmCoreLocation(R0));
      break;

    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RegisterLocation(ArmManagedRegister::FromRegisterPair(R0_R1)));
      break;

    default:
      LOG(FATAL) << "Unimplemented return type " << ret->InputAt(0)->GetType();
  }

  ret->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitReturn(HReturn* ret) {
  if (kIsDebugBuild) {
    switch (ret->InputAt(0)->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
      case Primitive::kPrimNot:
        DCHECK_EQ(ret->GetLocations()->InAt(0).AsArm().AsCoreRegister(), R0);
        break;

      case Primitive::kPrimLong:
        DCHECK_EQ(ret->GetLocations()->InAt(0).AsArm().AsRegisterPair(), R0_R1);
        break;

      default:
        LOG(FATAL) << "Unimplemented return type " << ret->InputAt(0)->GetType();
    }
  }
  codegen_->GenerateFrameExit();
}

void LocationsBuilderARM::VisitPushArgument(HPushArgument* argument) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(argument);
  InvokeDexCallingConvention calling_convention;
  uint32_t argument_index = argument->GetArgumentIndex();
  switch (argument->InputAt(0)->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      if (argument_index < calling_convention.GetNumberOfRegisters()) {
        locations->SetInAt(0, ArmCoreLocation(calling_convention.GetRegisterAt(argument_index)));
      } else {
        locations->SetInAt(
            0, Location::StackSlot(calling_convention.GetStackOffsetOf(argument_index)));
      }
      break;
    }
    case Primitive::kPrimLong: {
      if (argument_index + 1 < calling_convention.GetNumberOfRegisters()) {
        Location location = Location::RegisterLocation(ArmManagedRegister::FromRegisterPair(
            calling_convention.GetRegisterPairAt(argument_index)));
        locations->SetInAt(0, location);
      } else if (argument_index + 1 == calling_convention.GetNumberOfRegisters()) {
        locations->SetInAt(0, Location::QuickParameter(argument_index));
      } else {
        locations->SetInAt(
            0, Location::DoubleStackSlot(calling_convention.GetStackOffsetOf(argument_index)));
      }
      break;
    }
    default:
      LOG(FATAL) << "Unimplemented argument type " << argument->InputAt(0)->GetType();
  }
  argument->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitPushArgument(HPushArgument* argument) {
  // Nothing to do.
}

void LocationsBuilderARM::VisitInvokeStatic(HInvokeStatic* invoke) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(invoke);
  locations->AddTemp(ArmCoreLocation(R0));
    switch (invoke->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
      locations->SetOut(ArmCoreLocation(R0));
      break;

    case Primitive::kPrimLong:
      locations->SetOut(Location::RegisterLocation(ArmManagedRegister::FromRegisterPair(R0_R1)));
      break;

    case Primitive::kPrimVoid:
      break;

    case Primitive::kPrimDouble:
    case Primitive::kPrimFloat:
      LOG(FATAL) << "Unimplemented return type " << invoke->GetType();
      break;
  }

  invoke->SetLocations(locations);
}

void InstructionCodeGeneratorARM::LoadCurrentMethod(Register reg) {
  __ ldr(reg, Address(SP, kCurrentMethodStackOffset));
}

void InstructionCodeGeneratorARM::VisitInvokeStatic(HInvokeStatic* invoke) {
  Register temp = invoke->GetLocations()->GetTemp(0).AsArm().AsCoreRegister();
  size_t index_in_cache = mirror::Array::DataOffset(sizeof(mirror::Object*)).Int32Value() +
      invoke->GetIndexInDexCache() * kArmWordSize;

  // TODO: Implement all kinds of calls:
  // 1) boot -> boot
  // 2) app -> boot
  // 3) app -> app
  //
  // Currently we implement the app -> app logic, which looks up in the resolve cache.

  // temp = method;
  LoadCurrentMethod(temp);
  // temp = temp->dex_cache_resolved_methods_;
  __ ldr(temp, Address(temp, mirror::ArtMethod::DexCacheResolvedMethodsOffset().Int32Value()));
  // temp = temp[index_in_cache]
  __ ldr(temp, Address(temp, index_in_cache));
  // LR = temp[offset_of_quick_compiled_code]
  __ ldr(LR, Address(temp,
                     mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset().Int32Value()));
  // LR()
  __ blx(LR);

  codegen_->RecordPcInfo(invoke->GetDexPc());
}

void LocationsBuilderARM::VisitAdd(HAdd* add) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(add);
  switch (add->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, ArmCoreLocation(R0));
      locations->SetInAt(1, ArmCoreLocation(R1));
      locations->SetOut(ArmCoreLocation(R0));
      break;
    }

    case Primitive::kPrimLong: {
      locations->SetInAt(
          0, Location::RegisterLocation(ArmManagedRegister::FromRegisterPair(R0_R1)));
      locations->SetInAt(
          1, Location::RegisterLocation(ArmManagedRegister::FromRegisterPair(R2_R3)));
      locations->SetOut(Location::RegisterLocation(ArmManagedRegister::FromRegisterPair(R0_R1)));
      break;
    }

    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
      LOG(FATAL) << "Unexpected add type " << add->GetResultType();
      break;

    default:
      LOG(FATAL) << "Unimplemented add type " << add->GetResultType();
  }
  add->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitAdd(HAdd* add) {
  LocationSummary* locations = add->GetLocations();
  switch (add->GetResultType()) {
    case Primitive::kPrimInt:
      __ add(locations->Out().AsArm().AsCoreRegister(),
             locations->InAt(0).AsArm().AsCoreRegister(),
             ShifterOperand(locations->InAt(1).AsArm().AsCoreRegister()));
      break;

    case Primitive::kPrimLong:
      __ adds(locations->Out().AsArm().AsRegisterPairLow(),
              locations->InAt(0).AsArm().AsRegisterPairLow(),
              ShifterOperand(locations->InAt(1).AsArm().AsRegisterPairLow()));
      __ adc(locations->Out().AsArm().AsRegisterPairHigh(),
             locations->InAt(0).AsArm().AsRegisterPairHigh(),
             ShifterOperand(locations->InAt(1).AsArm().AsRegisterPairHigh()));
      break;

    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
      LOG(FATAL) << "Unexpected add type " << add->GetResultType();
      break;

    default:
      LOG(FATAL) << "Unimplemented add type " << add->GetResultType();
  }
}

void LocationsBuilderARM::VisitSub(HSub* sub) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(sub);
  switch (sub->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, ArmCoreLocation(R0));
      locations->SetInAt(1, ArmCoreLocation(R1));
      locations->SetOut(ArmCoreLocation(R0));
      break;
    }

    case Primitive::kPrimLong: {
      locations->SetInAt(
          0, Location::RegisterLocation(ArmManagedRegister::FromRegisterPair(R0_R1)));
      locations->SetInAt(
          1, Location::RegisterLocation(ArmManagedRegister::FromRegisterPair(R2_R3)));
      locations->SetOut(Location::RegisterLocation(ArmManagedRegister::FromRegisterPair(R0_R1)));
      break;
    }

    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
      LOG(FATAL) << "Unexpected sub type " << sub->GetResultType();
      break;

    default:
      LOG(FATAL) << "Unimplemented sub type " << sub->GetResultType();
  }
  sub->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitSub(HSub* sub) {
  LocationSummary* locations = sub->GetLocations();
  switch (sub->GetResultType()) {
    case Primitive::kPrimInt:
      __ sub(locations->Out().AsArm().AsCoreRegister(),
             locations->InAt(0).AsArm().AsCoreRegister(),
             ShifterOperand(locations->InAt(1).AsArm().AsCoreRegister()));
      break;

    case Primitive::kPrimLong:
      __ subs(locations->Out().AsArm().AsRegisterPairLow(),
              locations->InAt(0).AsArm().AsRegisterPairLow(),
              ShifterOperand(locations->InAt(1).AsArm().AsRegisterPairLow()));
      __ sbc(locations->Out().AsArm().AsRegisterPairHigh(),
             locations->InAt(0).AsArm().AsRegisterPairHigh(),
             ShifterOperand(locations->InAt(1).AsArm().AsRegisterPairHigh()));
      break;

    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
      LOG(FATAL) << "Unexpected sub type " << sub->GetResultType();
      break;

    default:
      LOG(FATAL) << "Unimplemented sub type " << sub->GetResultType();
  }
}

static constexpr Register kRuntimeParameterCoreRegisters[] = { R0, R1 };
static constexpr size_t kRuntimeParameterCoreRegistersLength =
    arraysize(kRuntimeParameterCoreRegisters);

class InvokeRuntimeCallingConvention : public CallingConvention<Register> {
 public:
  InvokeRuntimeCallingConvention()
      : CallingConvention(kRuntimeParameterCoreRegisters,
                          kRuntimeParameterCoreRegistersLength) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeRuntimeCallingConvention);
};

void LocationsBuilderARM::VisitNewInstance(HNewInstance* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetOut(ArmCoreLocation(R0));
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitNewInstance(HNewInstance* instruction) {
  InvokeRuntimeCallingConvention calling_convention;
  LoadCurrentMethod(calling_convention.GetRegisterAt(1));
  __ LoadImmediate(calling_convention.GetRegisterAt(0), instruction->GetTypeIndex());

  int32_t offset = QUICK_ENTRYPOINT_OFFSET(kArmWordSize, pAllocObjectWithAccessCheck).Int32Value();
  __ ldr(LR, Address(TR, offset));
  __ blx(LR);

  codegen_->RecordPcInfo(instruction->GetDexPc());
}

void LocationsBuilderARM::VisitParameterValue(HParameterValue* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  InvokeDexCallingConvention calling_convention;
  uint32_t argument_index = instruction->GetIndex();
  switch (instruction->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
      if (argument_index < calling_convention.GetNumberOfRegisters()) {
        locations->SetOut(ArmCoreLocation(calling_convention.GetRegisterAt(argument_index)));
      } else {
        locations->SetOut(Location::StackSlot(
            calling_convention.GetStackOffsetOf(argument_index) + codegen_->GetFrameSize()));
      }
      break;

    case Primitive::kPrimLong:
      if (argument_index + 1 < calling_convention.GetNumberOfRegisters()) {
        locations->SetOut(Location::RegisterLocation(ArmManagedRegister::FromRegisterPair(
            (calling_convention.GetRegisterPairAt(argument_index)))));
      } else if (argument_index + 1 == calling_convention.GetNumberOfRegisters()) {
        // Spanning a register and a stack slot. Use the quick parameter kind.
        locations->SetOut(Location::QuickParameter(argument_index));
      } else {
        locations->SetOut(Location::DoubleStackSlot(
            calling_convention.GetStackOffsetOf(argument_index) + codegen_->GetFrameSize()));
      }
      break;

    default:
      LOG(FATAL) << "Unimplemented parameter type " << instruction->GetType();
  }
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitParameterValue(HParameterValue* instruction) {
  // Nothing to do, the parameter is already at its location.
}

void LocationsBuilderARM::VisitNot(HNot* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, ArmCoreLocation(R0));
  locations->SetOut(ArmCoreLocation(R0));
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitNot(HNot* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  __ eor(locations->Out().AsArm().AsCoreRegister(),
         locations->InAt(0).AsArm().AsCoreRegister(), ShifterOperand(1));
}

}  // namespace arm
}  // namespace art
