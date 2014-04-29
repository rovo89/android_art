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

#include "entrypoints/quick/quick_entrypoints.h"
#include "mirror/array.h"
#include "mirror/art_method.h"
#include "thread.h"

#define __ reinterpret_cast<ArmAssembler*>(GetAssembler())->

namespace art {

arm::ArmManagedRegister Location::AsArm() const {
  return reg().AsArm();
}

namespace arm {

static constexpr int kNumberOfPushedRegistersAtEntry = 1;
static constexpr int kCurrentMethodStackOffset = 0;

CodeGeneratorARM::CodeGeneratorARM(HGraph* graph)
    : CodeGenerator(graph, kNumberOfRegIds),
      location_builder_(graph, this),
      instruction_visitor_(graph, this) {}

static bool* GetBlockedRegisterPairs(bool* blocked_registers) {
  return blocked_registers + kNumberOfAllocIds;
}

ManagedRegister CodeGeneratorARM::AllocateFreeRegister(Primitive::Type type,
                                                       bool* blocked_registers) const {
  switch (type) {
    case Primitive::kPrimLong: {
      size_t reg = AllocateFreeRegisterInternal(
          GetBlockedRegisterPairs(blocked_registers), kNumberOfRegisterPairs);
      ArmManagedRegister pair =
          ArmManagedRegister::FromRegisterPair(static_cast<RegisterPair>(reg));
      blocked_registers[pair.AsRegisterPairLow()] = true;
      blocked_registers[pair.AsRegisterPairHigh()] = true;
      return pair;
    }

    case Primitive::kPrimByte:
    case Primitive::kPrimBoolean:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      size_t reg = AllocateFreeRegisterInternal(blocked_registers, kNumberOfCoreRegisters);
      return ArmManagedRegister::FromCoreRegister(static_cast<Register>(reg));
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register type " << type;

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << type;
  }

  return ManagedRegister::NoRegister();
}

void CodeGeneratorARM::SetupBlockedRegisters(bool* blocked_registers) const {
  bool* blocked_register_pairs = GetBlockedRegisterPairs(blocked_registers);

  // Don't allocate the dalvik style register pair passing.
  blocked_register_pairs[R1_R2] = true;

  // Stack register, LR and PC are always reserved.
  blocked_registers[SP] = true;
  blocked_registers[LR] = true;
  blocked_registers[PC] = true;

  // Reserve R4 for suspend check.
  blocked_registers[R4] = true;
  blocked_register_pairs[R4_R5] = true;

  // Reserve thread register.
  blocked_registers[TR] = true;

  // TODO: We currently don't use Quick's callee saved registers.
  blocked_registers[R5] = true;
  blocked_registers[R6] = true;
  blocked_registers[R7] = true;
  blocked_registers[R8] = true;
  blocked_registers[R10] = true;
  blocked_registers[R11] = true;
  blocked_register_pairs[R6_R7] = true;
}

size_t CodeGeneratorARM::GetNumberOfRegisters() const {
  return kNumberOfRegIds;
}

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

  SetFrameSize(RoundUp(
      (GetGraph()->GetMaximumNumberOfOutVRegs() + GetGraph()->GetNumberOfVRegs()) * kVRegSize
      + kVRegSize  // filler
      + kArmWordSize  // Art method
      + kNumberOfPushedRegistersAtEntry * kArmWordSize,
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
                          + (reg_number - number_of_vregs + number_of_in_vregs) * kVRegSize;
  } else {
    // Local is a temporary in this method. It is stored in this method's frame.
    return GetFrameSize() - (kNumberOfPushedRegistersAtEntry * kArmWordSize)
                          - kVRegSize  // filler.
                          - (number_of_vregs * kVRegSize)
                          + (reg_number * kVRegSize);
  }
}

Location CodeGeneratorARM::GetStackLocation(HLoadLocal* load) const {
  switch (load->GetType()) {
    case Primitive::kPrimLong:
      return Location::DoubleStackSlot(GetStackSlot(load->GetLocal()));
      break;

    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
      return Location::StackSlot(GetStackSlot(load->GetLocal()));

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented type " << load->GetType();

    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unexpected type " << load->GetType();
  }

  LOG(FATAL) << "Unreachable";
  return Location();
}

Location InvokeDexCallingConventionVisitor::GetNextLocation(Primitive::Type type) {
  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      uint32_t index = gp_index_++;
      if (index < calling_convention.GetNumberOfRegisters()) {
        return ArmCoreLocation(calling_convention.GetRegisterAt(index));
      } else {
        return Location::StackSlot(calling_convention.GetStackOffsetOf(index, kArmWordSize));
      }
    }

