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

#include "code_generator.h"

#ifdef ART_ENABLE_CODEGEN_arm
#include "code_generator_arm.h"
#endif

#ifdef ART_ENABLE_CODEGEN_arm64
#include "code_generator_arm64.h"
#endif

#ifdef ART_ENABLE_CODEGEN_x86
#include "code_generator_x86.h"
#endif

#ifdef ART_ENABLE_CODEGEN_x86_64
#include "code_generator_x86_64.h"
#endif

#ifdef ART_ENABLE_CODEGEN_mips
#include "code_generator_mips.h"
#endif

#ifdef ART_ENABLE_CODEGEN_mips64
#include "code_generator_mips64.h"
#endif

#include "bytecode_utils.h"
#include "compiled_method.h"
#include "dex/verified_method.h"
#include "driver/compiler_driver.h"
#include "graph_visualizer.h"
#include "intrinsics.h"
#include "leb128.h"
#include "mirror/array-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/object_reference.h"
#include "parallel_move_resolver.h"
#include "ssa_liveness_analysis.h"
#include "utils/assembler.h"

namespace art {

// Return whether a location is consistent with a type.
static bool CheckType(Primitive::Type type, Location location) {
  if (location.IsFpuRegister()
      || (location.IsUnallocated() && (location.GetPolicy() == Location::kRequiresFpuRegister))) {
    return (type == Primitive::kPrimFloat) || (type == Primitive::kPrimDouble);
  } else if (location.IsRegister() ||
             (location.IsUnallocated() && (location.GetPolicy() == Location::kRequiresRegister))) {
    return Primitive::IsIntegralType(type) || (type == Primitive::kPrimNot);
  } else if (location.IsRegisterPair()) {
    return type == Primitive::kPrimLong;
  } else if (location.IsFpuRegisterPair()) {
    return type == Primitive::kPrimDouble;
  } else if (location.IsStackSlot()) {
    return (Primitive::IsIntegralType(type) && type != Primitive::kPrimLong)
           || (type == Primitive::kPrimFloat)
           || (type == Primitive::kPrimNot);
  } else if (location.IsDoubleStackSlot()) {
    return (type == Primitive::kPrimLong) || (type == Primitive::kPrimDouble);
  } else if (location.IsConstant()) {
    if (location.GetConstant()->IsIntConstant()) {
      return Primitive::IsIntegralType(type) && (type != Primitive::kPrimLong);
    } else if (location.GetConstant()->IsNullConstant()) {
      return type == Primitive::kPrimNot;
    } else if (location.GetConstant()->IsLongConstant()) {
      return type == Primitive::kPrimLong;
    } else if (location.GetConstant()->IsFloatConstant()) {
      return type == Primitive::kPrimFloat;
    } else {
      return location.GetConstant()->IsDoubleConstant()
          && (type == Primitive::kPrimDouble);
    }
  } else {
    return location.IsInvalid() || (location.GetPolicy() == Location::kAny);
  }
}

// Check that a location summary is consistent with an instruction.
static bool CheckTypeConsistency(HInstruction* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  if (locations == nullptr) {
    return true;
  }

  if (locations->Out().IsUnallocated()
      && (locations->Out().GetPolicy() == Location::kSameAsFirstInput)) {
    DCHECK(CheckType(instruction->GetType(), locations->InAt(0)))
        << instruction->GetType()
        << " " << locations->InAt(0);
  } else {
    DCHECK(CheckType(instruction->GetType(), locations->Out()))
        << instruction->GetType()
        << " " << locations->Out();
  }

  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    DCHECK(CheckType(instruction->InputAt(i)->GetType(), locations->InAt(i)))
      << instruction->InputAt(i)->GetType()
      << " " << locations->InAt(i);
  }

  HEnvironment* environment = instruction->GetEnvironment();
  for (size_t i = 0; i < instruction->EnvironmentSize(); ++i) {
    if (environment->GetInstructionAt(i) != nullptr) {
      Primitive::Type type = environment->GetInstructionAt(i)->GetType();
      DCHECK(CheckType(type, environment->GetLocationAt(i)))
        << type << " " << environment->GetLocationAt(i);
    } else {
      DCHECK(environment->GetLocationAt(i).IsInvalid())
        << environment->GetLocationAt(i);
    }
  }
  return true;
}

size_t CodeGenerator::GetCacheOffset(uint32_t index) {
  return sizeof(GcRoot<mirror::Object>) * index;
}

size_t CodeGenerator::GetCachePointerOffset(uint32_t index) {
  auto pointer_size = InstructionSetPointerSize(GetInstructionSet());
  return pointer_size * index;
}

bool CodeGenerator::GoesToNextBlock(HBasicBlock* current, HBasicBlock* next) const {
  DCHECK_EQ((*block_order_)[current_block_index_], current);
  return GetNextBlockToEmit() == FirstNonEmptyBlock(next);
}

HBasicBlock* CodeGenerator::GetNextBlockToEmit() const {
  for (size_t i = current_block_index_ + 1; i < block_order_->size(); ++i) {
    HBasicBlock* block = (*block_order_)[i];
    if (!block->IsSingleJump()) {
      return block;
    }
  }
  return nullptr;
}

HBasicBlock* CodeGenerator::FirstNonEmptyBlock(HBasicBlock* block) const {
  while (block->IsSingleJump()) {
    block = block->GetSuccessors()[0];
  }
  return block;
}

class DisassemblyScope {
 public:
  DisassemblyScope(HInstruction* instruction, const CodeGenerator& codegen)
      : codegen_(codegen), instruction_(instruction), start_offset_(static_cast<size_t>(-1)) {
    if (codegen_.GetDisassemblyInformation() != nullptr) {
      start_offset_ = codegen_.GetAssembler().CodeSize();
    }
  }

  ~DisassemblyScope() {
    // We avoid building this data when we know it will not be used.
    if (codegen_.GetDisassemblyInformation() != nullptr) {
      codegen_.GetDisassemblyInformation()->AddInstructionInterval(
          instruction_, start_offset_, codegen_.GetAssembler().CodeSize());
    }
  }

 private:
  const CodeGenerator& codegen_;
  HInstruction* instruction_;
  size_t start_offset_;
};


void CodeGenerator::GenerateSlowPaths() {
  size_t code_start = 0;
  for (const std::unique_ptr<SlowPathCode>& slow_path_unique_ptr : slow_paths_) {
    SlowPathCode* slow_path = slow_path_unique_ptr.get();
    current_slow_path_ = slow_path;
    if (disasm_info_ != nullptr) {
      code_start = GetAssembler()->CodeSize();
    }
    // Record the dex pc at start of slow path (required for java line number mapping).
    MaybeRecordNativeDebugInfo(slow_path->GetInstruction(), slow_path->GetDexPc(), slow_path);
    slow_path->EmitNativeCode(this);
    if (disasm_info_ != nullptr) {
      disasm_info_->AddSlowPathInterval(slow_path, code_start, GetAssembler()->CodeSize());
    }
  }
  current_slow_path_ = nullptr;
}

