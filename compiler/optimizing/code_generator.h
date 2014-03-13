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

#include "globals.h"
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

/**
 * A Location is an abstraction over the potential location
 * of an instruction. It could be in register or stack.
 */
class Location : public ValueObject {
 public:
  template<typename T>
  T reg() const { return static_cast<T>(reg_); }

  Location() : reg_(kInvalid) { }
  explicit Location(uword reg) : reg_(reg) { }

  static Location RegisterLocation(uword reg) {
    return Location(reg);
  }

  bool IsValid() const { return reg_ != kInvalid; }

  Location(const Location& other) : reg_(other.reg_) { }

  Location& operator=(const Location& other) {
    reg_ = other.reg_;
    return *this;
  }

 private:
  // The target register for that location.
  // TODO: Support stack location.
  uword reg_;
  static const uword kInvalid = -1;
};

/**
 * The code generator computes LocationSummary for each instruction so that
 * the instruction itself knows what code to generate: where to find the inputs
 * and where to place the result.
 *
 * The intent is to have the code for generating the instruction independent of
 * register allocation. A register allocator just has to provide a LocationSummary.
 */
class LocationSummary : public ArenaObject {
 public:
  explicit LocationSummary(HInstruction* instruction)
      : inputs(instruction->block()->graph()->arena(), instruction->InputCount()) {
    inputs.SetSize(instruction->InputCount());
    for (int i = 0; i < instruction->InputCount(); i++) {
      inputs.Put(i, Location());
    }
  }

  void SetInAt(uint32_t at, Location location) {
    inputs.Put(at, location);
  }

  Location InAt(uint32_t at) const {
    return inputs.Get(at);
  }

  void SetOut(Location location) {
    output = Location(location);
  }

  Location Out() const { return output; }

 private:
  GrowableArray<Location> inputs;
  Location output;

  DISALLOW_COPY_AND_ASSIGN(LocationSummary);
};

class CodeGenerator : public HGraphVisitor {
 public:
  // Compiles the graph to executable instructions. Returns whether the compilation
  // succeeded.
  static bool CompileGraph(HGraph* graph, InstructionSet instruction_set, CodeAllocator* allocator);

  Assembler* assembler() const { return assembler_; }

  // Visit functions for instruction classes.
#define DECLARE_VISIT_INSTRUCTION(name)     \
  virtual void Visit##name(H##name* instr) = 0;

  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

 protected:
  CodeGenerator(Assembler* assembler, HGraph* graph)
      : HGraphVisitor(graph),
        frame_size_(0),
        assembler_(assembler),
        block_labels_(graph->arena(), 0) {
    block_labels_.SetSize(graph->blocks()->Size());
  }

  Label* GetLabelOf(HBasicBlock* block) const;
  bool GoesToNextBlock(HBasicBlock* current, HBasicBlock* next) const;

  // Frame size required for this method.
  uint32_t frame_size_;

  virtual void GenerateFrameEntry() = 0;
  virtual void GenerateFrameExit() = 0;
  virtual void Bind(Label* label) = 0;
  virtual void Move(HInstruction* instruction, Location location) = 0;
  virtual void Push(HInstruction* instruction, Location location) = 0;
  virtual HGraphVisitor* GetLocationBuilder() = 0;

 private:
  void InitLocations(HInstruction* instruction);
  void Compile(CodeAllocator* allocator);
  void CompileBlock(HBasicBlock* block);
  void CompileEntryBlock();

  Assembler* const assembler_;

  // Labels for each block that will be compiled.
  GrowableArray<Label> block_labels_;

  DISALLOW_COPY_AND_ASSIGN(CodeGenerator);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_H_
