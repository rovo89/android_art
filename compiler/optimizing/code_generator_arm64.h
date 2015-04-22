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
#include "dex/compiler_enums.h"
#include "driver/compiler_options.h"
#include "nodes.h"
#include "parallel_move_resolver.h"
#include "utils/arm64/assembler_arm64.h"
#include "vixl/a64/disasm-a64.h"
#include "vixl/a64/macro-assembler-a64.h"
#include "arch/arm64/quick_method_frame_info_arm64.h"

namespace art {
namespace arm64 {

class CodeGeneratorARM64;

// Use a local definition to prevent copying mistakes.
static constexpr size_t kArm64WordSize = kArm64PointerSize;

static const vixl::Register kParameterCoreRegisters[] = {
  vixl::x1, vixl::x2, vixl::x3, vixl::x4, vixl::x5, vixl::x6, vixl::x7
};
static constexpr size_t kParameterCoreRegistersLength = arraysize(kParameterCoreRegisters);
static const vixl::FPRegister kParameterFPRegisters[] = {
  vixl::d0, vixl::d1, vixl::d2, vixl::d3, vixl::d4, vixl::d5, vixl::d6, vixl::d7
};
static constexpr size_t kParameterFPRegistersLength = arraysize(kParameterFPRegisters);

const vixl::Register tr = vixl::x18;                        // Thread Register
static const vixl::Register kArtMethodRegister = vixl::x0;  // Method register on invoke.

const vixl::CPURegList vixl_reserved_core_registers(vixl::ip0, vixl::ip1);
const vixl::CPURegList vixl_reserved_fp_registers(vixl::d31);

const vixl::CPURegList runtime_reserved_core_registers(tr, vixl::lr);

// Callee-saved registers defined by AAPCS64.
const vixl::CPURegList callee_saved_core_registers(vixl::CPURegister::kRegister,
                                                   vixl::kXRegSize,
                                                   vixl::x19.code(),
                                                   vixl::x30.code());
const vixl::CPURegList callee_saved_fp_registers(vixl::CPURegister::kFPRegister,
                                                 vixl::kDRegSize,
                                                 vixl::d8.code(),
                                                 vixl::d15.code());
Location ARM64ReturnLocation(Primitive::Type return_type);

class SlowPathCodeARM64 : public SlowPathCode {
 public:
  SlowPathCodeARM64() : entry_label_(), exit_label_() {}

  vixl::Label* GetEntryLabel() { return &entry_label_; }
  vixl::Label* GetExitLabel() { return &exit_label_; }

 private:
  vixl::Label entry_label_;
  vixl::Label exit_label_;

  DISALLOW_COPY_AND_ASSIGN(SlowPathCodeARM64);
};

static const vixl::Register kRuntimeParameterCoreRegisters[] =
    { vixl::x0, vixl::x1, vixl::x2, vixl::x3, vixl::x4, vixl::x5, vixl::x6, vixl::x7 };
static constexpr size_t kRuntimeParameterCoreRegistersLength =
    arraysize(kRuntimeParameterCoreRegisters);
static const vixl::FPRegister kRuntimeParameterFpuRegisters[] =
    { vixl::d0, vixl::d1, vixl::d2, vixl::d3, vixl::d4, vixl::d5, vixl::d6, vixl::d7 };
static constexpr size_t kRuntimeParameterFpuRegistersLength =
    arraysize(kRuntimeParameterCoreRegisters);

class InvokeRuntimeCallingConvention : public CallingConvention<vixl::Register, vixl::FPRegister> {
 public:
  static constexpr size_t kParameterCoreRegistersLength = arraysize(kParameterCoreRegisters);

  InvokeRuntimeCallingConvention()
      : CallingConvention(kRuntimeParameterCoreRegisters,
                          kRuntimeParameterCoreRegistersLength,
                          kRuntimeParameterFpuRegisters,
                          kRuntimeParameterFpuRegistersLength,
                          kArm64PointerSize) {}

  Location GetReturnLocation(Primitive::Type return_type);

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeRuntimeCallingConvention);
};

class InvokeDexCallingConvention : public CallingConvention<vixl::Register, vixl::FPRegister> {
 public:
  InvokeDexCallingConvention()
      : CallingConvention(kParameterCoreRegisters,
                          kParameterCoreRegistersLength,
                          kParameterFPRegisters,
                          kParameterFPRegistersLength,
                          kArm64PointerSize) {}

  Location GetReturnLocation(Primitive::Type return_type) {
    return ARM64ReturnLocation(return_type);
  }


 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConvention);
};

class InvokeDexCallingConventionVisitorARM64 : public InvokeDexCallingConventionVisitor {
 public:
  InvokeDexCallingConventionVisitorARM64() {}
  virtual ~InvokeDexCallingConventionVisitorARM64() {}

