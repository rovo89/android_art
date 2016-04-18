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

#ifndef ART_COMPILER_OPTIMIZING_CODE_GENERATOR_MIPS_H_
#define ART_COMPILER_OPTIMIZING_CODE_GENERATOR_MIPS_H_

#include "code_generator.h"
#include "dex/compiler_enums.h"
#include "driver/compiler_options.h"
#include "nodes.h"
#include "parallel_move_resolver.h"
#include "utils/mips/assembler_mips.h"

namespace art {
namespace mips {

// InvokeDexCallingConvention registers

static constexpr Register kParameterCoreRegisters[] =
    { A1, A2, A3 };
static constexpr size_t kParameterCoreRegistersLength = arraysize(kParameterCoreRegisters);

static constexpr FRegister kParameterFpuRegisters[] =
    { F12, F14 };
static constexpr size_t kParameterFpuRegistersLength = arraysize(kParameterFpuRegisters);


// InvokeRuntimeCallingConvention registers

static constexpr Register kRuntimeParameterCoreRegisters[] =
    { A0, A1, A2, A3 };
static constexpr size_t kRuntimeParameterCoreRegistersLength =
    arraysize(kRuntimeParameterCoreRegisters);

static constexpr FRegister kRuntimeParameterFpuRegisters[] =
    { F12, F14};
static constexpr size_t kRuntimeParameterFpuRegistersLength =
    arraysize(kRuntimeParameterFpuRegisters);


static constexpr Register kCoreCalleeSaves[] =
    { S0, S1, S2, S3, S4, S5, S6, S7, FP, RA };
static constexpr FRegister kFpuCalleeSaves[] =
    { F20, F22, F24, F26, F28, F30 };


class CodeGeneratorMIPS;

class InvokeDexCallingConvention : public CallingConvention<Register, FRegister> {
 public:
  InvokeDexCallingConvention()
      : CallingConvention(kParameterCoreRegisters,
                          kParameterCoreRegistersLength,
                          kParameterFpuRegisters,
                          kParameterFpuRegistersLength,
                          kMipsPointerSize) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConvention);
};

class InvokeDexCallingConventionVisitorMIPS : public InvokeDexCallingConventionVisitor {
 public:
  InvokeDexCallingConventionVisitorMIPS() {}
  virtual ~InvokeDexCallingConventionVisitorMIPS() {}

  Location GetNextLocation(Primitive::Type type) OVERRIDE;
  Location GetReturnLocation(Primitive::Type type) const OVERRIDE;
  Location GetMethodLocation() const OVERRIDE;

 private:
  InvokeDexCallingConvention calling_convention;

  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConventionVisitorMIPS);
};

class InvokeRuntimeCallingConvention : public CallingConvention<Register, FRegister> {
 public:
  InvokeRuntimeCallingConvention()
      : CallingConvention(kRuntimeParameterCoreRegisters,
                          kRuntimeParameterCoreRegistersLength,
                          kRuntimeParameterFpuRegisters,
                          kRuntimeParameterFpuRegistersLength,
                          kMipsPointerSize) {}

  Location GetReturnLocation(Primitive::Type return_type);

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeRuntimeCallingConvention);
};

class FieldAccessCallingConventionMIPS : public FieldAccessCallingConvention {
 public:
  FieldAccessCallingConventionMIPS() {}

  Location GetObjectLocation() const OVERRIDE {
    return Location::RegisterLocation(A1);
  }
  Location GetFieldIndexLocation() const OVERRIDE {
    return Location::RegisterLocation(A0);
  }
  Location GetReturnLocation(Primitive::Type type) const OVERRIDE {
    return Primitive::Is64BitType(type)
        ? Location::RegisterPairLocation(V0, V1)
        : Location::RegisterLocation(V0);
  }
  Location GetSetValueLocation(Primitive::Type type, bool is_instance) const OVERRIDE {
    return Primitive::Is64BitType(type)
        ? Location::RegisterPairLocation(A2, A3)
        : (is_instance ? Location::RegisterLocation(A2) : Location::RegisterLocation(A1));
  }
  Location GetFpuLocation(Primitive::Type type ATTRIBUTE_UNUSED) const OVERRIDE {
    return Location::FpuRegisterLocation(F0);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(FieldAccessCallingConventionMIPS);
};

class ParallelMoveResolverMIPS : public ParallelMoveResolverWithSwap {
 public:
  ParallelMoveResolverMIPS(ArenaAllocator* allocator, CodeGeneratorMIPS* codegen)
      : ParallelMoveResolverWithSwap(allocator), codegen_(codegen) {}

  void EmitMove(size_t index) OVERRIDE;
  void EmitSwap(size_t index) OVERRIDE;
  void SpillScratch(int reg) OVERRIDE;
  void RestoreScratch(int reg) OVERRIDE;

