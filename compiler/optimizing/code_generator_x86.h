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

#ifndef ART_COMPILER_OPTIMIZING_CODE_GENERATOR_X86_H_
#define ART_COMPILER_OPTIMIZING_CODE_GENERATOR_X86_H_

#include "code_generator.h"
#include "nodes.h"

namespace art {

class Assembler;
class Label;

namespace x86 {

class CodeGeneratorX86 : public CodeGenerator {
 public:
  CodeGeneratorX86(Assembler* assembler, HGraph* graph)
      : CodeGenerator(assembler, graph) { }

  // Visit functions for instruction classes.
#define DECLARE_VISIT_INSTRUCTION(name)     \
  virtual void Visit##name(H##name* instr);

  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

 private:
  virtual void GenerateFrameEntry();
  virtual void GenerateFrameExit();
  virtual void Bind(Label* label);

  DISALLOW_COPY_AND_ASSIGN(CodeGeneratorX86);
};

}  // namespace x86
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_X86_H_
