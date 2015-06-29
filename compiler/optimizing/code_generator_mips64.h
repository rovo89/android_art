/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ART_COMPILER_OPTIMIZING_CODE_GENERATOR_MIPS64_H_
#define ART_COMPILER_OPTIMIZING_CODE_GENERATOR_MIPS64_H_

#include "code_generator.h"
#include "dex/compiler_enums.h"
#include "driver/compiler_options.h"
#include "nodes.h"
#include "parallel_move_resolver.h"
#include "utils/mips64/assembler_mips64.h"

namespace art {
namespace mips64 {

// Use a local definition to prevent copying mistakes.
static constexpr size_t kMips64WordSize = kMips64PointerSize;


// InvokeDexCallingConvention registers

static constexpr GpuRegister kParameterCoreRegisters[] =
    { A1, A2, A3, A4, A5, A6, A7 };
static constexpr size_t kParameterCoreRegistersLength = arraysize(kParameterCoreRegisters);

static constexpr FpuRegister kParameterFpuRegisters[] =
    { F13, F14, F15, F16, F17, F18, F19 };
static constexpr size_t kParameterFpuRegistersLength = arraysize(kParameterFpuRegisters);


// InvokeRuntimeCallingConvention registers

static constexpr GpuRegister kRuntimeParameterCoreRegisters[] =
    { A0, A1, A2, A3, A4, A5, A6, A7 };
static constexpr size_t kRuntimeParameterCoreRegistersLength =
    arraysize(kRuntimeParameterCoreRegisters);

static constexpr FpuRegister kRuntimeParameterFpuRegisters[] =
    { F12, F13, F14, F15, F16, F17, F18, F19 };
static constexpr size_t kRuntimeParameterFpuRegistersLength =
    arraysize(kRuntimeParameterFpuRegisters);


static constexpr GpuRegister kCoreCalleeSaves[] =
    { S0, S1, S2, S3, S4, S5, S6, S7, GP, S8, RA };  // TODO: review
static constexpr FpuRegister kFpuCalleeSaves[] =
    { F24, F25, F26, F27, F28, F29, F30, F31 };


class CodeGeneratorMIPS64;

class InvokeDexCallingConvention : public CallingConvention<GpuRegister, FpuRegister> {
 public:
  InvokeDexCallingConvention()
      : CallingConvention(kParameterCoreRegisters,
                          kParameterCoreRegistersLength,
                          kParameterFpuRegisters,
                          kParameterFpuRegistersLength,
                          kMips64PointerSize) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConvention);
};

class InvokeDexCallingConventionVisitorMIPS64 : public InvokeDexCallingConventionVisitor {
 public:
  InvokeDexCallingConventionVisitorMIPS64() {}
  virtual ~InvokeDexCallingConventionVisitorMIPS64() {}

  Location GetNextLocation(Primitive::Type type) OVERRIDE;
  Location GetReturnLocation(Primitive::Type type) const;

 private:
  InvokeDexCallingConvention calling_convention;

  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConventionVisitorMIPS64);
};

class InvokeRuntimeCallingConvention : public CallingConvention<GpuRegister, FpuRegister> {
 public:
  InvokeRuntimeCallingConvention()
      : CallingConvention(kRuntimeParameterCoreRegisters,
                          kRuntimeParameterCoreRegistersLength,
                          kRuntimeParameterFpuRegisters,
                          kRuntimeParameterFpuRegistersLength,
                          kMips64PointerSize) {}

  Location GetReturnLocation(Primitive::Type return_type);

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeRuntimeCallingConvention);
};

class ParallelMoveResolverMIPS64 : public ParallelMoveResolverWithSwap {
 public:
  ParallelMoveResolverMIPS64(ArenaAllocator* allocator, CodeGeneratorMIPS64* codegen)
      : ParallelMoveResolverWithSwap(allocator), codegen_(codegen) {}

  void EmitMove(size_t index) OVERRIDE;
  void EmitSwap(size_t index) OVERRIDE;
  void SpillScratch(int reg) OVERRIDE;
  void RestoreScratch(int reg) OVERRIDE;

  void Exchange(int index1, int index2, bool double_slot);

  Mips64Assembler* GetAssembler() const;

 private:
  CodeGeneratorMIPS64* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(ParallelMoveResolverMIPS64);
};

class SlowPathCodeMIPS64 : public SlowPathCode {
 public:
  SlowPathCodeMIPS64() : entry_label_(), exit_label_() {}

  Label* GetEntryLabel() { return &entry_label_; }
  Label* GetExitLabel() { return &exit_label_; }

 private:
  Label entry_label_;
  Label exit_label_;

  DISALLOW_COPY_AND_ASSIGN(SlowPathCodeMIPS64);
};

class LocationsBuilderMIPS64 : public HGraphVisitor {
 public:
  LocationsBuilderMIPS64(HGraph* graph, CodeGeneratorMIPS64* codegen)
      : HGraphVisitor(graph), codegen_(codegen) {}

#define DECLARE_VISIT_INSTRUCTION(name, super)     \
  void Visit##name(H##name* instr);

  FOR_EACH_CONCRETE_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

 private:
  void HandleInvoke(HInvoke* invoke);
  void HandleBinaryOp(HBinaryOperation* operation);
  void HandleShift(HBinaryOperation* operation);
  void HandleFieldSet(HInstruction* instruction, const FieldInfo& field_info);
  void HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info);

  InvokeDexCallingConventionVisitorMIPS64 parameter_visitor_;

  CodeGeneratorMIPS64* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(LocationsBuilderMIPS64);
};

