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
#include "base/arena_containers.h"
#include "base/arena_object.h"
#include "base/bit_field.h"
#include "compiled_method.h"
#include "driver/compiler_options.h"
#include "globals.h"
#include "graph_visualizer.h"
#include "locations.h"
#include "memory_region.h"
#include "nodes.h"
#include "optimizing_compiler_stats.h"
#include "stack_map_stream.h"
#include "utils/label.h"

namespace art {

// Binary encoding of 2^32 for type double.
static int64_t constexpr k2Pow32EncodingForDouble = INT64_C(0x41F0000000000000);
// Binary encoding of 2^31 for type double.
static int64_t constexpr k2Pow31EncodingForDouble = INT64_C(0x41E0000000000000);

// Minimum value for a primitive integer.
static int32_t constexpr kPrimIntMin = 0x80000000;
// Minimum value for a primitive long.
static int64_t constexpr kPrimLongMin = INT64_C(0x8000000000000000);

// Maximum value for a primitive integer.
static int32_t constexpr kPrimIntMax = 0x7fffffff;
// Maximum value for a primitive long.
static int64_t constexpr kPrimLongMax = INT64_C(0x7fffffffffffffff);

class Assembler;
class CodeGenerator;
class CompilerDriver;
class LinkerPatch;
class ParallelMoveResolver;

class CodeAllocator {
 public:
  CodeAllocator() {}
  virtual ~CodeAllocator() {}

  virtual uint8_t* Allocate(size_t size) = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(CodeAllocator);
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

  virtual void SaveLiveRegisters(CodeGenerator* codegen, LocationSummary* locations);
  virtual void RestoreLiveRegisters(CodeGenerator* codegen, LocationSummary* locations);

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

  virtual bool IsFatal() const { return false; }

  virtual const char* GetDescription() const = 0;

  Label* GetEntryLabel() { return &entry_label_; }
  Label* GetExitLabel() { return &exit_label_; }

 protected:
  static constexpr size_t kMaximumNumberOfExpectedRegisters = 32;
  static constexpr uint32_t kRegisterNotSaved = -1;
  uint32_t saved_core_stack_offsets_[kMaximumNumberOfExpectedRegisters];
  uint32_t saved_fpu_stack_offsets_[kMaximumNumberOfExpectedRegisters];

 private:
  Label entry_label_;
  Label exit_label_;

  DISALLOW_COPY_AND_ASSIGN(SlowPathCode);
};

class InvokeDexCallingConventionVisitor {
 public:
  virtual Location GetNextLocation(Primitive::Type type) = 0;
  virtual Location GetReturnLocation(Primitive::Type type) const = 0;
  virtual Location GetMethodLocation() const = 0;

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

class FieldAccessCallingConvention {
 public:
  virtual Location GetObjectLocation() const = 0;
  virtual Location GetFieldIndexLocation() const = 0;
  virtual Location GetReturnLocation(Primitive::Type type) const = 0;
  virtual Location GetSetValueLocation(Primitive::Type type, bool is_instance) const = 0;
  virtual Location GetFpuLocation(Primitive::Type type) const = 0;
  virtual ~FieldAccessCallingConvention() {}

 protected:
  FieldAccessCallingConvention() {}

 private:
  DISALLOW_COPY_AND_ASSIGN(FieldAccessCallingConvention);
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
                               const CompilerOptions& compiler_options,
                               OptimizingCompilerStats* stats = nullptr);
  virtual ~CodeGenerator() {}

  // Get the graph. This is the outermost graph, never the graph of a method being inlined.
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
  virtual void EmitLinkerPatches(ArenaVector<LinkerPatch>* linker_patches);
  virtual void GenerateFrameEntry() = 0;
  virtual void GenerateFrameExit() = 0;
  virtual void Bind(HBasicBlock* block) = 0;
  virtual void Move(HInstruction* instruction, Location location, HInstruction* move_for) = 0;
  virtual void MoveConstant(Location destination, int32_t value) = 0;
  virtual void MoveLocation(Location dst, Location src, Primitive::Type dst_type) = 0;
  virtual void AddLocationAsTemp(Location location, LocationSummary* locations) = 0;

  virtual Assembler* GetAssembler() = 0;
  virtual const Assembler& GetAssembler() const = 0;
  virtual size_t GetWordSize() const = 0;
  virtual size_t GetFloatingPointSpillSlotSize() const = 0;
  virtual uintptr_t GetAddressOf(HBasicBlock* block) const = 0;
  void InitializeCodeGeneration(size_t number_of_spill_slots,
                                size_t maximum_number_of_live_core_registers,
                                size_t maximum_number_of_live_fp_registers,
                                size_t number_of_out_slots,
                                const ArenaVector<HBasicBlock*>& block_order);
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

