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

#include "base/bit_field.h"
#include "globals.h"
#include "instruction_set.h"
#include "locations.h"
#include "memory_region.h"
#include "nodes.h"
#include "stack_map_stream.h"
#include "utils/assembler.h"

namespace art {

static size_t constexpr kVRegSize = 4;
static size_t constexpr kUninitializedFrameSize = 0;

class CodeGenerator;
class DexCompilationUnit;
class SrcMap;

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

class SlowPathCode : public ArenaObject {
 public:
  SlowPathCode() : entry_label_(), exit_label_() {}
  virtual ~SlowPathCode() {}

  Label* GetEntryLabel() { return &entry_label_; }
  Label* GetExitLabel() { return &exit_label_; }

  virtual void EmitNativeCode(CodeGenerator* codegen) = 0;

 private:
  Label entry_label_;
  Label exit_label_;

  DISALLOW_COPY_AND_ASSIGN(SlowPathCode);
};

class CodeGenerator : public ArenaObject {
 public:
  // Compiles the graph to executable instructions. Returns whether the compilation
  // succeeded.
  void CompileBaseline(CodeAllocator* allocator, bool is_leaf = false);
  void CompileOptimized(CodeAllocator* allocator);
  static CodeGenerator* Create(ArenaAllocator* allocator,
                               HGraph* graph,
                               InstructionSet instruction_set);

  HGraph* GetGraph() const { return graph_; }

  Label* GetLabelOf(HBasicBlock* block) const;
  bool GoesToNextBlock(HBasicBlock* current, HBasicBlock* next) const;

  size_t GetStackSlotOfParameter(HParameterValue* parameter) const {
    // Note that this follows the current calling convention.
    return GetFrameSize()
        + kVRegSize  // Art method
        + parameter->GetIndex() * kVRegSize;
  }

  virtual void GenerateFrameEntry() = 0;
  virtual void GenerateFrameExit() = 0;
  virtual void Bind(Label* label) = 0;
  virtual void Move(HInstruction* instruction, Location location, HInstruction* move_for) = 0;
  virtual HGraphVisitor* GetLocationBuilder() = 0;
  virtual HGraphVisitor* GetInstructionVisitor() = 0;
  virtual Assembler* GetAssembler() = 0;
  virtual size_t GetWordSize() const = 0;
  void ComputeFrameSize(size_t number_of_spill_slots,
                        size_t maximum_number_of_live_registers,
                        size_t number_of_out_slots);
  virtual size_t FrameEntrySpillSize() const = 0;
  int32_t GetStackSlot(HLocal* local) const;
  Location GetTemporaryLocation(HTemporary* temp) const;

  uint32_t GetFrameSize() const { return frame_size_; }
  void SetFrameSize(uint32_t size) { frame_size_ = size; }
  uint32_t GetCoreSpillMask() const { return core_spill_mask_; }

  size_t GetNumberOfCoreRegisters() const { return number_of_core_registers_; }
  size_t GetNumberOfFloatingPointRegisters() const { return number_of_fpu_registers_; }
  virtual void SetupBlockedRegisters() const = 0;

  virtual void DumpCoreRegister(std::ostream& stream, int reg) const = 0;
  virtual void DumpFloatingPointRegister(std::ostream& stream, int reg) const = 0;
  virtual InstructionSet GetInstructionSet() const = 0;
  virtual void SaveCoreRegister(Location stack_location, uint32_t reg_id) = 0;
  virtual void RestoreCoreRegister(Location stack_location, uint32_t reg_id) = 0;

  void RecordPcInfo(HInstruction* instruction, uint32_t dex_pc);

  void AddSlowPath(SlowPathCode* slow_path) {
    slow_paths_.Add(slow_path);
  }

  void GenerateSlowPaths();

  void BuildMappingTable(std::vector<uint8_t>* vector, SrcMap* src_map) const;
  void BuildVMapTable(std::vector<uint8_t>* vector) const;
  void BuildNativeGCMap(
      std::vector<uint8_t>* vector, const DexCompilationUnit& dex_compilation_unit) const;
  void BuildStackMaps(std::vector<uint8_t>* vector);
  void SaveLiveRegisters(LocationSummary* locations);
  void RestoreLiveRegisters(LocationSummary* locations);