void CodeGenerator::Compile(CodeAllocator* allocator) {
  // The register allocator already called `InitializeCodeGeneration`,
  // where the frame size has been computed.
  DCHECK(block_order_ != nullptr);
  Initialize();

  HGraphVisitor* instruction_visitor = GetInstructionVisitor();
  DCHECK_EQ(current_block_index_, 0u);

  size_t frame_start = GetAssembler()->CodeSize();
  GenerateFrameEntry();
  DCHECK_EQ(GetAssembler()->cfi().GetCurrentCFAOffset(), static_cast<int>(frame_size_));
  if (disasm_info_ != nullptr) {
    disasm_info_->SetFrameEntryInterval(frame_start, GetAssembler()->CodeSize());
  }

  for (size_t e = block_order_->size(); current_block_index_ < e; ++current_block_index_) {
    HBasicBlock* block = (*block_order_)[current_block_index_];
    // Don't generate code for an empty block. Its predecessors will branch to its successor
    // directly. Also, the label of that block will not be emitted, so this helps catch
    // errors where we reference that label.
    if (block->IsSingleJump()) continue;
    Bind(block);
    // This ensures that we have correct native line mapping for all native instructions.
    // It is necessary to make stepping over a statement work. Otherwise, any initial
    // instructions (e.g. moves) would be assumed to be the start of next statement.
    MaybeRecordNativeDebugInfo(nullptr /* instruction */, block->GetDexPc());
    for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
      HInstruction* current = it.Current();
      if (current->HasEnvironment()) {
        // Create stackmap for HNativeDebugInfo or any instruction which calls native code.
        // Note that we need correct mapping for the native PC of the call instruction,
        // so the runtime's stackmap is not sufficient since it is at PC after the call.
        MaybeRecordNativeDebugInfo(current, block->GetDexPc());
      }
      DisassemblyScope disassembly_scope(current, *this);
      DCHECK(CheckTypeConsistency(current));
      current->Accept(instruction_visitor);
    }
  }

  GenerateSlowPaths();

  // Emit catch stack maps at the end of the stack map stream as expected by the
  // runtime exception handler.
  if (graph_->HasTryCatch()) {
    RecordCatchBlockInfo();
  }

  // Finalize instructions in assember;
  Finalize(allocator);
}

void CodeGenerator::Finalize(CodeAllocator* allocator) {
  size_t code_size = GetAssembler()->CodeSize();
  uint8_t* buffer = allocator->Allocate(code_size);

  MemoryRegion code(buffer, code_size);
  GetAssembler()->FinalizeInstructions(code);
}

void CodeGenerator::EmitLinkerPatches(ArenaVector<LinkerPatch>* linker_patches ATTRIBUTE_UNUSED) {
  // No linker patches by default.
}

void CodeGenerator::InitializeCodeGeneration(size_t number_of_spill_slots,
                                             size_t maximum_number_of_live_core_registers,
                                             size_t maximum_number_of_live_fpu_registers,
                                             size_t number_of_out_slots,
                                             const ArenaVector<HBasicBlock*>& block_order) {
  block_order_ = &block_order;
  DCHECK(!block_order.empty());
  DCHECK(block_order[0] == GetGraph()->GetEntryBlock());
  ComputeSpillMask();
  first_register_slot_in_slow_path_ = (number_of_out_slots + number_of_spill_slots) * kVRegSize;

  if (number_of_spill_slots == 0
      && !HasAllocatedCalleeSaveRegisters()
      && IsLeafMethod()
      && !RequiresCurrentMethod()) {
    DCHECK_EQ(maximum_number_of_live_core_registers, 0u);
    DCHECK_EQ(maximum_number_of_live_fpu_registers, 0u);
    SetFrameSize(CallPushesPC() ? GetWordSize() : 0);
  } else {
    SetFrameSize(RoundUp(
        number_of_spill_slots * kVRegSize
        + number_of_out_slots * kVRegSize
        + maximum_number_of_live_core_registers * GetWordSize()
        + maximum_number_of_live_fpu_registers * GetFloatingPointSpillSlotSize()
        + FrameEntrySpillSize(),
        kStackAlignment));
  }
}

void CodeGenerator::CreateCommonInvokeLocationSummary(
    HInvoke* invoke, InvokeDexCallingConventionVisitor* visitor) {
  ArenaAllocator* allocator = invoke->GetBlock()->GetGraph()->GetArena();
  LocationSummary* locations = new (allocator) LocationSummary(invoke, LocationSummary::kCall);

  for (size_t i = 0; i < invoke->GetNumberOfArguments(); i++) {
    HInstruction* input = invoke->InputAt(i);
    locations->SetInAt(i, visitor->GetNextLocation(input->GetType()));
  }

  locations->SetOut(visitor->GetReturnLocation(invoke->GetType()));

  if (invoke->IsInvokeStaticOrDirect()) {
    HInvokeStaticOrDirect* call = invoke->AsInvokeStaticOrDirect();
    switch (call->GetMethodLoadKind()) {
      case HInvokeStaticOrDirect::MethodLoadKind::kRecursive:
        locations->SetInAt(call->GetSpecialInputIndex(), visitor->GetMethodLocation());
        break;
      case HInvokeStaticOrDirect::MethodLoadKind::kDexCacheViaMethod:
        locations->AddTemp(visitor->GetMethodLocation());
        locations->SetInAt(call->GetSpecialInputIndex(), Location::RequiresRegister());
        break;
      default:
        locations->AddTemp(visitor->GetMethodLocation());
        break;
    }
  } else {
    locations->AddTemp(visitor->GetMethodLocation());
  }
}

void CodeGenerator::GenerateInvokeUnresolvedRuntimeCall(HInvokeUnresolved* invoke) {
  MoveConstant(invoke->GetLocations()->GetTemp(0), invoke->GetDexMethodIndex());

  // Initialize to anything to silent compiler warnings.
  QuickEntrypointEnum entrypoint = kQuickInvokeStaticTrampolineWithAccessCheck;
  switch (invoke->GetOriginalInvokeType()) {
    case kStatic:
      entrypoint = kQuickInvokeStaticTrampolineWithAccessCheck;
      break;
    case kDirect:
      entrypoint = kQuickInvokeDirectTrampolineWithAccessCheck;
      break;
    case kVirtual:
      entrypoint = kQuickInvokeVirtualTrampolineWithAccessCheck;
      break;
    case kSuper:
      entrypoint = kQuickInvokeSuperTrampolineWithAccessCheck;
      break;
    case kInterface:
      entrypoint = kQuickInvokeInterfaceTrampolineWithAccessCheck;
      break;
  }
  InvokeRuntime(entrypoint, invoke, invoke->GetDexPc(), nullptr);
}

