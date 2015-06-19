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

#include "base/arena_object.h"
#include "dex_file.h"
#include "dex_file-inl.h"
#include "driver/compiler_driver.h"
#include "driver/dex_compilation_unit.h"
#include "optimizing_compiler_stats.h"
#include "primitive.h"
#include "utils/growable_array.h"
#include "nodes.h"

namespace art {

class Instruction;
class SwitchTable;

class HGraphBuilder : public ValueObject {
 public:
  HGraphBuilder(HGraph* graph,
                DexCompilationUnit* dex_compilation_unit,
                const DexCompilationUnit* const outer_compilation_unit,
                const DexFile* dex_file,
                CompilerDriver* driver,
                OptimizingCompilerStats* compiler_stats)
      : arena_(graph->GetArena()),
        branch_targets_(graph->GetArena(), 0),
        locals_(graph->GetArena(), 0),
        entry_block_(nullptr),
        exit_block_(nullptr),
        current_block_(nullptr),
        graph_(graph),
        dex_file_(dex_file),
        dex_compilation_unit_(dex_compilation_unit),
        compiler_driver_(driver),
        outer_compilation_unit_(outer_compilation_unit),
        return_type_(Primitive::GetType(dex_compilation_unit_->GetShorty()[0])),
        code_start_(nullptr),
        latest_result_(nullptr),
        compilation_stats_(compiler_stats) {}

  // Only for unit testing.
  HGraphBuilder(HGraph* graph, Primitive::Type return_type = Primitive::kPrimInt)
      : arena_(graph->GetArena()),
        branch_targets_(graph->GetArena(), 0),
        locals_(graph->GetArena(), 0),
        entry_block_(nullptr),
        exit_block_(nullptr),
        current_block_(nullptr),
        graph_(graph),
        dex_file_(nullptr),
        dex_compilation_unit_(nullptr),
        compiler_driver_(nullptr),
        outer_compilation_unit_(nullptr),
        return_type_(return_type),
        code_start_(nullptr),
        latest_result_(nullptr),
        compilation_stats_(nullptr) {}

  bool BuildGraph(const DexFile::CodeItem& code);

  static constexpr const char* kBuilderPassName = "builder";

 private:
  // Analyzes the dex instruction and adds HInstruction to the graph
  // to execute that instruction. Returns whether the instruction can
  // be handled.
  bool AnalyzeDexInstruction(const Instruction& instruction, uint32_t dex_pc);

  // Finds all instructions that start a new block, and populates branch_targets_ with
  // the newly created blocks.
  // As a side effect, also compute the number of dex instructions, blocks, and
  // branches.
  // Returns true if all the branches fall inside the method code, false otherwise.
  // (In normal cases this should always return true but someone can artificially
  // create a code unit in which branches fall-through out of it).
  bool ComputeBranchTargets(const uint16_t* start,
                            const uint16_t* end,
                            size_t* number_of_branches);
  void MaybeUpdateCurrentBlock(size_t index);
  HBasicBlock* FindBlockStartingAt(int32_t index) const;

  void InitializeLocals(uint16_t count);
  HLocal* GetLocalAt(int register_index) const;
  void UpdateLocal(int register_index, HInstruction* instruction) const;
  HInstruction* LoadLocal(int register_index, Primitive::Type type) const;
  void PotentiallyAddSuspendCheck(HBasicBlock* target, uint32_t dex_pc);
  void InitializeParameters(uint16_t number_of_parameters);
  bool NeedsAccessCheck(uint32_t type_index) const;

  template<typename T>
  void Unop_12x(const Instruction& instruction, Primitive::Type type);

  template<typename T>
  void Binop_23x(const Instruction& instruction, Primitive::Type type);

  template<typename T>
  void Binop_23x(const Instruction& instruction, Primitive::Type type, uint32_t dex_pc);

  template<typename T>
  void Binop_23x_shift(const Instruction& instruction, Primitive::Type type);

  void Binop_23x_cmp(const Instruction& instruction,
                     Primitive::Type type,
                     HCompare::Bias bias,
                     uint32_t dex_pc);

  template<typename T>
  void Binop_12x(const Instruction& instruction, Primitive::Type type);

  template<typename T>
  void Binop_12x(const Instruction& instruction, Primitive::Type type, uint32_t dex_pc);

  template<typename T>
  void Binop_12x_shift(const Instruction& instruction, Primitive::Type type);

  template<typename T>
  void Binop_22b(const Instruction& instruction, bool reverse);

  template<typename T>
  void Binop_22s(const Instruction& instruction, bool reverse);

  template<typename T> void If_21t(const Instruction& instruction, uint32_t dex_pc);
  template<typename T> void If_22t(const Instruction& instruction, uint32_t dex_pc);

  void Conversion_12x(const Instruction& instruction,
                      Primitive::Type input_type,
                      Primitive::Type result_type,
                      uint32_t dex_pc);