  Location GetNextLocation(Primitive::Type type) OVERRIDE;
  Location GetReturnLocation(Primitive::Type return_type) {
    return calling_convention.GetReturnLocation(return_type);
  }

 private:
  InvokeDexCallingConvention calling_convention;

  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConventionVisitorARM64);
};

class InstructionCodeGeneratorARM64 : public HGraphVisitor {
 public:
  InstructionCodeGeneratorARM64(HGraph* graph, CodeGeneratorARM64* codegen);

#define DECLARE_VISIT_INSTRUCTION(name, super) \
  void Visit##name(H##name* instr) OVERRIDE;
  FOR_EACH_CONCRETE_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)
#undef DECLARE_VISIT_INSTRUCTION

  void LoadCurrentMethod(XRegister reg);

  Arm64Assembler* GetAssembler() const { return assembler_; }
  vixl::MacroAssembler* GetVIXLAssembler() { return GetAssembler()->vixl_masm_; }

 private:
  void GenerateClassInitializationCheck(SlowPathCodeARM64* slow_path, vixl::Register class_reg);
  void GenerateMemoryBarrier(MemBarrierKind kind);
  void GenerateSuspendCheck(HSuspendCheck* instruction, HBasicBlock* successor);
  void HandleBinaryOp(HBinaryOperation* instr);
  void HandleFieldSet(HInstruction* instruction, const FieldInfo& field_info);
  void HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info);
  void HandleShift(HBinaryOperation* instr);
  void GenerateImplicitNullCheck(HNullCheck* instruction);
  void GenerateExplicitNullCheck(HNullCheck* instruction);
  void GenerateTestAndBranch(HInstruction* instruction,
                             vixl::Label* true_target,
                             vixl::Label* false_target,
                             vixl::Label* always_true_target);

  Arm64Assembler* const assembler_;
  CodeGeneratorARM64* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(InstructionCodeGeneratorARM64);
};

class LocationsBuilderARM64 : public HGraphVisitor {
 public:
  explicit LocationsBuilderARM64(HGraph* graph, CodeGeneratorARM64* codegen)
      : HGraphVisitor(graph), codegen_(codegen) {}

#define DECLARE_VISIT_INSTRUCTION(name, super) \
  void Visit##name(H##name* instr) OVERRIDE;
  FOR_EACH_CONCRETE_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)
#undef DECLARE_VISIT_INSTRUCTION

 private:
  void HandleBinaryOp(HBinaryOperation* instr);
  void HandleFieldSet(HInstruction* instruction);
  void HandleFieldGet(HInstruction* instruction);
  void HandleInvoke(HInvoke* instr);
  void HandleShift(HBinaryOperation* instr);

  CodeGeneratorARM64* const codegen_;
  InvokeDexCallingConventionVisitorARM64 parameter_visitor_;

  DISALLOW_COPY_AND_ASSIGN(LocationsBuilderARM64);
};

class ParallelMoveResolverARM64 : public ParallelMoveResolverNoSwap {
 public:
  ParallelMoveResolverARM64(ArenaAllocator* allocator, CodeGeneratorARM64* codegen)
      : ParallelMoveResolverNoSwap(allocator), codegen_(codegen), vixl_temps_() {}

 protected:
  void PrepareForEmitNativeCode() OVERRIDE;
  void FinishEmitNativeCode() OVERRIDE;
  Location AllocateScratchLocationFor(Location::Kind kind) OVERRIDE;
  void FreeScratchLocation(Location loc) OVERRIDE;
  void EmitMove(size_t index) OVERRIDE;

 private:
  Arm64Assembler* GetAssembler() const;
  vixl::MacroAssembler* GetVIXLAssembler() const {
    return GetAssembler()->vixl_masm_;
  }

  CodeGeneratorARM64* const codegen_;
  vixl::UseScratchRegisterScope vixl_temps_;

  DISALLOW_COPY_AND_ASSIGN(ParallelMoveResolverARM64);
};

class CodeGeneratorARM64 : public CodeGenerator {
 public:
  CodeGeneratorARM64(HGraph* graph,
                     const Arm64InstructionSetFeatures& isa_features,
                     const CompilerOptions& compiler_options);
  virtual ~CodeGeneratorARM64() {}

  void GenerateFrameEntry() OVERRIDE;
  void GenerateFrameExit() OVERRIDE;

  vixl::CPURegList GetFramePreservedCoreRegisters() const {
    return vixl::CPURegList(vixl::CPURegister::kRegister, vixl::kXRegSize,
                            core_spill_mask_);
  }

  vixl::CPURegList GetFramePreservedFPRegisters() const {
    return vixl::CPURegList(vixl::CPURegister::kFPRegister, vixl::kDRegSize,
                            fpu_spill_mask_);
  }