void CodeGenerator::CreateUnresolvedFieldLocationSummary(
    HInstruction* field_access,
    Primitive::Type field_type,
    const FieldAccessCallingConvention& calling_convention) {
  bool is_instance = field_access->IsUnresolvedInstanceFieldGet()
      || field_access->IsUnresolvedInstanceFieldSet();
  bool is_get = field_access->IsUnresolvedInstanceFieldGet()
      || field_access->IsUnresolvedStaticFieldGet();

  ArenaAllocator* allocator = field_access->GetBlock()->GetGraph()->GetArena();
  LocationSummary* locations =
      new (allocator) LocationSummary(field_access, LocationSummary::kCall);

  locations->AddTemp(calling_convention.GetFieldIndexLocation());

  if (is_instance) {
    // Add the `this` object for instance field accesses.
    locations->SetInAt(0, calling_convention.GetObjectLocation());
  }

  // Note that pSetXXStatic/pGetXXStatic always takes/returns an int or int64
  // regardless of the the type. Because of that we forced to special case
  // the access to floating point values.
  if (is_get) {
    if (Primitive::IsFloatingPointType(field_type)) {
      // The return value will be stored in regular registers while register
      // allocator expects it in a floating point register.
      // Note We don't need to request additional temps because the return
      // register(s) are already blocked due the call and they may overlap with
      // the input or field index.
      // The transfer between the two will be done at codegen level.
      locations->SetOut(calling_convention.GetFpuLocation(field_type));
    } else {
      locations->SetOut(calling_convention.GetReturnLocation(field_type));
    }
  } else {
     size_t set_index = is_instance ? 1 : 0;
     if (Primitive::IsFloatingPointType(field_type)) {
      // The set value comes from a float location while the calling convention
      // expects it in a regular register location. Allocate a temp for it and
      // make the transfer at codegen.
      AddLocationAsTemp(calling_convention.GetSetValueLocation(field_type, is_instance), locations);
      locations->SetInAt(set_index, calling_convention.GetFpuLocation(field_type));
    } else {
      locations->SetInAt(set_index,
          calling_convention.GetSetValueLocation(field_type, is_instance));
    }
  }
}

void CodeGenerator::GenerateUnresolvedFieldAccess(
    HInstruction* field_access,
    Primitive::Type field_type,
    uint32_t field_index,
    uint32_t dex_pc,
    const FieldAccessCallingConvention& calling_convention) {
  LocationSummary* locations = field_access->GetLocations();

  MoveConstant(locations->GetTemp(0), field_index);

  bool is_instance = field_access->IsUnresolvedInstanceFieldGet()
      || field_access->IsUnresolvedInstanceFieldSet();
  bool is_get = field_access->IsUnresolvedInstanceFieldGet()
      || field_access->IsUnresolvedStaticFieldGet();

  if (!is_get && Primitive::IsFloatingPointType(field_type)) {
    // Copy the float value to be set into the calling convention register.
    // Note that using directly the temp location is problematic as we don't
    // support temp register pairs. To avoid boilerplate conversion code, use
    // the location from the calling convention.
    MoveLocation(calling_convention.GetSetValueLocation(field_type, is_instance),
                 locations->InAt(is_instance ? 1 : 0),
                 (Primitive::Is64BitType(field_type) ? Primitive::kPrimLong : Primitive::kPrimInt));
  }

  QuickEntrypointEnum entrypoint = kQuickSet8Static;  // Initialize to anything to avoid warnings.
  switch (field_type) {
    case Primitive::kPrimBoolean:
      entrypoint = is_instance
          ? (is_get ? kQuickGetBooleanInstance : kQuickSet8Instance)
          : (is_get ? kQuickGetBooleanStatic : kQuickSet8Static);
      break;
    case Primitive::kPrimByte:
      entrypoint = is_instance
          ? (is_get ? kQuickGetByteInstance : kQuickSet8Instance)
          : (is_get ? kQuickGetByteStatic : kQuickSet8Static);
      break;
    case Primitive::kPrimShort:
      entrypoint = is_instance
          ? (is_get ? kQuickGetShortInstance : kQuickSet16Instance)
          : (is_get ? kQuickGetShortStatic : kQuickSet16Static);
      break;
    case Primitive::kPrimChar:
      entrypoint = is_instance
          ? (is_get ? kQuickGetCharInstance : kQuickSet16Instance)
          : (is_get ? kQuickGetCharStatic : kQuickSet16Static);
      break;
    case Primitive::kPrimInt:
    case Primitive::kPrimFloat:
      entrypoint = is_instance
          ? (is_get ? kQuickGet32Instance : kQuickSet32Instance)
          : (is_get ? kQuickGet32Static : kQuickSet32Static);
      break;
    case Primitive::kPrimNot:
      entrypoint = is_instance
          ? (is_get ? kQuickGetObjInstance : kQuickSetObjInstance)
          : (is_get ? kQuickGetObjStatic : kQuickSetObjStatic);
      break;
    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      entrypoint = is_instance
          ? (is_get ? kQuickGet64Instance : kQuickSet64Instance)
          : (is_get ? kQuickGet64Static : kQuickSet64Static);
      break;
    default:
      LOG(FATAL) << "Invalid type " << field_type;
  }
  InvokeRuntime(entrypoint, field_access, dex_pc, nullptr);

  if (is_get && Primitive::IsFloatingPointType(field_type)) {
    MoveLocation(locations->Out(), calling_convention.GetReturnLocation(field_type), field_type);
  }
}

// TODO: Remove argument `code_generator_supports_read_barrier` when
// all code generators have read barrier support.
void CodeGenerator::CreateLoadClassLocationSummary(HLoadClass* cls,
                                                   Location runtime_type_index_location,
                                                   Location runtime_return_location,
                                                   bool code_generator_supports_read_barrier) {
  ArenaAllocator* allocator = cls->GetBlock()->GetGraph()->GetArena();
  LocationSummary::CallKind call_kind = cls->NeedsAccessCheck()
      ? LocationSummary::kCall
      : (((code_generator_supports_read_barrier && kEmitCompilerReadBarrier) ||
          cls->CanCallRuntime())
            ? LocationSummary::kCallOnSlowPath
            : LocationSummary::kNoCall);
  LocationSummary* locations = new (allocator) LocationSummary(cls, call_kind);
  if (cls->NeedsAccessCheck()) {
    locations->SetInAt(0, Location::NoLocation());
    locations->AddTemp(runtime_type_index_location);
    locations->SetOut(runtime_return_location);
  } else {
    locations->SetInAt(0, Location::RequiresRegister());
    locations->SetOut(Location::RequiresRegister());
  }
}


