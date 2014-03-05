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

#define __ reinterpret_cast<ArmAssembler*>(assembler())->

namespace art {
namespace arm {

void CodeGeneratorARM::GenerateFrameEntry() {
  RegList registers = (1 << LR) | (1 << FP);
  __ PushList(registers);
}

void CodeGeneratorARM::GenerateFrameExit() {
  RegList registers = (1 << PC) | (1 << FP);
  __ PopList(registers);
}

void CodeGeneratorARM::Bind(Label* label) {
  __ Bind(label);
}

void CodeGeneratorARM::VisitGoto(HGoto* got) {
  HBasicBlock* successor = got->GetSuccessor();
  if (graph()->exit_block() == successor) {
    GenerateFrameExit();
  } else if (!GoesToNextBlock(got)) {
    __ b(GetLabelOf(successor));
  }
}

void CodeGeneratorARM::VisitExit(HExit* exit) {
  if (kIsDebugBuild) {
    __ Comment("Unreachable");
    __ bkpt(0);
  }
}

void CodeGeneratorARM::VisitIf(HIf* if_instr) {
  LOG(FATAL) << "UNIMPLEMENTED";
}

void CodeGeneratorARM::VisitReturnVoid(HReturnVoid* ret) {
  GenerateFrameExit();
}

}  // namespace arm
}  // namespace art
