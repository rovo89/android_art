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

#include "entrypoints/quick/quick_entrypoints.h"
#include "gc/accounting/card_table.h"
#include "mirror/array.h"
#include "mirror/art_method.h"
#include "thread.h"
#include "utils/assembler.h"
#include "utils/arm/assembler_arm.h"
#include "utils/arm/managed_register_arm.h"
#include "utils/stack_checks.h"

namespace art {

arm::ArmManagedRegister Location::AsArm() const {
  return reg().AsArm();
}

namespace arm {

static constexpr bool kExplicitStackOverflowCheck = false;

static constexpr int kNumberOfPushedRegistersAtEntry = 1 + 2;  // LR, R6, R7
static constexpr int kCurrentMethodStackOffset = 0;

static Location ArmCoreLocation(Register reg) {
  return Location::RegisterLocation(ArmManagedRegister::FromCoreRegister(reg));
}

static constexpr Register kRuntimeParameterCoreRegisters[] = { R0, R1, R2 };
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

#define __ reinterpret_cast<ArmAssembler*>(codegen->GetAssembler())->

class NullCheckSlowPathARM : public SlowPathCode {
 public:
  explicit NullCheckSlowPathARM(uint32_t dex_pc) : dex_pc_(dex_pc) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    int32_t offset = QUICK_ENTRYPOINT_OFFSET(kArmWordSize, pThrowNullPointer).Int32Value();
    __ ldr(LR, Address(TR, offset));
    __ blx(LR);
    codegen->RecordPcInfo(dex_pc_);
  }

 private:
  const uint32_t dex_pc_;
  DISALLOW_COPY_AND_ASSIGN(NullCheckSlowPathARM);
};

class StackOverflowCheckSlowPathARM : public SlowPathCode {
 public:
  StackOverflowCheckSlowPathARM() {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    __ LoadFromOffset(kLoadWord, PC, TR,
        QUICK_ENTRYPOINT_OFFSET(kArmWordSize, pThrowStackOverflow).Int32Value());
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(StackOverflowCheckSlowPathARM);
};

class BoundsCheckSlowPathARM : public SlowPathCode {
 public:
  explicit BoundsCheckSlowPathARM(uint32_t dex_pc,
                                  Location index_location,
                                  Location length_location)
      : dex_pc_(dex_pc), index_location_(index_location), length_location_(length_location) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM* arm_codegen = reinterpret_cast<CodeGeneratorARM*>(codegen);
    __ Bind(GetEntryLabel());
    InvokeRuntimeCallingConvention calling_convention;
    arm_codegen->Move32(ArmCoreLocation(calling_convention.GetRegisterAt(0)), index_location_);
    arm_codegen->Move32(ArmCoreLocation(calling_convention.GetRegisterAt(1)), length_location_);
    int32_t offset = QUICK_ENTRYPOINT_OFFSET(kArmWordSize, pThrowArrayBounds).Int32Value();
    __ ldr(LR, Address(TR, offset));
    __ blx(LR);
    codegen->RecordPcInfo(dex_pc_);
  }

 private:
  const uint32_t dex_pc_;
  const Location index_location_;
  const Location length_location_;

  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathARM);
};

#undef __
#define __ reinterpret_cast<ArmAssembler*>(GetAssembler())->

inline Condition ARMCondition(IfCondition cond) {
  switch (cond) {
    case kCondEQ: return EQ;
    case kCondNE: return NE;
    case kCondLT: return LT;
    case kCondLE: return LE;
    case kCondGT: return GT;
    case kCondGE: return GE;
    default:
      LOG(FATAL) << "Unknown if condition";
  }
  return EQ;        // Unreachable.
}

inline Condition ARMOppositeCondition(IfCondition cond) {
  switch (cond) {
    case kCondEQ: return NE;
    case kCondNE: return EQ;
    case kCondLT: return GE;
    case kCondLE: return GT;
    case kCondGT: return LE;
    case kCondGE: return LT;
    default:
      LOG(FATAL) << "Unknown if condition";
  }
  return EQ;        // Unreachable.
}

void CodeGeneratorARM::DumpCoreRegister(std::ostream& stream, int reg) const {
  stream << ArmManagedRegister::FromCoreRegister(Register(reg));
}

