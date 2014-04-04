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

#ifndef ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM_H_
#define ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM_H_

#include "code_generator.h"
#include "nodes.h"
#include "utils/arm/assembler_arm.h"

namespace art {
namespace arm {

static constexpr size_t kArmWordSize = 4;

class LocationsBuilderARM : public HGraphVisitor {
 public:
  explicit LocationsBuilderARM(HGraph* graph) : HGraphVisitor(graph) { }

#define DECLARE_VISIT_INSTRUCTION(name)     \
  virtual void Visit##name(H##name* instr);

  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

 private:
  DISALLOW_COPY_AND_ASSIGN(LocationsBuilderARM);
};

class CodeGeneratorARM;

class InstructionCodeGeneratorARM : public HGraphVisitor {
 public:
  InstructionCodeGeneratorARM(HGraph* graph, CodeGeneratorARM* codegen);

#define DECLARE_VISIT_INSTRUCTION(name)     \
  virtual void Visit##name(H##name* instr);

  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  ArmAssembler* GetAssembler() const { return assembler_; }
  void LoadCurrentMethod(Register reg);

 private:
  ArmAssembler* const assembler_;
  CodeGeneratorARM* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(InstructionCodeGeneratorARM);
};

class CodeGeneratorARM : public CodeGenerator {
 public:
  explicit CodeGeneratorARM(HGraph* graph)
      : CodeGenerator(graph),
        location_builder_(graph),
        instruction_visitor_(graph, this) { }
  virtual ~CodeGeneratorARM() { }

  virtual void GenerateFrameEntry() OVERRIDE;
  virtual void GenerateFrameExit() OVERRIDE;
  virtual void Bind(Label* label) OVERRIDE;
  virtual void Move(HInstruction* instruction, Location location, HInstruction* move_for) OVERRIDE;

  virtual size_t GetWordSize() const OVERRIDE {
    return kArmWordSize;
  }

  virtual HGraphVisitor* GetLocationBuilder() OVERRIDE {
    return &location_builder_;
  }

  virtual HGraphVisitor* GetInstructionVisitor() OVERRIDE {
    return &instruction_visitor_;
  }

  virtual ArmAssembler* GetAssembler() OVERRIDE {
    return &assembler_;
  }

  int32_t GetStackSlot(HLocal* local) const;

 private:
  LocationsBuilderARM location_builder_;
  InstructionCodeGeneratorARM instruction_visitor_;
  ArmAssembler assembler_;

  DISALLOW_COPY_AND_ASSIGN(CodeGeneratorARM);
};

}  // namespace arm
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM_H_