void CodeGenerator::BlockIfInRegister(Location location, bool is_out) const {
  // The DCHECKS below check that a register is not specified twice in
  // the summary. The out location can overlap with an input, so we need
  // to special case it.
  if (location.IsRegister()) {
    DCHECK(is_out || !blocked_core_registers_[location.reg()]);
    blocked_core_registers_[location.reg()] = true;
  } else if (location.IsFpuRegister()) {
    DCHECK(is_out || !blocked_fpu_registers_[location.reg()]);
    blocked_fpu_registers_[location.reg()] = true;
  } else if (location.IsFpuRegisterPair()) {
    DCHECK(is_out || !blocked_fpu_registers_[location.AsFpuRegisterPairLow<int>()]);
    blocked_fpu_registers_[location.AsFpuRegisterPairLow<int>()] = true;
    DCHECK(is_out || !blocked_fpu_registers_[location.AsFpuRegisterPairHigh<int>()]);
    blocked_fpu_registers_[location.AsFpuRegisterPairHigh<int>()] = true;
  } else if (location.IsRegisterPair()) {
    DCHECK(is_out || !blocked_core_registers_[location.AsRegisterPairLow<int>()]);
    blocked_core_registers_[location.AsRegisterPairLow<int>()] = true;
    DCHECK(is_out || !blocked_core_registers_[location.AsRegisterPairHigh<int>()]);
    blocked_core_registers_[location.AsRegisterPairHigh<int>()] = true;
  }
}

void CodeGenerator::AllocateLocations(HInstruction* instruction) {
  instruction->Accept(GetLocationBuilder());
  DCHECK(CheckTypeConsistency(instruction));
  LocationSummary* locations = instruction->GetLocations();
  if (!instruction->IsSuspendCheckEntry()) {
    if (locations != nullptr) {
      if (locations->CanCall()) {
        MarkNotLeaf();
      } else if (locations->Intrinsified() &&
                 instruction->IsInvokeStaticOrDirect() &&
                 !instruction->AsInvokeStaticOrDirect()->HasCurrentMethodInput()) {
        // A static method call that has been fully intrinsified, and cannot call on the slow
        // path or refer to the current method directly, no longer needs current method.
        return;
      }
    }
    if (instruction->NeedsCurrentMethod()) {
      SetRequiresCurrentMethod();
    }
  }
}

void CodeGenerator::MaybeRecordStat(MethodCompilationStat compilation_stat, size_t count) const {
  if (stats_ != nullptr) {
    stats_->RecordStat(compilation_stat, count);
  }
}

std::unique_ptr<CodeGenerator> CodeGenerator::Create(HGraph* graph,
                                                     InstructionSet instruction_set,
                                                     const InstructionSetFeatures& isa_features,
                                                     const CompilerOptions& compiler_options,
                                                     OptimizingCompilerStats* stats) {
  ArenaAllocator* arena = graph->GetArena();
  switch (instruction_set) {
#ifdef ART_ENABLE_CODEGEN_arm
    case kArm:
    case kThumb2: {
      return std::unique_ptr<CodeGenerator>(
          new (arena) arm::CodeGeneratorARM(graph,
                                            *isa_features.AsArmInstructionSetFeatures(),
                                            compiler_options,
                                            stats));
    }
#endif
#ifdef ART_ENABLE_CODEGEN_arm64
    case kArm64: {
      return std::unique_ptr<CodeGenerator>(
          new (arena) arm64::CodeGeneratorARM64(graph,
                                                *isa_features.AsArm64InstructionSetFeatures(),
                                                compiler_options,
                                                stats));
    }
#endif
#ifdef ART_ENABLE_CODEGEN_mips
    case kMips: {
      return std::unique_ptr<CodeGenerator>(
          new (arena) mips::CodeGeneratorMIPS(graph,
                                              *isa_features.AsMipsInstructionSetFeatures(),
                                              compiler_options,
                                              stats));
    }
#endif
#ifdef ART_ENABLE_CODEGEN_mips64
    case kMips64: {
      return std::unique_ptr<CodeGenerator>(
          new (arena) mips64::CodeGeneratorMIPS64(graph,
                                                  *isa_features.AsMips64InstructionSetFeatures(),
                                                  compiler_options,
                                                  stats));
    }
#endif
#ifdef ART_ENABLE_CODEGEN_x86
    case kX86: {
      return std::unique_ptr<CodeGenerator>(
          new (arena) x86::CodeGeneratorX86(graph,
                                            *isa_features.AsX86InstructionSetFeatures(),
                                            compiler_options,
                                            stats));
    }
#endif
#ifdef ART_ENABLE_CODEGEN_x86_64
    case kX86_64: {
      return std::unique_ptr<CodeGenerator>(
          new (arena) x86_64::CodeGeneratorX86_64(graph,
                                                  *isa_features.AsX86_64InstructionSetFeatures(),
                                                  compiler_options,
                                                  stats));
    }
#endif
    default:
      return nullptr;
  }
}

size_t CodeGenerator::ComputeStackMapsSize() {
  return stack_map_stream_.PrepareForFillIn();
}

static void CheckCovers(uint32_t dex_pc,
                        const HGraph& graph,
                        const CodeInfo& code_info,
                        const ArenaVector<HSuspendCheck*>& loop_headers,
                        ArenaVector<size_t>* covered) {
  CodeInfoEncoding encoding = code_info.ExtractEncoding();
  for (size_t i = 0; i < loop_headers.size(); ++i) {
    if (loop_headers[i]->GetDexPc() == dex_pc) {
      if (graph.IsCompilingOsr()) {
        DCHECK(code_info.GetOsrStackMapForDexPc(dex_pc, encoding).IsValid());
      }
      ++(*covered)[i];
    }
  }
}

// Debug helper to ensure loop entries in compiled code are matched by
// dex branch instructions.
static void CheckLoopEntriesCanBeUsedForOsr(const HGraph& graph,
                                            const CodeInfo& code_info,
                                            const DexFile::CodeItem& code_item) {
  if (graph.HasTryCatch()) {
    // One can write loops through try/catch, which we do not support for OSR anyway.
    return;
  }
  ArenaVector<HSuspendCheck*> loop_headers(graph.GetArena()->Adapter(kArenaAllocMisc));
  for (HReversePostOrderIterator it(graph); !it.Done(); it.Advance()) {
    if (it.Current()->IsLoopHeader()) {
      HSuspendCheck* suspend_check = it.Current()->GetLoopInformation()->GetSuspendCheck();
      if (!suspend_check->GetEnvironment()->IsFromInlinedInvoke()) {
        loop_headers.push_back(suspend_check);
      }
    }
  }
  ArenaVector<size_t> covered(loop_headers.size(), 0, graph.GetArena()->Adapter(kArenaAllocMisc));
  const uint16_t* code_ptr = code_item.insns_;
  const uint16_t* code_end = code_item.insns_ + code_item.insns_size_in_code_units_;

  size_t dex_pc = 0;
  while (code_ptr < code_end) {
    const Instruction& instruction = *Instruction::At(code_ptr);
    if (instruction.IsBranch()) {
      uint32_t target = dex_pc + instruction.GetTargetOffset();
      CheckCovers(target, graph, code_info, loop_headers, &covered);
    } else if (instruction.IsSwitch()) {
      DexSwitchTable table(instruction, dex_pc);
      uint16_t num_entries = table.GetNumEntries();
      size_t offset = table.GetFirstValueIndex();

      // Use a larger loop counter type to avoid overflow issues.
      for (size_t i = 0; i < num_entries; ++i) {
        // The target of the case.
        uint32_t target = dex_pc + table.GetEntryAt(i + offset);
        CheckCovers(target, graph, code_info, loop_headers, &covered);
      }
    }
    dex_pc += instruction.SizeInCodeUnits();
    code_ptr += instruction.SizeInCodeUnits();
  }

  for (size_t i = 0; i < covered.size(); ++i) {
    DCHECK_NE(covered[i], 0u) << "Loop in compiled code has no dex branch equivalent";
  }
}

