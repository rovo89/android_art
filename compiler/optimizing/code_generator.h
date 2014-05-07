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
#include "memory_region.h"
#include "nodes.h"
#include "utils/assembler.h"

namespace art {

static size_t constexpr kVRegSize = 4;

class DexCompilationUnit;

class CodeAllocator {
 public:
  CodeAllocator() { }
  virtual ~CodeAllocator() { }

  virtual uint8_t* Allocate(size_t size) = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(CodeAllocator);
};

struct PcInfo {
  uint32_t dex_pc;
  uintptr_t native_pc;
};

/**
 * A Location is an abstraction over the potential location
 * of an instruction. It could be in register or stack.
 */
class Location : public ValueObject {
 public:
  enum Kind {
    kInvalid = 0,
    kStackSlot = 1,  // Word size slot.
    kDoubleStackSlot = 2,  // 64bit stack slot.
    kRegister = 3,
    // On 32bits architectures, quick can pass a long where the
    // low bits are in the last parameter register, and the high
    // bits are in a stack slot. The kQuickParameter kind is for
    // handling this special case.
    kQuickParameter = 4,

    // Unallocated location represents a location that is not fixed and can be
    // allocated by a register allocator.  Each unallocated location has
    // a policy that specifies what kind of location is suitable. Payload
    // contains register allocation policy.
    kUnallocated = 5,
  };

  Location() : value_(kInvalid) {
    DCHECK(!IsValid());
  }

  Location(const Location& other) : ValueObject(), value_(other.value_) {}

  Location& operator=(const Location& other) {
    value_ = other.value_;
    return *this;
  }

  bool IsValid() const {
    return value_ != kInvalid;
  }

  // Register locations.
  static Location RegisterLocation(ManagedRegister reg) {
    return Location(kRegister, reg.RegId());
  }

  bool IsRegister() const {
    return GetKind() == kRegister;
  }

  ManagedRegister reg() const {
    DCHECK(IsRegister());
    return static_cast<ManagedRegister>(GetPayload());
  }

  static uword EncodeStackIndex(intptr_t stack_index) {
    DCHECK(-kStackIndexBias <= stack_index);
    DCHECK(stack_index < kStackIndexBias);
    return static_cast<uword>(kStackIndexBias + stack_index);
  }

  static Location StackSlot(intptr_t stack_index) {
    uword payload = EncodeStackIndex(stack_index);
    Location loc(kStackSlot, payload);
    // Ensure that sign is preserved.
    DCHECK_EQ(loc.GetStackIndex(), stack_index);
    return loc;
  }

  bool IsStackSlot() const {
    return GetKind() == kStackSlot;
  }

  static Location DoubleStackSlot(intptr_t stack_index) {
    uword payload = EncodeStackIndex(stack_index);
    Location loc(kDoubleStackSlot, payload);
    // Ensure that sign is preserved.
    DCHECK_EQ(loc.GetStackIndex(), stack_index);
    return loc;
  }

  bool IsDoubleStackSlot() const {
    return GetKind() == kDoubleStackSlot;
  }

  intptr_t GetStackIndex() const {
    DCHECK(IsStackSlot() || IsDoubleStackSlot());
    // Decode stack index manually to preserve sign.
    return GetPayload() - kStackIndexBias;
  }

  intptr_t GetHighStackIndex(uintptr_t word_size) const {
    DCHECK(IsDoubleStackSlot());
    // Decode stack index manually to preserve sign.
    return GetPayload() - kStackIndexBias + word_size;
  }

  static Location QuickParameter(uint32_t parameter_index) {
    return Location(kQuickParameter, parameter_index);
  }

  uint32_t GetQuickParameterIndex() const {
    DCHECK(IsQuickParameter());
    return GetPayload();
  }

  bool IsQuickParameter() const {
    return GetKind() == kQuickParameter;
  }

  arm::ArmManagedRegister AsArm() const;
  x86::X86ManagedRegister AsX86() const;

  Kind GetKind() const {
    return KindField::Decode(value_);
  }