  void Exchange(int index1, int index2, bool double_slot);

  MipsAssembler* GetAssembler() const;

 private:
  CodeGeneratorMIPS* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(ParallelMoveResolverMIPS);
};

class SlowPathCodeMIPS : public SlowPathCode {
 public:
  explicit SlowPathCodeMIPS(HInstruction* instruction)
      : SlowPathCode(instruction), entry_label_(), exit_label_() {}

  MipsLabel* GetEntryLabel() { return &entry_label_; }
  MipsLabel* GetExitLabel() { return &exit_label_; }

 private:
  MipsLabel entry_label_;
  MipsLabel exit_label_;

  DISALLOW_COPY_AND_ASSIGN(SlowPathCodeMIPS);
};

class LocationsBuilderMIPS : public HGraphVisitor {
 public:
  LocationsBuilderMIPS(HGraph* graph, CodeGeneratorMIPS* codegen)
      : HGraphVisitor(graph), codegen_(codegen) {}

#define DECLARE_VISIT_INSTRUCTION(name, super)     \
  void Visit##name(H##name* instr) OVERRIDE;

  FOR_EACH_CONCRETE_INSTRUCTION_COMMON(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_MIPS(DECLARE_VISIT_INSTRUCTION)

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

  InvokeDexCallingConventionVisitorMIPS parameter_visitor_;

  CodeGeneratorMIPS* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(LocationsBuilderMIPS);
};

class InstructionCodeGeneratorMIPS : public InstructionCodeGenerator {
 public:
  InstructionCodeGeneratorMIPS(HGraph* graph, CodeGeneratorMIPS* codegen);

#define DECLARE_VISIT_INSTRUCTION(name, super)     \
  void Visit##name(H##name* instr) OVERRIDE;

  FOR_EACH_CONCRETE_INSTRUCTION_COMMON(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_MIPS(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  void VisitInstruction(HInstruction* instruction) OVERRIDE {
    LOG(FATAL) << "Unreachable instruction " << instruction->DebugName()
               << " (id " << instruction->GetId() << ")";
  }

  MipsAssembler* GetAssembler() const { return assembler_; }

 private:
  void GenerateClassInitializationCheck(SlowPathCodeMIPS* slow_path, Register class_reg);
  void GenerateMemoryBarrier(MemBarrierKind kind);
  void GenerateSuspendCheck(HSuspendCheck* check, HBasicBlock* successor);
  void HandleBinaryOp(HBinaryOperation* operation);
  void HandleCondition(HCondition* instruction);
  void HandleShift(HBinaryOperation* operation);
  void HandleFieldSet(HInstruction* instruction, const FieldInfo& field_info, uint32_t dex_pc);
  void HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info, uint32_t dex_pc);
  void GenerateIntCompare(IfCondition cond, LocationSummary* locations);
  void GenerateIntCompareAndBranch(IfCondition cond,
                                   LocationSummary* locations,
                                   MipsLabel* label);
  void GenerateLongCompareAndBranch(IfCondition cond,
                                    LocationSummary* locations,
                                    MipsLabel* label);
  void GenerateFpCompareAndBranch(IfCondition cond,
                                  bool gt_bias,
                                  Primitive::Type type,
                                  LocationSummary* locations,
                                  MipsLabel* label);
  void GenerateTestAndBranch(HInstruction* instruction,
                             size_t condition_input_index,
                             MipsLabel* true_target,
                             MipsLabel* false_target);
  void DivRemOneOrMinusOne(HBinaryOperation* instruction);
  void DivRemByPowerOfTwo(HBinaryOperation* instruction);
  void GenerateDivRemWithAnyConstant(HBinaryOperation* instruction);
  void GenerateDivRemIntegral(HBinaryOperation* instruction);
  void HandleGoto(HInstruction* got, HBasicBlock* successor);

  MipsAssembler* const assembler_;
  CodeGeneratorMIPS* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(InstructionCodeGeneratorMIPS);
};

class CodeGeneratorMIPS : public CodeGenerator {
 public:
  CodeGeneratorMIPS(HGraph* graph,
                    const MipsInstructionSetFeatures& isa_features,
                    const CompilerOptions& compiler_options,
                    OptimizingCompilerStats* stats = nullptr);
  virtual ~CodeGeneratorMIPS() {}

  void GenerateFrameEntry() OVERRIDE;
  void GenerateFrameExit() OVERRIDE;

  void Bind(HBasicBlock* block) OVERRIDE;

  void Move32(Location destination, Location source);
  void Move64(Location destination, Location source);
  void MoveConstant(Location location, HConstant* c);

  size_t GetWordSize() const OVERRIDE { return kMipsWordSize; }

