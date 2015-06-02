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

#ifndef ART_COMPILER_OPTIMIZING_CODE_GENERATOR_X86_64_H_
#define ART_COMPILER_OPTIMIZING_CODE_GENERATOR_X86_64_H_

#include "code_generator.h"
#include "dex/compiler_enums.h"
#include "driver/compiler_options.h"
#include "nodes.h"
#include "parallel_move_resolver.h"
#include "utils/x86_64/assembler_x86_64.h"

namespace art {
namespace x86_64 {

// Use a local definition to prevent copying mistakes.
static constexpr size_t kX86_64WordSize = kX86_64PointerSize;

static constexpr Register kParameterCoreRegisters[] = { RSI, RDX, RCX, R8, R9 };
static constexpr FloatRegister kParameterFloatRegisters[] =
    { XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7 };

static constexpr size_t kParameterCoreRegistersLength = arraysize(kParameterCoreRegisters);
static constexpr size_t kParameterFloatRegistersLength = arraysize(kParameterFloatRegisters);

static constexpr Register kRuntimeParameterCoreRegisters[] = { RDI, RSI, RDX, RCX };
static constexpr size_t kRuntimeParameterCoreRegistersLength =
    arraysize(kRuntimeParameterCoreRegisters);
static constexpr FloatRegister kRuntimeParameterFpuRegisters[] = { XMM0, XMM1 };
static constexpr size_t kRuntimeParameterFpuRegistersLength =
    arraysize(kRuntimeParameterFpuRegisters);

class InvokeRuntimeCallingConvention : public CallingConvention<Register, FloatRegister> {
 public:
  InvokeRuntimeCallingConvention()
      : CallingConvention(kRuntimeParameterCoreRegisters,
                          kRuntimeParameterCoreRegistersLength,
                          kRuntimeParameterFpuRegisters,
                          kRuntimeParameterFpuRegistersLength,
                          kX86_64PointerSize) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeRuntimeCallingConvention);
};

class InvokeDexCallingConvention : public CallingConvention<Register, FloatRegister> {
 public:
  InvokeDexCallingConvention() : CallingConvention(
      kParameterCoreRegisters,
      kParameterCoreRegistersLength,
      kParameterFloatRegisters,
      kParameterFloatRegistersLength,
      kX86_64PointerSize) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConvention);
};

class InvokeDexCallingConventionVisitorX86_64 : public InvokeDexCallingConventionVisitor {
 public:
  InvokeDexCallingConventionVisitorX86_64() {}
  virtual ~InvokeDexCallingConventionVisitorX86_64() {}

  Location GetNextLocation(Primitive::Type type) OVERRIDE;

 private:
  InvokeDexCallingConvention calling_convention;

  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConventionVisitorX86_64);
};

class CodeGeneratorX86_64;

class SlowPathCodeX86_64 : public SlowPathCode {
 public:
  SlowPathCodeX86_64() : entry_label_(), exit_label_() {}

  Label* GetEntryLabel() { return &entry_label_; }
  Label* GetExitLabel() { return &exit_label_; }

 private:
  Label entry_label_;
  Label exit_label_;

  DISALLOW_COPY_AND_ASSIGN(SlowPathCodeX86_64);
};

class ParallelMoveResolverX86_64 : public ParallelMoveResolverWithSwap {
 public:
  ParallelMoveResolverX86_64(ArenaAllocator* allocator, CodeGeneratorX86_64* codegen)
      : ParallelMoveResolverWithSwap(allocator), codegen_(codegen) {}

  void EmitMove(size_t index) OVERRIDE;
  void EmitSwap(size_t index) OVERRIDE;
  void SpillScratch(int reg) OVERRIDE;
  void RestoreScratch(int reg) OVERRIDE;

  X86_64Assembler* GetAssembler() const;

 private:
  void Exchange32(CpuRegister reg, int mem);
  void Exchange32(XmmRegister reg, int mem);
  void Exchange32(int mem1, int mem2);
  void Exchange64(CpuRegister reg, int mem);
  void Exchange64(XmmRegister reg, int mem);
  void Exchange64(int mem1, int mem2);

  CodeGeneratorX86_64* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(ParallelMoveResolverX86_64);
};

class LocationsBuilderX86_64 : public HGraphVisitor {
 public:
  LocationsBuilderX86_64(HGraph* graph, CodeGeneratorX86_64* codegen)
      : HGraphVisitor(graph), codegen_(codegen) {}

#define DECLARE_VISIT_INSTRUCTION(name, super)     \
  void Visit##name(H##name* instr) OVERRIDE;

  FOR_EACH_CONCRETE_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

 private:
  void HandleInvoke(HInvoke* invoke);
  void HandleBitwiseOperation(HBinaryOperation* operation);
  void HandleShift(HBinaryOperation* operation);
  void HandleFieldSet(HInstruction* instruction, const FieldInfo& field_info);
  void HandleFieldGet(HInstruction* instruction);

  CodeGeneratorX86_64* const codegen_;
  InvokeDexCallingConventionVisitorX86_64 parameter_visitor_;

  DISALLOW_COPY_AND_ASSIGN(LocationsBuilderX86_64);
};

class InstructionCodeGeneratorX86_64 : public HGraphVisitor {
 public:
  InstructionCodeGeneratorX86_64(HGraph* graph, CodeGeneratorX86_64* codegen);

#define DECLARE_VISIT_INSTRUCTION(name, super)     \
  void Visit##name(H##name* instr) OVERRIDE;

  FOR_EACH_CONCRETE_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  X86_64Assembler* GetAssembler() const { return assembler_; }