  bool Equals(Location other) const {
    return value_ == other.value_;
  }

  const char* DebugString() const {
    switch (GetKind()) {
      case kInvalid: return "?";
      case kRegister: return "R";
      case kStackSlot: return "S";
      case kDoubleStackSlot: return "DS";
      case kQuickParameter: return "Q";
      case kUnallocated: return "U";
    }
    return "?";
  }

  // Unallocated locations.
  enum Policy {
    kAny,
    kRequiresRegister,
    kSameAsFirstInput,
  };

  bool IsUnallocated() const {
    return GetKind() == kUnallocated;
  }

  static Location UnallocatedLocation(Policy policy) {
    return Location(kUnallocated, PolicyField::Encode(policy));
  }

  // Any free register is suitable to replace this unallocated location.
  static Location Any() {
    return UnallocatedLocation(kAny);
  }

  static Location RequiresRegister() {
    return UnallocatedLocation(kRequiresRegister);
  }

  // The location of the first input to the instruction will be
  // used to replace this unallocated location.
  static Location SameAsFirstInput() {
    return UnallocatedLocation(kSameAsFirstInput);
  }

  Policy GetPolicy() const {
    DCHECK(IsUnallocated());
    return PolicyField::Decode(GetPayload());
  }

  uword GetEncoding() const {
    return GetPayload();
  }

 private:
  // Number of bits required to encode Kind value.
  static constexpr uint32_t kBitsForKind = 4;
  static constexpr uint32_t kBitsForPayload = kWordSize * kBitsPerByte - kBitsForKind;

  explicit Location(uword value) : value_(value) {}

  Location(Kind kind, uword payload)
      : value_(KindField::Encode(kind) | PayloadField::Encode(payload)) {}

  uword GetPayload() const {
    return PayloadField::Decode(value_);
  }

  typedef BitField<Kind, 0, kBitsForKind> KindField;
  typedef BitField<uword, kBitsForKind, kBitsForPayload> PayloadField;

  // Layout for kUnallocated locations payload.
  typedef BitField<Policy, 0, 3> PolicyField;

  // Layout for stack slots.
  static const intptr_t kStackIndexBias =
      static_cast<intptr_t>(1) << (kBitsForPayload - 1);

  // Location either contains kind and payload fields or a tagged handle for
  // a constant locations. Values of enumeration Kind are selected in such a
  // way that none of them can be interpreted as a kConstant tag.
  uword value_;
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
      : inputs_(instruction->GetBlock()->GetGraph()->GetArena(), instruction->InputCount()),
        temps_(instruction->GetBlock()->GetGraph()->GetArena(), 0) {
    inputs_.SetSize(instruction->InputCount());
    for (size_t i = 0; i < instruction->InputCount(); i++) {
      inputs_.Put(i, Location());
    }
  }

  void SetInAt(uint32_t at, Location location) {
    inputs_.Put(at, location);
  }

  Location InAt(uint32_t at) const {
    return inputs_.Get(at);
  }

  size_t GetInputCount() const {
    return inputs_.Size();
  }

  void SetOut(Location location) {
    output_ = Location(location);
  }

  void AddTemp(Location location) {
    temps_.Add(location);
  }

  Location GetTemp(uint32_t at) const {
    return temps_.Get(at);
  }

  void SetTempAt(uint32_t at, Location location) {
    temps_.Put(at, location);
  }

  size_t GetTempCount() const {
    return temps_.Size();
  }

  Location Out() const { return output_; }

 private:
  GrowableArray<Location> inputs_;
  GrowableArray<Location> temps_;
  Location output_;

  DISALLOW_COPY_AND_ASSIGN(LocationSummary);
};

class CodeGenerator : public ArenaObject {
 public:
  // Compiles the graph to executable instructions. Returns whether the compilation
  // succeeded.
  void Compile(CodeAllocator* allocator);
  static CodeGenerator* Create(ArenaAllocator* allocator,
                               HGraph* graph,
                               InstructionSet instruction_set);

