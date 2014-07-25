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

#include "code_generator_x86_64.h"

#include "entrypoints/quick/quick_entrypoints.h"
#include "gc/accounting/card_table.h"
#include "mirror/array.h"
#include "mirror/art_method.h"
#include "mirror/object_reference.h"
#include "thread.h"
#include "utils/assembler.h"
#include "utils/stack_checks.h"
#include "utils/x86_64/assembler_x86_64.h"
#include "utils/x86_64/managed_register_x86_64.h"

namespace art {

x86_64::X86_64ManagedRegister Location::AsX86_64() const {
  return reg().AsX86_64();
}

namespace x86_64 {

static constexpr bool kExplicitStackOverflowCheck = true;

// Some x86_64 instructions require a register to be available as temp.
static constexpr Register TMP = R11;

static constexpr int kNumberOfPushedRegistersAtEntry = 1;
static constexpr int kCurrentMethodStackOffset = 0;

static Location X86_64CpuLocation(Register reg) {
  return Location::RegisterLocation(X86_64ManagedRegister::FromCpuRegister(reg));
}

static constexpr Register kRuntimeParameterCoreRegisters[] = { RDI, RSI, RDX };
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

#define __ reinterpret_cast<X86_64Assembler*>(codegen->GetAssembler())->

class NullCheckSlowPathX86_64 : public SlowPathCode {
 public:
  explicit NullCheckSlowPathX86_64(uint32_t dex_pc) : dex_pc_(dex_pc) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    __ gs()->call(
        Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pThrowNullPointer), true));
    codegen->RecordPcInfo(dex_pc_);
  }

 private:
  const uint32_t dex_pc_;
  DISALLOW_COPY_AND_ASSIGN(NullCheckSlowPathX86_64);
};

class StackOverflowCheckSlowPathX86_64 : public SlowPathCode {
 public:
  StackOverflowCheckSlowPathX86_64() {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    __ addq(CpuRegister(RSP),
            Immediate(codegen->GetFrameSize() - kNumberOfPushedRegistersAtEntry * kX86_64WordSize));
    __ gs()->jmp(
        Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pThrowStackOverflow), true));
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(StackOverflowCheckSlowPathX86_64);
};

class BoundsCheckSlowPathX86_64 : public SlowPathCode {
 public:
  explicit BoundsCheckSlowPathX86_64(uint32_t dex_pc,
                                     Location index_location,
                                     Location length_location)
      : dex_pc_(dex_pc), index_location_(index_location), length_location_(length_location) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorX86_64* x64_codegen = reinterpret_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());
    InvokeRuntimeCallingConvention calling_convention;
    x64_codegen->Move(X86_64CpuLocation(calling_convention.GetRegisterAt(0)), index_location_);
    x64_codegen->Move(X86_64CpuLocation(calling_convention.GetRegisterAt(1)), length_location_);
    __ gs()->call(Address::Absolute(
        QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pThrowArrayBounds), true));
    codegen->RecordPcInfo(dex_pc_);
  }

 private:
  const uint32_t dex_pc_;
  const Location index_location_;
  const Location length_location_;

  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathX86_64);
};

#undef __
#define __ reinterpret_cast<X86_64Assembler*>(GetAssembler())->