    case Primitive::kPrimLong: {
      uint32_t index = gp_index_;
      gp_index_ += 2;
      if (index + 1 < calling_convention.GetNumberOfRegisters()) {
        return Location::RegisterLocation(ArmManagedRegister::FromRegisterPair(
            calling_convention.GetRegisterPairAt(index)));
      } else if (index + 1 == calling_convention.GetNumberOfRegisters()) {
        return Location::QuickParameter(index);
      } else {
        return Location::DoubleStackSlot(calling_convention.GetStackOffsetOf(index, kArmWordSize));
      }
    }

    case Primitive::kPrimDouble:
    case Primitive::kPrimFloat:
      LOG(FATAL) << "Unimplemented parameter type " << type;
      break;

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unexpected parameter type " << type;
      break;
  }
  return Location();
}

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
             Address(SP, calling_convention.GetStackOffsetOf(argument_index + 1, kArmWordSize) + GetFrameSize()));
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
             Address(SP, calling_convention.GetStackOffsetOf(argument_index + 1, kArmWordSize)));
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ ldr(calling_convention.GetRegisterAt(argument_index), Address(SP, source.GetStackIndex()));
      __ ldr(R0, Address(SP, source.GetHighStackIndex(kArmWordSize)));
      __ str(R0, Address(SP, calling_convention.GetStackOffsetOf(argument_index + 1, kArmWordSize)));
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
             Address(SP, calling_convention.GetStackOffsetOf(argument_index + 1, kArmWordSize) + GetFrameSize()));
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
  locations->SetInAt(0, Location::RequiresRegister());
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
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
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
      locations->SetInAt(
          0, Location::RegisterLocation(ArmManagedRegister::FromRegisterPair(R0_R1)));
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

void LocationsBuilderARM::VisitInvokeStatic(HInvokeStatic* invoke) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(invoke);
  locations->AddTemp(Location::RequiresRegister());

  InvokeDexCallingConventionVisitor calling_convention_visitor;
  for (size_t i = 0; i < invoke->InputCount(); i++) {
    HInstruction* input = invoke->InputAt(i);
    locations->SetInAt(i, calling_convention_visitor.GetNextLocation(input->GetType()));
  }

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
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister());
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
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister());
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
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(ArmCoreLocation(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(ArmCoreLocation(calling_convention.GetRegisterAt(1)));
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
  Location location = parameter_visitor_.GetNextLocation(instruction->GetType());
  if (location.IsStackSlot()) {
    location = Location::StackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
  } else if (location.IsDoubleStackSlot()) {
    location = Location::DoubleStackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
  }
  locations->SetOut(location);
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitParameterValue(HParameterValue* instruction) {
  // Nothing to do, the parameter is already at its location.
}

void LocationsBuilderARM::VisitNot(HNot* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitNot(HNot* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  __ eor(locations->Out().AsArm().AsCoreRegister(),
         locations->InAt(0).AsArm().AsCoreRegister(), ShifterOperand(1));
}

void LocationsBuilderARM::VisitPhi(HPhi* instruction) {
  LOG(FATAL) << "Unimplemented";
}

void InstructionCodeGeneratorARM::VisitPhi(HPhi* instruction) {
  LOG(FATAL) << "Unimplemented";
}

}  // namespace arm
}  // namespace art