  void MaybeRecordStat(MethodCompilationStat compilation_stat, size_t count = 1) const;

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

  // Record native to dex mapping for a suspend point.  Required by runtime.
  void RecordPcInfo(HInstruction* instruction, uint32_t dex_pc, SlowPathCode* slow_path = nullptr);
  // Record additional native to dex mappings for native debugging/profiling tools.
  void RecordNativeDebugInfo(uint32_t dex_pc, uintptr_t native_pc_begin, uintptr_t native_pc_end);

  bool CanMoveNullCheckToUser(HNullCheck* null_check);
  void MaybeRecordImplicitNullCheck(HInstruction* instruction);

  // Records a stack map which the runtime might use to set catch phi values
  // during exception delivery.
  // TODO: Replace with a catch-entering instruction that records the environment.
  void RecordCatchBlockInfo();

  // Returns true if implicit null checks are allowed in the compiler options
  // and if the null check is not inside a try block. We currently cannot do
  // implicit null checks in that case because we need the NullCheckSlowPath to
  // save live registers, which may be needed by the runtime to set catch phis.
  bool IsImplicitNullCheckAllowed(HNullCheck* null_check) const;

  void AddSlowPath(SlowPathCode* slow_path) {
    slow_paths_.push_back(slow_path);
  }

  void BuildMappingTable(ArenaVector<uint8_t>* vector) const;
  void BuildVMapTable(ArenaVector<uint8_t>* vector) const;
  void BuildNativeGCMap(
      ArenaVector<uint8_t>* vector, const CompilerDriver& compiler_driver) const;
  void BuildStackMaps(MemoryRegion region);
  size_t ComputeStackMapsSize();

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

  void ValidateInvokeRuntime(HInstruction* instruction, SlowPathCode* slow_path);

  void AddAllocatedRegister(Location location) {
    allocated_registers_.Add(location);
  }