void CodeGenerator::BuildStackMaps(MemoryRegion region, const DexFile::CodeItem& code_item) {
  stack_map_stream_.FillIn(region);
  if (kIsDebugBuild) {
    CheckLoopEntriesCanBeUsedForOsr(*graph_, CodeInfo(region), code_item);
  }
}

void CodeGenerator::RecordPcInfo(HInstruction* instruction,
                                 uint32_t dex_pc,
                                 SlowPathCode* slow_path) {
  if (instruction != nullptr) {
    // The code generated for some type conversions
    // may call the runtime, thus normally requiring a subsequent
    // call to this method. However, the method verifier does not
    // produce PC information for certain instructions, which are
    // considered "atomic" (they cannot join a GC).
    // Therefore we do not currently record PC information for such
    // instructions.  As this may change later, we added this special
    // case so that code generators may nevertheless call
    // CodeGenerator::RecordPcInfo without triggering an error in
    // CodeGenerator::BuildNativeGCMap ("Missing ref for dex pc 0x")
    // thereafter.
    if (instruction->IsTypeConversion()) {
      return;
    }
    if (instruction->IsRem()) {
      Primitive::Type type = instruction->AsRem()->GetResultType();
      if ((type == Primitive::kPrimFloat) || (type == Primitive::kPrimDouble)) {
        return;
      }
    }
  }

  uint32_t outer_dex_pc = dex_pc;
  uint32_t outer_environment_size = 0;
  uint32_t inlining_depth = 0;
  if (instruction != nullptr) {
    for (HEnvironment* environment = instruction->GetEnvironment();
         environment != nullptr;
         environment = environment->GetParent()) {
      outer_dex_pc = environment->GetDexPc();
      outer_environment_size = environment->Size();
      if (environment != instruction->GetEnvironment()) {
        inlining_depth++;
      }
    }
  }

  // Collect PC infos for the mapping table.
  uint32_t native_pc = GetAssembler()->CodeSize();

  if (instruction == nullptr) {
    // For stack overflow checks and native-debug-info entries without dex register
    // mapping (i.e. start of basic block or start of slow path).
    stack_map_stream_.BeginStackMapEntry(outer_dex_pc, native_pc, 0, 0, 0, 0);
    stack_map_stream_.EndStackMapEntry();
    return;
  }
  LocationSummary* locations = instruction->GetLocations();

  uint32_t register_mask = locations->GetRegisterMask();
  if (locations->OnlyCallsOnSlowPath()) {
    // In case of slow path, we currently set the location of caller-save registers
    // to register (instead of their stack location when pushed before the slow-path
    // call). Therefore register_mask contains both callee-save and caller-save
    // registers that hold objects. We must remove the caller-save from the mask, since
    // they will be overwritten by the callee.
    register_mask &= core_callee_save_mask_;
  }
  // The register mask must be a subset of callee-save registers.
  DCHECK_EQ(register_mask & core_callee_save_mask_, register_mask);
  stack_map_stream_.BeginStackMapEntry(outer_dex_pc,
                                       native_pc,
                                       register_mask,
                                       locations->GetStackMask(),
                                       outer_environment_size,
                                       inlining_depth);

  EmitEnvironment(instruction->GetEnvironment(), slow_path);
  stack_map_stream_.EndStackMapEntry();

  HLoopInformation* info = instruction->GetBlock()->GetLoopInformation();
  if (instruction->IsSuspendCheck() &&
      (info != nullptr) &&
      graph_->IsCompilingOsr() &&
      (inlining_depth == 0)) {
    DCHECK_EQ(info->GetSuspendCheck(), instruction);
    // We duplicate the stack map as a marker that this stack map can be an OSR entry.
    // Duplicating it avoids having the runtime recognize and skip an OSR stack map.
    DCHECK(info->IsIrreducible());
    stack_map_stream_.BeginStackMapEntry(
        dex_pc, native_pc, register_mask, locations->GetStackMask(), outer_environment_size, 0);
    EmitEnvironment(instruction->GetEnvironment(), slow_path);
    stack_map_stream_.EndStackMapEntry();
    if (kIsDebugBuild) {
      HEnvironment* environment = instruction->GetEnvironment();
      for (size_t i = 0, environment_size = environment->Size(); i < environment_size; ++i) {
        HInstruction* in_environment = environment->GetInstructionAt(i);
        if (in_environment != nullptr) {
          DCHECK(in_environment->IsPhi() || in_environment->IsConstant());
          Location location = environment->GetLocationAt(i);
          DCHECK(location.IsStackSlot() ||
                 location.IsDoubleStackSlot() ||
                 location.IsConstant() ||
                 location.IsInvalid());
          if (location.IsStackSlot() || location.IsDoubleStackSlot()) {
            DCHECK_LT(location.GetStackIndex(), static_cast<int32_t>(GetFrameSize()));
          }
        }
      }
    }
  } else if (kIsDebugBuild) {
    // Ensure stack maps are unique, by checking that the native pc in the stack map
    // last emitted is different than the native pc of the stack map just emitted.
    size_t number_of_stack_maps = stack_map_stream_.GetNumberOfStackMaps();
    if (number_of_stack_maps > 1) {
      DCHECK_NE(stack_map_stream_.GetStackMap(number_of_stack_maps - 1).native_pc_offset,
                stack_map_stream_.GetStackMap(number_of_stack_maps - 2).native_pc_offset);
    }
  }
}

bool CodeGenerator::HasStackMapAtCurrentPc() {
  uint32_t pc = GetAssembler()->CodeSize();
  size_t count = stack_map_stream_.GetNumberOfStackMaps();
  return count > 0 && stack_map_stream_.GetStackMap(count - 1).native_pc_offset == pc;
}

void CodeGenerator::MaybeRecordNativeDebugInfo(HInstruction* instruction,
                                               uint32_t dex_pc,
                                               SlowPathCode* slow_path) {
  if (GetCompilerOptions().GetNativeDebuggable() && dex_pc != kNoDexPc) {
    if (HasStackMapAtCurrentPc()) {
      // Ensure that we do not collide with the stack map of the previous instruction.
      GenerateNop();
    }
    RecordPcInfo(instruction, dex_pc, slow_path);
  }
}