  HGraph* GetGraph() const { return graph_; }

  Label* GetLabelOf(HBasicBlock* block) const;
  bool GoesToNextBlock(HBasicBlock* current, HBasicBlock* next) const;

  virtual void GenerateFrameEntry() = 0;
  virtual void GenerateFrameExit() = 0;
  virtual void Bind(Label* label) = 0;
  virtual void Move(HInstruction* instruction, Location location, HInstruction* move_for) = 0;
  virtual HGraphVisitor* GetLocationBuilder() = 0;
  virtual HGraphVisitor* GetInstructionVisitor() = 0;
  virtual Assembler* GetAssembler() = 0;
  virtual size_t GetWordSize() const = 0;

  uint32_t GetFrameSize() const { return frame_size_; }
  void SetFrameSize(uint32_t size) { frame_size_ = size; }
  uint32_t GetCoreSpillMask() const { return core_spill_mask_; }

  void RecordPcInfo(uint32_t dex_pc) {
    struct PcInfo pc_info;
    pc_info.dex_pc = dex_pc;
    pc_info.native_pc = GetAssembler()->CodeSize();
    pc_infos_.Add(pc_info);
  }

  void BuildMappingTable(std::vector<uint8_t>* vector) const;
  void BuildVMapTable(std::vector<uint8_t>* vector) const;
  void BuildNativeGCMap(
      std::vector<uint8_t>* vector, const DexCompilationUnit& dex_compilation_unit) const;

 protected:
  CodeGenerator(HGraph* graph, size_t number_of_registers)
      : frame_size_(0),
        graph_(graph),
        block_labels_(graph->GetArena(), 0),
        pc_infos_(graph->GetArena(), 32),
        blocked_registers_(static_cast<bool*>(
            graph->GetArena()->Alloc(number_of_registers * sizeof(bool), kArenaAllocData))) {
    block_labels_.SetSize(graph->GetBlocks().Size());
  }
  ~CodeGenerator() { }

  // Register allocation logic.
  void AllocateRegistersLocally(HInstruction* instruction) const;

  // Backend specific implementation for allocating a register.
  virtual ManagedRegister AllocateFreeRegister(Primitive::Type type,
                                               bool* blocked_registers) const = 0;

  // Raw implementation of allocating a register: loops over blocked_registers to find
  // the first available register.
  size_t AllocateFreeRegisterInternal(bool* blocked_registers, size_t number_of_registers) const;

  virtual void SetupBlockedRegisters(bool* blocked_registers) const = 0;
  virtual size_t GetNumberOfRegisters() const = 0;

  virtual Location GetStackLocation(HLoadLocal* load) const = 0;

  // Frame size required for this method.
  uint32_t frame_size_;
  uint32_t core_spill_mask_;

 private:
  void InitLocations(HInstruction* instruction);
  void CompileBlock(HBasicBlock* block);

  HGraph* const graph_;

  // Labels for each block that will be compiled.
  GrowableArray<Label> block_labels_;
  GrowableArray<PcInfo> pc_infos_;

  // Temporary data structure used when doing register allocation.
  bool* const blocked_registers_;

  DISALLOW_COPY_AND_ASSIGN(CodeGenerator);
};

template <typename T>
class CallingConvention {
 public:
  CallingConvention(const T* registers, int number_of_registers)
      : registers_(registers), number_of_registers_(number_of_registers) {}

  size_t GetNumberOfRegisters() const { return number_of_registers_; }

  T GetRegisterAt(size_t index) const {
    DCHECK_LT(index, number_of_registers_);
    return registers_[index];
  }

  uint8_t GetStackOffsetOf(size_t index, size_t word_size) const {
    // We still reserve the space for parameters passed by registers.
    // Add word_size for the method pointer.
    return index * kVRegSize + word_size;
  }

 private:
  const T* registers_;
  const size_t number_of_registers_;

  DISALLOW_COPY_AND_ASSIGN(CallingConvention);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_H_
