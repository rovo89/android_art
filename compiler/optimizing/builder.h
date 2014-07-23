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

#ifndef ART_COMPILER_OPTIMIZING_BUILDER_H_
#define ART_COMPILER_OPTIMIZING_BUILDER_H_

#include "dex_file.h"
#include "driver/compiler_driver.h"
#include "driver/dex_compilation_unit.h"
#include "primitive.h"
#include "utils/allocation.h"
#include "utils/growable_array.h"
#include "nodes.h"

namespace art {

class Instruction;

class HGraphBuilder : public ValueObject {
 public:
  HGraphBuilder(ArenaAllocator* arena,
                DexCompilationUnit* dex_compilation_unit = nullptr,
                const DexFile* dex_file = nullptr,
                CompilerDriver* driver = nullptr)
      : arena_(arena),
        branch_targets_(arena, 0),
        locals_(arena, 0),
        entry_block_(nullptr),
        exit_block_(nullptr),
        current_block_(nullptr),
        graph_(nullptr),
        constant0_(nullptr),
        constant1_(nullptr),
        dex_file_(dex_file),
        dex_compilation_unit_(dex_compilation_unit),
        compiler_driver_(driver) {}

  HGraph* BuildGraph(const DexFile::CodeItem& code);

 private:
  // Analyzes the dex instruction and adds HInstruction to the graph
  // to execute that instruction. Returns whether the instruction can
  // be handled.
  bool AnalyzeDexInstruction(const Instruction& instruction, int32_t dex_offset);

  // Finds all instructions that start a new block, and populates branch_targets_ with
  // the newly created blocks.
  void ComputeBranchTargets(const uint16_t* start, const uint16_t* end);
  void MaybeUpdateCurrentBlock(size_t index);
  HBasicBlock* FindBlockStartingAt(int32_t index) const;

  HIntConstant* GetIntConstant0();
  HIntConstant* GetIntConstant1();
  HIntConstant* GetIntConstant(int32_t constant);
  HLongConstant* GetLongConstant(int64_t constant);
  void InitializeLocals(uint16_t count);
  HLocal* GetLocalAt(int register_index) const;
  void UpdateLocal(int register_index, HInstruction* instruction) const;
  HInstruction* LoadLocal(int register_index, Primitive::Type type) const;

  // Temporarily returns whether the compiler supports the parameters
  // of the method.
  bool InitializeParameters(uint16_t number_of_parameters);

  template<typename T>
  void Binop_23x(const Instruction& instruction, Primitive::Type type);

  template<typename T>
  void Binop_12x(const Instruction& instruction, Primitive::Type type);

  template<typename T>
  void Binop_22b(const Instruction& instruction, bool reverse);

  template<typename T>
  void Binop_22s(const Instruction& instruction, bool reverse);

  template<typename T> void If_21t(const Instruction& instruction, uint32_t dex_offset);
  template<typename T> void If_22t(const Instruction& instruction, uint32_t dex_offset);

  void BuildReturn(const Instruction& instruction, Primitive::Type type);

  bool BuildFieldAccess(const Instruction& instruction, uint32_t dex_offset, bool is_get);
  void BuildArrayAccess(const Instruction& instruction,
                        uint32_t dex_offset,
                        bool is_get,
                        Primitive::Type anticipated_type);

  // Builds an invocation node and returns whether the instruction is supported.
  bool BuildInvoke(const Instruction& instruction,
                   uint32_t dex_offset,
                   uint32_t method_idx,
                   uint32_t number_of_vreg_arguments,
                   bool is_range,
                   uint32_t* args,
                   uint32_t register_index);

  ArenaAllocator* const arena_;

  // A list of the size of the dex code holding block information for
  // the method. If an entry contains a block, then the dex instruction
  // starting at that entry is the first instruction of a new block.
  GrowableArray<HBasicBlock*> branch_targets_;

  GrowableArray<HLocal*> locals_;

  HBasicBlock* entry_block_;
  HBasicBlock* exit_block_;
  HBasicBlock* current_block_;
  HGraph* graph_;

  HIntConstant* constant0_;
  HIntConstant* constant1_;

  const DexFile* const dex_file_;
  DexCompilationUnit* const dex_compilation_unit_;
  CompilerDriver* const compiler_driver_;

  DISALLOW_COPY_AND_ASSIGN(HGraphBuilder);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_BUILDER_H_