  void BuildCheckedDivRem(uint16_t out_reg,
                          uint16_t first_reg,
                          int64_t second_reg_or_constant,
                          uint32_t dex_pc,
                          Primitive::Type type,
                          bool second_is_lit,
                          bool is_div);

  void BuildReturn(const Instruction& instruction, Primitive::Type type);

  // Builds an instance field access node and returns whether the instruction is supported.
  bool BuildInstanceFieldAccess(const Instruction& instruction, uint32_t dex_pc, bool is_put);

  // Builds a static field access node and returns whether the instruction is supported.
  bool BuildStaticFieldAccess(const Instruction& instruction, uint32_t dex_pc, bool is_put);

  void BuildArrayAccess(const Instruction& instruction,
                        uint32_t dex_pc,
                        bool is_get,
                        Primitive::Type anticipated_type);

  // Builds an invocation node and returns whether the instruction is supported.
  bool BuildInvoke(const Instruction& instruction,
                   uint32_t dex_pc,
                   uint32_t method_idx,
                   uint32_t number_of_vreg_arguments,
                   bool is_range,
                   uint32_t* args,
                   uint32_t register_index);

  // Builds a new array node and the instructions that fill it.
  void BuildFilledNewArray(uint32_t dex_pc,
                           uint32_t type_index,
                           uint32_t number_of_vreg_arguments,
                           bool is_range,
                           uint32_t* args,
                           uint32_t register_index);

  void BuildFillArrayData(const Instruction& instruction, uint32_t dex_pc);

  // Fills the given object with data as specified in the fill-array-data
  // instruction. Currently only used for non-reference and non-floating point
  // arrays.
  template <typename T>
  void BuildFillArrayData(HInstruction* object,
                          const T* data,
                          uint32_t element_count,
                          Primitive::Type anticipated_type,
                          uint32_t dex_pc);

  // Fills the given object with data as specified in the fill-array-data
  // instruction. The data must be for long and double arrays.
  void BuildFillWideArrayData(HInstruction* object,
                              const int64_t* data,
                              uint32_t element_count,
                              uint32_t dex_pc);

  // Builds a `HInstanceOf`, or a `HCheckCast` instruction.
  // Returns whether we succeeded in building the instruction.
  bool BuildTypeCheck(const Instruction& instruction,
                      uint8_t destination,
                      uint8_t reference,
                      uint16_t type_index,
                      uint32_t dex_pc);

  // Builds an instruction sequence for a packed switch statement.
  void BuildPackedSwitch(const Instruction& instruction, uint32_t dex_pc);

  // Builds an instruction sequence for a sparse switch statement.
  void BuildSparseSwitch(const Instruction& instruction, uint32_t dex_pc);

  void BuildSwitchCaseHelper(const Instruction& instruction, size_t index,
                             bool is_last_case, const SwitchTable& table,
                             HInstruction* value, int32_t case_value_int,
                             int32_t target_offset, uint32_t dex_pc);

  bool SkipCompilation(const DexFile::CodeItem& code_item, size_t number_of_branches);

  void MaybeRecordStat(MethodCompilationStat compilation_stat);

  // Returns the outer-most compiling method's class.
  mirror::Class* GetOutermostCompilingClass() const;

  // Returns the class whose method is being compiled.
  mirror::Class* GetCompilingClass() const;

  // Returns whether `type_index` points to the outer-most compiling method's class.
  bool IsOutermostCompilingClass(uint16_t type_index) const;

  ArenaAllocator* const arena_;

  // A list of the size of the dex code holding block information for
  // the method. If an entry contains a block, then the dex instruction
  // starting at that entry is the first instruction of a new block.
  GrowableArray<HBasicBlock*> branch_targets_;

  GrowableArray<HLocal*> locals_;

  HBasicBlock* entry_block_;
  HBasicBlock* exit_block_;
  HBasicBlock* current_block_;
  HGraph* const graph_;

  // The dex file where the method being compiled is.
  const DexFile* const dex_file_;

  // The compilation unit of the current method being compiled. Note that
  // it can be an inlined method.
  DexCompilationUnit* const dex_compilation_unit_;

  CompilerDriver* const compiler_driver_;

  // The compilation unit of the outermost method being compiled. That is the
  // method being compiled (and not inlined), and potentially inlining other
  // methods.
  const DexCompilationUnit* const outer_compilation_unit_;

  // The return type of the method being compiled.
  const Primitive::Type return_type_;

  // The pointer in the dex file where the instructions of the code item
  // being currently compiled start.
  const uint16_t* code_start_;

  // The last invoke or fill-new-array being built. Only to be
  // used by move-result instructions.
  HInstruction* latest_result_;

  OptimizingCompilerStats* compilation_stats_;

  DISALLOW_COPY_AND_ASSIGN(HGraphBuilder);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_BUILDER_H_
