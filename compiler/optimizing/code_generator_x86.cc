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

#include "code_generator_x86.h"
#include "gc/accounting/card_table.h"
#include "utils/assembler.h"
#include "utils/stack_checks.h"
#include "utils/x86/assembler_x86.h"
#include "utils/x86/managed_register_x86.h"

#include "entrypoints/quick/quick_entrypoints.h"
#include "mirror/array.h"
#include "mirror/art_method.h"
#include "thread.h"

namespace art {

x86::X86ManagedRegister Location::AsX86() const {
  return reg().AsX86();
}

namespace x86 {

static constexpr bool kExplicitStackOverflowCheck = false;

static constexpr int kNumberOfPushedRegistersAtEntry = 1;
static constexpr int kCurrentMethodStackOffset = 0;

static Location X86CpuLocation(Register reg) {
  return Location::RegisterLocation(X86ManagedRegister::FromCpuRegister(reg));
}

static constexpr Register kRuntimeParameterCoreRegisters[] = { EAX, ECX, EDX };
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

#define __ reinterpret_cast<X86Assembler*>(codegen->GetAssembler())->

class NullCheckSlowPathX86 : public SlowPathCode {
 public:
  explicit NullCheckSlowPathX86(uint32_t dex_pc) : dex_pc_(dex_pc) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pThrowNullPointer)));
    codegen->RecordPcInfo(dex_pc_);
  }

 private:
  const uint32_t dex_pc_;
  DISALLOW_COPY_AND_ASSIGN(NullCheckSlowPathX86);
};

class StackOverflowCheckSlowPathX86 : public SlowPathCode {
 public:
  StackOverflowCheckSlowPathX86() {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    __ addl(ESP,
            Immediate(codegen->GetFrameSize() - kNumberOfPushedRegistersAtEntry * kX86WordSize));
    __ fs()->jmp(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pThrowStackOverflow)));
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(StackOverflowCheckSlowPathX86);
};

class BoundsCheckSlowPathX86 : public SlowPathCode {
 public:
  explicit BoundsCheckSlowPathX86(uint32_t dex_pc,
                                  Location index_location,
                                  Location length_location)
      : dex_pc_(dex_pc), index_location_(index_location), length_location_(length_location) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorX86* x86_codegen = reinterpret_cast<CodeGeneratorX86*>(codegen);
    __ Bind(GetEntryLabel());
    InvokeRuntimeCallingConvention calling_convention;
    x86_codegen->Move32(X86CpuLocation(calling_convention.GetRegisterAt(0)), index_location_);
    x86_codegen->Move32(X86CpuLocation(calling_convention.GetRegisterAt(1)), length_location_);
    __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pThrowArrayBounds)));
    codegen->RecordPcInfo(dex_pc_);
  }

 private:
  const uint32_t dex_pc_;
  const Location index_location_;
  const Location length_location_;

  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathX86);
};

#undef __
#define __ reinterpret_cast<X86Assembler*>(GetAssembler())->

inline Condition X86Condition(IfCondition cond) {
  switch (cond) {
    case kCondEQ: return kEqual;
    case kCondNE: return kNotEqual;
    case kCondLT: return kLess;
    case kCondLE: return kLessEqual;
    case kCondGT: return kGreater;
    case kCondGE: return kGreaterEqual;
    default:
      LOG(FATAL) << "Unknown if condition";
  }
  return kEqual;
}

void CodeGeneratorX86::DumpCoreRegister(std::ostream& stream, int reg) const {
  stream << X86ManagedRegister::FromCpuRegister(Register(reg));
}

void CodeGeneratorX86::DumpFloatingPointRegister(std::ostream& stream, int reg) const {
  stream << X86ManagedRegister::FromXmmRegister(XmmRegister(reg));
}

CodeGeneratorX86::CodeGeneratorX86(HGraph* graph)
    : CodeGenerator(graph, kNumberOfRegIds),
      location_builder_(graph, this),
      instruction_visitor_(graph, this),
      move_resolver_(graph->GetArena(), this) {}

size_t CodeGeneratorX86::FrameEntrySpillSize() const {
  return kNumberOfPushedRegistersAtEntry * kX86WordSize;
}

static bool* GetBlockedRegisterPairs(bool* blocked_registers) {
  return blocked_registers + kNumberOfAllocIds;
}

