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
#include "utils/allocation.h"
#include "utils/growable_array.h"

namespace art {

class ArenaAllocator;
class Instruction;
class HBasicBlock;
class HGraph;
class HIntConstant;
class HInstruction;
class HLocal;

class HGraphBuilder : public ValueObject {
 public:
  explicit HGraphBuilder(ArenaAllocator* arena)
      : arena_(arena),
        branch_targets_(arena, 0),
        locals_(arena, 0),
        entry_block_(nullptr),
        exit_block_(nullptr),
        current_block_(nullptr),
        graph_(nullptr),
        constant0_(nullptr),
        constant1_(nullptr) { }

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

  HIntConstant* GetConstant0();
  HIntConstant* GetConstant1();
  HIntConstant* GetConstant(int constant);
  void InitializeLocals(int count);
  HLocal* GetLocalAt(int register_index) const;
  void UpdateLocal(int register_index, HInstruction* instruction) const;
  HInstruction* LoadLocal(int register_index) const;

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

  DISALLOW_COPY_AND_ASSIGN(HGraphBuilder);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_BUILDER_H_