void CodeGenerator::RecordCatchBlockInfo() {
  ArenaAllocator* arena = graph_->GetArena();

  for (HBasicBlock* block : *block_order_) {
    if (!block->IsCatchBlock()) {
      continue;
    }

    uint32_t dex_pc = block->GetDexPc();
    uint32_t num_vregs = graph_->GetNumberOfVRegs();
    uint32_t inlining_depth = 0;  // Inlining of catch blocks is not supported at the moment.
    uint32_t native_pc = GetAddressOf(block);
    uint32_t register_mask = 0;   // Not used.

    // The stack mask is not used, so we leave it empty.
    ArenaBitVector* stack_mask =
        ArenaBitVector::Create(arena, 0, /* expandable */ true, kArenaAllocCodeGenerator);

    stack_map_stream_.BeginStackMapEntry(dex_pc,
                                         native_pc,
                                         register_mask,
                                         stack_mask,
                                         num_vregs,
                                         inlining_depth);

    HInstruction* current_phi = block->GetFirstPhi();
    for (size_t vreg = 0; vreg < num_vregs; ++vreg) {
    while (current_phi != nullptr && current_phi->AsPhi()->GetRegNumber() < vreg) {
      HInstruction* next_phi = current_phi->GetNext();
      DCHECK(next_phi == nullptr ||
             current_phi->AsPhi()->GetRegNumber() <= next_phi->AsPhi()->GetRegNumber())
          << "Phis need to be sorted by vreg number to keep this a linear-time loop.";
      current_phi = next_phi;
    }

      if (current_phi == nullptr || current_phi->AsPhi()->GetRegNumber() != vreg) {
        stack_map_stream_.AddDexRegisterEntry(DexRegisterLocation::Kind::kNone, 0);
      } else {
        Location location = current_phi->GetLiveInterval()->ToLocation();
        switch (location.GetKind()) {
          case Location::kStackSlot: {
            stack_map_stream_.AddDexRegisterEntry(
                DexRegisterLocation::Kind::kInStack, location.GetStackIndex());
            break;
          }
          case Location::kDoubleStackSlot: {
            stack_map_stream_.AddDexRegisterEntry(
                DexRegisterLocation::Kind::kInStack, location.GetStackIndex());
            stack_map_stream_.AddDexRegisterEntry(
                DexRegisterLocation::Kind::kInStack, location.GetHighStackIndex(kVRegSize));
            ++vreg;
            DCHECK_LT(vreg, num_vregs);
            break;
          }
          default: {
            // All catch phis must be allocated to a stack slot.
            LOG(FATAL) << "Unexpected kind " << location.GetKind();
            UNREACHABLE();
          }
        }
      }
    }

    stack_map_stream_.EndStackMapEntry();
  }
}

void CodeGenerator::EmitEnvironment(HEnvironment* environment, SlowPathCode* slow_path) {
  if (environment == nullptr) return;

  if (environment->GetParent() != nullptr) {
    // We emit the parent environment first.
    EmitEnvironment(environment->GetParent(), slow_path);
    stack_map_stream_.BeginInlineInfoEntry(environment->GetMethodIdx(),
                                           environment->GetDexPc(),
                                           environment->GetInvokeType(),
                                           environment->Size());
  }

  // Walk over the environment, and record the location of dex registers.
  for (size_t i = 0, environment_size = environment->Size(); i < environment_size; ++i) {
    HInstruction* current = environment->GetInstructionAt(i);
    if (current == nullptr) {
      stack_map_stream_.AddDexRegisterEntry(DexRegisterLocation::Kind::kNone, 0);
      continue;
    }

    Location location = environment->GetLocationAt(i);
    switch (location.GetKind()) {
      case Location::kConstant: {
        DCHECK_EQ(current, location.GetConstant());
        if (current->IsLongConstant()) {
          int64_t value = current->AsLongConstant()->GetValue();
          stack_map_stream_.AddDexRegisterEntry(
              DexRegisterLocation::Kind::kConstant, Low32Bits(value));
          stack_map_stream_.AddDexRegisterEntry(
              DexRegisterLocation::Kind::kConstant, High32Bits(value));
          ++i;
          DCHECK_LT(i, environment_size);
        } else if (current->IsDoubleConstant()) {
          int64_t value = bit_cast<int64_t, double>(current->AsDoubleConstant()->GetValue());
          stack_map_stream_.AddDexRegisterEntry(
              DexRegisterLocation::Kind::kConstant, Low32Bits(value));
          stack_map_stream_.AddDexRegisterEntry(
              DexRegisterLocation::Kind::kConstant, High32Bits(value));
          ++i;
          DCHECK_LT(i, environment_size);
        } else if (current->IsIntConstant()) {
          int32_t value = current->AsIntConstant()->GetValue();
          stack_map_stream_.AddDexRegisterEntry(DexRegisterLocation::Kind::kConstant, value);
        } else if (current->IsNullConstant()) {
          stack_map_stream_.AddDexRegisterEntry(DexRegisterLocation::Kind::kConstant, 0);
        } else {
          DCHECK(current->IsFloatConstant()) << current->DebugName();
          int32_t value = bit_cast<int32_t, float>(current->AsFloatConstant()->GetValue());
          stack_map_stream_.AddDexRegisterEntry(DexRegisterLocation::Kind::kConstant, value);
        }
        break;
      }

      case Location::kStackSlot: {
        stack_map_stream_.AddDexRegisterEntry(
            DexRegisterLocation::Kind::kInStack, location.GetStackIndex());
        break;
      }

      case Location::kDoubleStackSlot: {
        stack_map_stream_.AddDexRegisterEntry(
            DexRegisterLocation::Kind::kInStack, location.GetStackIndex());
        stack_map_stream_.AddDexRegisterEntry(
            DexRegisterLocation::Kind::kInStack, location.GetHighStackIndex(kVRegSize));
        ++i;
        DCHECK_LT(i, environment_size);
        break;
      }

      case Location::kRegister : {
        int id = location.reg();
        if (slow_path != nullptr && slow_path->IsCoreRegisterSaved(id)) {
          uint32_t offset = slow_path->GetStackOffsetOfCoreRegister(id);
          stack_map_stream_.AddDexRegisterEntry(DexRegisterLocation::Kind::kInStack, offset);
          if (current->GetType() == Primitive::kPrimLong) {
            stack_map_stream_.AddDexRegisterEntry(
                DexRegisterLocation::Kind::kInStack, offset + kVRegSize);
            ++i;
            DCHECK_LT(i, environment_size);
          }
        } else {
          stack_map_stream_.AddDexRegisterEntry(DexRegisterLocation::Kind::kInRegister, id);
          if (current->GetType() == Primitive::kPrimLong) {
            stack_map_stream_.AddDexRegisterEntry(DexRegisterLocation::Kind::kInRegisterHigh, id);
            ++i;
            DCHECK_LT(i, environment_size);
          }
        }
        break;
      }

      case Location::kFpuRegister : {
        int id = location.reg();
        if (slow_path != nullptr && slow_path->IsFpuRegisterSaved(id)) {
          uint32_t offset = slow_path->GetStackOffsetOfFpuRegister(id);
          stack_map_stream_.AddDexRegisterEntry(DexRegisterLocation::Kind::kInStack, offset);
          if (current->GetType() == Primitive::kPrimDouble) {
            stack_map_stream_.AddDexRegisterEntry(
                DexRegisterLocation::Kind::kInStack, offset + kVRegSize);
            ++i;
            DCHECK_LT(i, environment_size);
          }
        } else {
          stack_map_stream_.AddDexRegisterEntry(DexRegisterLocation::Kind::kInFpuRegister, id);
          if (current->GetType() == Primitive::kPrimDouble) {
            stack_map_stream_.AddDexRegisterEntry(
                DexRegisterLocation::Kind::kInFpuRegisterHigh, id);
            ++i;
            DCHECK_LT(i, environment_size);
          }
        }
        break;
      }

      case Location::kFpuRegisterPair : {
        int low = location.low();
        int high = location.high();
        if (slow_path != nullptr && slow_path->IsFpuRegisterSaved(low)) {
          uint32_t offset = slow_path->GetStackOffsetOfFpuRegister(low);
          stack_map_stream_.AddDexRegisterEntry(DexRegisterLocation::Kind::kInStack, offset);
        } else {
          stack_map_stream_.AddDexRegisterEntry(DexRegisterLocation::Kind::kInFpuRegister, low);
        }
        if (slow_path != nullptr && slow_path->IsFpuRegisterSaved(high)) {
          uint32_t offset = slow_path->GetStackOffsetOfFpuRegister(high);
          stack_map_stream_.AddDexRegisterEntry(DexRegisterLocation::Kind::kInStack, offset);
          ++i;
        } else {
          stack_map_stream_.AddDexRegisterEntry(DexRegisterLocation::Kind::kInFpuRegister, high);
          ++i;
        }
        DCHECK_LT(i, environment_size);
        break;
      }

      case Location::kRegisterPair : {
        int low = location.low();
        int high = location.high();
        if (slow_path != nullptr && slow_path->IsCoreRegisterSaved(low)) {
          uint32_t offset = slow_path->GetStackOffsetOfCoreRegister(low);
          stack_map_stream_.AddDexRegisterEntry(DexRegisterLocation::Kind::kInStack, offset);
        } else {
          stack_map_stream_.AddDexRegisterEntry(DexRegisterLocation::Kind::kInRegister, low);
        }
        if (slow_path != nullptr && slow_path->IsCoreRegisterSaved(high)) {
          uint32_t offset = slow_path->GetStackOffsetOfCoreRegister(high);
          stack_map_stream_.AddDexRegisterEntry(DexRegisterLocation::Kind::kInStack, offset);
        } else {
          stack_map_stream_.AddDexRegisterEntry(DexRegisterLocation::Kind::kInRegister, high);
        }
        ++i;
        DCHECK_LT(i, environment_size);
        break;
      }

      case Location::kInvalid: {
        stack_map_stream_.AddDexRegisterEntry(DexRegisterLocation::Kind::kNone, 0);
        break;
      }

      default:
        LOG(FATAL) << "Unexpected kind " << location.GetKind();
    }
  }

  if (environment->GetParent() != nullptr) {
    stack_map_stream_.EndInlineInfoEntry();
  }
}

