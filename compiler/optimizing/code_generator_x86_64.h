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

static constexpr size_t kX86_64WordSize = 8;

static constexpr Register kParameterCoreRegisters[] = { RSI, RDX, RCX, R8, R9 };

static constexpr size_t kParameterCoreRegistersLength = arraysize(kParameterCoreRegisters);

class InvokeDexCallingConvention : public CallingConvention<Register> {
 public:
  InvokeDexCallingConvention()
      : CallingConvention(kParameterCoreRegisters, kParameterCoreRegistersLength) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConvention);
};

class InvokeDexCallingConventionVisitor {
 public:
  InvokeDexCallingConventionVisitor() : gp_index_(0), stack_index_(0) {}

  Location GetNextLocation(Primitive::Type type);

 private:
  InvokeDexCallingConvention calling_convention;
  uint32_t gp_index_;
  uint32_t stack_index_;

  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConventionVisitor);
};

class CodeGeneratorX86_64;

class ParallelMoveResolverX86_64 : public ParallelMoveResolver {
 public:
  ParallelMoveResolverX86_64(ArenaAllocator* allocator, CodeGeneratorX86_64* codegen)
      : ParallelMoveResolver(allocator), codegen_(codegen) {}

  virtual void EmitMove(size_t index) OVERRIDE;
  virtual void EmitSwap(size_t index) OVERRIDE;
  virtual void SpillScratch(int reg) OVERRIDE;
  virtual void RestoreScratch(int reg) OVERRIDE;

  X86_64Assembler* GetAssembler() const;

 private:
  void Exchange32(CpuRegister reg, int mem);
  void Exchange32(int mem1, int mem2);
  void Exchange64(CpuRegister reg, int mem);
  void Exchange64(int mem1, int mem2);

  CodeGeneratorX86_64* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(ParallelMoveResolverX86_64);
};

class LocationsBuilderX86_64 : public HGraphVisitor {
 public:
  LocationsBuilderX86_64(HGraph* graph, CodeGeneratorX86_64* codegen)
      : HGraphVisitor(graph), codegen_(codegen) {}

#define DECLARE_VISIT_INSTRUCTION(name)     \
  virtual void Visit##name(H##name* instr);

  FOR_EACH_CONCRETE_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

 private:
  CodeGeneratorX86_64* const codegen_;
  InvokeDexCallingConventionVisitor parameter_visitor_;

  DISALLOW_COPY_AND_ASSIGN(LocationsBuilderX86_64);
};

class InstructionCodeGeneratorX86_64 : public HGraphVisitor {
 public:
  InstructionCodeGeneratorX86_64(HGraph* graph, CodeGeneratorX86_64* codegen);

#define DECLARE_VISIT_INSTRUCTION(name)     \
  virtual void Visit##name(H##name* instr);

  FOR_EACH_CONCRETE_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  void LoadCurrentMethod(CpuRegister reg);

  X86_64Assembler* GetAssembler() const { return assembler_; }

 private:
  X86_64Assembler* const assembler_;
  CodeGeneratorX86_64* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(InstructionCodeGeneratorX86_64);
};

class CodeGeneratorX86_64 : public CodeGenerator {
 public:
  explicit CodeGeneratorX86_64(HGraph* graph);
  virtual ~CodeGeneratorX86_64() {}

  virtual void GenerateFrameEntry() OVERRIDE;
  virtual void GenerateFrameExit() OVERRIDE;
  virtual void Bind(Label* label) OVERRIDE;
  virtual void Move(HInstruction* instruction, Location location, HInstruction* move_for) OVERRIDE;

  virtual size_t GetWordSize() const OVERRIDE {
    return kX86_64WordSize;
  }

  virtual size_t FrameEntrySpillSize() const OVERRIDE;

  virtual HGraphVisitor* GetLocationBuilder() OVERRIDE {
    return &location_builder_;
  }

  virtual HGraphVisitor* GetInstructionVisitor() OVERRIDE {
    return &instruction_visitor_;
  }

  virtual X86_64Assembler* GetAssembler() OVERRIDE {
    return &assembler_;
  }

  ParallelMoveResolverX86_64* GetMoveResolver() {
    return &move_resolver_;
  }

  virtual Location GetStackLocation(HLoadLocal* load) const OVERRIDE;

  virtual size_t GetNumberOfRegisters() const OVERRIDE {
    return kNumberOfRegIds;
  }

  virtual size_t GetNumberOfCoreRegisters() const OVERRIDE {
    return kNumberOfCpuRegisters;
  }

  virtual size_t GetNumberOfFloatingPointRegisters() const OVERRIDE {
    return kNumberOfFloatRegisters;
  }

  virtual void SetupBlockedRegisters(bool* blocked_registers) const OVERRIDE;
  virtual ManagedRegister AllocateFreeRegister(
      Primitive::Type type, bool* blocked_registers) const OVERRIDE;
  virtual void DumpCoreRegister(std::ostream& stream, int reg) const OVERRIDE;
  virtual void DumpFloatingPointRegister(std::ostream& stream, int reg) const OVERRIDE;

  virtual InstructionSet GetInstructionSet() const OVERRIDE {
    return InstructionSet::kX86_64;
  }

  // Emit a write barrier.
  void MarkGCCard(CpuRegister temp, CpuRegister card, CpuRegister object, CpuRegister value);

  // Helper method to move a value between two locations.
  void Move(Location destination, Location source);

 private:
  LocationsBuilderX86_64 location_builder_;
  InstructionCodeGeneratorX86_64 instruction_visitor_;
  ParallelMoveResolverX86_64 move_resolver_;
  X86_64Assembler assembler_;

  DISALLOW_COPY_AND_ASSIGN(CodeGeneratorX86_64);
};

}  // namespace x86_64
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_X86_64_H_
