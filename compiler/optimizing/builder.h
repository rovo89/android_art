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

#include "base/arena_containers.h"
#include "base/arena_object.h"
#include "dex_file.h"
#include "dex_file-inl.h"
#include "driver/compiler_driver.h"
#include "driver/dex_compilation_unit.h"
#include "optimizing_compiler_stats.h"
#include "primitive.h"
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
                OptimizingCompilerStats* compiler_stats,
                const uint8_t* interpreter_metadata,
                Handle<mirror::DexCache> dex_cache)
      : arena_(graph->GetArena()),
        branch_targets_(graph->GetArena()->Adapter(kArenaAllocGraphBuilder)),
        locals_(graph->GetArena()->Adapter(kArenaAllocGraphBuilder)),
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
        can_use_baseline_for_string_init_(true),
        compilation_stats_(compiler_stats),
        interpreter_metadata_(interpreter_metadata),
        dex_cache_(dex_cache) {}

  // Only for unit testing.
  HGraphBuilder(HGraph* graph, Primitive::Type return_type = Primitive::kPrimInt)
      : arena_(graph->GetArena()),
        branch_targets_(graph->GetArena()->Adapter(kArenaAllocGraphBuilder)),
        locals_(graph->GetArena()->Adapter(kArenaAllocGraphBuilder)),
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
        can_use_baseline_for_string_init_(true),
        compilation_stats_(nullptr),
        interpreter_metadata_(nullptr),
        dex_cache_(NullHandle<mirror::DexCache>()) {}

  bool BuildGraph(const DexFile::CodeItem& code);

  bool CanUseBaselineForStringInit() const {
    return can_use_baseline_for_string_init_;
  }

  static constexpr const char* kBuilderPassName = "builder";

  // The number of entries in a packed switch before we use a jump table.
  static constexpr uint16_t kSmallSwitchThreshold = 5;

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
  void MaybeUpdateCurrentBlock(size_t dex_pc);
  HBasicBlock* FindBlockStartingAt(int32_t dex_pc) const;
  HBasicBlock* FindOrCreateBlockStartingAt(int32_t dex_pc);

  // Adds new blocks to `branch_targets_` starting at the limits of TryItems and
  // their exception handlers.
  void CreateBlocksForTryCatch(const DexFile::CodeItem& code_item);

  // Splits edges which cross the boundaries of TryItems, inserts TryBoundary
  // instructions and links them to the corresponding catch blocks.
  void InsertTryBoundaryBlocks(const DexFile::CodeItem& code_item);

  // Iterates over the exception handlers of `try_item`, finds the corresponding
  // catch blocks and makes them successors of `try_boundary`. The order of
  // successors matches the order in which runtime exception delivery searches
  // for a handler.
  void LinkToCatchBlocks(HTryBoundary* try_boundary,
                         const DexFile::CodeItem& code_item,
                         const DexFile::TryItem* try_item);

  bool CanDecodeQuickenedInfo() const;
  uint16_t LookupQuickenedInfo(uint32_t dex_pc);

  void InitializeLocals(uint16_t count);
  HLocal* GetLocalAt(uint32_t register_index) const;
  void UpdateLocal(uint32_t register_index, HInstruction* instruction, uint32_t dex_pc) const;
  HInstruction* LoadLocal(uint32_t register_index, Primitive::Type type, uint32_t dex_pc) const;
  void PotentiallyAddSuspendCheck(HBasicBlock* target, uint32_t dex_pc);
  void InitializeParameters(uint16_t number_of_parameters);

  // Returns whether the current method needs access check for the type.
  // Output parameter finalizable is set to whether the type is finalizable.
  bool NeedsAccessCheck(uint32_t type_index, /*out*/bool* finalizable) const;

  template<typename T>
  void Unop_12x(const Instruction& instruction, Primitive::Type type, uint32_t dex_pc);

  template<typename T>
  void Binop_23x(const Instruction& instruction, Primitive::Type type, uint32_t dex_pc);

  template<typename T>
  void Binop_23x_shift(const Instruction& instruction, Primitive::Type type, uint32_t dex_pc);

  void Binop_23x_cmp(const Instruction& instruction,
                     Primitive::Type type,
                     ComparisonBias bias,
                     uint32_t dex_pc);

  template<typename T>
  void Binop_12x(const Instruction& instruction, Primitive::Type type, uint32_t dex_pc);

  template<typename T>
  void Binop_12x_shift(const Instruction& instruction, Primitive::Type type, uint32_t dex_pc);

  template<typename T>
  void Binop_22b(const Instruction& instruction, bool reverse, uint32_t dex_pc);

  template<typename T>
  void Binop_22s(const Instruction& instruction, bool reverse, uint32_t dex_pc);

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

  void BuildReturn(const Instruction& instruction, Primitive::Type type, uint32_t dex_pc);

  // Builds an instance field access node and returns whether the instruction is supported.
  bool BuildInstanceFieldAccess(const Instruction& instruction, uint32_t dex_pc, bool is_put);

  void BuildUnresolvedStaticFieldAccess(const Instruction& instruction,
                                        uint32_t dex_pc,
                                        bool is_put,
                                        Primitive::Type field_type);
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
  void BuildTypeCheck(const Instruction& instruction,
                      uint8_t destination,
                      uint8_t reference,
                      uint16_t type_index,
                      uint32_t dex_pc);

  // Builds an instruction sequence for a packed switch statement.
  void BuildPackedSwitch(const Instruction& instruction, uint32_t dex_pc);

  // Build a switch instruction from a packed switch statement.
  void BuildSwitchJumpTable(const SwitchTable& table,
                            const Instruction& instruction,
                            HInstruction* value,
                            uint32_t dex_pc);

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

  void PotentiallySimplifyFakeString(uint16_t original_dex_register,
                                     uint32_t dex_pc,
                                     HInvoke* invoke);

  bool SetupInvokeArguments(HInvoke* invoke,
                            uint32_t number_of_vreg_arguments,
                            uint32_t* args,
                            uint32_t register_index,
                            bool is_range,
                            const char* descriptor,
                            size_t start_index,
                            size_t* argument_index);

  bool HandleInvoke(HInvoke* invoke,
                    uint32_t number_of_vreg_arguments,
                    uint32_t* args,
                    uint32_t register_index,
                    bool is_range,
                    const char* descriptor,
                    HClinitCheck* clinit_check);

  bool HandleStringInit(HInvoke* invoke,
                        uint32_t number_of_vreg_arguments,
                        uint32_t* args,
                        uint32_t register_index,
                        bool is_range,
                        const char* descriptor);

  HClinitCheck* ProcessClinitCheckForInvoke(
      uint32_t dex_pc,
      uint32_t method_idx,
      HInvokeStaticOrDirect::ClinitCheckRequirement* clinit_check_requirement);

  // Build a HNewInstance instruction.
  bool BuildNewInstance(uint16_t type_index, uint32_t dex_pc);

  // Return whether the compiler can assume `cls` is initialized. `type_index` is the index
  // of the class in the outer dex file.
  bool IsInitialized(Handle<mirror::Class> cls, uint16_t type_index) const
      SHARED_REQUIRES(Locks::mutator_lock_);

  ArenaAllocator* const arena_;

  // A list of the size of the dex code holding block information for
  // the method. If an entry contains a block, then the dex instruction
  // starting at that entry is the first instruction of a new block.
  ArenaVector<HBasicBlock*> branch_targets_;

  ArenaVector<HLocal*> locals_;

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

  // We need to know whether we have built a graph that has calls to StringFactory
  // and hasn't gone through the verifier. If the following flag is `false`, then
  // we cannot compile with baseline.
  bool can_use_baseline_for_string_init_;

  OptimizingCompilerStats* compilation_stats_;

  const uint8_t* interpreter_metadata_;

  // Dex cache for dex_file_.
  Handle<mirror::DexCache> dex_cache_;

  DISALLOW_COPY_AND_ASSIGN(HGraphBuilder);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_BUILDER_H_