ManagedRegister CodeGeneratorX86::AllocateFreeRegister(Primitive::Type type,
                                                       bool* blocked_registers) const {
  switch (type) {
    case Primitive::kPrimLong: {
      bool* blocked_register_pairs = GetBlockedRegisterPairs(blocked_registers);
      size_t reg = AllocateFreeRegisterInternal(blocked_register_pairs, kNumberOfRegisterPairs);
      X86ManagedRegister pair =
          X86ManagedRegister::FromRegisterPair(static_cast<RegisterPair>(reg));
      blocked_registers[pair.AsRegisterPairLow()] = true;
      blocked_registers[pair.AsRegisterPairHigh()] = true;
      // Block all other register pairs that share a register with `pair`.
      for (int i = 0; i < kNumberOfRegisterPairs; i++) {
        X86ManagedRegister current =
            X86ManagedRegister::FromRegisterPair(static_cast<RegisterPair>(i));
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
      Register reg = static_cast<Register>(
          AllocateFreeRegisterInternal(blocked_registers, kNumberOfCpuRegisters));
      // Block all register pairs that contain `reg`.
      bool* blocked_register_pairs = GetBlockedRegisterPairs(blocked_registers);
      for (int i = 0; i < kNumberOfRegisterPairs; i++) {
        X86ManagedRegister current =
            X86ManagedRegister::FromRegisterPair(static_cast<RegisterPair>(i));
        if (current.AsRegisterPairLow() == reg || current.AsRegisterPairHigh() == reg) {
          blocked_register_pairs[i] = true;
        }
      }
      return X86ManagedRegister::FromCpuRegister(reg);
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register type " << type;

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << type;
  }

  return ManagedRegister::NoRegister();
}

void CodeGeneratorX86::SetupBlockedRegisters(bool* blocked_registers) const {
  bool* blocked_register_pairs = GetBlockedRegisterPairs(blocked_registers);

  // Don't allocate the dalvik style register pair passing.
  blocked_register_pairs[ECX_EDX] = true;

  // Stack register is always reserved.
  blocked_registers[ESP] = true;

  // TODO: We currently don't use Quick's callee saved registers.
  blocked_registers[EBP] = true;
  blocked_registers[ESI] = true;
  blocked_registers[EDI] = true;
  blocked_register_pairs[EAX_EDI] = true;
  blocked_register_pairs[EDX_EDI] = true;
  blocked_register_pairs[ECX_EDI] = true;
  blocked_register_pairs[EBX_EDI] = true;
}

size_t CodeGeneratorX86::GetNumberOfRegisters() const {
  return kNumberOfRegIds;
}

InstructionCodeGeneratorX86::InstructionCodeGeneratorX86(HGraph* graph, CodeGeneratorX86* codegen)
      : HGraphVisitor(graph),
        assembler_(codegen->GetAssembler()),
        codegen_(codegen) {}

void CodeGeneratorX86::GenerateFrameEntry() {
  // Create a fake register to mimic Quick.
  static const int kFakeReturnRegister = 8;
  core_spill_mask_ |= (1 << kFakeReturnRegister);

  bool skip_overflow_check = IsLeafMethod() && !FrameNeedsStackCheck(GetFrameSize(), InstructionSet::kX86);
  if (!skip_overflow_check && !kExplicitStackOverflowCheck) {
    __ testl(EAX, Address(ESP, -static_cast<int32_t>(GetStackOverflowReservedBytes(kX86))));
    RecordPcInfo(0);
  }

  // The return PC has already been pushed on the stack.
  __ subl(ESP, Immediate(GetFrameSize() - kNumberOfPushedRegistersAtEntry * kX86WordSize));

  if (!skip_overflow_check && kExplicitStackOverflowCheck) {
    SlowPathCode* slow_path = new (GetGraph()->GetArena()) StackOverflowCheckSlowPathX86();
    AddSlowPath(slow_path);

    __ fs()->cmpl(ESP, Address::Absolute(Thread::StackEndOffset<kX86WordSize>()));
    __ j(kLess, slow_path->GetEntryLabel());
  }

  __ movl(Address(ESP, kCurrentMethodStackOffset), EAX);
}

void CodeGeneratorX86::GenerateFrameExit() {
  __ addl(ESP, Immediate(GetFrameSize() - kNumberOfPushedRegistersAtEntry * kX86WordSize));
}

void CodeGeneratorX86::Bind(Label* label) {
  __ Bind(label);
}

void InstructionCodeGeneratorX86::LoadCurrentMethod(Register reg) {
  __ movl(reg, Address(ESP, kCurrentMethodStackOffset));
}

Location CodeGeneratorX86::GetStackLocation(HLoadLocal* load) const {
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
        return X86CpuLocation(calling_convention.GetRegisterAt(index));
      } else {
        return Location::StackSlot(calling_convention.GetStackOffsetOf(index));
      }
    }

    case Primitive::kPrimLong: {
      uint32_t index = gp_index_;
      gp_index_ += 2;
      if (index + 1 < calling_convention.GetNumberOfRegisters()) {
        return Location::RegisterLocation(X86ManagedRegister::FromRegisterPair(
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

void CodeGeneratorX86::Move32(Location destination, Location source) {
  if (source.Equals(destination)) {
    return;
  }
  if (destination.IsRegister()) {
    if (source.IsRegister()) {
      __ movl(destination.AsX86().AsCpuRegister(), source.AsX86().AsCpuRegister());
    } else {
      DCHECK(source.IsStackSlot());
      __ movl(destination.AsX86().AsCpuRegister(), Address(ESP, source.GetStackIndex()));
    }
  } else {
    if (source.IsRegister()) {
      __ movl(Address(ESP, destination.GetStackIndex()), source.AsX86().AsCpuRegister());
    } else {
      DCHECK(source.IsStackSlot());
      __ pushl(Address(ESP, source.GetStackIndex()));
      __ popl(Address(ESP, destination.GetStackIndex()));
    }
  }
}

void CodeGeneratorX86::Move64(Location destination, Location source) {
  if (source.Equals(destination)) {
    return;
  }
  if (destination.IsRegister()) {
    if (source.IsRegister()) {
      __ movl(destination.AsX86().AsRegisterPairLow(), source.AsX86().AsRegisterPairLow());
      __ movl(destination.AsX86().AsRegisterPairHigh(), source.AsX86().AsRegisterPairHigh());
    } else if (source.IsQuickParameter()) {
      uint32_t argument_index = source.GetQuickParameterIndex();
      InvokeDexCallingConvention calling_convention;
      __ movl(destination.AsX86().AsRegisterPairLow(),
              calling_convention.GetRegisterAt(argument_index));
      __ movl(destination.AsX86().AsRegisterPairHigh(), Address(ESP,
          calling_convention.GetStackOffsetOf(argument_index + 1) + GetFrameSize()));
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ movl(destination.AsX86().AsRegisterPairLow(), Address(ESP, source.GetStackIndex()));
      __ movl(destination.AsX86().AsRegisterPairHigh(),
              Address(ESP, source.GetHighStackIndex(kX86WordSize)));
    }
  } else if (destination.IsQuickParameter()) {
    InvokeDexCallingConvention calling_convention;
    uint32_t argument_index = destination.GetQuickParameterIndex();
    if (source.IsRegister()) {
      __ movl(calling_convention.GetRegisterAt(argument_index), source.AsX86().AsRegisterPairLow());
      __ movl(Address(ESP, calling_convention.GetStackOffsetOf(argument_index + 1)),
              source.AsX86().AsRegisterPairHigh());
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ movl(calling_convention.GetRegisterAt(argument_index),
              Address(ESP, source.GetStackIndex()));
      __ pushl(Address(ESP, source.GetHighStackIndex(kX86WordSize)));
      __ popl(Address(ESP, calling_convention.GetStackOffsetOf(argument_index + 1)));
    }
  } else {
    if (source.IsRegister()) {
      __ movl(Address(ESP, destination.GetStackIndex()), source.AsX86().AsRegisterPairLow());
      __ movl(Address(ESP, destination.GetHighStackIndex(kX86WordSize)),
              source.AsX86().AsRegisterPairHigh());
    } else if (source.IsQuickParameter()) {
      InvokeDexCallingConvention calling_convention;
      uint32_t argument_index = source.GetQuickParameterIndex();
      __ movl(Address(ESP, destination.GetStackIndex()),
              calling_convention.GetRegisterAt(argument_index));
      __ pushl(Address(ESP,
          calling_convention.GetStackOffsetOf(argument_index + 1) + GetFrameSize()));
      __ popl(Address(ESP, destination.GetHighStackIndex(kX86WordSize)));
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ pushl(Address(ESP, source.GetStackIndex()));
      __ popl(Address(ESP, destination.GetStackIndex()));
      __ pushl(Address(ESP, source.GetHighStackIndex(kX86WordSize)));
      __ popl(Address(ESP, destination.GetHighStackIndex(kX86WordSize)));
    }
  }
}

void CodeGeneratorX86::Move(HInstruction* instruction, Location location, HInstruction* move_for) {
  if (instruction->AsIntConstant() != nullptr) {
    Immediate imm(instruction->AsIntConstant()->GetValue());
    if (location.IsRegister()) {
      __ movl(location.AsX86().AsCpuRegister(), imm);
    } else {
      __ movl(Address(ESP, location.GetStackIndex()), imm);
    }
  } else if (instruction->AsLongConstant() != nullptr) {
    int64_t value = instruction->AsLongConstant()->GetValue();
    if (location.IsRegister()) {
      __ movl(location.AsX86().AsRegisterPairLow(), Immediate(Low32Bits(value)));
      __ movl(location.AsX86().AsRegisterPairHigh(), Immediate(High32Bits(value)));
    } else {
      __ movl(Address(ESP, location.GetStackIndex()), Immediate(Low32Bits(value)));
      __ movl(Address(ESP, location.GetHighStackIndex(kX86WordSize)), Immediate(High32Bits(value)));
    }
  } else if (instruction->AsLoadLocal() != nullptr) {
    switch (instruction->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
      case Primitive::kPrimNot:
        Move32(location, Location::StackSlot(GetStackSlot(instruction->AsLoadLocal()->GetLocal())));
        break;

      case Primitive::kPrimLong:
        Move64(location, Location::DoubleStackSlot(
            GetStackSlot(instruction->AsLoadLocal()->GetLocal())));
        break;

      default:
        LOG(FATAL) << "Unimplemented local type " << instruction->GetType();
    }
  } else {
    DCHECK((instruction->GetNext() == move_for) || instruction->GetNext()->IsTemporary());
    switch (instruction->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
      case Primitive::kPrimNot:
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

void LocationsBuilderX86::VisitGoto(HGoto* got) {
  got->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitGoto(HGoto* got) {
  HBasicBlock* successor = got->GetSuccessor();
  if (GetGraph()->GetExitBlock() == successor) {
    codegen_->GenerateFrameExit();
  } else if (!codegen_->GoesToNextBlock(got->GetBlock(), successor)) {
    __ jmp(codegen_->GetLabelOf(successor));
  }
}

void LocationsBuilderX86::VisitExit(HExit* exit) {
  exit->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitExit(HExit* exit) {
  if (kIsDebugBuild) {
    __ Comment("Unreachable");
    __ int3();
  }
}

void LocationsBuilderX86::VisitIf(HIf* if_instr) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(if_instr);
  HInstruction* cond = if_instr->InputAt(0);
  DCHECK(cond->IsCondition());
  HCondition* condition = cond->AsCondition();
  if (condition->NeedsMaterialization()) {
    locations->SetInAt(0, Location::Any());
  }
  if_instr->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitIf(HIf* if_instr) {
  HInstruction* cond = if_instr->InputAt(0);
  DCHECK(cond->IsCondition());
  HCondition* condition = cond->AsCondition();
  if (condition->NeedsMaterialization()) {
    // Materialized condition, compare against 0
    Location lhs = if_instr->GetLocations()->InAt(0);
    if (lhs.IsRegister()) {
      __ cmpl(lhs.AsX86().AsCpuRegister(), Immediate(0));
    } else {
      __ cmpl(Address(ESP, lhs.GetStackIndex()), Immediate(0));
    }
    __ j(kEqual,  codegen_->GetLabelOf(if_instr->IfTrueSuccessor()));
  } else {
    Location lhs = condition->GetLocations()->InAt(0);
    Location rhs = condition->GetLocations()->InAt(1);
    // LHS is guaranteed to be in a register (see LocationsBuilderX86::VisitCondition).
    if (rhs.IsRegister()) {
      __ cmpl(lhs.AsX86().AsCpuRegister(), rhs.AsX86().AsCpuRegister());
    } else if (rhs.IsConstant()) {
      HIntConstant* instruction = rhs.GetConstant()->AsIntConstant();
      Immediate imm(instruction->AsIntConstant()->GetValue());
      __ cmpl(lhs.AsX86().AsCpuRegister(), imm);
    } else {
      __ cmpl(lhs.AsX86().AsCpuRegister(), Address(ESP, rhs.GetStackIndex()));
    }
    __ j(X86Condition(condition->GetCondition()),
         codegen_->GetLabelOf(if_instr->IfTrueSuccessor()));
  }
  if (!codegen_->GoesToNextBlock(if_instr->GetBlock(), if_instr->IfFalseSuccessor())) {
    __ jmp(codegen_->GetLabelOf(if_instr->IfFalseSuccessor()));
  }
}

void LocationsBuilderX86::VisitLocal(HLocal* local) {
  local->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitLocal(HLocal* local) {
  DCHECK_EQ(local->GetBlock(), GetGraph()->GetEntryBlock());
}

void LocationsBuilderX86::VisitLoadLocal(HLoadLocal* local) {
  local->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitLoadLocal(HLoadLocal* load) {
  // Nothing to do, this is driven by the code generator.
}

void LocationsBuilderX86::VisitStoreLocal(HStoreLocal* store) {
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

void InstructionCodeGeneratorX86::VisitStoreLocal(HStoreLocal* store) {
}

void LocationsBuilderX86::VisitCondition(HCondition* comp) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(comp);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::Any());
  if (comp->NeedsMaterialization()) {
    locations->SetOut(Location::RequiresRegister());
  }
  comp->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitCondition(HCondition* comp) {
  if (comp->NeedsMaterialization()) {
    LocationSummary* locations = comp->GetLocations();
    if (locations->InAt(1).IsRegister()) {
      __ cmpl(locations->InAt(0).AsX86().AsCpuRegister(),
              locations->InAt(1).AsX86().AsCpuRegister());
    } else if (locations->InAt(1).IsConstant()) {
      HConstant* instruction = locations->InAt(1).GetConstant();
      Immediate imm(instruction->AsIntConstant()->GetValue());
      __ cmpl(locations->InAt(0).AsX86().AsCpuRegister(), imm);
    } else {
      __ cmpl(locations->InAt(0).AsX86().AsCpuRegister(),
              Address(ESP, locations->InAt(1).GetStackIndex()));
    }
    __ setb(X86Condition(comp->GetCondition()), locations->Out().AsX86().AsCpuRegister());
  }
}

void LocationsBuilderX86::VisitEqual(HEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86::VisitEqual(HEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86::VisitNotEqual(HNotEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86::VisitNotEqual(HNotEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86::VisitLessThan(HLessThan* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86::VisitLessThan(HLessThan* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86::VisitGreaterThan(HGreaterThan* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86::VisitGreaterThan(HGreaterThan* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86::VisitIntConstant(HIntConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
  constant->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitIntConstant(HIntConstant* constant) {
}

void LocationsBuilderX86::VisitLongConstant(HLongConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
  constant->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitLongConstant(HLongConstant* constant) {
  // Will be generated at use site.
}

void LocationsBuilderX86::VisitReturnVoid(HReturnVoid* ret) {
  ret->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitReturnVoid(HReturnVoid* ret) {
  codegen_->GenerateFrameExit();
  __ ret();
}

void LocationsBuilderX86::VisitReturn(HReturn* ret) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(ret);
  switch (ret->InputAt(0)->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
      locations->SetInAt(0, X86CpuLocation(EAX));
      break;

    case Primitive::kPrimLong:
      locations->SetInAt(
          0, Location::RegisterLocation(X86ManagedRegister::FromRegisterPair(EAX_EDX)));
      break;

    default:
      LOG(FATAL) << "Unimplemented return type " << ret->InputAt(0)->GetType();
  }
  ret->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitReturn(HReturn* ret) {
  if (kIsDebugBuild) {
    switch (ret->InputAt(0)->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
      case Primitive::kPrimNot:
        DCHECK_EQ(ret->GetLocations()->InAt(0).AsX86().AsCpuRegister(), EAX);
        break;

      case Primitive::kPrimLong:
        DCHECK_EQ(ret->GetLocations()->InAt(0).AsX86().AsRegisterPair(), EAX_EDX);
        break;

      default:
        LOG(FATAL) << "Unimplemented return type " << ret->InputAt(0)->GetType();
    }
  }
  codegen_->GenerateFrameExit();
  __ ret();
}

void LocationsBuilderX86::VisitInvokeStatic(HInvokeStatic* invoke) {
  codegen_->MarkNotLeaf();
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(invoke);
  locations->AddTemp(X86CpuLocation(EAX));

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
      locations->SetOut(X86CpuLocation(EAX));
      break;

    case Primitive::kPrimLong:
      locations->SetOut(Location::RegisterLocation(X86ManagedRegister::FromRegisterPair(EAX_EDX)));
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

void InstructionCodeGeneratorX86::VisitInvokeStatic(HInvokeStatic* invoke) {
  Register temp = invoke->GetLocations()->GetTemp(0).AsX86().AsCpuRegister();
  uint32_t heap_reference_size = sizeof(mirror::HeapReference<mirror::Object>);
  size_t index_in_cache = mirror::Array::DataOffset(heap_reference_size).Int32Value() +
      invoke->GetIndexInDexCache() * kX86WordSize;

  // TODO: Implement all kinds of calls:
  // 1) boot -> boot
  // 2) app -> boot
  // 3) app -> app
  //
  // Currently we implement the app -> app logic, which looks up in the resolve cache.

  // temp = method;
  LoadCurrentMethod(temp);
  // temp = temp->dex_cache_resolved_methods_;
  __ movl(temp, Address(temp, mirror::ArtMethod::DexCacheResolvedMethodsOffset().Int32Value()));
  // temp = temp[index_in_cache]
  __ movl(temp, Address(temp, index_in_cache));
  // (temp + offset_of_quick_compiled_code)()
  __ call(Address(temp, mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset().Int32Value()));

  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke->GetDexPc());
}

void LocationsBuilderX86::VisitAdd(HAdd* add) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(add);
  switch (add->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::SameAsFirstInput());
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

void InstructionCodeGeneratorX86::VisitAdd(HAdd* add) {
  LocationSummary* locations = add->GetLocations();
  switch (add->GetResultType()) {
    case Primitive::kPrimInt: {
      DCHECK_EQ(locations->InAt(0).AsX86().AsCpuRegister(),
                locations->Out().AsX86().AsCpuRegister());
      if (locations->InAt(1).IsRegister()) {
        __ addl(locations->InAt(0).AsX86().AsCpuRegister(),
                locations->InAt(1).AsX86().AsCpuRegister());
      } else if (locations->InAt(1).IsConstant()) {
        HConstant* instruction = locations->InAt(1).GetConstant();
        Immediate imm(instruction->AsIntConstant()->GetValue());
        __ addl(locations->InAt(0).AsX86().AsCpuRegister(), imm);
      } else {
        __ addl(locations->InAt(0).AsX86().AsCpuRegister(),
                Address(ESP, locations->InAt(1).GetStackIndex()));
      }
      break;
    }

    case Primitive::kPrimLong: {
      DCHECK_EQ(locations->InAt(0).AsX86().AsRegisterPair(),
                locations->Out().AsX86().AsRegisterPair());
      if (locations->InAt(1).IsRegister()) {
        __ addl(locations->InAt(0).AsX86().AsRegisterPairLow(),
                locations->InAt(1).AsX86().AsRegisterPairLow());
        __ adcl(locations->InAt(0).AsX86().AsRegisterPairHigh(),
                locations->InAt(1).AsX86().AsRegisterPairHigh());
      } else {
        __ addl(locations->InAt(0).AsX86().AsRegisterPairLow(),
                Address(ESP, locations->InAt(1).GetStackIndex()));
        __ adcl(locations->InAt(0).AsX86().AsRegisterPairHigh(),
                Address(ESP, locations->InAt(1).GetHighStackIndex(kX86WordSize)));
      }
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
}

void LocationsBuilderX86::VisitSub(HSub* sub) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(sub);
  switch (sub->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::SameAsFirstInput());
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

void InstructionCodeGeneratorX86::VisitSub(HSub* sub) {
  LocationSummary* locations = sub->GetLocations();
  switch (sub->GetResultType()) {
    case Primitive::kPrimInt: {
      DCHECK_EQ(locations->InAt(0).AsX86().AsCpuRegister(),
                locations->Out().AsX86().AsCpuRegister());
      if (locations->InAt(1).IsRegister()) {
        __ subl(locations->InAt(0).AsX86().AsCpuRegister(),
                locations->InAt(1).AsX86().AsCpuRegister());
      } else if (locations->InAt(1).IsConstant()) {
        HConstant* instruction = locations->InAt(1).GetConstant();
        Immediate imm(instruction->AsIntConstant()->GetValue());
        __ subl(locations->InAt(0).AsX86().AsCpuRegister(), imm);
      } else {
        __ subl(locations->InAt(0).AsX86().AsCpuRegister(),
                Address(ESP, locations->InAt(1).GetStackIndex()));
      }
      break;
    }

    case Primitive::kPrimLong: {
      DCHECK_EQ(locations->InAt(0).AsX86().AsRegisterPair(),
                locations->Out().AsX86().AsRegisterPair());
      if (locations->InAt(1).IsRegister()) {
        __ subl(locations->InAt(0).AsX86().AsRegisterPairLow(),
                locations->InAt(1).AsX86().AsRegisterPairLow());
        __ sbbl(locations->InAt(0).AsX86().AsRegisterPairHigh(),
                locations->InAt(1).AsX86().AsRegisterPairHigh());
      } else {
        __ subl(locations->InAt(0).AsX86().AsRegisterPairLow(),
                Address(ESP, locations->InAt(1).GetStackIndex()));
        __ sbbl(locations->InAt(0).AsX86().AsRegisterPairHigh(),
                Address(ESP, locations->InAt(1).GetHighStackIndex(kX86WordSize)));
      }
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
}

void LocationsBuilderX86::VisitNewInstance(HNewInstance* instruction) {
  codegen_->MarkNotLeaf();
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetOut(X86CpuLocation(EAX));
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(X86CpuLocation(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(X86CpuLocation(calling_convention.GetRegisterAt(1)));
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitNewInstance(HNewInstance* instruction) {
  InvokeRuntimeCallingConvention calling_convention;
  LoadCurrentMethod(calling_convention.GetRegisterAt(1));
  __ movl(calling_convention.GetRegisterAt(0), Immediate(instruction->GetTypeIndex()));

  __ fs()->call(
      Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pAllocObjectWithAccessCheck)));

  codegen_->RecordPcInfo(instruction->GetDexPc());
  DCHECK(!codegen_->IsLeafMethod());
}

void LocationsBuilderX86::VisitParameterValue(HParameterValue* instruction) {
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

void InstructionCodeGeneratorX86::VisitParameterValue(HParameterValue* instruction) {
}

void LocationsBuilderX86::VisitNot(HNot* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitNot(HNot* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location out = locations->Out();
  DCHECK_EQ(locations->InAt(0).AsX86().AsCpuRegister(), out.AsX86().AsCpuRegister());
  __ xorl(out.AsX86().AsCpuRegister(), Immediate(1));
}

void LocationsBuilderX86::VisitCompare(HCompare* compare) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(compare);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::Any());
  locations->SetOut(Location::RequiresRegister());
  compare->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitCompare(HCompare* compare) {
  Label greater, done;
  LocationSummary* locations = compare->GetLocations();
  switch (compare->InputAt(0)->GetType()) {
    case Primitive::kPrimLong: {
      Label less, greater, done;
      Register output = locations->Out().AsX86().AsCpuRegister();
      X86ManagedRegister left = locations->InAt(0).AsX86();
      Location right = locations->InAt(1);
      if (right.IsRegister()) {
        __ cmpl(left.AsRegisterPairHigh(), right.AsX86().AsRegisterPairHigh());
      } else {
        DCHECK(right.IsDoubleStackSlot());
        __ cmpl(left.AsRegisterPairHigh(), Address(ESP, right.GetHighStackIndex(kX86WordSize)));
      }
      __ j(kLess, &less);  // Signed compare.
      __ j(kGreater, &greater);  // Signed compare.
      if (right.IsRegister()) {
        __ cmpl(left.AsRegisterPairLow(), right.AsX86().AsRegisterPairLow());
      } else {
        DCHECK(right.IsDoubleStackSlot());
        __ cmpl(left.AsRegisterPairLow(), Address(ESP, right.GetStackIndex()));
      }
      __ movl(output, Immediate(0));
      __ j(kEqual, &done);
      __ j(kBelow, &less);  // Unsigned compare.

      __ Bind(&greater);
      __ movl(output, Immediate(1));
      __ jmp(&done);

      __ Bind(&less);
      __ movl(output, Immediate(-1));

      __ Bind(&done);
      break;
    }
    default:
      LOG(FATAL) << "Unimplemented compare type " << compare->InputAt(0)->GetType();
  }
}

void LocationsBuilderX86::VisitPhi(HPhi* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    locations->SetInAt(i, Location::Any());
  }
  locations->SetOut(Location::Any());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitPhi(HPhi* instruction) {
  LOG(FATAL) << "Unreachable";
}

void LocationsBuilderX86::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  Primitive::Type field_type = instruction->InputAt(1)->GetType();
  if (field_type == Primitive::kPrimBoolean || field_type == Primitive::kPrimByte) {
    // Ensure the value is in a byte register.
    locations->SetInAt(1, X86CpuLocation(EAX));
  } else {
    locations->SetInAt(1, Location::RequiresRegister());
  }
  // Temporary registers for the write barrier.
  if (field_type == Primitive::kPrimNot) {
    locations->AddTemp(Location::RequiresRegister());
    // Ensure the card is in a byte register.
    locations->AddTemp(X86CpuLocation(ECX));
  }
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsX86().AsCpuRegister();
  uint32_t offset = instruction->GetFieldOffset().Uint32Value();
  Primitive::Type field_type = instruction->InputAt(1)->GetType();

  switch (field_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      ByteRegister value = locations->InAt(1).AsX86().AsByteRegister();
      __ movb(Address(obj, offset), value);
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      Register value = locations->InAt(1).AsX86().AsCpuRegister();
      __ movw(Address(obj, offset), value);
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      Register value = locations->InAt(1).AsX86().AsCpuRegister();
      __ movl(Address(obj, offset), value);

      if (field_type == Primitive::kPrimNot) {
        Register temp = locations->GetTemp(0).AsX86().AsCpuRegister();
        Register card = locations->GetTemp(1).AsX86().AsCpuRegister();
        codegen_->MarkGCCard(temp, card, obj, value);
      }
      break;
    }

    case Primitive::kPrimLong: {
      X86ManagedRegister value = locations->InAt(1).AsX86();
      __ movl(Address(obj, offset), value.AsRegisterPairLow());
      __ movl(Address(obj, kX86WordSize + offset), value.AsRegisterPairHigh());
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register type " << field_type;

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << field_type;
  }
}

void CodeGeneratorX86::MarkGCCard(Register temp, Register card, Register object, Register value) {
  Label is_null;
  __ testl(value, value);
  __ j(kEqual, &is_null);
  __ fs()->movl(card, Address::Absolute(Thread::CardTableOffset<kX86WordSize>().Int32Value()));
  __ movl(temp, object);
  __ shrl(temp, Immediate(gc::accounting::CardTable::kCardShift));
  __ movb(Address(temp, card, TIMES_1, 0),
          X86ManagedRegister::FromCpuRegister(card).AsByteRegister());
  __ Bind(&is_null);
}

void LocationsBuilderX86::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsX86().AsCpuRegister();
  uint32_t offset = instruction->GetFieldOffset().Uint32Value();

  switch (instruction->GetType()) {
    case Primitive::kPrimBoolean: {
      Register out = locations->Out().AsX86().AsCpuRegister();
      __ movzxb(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimByte: {
      Register out = locations->Out().AsX86().AsCpuRegister();
      __ movsxb(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimShort: {
      Register out = locations->Out().AsX86().AsCpuRegister();
      __ movsxw(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimChar: {
      Register out = locations->Out().AsX86().AsCpuRegister();
      __ movzxw(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      Register out = locations->Out().AsX86().AsCpuRegister();
      __ movl(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimLong: {
      // TODO: support volatile.
      X86ManagedRegister out = locations->Out().AsX86();
      __ movl(out.AsRegisterPairLow(), Address(obj, offset));
      __ movl(out.AsRegisterPairHigh(), Address(obj, kX86WordSize + offset));
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register type " << instruction->GetType();

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
  }
}

void LocationsBuilderX86::VisitNullCheck(HNullCheck* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::Any());
  // TODO: Have a normalization phase that makes this instruction never used.
  locations->SetOut(Location::SameAsFirstInput());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitNullCheck(HNullCheck* instruction) {
  SlowPathCode* slow_path =
      new (GetGraph()->GetArena()) NullCheckSlowPathX86(instruction->GetDexPc());
  codegen_->AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location obj = locations->InAt(0);
  DCHECK(obj.Equals(locations->Out()));

  if (obj.IsRegister()) {
    __ cmpl(obj.AsX86().AsCpuRegister(), Immediate(0));
  } else {
    DCHECK(locations->InAt(0).IsStackSlot());
    __ cmpl(Address(ESP, obj.GetStackIndex()), Immediate(0));
  }
  __ j(kEqual, slow_path->GetEntryLabel());
}

void LocationsBuilderX86::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  locations->SetOut(Location::RequiresRegister());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsX86().AsCpuRegister();
  Location index = locations->InAt(1);

  switch (instruction->GetType()) {
    case Primitive::kPrimBoolean: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      Register out = locations->Out().AsX86().AsCpuRegister();
      if (index.IsConstant()) {
        __ movzxb(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset));
      } else {
        __ movzxb(out, Address(obj, index.AsX86().AsCpuRegister(), TIMES_1, data_offset));
      }
      break;
    }

    case Primitive::kPrimByte: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int8_t)).Uint32Value();
      Register out = locations->Out().AsX86().AsCpuRegister();
      if (index.IsConstant()) {
        __ movsxb(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset));
      } else {
        __ movsxb(out, Address(obj, index.AsX86().AsCpuRegister(), TIMES_1, data_offset));
      }
      break;
    }

    case Primitive::kPrimShort: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int16_t)).Uint32Value();
      Register out = locations->Out().AsX86().AsCpuRegister();
      if (index.IsConstant()) {
        __ movsxw(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset));
      } else {
        __ movsxw(out, Address(obj, index.AsX86().AsCpuRegister(), TIMES_2, data_offset));
      }
      break;
    }

    case Primitive::kPrimChar: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Uint32Value();
      Register out = locations->Out().AsX86().AsCpuRegister();
      if (index.IsConstant()) {
        __ movzxw(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset));
      } else {
        __ movzxw(out, Address(obj, index.AsX86().AsCpuRegister(), TIMES_2, data_offset));
      }
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
      Register out = locations->Out().AsX86().AsCpuRegister();
      if (index.IsConstant()) {
        __ movl(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset));
      } else {
        __ movl(out, Address(obj, index.AsX86().AsCpuRegister(), TIMES_4, data_offset));
      }
      break;
    }

    case Primitive::kPrimLong: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Uint32Value();
      X86ManagedRegister out = locations->Out().AsX86();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ movl(out.AsRegisterPairLow(), Address(obj, offset));
        __ movl(out.AsRegisterPairHigh(), Address(obj, offset + kX86WordSize));
      } else {
        __ movl(out.AsRegisterPairLow(),
                Address(obj, index.AsX86().AsCpuRegister(), TIMES_8, data_offset));
        __ movl(out.AsRegisterPairHigh(),
                Address(obj, index.AsX86().AsCpuRegister(), TIMES_8, data_offset + kX86WordSize));
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

void LocationsBuilderX86::VisitArraySet(HArraySet* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  Primitive::Type value_type = instruction->InputAt(2)->GetType();
  if (value_type == Primitive::kPrimNot) {
    InvokeRuntimeCallingConvention calling_convention;
    locations->SetInAt(0, X86CpuLocation(calling_convention.GetRegisterAt(0)));
    locations->SetInAt(1, X86CpuLocation(calling_convention.GetRegisterAt(1)));
    locations->SetInAt(2, X86CpuLocation(calling_convention.GetRegisterAt(2)));
    codegen_->MarkNotLeaf();
  } else {
    locations->SetInAt(0, Location::RequiresRegister());
    locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
    if (value_type == Primitive::kPrimBoolean || value_type == Primitive::kPrimByte) {
      // Ensure the value is in a byte register.
      locations->SetInAt(2, X86CpuLocation(EAX));
    } else {
      locations->SetInAt(2, Location::RequiresRegister());
    }
  }

  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitArraySet(HArraySet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsX86().AsCpuRegister();
  Location index = locations->InAt(1);
  Primitive::Type value_type = instruction->InputAt(2)->GetType();

  switch (value_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      ByteRegister value = locations->InAt(2).AsX86().AsByteRegister();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset;
        __ movb(Address(obj, offset), value);
      } else {
        __ movb(Address(obj, index.AsX86().AsCpuRegister(), TIMES_1, data_offset), value);
      }
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Uint32Value();
      Register value = locations->InAt(2).AsX86().AsCpuRegister();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset;
        __ movw(Address(obj, offset), value);
      } else {
        __ movw(Address(obj, index.AsX86().AsCpuRegister(), TIMES_2, data_offset), value);
      }
      break;
    }

    case Primitive::kPrimInt: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
      Register value = locations->InAt(2).AsX86().AsCpuRegister();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
        __ movl(Address(obj, offset), value);
      } else {
        __ movl(Address(obj, index.AsX86().AsCpuRegister(), TIMES_4, data_offset), value);
      }
      break;
    }

    case Primitive::kPrimNot: {
      DCHECK(!codegen_->IsLeafMethod());
      __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pAputObject)));
      codegen_->RecordPcInfo(instruction->GetDexPc());
      break;
    }

    case Primitive::kPrimLong: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Uint32Value();
      X86ManagedRegister value = locations->InAt(2).AsX86();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ movl(Address(obj, offset), value.AsRegisterPairLow());
        __ movl(Address(obj, offset + kX86WordSize), value.AsRegisterPairHigh());
      } else {
        __ movl(Address(obj, index.AsX86().AsCpuRegister(), TIMES_8, data_offset),
                value.AsRegisterPairLow());
        __ movl(Address(obj, index.AsX86().AsCpuRegister(), TIMES_8, data_offset + kX86WordSize),
                value.AsRegisterPairHigh());
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

void LocationsBuilderX86::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  uint32_t offset = mirror::Array::LengthOffset().Uint32Value();
  Register obj = locations->InAt(0).AsX86().AsCpuRegister();
  Register out = locations->Out().AsX86().AsCpuRegister();
  __ movl(out, Address(obj, offset));
}

void LocationsBuilderX86::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  // TODO: Have a normalization phase that makes this instruction never used.
  locations->SetOut(Location::SameAsFirstInput());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  SlowPathCode* slow_path = new (GetGraph()->GetArena()) BoundsCheckSlowPathX86(
      instruction->GetDexPc(), locations->InAt(0), locations->InAt(1));
  codegen_->AddSlowPath(slow_path);

  Register index = locations->InAt(0).AsX86().AsCpuRegister();
  Register length = locations->InAt(1).AsX86().AsCpuRegister();

  __ cmpl(index, length);
  __ j(kAboveEqual, slow_path->GetEntryLabel());
}

void LocationsBuilderX86::VisitTemporary(HTemporary* temp) {
  temp->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitTemporary(HTemporary* temp) {
  // Nothing to do, this is driven by the code generator.
}

void LocationsBuilderX86::VisitParallelMove(HParallelMove* instruction) {
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorX86::VisitParallelMove(HParallelMove* instruction) {
  codegen_->GetMoveResolver()->EmitNativeCode(instruction);
}

X86Assembler* ParallelMoveResolverX86::GetAssembler() const {
  return codegen_->GetAssembler();
}

void ParallelMoveResolverX86::MoveMemoryToMemory(int dst, int src) {
  ScratchRegisterScope ensure_scratch(
      this, kNoRegister, EAX, codegen_->GetNumberOfCoreRegisters());
  int stack_offset = ensure_scratch.IsSpilled() ? kX86WordSize : 0;
  __ movl(static_cast<Register>(ensure_scratch.GetRegister()), Address(ESP, src + stack_offset));
  __ movl(Address(ESP, dst + stack_offset), static_cast<Register>(ensure_scratch.GetRegister()));
}

void ParallelMoveResolverX86::EmitMove(size_t index) {
  MoveOperands* move = moves_.Get(index);
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister()) {
    if (destination.IsRegister()) {
      __ movl(destination.AsX86().AsCpuRegister(), source.AsX86().AsCpuRegister());
    } else {
      DCHECK(destination.IsStackSlot());
      __ movl(Address(ESP, destination.GetStackIndex()), source.AsX86().AsCpuRegister());
    }
  } else if (source.IsStackSlot()) {
    if (destination.IsRegister()) {
      __ movl(destination.AsX86().AsCpuRegister(), Address(ESP, source.GetStackIndex()));
    } else {
      DCHECK(destination.IsStackSlot());
      MoveMemoryToMemory(destination.GetStackIndex(),
                         source.GetStackIndex());
    }
  } else if (source.IsConstant()) {
    HIntConstant* instruction = source.GetConstant()->AsIntConstant();
    Immediate imm(instruction->AsIntConstant()->GetValue());
    if (destination.IsRegister()) {
      __ movl(destination.AsX86().AsCpuRegister(), imm);
    } else {
      __ movl(Address(ESP, destination.GetStackIndex()), imm);
    }
  } else {
    LOG(FATAL) << "Unimplemented";
  }
}

void ParallelMoveResolverX86::Exchange(Register reg, int mem) {
  Register suggested_scratch = reg == EAX ? EBX : EAX;
  ScratchRegisterScope ensure_scratch(
      this, reg, suggested_scratch, codegen_->GetNumberOfCoreRegisters());

  int stack_offset = ensure_scratch.IsSpilled() ? kX86WordSize : 0;
  __ movl(static_cast<Register>(ensure_scratch.GetRegister()), Address(ESP, mem + stack_offset));
  __ movl(Address(ESP, mem + stack_offset), reg);
  __ movl(reg, static_cast<Register>(ensure_scratch.GetRegister()));
}

void ParallelMoveResolverX86::Exchange(int mem1, int mem2) {
  ScratchRegisterScope ensure_scratch1(
      this, kNoRegister, EAX, codegen_->GetNumberOfCoreRegisters());

  Register suggested_scratch = ensure_scratch1.GetRegister() == EAX ? EBX : EAX;
  ScratchRegisterScope ensure_scratch2(
      this, ensure_scratch1.GetRegister(), suggested_scratch, codegen_->GetNumberOfCoreRegisters());

  int stack_offset = ensure_scratch1.IsSpilled() ? kX86WordSize : 0;
  stack_offset += ensure_scratch2.IsSpilled() ? kX86WordSize : 0;
  __ movl(static_cast<Register>(ensure_scratch1.GetRegister()), Address(ESP, mem1 + stack_offset));
  __ movl(static_cast<Register>(ensure_scratch2.GetRegister()), Address(ESP, mem2 + stack_offset));
  __ movl(Address(ESP, mem2 + stack_offset), static_cast<Register>(ensure_scratch1.GetRegister()));
  __ movl(Address(ESP, mem1 + stack_offset), static_cast<Register>(ensure_scratch2.GetRegister()));
}

void ParallelMoveResolverX86::EmitSwap(size_t index) {
  MoveOperands* move = moves_.Get(index);
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister() && destination.IsRegister()) {
    __ xchgl(destination.AsX86().AsCpuRegister(), source.AsX86().AsCpuRegister());
  } else if (source.IsRegister() && destination.IsStackSlot()) {
    Exchange(source.AsX86().AsCpuRegister(), destination.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsRegister()) {
    Exchange(destination.AsX86().AsCpuRegister(), source.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsStackSlot()) {
    Exchange(destination.GetStackIndex(), source.GetStackIndex());
  } else {
    LOG(FATAL) << "Unimplemented";
  }
}

void ParallelMoveResolverX86::SpillScratch(int reg) {
  __ pushl(static_cast<Register>(reg));
}

void ParallelMoveResolverX86::RestoreScratch(int reg) {
  __ popl(static_cast<Register>(reg));
}

}  // namespace x86
}  // namespace art
