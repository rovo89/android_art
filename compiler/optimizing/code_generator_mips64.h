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
  Location GetReturnLocation(Primitive::Type type) const OVERRIDE;
  Location GetMethodLocation() const OVERRIDE;

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

class FieldAccessCallingConventionMIPS64 : public FieldAccessCallingConvention {
 public:
  FieldAccessCallingConventionMIPS64() {}

  Location GetObjectLocation() const OVERRIDE {
    return Location::RegisterLocation(A1);
  }
  Location GetFieldIndexLocation() const OVERRIDE {
    return Location::RegisterLocation(A0);
  }
  Location GetReturnLocation(Primitive::Type type ATTRIBUTE_UNUSED) const OVERRIDE {
    return Location::RegisterLocation(V0);
  }
  Location GetSetValueLocation(Primitive::Type type, bool is_instance) const OVERRIDE {
    return Primitive::Is64BitType(type)
        ? Location::RegisterLocation(A2)
        : (is_instance
            ? Location::RegisterLocation(A2)
            : Location::RegisterLocation(A1));
  }
  Location GetFpuLocation(Primitive::Type type ATTRIBUTE_UNUSED) const OVERRIDE {
    return Location::FpuRegisterLocation(F0);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(FieldAccessCallingConventionMIPS64);
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
  explicit SlowPathCodeMIPS64(HInstruction* instruction)
      : SlowPathCode(instruction), entry_label_(), exit_label_() {}

  Mips64Label* GetEntryLabel() { return &entry_label_; }
  Mips64Label* GetExitLabel() { return &exit_label_; }

 private:
  Mips64Label entry_label_;
  Mips64Label exit_label_;

  DISALLOW_COPY_AND_ASSIGN(SlowPathCodeMIPS64);
};

class LocationsBuilderMIPS64 : public HGraphVisitor {
 public:
  LocationsBuilderMIPS64(HGraph* graph, CodeGeneratorMIPS64* codegen)
      : HGraphVisitor(graph), codegen_(codegen) {}

#define DECLARE_VISIT_INSTRUCTION(name, super)     \
  void Visit##name(H##name* instr) OVERRIDE;

  FOR_EACH_CONCRETE_INSTRUCTION_COMMON(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_MIPS64(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  void VisitInstruction(HInstruction* instruction) OVERRIDE {
    LOG(FATAL) << "Unreachable instruction " << instruction->DebugName()
               << " (id " << instruction->GetId() << ")";
  }

 private:
  void HandleInvoke(HInvoke* invoke);
  void HandleBinaryOp(HBinaryOperation* operation);
  void HandleCondition(HCondition* instruction);
  void HandleShift(HBinaryOperation* operation);
  void HandleFieldSet(HInstruction* instruction, const FieldInfo& field_info);
  void HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info);

  InvokeDexCallingConventionVisitorMIPS64 parameter_visitor_;

  CodeGeneratorMIPS64* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(LocationsBuilderMIPS64);
};

class InstructionCodeGeneratorMIPS64 : public InstructionCodeGenerator {
 public:
  InstructionCodeGeneratorMIPS64(HGraph* graph, CodeGeneratorMIPS64* codegen);

#define DECLARE_VISIT_INSTRUCTION(name, super)     \
  void Visit##name(H##name* instr) OVERRIDE;

  FOR_EACH_CONCRETE_INSTRUCTION_COMMON(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_MIPS64(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  void VisitInstruction(HInstruction* instruction) OVERRIDE {
    LOG(FATAL) << "Unreachable instruction " << instruction->DebugName()
               << " (id " << instruction->GetId() << ")";
  }

  Mips64Assembler* GetAssembler() const { return assembler_; }

 private:
  void GenerateClassInitializationCheck(SlowPathCodeMIPS64* slow_path, GpuRegister class_reg);
  void GenerateMemoryBarrier(MemBarrierKind kind);
  void GenerateSuspendCheck(HSuspendCheck* check, HBasicBlock* successor);
  void HandleBinaryOp(HBinaryOperation* operation);
  void HandleCondition(HCondition* instruction);
  void HandleShift(HBinaryOperation* operation);
  void HandleFieldSet(HInstruction* instruction,
                      const FieldInfo& field_info,
                      bool value_can_be_null);
  void HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info);
  void GenerateTestAndBranch(HInstruction* instruction,
                             size_t condition_input_index,
                             Mips64Label* true_target,
                             Mips64Label* false_target);
  void DivRemOneOrMinusOne(HBinaryOperation* instruction);
  void DivRemByPowerOfTwo(HBinaryOperation* instruction);
  void GenerateDivRemWithAnyConstant(HBinaryOperation* instruction);
  void GenerateDivRemIntegral(HBinaryOperation* instruction);
  void GenerateIntLongCompare(IfCondition cond, bool is64bit, LocationSummary* locations);
  void GenerateIntLongCompareAndBranch(IfCondition cond,
                                       bool is64bit,
                                       LocationSummary* locations,
                                       Mips64Label* label);
  void GenerateFpCompareAndBranch(IfCondition cond,
                                  bool gt_bias,
                                  Primitive::Type type,
                                  LocationSummary* locations,
                                  Mips64Label* label);
  void HandleGoto(HInstruction* got, HBasicBlock* successor);

  Mips64Assembler* const assembler_;
  CodeGeneratorMIPS64* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(InstructionCodeGeneratorMIPS64);
};

class CodeGeneratorMIPS64 : public CodeGenerator {
 public:
  CodeGeneratorMIPS64(HGraph* graph,
                      const Mips64InstructionSetFeatures& isa_features,
                      const CompilerOptions& compiler_options,
                      OptimizingCompilerStats* stats = nullptr);
  virtual ~CodeGeneratorMIPS64() {}

