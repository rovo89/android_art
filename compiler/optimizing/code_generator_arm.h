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

#ifndef ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM_H_
#define ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM_H_

#include "code_generator.h"
#include "dex/compiler_enums.h"
#include "driver/compiler_options.h"
#include "nodes.h"
#include "parallel_move_resolver.h"
#include "utils/arm/assembler_thumb2.h"

namespace art {
namespace arm {

class CodeGeneratorARM;
class SlowPathCodeARM;

// Use a local definition to prevent copying mistakes.
static constexpr size_t kArmWordSize = kArmPointerSize;
static constexpr size_t kArmBitsPerWord = kArmWordSize * kBitsPerByte;

static constexpr Register kParameterCoreRegisters[] = { R1, R2, R3 };
static constexpr size_t kParameterCoreRegistersLength = arraysize(kParameterCoreRegisters);
static constexpr SRegister kParameterFpuRegisters[] =
    { S0, S1, S2, S3, S4, S5, S6, S7, S8, S9, S10, S11, S12, S13, S14, S15 };
static constexpr size_t kParameterFpuRegistersLength = arraysize(kParameterFpuRegisters);

static constexpr Register kArtMethodRegister = R0;

static constexpr Register kRuntimeParameterCoreRegisters[] = { R0, R1, R2, R3 };
static constexpr size_t kRuntimeParameterCoreRegistersLength =
    arraysize(kRuntimeParameterCoreRegisters);
static constexpr SRegister kRuntimeParameterFpuRegisters[] = { S0, S1, S2, S3 };
static constexpr size_t kRuntimeParameterFpuRegistersLength =
    arraysize(kRuntimeParameterFpuRegisters);

class InvokeRuntimeCallingConvention : public CallingConvention<Register, SRegister> {
 public:
  InvokeRuntimeCallingConvention()
      : CallingConvention(kRuntimeParameterCoreRegisters,
                          kRuntimeParameterCoreRegistersLength,
                          kRuntimeParameterFpuRegisters,
                          kRuntimeParameterFpuRegistersLength,
                          kArmPointerSize) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeRuntimeCallingConvention);
};

static constexpr DRegister FromLowSToD(SRegister reg) {
  return DCHECK_CONSTEXPR(reg % 2 == 0, , D0)
      static_cast<DRegister>(reg / 2);
}


class InvokeDexCallingConvention : public CallingConvention<Register, SRegister> {
 public:
  InvokeDexCallingConvention()
      : CallingConvention(kParameterCoreRegisters,
                          kParameterCoreRegistersLength,
                          kParameterFpuRegisters,
                          kParameterFpuRegistersLength,
                          kArmPointerSize) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConvention);
};

class InvokeDexCallingConventionVisitorARM : public InvokeDexCallingConventionVisitor {
 public:
  InvokeDexCallingConventionVisitorARM() {}
  virtual ~InvokeDexCallingConventionVisitorARM() {}

  Location GetNextLocation(Primitive::Type type) OVERRIDE;
  Location GetReturnLocation(Primitive::Type type);

 private:
  InvokeDexCallingConvention calling_convention;
  uint32_t double_index_ = 0;

  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConventionVisitorARM);
};

class ParallelMoveResolverARM : public ParallelMoveResolverWithSwap {
 public:
  ParallelMoveResolverARM(ArenaAllocator* allocator, CodeGeneratorARM* codegen)
      : ParallelMoveResolverWithSwap(allocator), codegen_(codegen) {}

  void EmitMove(size_t index) OVERRIDE;
  void EmitSwap(size_t index) OVERRIDE;
  void SpillScratch(int reg) OVERRIDE;
  void RestoreScratch(int reg) OVERRIDE;

  ArmAssembler* GetAssembler() const;

 private:
  void Exchange(Register reg, int mem);
  void Exchange(int mem1, int mem2);

  CodeGeneratorARM* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(ParallelMoveResolverARM);
};

class SlowPathCodeARM : public SlowPathCode {
 public:
  SlowPathCodeARM() : entry_label_(), exit_label_() {}

  Label* GetEntryLabel() { return &entry_label_; }
  Label* GetExitLabel() { return &exit_label_; }

 private:
  Label entry_label_;
  Label exit_label_;

  DISALLOW_COPY_AND_ASSIGN(SlowPathCodeARM);
};

class LocationsBuilderARM : public HGraphVisitor {
 public:
  LocationsBuilderARM(HGraph* graph, CodeGeneratorARM* codegen)
      : HGraphVisitor(graph), codegen_(codegen) {}

#define DECLARE_VISIT_INSTRUCTION(name, super)     \
  void Visit##name(H##name* instr);

  FOR_EACH_CONCRETE_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

 private:
  void HandleInvoke(HInvoke* invoke);
  void HandleBitwiseOperation(HBinaryOperation* operation);
  void HandleShift(HBinaryOperation* operation);
  void HandleFieldSet(HInstruction* instruction, const FieldInfo& field_info);
  void HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info);

  CodeGeneratorARM* const codegen_;
  InvokeDexCallingConventionVisitorARM parameter_visitor_;

  DISALLOW_COPY_AND_ASSIGN(LocationsBuilderARM);
};

class InstructionCodeGeneratorARM : public HGraphVisitor {
 public:
  InstructionCodeGeneratorARM(HGraph* graph, CodeGeneratorARM* codegen);

#define DECLARE_VISIT_INSTRUCTION(name, super)     \
  void Visit##name(H##name* instr);

  FOR_EACH_CONCRETE_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  ArmAssembler* GetAssembler() const { return assembler_; }