inline Condition X86_64Condition(IfCondition cond) {
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

void CodeGeneratorX86_64::DumpCoreRegister(std::ostream& stream, int reg) const {
  stream << X86_64ManagedRegister::FromCpuRegister(Register(reg));
}

void CodeGeneratorX86_64::DumpFloatingPointRegister(std::ostream& stream, int reg) const {
  stream << X86_64ManagedRegister::FromXmmRegister(FloatRegister(reg));
}

CodeGeneratorX86_64::CodeGeneratorX86_64(HGraph* graph)
      : CodeGenerator(graph, kNumberOfRegIds),
        location_builder_(graph, this),
        instruction_visitor_(graph, this),
        move_resolver_(graph->GetArena(), this) {}

size_t CodeGeneratorX86_64::FrameEntrySpillSize() const {
  return kNumberOfPushedRegistersAtEntry * kX86_64WordSize;
}

InstructionCodeGeneratorX86_64::InstructionCodeGeneratorX86_64(HGraph* graph,
                                                               CodeGeneratorX86_64* codegen)
      : HGraphVisitor(graph),
        assembler_(codegen->GetAssembler()),
        codegen_(codegen) {}

ManagedRegister CodeGeneratorX86_64::AllocateFreeRegister(Primitive::Type type,
                                                          bool* blocked_registers) const {
  switch (type) {
    case Primitive::kPrimLong:
    case Primitive::kPrimByte:
    case Primitive::kPrimBoolean:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      size_t reg = AllocateFreeRegisterInternal(blocked_registers, kNumberOfCpuRegisters);
      return X86_64ManagedRegister::FromCpuRegister(static_cast<Register>(reg));
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register type " << type;

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << type;
  }

  return ManagedRegister::NoRegister();
}

void CodeGeneratorX86_64::SetupBlockedRegisters(bool* blocked_registers) const {
  // Stack register is always reserved.
  blocked_registers[RSP] = true;

  // Block the register used as TMP.
  blocked_registers[TMP] = true;

  // TODO: We currently don't use Quick's callee saved registers.
  blocked_registers[RBX] = true;
  blocked_registers[RBP] = true;
  blocked_registers[R12] = true;
  blocked_registers[R13] = true;
  blocked_registers[R14] = true;
  blocked_registers[R15] = true;
}

void CodeGeneratorX86_64::GenerateFrameEntry() {
  // Create a fake register to mimic Quick.
  static const int kFakeReturnRegister = 16;
  core_spill_mask_ |= (1 << kFakeReturnRegister);

  // The return PC has already been pushed on the stack.
  __ subq(CpuRegister(RSP),
          Immediate(GetFrameSize() - kNumberOfPushedRegistersAtEntry * kX86_64WordSize));

  bool skip_overflow_check = IsLeafMethod()
      && !FrameNeedsStackCheck(GetFrameSize(), InstructionSet::kX86_64);

  if (!skip_overflow_check) {
    if (kExplicitStackOverflowCheck) {
      SlowPathCode* slow_path = new (GetGraph()->GetArena()) StackOverflowCheckSlowPathX86_64();
      AddSlowPath(slow_path);

      __ gs()->cmpq(CpuRegister(RSP),
                    Address::Absolute(Thread::StackEndOffset<kX86_64WordSize>(), true));
      __ j(kLess, slow_path->GetEntryLabel());
    } else {
      __ testq(CpuRegister(RAX), Address(
          CpuRegister(RSP), -static_cast<int32_t>(GetStackOverflowReservedBytes(kX86_64))));
    }
  }

  __ movl(Address(CpuRegister(RSP), kCurrentMethodStackOffset), CpuRegister(RDI));
}

void CodeGeneratorX86_64::GenerateFrameExit() {
  __ addq(CpuRegister(RSP),
          Immediate(GetFrameSize() - kNumberOfPushedRegistersAtEntry * kX86_64WordSize));
}

void CodeGeneratorX86_64::Bind(Label* label) {
  __ Bind(label);
}

void InstructionCodeGeneratorX86_64::LoadCurrentMethod(CpuRegister reg) {
  __ movl(reg, Address(CpuRegister(RSP), kCurrentMethodStackOffset));
}

Location CodeGeneratorX86_64::GetStackLocation(HLoadLocal* load) const {
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

void CodeGeneratorX86_64::Move(Location destination, Location source) {
  if (source.Equals(destination)) {
    return;
  }
  if (destination.IsRegister()) {
    if (source.IsRegister()) {
      __ movq(destination.AsX86_64().AsCpuRegister(), source.AsX86_64().AsCpuRegister());
    } else if (source.IsStackSlot()) {
      __ movl(destination.AsX86_64().AsCpuRegister(), Address(CpuRegister(RSP), source.GetStackIndex()));
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ movq(destination.AsX86_64().AsCpuRegister(), Address(CpuRegister(RSP), source.GetStackIndex()));
    }
  } else if (destination.IsStackSlot()) {
    if (source.IsRegister()) {
      __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()), source.AsX86_64().AsCpuRegister());
    } else {
      DCHECK(source.IsStackSlot());
      __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), source.GetStackIndex()));
      __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
    }
  } else {
    DCHECK(destination.IsDoubleStackSlot());
    if (source.IsRegister()) {
      __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()), source.AsX86_64().AsCpuRegister());
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ movq(CpuRegister(TMP), Address(CpuRegister(RSP), source.GetStackIndex()));
      __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
    }
  }
}

void CodeGeneratorX86_64::Move(HInstruction* instruction,
                               Location location,
                               HInstruction* move_for) {
  if (instruction->AsIntConstant() != nullptr) {
    Immediate imm(instruction->AsIntConstant()->GetValue());
    if (location.IsRegister()) {
      __ movl(location.AsX86_64().AsCpuRegister(), imm);
    } else {
      __ movl(Address(CpuRegister(RSP), location.GetStackIndex()), imm);
    }
  } else if (instruction->AsLongConstant() != nullptr) {
    int64_t value = instruction->AsLongConstant()->GetValue();
    if (location.IsRegister()) {
      __ movq(location.AsX86_64().AsCpuRegister(), Immediate(value));
    } else {
      __ movq(CpuRegister(TMP), Immediate(value));
      __ movq(Address(CpuRegister(RSP), location.GetStackIndex()), CpuRegister(TMP));
    }
  } else if (instruction->AsLoadLocal() != nullptr) {
    switch (instruction->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
      case Primitive::kPrimNot:
        Move(location, Location::StackSlot(GetStackSlot(instruction->AsLoadLocal()->GetLocal())));
        break;

      case Primitive::kPrimLong:
        Move(location, Location::DoubleStackSlot(GetStackSlot(instruction->AsLoadLocal()->GetLocal())));
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
      case Primitive::kPrimLong:
        Move(location, instruction->GetLocations()->Out());
        break;

      default:
        LOG(FATAL) << "Unimplemented type " << instruction->GetType();
    }
  }
}

