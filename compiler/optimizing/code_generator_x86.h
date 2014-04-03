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
#include "utils/x86/assembler_x86.h"

namespace art {

namespace x86 {

class LocationsBuilderX86 : public HGraphVisitor {
 public:
  explicit LocationsBuilderX86(HGraph* graph) : HGraphVisitor(graph) { }

#define DECLARE_VISIT_INSTRUCTION(name)     \
  virtual void Visit##name(H##name* instr);

  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

 private:
  DISALLOW_COPY_AND_ASSIGN(LocationsBuilderX86);
};

class CodeGeneratorX86;

class InstructionCodeGeneratorX86 : public HGraphVisitor {
 public:
  InstructionCodeGeneratorX86(HGraph* graph, CodeGeneratorX86* codegen);

#define DECLARE_VISIT_INSTRUCTION(name)     \
  virtual void Visit##name(H##name* instr);

  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  void LoadCurrentMethod(Register reg);

  X86Assembler* GetAssembler() const { return assembler_; }

 private:
  X86Assembler* const assembler_;
  CodeGeneratorX86* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(InstructionCodeGeneratorX86);
};

class CodeGeneratorX86 : public CodeGenerator {
 public:
  explicit CodeGeneratorX86(HGraph* graph)
      : CodeGenerator(graph),
        location_builder_(graph),
        instruction_visitor_(graph, this) { }
  virtual ~CodeGeneratorX86() { }

  virtual void GenerateFrameEntry() OVERRIDE;
  virtual void GenerateFrameExit() OVERRIDE;
  virtual void Bind(Label* label) OVERRIDE;
  virtual void Move(HInstruction* instruction, Location location, HInstruction* move_for) OVERRIDE;

  virtual HGraphVisitor* GetLocationBuilder() OVERRIDE {
    return &location_builder_;
  }

  virtual HGraphVisitor* GetInstructionVisitor() OVERRIDE {
    return &instruction_visitor_;
  }

  virtual X86Assembler* GetAssembler() OVERRIDE {
    return &assembler_;
  }

  int32_t GetStackSlot(HLocal* local) const;

 private:
  LocationsBuilderX86 location_builder_;
  InstructionCodeGeneratorX86 instruction_visitor_;
  X86Assembler assembler_;

  DISALLOW_COPY_AND_ASSIGN(CodeGeneratorX86);
};

}  // namespace x86
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_X86_H_