 private:
  // Generate code for the given suspend check. If not null, `successor`
  // is the block to branch to if the suspend check is not needed, and after
  // the suspend call.
  void GenerateSuspendCheck(HSuspendCheck* check, HBasicBlock* successor);
  void GenerateClassInitializationCheck(SlowPathCodeARM* slow_path, Register class_reg);
  void HandleBitwiseOperation(HBinaryOperation* operation);
  void HandleShift(HBinaryOperation* operation);
  void GenerateMemoryBarrier(MemBarrierKind kind);
  void GenerateWideAtomicStore(Register addr, uint32_t offset,
                               Register value_lo, Register value_hi,
                               Register temp1, Register temp2,
                               HInstruction* instruction);
  void GenerateWideAtomicLoad(Register addr, uint32_t offset,
                              Register out_lo, Register out_hi);
  void HandleFieldSet(HInstruction* instruction, const FieldInfo& field_info);
  void HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info);
  void GenerateImplicitNullCheck(HNullCheck* instruction);
  void GenerateExplicitNullCheck(HNullCheck* instruction);
  void GenerateTestAndBranch(HInstruction* instruction,
                             Label* true_target,
                             Label* false_target,
                             Label* always_true_target);

  ArmAssembler* const assembler_;
  CodeGeneratorARM* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(InstructionCodeGeneratorARM);
};

class CodeGeneratorARM : public CodeGenerator {
 public:
  CodeGeneratorARM(HGraph* graph,
                   const ArmInstructionSetFeatures& isa_features,
                   const CompilerOptions& compiler_options);
  virtual ~CodeGeneratorARM() {}

  void GenerateFrameEntry() OVERRIDE;
  void GenerateFrameExit() OVERRIDE;
  void Bind(HBasicBlock* block) OVERRIDE;
  void Move(HInstruction* instruction, Location location, HInstruction* move_for) OVERRIDE;
  size_t SaveCoreRegister(size_t stack_index, uint32_t reg_id) OVERRIDE;
  size_t RestoreCoreRegister(size_t stack_index, uint32_t reg_id) OVERRIDE;
  size_t SaveFloatingPointRegister(size_t stack_index, uint32_t reg_id) OVERRIDE;
  size_t RestoreFloatingPointRegister(size_t stack_index, uint32_t reg_id) OVERRIDE;

  size_t GetWordSize() const OVERRIDE {
    return kArmWordSize;
  }

  size_t GetFloatingPointSpillSlotSize() const OVERRIDE {
    // Allocated in S registers, which are word sized.
    return kArmWordSize;
  }

  HGraphVisitor* GetLocationBuilder() OVERRIDE {
    return &location_builder_;
  }

  HGraphVisitor* GetInstructionVisitor() OVERRIDE {
    return &instruction_visitor_;
  }

  ArmAssembler* GetAssembler() OVERRIDE {
    return &assembler_;
  }

  uintptr_t GetAddressOf(HBasicBlock* block) const OVERRIDE {
    return GetLabelOf(block)->Position();
  }

  void SetupBlockedRegisters(bool is_baseline) const OVERRIDE;

  Location AllocateFreeRegister(Primitive::Type type) const OVERRIDE;

  Location GetStackLocation(HLoadLocal* load) const OVERRIDE;

  void DumpCoreRegister(std::ostream& stream, int reg) const OVERRIDE;
  void DumpFloatingPointRegister(std::ostream& stream, int reg) const OVERRIDE;

  // Blocks all register pairs made out of blocked core registers.
  void UpdateBlockedPairRegisters() const;

  ParallelMoveResolverARM* GetMoveResolver() OVERRIDE {
    return &move_resolver_;
  }

  InstructionSet GetInstructionSet() const OVERRIDE {
    return InstructionSet::kThumb2;
  }

  // Helper method to move a 32bits value between two locations.
  void Move32(Location destination, Location source);
  // Helper method to move a 64bits value between two locations.
  void Move64(Location destination, Location source);

  // Load current method into `reg`.
  void LoadCurrentMethod(Register reg);

  // Generate code to invoke a runtime entry point.
  void InvokeRuntime(
      int32_t offset, HInstruction* instruction, uint32_t dex_pc, SlowPathCode* slow_path);

  // Emit a write barrier.
  void MarkGCCard(Register temp, Register card, Register object, Register value);

  Label* GetLabelOf(HBasicBlock* block) const {
    return CommonGetLabelOf<Label>(block_labels_.GetRawStorage(), block);
  }

  void Initialize() OVERRIDE {
    block_labels_.SetSize(GetGraph()->GetBlocks().Size());
  }

  const ArmInstructionSetFeatures& GetInstructionSetFeatures() const {
    return isa_features_;
  }

  bool NeedsTwoRegisters(Primitive::Type type) const OVERRIDE {
    return type == Primitive::kPrimDouble || type == Primitive::kPrimLong;
  }

  void ComputeSpillMask() OVERRIDE;

  Label* GetFrameEntryLabel() { return &frame_entry_label_; }

  void GenerateStaticOrDirectCall(HInvokeStaticOrDirect* invoke, Register temp);

 private:
  // Labels for each block that will be compiled.
  GrowableArray<Label> block_labels_;
  Label frame_entry_label_;
  LocationsBuilderARM location_builder_;
  InstructionCodeGeneratorARM instruction_visitor_;
  ParallelMoveResolverARM move_resolver_;
  Thumb2Assembler assembler_;
  const ArmInstructionSetFeatures& isa_features_;

  DISALLOW_COPY_AND_ASSIGN(CodeGeneratorARM);
};

}  // namespace arm
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM_H_