  void Bind(HBasicBlock* block) OVERRIDE;

  vixl::Label* GetLabelOf(HBasicBlock* block) const {
    return CommonGetLabelOf<vixl::Label>(block_labels_, block);
  }

  void Move(HInstruction* instruction, Location location, HInstruction* move_for) OVERRIDE;

  size_t GetWordSize() const OVERRIDE {
    return kArm64WordSize;
  }

  size_t GetFloatingPointSpillSlotSize() const OVERRIDE {
    // Allocated in D registers, which are word sized.
    return kArm64WordSize;
  }

  uintptr_t GetAddressOf(HBasicBlock* block) const OVERRIDE {
    vixl::Label* block_entry_label = GetLabelOf(block);
    DCHECK(block_entry_label->IsBound());
    return block_entry_label->location();
  }

  HGraphVisitor* GetLocationBuilder() OVERRIDE { return &location_builder_; }
  HGraphVisitor* GetInstructionVisitor() OVERRIDE { return &instruction_visitor_; }
  Arm64Assembler* GetAssembler() OVERRIDE { return &assembler_; }
  vixl::MacroAssembler* GetVIXLAssembler() { return GetAssembler()->vixl_masm_; }

  // Emit a write barrier.
  void MarkGCCard(vixl::Register object, vixl::Register value);

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

  // The number of registers that can be allocated. The register allocator may
  // decide to reserve and not use a few of them.
  // We do not consider registers sp, xzr, wzr. They are either not allocatable
  // (xzr, wzr), or make for poor allocatable registers (sp alignment
  // requirements, etc.). This also facilitates our task as all other registers
  // can easily be mapped via to or from their type and index or code.
  static const int kNumberOfAllocatableRegisters = vixl::kNumberOfRegisters - 1;
  static const int kNumberOfAllocatableFPRegisters = vixl::kNumberOfFPRegisters;
  static constexpr int kNumberOfAllocatableRegisterPairs = 0;

  void DumpCoreRegister(std::ostream& stream, int reg) const OVERRIDE;
  void DumpFloatingPointRegister(std::ostream& stream, int reg) const OVERRIDE;

  InstructionSet GetInstructionSet() const OVERRIDE {
    return InstructionSet::kArm64;
  }

  const Arm64InstructionSetFeatures& GetInstructionSetFeatures() const {
    return isa_features_;
  }

  void Initialize() OVERRIDE {
    HGraph* graph = GetGraph();
    int length = graph->GetBlocks().Size();
    block_labels_ = graph->GetArena()->AllocArray<vixl::Label>(length);
    for (int i = 0; i < length; ++i) {
      new(block_labels_ + i) vixl::Label();
    }
  }

  void Finalize(CodeAllocator* allocator) OVERRIDE;

  // Code generation helpers.
  void MoveConstant(vixl::CPURegister destination, HConstant* constant);
  // The type is optional. When specified it must be coherent with the
  // locations, and is used for optimisation and debugging.
  void MoveLocation(Location destination, Location source,
                    Primitive::Type type = Primitive::kPrimVoid);
  void Load(Primitive::Type type, vixl::CPURegister dst, const vixl::MemOperand& src);
  void Store(Primitive::Type type, vixl::CPURegister rt, const vixl::MemOperand& dst);
  void LoadCurrentMethod(vixl::Register current_method);
  void LoadAcquire(HInstruction* instruction, vixl::CPURegister dst, const vixl::MemOperand& src);
  void StoreRelease(Primitive::Type type, vixl::CPURegister rt, const vixl::MemOperand& dst);

  // Generate code to invoke a runtime entry point.
  void InvokeRuntime(int32_t offset,
                     HInstruction* instruction,
                     uint32_t dex_pc,
                     SlowPathCode* slow_path);

  ParallelMoveResolverARM64* GetMoveResolver() { return &move_resolver_; }

  bool NeedsTwoRegisters(Primitive::Type type ATTRIBUTE_UNUSED) const OVERRIDE {
    return false;
  }

  void GenerateStaticOrDirectCall(HInvokeStaticOrDirect* invoke, vixl::Register temp);

 private:
  // Labels for each block that will be compiled.
  vixl::Label* block_labels_;
  vixl::Label frame_entry_label_;

  LocationsBuilderARM64 location_builder_;
  InstructionCodeGeneratorARM64 instruction_visitor_;
  ParallelMoveResolverARM64 move_resolver_;
  Arm64Assembler assembler_;
  const Arm64InstructionSetFeatures& isa_features_;

  DISALLOW_COPY_AND_ASSIGN(CodeGeneratorARM64);
};

inline Arm64Assembler* ParallelMoveResolverARM64::GetAssembler() const {
  return codegen_->GetAssembler();
}

}  // namespace arm64
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM64_H_
