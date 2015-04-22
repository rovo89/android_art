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

#include "arch/instruction_set.h"
#include "arch/instruction_set_features.h"
#include "base/bit_field.h"
#include "driver/compiler_options.h"
#include "globals.h"
#include "locations.h"
#include "memory_region.h"
#include "nodes.h"
#include "stack_map_stream.h"

namespace art {

// Binary encoding of 2^32 for type double.
static int64_t constexpr k2Pow32EncodingForDouble = INT64_C(0x41F0000000000000);
// Binary encoding of 2^31 for type double.
static int64_t constexpr k2Pow31EncodingForDouble = INT64_C(0x41E0000000000000);

// Maximum value for a primitive integer.
static int32_t constexpr kPrimIntMax = 0x7fffffff;
// Maximum value for a primitive long.
static int64_t constexpr kPrimLongMax = 0x7fffffffffffffff;

class Assembler;
class CodeGenerator;
class DexCompilationUnit;
class ParallelMoveResolver;
class SrcMapElem;
template <class Alloc>
class SrcMap;
using DefaultSrcMap = SrcMap<std::allocator<SrcMapElem>>;

class CodeAllocator {
 public:
  CodeAllocator() {}
  virtual ~CodeAllocator() {}

  virtual uint8_t* Allocate(size_t size) = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(CodeAllocator);
};

struct PcInfo {
  uint32_t dex_pc;
  uintptr_t native_pc;
};

class SlowPathCode : public ArenaObject<kArenaAllocSlowPaths> {
 public:
  SlowPathCode() {
    for (size_t i = 0; i < kMaximumNumberOfExpectedRegisters; ++i) {
      saved_core_stack_offsets_[i] = kRegisterNotSaved;
      saved_fpu_stack_offsets_[i] = kRegisterNotSaved;
    }
  }

  virtual ~SlowPathCode() {}

  virtual void EmitNativeCode(CodeGenerator* codegen) = 0;

  void SaveLiveRegisters(CodeGenerator* codegen, LocationSummary* locations);
  void RestoreLiveRegisters(CodeGenerator* codegen, LocationSummary* locations);
  void RecordPcInfo(CodeGenerator* codegen, HInstruction* instruction, uint32_t dex_pc);

  bool IsCoreRegisterSaved(int reg) const {
    return saved_core_stack_offsets_[reg] != kRegisterNotSaved;
  }

  bool IsFpuRegisterSaved(int reg) const {
    return saved_fpu_stack_offsets_[reg] != kRegisterNotSaved;
  }

  uint32_t GetStackOffsetOfCoreRegister(int reg) const {
    return saved_core_stack_offsets_[reg];
  }

  uint32_t GetStackOffsetOfFpuRegister(int reg) const {
    return saved_fpu_stack_offsets_[reg];
  }

 private:
  static constexpr size_t kMaximumNumberOfExpectedRegisters = 32;
  static constexpr uint32_t kRegisterNotSaved = -1;
  uint32_t saved_core_stack_offsets_[kMaximumNumberOfExpectedRegisters];
  uint32_t saved_fpu_stack_offsets_[kMaximumNumberOfExpectedRegisters];
  DISALLOW_COPY_AND_ASSIGN(SlowPathCode);
};

class InvokeDexCallingConventionVisitor {
 public:
  virtual Location GetNextLocation(Primitive::Type type) = 0;

 protected:
  InvokeDexCallingConventionVisitor() {}
  virtual ~InvokeDexCallingConventionVisitor() {}

  // The current index for core registers.
  uint32_t gp_index_ = 0u;
  // The current index for floating-point registers.
  uint32_t float_index_ = 0u;
  // The current stack index.
  uint32_t stack_index_ = 0u;

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConventionVisitor);
};

class CodeGenerator {
 public:
  // Compiles the graph to executable instructions. Returns whether the compilation
  // succeeded.
  void CompileBaseline(CodeAllocator* allocator, bool is_leaf = false);
  void CompileOptimized(CodeAllocator* allocator);
  static CodeGenerator* Create(HGraph* graph,
                               InstructionSet instruction_set,
                               const InstructionSetFeatures& isa_features,
                               const CompilerOptions& compiler_options);
  virtual ~CodeGenerator() {}

  HGraph* GetGraph() const { return graph_; }

  HBasicBlock* GetNextBlockToEmit() const;
  HBasicBlock* FirstNonEmptyBlock(HBasicBlock* block) const;
  bool GoesToNextBlock(HBasicBlock* current, HBasicBlock* next) const;

  size_t GetStackSlotOfParameter(HParameterValue* parameter) const {
    // Note that this follows the current calling convention.
    return GetFrameSize()
        + InstructionSetPointerSize(GetInstructionSet())  // Art method
        + parameter->GetIndex() * kVRegSize;
  }