void CodeGeneratorARM::DumpFloatingPointRegister(std::ostream& stream, int reg) const {
  stream << ArmManagedRegister::FromDRegister(DRegister(reg));
}

CodeGeneratorARM::CodeGeneratorARM(HGraph* graph)
    : CodeGenerator(graph, kNumberOfRegIds),
      location_builder_(graph, this),
      instruction_visitor_(graph, this),
      move_resolver_(graph->GetArena(), this),
      assembler_(true) {}

size_t CodeGeneratorARM::FrameEntrySpillSize() const {
  return kNumberOfPushedRegistersAtEntry * kArmWordSize;
}

static bool* GetBlockedRegisterPairs(bool* blocked_registers) {
  return blocked_registers + kNumberOfAllocIds;
}

ManagedRegister CodeGeneratorARM::AllocateFreeRegister(Primitive::Type type,
                                                       bool* blocked_registers) const {
  switch (type) {
    case Primitive::kPrimLong: {
      bool* blocked_register_pairs = GetBlockedRegisterPairs(blocked_registers);
      size_t reg = AllocateFreeRegisterInternal(blocked_register_pairs, kNumberOfRegisterPairs);
      ArmManagedRegister pair =
          ArmManagedRegister::FromRegisterPair(static_cast<RegisterPair>(reg));
      blocked_registers[pair.AsRegisterPairLow()] = true;
      blocked_registers[pair.AsRegisterPairHigh()] = true;
       // Block all other register pairs that share a register with `pair`.
      for (int i = 0; i < kNumberOfRegisterPairs; i++) {
        ArmManagedRegister current =
            ArmManagedRegister::FromRegisterPair(static_cast<RegisterPair>(i));
        if (current.AsRegisterPairLow() == pair.AsRegisterPairLow()
            || current.AsRegisterPairLow() == pair.AsRegisterPairHigh()
            || current.AsRegisterPairHigh() == pair.AsRegisterPairLow()
            || current.AsRegisterPairHigh() == pair.AsRegisterPairHigh()) {
          blocked_register_pairs[i] = true;
        }
      }
      return pair;
    }

    case Primitive::kPrimByte:
    case Primitive::kPrimBoolean:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      int reg = AllocateFreeRegisterInternal(blocked_registers, kNumberOfCoreRegisters);
      // Block all register pairs that contain `reg`.
      bool* blocked_register_pairs = GetBlockedRegisterPairs(blocked_registers);
      for (int i = 0; i < kNumberOfRegisterPairs; i++) {
        ArmManagedRegister current =
            ArmManagedRegister::FromRegisterPair(static_cast<RegisterPair>(i));
        if (current.AsRegisterPairLow() == reg || current.AsRegisterPairHigh() == reg) {
          blocked_register_pairs[i] = true;
        }
      }
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

  // Reserve temp register.
  blocked_registers[IP] = true;

  // TODO: We currently don't use Quick's callee saved registers.
  // We always save and restore R6 and R7 to make sure we can use three
  // register pairs for long operations.
  blocked_registers[R5] = true;
  blocked_registers[R8] = true;
  blocked_registers[R10] = true;
  blocked_registers[R11] = true;
}

size_t CodeGeneratorARM::GetNumberOfRegisters() const {
  return kNumberOfRegIds;
}

InstructionCodeGeneratorARM::InstructionCodeGeneratorARM(HGraph* graph, CodeGeneratorARM* codegen)
      : HGraphVisitor(graph),
        assembler_(codegen->GetAssembler()),
        codegen_(codegen) {}

void CodeGeneratorARM::GenerateFrameEntry() {
  bool skip_overflow_check = IsLeafMethod() && !FrameNeedsStackCheck(GetFrameSize(), InstructionSet::kArm);
  if (!skip_overflow_check) {
    if (kExplicitStackOverflowCheck) {
      SlowPathCode* slow_path = new (GetGraph()->GetArena()) StackOverflowCheckSlowPathARM();
      AddSlowPath(slow_path);

      __ LoadFromOffset(kLoadWord, IP, TR, Thread::StackEndOffset<kArmWordSize>().Int32Value());
      __ cmp(SP, ShifterOperand(IP));
      __ b(slow_path->GetEntryLabel(), CC);
    } else {
      __ AddConstant(IP, SP, -static_cast<int32_t>(GetStackOverflowReservedBytes(kArm)));
      __ ldr(IP, Address(IP, 0));
      RecordPcInfo(0);
    }
  }

  core_spill_mask_ |= (1 << LR | 1 << R6 | 1 << R7);
  __ PushList(1 << LR | 1 << R6 | 1 << R7);

  // The return PC has already been pushed on the stack.
  __ AddConstant(SP, -(GetFrameSize() - kNumberOfPushedRegistersAtEntry * kArmWordSize));
  __ str(R0, Address(SP, 0));
}

void CodeGeneratorARM::GenerateFrameExit() {
  __ AddConstant(SP, GetFrameSize() - kNumberOfPushedRegistersAtEntry * kArmWordSize);
  __ PopList(1 << PC | 1 << R6 | 1 << R7);
}

void CodeGeneratorARM::Bind(Label* label) {
  __ Bind(label);
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
        return Location::StackSlot(calling_convention.GetStackOffsetOf(index));
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
        return Location::DoubleStackSlot(calling_convention.GetStackOffsetOf(index));
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
      __ ldr(IP, Address(SP, source.GetStackIndex()));
      __ str(IP, Address(SP, destination.GetStackIndex()));
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
      __ ldr(IP, Address(SP, source.GetStackIndex()));
      __ str(IP, Address(SP, destination.GetStackIndex()));
      __ ldr(IP, Address(SP, source.GetHighStackIndex(kArmWordSize)));
      __ str(IP, Address(SP, destination.GetHighStackIndex(kArmWordSize)));
    }
  }
}

