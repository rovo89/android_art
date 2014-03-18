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

#define __ reinterpret_cast<ArmAssembler*>(GetAssembler())->

namespace art {
namespace arm {

void CodeGeneratorARM::GenerateFrameEntry() {
  __ PushList((1 << FP) | (1 << LR));
  __ mov(FP, ShifterOperand(SP));
  if (GetFrameSize() != 0) {
    __ AddConstant(SP, -GetFrameSize());
  }
}

void CodeGeneratorARM::GenerateFrameExit() {
  __ mov(SP, ShifterOperand(FP));
  __ PopList((1 << FP) | (1 << PC));
}

void CodeGeneratorARM::Bind(Label* label) {
  __ Bind(label);
}

void CodeGeneratorARM::Push(HInstruction* instruction, Location location) {
  __ Push(location.reg<Register>());
}

void CodeGeneratorARM::Move(HInstruction* instruction, Location location) {
  HIntConstant* constant = instruction->AsIntConstant();
  if (constant != nullptr) {
    __ LoadImmediate(location.reg<Register>(), constant->GetValue());
  } else {
    __ Pop(location.reg<Register>());
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
  codegen_->SetFrameSize(codegen_->GetFrameSize() + kWordSize);
}

void LocationsBuilderARM::VisitLoadLocal(HLoadLocal* load) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(load);
  locations->SetOut(Location(R0));
  load->SetLocations(locations);
}

static int32_t GetStackSlot(HLocal* local) {
  // We are currently using FP to access locals, so the offset must be negative.
  return (local->GetRegNumber() + 1) * -kWordSize;
}

void InstructionCodeGeneratorARM::VisitLoadLocal(HLoadLocal* load) {
  LocationSummary* locations = load->GetLocations();
  __ LoadFromOffset(kLoadWord, locations->Out().reg<Register>(),
                    FP, GetStackSlot(load->GetLocal()));
}

void LocationsBuilderARM::VisitStoreLocal(HStoreLocal* store) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(store);
  locations->SetInAt(1, Location(R0));
  store->SetLocations(locations);
}

void InstructionCodeGeneratorARM::VisitStoreLocal(HStoreLocal* store) {
  LocationSummary* locations = store->GetLocations();
  __ StoreToOffset(kStoreWord, locations->InAt(1).reg<Register>(),
                   FP, GetStackSlot(store->GetLocal()));
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

}  // namespace arm
}  // namespace art