  virtual void Initialize() = 0;
  virtual void Finalize(CodeAllocator* allocator);
  virtual void GenerateFrameEntry() = 0;
  virtual void GenerateFrameExit() = 0;
  virtual void Bind(HBasicBlock* block) = 0;
  virtual void Move(HInstruction* instruction, Location location, HInstruction* move_for) = 0;
  virtual Assembler* GetAssembler() = 0;
  virtual size_t GetWordSize() const = 0;
  virtual size_t GetFloatingPointSpillSlotSize() const = 0;
  virtual uintptr_t GetAddressOf(HBasicBlock* block) const = 0;
  void InitializeCodeGeneration(size_t number_of_spill_slots,
                                size_t maximum_number_of_live_core_registers,
                                size_t maximum_number_of_live_fp_registers,
                                size_t number_of_out_slots,
                                const GrowableArray<HBasicBlock*>& block_order);
  int32_t GetStackSlot(HLocal* local) const;
  Location GetTemporaryLocation(HTemporary* temp) const;

  uint32_t GetFrameSize() const { return frame_size_; }
  void SetFrameSize(uint32_t size) { frame_size_ = size; }
  uint32_t GetCoreSpillMask() const { return core_spill_mask_; }
  uint32_t GetFpuSpillMask() const { return fpu_spill_mask_; }

  size_t GetNumberOfCoreRegisters() const { return number_of_core_registers_; }
  size_t GetNumberOfFloatingPointRegisters() const { return number_of_fpu_registers_; }
  virtual void SetupBlockedRegisters(bool is_baseline) const = 0;

  virtual void ComputeSpillMask() {
    core_spill_mask_ = allocated_registers_.GetCoreRegisters() & core_callee_save_mask_;
    DCHECK_NE(core_spill_mask_, 0u) << "At least the return address register must be saved";
    fpu_spill_mask_ = allocated_registers_.GetFloatingPointRegisters() & fpu_callee_save_mask_;
  }

  static uint32_t ComputeRegisterMask(const int* registers, size_t length) {
    uint32_t mask = 0;
    for (size_t i = 0, e = length; i < e; ++i) {
      mask |= (1 << registers[i]);
    }
    return mask;
  }

  virtual void DumpCoreRegister(std::ostream& stream, int reg) const = 0;
  virtual void DumpFloatingPointRegister(std::ostream& stream, int reg) const = 0;
  virtual InstructionSet GetInstructionSet() const = 0;

  const CompilerOptions& GetCompilerOptions() const { return compiler_options_; }

  // Saves the register in the stack. Returns the size taken on stack.
  virtual size_t SaveCoreRegister(size_t stack_index, uint32_t reg_id) = 0;
  // Restores the register from the stack. Returns the size taken on stack.
  virtual size_t RestoreCoreRegister(size_t stack_index, uint32_t reg_id) = 0;

  virtual size_t SaveFloatingPointRegister(size_t stack_index, uint32_t reg_id) = 0;
  virtual size_t RestoreFloatingPointRegister(size_t stack_index, uint32_t reg_id) = 0;

  virtual bool NeedsTwoRegisters(Primitive::Type type) const = 0;
  // Returns whether we should split long moves in parallel moves.
  virtual bool ShouldSplitLongMoves() const { return false; }

  bool IsCoreCalleeSaveRegister(int reg) const {
    return (core_callee_save_mask_ & (1 << reg)) != 0;
  }

  bool IsFloatingPointCalleeSaveRegister(int reg) const {
    return (fpu_callee_save_mask_ & (1 << reg)) != 0;
  }

  void RecordPcInfo(HInstruction* instruction, uint32_t dex_pc, SlowPathCode* slow_path = nullptr);
  bool CanMoveNullCheckToUser(HNullCheck* null_check);
  void MaybeRecordImplicitNullCheck(HInstruction* instruction);

  void AddSlowPath(SlowPathCode* slow_path) {
    slow_paths_.Add(slow_path);
  }

  void BuildSourceMap(DefaultSrcMap* src_map) const;
  void BuildMappingTable(std::vector<uint8_t>* vector) const;
  void BuildVMapTable(std::vector<uint8_t>* vector) const;
  void BuildNativeGCMap(
      std::vector<uint8_t>* vector, const DexCompilationUnit& dex_compilation_unit) const;
  void BuildStackMaps(std::vector<uint8_t>* vector);

  bool IsBaseline() const {
    return is_baseline_;
  }

  bool IsLeafMethod() const {
    return is_leaf_;
  }

  void MarkNotLeaf() {
    is_leaf_ = false;
    requires_current_method_ = true;
  }

  void SetRequiresCurrentMethod() {
    requires_current_method_ = true;
  }

  bool RequiresCurrentMethod() const {
    return requires_current_method_;
  }

