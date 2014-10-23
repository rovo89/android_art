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

#ifndef ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM64_H_
#define ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM64_H_

#include "code_generator.h"
#include "nodes.h"
#include "parallel_move_resolver.h"
#include "utils/arm64/assembler_arm64.h"
#include "a64/disasm-a64.h"
#include "a64/macro-assembler-a64.h"
#include "arch/arm64/quick_method_frame_info_arm64.h"

namespace art {
namespace arm64 {

class CodeGeneratorARM64;

static constexpr size_t kArm64WordSize = 8;
static const vixl::Register kParameterCoreRegisters[] = {
  vixl::x1, vixl::x2, vixl::x3, vixl::x4, vixl::x5, vixl::x6, vixl::x7
};
static constexpr size_t kParameterCoreRegistersLength = arraysize(kParameterCoreRegisters);
static const vixl::FPRegister kParameterFPRegisters[] = {
  vixl::d0, vixl::d1, vixl::d2, vixl::d3, vixl::d4, vixl::d5, vixl::d6, vixl::d7
};
static constexpr size_t kParameterFPRegistersLength = arraysize(kParameterFPRegisters);

const vixl::Register tr = vixl::x18;        // Thread Register
const vixl::Register wSuspend = vixl::w19;  // Suspend Register
const vixl::Register xSuspend = vixl::x19;

const vixl::CPURegList vixl_reserved_core_registers(vixl::ip0, vixl::ip1);
const vixl::CPURegList runtime_reserved_core_registers(tr, xSuspend, vixl::lr);
const vixl::CPURegList quick_callee_saved_registers(vixl::CPURegister::kRegister,
                                                    vixl::kXRegSize,
                                                    kArm64CalleeSaveRefSpills);

class InvokeDexCallingConvention : public CallingConvention<vixl::Register, vixl::FPRegister> {
 public:
  InvokeDexCallingConvention()
      : CallingConvention(kParameterCoreRegisters,
                          kParameterCoreRegistersLength,
                          kParameterFPRegisters,
                          kParameterFPRegistersLength) {}

  Location GetReturnLocation(Primitive::Type return_type) {
    DCHECK_NE(return_type, Primitive::kPrimVoid);
    if (return_type == Primitive::kPrimFloat || return_type == Primitive::kPrimDouble) {
      LOG(FATAL) << "Unimplemented return type " << return_type;
    }
    return Location::RegisterLocation(X0);
  }


 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConvention);
};

class InvokeDexCallingConventionVisitor {
 public:
  InvokeDexCallingConventionVisitor() : gp_index_(0), stack_index_(0) {}

  Location GetNextLocation(Primitive::Type type);
  Location GetReturnLocation(Primitive::Type return_type) {
    return calling_convention.GetReturnLocation(return_type);
  }

 private:
  InvokeDexCallingConvention calling_convention;
  // The current index for core registers.
  uint32_t gp_index_;
  // The current stack index.
  uint32_t stack_index_;

  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConventionVisitor);
};

class InstructionCodeGeneratorARM64 : public HGraphVisitor {
 public:
  InstructionCodeGeneratorARM64(HGraph* graph, CodeGeneratorARM64* codegen);

#define DECLARE_VISIT_INSTRUCTION(name, super) \
  virtual void Visit##name(H##name* instr);
  FOR_EACH_CONCRETE_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)
#undef DECLARE_VISIT_INSTRUCTION

  void LoadCurrentMethod(XRegister reg);

  Arm64Assembler* GetAssembler() const { return assembler_; }

 private:
  void HandleAddSub(HBinaryOperation* instr);

  Arm64Assembler* const assembler_;
  CodeGeneratorARM64* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(InstructionCodeGeneratorARM64);
};

class LocationsBuilderARM64 : public HGraphVisitor {
 public:
  explicit LocationsBuilderARM64(HGraph* graph, CodeGeneratorARM64* codegen)
      : HGraphVisitor(graph), codegen_(codegen) {}

#define DECLARE_VISIT_INSTRUCTION(name, super) \
  virtual void Visit##name(H##name* instr);
  FOR_EACH_CONCRETE_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)
#undef DECLARE_VISIT_INSTRUCTION

 private:
  void HandleAddSub(HBinaryOperation* instr);
  void HandleInvoke(HInvoke* instr);

  CodeGeneratorARM64* const codegen_;
  InvokeDexCallingConventionVisitor parameter_visitor_;

  DISALLOW_COPY_AND_ASSIGN(LocationsBuilderARM64);
};