  size_t GetFloatingPointSpillSlotSize() const OVERRIDE { return kMipsDoublewordSize; }

  uintptr_t GetAddressOf(HBasicBlock* block) OVERRIDE {
    return assembler_.GetLabelLocation(GetLabelOf(block));
  }

  HGraphVisitor* GetLocationBuilder() OVERRIDE { return &location_builder_; }
  HGraphVisitor* GetInstructionVisitor() OVERRIDE { return &instruction_visitor_; }
  MipsAssembler* GetAssembler() OVERRIDE { return &assembler_; }
  const MipsAssembler& GetAssembler() const OVERRIDE { return assembler_; }

  void MarkGCCard(Register object, Register value);

  // Register allocation.

  void SetupBlockedRegisters() const OVERRIDE;

  size_t SaveCoreRegister(size_t stack_index, uint32_t reg_id);
  size_t RestoreCoreRegister(size_t stack_index, uint32_t reg_id);
  size_t SaveFloatingPointRegister(size_t stack_index, uint32_t reg_id);
  size_t RestoreFloatingPointRegister(size_t stack_index, uint32_t reg_id);

  void DumpCoreRegister(std::ostream& stream, int reg) const OVERRIDE;
  void DumpFloatingPointRegister(std::ostream& stream, int reg) const OVERRIDE;

  // Blocks all register pairs made out of blocked core registers.
  void UpdateBlockedPairRegisters() const;

  InstructionSet GetInstructionSet() const OVERRIDE { return InstructionSet::kMips; }

  const MipsInstructionSetFeatures& GetInstructionSetFeatures() const {
    return isa_features_;
  }

  MipsLabel* GetLabelOf(HBasicBlock* block) const {
    return CommonGetLabelOf<MipsLabel>(block_labels_, block);
  }

  void Initialize() OVERRIDE {
    block_labels_ = CommonInitializeLabels<MipsLabel>();
  }

  void Finalize(CodeAllocator* allocator) OVERRIDE;

  // Code generation helpers.

  void MoveLocation(Location dst, Location src, Primitive::Type dst_type) OVERRIDE;

  void MoveConstant(Location destination, int32_t value);

  void AddLocationAsTemp(Location location, LocationSummary* locations) OVERRIDE;

  // Generate code to invoke a runtime entry point.
  void InvokeRuntime(QuickEntrypointEnum entrypoint,
                     HInstruction* instruction,
                     uint32_t dex_pc,
                     SlowPathCode* slow_path) OVERRIDE;

  void InvokeRuntime(int32_t offset,
                     HInstruction* instruction,
                     uint32_t dex_pc,
                     SlowPathCode* slow_path,
                     bool is_direct_entrypoint);

  ParallelMoveResolver* GetMoveResolver() OVERRIDE { return &move_resolver_; }

  bool NeedsTwoRegisters(Primitive::Type type) const {
    return type == Primitive::kPrimLong;
  }

  // Check if the desired_string_load_kind is supported. If it is, return it,
  // otherwise return a fall-back kind that should be used instead.
  HLoadString::LoadKind GetSupportedLoadStringKind(
      HLoadString::LoadKind desired_string_load_kind) OVERRIDE;

  // Check if the desired_dispatch_info is supported. If it is, return it,
  // otherwise return a fall-back info that should be used instead.
  HInvokeStaticOrDirect::DispatchInfo GetSupportedInvokeStaticOrDirectDispatch(
      const HInvokeStaticOrDirect::DispatchInfo& desired_dispatch_info,
      MethodReference target_method) OVERRIDE;

  void GenerateStaticOrDirectCall(HInvokeStaticOrDirect* invoke, Location temp);
  void GenerateVirtualCall(HInvokeVirtual* invoke, Location temp) OVERRIDE;

  void MoveFromReturnRegister(Location trg ATTRIBUTE_UNUSED,
                              Primitive::Type type ATTRIBUTE_UNUSED) OVERRIDE {
    UNIMPLEMENTED(FATAL) << "Not implemented on MIPS";
  }

  void GenerateNop();
  void GenerateImplicitNullCheck(HNullCheck* instruction);
  void GenerateExplicitNullCheck(HNullCheck* instruction);

 private:
  // Labels for each block that will be compiled.
  MipsLabel* block_labels_;
  MipsLabel frame_entry_label_;
  LocationsBuilderMIPS location_builder_;
  InstructionCodeGeneratorMIPS instruction_visitor_;
  ParallelMoveResolverMIPS move_resolver_;
  MipsAssembler assembler_;
  const MipsInstructionSetFeatures& isa_features_;

  DISALLOW_COPY_AND_ASSIGN(CodeGeneratorMIPS);
};

}  // namespace mips
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_MIPS_H_