bool CodeGenerator::IsImplicitNullCheckAllowed(HNullCheck* null_check) const {
  return compiler_options_.GetImplicitNullChecks() &&
         // Null checks which might throw into a catch block need to save live
         // registers and therefore cannot be done implicitly.
         !null_check->CanThrowIntoCatchBlock();
}

bool CodeGenerator::CanMoveNullCheckToUser(HNullCheck* null_check) {
  HInstruction* first_next_not_move = null_check->GetNextDisregardingMoves();

  return (first_next_not_move != nullptr)
      && first_next_not_move->CanDoImplicitNullCheckOn(null_check->InputAt(0));
}

void CodeGenerator::MaybeRecordImplicitNullCheck(HInstruction* instr) {
  // If we are from a static path don't record the pc as we can't throw NPE.
  // NB: having the checks here makes the code much less verbose in the arch
  // specific code generators.
  if (instr->IsStaticFieldSet() || instr->IsStaticFieldGet()) {
    return;
  }

  if (!instr->CanDoImplicitNullCheckOn(instr->InputAt(0))) {
    return;
  }

  // Find the first previous instruction which is not a move.
  HInstruction* first_prev_not_move = instr->GetPreviousDisregardingMoves();

  // If the instruction is a null check it means that `instr` is the first user
  // and needs to record the pc.
  if (first_prev_not_move != nullptr && first_prev_not_move->IsNullCheck()) {
    HNullCheck* null_check = first_prev_not_move->AsNullCheck();
    if (IsImplicitNullCheckAllowed(null_check)) {
      // TODO: The parallel moves modify the environment. Their changes need to be
      // reverted otherwise the stack maps at the throw point will not be correct.
      RecordPcInfo(null_check, null_check->GetDexPc());
    }
  }
}

void CodeGenerator::GenerateNullCheck(HNullCheck* instruction) {
  if (IsImplicitNullCheckAllowed(instruction)) {
    MaybeRecordStat(kImplicitNullCheckGenerated);
    GenerateImplicitNullCheck(instruction);
  } else {
    MaybeRecordStat(kExplicitNullCheckGenerated);
    GenerateExplicitNullCheck(instruction);
  }
}

void CodeGenerator::ClearSpillSlotsFromLoopPhisInStackMap(HSuspendCheck* suspend_check) const {
  LocationSummary* locations = suspend_check->GetLocations();
  HBasicBlock* block = suspend_check->GetBlock();
  DCHECK(block->GetLoopInformation()->GetSuspendCheck() == suspend_check);
  DCHECK(block->IsLoopHeader());

  for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    HInstruction* current = it.Current();
    LiveInterval* interval = current->GetLiveInterval();
    // We only need to clear bits of loop phis containing objects and allocated in register.
    // Loop phis allocated on stack already have the object in the stack.
    if (current->GetType() == Primitive::kPrimNot
        && interval->HasRegister()
        && interval->HasSpillSlot()) {
      locations->ClearStackBit(interval->GetSpillSlot() / kVRegSize);
    }
  }
}

void CodeGenerator::EmitParallelMoves(Location from1,
                                      Location to1,
                                      Primitive::Type type1,
                                      Location from2,
                                      Location to2,
                                      Primitive::Type type2) {
  HParallelMove parallel_move(GetGraph()->GetArena());
  parallel_move.AddMove(from1, to1, type1, nullptr);
  parallel_move.AddMove(from2, to2, type2, nullptr);
  GetMoveResolver()->EmitNativeCode(&parallel_move);
}

