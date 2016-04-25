/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef ART_COMPILER_OPTIMIZING_INSTRUCTION_BUILDER_H_
#define ART_COMPILER_OPTIMIZING_INSTRUCTION_BUILDER_H_

#include "base/arena_containers.h"
#include "base/arena_object.h"
#include "block_builder.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_driver-inl.h"
#include "driver/dex_compilation_unit.h"
#include "mirror/dex_cache.h"
#include "nodes.h"
#include "optimizing_compiler_stats.h"
#include "ssa_builder.h"

namespace art {

class HInstructionBuilder : public ValueObject {
 public:
  HInstructionBuilder(HGraph* graph,
                      HBasicBlockBuilder* block_builder,
                      SsaBuilder* ssa_builder,
                      const DexFile* dex_file,
                      const DexFile::CodeItem& code_item,
                      Primitive::Type return_type,
                      DexCompilationUnit* dex_compilation_unit,
                      const DexCompilationUnit* const outer_compilation_unit,
                      CompilerDriver* driver,
                      const uint8_t* interpreter_metadata,
                      OptimizingCompilerStats* compiler_stats,
                      Handle<mirror::DexCache> dex_cache)
      : arena_(graph->GetArena()),
        graph_(graph),
        dex_file_(dex_file),
        code_item_(code_item),
        return_type_(return_type),
        block_builder_(block_builder),
        ssa_builder_(ssa_builder),
        locals_for_(arena_->Adapter(kArenaAllocGraphBuilder)),
        current_block_(nullptr),
        current_locals_(nullptr),
        latest_result_(nullptr),
        compiler_driver_(driver),
        dex_compilation_unit_(dex_compilation_unit),
        outer_compilation_unit_(outer_compilation_unit),
        interpreter_metadata_(interpreter_metadata),
        skipped_interpreter_metadata_(std::less<uint32_t>(),
                                      arena_->Adapter(kArenaAllocGraphBuilder)),
        compilation_stats_(compiler_stats),
        dex_cache_(dex_cache),
        loop_headers_(graph->GetArena()->Adapter(kArenaAllocGraphBuilder)) {
    loop_headers_.reserve(kDefaultNumberOfLoops);
  }

  bool Build();

 private:
  void MaybeRecordStat(MethodCompilationStat compilation_stat);

  void InitializeBlockLocals();
  void PropagateLocalsToCatchBlocks();
  void SetLoopHeaderPhiInputs();

  bool ProcessDexInstruction(const Instruction& instruction, uint32_t dex_pc);
  void FindNativeDebugInfoLocations(ArenaBitVector* locations);

  bool CanDecodeQuickenedInfo() const;
  uint16_t LookupQuickenedInfo(uint32_t dex_pc);

  HBasicBlock* FindBlockStartingAt(uint32_t dex_pc) const;

  ArenaVector<HInstruction*>* GetLocalsFor(HBasicBlock* block);
  HInstruction* ValueOfLocalAt(HBasicBlock* block, size_t local);
  HInstruction* LoadLocal(uint32_t register_index, Primitive::Type type) const;
  HInstruction* LoadNullCheckedLocal(uint32_t register_index, uint32_t dex_pc);
  void UpdateLocal(uint32_t register_index, HInstruction* instruction);

  void AppendInstruction(HInstruction* instruction);
  void InsertInstructionAtTop(HInstruction* instruction);
  void InitializeInstruction(HInstruction* instruction);

  void InitializeParameters();

  // Returns whether the current method needs access check for the type.
  // Output parameter finalizable is set to whether the type is finalizable.
  bool NeedsAccessCheck(uint32_t type_index,
                        Handle<mirror::DexCache> dex_cache,
                        /*out*/bool* finalizable) const
      SHARED_REQUIRES(Locks::mutator_lock_);
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

  // Builds an instruction sequence for a switch statement.
  void BuildSwitch(const Instruction& instruction, uint32_t dex_pc);

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
  void HandleStringInitResult(HInvokeStaticOrDirect* invoke);

  HClinitCheck* ProcessClinitCheckForInvoke(
      uint32_t dex_pc,
      ArtMethod* method,
      uint32_t method_idx,
      HInvokeStaticOrDirect::ClinitCheckRequirement* clinit_check_requirement)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Build a HNewInstance instruction.
  bool BuildNewInstance(uint16_t type_index, uint32_t dex_pc);

  // Return whether the compiler can assume `cls` is initialized.
  bool IsInitialized(Handle<mirror::Class> cls) const
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Try to resolve a method using the class linker. Return null if a method could
  // not be resolved.
  ArtMethod* ResolveMethod(uint16_t method_idx, InvokeType invoke_type);

  ArenaAllocator* const arena_;
  HGraph* const graph_;

  // The dex file where the method being compiled is, and the bytecode data.
  const DexFile* const dex_file_;
  const DexFile::CodeItem& code_item_;

  // The return type of the method being compiled.
  const Primitive::Type return_type_;

  HBasicBlockBuilder* block_builder_;
  SsaBuilder* ssa_builder_;

  ArenaVector<ArenaVector<HInstruction*>> locals_for_;
  HBasicBlock* current_block_;
  ArenaVector<HInstruction*>* current_locals_;
  HInstruction* latest_result_;

  CompilerDriver* const compiler_driver_;

  // The compilation unit of the current method being compiled. Note that
  // it can be an inlined method.
  DexCompilationUnit* const dex_compilation_unit_;

  // The compilation unit of the outermost method being compiled. That is the
  // method being compiled (and not inlined), and potentially inlining other
  // methods.
  const DexCompilationUnit* const outer_compilation_unit_;

  // Original values kept after instruction quickening. This is a data buffer
  // of Leb128-encoded (dex_pc, value) pairs sorted by dex_pc.
  const uint8_t* interpreter_metadata_;

  // InstructionBuilder does not parse instructions in dex_pc order. Quickening
  // info for out-of-order dex_pcs is stored in a map until the positions
  // are eventually visited.
  ArenaSafeMap<uint32_t, uint16_t> skipped_interpreter_metadata_;

  OptimizingCompilerStats* compilation_stats_;
  Handle<mirror::DexCache> dex_cache_;

  ArenaVector<HBasicBlock*> loop_headers_;

  static constexpr int kDefaultNumberOfLoops = 2;

  DISALLOW_COPY_AND_ASSIGN(HInstructionBuilder);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_INSTRUCTION_BUILDER_H_