void CodeGeneratorARM::Move(HInstruction* instruction, Location location, HInstruction* move_for) {
  LocationSummary* locations = instruction->GetLocations();
  if (locations != nullptr && locations->Out().Equals(location)) {
    return;
  }

  if (instruction->AsIntConstant() != nullptr) {
    int32_t value = instruction->AsIntConstant()->GetValue();
    if (location.IsRegister()) {
      __ LoadImmediate(location.AsArm().AsCoreRegister(), value);
    } else {
      DCHECK(location.IsStackSlot());
      __ LoadImmediate(IP, value);
      __ str(IP, Address(SP, location.GetStackIndex()));
    }
  } else if (instruction->AsLongConstant() != nullptr) {
    int64_t value = instruction->AsLongConstant()->GetValue();
    if (location.IsRegister()) {
      __ LoadImmediate(location.AsArm().AsRegisterPairLow(), Low32Bits(value));
      __ LoadImmediate(location.AsArm().AsRegisterPairHigh(), High32Bits(value));
    } else {
      DCHECK(location.IsDoubleStackSlot());
      __ LoadImmediate(IP, Low32Bits(value));
      __ str(IP, Address(SP, location.GetStackIndex()));
      __ LoadImmediate(IP, High32Bits(value));
      __ str(IP, Address(SP, location.GetHighStackIndex(kArmWordSize)));
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
    DCHECK((instruction->GetNext() == move_for) || instruction->GetNext()->IsTemporary());
    switch (instruction->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimNot:
      case Primitive::kPrimInt:
        Move32(location, locations->Out());
        break;

      case Primitive::kPrimLong:
        Move64(location, locations->Out());
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
  HInstruction* cond = if_instr->InputAt(0);
  DCHECK(cond->IsCondition());
  HCondition* condition = cond->AsCondition();
  if (condition->NeedsMaterialization()) {
    locations->SetInAt(0, Location::Any());
  }
  if_instr->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitIf(HIf* if_instr) {
  HInstruction* cond = if_instr->InputAt(0);
  DCHECK(cond->IsCondition());
  HCondition* condition = cond->AsCondition();
  if (condition->NeedsMaterialization()) {
    // Condition has been materialized, compare the output to 0
    DCHECK(if_instr->GetLocations()->InAt(0).IsRegister());
    __ cmp(if_instr->GetLocations()->InAt(0).AsArm().AsCoreRegister(),
           ShifterOperand(0));
    __ b(codegen_->GetLabelOf(if_instr->IfTrueSuccessor()), EQ);
  } else {
    // Condition has not been materialized, use its inputs as the comparison and its
    // condition as the branch condition.
    LocationSummary* locations = condition->GetLocations();
    if (locations->InAt(1).IsRegister()) {
      __ cmp(locations->InAt(0).AsArm().AsCoreRegister(),
             ShifterOperand(locations->InAt(1).AsArm().AsCoreRegister()));
    } else {
      DCHECK(locations->InAt(1).IsConstant());
      int32_t value = locations->InAt(1).GetConstant()->AsIntConstant()->GetValue();
      ShifterOperand operand;
      if (ShifterOperand::CanHoldArm(value, &operand)) {
        __ cmp(locations->InAt(0).AsArm().AsCoreRegister(), ShifterOperand(value));
      } else {
        Register temp = IP;
        __ LoadImmediate(temp, value);
        __ cmp(locations->InAt(0).AsArm().AsCoreRegister(), ShifterOperand(temp));
      }
    }
    __ b(codegen_->GetLabelOf(if_instr->IfTrueSuccessor()),
         ARMCondition(condition->GetCondition()));
  }

  if (!codegen_->GoesToNextBlock(if_instr->GetBlock(), if_instr->IfFalseSuccessor())) {
    __ b(codegen_->GetLabelOf(if_instr->IfFalseSuccessor()));
  }
}


void LocationsBuilderARM::VisitCondition(HCondition* comp) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(comp);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(comp->InputAt(1)));
  if (comp->NeedsMaterialization()) {
    locations->SetOut(Location::RequiresRegister());
  }
  comp->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitCondition(HCondition* comp) {
  if (!comp->NeedsMaterialization()) return;

  LocationSummary* locations = comp->GetLocations();
  if (locations->InAt(1).IsRegister()) {
    __ cmp(locations->InAt(0).AsArm().AsCoreRegister(),
           ShifterOperand(locations->InAt(1).AsArm().AsCoreRegister()));
  } else {
    DCHECK(locations->InAt(1).IsConstant());
    int32_t value = locations->InAt(1).GetConstant()->AsIntConstant()->GetValue();
    ShifterOperand operand;
    if (ShifterOperand::CanHoldArm(value, &operand)) {
      __ cmp(locations->InAt(0).AsArm().AsCoreRegister(), ShifterOperand(value));
    } else {
      Register temp = IP;
      __ LoadImmediate(temp, value);
      __ cmp(locations->InAt(0).AsArm().AsCoreRegister(), ShifterOperand(temp));
    }
  }
  __ it(ARMCondition(comp->GetCondition()), kItElse);
  __ mov(locations->Out().AsArm().AsCoreRegister(), ShifterOperand(1),
         ARMCondition(comp->GetCondition()));
  __ mov(locations->Out().AsArm().AsCoreRegister(), ShifterOperand(0),
         ARMOppositeCondition(comp->GetCondition()));
}

void LocationsBuilderARM::VisitEqual(HEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorARM::VisitEqual(HEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderARM::VisitNotEqual(HNotEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorARM::VisitNotEqual(HNotEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderARM::VisitLessThan(HLessThan* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorARM::VisitLessThan(HLessThan* comp) {
  VisitCondition(comp);
}

void LocationsBuilderARM::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorARM::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderARM::VisitGreaterThan(HGreaterThan* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorARM::VisitGreaterThan(HGreaterThan* comp) {
  VisitCondition(comp);
}

void LocationsBuilderARM::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorARM::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  VisitCondition(comp);
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
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
  constant->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitIntConstant(HIntConstant* constant) {
}

void LocationsBuilderARM::VisitLongConstant(HLongConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
  constant->SetLocations(locations);
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
  codegen_->MarkNotLeaf();
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(invoke);
  locations->AddTemp(ArmCoreLocation(R0));

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
  uint32_t heap_reference_size = sizeof(mirror::HeapReference<mirror::Object>);
  size_t index_in_cache = mirror::Array::DataOffset(heap_reference_size).Int32Value() +
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
  DCHECK(!codegen_->IsLeafMethod());
}

void LocationsBuilderARM::VisitAdd(HAdd* add) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(add);
  switch (add->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(add->InputAt(1)));
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
      if (locations->InAt(1).IsRegister()) {
        __ add(locations->Out().AsArm().AsCoreRegister(),
               locations->InAt(0).AsArm().AsCoreRegister(),
               ShifterOperand(locations->InAt(1).AsArm().AsCoreRegister()));
      } else {
        __ AddConstant(locations->Out().AsArm().AsCoreRegister(),
                       locations->InAt(0).AsArm().AsCoreRegister(),
                       locations->InAt(1).GetConstant()->AsIntConstant()->GetValue());
      }
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
      locations->SetInAt(1, Location::RegisterOrConstant(sub->InputAt(1)));
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
    case Primitive::kPrimInt: {
      if (locations->InAt(1).IsRegister()) {
        __ sub(locations->Out().AsArm().AsCoreRegister(),
               locations->InAt(0).AsArm().AsCoreRegister(),
               ShifterOperand(locations->InAt(1).AsArm().AsCoreRegister()));
      } else {
        __ AddConstant(locations->Out().AsArm().AsCoreRegister(),
                       locations->InAt(0).AsArm().AsCoreRegister(),
                       -locations->InAt(1).GetConstant()->AsIntConstant()->GetValue());
      }
      break;
    }

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

void LocationsBuilderARM::VisitNewInstance(HNewInstance* instruction) {
  codegen_->MarkNotLeaf();
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
  DCHECK(!codegen_->IsLeafMethod());
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

void LocationsBuilderARM::VisitCompare(HCompare* compare) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(compare);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
  compare->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitCompare(HCompare* compare) {
  Label greater, done;
  LocationSummary* locations = compare->GetLocations();
  switch (compare->InputAt(0)->GetType()) {
    case Primitive::kPrimLong: {
      Register output = locations->Out().AsArm().AsCoreRegister();
      ArmManagedRegister left = locations->InAt(0).AsArm();
      ArmManagedRegister right = locations->InAt(1).AsArm();
      Label less, greater, done;
      __ cmp(left.AsRegisterPairHigh(),
             ShifterOperand(right.AsRegisterPairHigh()));  // Signed compare.
      __ b(&less, LT);
      __ b(&greater, GT);
      // Do LoadImmediate before any `cmp`, as LoadImmediate might affect
      // the status flags.
      __ LoadImmediate(output, 0);
      __ cmp(left.AsRegisterPairLow(),
             ShifterOperand(right.AsRegisterPairLow()));  // Unsigned compare.
      __ b(&done, EQ);
      __ b(&less, CC);

      __ Bind(&greater);
      __ LoadImmediate(output, 1);
      __ b(&done);

      __ Bind(&less);
      __ LoadImmediate(output, -1);

      __ Bind(&done);
      break;
    }
    default:
      LOG(FATAL) << "Unimplemented compare type " << compare->InputAt(0)->GetType();
  }
}

void LocationsBuilderARM::VisitPhi(HPhi* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    locations->SetInAt(i, Location::Any());
  }
  locations->SetOut(Location::Any());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitPhi(HPhi* instruction) {
  LOG(FATAL) << "Unreachable";
}

void LocationsBuilderARM::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  // Temporary registers for the write barrier.
  if (instruction->InputAt(1)->GetType() == Primitive::kPrimNot) {
    locations->AddTemp(Location::RequiresRegister());
    locations->AddTemp(Location::RequiresRegister());
  }
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsArm().AsCoreRegister();
  uint32_t offset = instruction->GetFieldOffset().Uint32Value();
  Primitive::Type field_type = instruction->InputAt(1)->GetType();

  switch (field_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      Register value = locations->InAt(1).AsArm().AsCoreRegister();
      __ StoreToOffset(kStoreByte, value, obj, offset);
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      Register value = locations->InAt(1).AsArm().AsCoreRegister();
      __ StoreToOffset(kStoreHalfword, value, obj, offset);
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      Register value = locations->InAt(1).AsArm().AsCoreRegister();
      __ StoreToOffset(kStoreWord, value, obj, offset);
      if (field_type == Primitive::kPrimNot) {
        Register temp = locations->GetTemp(0).AsArm().AsCoreRegister();
        Register card = locations->GetTemp(1).AsArm().AsCoreRegister();
        codegen_->MarkGCCard(temp, card, obj, value);
      }
      break;
    }

    case Primitive::kPrimLong: {
      ArmManagedRegister value = locations->InAt(1).AsArm();
      __ StoreToOffset(kStoreWordPair, value.AsRegisterPairLow(), obj, offset);
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register type " << field_type;

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << field_type;
  }
}

void LocationsBuilderARM::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsArm().AsCoreRegister();
  uint32_t offset = instruction->GetFieldOffset().Uint32Value();

  switch (instruction->GetType()) {
    case Primitive::kPrimBoolean: {
      Register out = locations->Out().AsArm().AsCoreRegister();
      __ LoadFromOffset(kLoadUnsignedByte, out, obj, offset);
      break;
    }

    case Primitive::kPrimByte: {
      Register out = locations->Out().AsArm().AsCoreRegister();
      __ LoadFromOffset(kLoadSignedByte, out, obj, offset);
      break;
    }

    case Primitive::kPrimShort: {
      Register out = locations->Out().AsArm().AsCoreRegister();
      __ LoadFromOffset(kLoadSignedHalfword, out, obj, offset);
      break;
    }

    case Primitive::kPrimChar: {
      Register out = locations->Out().AsArm().AsCoreRegister();
      __ LoadFromOffset(kLoadUnsignedHalfword, out, obj, offset);
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      Register out = locations->Out().AsArm().AsCoreRegister();
      __ LoadFromOffset(kLoadWord, out, obj, offset);
      break;
    }

    case Primitive::kPrimLong: {
      // TODO: support volatile.
      ArmManagedRegister out = locations->Out().AsArm();
      __ LoadFromOffset(kLoadWordPair, out.AsRegisterPairLow(), obj, offset);
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register type " << instruction->GetType();

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
  }
}

void LocationsBuilderARM::VisitNullCheck(HNullCheck* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  // TODO: Have a normalization phase that makes this instruction never used.
  locations->SetOut(Location::SameAsFirstInput());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitNullCheck(HNullCheck* instruction) {
  SlowPathCode* slow_path =
      new (GetGraph()->GetArena()) NullCheckSlowPathARM(instruction->GetDexPc());
  codegen_->AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location obj = locations->InAt(0);
  DCHECK(obj.Equals(locations->Out()));

  if (obj.IsRegister()) {
    __ cmp(obj.AsArm().AsCoreRegister(), ShifterOperand(0));
  }
  __ b(slow_path->GetEntryLabel(), EQ);
}

void LocationsBuilderARM::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  locations->SetOut(Location::RequiresRegister());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsArm().AsCoreRegister();
  Location index = locations->InAt(1);

  switch (instruction->GetType()) {
    case Primitive::kPrimBoolean: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      Register out = locations->Out().AsArm().AsCoreRegister();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset;
        __ LoadFromOffset(kLoadUnsignedByte, out, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsArm().AsCoreRegister()));
        __ LoadFromOffset(kLoadUnsignedByte, out, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimByte: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int8_t)).Uint32Value();
      Register out = locations->Out().AsArm().AsCoreRegister();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset;
        __ LoadFromOffset(kLoadSignedByte, out, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsArm().AsCoreRegister()));
        __ LoadFromOffset(kLoadSignedByte, out, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimShort: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int16_t)).Uint32Value();
      Register out = locations->Out().AsArm().AsCoreRegister();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset;
        __ LoadFromOffset(kLoadSignedHalfword, out, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsArm().AsCoreRegister(), LSL, TIMES_2));
        __ LoadFromOffset(kLoadSignedHalfword, out, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimChar: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Uint32Value();
      Register out = locations->Out().AsArm().AsCoreRegister();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset;
        __ LoadFromOffset(kLoadUnsignedHalfword, out, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsArm().AsCoreRegister(), LSL, TIMES_2));
        __ LoadFromOffset(kLoadUnsignedHalfword, out, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      DCHECK_EQ(sizeof(mirror::HeapReference<mirror::Object>), sizeof(int32_t));
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
      Register out = locations->Out().AsArm().AsCoreRegister();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
        __ LoadFromOffset(kLoadWord, out, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsArm().AsCoreRegister(), LSL, TIMES_4));
        __ LoadFromOffset(kLoadWord, out, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimLong: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Uint32Value();
      ArmManagedRegister out = locations->Out().AsArm();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ LoadFromOffset(kLoadWordPair, out.AsRegisterPairLow(), obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsArm().AsCoreRegister(), LSL, TIMES_8));
        __ LoadFromOffset(kLoadWordPair, out.AsRegisterPairLow(), IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register type " << instruction->GetType();

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
  }
}

void LocationsBuilderARM::VisitArraySet(HArraySet* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  Primitive::Type value_type = instruction->InputAt(2)->GetType();
  if (value_type == Primitive::kPrimNot) {
    InvokeRuntimeCallingConvention calling_convention;
    locations->SetInAt(0, ArmCoreLocation(calling_convention.GetRegisterAt(0)));
    locations->SetInAt(1, ArmCoreLocation(calling_convention.GetRegisterAt(1)));
    locations->SetInAt(2, ArmCoreLocation(calling_convention.GetRegisterAt(2)));
    codegen_->MarkNotLeaf();
  } else {
    locations->SetInAt(0, Location::RequiresRegister());
    locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
    locations->SetInAt(2, Location::RequiresRegister());
  }
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitArraySet(HArraySet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsArm().AsCoreRegister();
  Location index = locations->InAt(1);
  Primitive::Type value_type = instruction->InputAt(2)->GetType();

  switch (value_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      Register value = locations->InAt(2).AsArm().AsCoreRegister();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset;
        __ StoreToOffset(kStoreByte, value, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsArm().AsCoreRegister()));
        __ StoreToOffset(kStoreByte, value, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Uint32Value();
      Register value = locations->InAt(2).AsArm().AsCoreRegister();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset;
        __ StoreToOffset(kStoreHalfword, value, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsArm().AsCoreRegister(), LSL, TIMES_2));
        __ StoreToOffset(kStoreHalfword, value, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimInt: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
      Register value = locations->InAt(2).AsArm().AsCoreRegister();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
        __ StoreToOffset(kStoreWord, value, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsArm().AsCoreRegister(), LSL, TIMES_4));
        __ StoreToOffset(kStoreWord, value, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimNot: {
      int32_t offset = QUICK_ENTRYPOINT_OFFSET(kArmWordSize, pAputObject).Int32Value();
      __ ldr(LR, Address(TR, offset));
      __ blx(LR);
      codegen_->RecordPcInfo(instruction->GetDexPc());
      DCHECK(!codegen_->IsLeafMethod());
      break;
    }

    case Primitive::kPrimLong: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Uint32Value();
      ArmManagedRegister value = locations->InAt(2).AsArm();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ StoreToOffset(kStoreWordPair, value.AsRegisterPairLow(), obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsArm().AsCoreRegister(), LSL, TIMES_8));
        __ StoreToOffset(kStoreWordPair, value.AsRegisterPairLow(), IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register type " << instruction->GetType();

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
  }
}

void LocationsBuilderARM::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  uint32_t offset = mirror::Array::LengthOffset().Uint32Value();
  Register obj = locations->InAt(0).AsArm().AsCoreRegister();
  Register out = locations->Out().AsArm().AsCoreRegister();
  __ LoadFromOffset(kLoadWord, out, obj, offset);
}

void LocationsBuilderARM::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  // TODO: Have a normalization phase that makes this instruction never used.
  locations->SetOut(Location::SameAsFirstInput());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  SlowPathCode* slow_path = new (GetGraph()->GetArena()) BoundsCheckSlowPathARM(
      instruction->GetDexPc(), locations->InAt(0), locations->InAt(1));
  codegen_->AddSlowPath(slow_path);

  Register index = locations->InAt(0).AsArm().AsCoreRegister();
  Register length = locations->InAt(1).AsArm().AsCoreRegister();

  __ cmp(index, ShifterOperand(length));
  __ b(slow_path->GetEntryLabel(), CS);
}

void CodeGeneratorARM::MarkGCCard(Register temp, Register card, Register object, Register value) {
  Label is_null;
  __ CompareAndBranchIfZero(value, &is_null);
  __ LoadFromOffset(kLoadWord, card, TR, Thread::CardTableOffset<kArmWordSize>().Int32Value());
  __ Lsr(temp, object, gc::accounting::CardTable::kCardShift);
  __ strb(card, Address(card, temp));
  __ Bind(&is_null);
}

void LocationsBuilderARM::VisitTemporary(HTemporary* temp) {
  temp->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitTemporary(HTemporary* temp) {
  // Nothing to do, this is driven by the code generator.
}

void LocationsBuilderARM::VisitParallelMove(HParallelMove* instruction) {
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorARM::VisitParallelMove(HParallelMove* instruction) {
  codegen_->GetMoveResolver()->EmitNativeCode(instruction);
}

ArmAssembler* ParallelMoveResolverARM::GetAssembler() const {
  return codegen_->GetAssembler();
}

void ParallelMoveResolverARM::EmitMove(size_t index) {
  MoveOperands* move = moves_.Get(index);
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister()) {
    if (destination.IsRegister()) {
      __ Mov(destination.AsArm().AsCoreRegister(), source.AsArm().AsCoreRegister());
    } else {
      DCHECK(destination.IsStackSlot());
      __ StoreToOffset(kStoreWord, source.AsArm().AsCoreRegister(),
                       SP, destination.GetStackIndex());
    }
  } else if (source.IsStackSlot()) {
    if (destination.IsRegister()) {
      __ LoadFromOffset(kLoadWord, destination.AsArm().AsCoreRegister(),
                        SP, source.GetStackIndex());
    } else {
      DCHECK(destination.IsStackSlot());
      __ LoadFromOffset(kLoadWord, IP, SP, source.GetStackIndex());
      __ StoreToOffset(kStoreWord, IP, SP, destination.GetStackIndex());
    }
  } else {
    DCHECK(source.IsConstant());
    DCHECK(source.GetConstant()->AsIntConstant() != nullptr);
    int32_t value = source.GetConstant()->AsIntConstant()->GetValue();
    if (destination.IsRegister()) {
      __ LoadImmediate(destination.AsArm().AsCoreRegister(), value);
    } else {
      DCHECK(destination.IsStackSlot());
      __ LoadImmediate(IP, value);
      __ str(IP, Address(SP, destination.GetStackIndex()));
    }
  }
}

void ParallelMoveResolverARM::Exchange(Register reg, int mem) {
  __ Mov(IP, reg);
  __ LoadFromOffset(kLoadWord, reg, SP, mem);
  __ StoreToOffset(kStoreWord, IP, SP, mem);
}

void ParallelMoveResolverARM::Exchange(int mem1, int mem2) {
  ScratchRegisterScope ensure_scratch(this, IP, R0, codegen_->GetNumberOfCoreRegisters());
  int stack_offset = ensure_scratch.IsSpilled() ? kArmWordSize : 0;
  __ LoadFromOffset(kLoadWord, static_cast<Register>(ensure_scratch.GetRegister()),
                    SP, mem1 + stack_offset);
  __ LoadFromOffset(kLoadWord, IP, SP, mem2 + stack_offset);
  __ StoreToOffset(kStoreWord, static_cast<Register>(ensure_scratch.GetRegister()),
                   SP, mem2 + stack_offset);
  __ StoreToOffset(kStoreWord, IP, SP, mem1 + stack_offset);
}

void ParallelMoveResolverARM::EmitSwap(size_t index) {
  MoveOperands* move = moves_.Get(index);
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister() && destination.IsRegister()) {
    DCHECK_NE(source.AsArm().AsCoreRegister(), IP);
    DCHECK_NE(destination.AsArm().AsCoreRegister(), IP);
    __ Mov(IP, source.AsArm().AsCoreRegister());
    __ Mov(source.AsArm().AsCoreRegister(), destination.AsArm().AsCoreRegister());
    __ Mov(destination.AsArm().AsCoreRegister(), IP);
  } else if (source.IsRegister() && destination.IsStackSlot()) {
    Exchange(source.AsArm().AsCoreRegister(), destination.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsRegister()) {
    Exchange(destination.AsArm().AsCoreRegister(), source.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsStackSlot()) {
    Exchange(source.GetStackIndex(), destination.GetStackIndex());
  } else {
    LOG(FATAL) << "Unimplemented";
  }
}

void ParallelMoveResolverARM::SpillScratch(int reg) {
  __ Push(static_cast<Register>(reg));
}

void ParallelMoveResolverARM::RestoreScratch(int reg) {
  __ Pop(static_cast<Register>(reg));
}

}  // namespace arm
}  // namespace art