  // Clears the spill slots taken by loop phis in the `LocationSummary` of the
  // suspend check. This is called when the code generator generates code
  // for the suspend check at the back edge (instead of where the suspend check
  // is, which is the loop entry). At this point, the spill slots for the phis
  // have not been written to.
  void ClearSpillSlotsFromLoopPhisInStackMap(HSuspendCheck* suspend_check) const;

  bool* GetBlockedCoreRegisters() const { return blocked_core_registers_; }
  bool* GetBlockedFloatingPointRegisters() const { return blocked_fpu_registers_; }

  // Helper that returns the pointer offset of an index in an object array.
  // Note: this method assumes we always have the same pointer size, regardless
  // of the architecture.
  static size_t GetCacheOffset(uint32_t index);
  // Pointer variant for ArtMethod and ArtField arrays.
  size_t GetCachePointerOffset(uint32_t index);

  void EmitParallelMoves(Location from1,
                         Location to1,
                         Primitive::Type type1,
                         Location from2,
                         Location to2,
                         Primitive::Type type2);

  static bool StoreNeedsWriteBarrier(Primitive::Type type, HInstruction* value) {
    // Check that null value is not represented as an integer constant.
    DCHECK(type != Primitive::kPrimNot || !value->IsIntConstant());
    return type == Primitive::kPrimNot && !value->IsNullConstant();
  }

  void AddAllocatedRegister(Location location) {
    allocated_registers_.Add(location);
  }

  void AllocateLocations(HInstruction* instruction);

  // Tells whether the stack frame of the compiled method is
  // considered "empty", that is either actually having a size of zero,
  // or just containing the saved return address register.
  bool HasEmptyFrame() const {
    return GetFrameSize() == (CallPushesPC() ? GetWordSize() : 0);
  }

  static int32_t GetInt32ValueOf(HConstant* constant) {
    if (constant->IsIntConstant()) {
      return constant->AsIntConstant()->GetValue();
    } else if (constant->IsNullConstant()) {
      return 0;
    } else {
      DCHECK(constant->IsFloatConstant());
      return bit_cast<int32_t, float>(constant->AsFloatConstant()->GetValue());
    }
  }

  static int64_t GetInt64ValueOf(HConstant* constant) {
    if (constant->IsIntConstant()) {
      return constant->AsIntConstant()->GetValue();
    } else if (constant->IsNullConstant()) {
      return 0;
    } else if (constant->IsFloatConstant()) {
      return bit_cast<int32_t, float>(constant->AsFloatConstant()->GetValue());
    } else if (constant->IsLongConstant()) {
      return constant->AsLongConstant()->GetValue();
    } else {
      DCHECK(constant->IsDoubleConstant());
      return bit_cast<int64_t, double>(constant->AsDoubleConstant()->GetValue());
    }
  }

  size_t GetFirstRegisterSlotInSlowPath() const {
    return first_register_slot_in_slow_path_;
  }

  uint32_t FrameEntrySpillSize() const {
    return GetFpuSpillSize() + GetCoreSpillSize();
  }

  virtual ParallelMoveResolver* GetMoveResolver() = 0;

 protected:
  CodeGenerator(HGraph* graph,
                size_t number_of_core_registers,
                size_t number_of_fpu_registers,
                size_t number_of_register_pairs,
                uint32_t core_callee_save_mask,
                uint32_t fpu_callee_save_mask,
                const CompilerOptions& compiler_options)
      : frame_size_(0),
        core_spill_mask_(0),
        fpu_spill_mask_(0),
        first_register_slot_in_slow_path_(0),
        blocked_core_registers_(graph->GetArena()->AllocArray<bool>(number_of_core_registers)),
        blocked_fpu_registers_(graph->GetArena()->AllocArray<bool>(number_of_fpu_registers)),
        blocked_register_pairs_(graph->GetArena()->AllocArray<bool>(number_of_register_pairs)),
        number_of_core_registers_(number_of_core_registers),
        number_of_fpu_registers_(number_of_fpu_registers),
        number_of_register_pairs_(number_of_register_pairs),
        core_callee_save_mask_(core_callee_save_mask),
        fpu_callee_save_mask_(fpu_callee_save_mask),
        is_baseline_(false),
        graph_(graph),
        compiler_options_(compiler_options),
        pc_infos_(graph->GetArena(), 32),
        slow_paths_(graph->GetArena(), 8),
        block_order_(nullptr),
        current_block_index_(0),
        is_leaf_(true),
        requires_current_method_(false),
        stack_map_stream_(graph->GetArena()) {}

  // Register allocation logic.
  void AllocateRegistersLocally(HInstruction* instruction) const;

  // Backend specific implementation for allocating a register.
  virtual Location AllocateFreeRegister(Primitive::Type type) const = 0;

  static size_t FindFreeEntry(bool* array, size_t length);
  static size_t FindTwoFreeConsecutiveAlignedEntries(bool* array, size_t length);