void CodeGenerator::ValidateInvokeRuntime(HInstruction* instruction, SlowPathCode* slow_path) {
  // Ensure that the call kind indication given to the register allocator is
  // coherent with the runtime call generated, and that the GC side effect is
  // set when required.
  if (slow_path == nullptr) {
    DCHECK(instruction->GetLocations()->WillCall())
        << "instruction->DebugName()=" << instruction->DebugName();
    DCHECK(instruction->GetSideEffects().Includes(SideEffects::CanTriggerGC()))
        << "instruction->DebugName()=" << instruction->DebugName()
        << " instruction->GetSideEffects().ToString()=" << instruction->GetSideEffects().ToString();
  } else {
    DCHECK(instruction->GetLocations()->OnlyCallsOnSlowPath() || slow_path->IsFatal())
        << "instruction->DebugName()=" << instruction->DebugName()
        << " slow_path->GetDescription()=" << slow_path->GetDescription();
    DCHECK(instruction->GetSideEffects().Includes(SideEffects::CanTriggerGC()) ||
           // When read barriers are enabled, some instructions use a
           // slow path to emit a read barrier, which does not trigger
           // GC, is not fatal, nor is emitted by HDeoptimize
           // instructions.
           (kEmitCompilerReadBarrier &&
            (instruction->IsInstanceFieldGet() ||
             instruction->IsStaticFieldGet() ||
             instruction->IsArraySet() ||
             instruction->IsArrayGet() ||
             instruction->IsLoadClass() ||
             instruction->IsLoadString() ||
             instruction->IsInstanceOf() ||
             instruction->IsCheckCast())))
        << "instruction->DebugName()=" << instruction->DebugName()
        << " instruction->GetSideEffects().ToString()=" << instruction->GetSideEffects().ToString()
        << " slow_path->GetDescription()=" << slow_path->GetDescription();
  }

  // Check the coherency of leaf information.
  DCHECK(instruction->IsSuspendCheck()
         || ((slow_path != nullptr) && slow_path->IsFatal())
         || instruction->GetLocations()->CanCall()
         || !IsLeafMethod())
      << instruction->DebugName() << ((slow_path != nullptr) ? slow_path->GetDescription() : "");
}

void SlowPathCode::SaveLiveRegisters(CodeGenerator* codegen, LocationSummary* locations) {
  RegisterSet* live_registers = locations->GetLiveRegisters();
  size_t stack_offset = codegen->GetFirstRegisterSlotInSlowPath();

  for (size_t i = 0, e = codegen->GetNumberOfCoreRegisters(); i < e; ++i) {
    if (!codegen->IsCoreCalleeSaveRegister(i)) {
      if (live_registers->ContainsCoreRegister(i)) {
        // If the register holds an object, update the stack mask.
        if (locations->RegisterContainsObject(i)) {
          locations->SetStackBit(stack_offset / kVRegSize);
        }
        DCHECK_LT(stack_offset, codegen->GetFrameSize() - codegen->FrameEntrySpillSize());
        DCHECK_LT(i, kMaximumNumberOfExpectedRegisters);
        saved_core_stack_offsets_[i] = stack_offset;
        stack_offset += codegen->SaveCoreRegister(stack_offset, i);
      }
    }
  }

  for (size_t i = 0, e = codegen->GetNumberOfFloatingPointRegisters(); i < e; ++i) {
    if (!codegen->IsFloatingPointCalleeSaveRegister(i)) {
      if (live_registers->ContainsFloatingPointRegister(i)) {
        DCHECK_LT(stack_offset, codegen->GetFrameSize() - codegen->FrameEntrySpillSize());
        DCHECK_LT(i, kMaximumNumberOfExpectedRegisters);
        saved_fpu_stack_offsets_[i] = stack_offset;
        stack_offset += codegen->SaveFloatingPointRegister(stack_offset, i);
      }
    }
  }
}

void SlowPathCode::RestoreLiveRegisters(CodeGenerator* codegen, LocationSummary* locations) {
  RegisterSet* live_registers = locations->GetLiveRegisters();
  size_t stack_offset = codegen->GetFirstRegisterSlotInSlowPath();

  for (size_t i = 0, e = codegen->GetNumberOfCoreRegisters(); i < e; ++i) {
    if (!codegen->IsCoreCalleeSaveRegister(i)) {
      if (live_registers->ContainsCoreRegister(i)) {
        DCHECK_LT(stack_offset, codegen->GetFrameSize() - codegen->FrameEntrySpillSize());
        DCHECK_LT(i, kMaximumNumberOfExpectedRegisters);
        stack_offset += codegen->RestoreCoreRegister(stack_offset, i);
      }
    }
  }

  for (size_t i = 0, e = codegen->GetNumberOfFloatingPointRegisters(); i < e; ++i) {
    if (!codegen->IsFloatingPointCalleeSaveRegister(i)) {
      if (live_registers->ContainsFloatingPointRegister(i)) {
        DCHECK_LT(stack_offset, codegen->GetFrameSize() - codegen->FrameEntrySpillSize());
        DCHECK_LT(i, kMaximumNumberOfExpectedRegisters);
        stack_offset += codegen->RestoreFloatingPointRegister(stack_offset, i);
      }
    }
  }
}

void CodeGenerator::CreateSystemArrayCopyLocationSummary(HInvoke* invoke) {
  // Check to see if we have known failures that will cause us to have to bail out
  // to the runtime, and just generate the runtime call directly.
  HIntConstant* src_pos = invoke->InputAt(1)->AsIntConstant();
  HIntConstant* dest_pos = invoke->InputAt(3)->AsIntConstant();

  // The positions must be non-negative.
  if ((src_pos != nullptr && src_pos->GetValue() < 0) ||
      (dest_pos != nullptr && dest_pos->GetValue() < 0)) {
    // We will have to fail anyways.
    return;
  }

  // The length must be >= 0.
  HIntConstant* length = invoke->InputAt(4)->AsIntConstant();
  if (length != nullptr) {
    int32_t len = length->GetValue();
    if (len < 0) {
      // Just call as normal.
      return;
    }
  }

  SystemArrayCopyOptimizations optimizations(invoke);

  if (optimizations.GetDestinationIsSource()) {
    if (src_pos != nullptr && dest_pos != nullptr && src_pos->GetValue() < dest_pos->GetValue()) {
      // We only support backward copying if source and destination are the same.
      return;
    }
  }

  if (optimizations.GetDestinationIsPrimitiveArray() || optimizations.GetSourceIsPrimitiveArray()) {
    // We currently don't intrinsify primitive copying.
    return;
  }

  ArenaAllocator* allocator = invoke->GetBlock()->GetGraph()->GetArena();
  LocationSummary* locations = new (allocator) LocationSummary(invoke,
                                                               LocationSummary::kCallOnSlowPath,
                                                               kIntrinsified);
  // arraycopy(Object src, int src_pos, Object dest, int dest_pos, int length).
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(invoke->InputAt(1)));
  locations->SetInAt(2, Location::RequiresRegister());
  locations->SetInAt(3, Location::RegisterOrConstant(invoke->InputAt(3)));
  locations->SetInAt(4, Location::RegisterOrConstant(invoke->InputAt(4)));

  locations->AddTemp(Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
}

}  // namespace art
