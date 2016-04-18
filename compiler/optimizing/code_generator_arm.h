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
#include "utils/string_reference.h"

namespace art {
namespace arm {

class CodeGeneratorARM;

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
  Location GetReturnLocation(Primitive::Type type) const OVERRIDE;
  Location GetMethodLocation() const OVERRIDE;

 private:
  InvokeDexCallingConvention calling_convention;
  uint32_t double_index_ = 0;

  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConventionVisitorARM);
};

class FieldAccessCallingConventionARM : public FieldAccessCallingConvention {
 public:
  FieldAccessCallingConventionARM() {}

  Location GetObjectLocation() const OVERRIDE {
    return Location::RegisterLocation(R1);
  }
  Location GetFieldIndexLocation() const OVERRIDE {
    return Location::RegisterLocation(R0);
  }
  Location GetReturnLocation(Primitive::Type type) const OVERRIDE {
    return Primitive::Is64BitType(type)
        ? Location::RegisterPairLocation(R0, R1)
        : Location::RegisterLocation(R0);
  }
  Location GetSetValueLocation(Primitive::Type type, bool is_instance) const OVERRIDE {
    return Primitive::Is64BitType(type)
        ? Location::RegisterPairLocation(R2, R3)
        : (is_instance
            ? Location::RegisterLocation(R2)
            : Location::RegisterLocation(R1));
  }
  Location GetFpuLocation(Primitive::Type type) const OVERRIDE {
    return Primitive::Is64BitType(type)
        ? Location::FpuRegisterPairLocation(S0, S1)
        : Location::FpuRegisterLocation(S0);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(FieldAccessCallingConventionARM);
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

class LocationsBuilderARM : public HGraphVisitor {
 public:
  LocationsBuilderARM(HGraph* graph, CodeGeneratorARM* codegen)
      : HGraphVisitor(graph), codegen_(codegen) {}

#define DECLARE_VISIT_INSTRUCTION(name, super)     \
  void Visit##name(H##name* instr) OVERRIDE;

  FOR_EACH_CONCRETE_INSTRUCTION_COMMON(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_ARM(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_SHARED(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  void VisitInstruction(HInstruction* instruction) OVERRIDE {
    LOG(FATAL) << "Unreachable instruction " << instruction->DebugName()
               << " (id " << instruction->GetId() << ")";
  }

 private:
  void HandleInvoke(HInvoke* invoke);
  void HandleBitwiseOperation(HBinaryOperation* operation, Opcode opcode);
  void HandleCondition(HCondition* condition);
  void HandleIntegerRotate(LocationSummary* locations);
  void HandleLongRotate(LocationSummary* locations);
  void HandleShift(HBinaryOperation* operation);
  void HandleFieldSet(HInstruction* instruction, const FieldInfo& field_info);
  void HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info);

  Location ArmEncodableConstantOrRegister(HInstruction* constant, Opcode opcode);
  bool CanEncodeConstantAsImmediate(HConstant* input_cst, Opcode opcode);
  bool CanEncodeConstantAsImmediate(uint32_t value, Opcode opcode);

  CodeGeneratorARM* const codegen_;
  InvokeDexCallingConventionVisitorARM parameter_visitor_;

  DISALLOW_COPY_AND_ASSIGN(LocationsBuilderARM);
};

class InstructionCodeGeneratorARM : public InstructionCodeGenerator {
 public:
  InstructionCodeGeneratorARM(HGraph* graph, CodeGeneratorARM* codegen);

#define DECLARE_VISIT_INSTRUCTION(name, super)     \
  void Visit##name(H##name* instr) OVERRIDE;

  FOR_EACH_CONCRETE_INSTRUCTION_COMMON(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_ARM(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_SHARED(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  void VisitInstruction(HInstruction* instruction) OVERRIDE {
    LOG(FATAL) << "Unreachable instruction " << instruction->DebugName()
               << " (id " << instruction->GetId() << ")";
  }

  ArmAssembler* GetAssembler() const { return assembler_; }

 private:
  // Generate code for the given suspend check. If not null, `successor`
  // is the block to branch to if the suspend check is not needed, and after
  // the suspend call.
  void GenerateSuspendCheck(HSuspendCheck* check, HBasicBlock* successor);
  void GenerateClassInitializationCheck(SlowPathCode* slow_path, Register class_reg);
  void GenerateAndConst(Register out, Register first, uint32_t value);
  void GenerateOrrConst(Register out, Register first, uint32_t value);
  void GenerateEorConst(Register out, Register first, uint32_t value);
  void HandleBitwiseOperation(HBinaryOperation* operation);
  void HandleCondition(HCondition* condition);
  void HandleIntegerRotate(LocationSummary* locations);
  void HandleLongRotate(LocationSummary* locations);
  void HandleShift(HBinaryOperation* operation);

  void GenerateWideAtomicStore(Register addr, uint32_t offset,
                               Register value_lo, Register value_hi,
                               Register temp1, Register temp2,
                               HInstruction* instruction);
  void GenerateWideAtomicLoad(Register addr, uint32_t offset,
                              Register out_lo, Register out_hi);

  void HandleFieldSet(HInstruction* instruction,
                      const FieldInfo& field_info,
                      bool value_can_be_null);
  void HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info);

  // Generate a heap reference load using one register `out`:
  //
  //   out <- *(out + offset)
  //
  // while honoring heap poisoning and/or read barriers (if any).
  //
  // Location `maybe_temp` is used when generating a read barrier and
  // shall be a register in that case; it may be an invalid location
  // otherwise.
  void GenerateReferenceLoadOneRegister(HInstruction* instruction,
                                        Location out,
                                        uint32_t offset,
                                        Location maybe_temp);
  // Generate a heap reference load using two different registers
  // `out` and `obj`:
  //
  //   out <- *(obj + offset)
  //
  // while honoring heap poisoning and/or read barriers (if any).
  //
  // Location `maybe_temp` is used when generating a Baker's (fast
  // path) read barrier and shall be a register in that case; it may
  // be an invalid location otherwise.
  void GenerateReferenceLoadTwoRegisters(HInstruction* instruction,
                                         Location out,
                                         Location obj,
                                         uint32_t offset,
                                         Location maybe_temp);
  // Generate a GC root reference load:
  //
  //   root <- *(obj + offset)
  //
  // while honoring read barriers (if any).
  void GenerateGcRootFieldLoad(HInstruction* instruction,
                               Location root,
                               Register obj,
                               uint32_t offset);
  void GenerateTestAndBranch(HInstruction* instruction,
                             size_t condition_input_index,
                             Label* true_target,
                             Label* false_target);
  void GenerateCompareTestAndBranch(HCondition* condition,
                                    Label* true_target,
                                    Label* false_target);
  void GenerateFPJumps(HCondition* cond, Label* true_label, Label* false_label);
  void GenerateLongComparesAndJumps(HCondition* cond, Label* true_label, Label* false_label);
  void DivRemOneOrMinusOne(HBinaryOperation* instruction);
  void DivRemByPowerOfTwo(HBinaryOperation* instruction);
  void GenerateDivRemWithAnyConstant(HBinaryOperation* instruction);
  void GenerateDivRemConstantIntegral(HBinaryOperation* instruction);
  void HandleGoto(HInstruction* got, HBasicBlock* successor);

  ArmAssembler* const assembler_;
  CodeGeneratorARM* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(InstructionCodeGeneratorARM);
};

class CodeGeneratorARM : public CodeGenerator {
 public:
  CodeGeneratorARM(HGraph* graph,
                   const ArmInstructionSetFeatures& isa_features,
                   const CompilerOptions& compiler_options,
                   OptimizingCompilerStats* stats = nullptr);
  virtual ~CodeGeneratorARM() {}

  void GenerateFrameEntry() OVERRIDE;
  void GenerateFrameExit() OVERRIDE;
  void Bind(HBasicBlock* block) OVERRIDE;
  void MoveConstant(Location destination, int32_t value) OVERRIDE;
  void MoveLocation(Location dst, Location src, Primitive::Type dst_type) OVERRIDE;
  void AddLocationAsTemp(Location location, LocationSummary* locations) OVERRIDE;

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

  const ArmAssembler& GetAssembler() const OVERRIDE {
    return assembler_;
  }

  uintptr_t GetAddressOf(HBasicBlock* block) OVERRIDE {
    return GetLabelOf(block)->Position();
  }

  void SetupBlockedRegisters() const OVERRIDE;

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

  // Generate code to invoke a runtime entry point.
  void InvokeRuntime(QuickEntrypointEnum entrypoint,
                     HInstruction* instruction,
                     uint32_t dex_pc,
                     SlowPathCode* slow_path) OVERRIDE;

  void InvokeRuntime(int32_t offset,
                     HInstruction* instruction,
                     uint32_t dex_pc,
                     SlowPathCode* slow_path);

  // Emit a write barrier.
  void MarkGCCard(Register temp, Register card, Register object, Register value, bool can_be_null);

  void GenerateMemoryBarrier(MemBarrierKind kind);

  Label* GetLabelOf(HBasicBlock* block) const {
    return CommonGetLabelOf<Label>(block_labels_, block);
  }

  void Initialize() OVERRIDE {
    block_labels_ = CommonInitializeLabels<Label>();
  }

  void Finalize(CodeAllocator* allocator) OVERRIDE;

  const ArmInstructionSetFeatures& GetInstructionSetFeatures() const {
    return isa_features_;
  }

  bool NeedsTwoRegisters(Primitive::Type type) const OVERRIDE {
    return type == Primitive::kPrimDouble || type == Primitive::kPrimLong;
  }

  void ComputeSpillMask() OVERRIDE;

  Label* GetFrameEntryLabel() { return &frame_entry_label_; }

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

  void MoveFromReturnRegister(Location trg, Primitive::Type type) OVERRIDE;

  // The PcRelativePatchInfo is used for PC-relative addressing of dex cache arrays
  // and boot image strings. The only difference is the interpretation of the offset_or_index.
  // The PC-relative address is loaded with three instructions, MOVW+MOVT
  // to load the offset to base_reg and then ADD base_reg, PC. The offset is
  // calculated from the ADD's effective PC, i.e. PC+4 on Thumb2. Though we
  // currently emit these 3 instructions together, instruction scheduling could
  // split this sequence apart, so we keep separate labels for each of them.
  struct PcRelativePatchInfo {
    PcRelativePatchInfo(const DexFile& dex_file, uint32_t off_or_idx)
        : target_dex_file(dex_file), offset_or_index(off_or_idx) { }
    PcRelativePatchInfo(PcRelativePatchInfo&& other) = default;

    const DexFile& target_dex_file;
    // Either the dex cache array element offset or the string index.
    uint32_t offset_or_index;
    Label movw_label;
    Label movt_label;
    Label add_pc_label;
  };

  PcRelativePatchInfo* NewPcRelativeStringPatch(const DexFile& dex_file, uint32_t string_index);
  PcRelativePatchInfo* NewPcRelativeDexCacheArrayPatch(const DexFile& dex_file,
                                                       uint32_t element_offset);
  Literal* DeduplicateBootImageStringLiteral(const DexFile& dex_file, uint32_t string_index);
  Literal* DeduplicateBootImageAddressLiteral(uint32_t address);
  Literal* DeduplicateDexCacheAddressLiteral(uint32_t address);

  void EmitLinkerPatches(ArenaVector<LinkerPatch>* linker_patches) OVERRIDE;

  // Fast path implementation of ReadBarrier::Barrier for a heap
  // reference field load when Baker's read barriers are used.
  void GenerateFieldLoadWithBakerReadBarrier(HInstruction* instruction,
                                             Location ref,
                                             Register obj,
                                             uint32_t offset,
                                             Location temp,
                                             bool needs_null_check);
  // Fast path implementation of ReadBarrier::Barrier for a heap
  // reference array load when Baker's read barriers are used.
  void GenerateArrayLoadWithBakerReadBarrier(HInstruction* instruction,
                                             Location ref,
                                             Register obj,
                                             uint32_t data_offset,
                                             Location index,
                                             Location temp,
                                             bool needs_null_check);

  // Generate a read barrier for a heap reference within `instruction`
  // using a slow path.
  //
  // A read barrier for an object reference read from the heap is
  // implemented as a call to the artReadBarrierSlow runtime entry
  // point, which is passed the values in locations `ref`, `obj`, and
  // `offset`:
  //
  //   mirror::Object* artReadBarrierSlow(mirror::Object* ref,
  //                                      mirror::Object* obj,
  //                                      uint32_t offset);
  //
  // The `out` location contains the value returned by
  // artReadBarrierSlow.
  //
  // When `index` is provided (i.e. for array accesses), the offset
  // value passed to artReadBarrierSlow is adjusted to take `index`
  // into account.
  void GenerateReadBarrierSlow(HInstruction* instruction,
                               Location out,
                               Location ref,
                               Location obj,
                               uint32_t offset,
                               Location index = Location::NoLocation());

  // If read barriers are enabled, generate a read barrier for a heap
  // reference using a slow path. If heap poisoning is enabled, also
  // unpoison the reference in `out`.
  void MaybeGenerateReadBarrierSlow(HInstruction* instruction,
                                    Location out,
                                    Location ref,
                                    Location obj,
                                    uint32_t offset,
                                    Location index = Location::NoLocation());

  // Generate a read barrier for a GC root within `instruction` using
  // a slow path.
  //
  // A read barrier for an object reference GC root is implemented as
  // a call to the artReadBarrierForRootSlow runtime entry point,
  // which is passed the value in location `root`:
  //
  //   mirror::Object* artReadBarrierForRootSlow(GcRoot<mirror::Object>* root);
  //
  // The `out` location contains the value returned by
  // artReadBarrierForRootSlow.
  void GenerateReadBarrierForRootSlow(HInstruction* instruction, Location out, Location root);

  void GenerateNop();

  void GenerateImplicitNullCheck(HNullCheck* instruction);
  void GenerateExplicitNullCheck(HNullCheck* instruction);

 private:
  // Factored implementation of GenerateFieldLoadWithBakerReadBarrier
  // and GenerateArrayLoadWithBakerReadBarrier.
  void GenerateReferenceLoadWithBakerReadBarrier(HInstruction* instruction,
                                                 Location ref,
                                                 Register obj,
                                                 uint32_t offset,
                                                 Location index,
                                                 Location temp,
                                                 bool needs_null_check);

  Register GetInvokeStaticOrDirectExtraParameter(HInvokeStaticOrDirect* invoke, Register temp);

  using Uint32ToLiteralMap = ArenaSafeMap<uint32_t, Literal*>;
  using MethodToLiteralMap = ArenaSafeMap<MethodReference, Literal*, MethodReferenceComparator>;
  using BootStringToLiteralMap = ArenaSafeMap<StringReference,
                                              Literal*,
                                              StringReferenceValueComparator>;

  Literal* DeduplicateUint32Literal(uint32_t value, Uint32ToLiteralMap* map);
  Literal* DeduplicateMethodLiteral(MethodReference target_method, MethodToLiteralMap* map);
  Literal* DeduplicateMethodAddressLiteral(MethodReference target_method);
  Literal* DeduplicateMethodCodeLiteral(MethodReference target_method);
  PcRelativePatchInfo* NewPcRelativePatch(const DexFile& dex_file,
                                          uint32_t offset_or_index,
                                          ArenaDeque<PcRelativePatchInfo>* patches);

  // Labels for each block that will be compiled.
  Label* block_labels_;  // Indexed by block id.
  Label frame_entry_label_;
  LocationsBuilderARM location_builder_;
  InstructionCodeGeneratorARM instruction_visitor_;
  ParallelMoveResolverARM move_resolver_;
  Thumb2Assembler assembler_;
  const ArmInstructionSetFeatures& isa_features_;

  // Deduplication map for 32-bit literals, used for non-patchable boot image addresses.
  Uint32ToLiteralMap uint32_literals_;
  // Method patch info, map MethodReference to a literal for method address and method code.
  MethodToLiteralMap method_patches_;
  MethodToLiteralMap call_patches_;
  // Relative call patch info.
  // Using ArenaDeque<> which retains element addresses on push/emplace_back().
  ArenaDeque<MethodPatchInfo<Label>> relative_call_patches_;
  // PC-relative patch info for each HArmDexCacheArraysBase.
  ArenaDeque<PcRelativePatchInfo> pc_relative_dex_cache_patches_;
  // Deduplication map for boot string literals for kBootImageLinkTimeAddress.
  BootStringToLiteralMap boot_image_string_patches_;
  // PC-relative String patch info.
  ArenaDeque<PcRelativePatchInfo> pc_relative_string_patches_;
  // Deduplication map for patchable boot image addresses.
  Uint32ToLiteralMap boot_image_address_patches_;

  DISALLOW_COPY_AND_ASSIGN(CodeGeneratorARM);
};

}  // namespace arm
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM_H_