  virtual Location GetStackLocation(HLoadLocal* load) const = 0;

  virtual HGraphVisitor* GetLocationBuilder() = 0;
  virtual HGraphVisitor* GetInstructionVisitor() = 0;

  // Returns the location of the first spilled entry for floating point registers,
  // relative to the stack pointer.
  uint32_t GetFpuSpillStart() const {
    return GetFrameSize() - FrameEntrySpillSize();
  }

  uint32_t GetFpuSpillSize() const {
    return POPCOUNT(fpu_spill_mask_) * GetFloatingPointSpillSlotSize();
  }

  uint32_t GetCoreSpillSize() const {
    return POPCOUNT(core_spill_mask_) * GetWordSize();
  }

  bool HasAllocatedCalleeSaveRegisters() const {
    // We check the core registers against 1 because it always comprises the return PC.
    return (POPCOUNT(allocated_registers_.GetCoreRegisters() & core_callee_save_mask_) != 1)
      || (POPCOUNT(allocated_registers_.GetFloatingPointRegisters() & fpu_callee_save_mask_) != 0);
  }

  bool CallPushesPC() const {
    InstructionSet instruction_set = GetInstructionSet();
    return instruction_set == kX86 || instruction_set == kX86_64;
  }

  // Arm64 has its own type for a label, so we need to templatize this method
  // to share the logic.
  template <typename T>
  T* CommonGetLabelOf(T* raw_pointer_to_labels_array, HBasicBlock* block) const {
    block = FirstNonEmptyBlock(block);
    return raw_pointer_to_labels_array + block->GetBlockId();
  }

  // Frame size required for this method.
  uint32_t frame_size_;
  uint32_t core_spill_mask_;
  uint32_t fpu_spill_mask_;
  uint32_t first_register_slot_in_slow_path_;

  // Registers that were allocated during linear scan.
  RegisterSet allocated_registers_;

  // Arrays used when doing register allocation to know which
  // registers we can allocate. `SetupBlockedRegisters` updates the
  // arrays.
  bool* const blocked_core_registers_;
  bool* const blocked_fpu_registers_;
  bool* const blocked_register_pairs_;
  size_t number_of_core_registers_;
  size_t number_of_fpu_registers_;
  size_t number_of_register_pairs_;
  const uint32_t core_callee_save_mask_;
  const uint32_t fpu_callee_save_mask_;

  // Whether we are using baseline.
  bool is_baseline_;

 private:
  void InitLocationsBaseline(HInstruction* instruction);
  size_t GetStackOffsetOfSavedRegister(size_t index);
  void CompileInternal(CodeAllocator* allocator, bool is_baseline);
  void BlockIfInRegister(Location location, bool is_out = false) const;

  HGraph* const graph_;
  const CompilerOptions& compiler_options_;

  GrowableArray<PcInfo> pc_infos_;
  GrowableArray<SlowPathCode*> slow_paths_;

  // The order to use for code generation.
  const GrowableArray<HBasicBlock*>* block_order_;

  // The current block index in `block_order_` of the block
  // we are generating code for.
  size_t current_block_index_;

  // Whether the method is a leaf method.
  bool is_leaf_;

  // Whether an instruction in the graph accesses the current method.
  bool requires_current_method_;

  StackMapStream stack_map_stream_;

  friend class OptimizingCFITest;

  DISALLOW_COPY_AND_ASSIGN(CodeGenerator);
};

template <typename C, typename F>
class CallingConvention {
 public:
  CallingConvention(const C* registers,
                    size_t number_of_registers,
                    const F* fpu_registers,
                    size_t number_of_fpu_registers,
                    size_t pointer_size)
      : registers_(registers),
        number_of_registers_(number_of_registers),
        fpu_registers_(fpu_registers),
        number_of_fpu_registers_(number_of_fpu_registers),
        pointer_size_(pointer_size) {}

  size_t GetNumberOfRegisters() const { return number_of_registers_; }
  size_t GetNumberOfFpuRegisters() const { return number_of_fpu_registers_; }

  C GetRegisterAt(size_t index) const {
    DCHECK_LT(index, number_of_registers_);
    return registers_[index];
  }

  F GetFpuRegisterAt(size_t index) const {
    DCHECK_LT(index, number_of_fpu_registers_);
    return fpu_registers_[index];
  }

  size_t GetStackOffsetOf(size_t index) const {
    // We still reserve the space for parameters passed by registers.
    // Add space for the method pointer.
    return pointer_size_ + index * kVRegSize;
  }

 private:
  const C* registers_;
  const size_t number_of_registers_;
  const F* fpu_registers_;
  const size_t number_of_fpu_registers_;
  const size_t pointer_size_;

  DISALLOW_COPY_AND_ASSIGN(CallingConvention);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_H_
