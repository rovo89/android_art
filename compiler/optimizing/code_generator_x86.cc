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
#include "utils/assembler.h"
#include "utils/x86/assembler_x86.h"

#define __ reinterpret_cast<X86Assembler*>(assembler())->

namespace art {
namespace x86 {

void CodeGeneratorX86::GenerateFrameEntry() {
  __ pushl(EBP);
  __ movl(EBP, ESP);

  if (frame_size_ != 0) {
    __ subl(ESP, Immediate(frame_size_));
  }
}

void CodeGeneratorX86::GenerateFrameExit() {
  __ movl(ESP, EBP);
  __ popl(EBP);
}

void CodeGeneratorX86::Bind(Label* label) {
  __ Bind(label);
}

void CodeGeneratorX86::Push(HInstruction* instruction, Location location) {
  __ pushl(location.reg<Register>());
}

void CodeGeneratorX86::Move(HInstruction* instruction, Location location) {
  HIntConstant* constant = instruction->AsIntConstant();
  if (constant != nullptr) {
    __ movl(location.reg<Register>(), Immediate(constant->value()));
  } else {
    __ popl(location.reg<Register>());
  }
}

void LocationsBuilderX86::VisitGoto(HGoto* got) {
  got->set_locations(nullptr);
}

void CodeGeneratorX86::VisitGoto(HGoto* got) {
  HBasicBlock* successor = got->GetSuccessor();
  if (graph()->exit_block() == successor) {
    GenerateFrameExit();
  } else if (!GoesToNextBlock(got->block(), successor)) {
    __ jmp(GetLabelOf(successor));
  }
}

void LocationsBuilderX86::VisitExit(HExit* exit) {
  exit->set_locations(nullptr);
}

void CodeGeneratorX86::VisitExit(HExit* exit) {
  if (kIsDebugBuild) {
    __ Comment("Unreachable");
    __ int3();
  }
}

void LocationsBuilderX86::VisitIf(HIf* if_instr) {
  LocationSummary* locations = new (graph()->arena()) LocationSummary(if_instr);
  locations->SetInAt(0, Location(EAX));
  if_instr->set_locations(locations);
}

void CodeGeneratorX86::VisitIf(HIf* if_instr) {
  // TODO: Generate the input as a condition, instead of materializing in a register.
  __ cmpl(if_instr->locations()->InAt(0).reg<Register>(), Immediate(0));
  __ j(kEqual, GetLabelOf(if_instr->IfFalseSuccessor()));
  if (!GoesToNextBlock(if_instr->block(), if_instr->IfTrueSuccessor())) {
    __ jmp(GetLabelOf(if_instr->IfTrueSuccessor()));
  }
}

void LocationsBuilderX86::VisitLocal(HLocal* local) {
  local->set_locations(nullptr);
}

void CodeGeneratorX86::VisitLocal(HLocal* local) {
  DCHECK_EQ(local->block(), graph()->entry_block());
  frame_size_ += kWordSize;
}

void LocationsBuilderX86::VisitLoadLocal(HLoadLocal* local) {
  LocationSummary* locations = new (graph()->arena()) LocationSummary(local);
  locations->SetOut(Location(EAX));
  local->set_locations(locations);
}

static int32_t GetStackSlot(HLocal* local) {
  // We are currently using EBP to access locals, so the offset must be negative.
  return (local->reg_number() + 1) * -kWordSize;
}

void CodeGeneratorX86::VisitLoadLocal(HLoadLocal* load) {
  __ movl(load->locations()->Out().reg<Register>(),
          Address(EBP, GetStackSlot(load->GetLocal())));
}

void LocationsBuilderX86::VisitStoreLocal(HStoreLocal* local) {
  LocationSummary* locations = new (graph()->arena()) LocationSummary(local);
  locations->SetInAt(1, Location(EAX));
  local->set_locations(locations);
}

void CodeGeneratorX86::VisitStoreLocal(HStoreLocal* store) {
  __ movl(Address(EBP, GetStackSlot(store->GetLocal())),
          store->locations()->InAt(1).reg<Register>());
}

void LocationsBuilderX86::VisitEqual(HEqual* equal) {
  LocationSummary* locations = new (graph()->arena()) LocationSummary(equal);
  locations->SetInAt(0, Location(EAX));
  locations->SetInAt(1, Location(ECX));
  locations->SetOut(Location(EAX));
  equal->set_locations(locations);
}

void CodeGeneratorX86::VisitEqual(HEqual* equal) {
  __ cmpl(equal->locations()->InAt(0).reg<Register>(),
          equal->locations()->InAt(1).reg<Register>());
  __ setb(kEqual, equal->locations()->Out().reg<Register>());
}

void LocationsBuilderX86::VisitIntConstant(HIntConstant* constant) {
  constant->set_locations(nullptr);
}

void CodeGeneratorX86::VisitIntConstant(HIntConstant* constant) {
  // Will be generated at use site.
}

void LocationsBuilderX86::VisitReturnVoid(HReturnVoid* ret) {
  ret->set_locations(nullptr);
}

void CodeGeneratorX86::VisitReturnVoid(HReturnVoid* ret) {
  GenerateFrameExit();
  __ ret();
}

void LocationsBuilderX86::VisitReturn(HReturn* ret) {
  LocationSummary* locations = new (graph()->arena()) LocationSummary(ret);
  locations->SetInAt(0, Location(EAX));
  ret->set_locations(locations);
}

void CodeGeneratorX86::VisitReturn(HReturn* ret) {
  DCHECK_EQ(ret->locations()->InAt(0).reg<Register>(), EAX);
  GenerateFrameExit();
  __ ret();
}

}  // namespace x86
}  // namespace art