  bool IsLeafMethod() const {
    return is_leaf_;
  }

  void MarkNotLeaf() {
    is_leaf_ = false;
  }

  // Clears the spill slots taken by loop phis in the `LocationSummary` of the
  // suspend check. This is called when the code generator generates code
  // for the suspend check at the back edge (instead of where the suspend check
  // is, which is the loop entry). At this point, the spill slots for the phis
  // have not been written to.
  void ClearSpillSlotsFromLoopPhisInStackMap(HSuspendCheck* suspend_check) const;

  bool* GetBlockedCoreRegisters() const { return blocked_core_registers_; }

 protected:
  CodeGenerator(HGraph* graph,
                size_t number_of_core_registers,
                size_t number_of_fpu_registers,
                size_t number_of_register_pairs)
      : frame_size_(kUninitializedFrameSize),
        core_spill_mask_(0),
        first_register_slot_in_slow_path_(0),
        blocked_core_registers_(graph->GetArena()->AllocArray<bool>(number_of_core_registers)),
        blocked_fpu_registers_(graph->GetArena()->AllocArray<bool>(number_of_fpu_registers)),
        blocked_register_pairs_(graph->GetArena()->AllocArray<bool>(number_of_register_pairs)),
        number_of_core_registers_(number_of_core_registers),
        number_of_fpu_registers_(number_of_fpu_registers),
        number_of_register_pairs_(number_of_register_pairs),
        graph_(graph),
        block_labels_(graph->GetArena(), 0),
        pc_infos_(graph->GetArena(), 32),
        slow_paths_(graph->GetArena(), 8),
        is_leaf_(true),
        stack_map_stream_(graph->GetArena()) {}
  ~CodeGenerator() {}

  // Register allocation logic.
  void AllocateRegistersLocally(HInstruction* instruction) const;

  // Backend specific implementation for allocating a register.
  virtual Location AllocateFreeRegister(Primitive::Type type) const = 0;

  static size_t FindFreeEntry(bool* array, size_t length);

  virtual Location GetStackLocation(HLoadLocal* load) const = 0;

  // Frame size required for this method.
  uint32_t frame_size_;
  uint32_t core_spill_mask_;
  uint32_t first_register_slot_in_slow_path_;

  // Arrays used when doing register allocation to know which
  // registers we can allocate. `SetupBlockedRegisters` updates the
  // arrays.
  bool* const blocked_core_registers_;
  bool* const blocked_fpu_registers_;
  bool* const blocked_register_pairs_;
  size_t number_of_core_registers_;
  size_t number_of_fpu_registers_;
  size_t number_of_register_pairs_;

 private:
  void InitLocations(HInstruction* instruction);
  size_t GetStackOffsetOfSavedRegister(size_t index);

  HGraph* const graph_;

  // Labels for each block that will be compiled.
  GrowableArray<Label> block_labels_;
  GrowableArray<PcInfo> pc_infos_;
  GrowableArray<SlowPathCode*> slow_paths_;

  bool is_leaf_;

  StackMapStream stack_map_stream_;

  DISALLOW_COPY_AND_ASSIGN(CodeGenerator);
};

template <typename C, typename F>
class CallingConvention {
 public:
  CallingConvention(const C* registers,
                    size_t number_of_registers,
                    const F* fpu_registers,
                    size_t number_of_fpu_registers)
      : registers_(registers),
        number_of_registers_(number_of_registers),
        fpu_registers_(fpu_registers),
        number_of_fpu_registers_(number_of_fpu_registers) {}

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
    // Add one for the method pointer.
    return (index + 1) * kVRegSize;
  }

 private:
  const C* registers_;
  const size_t number_of_registers_;
  const F* fpu_registers_;
  const size_t number_of_fpu_registers_;

  DISALLOW_COPY_AND_ASSIGN(CallingConvention);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_H_
