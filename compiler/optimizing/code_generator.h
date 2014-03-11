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

#ifndef ART_COMPILER_OPTIMIZING_CODE_GENERATOR_H_
#define ART_COMPILER_OPTIMIZING_CODE_GENERATOR_H_

#include "instruction_set.h"
#include "memory_region.h"
#include "nodes.h"
#include "utils/assembler.h"

namespace art {

class CodeAllocator {
 public:
  CodeAllocator() { }
  virtual ~CodeAllocator() { }

  virtual uint8_t* Allocate(size_t size) = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(CodeAllocator);
};

class CodeGenerator : public HGraphVisitor {
 public:
  // Compiles the graph to executable instructions. Returns whether the compilation
  // succeeded.
  static bool CompileGraph(
      HGraph* graph, InstructionSet instruction_set, CodeAllocator* allocator);

  Assembler* assembler() const { return assembler_; }

  // Visit functions for instruction classes.
#define DECLARE_VISIT_INSTRUCTION(name)     \
  virtual void Visit##name(H##name* instr) = 0;

  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

 protected:
  CodeGenerator(Assembler* assembler, HGraph* graph)
      : HGraphVisitor(graph), assembler_(assembler), block_labels_(graph->arena(), 0) {
    block_labels_.SetSize(graph->blocks()->Size());
  }

  Label* GetLabelOf(HBasicBlock* block) const;
  bool GoesToNextBlock(HGoto* got) const;

 private:
  virtual void GenerateFrameEntry() = 0;
  virtual void GenerateFrameExit() = 0;
  virtual void Bind(Label* label) = 0;

  void Compile(CodeAllocator* allocator);
  void CompileBlock(HBasicBlock* block);

  Assembler* const assembler_;

  // Labels for each block that will be compiled.
  GrowableArray<Label> block_labels_;

  DISALLOW_COPY_AND_ASSIGN(CodeGenerator);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_H_