  void GenerateFrameEntry() OVERRIDE;
  void GenerateFrameExit() OVERRIDE;

  void Bind(HBasicBlock* block) OVERRIDE;

  size_t GetWordSize() const OVERRIDE { return kMips64DoublewordSize; }

  size_t GetFloatingPointSpillSlotSize() const OVERRIDE { return kMips64DoublewordSize; }

  uintptr_t GetAddressOf(HBasicBlock* block) OVERRIDE {
    return assembler_.GetLabelLocation(GetLabelOf(block));
  }

  HGraphVisitor* GetLocationBuilder() OVERRIDE { return &location_builder_; }
  HGraphVisitor* GetInstructionVisitor() OVERRIDE { return &instruction_visitor_; }
  Mips64Assembler* GetAssembler() OVERRIDE { return &assembler_; }
  const Mips64Assembler& GetAssembler() const OVERRIDE { return assembler_; }

  void MarkGCCard(GpuRegister object, GpuRegister value, bool value_can_be_null);

  // Register allocation.

  void SetupBlockedRegisters() const OVERRIDE;

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

  Mips64Label* GetLabelOf(HBasicBlock* block) const {
    return CommonGetLabelOf<Mips64Label>(block_labels_, block);
  }

  void Initialize() OVERRIDE {
    block_labels_ = CommonInitializeLabels<Mips64Label>();
  }

  void Finalize(CodeAllocator* allocator) OVERRIDE;

  // Code generation helpers.
  void MoveLocation(Location dst, Location src, Primitive::Type dst_type) OVERRIDE;

  void MoveConstant(Location destination, int32_t value) OVERRIDE;

  void AddLocationAsTemp(Location location, LocationSummary* locations) OVERRIDE;


  void SwapLocations(Location loc1, Location loc2, Primitive::Type type);

  // Generate code to invoke a runtime entry point.
  void InvokeRuntime(QuickEntrypointEnum entrypoint,
                     HInstruction* instruction,
                     uint32_t dex_pc,
                     SlowPathCode* slow_path) OVERRIDE;

  void InvokeRuntime(int32_t offset,
                     HInstruction* instruction,
                     uint32_t dex_pc,
                     SlowPathCode* slow_path);

  ParallelMoveResolver* GetMoveResolver() OVERRIDE { return &move_resolver_; }

  bool NeedsTwoRegisters(Primitive::Type type ATTRIBUTE_UNUSED) const { return false; }

  // Check if the desired_string_load_kind is supported. If it is, return it,
  // otherwise return a fall-back kind that should be used instead.
  HLoadString::LoadKind GetSupportedLoadStringKind(
      HLoadString::LoadKind desired_string_load_kind) OVERRIDE;

  // Check if the desired_dispatch_info is supported. If it is, return it,
  // otherwise return a fall-back info that should be used instead.
  HInvokeStaticOrDirect::DispatchInfo GetSupportedInvokeStaticOrDirectDispatch(
      const HInvokeStaticOrDirect::DispatchInfo& desired_dispatch_info,
      MethodReference target_method) OVERRIDE;

  void GenerateStaticOrDirectCall(HInvokeStaticOrDirect* invoke, Location temp) OVERRIDE;
  void GenerateVirtualCall(HInvokeVirtual* invoke, Location temp) OVERRIDE;

  void MoveFromReturnRegister(Location trg ATTRIBUTE_UNUSED,
                              Primitive::Type type ATTRIBUTE_UNUSED) OVERRIDE {
    UNIMPLEMENTED(FATAL) << "Not implemented on MIPS64";
  }

  void GenerateNop();
  void GenerateImplicitNullCheck(HNullCheck* instruction);
  void GenerateExplicitNullCheck(HNullCheck* instruction);

 private:
  // Labels for each block that will be compiled.
  Mips64Label* block_labels_;  // Indexed by block id.
  Mips64Label frame_entry_label_;
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