void LocationsBuilderX86_64::VisitGoto(HGoto* got) {
  got->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitGoto(HGoto* got) {
  HBasicBlock* successor = got->GetSuccessor();
  if (GetGraph()->GetExitBlock() == successor) {
    codegen_->GenerateFrameExit();
  } else if (!codegen_->GoesToNextBlock(got->GetBlock(), successor)) {
    __ jmp(codegen_->GetLabelOf(successor));
  }
}

void LocationsBuilderX86_64::VisitExit(HExit* exit) {
  exit->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitExit(HExit* exit) {
  if (kIsDebugBuild) {
    __ Comment("Unreachable");
    __ int3();
  }
}

void LocationsBuilderX86_64::VisitIf(HIf* if_instr) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(if_instr);
  HInstruction* cond = if_instr->InputAt(0);
  DCHECK(cond->IsCondition());
  HCondition* condition = cond->AsCondition();
  if (condition->NeedsMaterialization()) {
    locations->SetInAt(0, Location::Any());
  }
  if_instr->SetLocations(locations);
}

void InstructionCodeGeneratorX86_64::VisitIf(HIf* if_instr) {
  HInstruction* cond = if_instr->InputAt(0);
  DCHECK(cond->IsCondition());
  HCondition* condition = cond->AsCondition();
  if (condition->NeedsMaterialization()) {
    // Materialized condition, compare against 0.
    Location lhs = if_instr->GetLocations()->InAt(0);
    if (lhs.IsRegister()) {
      __ cmpl(lhs.AsX86_64().AsCpuRegister(), Immediate(0));
    } else {
      __ cmpl(Address(CpuRegister(RSP), lhs.GetStackIndex()), Immediate(0));
    }
    __ j(kEqual, codegen_->GetLabelOf(if_instr->IfTrueSuccessor()));
  } else {
    Location lhs = condition->GetLocations()->InAt(0);
    Location rhs = condition->GetLocations()->InAt(1);
    if (rhs.IsRegister()) {
      __ cmpl(lhs.AsX86_64().AsCpuRegister(), rhs.AsX86_64().AsCpuRegister());
    } else if (rhs.IsConstant()) {
      __ cmpl(lhs.AsX86_64().AsCpuRegister(),
              Immediate(rhs.GetConstant()->AsIntConstant()->GetValue()));
    } else {
      __ cmpl(lhs.AsX86_64().AsCpuRegister(), Address(CpuRegister(RSP), rhs.GetStackIndex()));
    }
    __ j(X86_64Condition(condition->GetCondition()),
         codegen_->GetLabelOf(if_instr->IfTrueSuccessor()));
  }
  if (!codegen_->GoesToNextBlock(if_instr->GetBlock(), if_instr->IfFalseSuccessor())) {
    __ jmp(codegen_->GetLabelOf(if_instr->IfFalseSuccessor()));
  }
}

void LocationsBuilderX86_64::VisitLocal(HLocal* local) {
  local->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitLocal(HLocal* local) {
  DCHECK_EQ(local->GetBlock(), GetGraph()->GetEntryBlock());
}

void LocationsBuilderX86_64::VisitLoadLocal(HLoadLocal* local) {
  local->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitLoadLocal(HLoadLocal* load) {
  // Nothing to do, this is driven by the code generator.
}

void LocationsBuilderX86_64::VisitStoreLocal(HStoreLocal* store) {
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

void InstructionCodeGeneratorX86_64::VisitStoreLocal(HStoreLocal* store) {
}

void LocationsBuilderX86_64::VisitCondition(HCondition* comp) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(comp);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::Any());
  if (comp->NeedsMaterialization()) {
    locations->SetOut(Location::RequiresRegister());
  }
  comp->SetLocations(locations);
}

void InstructionCodeGeneratorX86_64::VisitCondition(HCondition* comp) {
  if (comp->NeedsMaterialization()) {
    LocationSummary* locations = comp->GetLocations();
    if (locations->InAt(1).IsRegister()) {
      __ cmpq(locations->InAt(0).AsX86_64().AsCpuRegister(),
              locations->InAt(1).AsX86_64().AsCpuRegister());
    } else if (locations->InAt(1).IsConstant()) {
      __ cmpq(locations->InAt(0).AsX86_64().AsCpuRegister(),
              Immediate(locations->InAt(1).GetConstant()->AsIntConstant()->GetValue()));
    } else {
      __ cmpq(locations->InAt(0).AsX86_64().AsCpuRegister(),
              Address(CpuRegister(RSP), locations->InAt(1).GetStackIndex()));
    }
    __ setcc(X86_64Condition(comp->GetCondition()),
             comp->GetLocations()->Out().AsX86_64().AsCpuRegister());
  }
}

void LocationsBuilderX86_64::VisitEqual(HEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitEqual(HEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86_64::VisitNotEqual(HNotEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitNotEqual(HNotEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86_64::VisitLessThan(HLessThan* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitLessThan(HLessThan* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86_64::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86_64::VisitGreaterThan(HGreaterThan* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitGreaterThan(HGreaterThan* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86_64::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86_64::VisitCompare(HCompare* compare) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(compare);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
  compare->SetLocations(locations);
}

void InstructionCodeGeneratorX86_64::VisitCompare(HCompare* compare) {
  Label greater, done;
  LocationSummary* locations = compare->GetLocations();
  switch (compare->InputAt(0)->GetType()) {
    case Primitive::kPrimLong:
      __ cmpq(locations->InAt(0).AsX86_64().AsCpuRegister(),
              locations->InAt(1).AsX86_64().AsCpuRegister());
      break;
    default:
      LOG(FATAL) << "Unimplemented compare type " << compare->InputAt(0)->GetType();
  }

  __ movl(locations->Out().AsX86_64().AsCpuRegister(), Immediate(0));
  __ j(kEqual, &done);
  __ j(kGreater, &greater);

  __ movl(locations->Out().AsX86_64().AsCpuRegister(), Immediate(-1));
  __ jmp(&done);

  __ Bind(&greater);
  __ movl(locations->Out().AsX86_64().AsCpuRegister(), Immediate(1));

  __ Bind(&done);
}

void LocationsBuilderX86_64::VisitIntConstant(HIntConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
  constant->SetLocations(locations);
}

void InstructionCodeGeneratorX86_64::VisitIntConstant(HIntConstant* constant) {
}

void LocationsBuilderX86_64::VisitLongConstant(HLongConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
  constant->SetLocations(locations);
}

void InstructionCodeGeneratorX86_64::VisitLongConstant(HLongConstant* constant) {
}

void LocationsBuilderX86_64::VisitReturnVoid(HReturnVoid* ret) {
  ret->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitReturnVoid(HReturnVoid* ret) {
  codegen_->GenerateFrameExit();
  __ ret();
}

void LocationsBuilderX86_64::VisitReturn(HReturn* ret) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(ret);
  switch (ret->InputAt(0)->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimLong:
      locations->SetInAt(0, X86_64CpuLocation(RAX));
      break;

    default:
      LOG(FATAL) << "Unimplemented return type " << ret->InputAt(0)->GetType();
  }
  ret->SetLocations(locations);
}

void InstructionCodeGeneratorX86_64::VisitReturn(HReturn* ret) {
  if (kIsDebugBuild) {
    switch (ret->InputAt(0)->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
      case Primitive::kPrimNot:
      case Primitive::kPrimLong:
        DCHECK_EQ(ret->GetLocations()->InAt(0).AsX86_64().AsCpuRegister().AsRegister(), RAX);
        break;

      default:
        LOG(FATAL) << "Unimplemented return type " << ret->InputAt(0)->GetType();
    }
  }
  codegen_->GenerateFrameExit();
  __ ret();
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
      stack_index_++;
      if (index < calling_convention.GetNumberOfRegisters()) {
        return X86_64CpuLocation(calling_convention.GetRegisterAt(index));
      } else {
        return Location::StackSlot(calling_convention.GetStackOffsetOf(stack_index_ - 1));
      }
    }

    case Primitive::kPrimLong: {
      uint32_t index = gp_index_;
      stack_index_ += 2;
      if (index < calling_convention.GetNumberOfRegisters()) {
        gp_index_ += 1;
        return X86_64CpuLocation(calling_convention.GetRegisterAt(index));
      } else {
        gp_index_ += 2;
        return Location::DoubleStackSlot(calling_convention.GetStackOffsetOf(stack_index_ - 2));
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

void LocationsBuilderX86_64::VisitInvokeStatic(HInvokeStatic* invoke) {
  codegen_->MarkNotLeaf();
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(invoke);
  locations->AddTemp(X86_64CpuLocation(RDI));

  InvokeDexCallingConventionVisitor calling_convention_visitor;
  for (size_t i = 0; i < invoke->InputCount(); ++i) {
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
    case Primitive::kPrimLong:
      locations->SetOut(X86_64CpuLocation(RAX));
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

void InstructionCodeGeneratorX86_64::VisitInvokeStatic(HInvokeStatic* invoke) {
  CpuRegister temp = invoke->GetLocations()->GetTemp(0).AsX86_64().AsCpuRegister();
  uint32_t heap_reference_size = sizeof(mirror::HeapReference<mirror::Object>);
  size_t index_in_cache = mirror::Array::DataOffset(heap_reference_size).SizeValue() +
      invoke->GetIndexInDexCache() * heap_reference_size;

  // TODO: Implement all kinds of calls:
  // 1) boot -> boot
  // 2) app -> boot
  // 3) app -> app
  //
  // Currently we implement the app -> app logic, which looks up in the resolve cache.

  // temp = method;
  LoadCurrentMethod(temp);
  // temp = temp->dex_cache_resolved_methods_;
  __ movl(temp, Address(temp, mirror::ArtMethod::DexCacheResolvedMethodsOffset().SizeValue()));
  // temp = temp[index_in_cache]
  __ movl(temp, Address(temp, index_in_cache));
  // (temp + offset_of_quick_compiled_code)()
  __ call(Address(temp, mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset().SizeValue()));

  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke->GetDexPc());
}

void LocationsBuilderX86_64::VisitAdd(HAdd* add) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(add);
  switch (add->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
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

void InstructionCodeGeneratorX86_64::VisitAdd(HAdd* add) {
  LocationSummary* locations = add->GetLocations();
  DCHECK_EQ(locations->InAt(0).AsX86_64().AsCpuRegister().AsRegister(),
            locations->Out().AsX86_64().AsCpuRegister().AsRegister());
  switch (add->GetResultType()) {
    case Primitive::kPrimInt: {
      if (locations->InAt(1).IsRegister()) {
        __ addl(locations->InAt(0).AsX86_64().AsCpuRegister(),
                locations->InAt(1).AsX86_64().AsCpuRegister());
      } else if (locations->InAt(1).IsConstant()) {
        HConstant* instruction = locations->InAt(1).GetConstant();
        Immediate imm(instruction->AsIntConstant()->GetValue());
        __ addl(locations->InAt(0).AsX86_64().AsCpuRegister(), imm);
      } else {
        __ addl(locations->InAt(0).AsX86_64().AsCpuRegister(),
                Address(CpuRegister(RSP), locations->InAt(1).GetStackIndex()));
      }
      break;
    }
    case Primitive::kPrimLong: {
      __ addq(locations->InAt(0).AsX86_64().AsCpuRegister(),
              locations->InAt(1).AsX86_64().AsCpuRegister());
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

void LocationsBuilderX86_64::VisitSub(HSub* sub) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(sub);
  switch (sub->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
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

void InstructionCodeGeneratorX86_64::VisitSub(HSub* sub) {
  LocationSummary* locations = sub->GetLocations();
  DCHECK_EQ(locations->InAt(0).AsX86_64().AsCpuRegister().AsRegister(),
            locations->Out().AsX86_64().AsCpuRegister().AsRegister());
  switch (sub->GetResultType()) {
    case Primitive::kPrimInt: {
      if (locations->InAt(1).IsRegister()) {
        __ subl(locations->InAt(0).AsX86_64().AsCpuRegister(),
                locations->InAt(1).AsX86_64().AsCpuRegister());
      } else if (locations->InAt(1).IsConstant()) {
        HConstant* instruction = locations->InAt(1).GetConstant();
        Immediate imm(instruction->AsIntConstant()->GetValue());
        __ subl(locations->InAt(0).AsX86_64().AsCpuRegister(), imm);
      } else {
        __ subl(locations->InAt(0).AsX86_64().AsCpuRegister(),
                Address(CpuRegister(RSP), locations->InAt(1).GetStackIndex()));
      }
      break;
    }
    case Primitive::kPrimLong: {
      __ subq(locations->InAt(0).AsX86_64().AsCpuRegister(),
              locations->InAt(1).AsX86_64().AsCpuRegister());
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

void LocationsBuilderX86_64::VisitNewInstance(HNewInstance* instruction) {
  codegen_->MarkNotLeaf();
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetOut(X86_64CpuLocation(RAX));
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86_64::VisitNewInstance(HNewInstance* instruction) {
  InvokeRuntimeCallingConvention calling_convention;
  LoadCurrentMethod(CpuRegister(calling_convention.GetRegisterAt(1)));
  __ movq(CpuRegister(calling_convention.GetRegisterAt(0)), Immediate(instruction->GetTypeIndex()));

  __ gs()->call(Address::Absolute(
      QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pAllocObjectWithAccessCheck), true));

  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(instruction->GetDexPc());
}

void LocationsBuilderX86_64::VisitParameterValue(HParameterValue* instruction) {
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

void InstructionCodeGeneratorX86_64::VisitParameterValue(HParameterValue* instruction) {
  // Nothing to do, the parameter is already at its location.
}

void LocationsBuilderX86_64::VisitNot(HNot* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86_64::VisitNot(HNot* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  DCHECK_EQ(locations->InAt(0).AsX86_64().AsCpuRegister().AsRegister(),
            locations->Out().AsX86_64().AsCpuRegister().AsRegister());
  __ xorq(locations->Out().AsX86_64().AsCpuRegister(), Immediate(1));
}

void LocationsBuilderX86_64::VisitPhi(HPhi* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    locations->SetInAt(i, Location::Any());
  }
  locations->SetOut(Location::Any());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86_64::VisitPhi(HPhi* instruction) {
  LOG(FATAL) << "Unimplemented";
}

void LocationsBuilderX86_64::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
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

void InstructionCodeGeneratorX86_64::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  CpuRegister obj = locations->InAt(0).AsX86_64().AsCpuRegister();
  CpuRegister value = locations->InAt(1).AsX86_64().AsCpuRegister();
  size_t offset = instruction->GetFieldOffset().SizeValue();
  Primitive::Type field_type = instruction->InputAt(1)->GetType();

  switch (field_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      __ movb(Address(obj, offset), value);
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      __ movw(Address(obj, offset), value);
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      __ movl(Address(obj, offset), value);
      if (field_type == Primitive::kPrimNot) {
        CpuRegister temp = locations->GetTemp(0).AsX86_64().AsCpuRegister();
        CpuRegister card = locations->GetTemp(1).AsX86_64().AsCpuRegister();
        codegen_->MarkGCCard(temp, card, obj, value);
      }
      break;
    }

    case Primitive::kPrimLong: {
      __ movq(Address(obj, offset), value);
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register type " << field_type;

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << field_type;
  }
}

void LocationsBuilderX86_64::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86_64::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  CpuRegister obj = locations->InAt(0).AsX86_64().AsCpuRegister();
  CpuRegister out = locations->Out().AsX86_64().AsCpuRegister();
  size_t offset = instruction->GetFieldOffset().SizeValue();

  switch (instruction->GetType()) {
    case Primitive::kPrimBoolean: {
      __ movzxb(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimByte: {
      __ movsxb(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimShort: {
      __ movsxw(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimChar: {
      __ movzxw(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      __ movl(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimLong: {
      __ movq(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register type " << instruction->GetType();

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
  }
}

void LocationsBuilderX86_64::VisitNullCheck(HNullCheck* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::Any());
  // TODO: Have a normalization phase that makes this instruction never used.
  locations->SetOut(Location::SameAsFirstInput());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86_64::VisitNullCheck(HNullCheck* instruction) {
  SlowPathCode* slow_path =
      new (GetGraph()->GetArena()) NullCheckSlowPathX86_64(instruction->GetDexPc());
  codegen_->AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location obj = locations->InAt(0);
  DCHECK(obj.Equals(locations->Out()));

  if (obj.IsRegister()) {
    __ cmpl(obj.AsX86_64().AsCpuRegister(), Immediate(0));
  } else {
    DCHECK(locations->InAt(0).IsStackSlot());
    __ cmpl(Address(CpuRegister(RSP), obj.GetStackIndex()), Immediate(0));
  }
  __ j(kEqual, slow_path->GetEntryLabel());
}

void LocationsBuilderX86_64::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  locations->SetOut(Location::RequiresRegister());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86_64::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  CpuRegister obj = locations->InAt(0).AsX86_64().AsCpuRegister();
  Location index = locations->InAt(1);

  switch (instruction->GetType()) {
    case Primitive::kPrimBoolean: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      CpuRegister out = locations->Out().AsX86_64().AsCpuRegister();
      if (index.IsConstant()) {
        __ movzxb(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset));
      } else {
        __ movzxb(out, Address(obj, index.AsX86_64().AsCpuRegister(), TIMES_1, data_offset));
      }
      break;
    }

    case Primitive::kPrimByte: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int8_t)).Uint32Value();
      CpuRegister out = locations->Out().AsX86_64().AsCpuRegister();
      if (index.IsConstant()) {
        __ movsxb(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset));
      } else {
        __ movsxb(out, Address(obj, index.AsX86_64().AsCpuRegister(), TIMES_1, data_offset));
      }
      break;
    }

    case Primitive::kPrimShort: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int16_t)).Uint32Value();
      CpuRegister out = locations->Out().AsX86_64().AsCpuRegister();
      if (index.IsConstant()) {
        __ movsxw(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset));
      } else {
        __ movsxw(out, Address(obj, index.AsX86_64().AsCpuRegister(), TIMES_2, data_offset));
      }
      break;
    }

    case Primitive::kPrimChar: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Uint32Value();
      CpuRegister out = locations->Out().AsX86_64().AsCpuRegister();
      if (index.IsConstant()) {
        __ movzxw(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset));
      } else {
        __ movzxw(out, Address(obj, index.AsX86_64().AsCpuRegister(), TIMES_2, data_offset));
      }
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      DCHECK_EQ(sizeof(mirror::HeapReference<mirror::Object>), sizeof(int32_t));
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
      CpuRegister out = locations->Out().AsX86_64().AsCpuRegister();
      if (index.IsConstant()) {
        __ movl(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset));
      } else {
        __ movl(out, Address(obj, index.AsX86_64().AsCpuRegister(), TIMES_4, data_offset));
      }
      break;
    }

    case Primitive::kPrimLong: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Uint32Value();
      CpuRegister out = locations->Out().AsX86_64().AsCpuRegister();
      if (index.IsConstant()) {
        __ movq(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset));
      } else {
        __ movq(out, Address(obj, index.AsX86_64().AsCpuRegister(), TIMES_8, data_offset));
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

void LocationsBuilderX86_64::VisitArraySet(HArraySet* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  Primitive::Type value_type = instruction->InputAt(2)->GetType();
  if (value_type == Primitive::kPrimNot) {
    InvokeRuntimeCallingConvention calling_convention;
    locations->SetInAt(0, X86_64CpuLocation(calling_convention.GetRegisterAt(0)));
    locations->SetInAt(1, X86_64CpuLocation(calling_convention.GetRegisterAt(1)));
    locations->SetInAt(2, X86_64CpuLocation(calling_convention.GetRegisterAt(2)));
    codegen_->MarkNotLeaf();
  } else {
    locations->SetInAt(0, Location::RequiresRegister());
    locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
    locations->SetInAt(2, Location::RequiresRegister());
  }
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86_64::VisitArraySet(HArraySet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  CpuRegister obj = locations->InAt(0).AsX86_64().AsCpuRegister();
  Location index = locations->InAt(1);
  Primitive::Type value_type = instruction->InputAt(2)->GetType();

  switch (value_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      CpuRegister value = locations->InAt(2).AsX86_64().AsCpuRegister();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset;
        __ movb(Address(obj, offset), value);
      } else {
        __ movb(Address(obj, index.AsX86_64().AsCpuRegister(), TIMES_1, data_offset), value);
      }
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Uint32Value();
      CpuRegister value = locations->InAt(2).AsX86_64().AsCpuRegister();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset;
        __ movw(Address(obj, offset), value);
      } else {
        __ movw(Address(obj, index.AsX86_64().AsCpuRegister(), TIMES_2, data_offset), value);
      }
      break;
    }

    case Primitive::kPrimInt: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
      CpuRegister value = locations->InAt(2).AsX86_64().AsCpuRegister();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
        __ movl(Address(obj, offset), value);
      } else {
        __ movl(Address(obj, index.AsX86_64().AsCpuRegister(), TIMES_4, data_offset), value);
      }
      break;
    }

    case Primitive::kPrimNot: {
      __ gs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pAputObject), true));
      DCHECK(!codegen_->IsLeafMethod());
      codegen_->RecordPcInfo(instruction->GetDexPc());
      break;
    }

    case Primitive::kPrimLong: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Uint32Value();
      CpuRegister value = locations->InAt(2).AsX86_64().AsCpuRegister();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ movq(Address(obj, offset), value);
      } else {
        __ movq(Address(obj, index.AsX86_64().AsCpuRegister(), TIMES_8, data_offset), value);
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

void LocationsBuilderX86_64::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86_64::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  uint32_t offset = mirror::Array::LengthOffset().Uint32Value();
  CpuRegister obj = locations->InAt(0).AsX86_64().AsCpuRegister();
  CpuRegister out = locations->Out().AsX86_64().AsCpuRegister();
  __ movl(out, Address(obj, offset));
}

void LocationsBuilderX86_64::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  // TODO: Have a normalization phase that makes this instruction never used.
  locations->SetOut(Location::SameAsFirstInput());
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86_64::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  SlowPathCode* slow_path = new (GetGraph()->GetArena()) BoundsCheckSlowPathX86_64(
      instruction->GetDexPc(), locations->InAt(0), locations->InAt(1));
  codegen_->AddSlowPath(slow_path);

  CpuRegister index = locations->InAt(0).AsX86_64().AsCpuRegister();
  CpuRegister length = locations->InAt(1).AsX86_64().AsCpuRegister();

  __ cmpl(index, length);
  __ j(kAboveEqual, slow_path->GetEntryLabel());
}

void CodeGeneratorX86_64::MarkGCCard(CpuRegister temp,
                                     CpuRegister card,
                                     CpuRegister object,
                                     CpuRegister value) {
  Label is_null;
  __ testl(value, value);
  __ j(kEqual, &is_null);
  __ gs()->movq(card, Address::Absolute(
      Thread::CardTableOffset<kX86_64WordSize>().Int32Value(), true));
  __ movq(temp, object);
  __ shrq(temp, Immediate(gc::accounting::CardTable::kCardShift));
  __ movb(Address(temp, card, TIMES_1, 0),  card);
  __ Bind(&is_null);
}

void LocationsBuilderX86_64::VisitTemporary(HTemporary* temp) {
  temp->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitTemporary(HTemporary* temp) {
  // Nothing to do, this is driven by the code generator.
}

void LocationsBuilderX86_64::VisitParallelMove(HParallelMove* instruction) {
  LOG(FATAL) << "Unimplemented";
}

void InstructionCodeGeneratorX86_64::VisitParallelMove(HParallelMove* instruction) {
  codegen_->GetMoveResolver()->EmitNativeCode(instruction);
}

X86_64Assembler* ParallelMoveResolverX86_64::GetAssembler() const {
  return codegen_->GetAssembler();
}

void ParallelMoveResolverX86_64::EmitMove(size_t index) {
  MoveOperands* move = moves_.Get(index);
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister()) {
    if (destination.IsRegister()) {
      __ movq(destination.AsX86_64().AsCpuRegister(), source.AsX86_64().AsCpuRegister());
    } else if (destination.IsStackSlot()) {
      __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()),
              source.AsX86_64().AsCpuRegister());
    } else {
      DCHECK(destination.IsDoubleStackSlot());
      __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()),
              source.AsX86_64().AsCpuRegister());
    }
  } else if (source.IsStackSlot()) {
    if (destination.IsRegister()) {
      __ movl(destination.AsX86_64().AsX86_64().AsCpuRegister(),
              Address(CpuRegister(RSP), source.GetStackIndex()));
    } else {
      DCHECK(destination.IsStackSlot());
      __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), source.GetStackIndex()));
      __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
    }
  } else if (source.IsDoubleStackSlot()) {
    if (destination.IsRegister()) {
      __ movq(destination.AsX86_64().AsX86_64().AsCpuRegister(),
              Address(CpuRegister(RSP), source.GetStackIndex()));
    } else {
      DCHECK(destination.IsDoubleStackSlot());
      __ movq(CpuRegister(TMP), Address(CpuRegister(RSP), source.GetStackIndex()));
      __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
    }
  } else if (source.IsConstant()) {
    HConstant* constant = source.GetConstant();
    if (constant->IsIntConstant()) {
      Immediate imm(constant->AsIntConstant()->GetValue());
      if (destination.IsRegister()) {
        __ movl(destination.AsX86_64().AsCpuRegister(), imm);
      } else {
        __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()), imm);
      }
    } else if (constant->IsLongConstant()) {
      int64_t value = constant->AsLongConstant()->GetValue();
      if (destination.IsRegister()) {
        __ movq(destination.AsX86_64().AsCpuRegister(), Immediate(value));
      } else {
        __ movq(CpuRegister(TMP), Immediate(value));
        __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
      }
    } else {
      LOG(FATAL) << "Unimplemented constant type";
    }
  } else {
    LOG(FATAL) << "Unimplemented";
  }
}

void ParallelMoveResolverX86_64::Exchange32(CpuRegister reg, int mem) {
  __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), mem));
  __ movl(Address(CpuRegister(RSP), mem), reg);
  __ movl(reg, CpuRegister(TMP));
}