class InstructionCodeGeneratorMIPS64 : public HGraphVisitor {
 public:
  InstructionCodeGeneratorMIPS64(HGraph* graph, CodeGeneratorMIPS64* codegen);

#define DECLARE_VISIT_INSTRUCTION(name, super)     \
  void Visit##name(H##name* instr);

  FOR_EACH_CONCRETE_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  Mips64Assembler* GetAssembler() const { return assembler_; }

 private:
  // Generate code for the given suspend check. If not null, `successor`
  // is the block to branch to if the suspend check is not needed, and after
  // the suspend call.
  void GenerateClassInitializationCheck(SlowPathCodeMIPS64* slow_path, GpuRegister class_reg);
  void GenerateMemoryBarrier(MemBarrierKind kind);
  void GenerateSuspendCheck(HSuspendCheck* check, HBasicBlock* successor);
  void HandleBinaryOp(HBinaryOperation* operation);
  void HandleShift(HBinaryOperation* operation);
  void HandleFieldSet(HInstruction* instruction, const FieldInfo& field_info);
  void HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info);
  void GenerateImplicitNullCheck(HNullCheck* instruction);
  void GenerateExplicitNullCheck(HNullCheck* instruction);
  void GenerateTestAndBranch(HInstruction* instruction,
                             Label* true_target,
                             Label* false_target,
                             Label* always_true_target);

  Mips64Assembler* const assembler_;
  CodeGeneratorMIPS64* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(InstructionCodeGeneratorMIPS64);
};

class CodeGeneratorMIPS64 : public CodeGenerator {
 public:
  CodeGeneratorMIPS64(HGraph* graph,
                      const Mips64InstructionSetFeatures& isa_features,
                      const CompilerOptions& compiler_options);
  virtual ~CodeGeneratorMIPS64() {}

  void GenerateFrameEntry() OVERRIDE;
  void GenerateFrameExit() OVERRIDE;

  void Bind(HBasicBlock* block) OVERRIDE;

  void Move(HInstruction* instruction, Location location, HInstruction* move_for) OVERRIDE;

  size_t GetWordSize() const OVERRIDE { return kMips64WordSize; }

  size_t GetFloatingPointSpillSlotSize() const OVERRIDE { return kMips64WordSize; }

  uintptr_t GetAddressOf(HBasicBlock* block) const OVERRIDE {
    return GetLabelOf(block)->Position();
  }

  HGraphVisitor* GetLocationBuilder() OVERRIDE { return &location_builder_; }
  HGraphVisitor* GetInstructionVisitor() OVERRIDE { return &instruction_visitor_; }
  Mips64Assembler* GetAssembler() OVERRIDE { return &assembler_; }

  void MarkGCCard(GpuRegister object, GpuRegister value);

  // Register allocation.

  void SetupBlockedRegisters(bool is_baseline) const OVERRIDE;
  // AllocateFreeRegister() is only used when allocating registers locally
  // during CompileBaseline().
  Location AllocateFreeRegister(Primitive::Type type) const OVERRIDE;

  Location GetStackLocation(HLoadLocal* load) const OVERRIDE;

  size_t SaveCoreRegister(size_t stack_index, uint32_t reg_id);
  size_t RestoreCoreRegister(size_t stack_index, uint32_t reg_id);
  size_t SaveFloatingPointRegister(size_t stack_index, uint32_t reg_id);
  size_t RestoreFloatingPointRegister(size_t stack_index, uint32_t reg_id);

  void DumpCoreRegister(std::ostream& stream, int reg) const OVERRIDE;
  void DumpFloatingPointRegister(std::ostream& stream, int reg) const OVERRIDE;

  InstructionSet GetInstructionSet() const OVERRIDE { return InstructionSet::kMips64; }

  const Mips64InstructionSetFeatures& GetInstructionSetFeatures() const {
    return isa_features_;
  }

  Label* GetLabelOf(HBasicBlock* block) const {
    return CommonGetLabelOf<Label>(block_labels_.GetRawStorage(), block);
  }

  void Initialize() OVERRIDE {
    block_labels_.SetSize(GetGraph()->GetBlocks().Size());
  }

  void Finalize(CodeAllocator* allocator) OVERRIDE;

  // Code generation helpers.

  void MoveLocation(Location destination, Location source, Primitive::Type type);

  void SwapLocations(Location loc1, Location loc2, Primitive::Type type);

  void LoadCurrentMethod(GpuRegister current_method);

  // Generate code to invoke a runtime entry point.
  void InvokeRuntime(int32_t offset,
                     HInstruction* instruction,
                     uint32_t dex_pc,
                     SlowPathCode* slow_path);

  ParallelMoveResolver* GetMoveResolver() OVERRIDE { return &move_resolver_; }

  bool NeedsTwoRegisters(Primitive::Type type ATTRIBUTE_UNUSED) const { return false; }

  void GenerateStaticOrDirectCall(HInvokeStaticOrDirect* invoke, GpuRegister temp);

 private:
  // Labels for each block that will be compiled.
  GrowableArray<Label> block_labels_;
  Label frame_entry_label_;
  LocationsBuilderMIPS64 location_builder_;
  InstructionCodeGeneratorMIPS64 instruction_visitor_;
  ParallelMoveResolverMIPS64 move_resolver_;
  Mips64Assembler assembler_;
  const Mips64InstructionSetFeatures& isa_features_;

  DISALLOW_COPY_AND_ASSIGN(CodeGeneratorMIPS64);
};

}  // namespace mips64
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_MIPS64_H_
