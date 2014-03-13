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

namespace art {

class Assembler;
class Label;

namespace arm {

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

class CodeGeneratorARM : public CodeGenerator {
 public:
  CodeGeneratorARM(Assembler* assembler, HGraph* graph)
      : CodeGenerator(assembler, graph), location_builder_(graph) { }

  // Visit functions for instruction classes.
#define DECLARE_VISIT_INSTRUCTION(name)     \
  virtual void Visit##name(H##name* instr);

  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

 protected:
  virtual void GenerateFrameEntry() OVERRIDE;
  virtual void GenerateFrameExit() OVERRIDE;
  virtual void Bind(Label* label) OVERRIDE;
  virtual void Move(HInstruction* instruction, Location location) OVERRIDE;
  virtual void Push(HInstruction* instruction, Location location) OVERRIDE;

  virtual HGraphVisitor* GetLocationBuilder() OVERRIDE {
    return &location_builder_;
  }

 private:
  LocationsBuilderARM location_builder_;

  DISALLOW_COPY_AND_ASSIGN(CodeGeneratorARM);
};

}  // namespace arm
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM_H_