void ParallelMoveResolverX86_64::Exchange32(int mem1, int mem2) {
  ScratchRegisterScope ensure_scratch(
      this, TMP, RAX, codegen_->GetNumberOfCoreRegisters());

  int stack_offset = ensure_scratch.IsSpilled() ? kX86_64WordSize : 0;
  __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), mem1 + stack_offset));
  __ movl(CpuRegister(ensure_scratch.GetRegister()),
          Address(CpuRegister(RSP), mem2 + stack_offset));
  __ movl(Address(CpuRegister(RSP), mem2 + stack_offset), CpuRegister(TMP));
  __ movl(Address(CpuRegister(RSP), mem1 + stack_offset),
          CpuRegister(ensure_scratch.GetRegister()));
}

void ParallelMoveResolverX86_64::Exchange64(CpuRegister reg, int mem) {
  __ movq(CpuRegister(TMP), Address(CpuRegister(RSP), mem));
  __ movq(Address(CpuRegister(RSP), mem), reg);
  __ movq(reg, CpuRegister(TMP));
}

void ParallelMoveResolverX86_64::Exchange64(int mem1, int mem2) {
  ScratchRegisterScope ensure_scratch(
      this, TMP, RAX, codegen_->GetNumberOfCoreRegisters());

  int stack_offset = ensure_scratch.IsSpilled() ? kX86_64WordSize : 0;
  __ movq(CpuRegister(TMP), Address(CpuRegister(RSP), mem1 + stack_offset));
  __ movq(CpuRegister(ensure_scratch.GetRegister()),
          Address(CpuRegister(RSP), mem2 + stack_offset));
  __ movq(Address(CpuRegister(RSP), mem2 + stack_offset), CpuRegister(TMP));
  __ movq(Address(CpuRegister(RSP), mem1 + stack_offset),
          CpuRegister(ensure_scratch.GetRegister()));
}

