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

class InvokeDexCallingConvention : public CallingConvention<Register, FloatRegister> {
 public:
  InvokeDexCallingConvention() : CallingConvention(
      kParameterCoreRegisters,
      kParameterCoreRegistersLength,
      kParameterFloatRegisters,
      kParameterFloatRegistersLength) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConvention);
};

class InvokeDexCallingConventionVisitor {
 public:
  InvokeDexCallingConventionVisitor() : gp_index_(0), fp_index_(0), stack_index_(0) {}

  Location GetNextLocation(Primitive::Type type);

 private:
  InvokeDexCallingConvention calling_convention;
  // The current index for cpu registers.
  uint32_t gp_index_;
  // The current index for fpu registers.
  uint32_t fp_index_;
  // The current stack index.
  uint32_t stack_index_;

  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConventionVisitor);
};

class CodeGeneratorX86_64;
class SlowPathCodeX86_64;

class ParallelMoveResolverX86_64 : public ParallelMoveResolver {
 public:
  ParallelMoveResolverX86_64(ArenaAllocator* allocator, CodeGeneratorX86_64* codegen)
      : ParallelMoveResolver(allocator), codegen_(codegen) {}

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

  CodeGeneratorX86_64* const codegen_;
  InvokeDexCallingConventionVisitor parameter_visitor_;

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
  void GenerateDivRemIntegral(HBinaryOperation* instruction);
  void HandleShift(HBinaryOperation* operation);

  X86_64Assembler* const assembler_;
  CodeGeneratorX86_64* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(InstructionCodeGeneratorX86_64);
};

class CodeGeneratorX86_64 : public CodeGenerator {
 public:
  explicit CodeGeneratorX86_64(HGraph* graph);
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

  size_t FrameEntrySpillSize() const OVERRIDE;

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

  void SetupBlockedRegisters() const OVERRIDE;
  Location AllocateFreeRegister(Primitive::Type type) const OVERRIDE;
  void DumpCoreRegister(std::ostream& stream, int reg) const OVERRIDE;
  void DumpFloatingPointRegister(std::ostream& stream, int reg) const OVERRIDE;

  InstructionSet GetInstructionSet() const OVERRIDE {
    return InstructionSet::kX86_64;
  }

  // Emit a write barrier.
  void MarkGCCard(CpuRegister temp, CpuRegister card, CpuRegister object, CpuRegister value);

  // Helper method to move a value between two locations.
  void Move(Location destination, Location source);

  void LoadCurrentMethod(CpuRegister reg);

  Label* GetLabelOf(HBasicBlock* block) const {
    return block_labels_.GetRawStorage() + block->GetBlockId();
  }

  void Initialize() OVERRIDE {
    block_labels_.SetSize(GetGraph()->GetBlocks().Size());
  }

 private:
  // Labels for each block that will be compiled.
  GrowableArray<Label> block_labels_;
  LocationsBuilderX86_64 location_builder_;
  InstructionCodeGeneratorX86_64 instruction_visitor_;
  ParallelMoveResolverX86_64 move_resolver_;
  X86_64Assembler assembler_;

  DISALLOW_COPY_AND_ASSIGN(CodeGeneratorX86_64);
};

}  // namespace x86_64
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_X86_64_H_
