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
#include "mirror/array.h"
#include "mirror/art_method.h"
#include "mirror/object_reference.h"
#include "thread.h"
#include "utils/assembler.h"
#include "utils/x86_64/assembler_x86_64.h"
#include "utils/x86_64/managed_register_x86_64.h"

#define __ reinterpret_cast<X86_64Assembler*>(GetAssembler())->

namespace art {

x86_64::X86_64ManagedRegister Location::AsX86_64() const {
  return reg().AsX86_64();
}

namespace x86_64 {

// Some x86_64 instructions require a register to be available as temp.
static constexpr Register TMP = R11;

static constexpr int kNumberOfPushedRegistersAtEntry = 1;
static constexpr int kCurrentMethodStackOffset = 0;

void CodeGeneratorX86_64::DumpCoreRegister(std::ostream& stream, int reg) const {
  stream << X86_64ManagedRegister::FromCpuRegister(Register(reg));
}

void CodeGeneratorX86_64::DumpFloatingPointRegister(std::ostream& stream, int reg) const {
  stream << X86_64ManagedRegister::FromXmmRegister(FloatRegister(reg));
}

static Location X86_64CpuLocation(Register reg) {
  return Location::RegisterLocation(X86_64ManagedRegister::FromCpuRegister(reg));
}

CodeGeneratorX86_64::CodeGeneratorX86_64(HGraph* graph)
      : CodeGenerator(graph, kNumberOfRegIds),
        location_builder_(graph, this),
        instruction_visitor_(graph, this),
        move_resolver_(graph->GetArena(), this) {}

InstructionCodeGeneratorX86_64::InstructionCodeGeneratorX86_64(HGraph* graph, CodeGeneratorX86_64* codegen)
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

void CodeGeneratorX86_64::ComputeFrameSize(size_t number_of_spill_slots) {
  // Add the current ART method to the frame size, the return PC, and the filler.
  SetFrameSize(RoundUp(
      number_of_spill_slots * kVRegSize
      + kVRegSize  // filler
      + kVRegSize  // Art method
      + kNumberOfPushedRegistersAtEntry * kX86_64WordSize,
      kStackAlignment));
}

void CodeGeneratorX86_64::GenerateFrameEntry() {
  // Create a fake register to mimic Quick.
  static const int kFakeReturnRegister = 16;
  core_spill_mask_ |= (1 << kFakeReturnRegister);

  // The return PC has already been pushed on the stack.
  __ subq(CpuRegister(RSP), Immediate(GetFrameSize() - kNumberOfPushedRegistersAtEntry * kX86_64WordSize));
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

int32_t CodeGeneratorX86_64::GetStackSlot(HLocal* local) const {
  uint16_t reg_number = local->GetRegNumber();
  uint16_t number_of_vregs = GetGraph()->GetNumberOfVRegs();
  uint16_t number_of_in_vregs = GetGraph()->GetNumberOfInVRegs();
  if (reg_number >= number_of_vregs - number_of_in_vregs) {
    // Local is a parameter of the method. It is stored in the caller's frame.
    return GetFrameSize() + kVRegSize  // ART method
                          + (reg_number - number_of_vregs + number_of_in_vregs) * kVRegSize;
  } else {
    // Local is a temporary in this method. It is stored in this method's frame.
    return GetFrameSize() - (kNumberOfPushedRegistersAtEntry * kX86_64WordSize)
                          - kVRegSize
                          - (number_of_vregs * kVRegSize)
                          + (reg_number * kVRegSize);
  }
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

void CodeGeneratorX86_64::Move(HInstruction* instruction, Location location, HInstruction* move_for) {
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
    // This can currently only happen when the instruction that requests the move
    // is the next to be compiled.
    DCHECK_EQ(instruction->GetNext(), move_for);
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
  locations->SetInAt(0, Location::RequiresRegister());
  if_instr->SetLocations(locations);
}

void InstructionCodeGeneratorX86_64::VisitIf(HIf* if_instr) {
  // TODO: Generate the input as a condition, instead of materializing in a register.
  __ cmpl(if_instr->GetLocations()->InAt(0).AsX86_64().AsCpuRegister(), Immediate(0));
  __ j(kEqual, codegen_->GetLabelOf(if_instr->IfFalseSuccessor()));
  if (!codegen_->GoesToNextBlock(if_instr->GetBlock(), if_instr->IfTrueSuccessor())) {
    __ jmp(codegen_->GetLabelOf(if_instr->IfTrueSuccessor()));
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

void LocationsBuilderX86_64::VisitEqual(HEqual* equal) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(equal);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
  equal->SetLocations(locations);
}

void InstructionCodeGeneratorX86_64::VisitEqual(HEqual* equal) {
  __ cmpq(equal->GetLocations()->InAt(0).AsX86_64().AsCpuRegister(),
          equal->GetLocations()->InAt(1).AsX86_64().AsCpuRegister());
  __ setcc(kEqual, equal->GetLocations()->Out().AsX86_64().AsCpuRegister());
}

void LocationsBuilderX86_64::VisitIntConstant(HIntConstant* constant) {
  // TODO: Support constant locations.
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::RequiresRegister());
  constant->SetLocations(locations);
}

void InstructionCodeGeneratorX86_64::VisitIntConstant(HIntConstant* constant) {
  codegen_->Move(constant, constant->GetLocations()->Out(), nullptr);
}

void LocationsBuilderX86_64::VisitLongConstant(HLongConstant* constant) {
  // TODO: Support constant locations.
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::RequiresRegister());
  constant->SetLocations(locations);
}

void InstructionCodeGeneratorX86_64::VisitLongConstant(HLongConstant* constant) {
  // Will be generated at use site.
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

  codegen_->RecordPcInfo(invoke->GetDexPc());
}

void LocationsBuilderX86_64::VisitAdd(HAdd* add) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(add);
  switch (add->GetResultType()) {
    case Primitive::kPrimInt:
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
      __ addl(locations->InAt(0).AsX86_64().AsCpuRegister(),
              locations->InAt(1).AsX86_64().AsCpuRegister());
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
    case Primitive::kPrimInt:
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
      __ subl(locations->InAt(0).AsX86_64().AsCpuRegister(),
              locations->InAt(1).AsX86_64().AsCpuRegister());
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
    } else {
      DCHECK(destination.IsStackSlot());
      __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()),
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
  } else {
    LOG(FATAL) << "Unimplemented";
  }
}

void ParallelMoveResolverX86_64::Exchange(CpuRegister reg, int mem) {
  __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), mem));
  __ movl(Address(CpuRegister(RSP), mem), CpuRegister(reg));
  __ movl(CpuRegister(reg), CpuRegister(TMP));
}

void ParallelMoveResolverX86_64::Exchange(int mem1, int mem2) {
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

void ParallelMoveResolverX86_64::EmitSwap(size_t index) {
  MoveOperands* move = moves_.Get(index);
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister() && destination.IsRegister()) {
    __ xchgq(destination.AsX86_64().AsCpuRegister(), source.AsX86_64().AsCpuRegister());
  } else if (source.IsRegister() && destination.IsStackSlot()) {
    Exchange(source.AsX86_64().AsCpuRegister(), destination.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsRegister()) {
    Exchange(destination.AsX86_64().AsCpuRegister(), source.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsStackSlot()) {
    Exchange(destination.GetStackIndex(), source.GetStackIndex());
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