void ParallelMoveResolverX86_64::EmitSwap(size_t index) {
  MoveOperands* move = moves_.Get(index);
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister() && destination.IsRegister()) {
    __ xchgq(destination.AsX86_64().AsCpuRegister(), source.AsX86_64().AsCpuRegister());
  } else if (source.IsRegister() && destination.IsStackSlot()) {
    Exchange32(source.AsX86_64().AsCpuRegister(), destination.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsRegister()) {
    Exchange32(destination.AsX86_64().AsCpuRegister(), source.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsStackSlot()) {
    Exchange32(destination.GetStackIndex(), source.GetStackIndex());
  } else if (source.IsRegister() && destination.IsDoubleStackSlot()) {
    Exchange64(source.AsX86_64().AsCpuRegister(), destination.GetStackIndex());
  } else if (source.IsDoubleStackSlot() && destination.IsRegister()) {
    Exchange64(destination.AsX86_64().AsCpuRegister(), source.GetStackIndex());
  } else if (source.IsDoubleStackSlot() && destination.IsDoubleStackSlot()) {
    Exchange64(destination.GetStackIndex(), source.GetStackIndex());
  } else {
    LOG(FATAL) << "Unimplemented";
  }
}


void ParallelMoveResolverX86_64::SpillScratch(int reg) {
  __ pushq(CpuRegister(reg));
}


void ParallelMoveResolverX86_64::RestoreScratch(int reg) {
  __ popq(CpuRegister(reg));
}

}  // namespace x86_64
}  // namespace art