 private:
  // Generate code for the given suspend check. If not null, `successor`
  // is the block to branch to if the suspend check is not needed, and after
  // the suspend call.
  void GenerateSuspendCheck(HSuspendCheck* instruction, HBasicBlock* successor);
  void GenerateClassInitializationCheck(SlowPathCodeX86_64* slow_path, CpuRegister class_reg);
  void HandleBitwiseOperation(HBinaryOperation* operation);
  void GenerateRemFP(HRem *rem);
  void DivRemOneOrMinusOne(HBinaryOperation* instruction);
  void DivByPowerOfTwo(HDiv* instruction);
  void GenerateDivRemWithAnyConstant(HBinaryOperation* instruction);
  void GenerateDivRemIntegral(HBinaryOperation* instruction);
  void HandleShift(HBinaryOperation* operation);
  void GenerateMemoryBarrier(MemBarrierKind kind);
  void HandleFieldSet(HInstruction* instruction, const FieldInfo& field_info);
  void HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info);
  void GenerateImplicitNullCheck(HNullCheck* instruction);
  void GenerateExplicitNullCheck(HNullCheck* instruction);
  void PushOntoFPStack(Location source, uint32_t temp_offset,
                       uint32_t stack_adjustment, bool is_float);
  void GenerateTestAndBranch(HInstruction* instruction,
                             Label* true_target,
                             Label* false_target,
                             Label* always_true_target);

  X86_64Assembler* const assembler_;
  CodeGeneratorX86_64* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(InstructionCodeGeneratorX86_64);
};

class CodeGeneratorX86_64 : public CodeGenerator {
 public:
  CodeGeneratorX86_64(HGraph* graph,
                  const X86_64InstructionSetFeatures& isa_features,
                  const CompilerOptions& compiler_options);
  virtual ~CodeGeneratorX86_64() {}

  void GenerateFrameEntry() OVERRIDE;
  void GenerateFrameExit() OVERRIDE;
  void Bind(HBasicBlock* block) OVERRIDE;
  void Move(HInstruction* instruction, Location location, HInstruction* move_for) OVERRIDE;
  size_t SaveCoreRegister(size_t stack_index, uint32_t reg_id) OVERRIDE;
  size_t RestoreCoreRegister(size_t stack_index, uint32_t reg_id) OVERRIDE;
  size_t SaveFloatingPointRegister(size_t stack_index, uint32_t reg_id) OVERRIDE;
  size_t RestoreFloatingPointRegister(size_t stack_index, uint32_t reg_id) OVERRIDE;

  size_t GetWordSize() const OVERRIDE {
    return kX86_64WordSize;
  }

  size_t GetFloatingPointSpillSlotSize() const OVERRIDE {
    return kX86_64WordSize;
  }

  HGraphVisitor* GetLocationBuilder() OVERRIDE {
    return &location_builder_;
  }

  HGraphVisitor* GetInstructionVisitor() OVERRIDE {
    return &instruction_visitor_;
  }

  X86_64Assembler* GetAssembler() OVERRIDE {
    return &assembler_;
  }

  ParallelMoveResolverX86_64* GetMoveResolver() OVERRIDE {
    return &move_resolver_;
  }

  uintptr_t GetAddressOf(HBasicBlock* block) const OVERRIDE {
    return GetLabelOf(block)->Position();
  }

  Location GetStackLocation(HLoadLocal* load) const OVERRIDE;

  void SetupBlockedRegisters(bool is_baseline) const OVERRIDE;
  Location AllocateFreeRegister(Primitive::Type type) const OVERRIDE;
  void DumpCoreRegister(std::ostream& stream, int reg) const OVERRIDE;
  void DumpFloatingPointRegister(std::ostream& stream, int reg) const OVERRIDE;
  void Finalize(CodeAllocator* allocator) OVERRIDE;

  InstructionSet GetInstructionSet() const OVERRIDE {
    return InstructionSet::kX86_64;
  }

  // Emit a write barrier.
  void MarkGCCard(CpuRegister temp, CpuRegister card, CpuRegister object, CpuRegister value);

  // Helper method to move a value between two locations.
  void Move(Location destination, Location source);

  void LoadCurrentMethod(CpuRegister reg);

  Label* GetLabelOf(HBasicBlock* block) const {
    return CommonGetLabelOf<Label>(block_labels_.GetRawStorage(), block);
  }

  void Initialize() OVERRIDE {
    block_labels_.SetSize(GetGraph()->GetBlocks().Size());
  }

  bool NeedsTwoRegisters(Primitive::Type type ATTRIBUTE_UNUSED) const OVERRIDE {
    return false;
  }

  void GenerateStaticOrDirectCall(HInvokeStaticOrDirect* invoke, CpuRegister temp);

  const X86_64InstructionSetFeatures& GetInstructionSetFeatures() const {
    return isa_features_;
  }

  int ConstantAreaStart() const {
    return constant_area_start_;
  }

  Address LiteralDoubleAddress(double v);
  Address LiteralFloatAddress(float v);
  Address LiteralInt32Address(int32_t v);
  Address LiteralInt64Address(int64_t v);

  // Load a 64 bit value into a register in the most efficient manner.
  void Load64BitValue(CpuRegister dest, int64_t value);

 private:
  // Labels for each block that will be compiled.
  GrowableArray<Label> block_labels_;
  Label frame_entry_label_;
  LocationsBuilderX86_64 location_builder_;
  InstructionCodeGeneratorX86_64 instruction_visitor_;
  ParallelMoveResolverX86_64 move_resolver_;
  X86_64Assembler assembler_;
  const X86_64InstructionSetFeatures& isa_features_;

  // Offset to the start of the constant area in the assembled code.
  // Used for fixups to the constant area.
  int constant_area_start_;

  DISALLOW_COPY_AND_ASSIGN(CodeGeneratorX86_64);
};

}  // namespace x86_64
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_X86_64_H_