class CodeGeneratorARM64 : public CodeGenerator {
 public:
  explicit CodeGeneratorARM64(HGraph* graph);
  virtual ~CodeGeneratorARM64() { }

  virtual void GenerateFrameEntry() OVERRIDE;
  virtual void GenerateFrameExit() OVERRIDE;

  static const vixl::CPURegList& GetFramePreservedRegisters() {
    static const vixl::CPURegList frame_preserved_regs =
        vixl::CPURegList(vixl::CPURegister::kRegister, vixl::kXRegSize, vixl::lr.Bit());
    return frame_preserved_regs;
  }
  static int GetFramePreservedRegistersSize() {
    return GetFramePreservedRegisters().TotalSizeInBytes();
  }

  virtual void Bind(HBasicBlock* block) OVERRIDE;

  vixl::Label* GetLabelOf(HBasicBlock* block) const {
    return block_labels_ + block->GetBlockId();
  }

  virtual void Move(HInstruction* instruction, Location location, HInstruction* move_for) OVERRIDE;

  virtual size_t GetWordSize() const OVERRIDE {
    return kArm64WordSize;
  }

  virtual size_t FrameEntrySpillSize() const OVERRIDE;

  virtual HGraphVisitor* GetLocationBuilder() OVERRIDE { return &location_builder_; }
  virtual HGraphVisitor* GetInstructionVisitor() OVERRIDE { return &instruction_visitor_; }
  virtual Arm64Assembler* GetAssembler() OVERRIDE { return &assembler_; }

  // Emit a write barrier.
  void MarkGCCard(vixl::Register object, vixl::Register value);

  // Register allocation.

  virtual void SetupBlockedRegisters() const OVERRIDE;
  // AllocateFreeRegister() is only used when allocating registers locally
  // during CompileBaseline().
  virtual Location AllocateFreeRegister(Primitive::Type type) const OVERRIDE;

  virtual Location GetStackLocation(HLoadLocal* load) const OVERRIDE;

  virtual size_t SaveCoreRegister(size_t stack_index, uint32_t reg_id) OVERRIDE {
    UNIMPLEMENTED(INFO) << "TODO: SaveCoreRegister";
    return 0;
  }

  virtual size_t RestoreCoreRegister(size_t stack_index, uint32_t reg_id) OVERRIDE {
    UNIMPLEMENTED(INFO) << "TODO: RestoreCoreRegister";
    return 0;
  }

  // The number of registers that can be allocated. The register allocator may
  // decide to reserve and not use a few of them.
  // We do not consider registers sp, xzr, wzr. They are either not allocatable
  // (xzr, wzr), or make for poor allocatable registers (sp alignment
  // requirements, etc.). This also facilitates our task as all other registers
  // can easily be mapped via to or from their type and index or code.
  static const int kNumberOfAllocatableCoreRegisters = vixl::kNumberOfRegisters - 1;
  static const int kNumberOfAllocatableFloatingPointRegisters = vixl::kNumberOfFPRegisters;
  static const int kNumberOfAllocatableRegisters =
      kNumberOfAllocatableCoreRegisters + kNumberOfAllocatableFloatingPointRegisters;
  static constexpr int kNumberOfAllocatableRegisterPairs = 0;

  virtual void DumpCoreRegister(std::ostream& stream, int reg) const OVERRIDE;
  virtual void DumpFloatingPointRegister(std::ostream& stream, int reg) const OVERRIDE;

  virtual InstructionSet GetInstructionSet() const OVERRIDE {
    return InstructionSet::kArm64;
  }

  void MoveHelper(Location destination, Location source, Primitive::Type type);

  virtual void Initialize() OVERRIDE {
    HGraph* graph = GetGraph();
    int length = graph->GetBlocks().Size();
    block_labels_ = graph->GetArena()->AllocArray<vixl::Label>(length);
    for (int i = 0; i < length; ++i) {
      new(block_labels_ + i) vixl::Label();
    }
  }

 private:
  // Labels for each block that will be compiled.
  vixl::Label* block_labels_;

  LocationsBuilderARM64 location_builder_;
  InstructionCodeGeneratorARM64 instruction_visitor_;
  Arm64Assembler assembler_;

  DISALLOW_COPY_AND_ASSIGN(CodeGeneratorARM64);
};

}  // namespace arm64
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM64_H_
