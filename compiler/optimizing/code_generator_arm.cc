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

#include "mirror/array.h"
#include "mirror/art_method.h"

#define __ reinterpret_cast<ArmAssembler*>(GetAssembler())->

namespace art {
namespace arm {

static constexpr int kNumberOfPushedRegistersAtEntry = 1;
static constexpr int kCurrentMethodStackOffset = 0;

InstructionCodeGeneratorARM::InstructionCodeGeneratorARM(HGraph* graph, CodeGeneratorARM* codegen)
      : HGraphVisitor(graph),
        assembler_(codegen->GetAssembler()),
        codegen_(codegen) {}

void CodeGeneratorARM::GenerateFrameEntry() {
  core_spill_mask_ |= (1 << LR);
  __ PushList((1 << LR));

  // Add the current ART method to the frame size and the return PC.
  SetFrameSize(RoundUp(GetFrameSize() + 2 * kArmWordSize, kStackAlignment));
  // The retrn PC has already been pushed on the stack.
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
  return (GetGraph()->GetMaximumNumberOfOutVRegs() + local->GetRegNumber()) * kArmWordSize;
}

void CodeGeneratorARM::Move(HInstruction* instruction, Location location, HInstruction* move_for) {
  if (instruction->AsIntConstant() != nullptr) {
    __ LoadImmediate(location.reg<Register>(), instruction->AsIntConstant()->GetValue());
  } else if (instruction->AsLoadLocal() != nullptr) {
    __ LoadFromOffset(kLoadWord, location.reg<Register>(),
                      SP, GetStackSlot(instruction->AsLoadLocal()->GetLocal()));
  } else {
    // This can currently only happen when the instruction that requests the move
    // is the next to be compiled.
    DCHECK_EQ(instruction->GetNext(), move_for);
    __ mov(location.reg<Register>(),
           ShifterOperand(instruction->GetLocations()->Out().reg<Register>()));
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
  locations->SetInAt(0, Location(R0));
  if_instr->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitIf(HIf* if_instr) {
  // TODO: Generate the input as a condition, instead of materializing in a register.
  __ cmp(if_instr->GetLocations()->InAt(0).reg<Register>(), ShifterOperand(0));
  __ b(codegen_->GetLabelOf(if_instr->IfFalseSuccessor()), EQ);
  if (!codegen_->GoesToNextBlock(if_instr->GetBlock(), if_instr->IfTrueSuccessor())) {
    __ b(codegen_->GetLabelOf(if_instr->IfTrueSuccessor()));
  }
}

void LocationsBuilderARM::VisitEqual(HEqual* equal) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(equal);
  locations->SetInAt(0, Location(R0));
  locations->SetInAt(1, Location(R1));
  locations->SetOut(Location(R0));
  equal->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitEqual(HEqual* equal) {
  LocationSummary* locations = equal->GetLocations();
  __ teq(locations->InAt(0).reg<Register>(),
         ShifterOperand(locations->InAt(1).reg<Register>()));
  __ mov(locations->Out().reg<Register>(), ShifterOperand(1), EQ);
  __ mov(locations->Out().reg<Register>(), ShifterOperand(0), NE);
}

void LocationsBuilderARM::VisitLocal(HLocal* local) {
  local->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitLocal(HLocal* local) {
  DCHECK_EQ(local->GetBlock(), GetGraph()->GetEntryBlock());
  codegen_->SetFrameSize(codegen_->GetFrameSize() + kArmWordSize);
}

void LocationsBuilderARM::VisitLoadLocal(HLoadLocal* load) {
  load->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitLoadLocal(HLoadLocal* load) {
  // Nothing to do, this is driven by the code generator.
}

void LocationsBuilderARM::VisitStoreLocal(HStoreLocal* store) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(store);
  locations->SetInAt(1, Location(R0));
  store->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitStoreLocal(HStoreLocal* store) {
  LocationSummary* locations = store->GetLocations();
  __ StoreToOffset(kStoreWord, locations->InAt(1).reg<Register>(),
                   SP, codegen_->GetStackSlot(store->GetLocal()));
}

void LocationsBuilderARM::VisitIntConstant(HIntConstant* constant) {
  constant->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitIntConstant(HIntConstant* constant) {
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
  locations->SetInAt(0, Location(R0));
  ret->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitReturn(HReturn* ret) {
  DCHECK_EQ(ret->GetLocations()->InAt(0).reg<Register>(), R0);
  codegen_->GenerateFrameExit();
}

static constexpr Register kParameterCoreRegisters[] = { R1, R2, R3 };
static constexpr size_t kParameterCoreRegistersLength = arraysize(kParameterCoreRegisters);

class InvokeStaticCallingConvention : public CallingConvention<Register> {
 public:
  InvokeStaticCallingConvention()
      : CallingConvention(kParameterCoreRegisters, kParameterCoreRegistersLength) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeStaticCallingConvention);
};

void LocationsBuilderARM::VisitPushArgument(HPushArgument* argument) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(argument);
  InvokeStaticCallingConvention calling_convention;
  if (argument->GetArgumentIndex() < calling_convention.GetNumberOfRegisters()) {
    Location location = Location(calling_convention.GetRegisterAt(argument->GetArgumentIndex()));
    locations->SetInAt(0, location);
    locations->SetOut(location);
  } else {
    locations->SetInAt(0, Location(R0));
  }
  argument->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitPushArgument(HPushArgument* argument) {
  uint8_t argument_index = argument->GetArgumentIndex();
  InvokeStaticCallingConvention calling_convention;
  size_t parameter_registers = calling_convention.GetNumberOfRegisters();
  LocationSummary* locations = argument->GetLocations();
  if (argument_index >= parameter_registers) {
    uint8_t offset = calling_convention.GetStackOffsetOf(argument_index);
    __ StoreToOffset(kStoreWord, locations->InAt(0).reg<Register>(), SP, offset);
  } else {
    DCHECK_EQ(locations->Out().reg<Register>(), locations->InAt(0).reg<Register>());
  }
}

void LocationsBuilderARM::VisitInvokeStatic(HInvokeStatic* invoke) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(invoke);
  locations->AddTemp(Location(R0));
  invoke->SetLocations(locations);
}

void InstructionCodeGeneratorARM::LoadCurrentMethod(Register reg) {
  __ ldr(reg, Address(SP, kCurrentMethodStackOffset));
}

void InstructionCodeGeneratorARM::VisitInvokeStatic(HInvokeStatic* invoke) {
  Register temp = invoke->GetLocations()->GetTemp(0).reg<Register>();
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
      locations->SetInAt(0, Location(R0));
      locations->SetInAt(1, Location(R1));
      locations->SetOut(Location(R0));
      break;
    }
    default:
      LOG(FATAL) << "Unimplemented";
  }
  add->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitAdd(HAdd* add) {
  LocationSummary* locations = add->GetLocations();
  switch (add->GetResultType()) {
    case Primitive::kPrimInt:
      __ add(locations->Out().reg<Register>(),
             locations->InAt(0).reg<Register>(),
             ShifterOperand(locations->InAt(1).reg<Register>()));
      break;
    default:
      LOG(FATAL) << "Unimplemented";
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
  locations->SetOut(Location(R0));
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

}  // namespace arm
}  // namespace art