  bool HasAllocatedRegister(bool is_core, int reg) const {
    return is_core
        ? allocated_registers_.ContainsCoreRegister(reg)
        : allocated_registers_.ContainsFloatingPointRegister(reg);
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

  static void CreateCommonInvokeLocationSummary(
      HInvoke* invoke, InvokeDexCallingConventionVisitor* visitor);

  void GenerateInvokeUnresolvedRuntimeCall(HInvokeUnresolved* invoke);

  void CreateUnresolvedFieldLocationSummary(
      HInstruction* field_access,
      Primitive::Type field_type,
      const FieldAccessCallingConvention& calling_convention);

  void GenerateUnresolvedFieldAccess(
      HInstruction* field_access,
      Primitive::Type field_type,
      uint32_t field_index,
      uint32_t dex_pc,
      const FieldAccessCallingConvention& calling_convention);

  // TODO: This overlaps a bit with MoveFromReturnRegister. Refactor for a better design.
  static void CreateLoadClassLocationSummary(HLoadClass* cls,
                                             Location runtime_type_index_location,
                                             Location runtime_return_location);

  static void CreateSystemArrayCopyLocationSummary(HInvoke* invoke);

  void SetDisassemblyInformation(DisassemblyInformation* info) { disasm_info_ = info; }
  DisassemblyInformation* GetDisassemblyInformation() const { return disasm_info_; }

  virtual void InvokeRuntime(QuickEntrypointEnum entrypoint,
                             HInstruction* instruction,
                             uint32_t dex_pc,
                             SlowPathCode* slow_path) = 0;

  // Check if the desired_dispatch_info is supported. If it is, return it,
  // otherwise return a fall-back info that should be used instead.
  virtual HInvokeStaticOrDirect::DispatchInfo GetSupportedInvokeStaticOrDirectDispatch(
      const HInvokeStaticOrDirect::DispatchInfo& desired_dispatch_info,
      MethodReference target_method) = 0;

  // Generate a call to a static or direct method.
  virtual void GenerateStaticOrDirectCall(HInvokeStaticOrDirect* invoke, Location temp) = 0;
  // Generate a call to a virtual method.
  virtual void GenerateVirtualCall(HInvokeVirtual* invoke, Location temp) = 0;

  // Copy the result of a call into the given target.
  virtual void MoveFromReturnRegister(Location trg, Primitive::Type type) = 0;

  const ArenaVector<SrcMapElem>& GetSrcMappingTable() const {
    return src_map_;
  }

 protected:
  // Method patch info used for recording locations of required linker patches and
  // target methods. The target method can be used for various purposes, whether for
  // patching the address of the method or the code pointer or a PC-relative call.
  template <typename LabelType>
  struct MethodPatchInfo {
    explicit MethodPatchInfo(MethodReference m) : target_method(m), label() { }

    MethodReference target_method;
    LabelType label;
  };

  CodeGenerator(HGraph* graph,
                size_t number_of_core_registers,
                size_t number_of_fpu_registers,
                size_t number_of_register_pairs,
                uint32_t core_callee_save_mask,
                uint32_t fpu_callee_save_mask,
                const CompilerOptions& compiler_options,
                OptimizingCompilerStats* stats)
      : frame_size_(0),
        core_spill_mask_(0),
        fpu_spill_mask_(0),
        first_register_slot_in_slow_path_(0),
        blocked_core_registers_(graph->GetArena()->AllocArray<bool>(number_of_core_registers,
                                                                    kArenaAllocCodeGenerator)),
        blocked_fpu_registers_(graph->GetArena()->AllocArray<bool>(number_of_fpu_registers,
                                                                   kArenaAllocCodeGenerator)),
        blocked_register_pairs_(graph->GetArena()->AllocArray<bool>(number_of_register_pairs,
                                                                    kArenaAllocCodeGenerator)),
        number_of_core_registers_(number_of_core_registers),
        number_of_fpu_registers_(number_of_fpu_registers),
        number_of_register_pairs_(number_of_register_pairs),
        core_callee_save_mask_(core_callee_save_mask),
        fpu_callee_save_mask_(fpu_callee_save_mask),
        stack_map_stream_(graph->GetArena()),
        block_order_(nullptr),
        is_baseline_(false),
        disasm_info_(nullptr),
        stats_(stats),
        graph_(graph),
        compiler_options_(compiler_options),
        src_map_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
        slow_paths_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
        current_slow_path_(nullptr),
        current_block_index_(0),
        is_leaf_(true),
        requires_current_method_(false) {
    slow_paths_.reserve(8);
  }

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

  // Arm64 has its own type for a label, so we need to templatize these methods
  // to share the logic.

  template <typename LabelType>
  LabelType* CommonInitializeLabels() {
    // We use raw array allocations instead of ArenaVector<> because Labels are
    // non-constructible and non-movable and as such cannot be held in a vector.
    size_t size = GetGraph()->GetBlocks().size();
    LabelType* labels = GetGraph()->GetArena()->AllocArray<LabelType>(size,
                                                                      kArenaAllocCodeGenerator);
    for (size_t i = 0; i != size; ++i) {
      new(labels + i) LabelType();
    }
    return labels;
  }

  template <typename LabelType>
  LabelType* CommonGetLabelOf(LabelType* raw_pointer_to_labels_array, HBasicBlock* block) const {
    block = FirstNonEmptyBlock(block);
    return raw_pointer_to_labels_array + block->GetBlockId();
  }

  SlowPathCode* GetCurrentSlowPath() {
    return current_slow_path_;
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

  StackMapStream stack_map_stream_;

  // The order to use for code generation.
  const ArenaVector<HBasicBlock*>* block_order_;

  // Whether we are using baseline.
  bool is_baseline_;

  DisassemblyInformation* disasm_info_;

 private:
  void InitLocationsBaseline(HInstruction* instruction);
  size_t GetStackOffsetOfSavedRegister(size_t index);
  void GenerateSlowPaths();
  void CompileInternal(CodeAllocator* allocator, bool is_baseline);
  void BlockIfInRegister(Location location, bool is_out = false) const;
  void EmitEnvironment(HEnvironment* environment, SlowPathCode* slow_path);

  OptimizingCompilerStats* stats_;

  HGraph* const graph_;
  const CompilerOptions& compiler_options_;

  // Native to dex_pc map used for native debugging/profiling tools.
  ArenaVector<SrcMapElem> src_map_;
  ArenaVector<SlowPathCode*> slow_paths_;

  // The current slow path that we're generating code for.
  SlowPathCode* current_slow_path_;

  // The current block index in `block_order_` of the block
  // we are generating code for.
  size_t current_block_index_;

  // Whether the method is a leaf method.
  bool is_leaf_;

  // Whether an instruction in the graph accesses the current method.
  bool requires_current_method_;

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
