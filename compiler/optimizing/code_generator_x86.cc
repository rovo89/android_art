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
}

void CodeGeneratorX86::GenerateFrameExit() {
  __ movl(ESP, EBP);
  __ popl(EBP);
}

void CodeGeneratorX86::Bind(Label* label) {
  __ Bind(label);
}

void CodeGeneratorX86::VisitGoto(HGoto* got) {
  HBasicBlock* successor = got->GetSuccessor();
  if (graph()->exit_block() == successor) {
    GenerateFrameExit();
  } else if (!GoesToNextBlock(got)) {
    __ jmp(GetLabelOf(successor));
  }
}

void CodeGeneratorX86::VisitExit(HExit* exit) {
  if (kIsDebugBuild) {
    __ Comment("Unreachable");
    __ int3();
  }
}

void CodeGeneratorX86::VisitIf(HIf* if_instr) {
  LOG(FATAL) << "UNIMPLEMENTED";
}

void CodeGeneratorX86::VisitLocal(HLocal* local) {
  LOG(FATAL) << "UNIMPLEMENTED";
}

void CodeGeneratorX86::VisitLoadLocal(HLoadLocal* local) {
  LOG(FATAL) << "UNIMPLEMENTED";
}

void CodeGeneratorX86::VisitStoreLocal(HStoreLocal* local) {
  LOG(FATAL) << "UNIMPLEMENTED";
}

void CodeGeneratorX86::VisitEqual(HEqual* equal) {
  LOG(FATAL) << "UNIMPLEMENTED";
}

void CodeGeneratorX86::VisitIntConstant(HIntConstant* constant) {
  LOG(FATAL) << "UNIMPLEMENTED";
}

void CodeGeneratorX86::VisitReturnVoid(HReturnVoid* ret) {
  GenerateFrameExit();
  __ ret();
}

}  // namespace x86
}  // namespace art
