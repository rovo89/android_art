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

#include "compiled_method.h"
#include "dex/verified_method.h"
#include "driver/compiler_driver.h"
#include "gc_map_builder.h"
#include "graph_visualizer.h"
#include "intrinsics.h"
#include "leb128.h"
#include "mapping_table.h"
#include "mirror/array-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/object_reference.h"
#include "parallel_move_resolver.h"
#include "ssa_liveness_analysis.h"
#include "utils/assembler.h"
#include "verifier/dex_gc_map.h"
#include "vmap_table.h"

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

void CodeGenerator::CompileBaseline(CodeAllocator* allocator, bool is_leaf) {
  Initialize();
  if (!is_leaf) {
    MarkNotLeaf();
  }
  const bool is_64_bit = Is64BitInstructionSet(GetInstructionSet());
  InitializeCodeGeneration(GetGraph()->GetNumberOfLocalVRegs()
                             + GetGraph()->GetTemporariesVRegSlots()
                             + 1 /* filler */,
                           0, /* the baseline compiler does not have live registers at slow path */
                           0, /* the baseline compiler does not have live registers at slow path */
                           GetGraph()->GetMaximumNumberOfOutVRegs()
                             + (is_64_bit ? 2 : 1) /* current method */,
                           GetGraph()->GetBlocks());
  CompileInternal(allocator, /* is_baseline */ true);
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
  for (SlowPathCode* slow_path : slow_paths_) {
    current_slow_path_ = slow_path;
    if (disasm_info_ != nullptr) {
      code_start = GetAssembler()->CodeSize();
    }
    slow_path->EmitNativeCode(this);
    if (disasm_info_ != nullptr) {
      disasm_info_->AddSlowPathInterval(slow_path, code_start, GetAssembler()->CodeSize());
    }
  }
  current_slow_path_ = nullptr;
}

void CodeGenerator::CompileInternal(CodeAllocator* allocator, bool is_baseline) {
  is_baseline_ = is_baseline;
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
    for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
      HInstruction* current = it.Current();
      DisassemblyScope disassembly_scope(current, *this);
      if (is_baseline) {
        InitLocationsBaseline(current);
      }
      DCHECK(CheckTypeConsistency(current));
      current->Accept(instruction_visitor);
    }
  }

  GenerateSlowPaths();

  // Emit catch stack maps at the end of the stack map stream as expected by the
  // runtime exception handler.
  if (!is_baseline && graph_->HasTryCatch()) {
    RecordCatchBlockInfo();
  }

  // Finalize instructions in assember;
  Finalize(allocator);
}

void CodeGenerator::CompileOptimized(CodeAllocator* allocator) {
  // The register allocator already called `InitializeCodeGeneration`,
  // where the frame size has been computed.
  DCHECK(block_order_ != nullptr);
  Initialize();
  CompileInternal(allocator, /* is_baseline */ false);
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

size_t CodeGenerator::FindFreeEntry(bool* array, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (!array[i]) {
      array[i] = true;
      return i;
    }
  }
  LOG(FATAL) << "Could not find a register in baseline register allocator";
  UNREACHABLE();
}

size_t CodeGenerator::FindTwoFreeConsecutiveAlignedEntries(bool* array, size_t length) {
  for (size_t i = 0; i < length - 1; i += 2) {
    if (!array[i] && !array[i + 1]) {
      array[i] = true;
      array[i + 1] = true;
      return i;
    }
  }
  LOG(FATAL) << "Could not find a register in baseline register allocator";
  UNREACHABLE();
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

Location CodeGenerator::GetTemporaryLocation(HTemporary* temp) const {
  uint16_t number_of_locals = GetGraph()->GetNumberOfLocalVRegs();
  // The type of the previous instruction tells us if we need a single or double stack slot.
  Primitive::Type type = temp->GetType();
  int32_t temp_size = (type == Primitive::kPrimLong) || (type == Primitive::kPrimDouble) ? 2 : 1;
  // Use the temporary region (right below the dex registers).
  int32_t slot = GetFrameSize() - FrameEntrySpillSize()
                                - kVRegSize  // filler
                                - (number_of_locals * kVRegSize)
                                - ((temp_size + temp->GetIndex()) * kVRegSize);
  return temp_size == 2 ? Location::DoubleStackSlot(slot) : Location::StackSlot(slot);
}

int32_t CodeGenerator::GetStackSlot(HLocal* local) const {
  uint16_t reg_number = local->GetRegNumber();
  uint16_t number_of_locals = GetGraph()->GetNumberOfLocalVRegs();
  if (reg_number >= number_of_locals) {
    // Local is a parameter of the method. It is stored in the caller's frame.
    // TODO: Share this logic with StackVisitor::GetVRegOffsetFromQuickCode.
    return GetFrameSize() + InstructionSetPointerSize(GetInstructionSet())  // ART method
                          + (reg_number - number_of_locals) * kVRegSize;
  } else {
    // Local is a temporary in this method. It is stored in this method's frame.
    return GetFrameSize() - FrameEntrySpillSize()
                          - kVRegSize  // filler.
                          - (number_of_locals * kVRegSize)
                          + (reg_number * kVRegSize);
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

void CodeGenerator::AllocateRegistersLocally(HInstruction* instruction) const {
  LocationSummary* locations = instruction->GetLocations();
  if (locations == nullptr) return;

  for (size_t i = 0, e = GetNumberOfCoreRegisters(); i < e; ++i) {
    blocked_core_registers_[i] = false;
  }

  for (size_t i = 0, e = GetNumberOfFloatingPointRegisters(); i < e; ++i) {
    blocked_fpu_registers_[i] = false;
  }

  for (size_t i = 0, e = number_of_register_pairs_; i < e; ++i) {
    blocked_register_pairs_[i] = false;
  }

  // Mark all fixed input, temp and output registers as used.
  for (size_t i = 0, e = locations->GetInputCount(); i < e; ++i) {
    BlockIfInRegister(locations->InAt(i));
  }

  for (size_t i = 0, e = locations->GetTempCount(); i < e; ++i) {
    Location loc = locations->GetTemp(i);
    BlockIfInRegister(loc);
  }
  Location result_location = locations->Out();
  if (locations->OutputCanOverlapWithInputs()) {
    BlockIfInRegister(result_location, /* is_out */ true);
  }

  SetupBlockedRegisters(/* is_baseline */ true);

  // Allocate all unallocated input locations.
  for (size_t i = 0, e = locations->GetInputCount(); i < e; ++i) {
    Location loc = locations->InAt(i);
    HInstruction* input = instruction->InputAt(i);
    if (loc.IsUnallocated()) {
      if ((loc.GetPolicy() == Location::kRequiresRegister)
          || (loc.GetPolicy() == Location::kRequiresFpuRegister)) {
        loc = AllocateFreeRegister(input->GetType());
      } else {
        DCHECK_EQ(loc.GetPolicy(), Location::kAny);
        HLoadLocal* load = input->AsLoadLocal();
        if (load != nullptr) {
          loc = GetStackLocation(load);
        } else {
          loc = AllocateFreeRegister(input->GetType());
        }
      }
      locations->SetInAt(i, loc);
    }
  }

  // Allocate all unallocated temp locations.
  for (size_t i = 0, e = locations->GetTempCount(); i < e; ++i) {
    Location loc = locations->GetTemp(i);
    if (loc.IsUnallocated()) {
      switch (loc.GetPolicy()) {
        case Location::kRequiresRegister:
          // Allocate a core register (large enough to fit a 32-bit integer).
          loc = AllocateFreeRegister(Primitive::kPrimInt);
          break;

        case Location::kRequiresFpuRegister:
          // Allocate a core register (large enough to fit a 64-bit double).
          loc = AllocateFreeRegister(Primitive::kPrimDouble);
          break;

        default:
          LOG(FATAL) << "Unexpected policy for temporary location "
                     << loc.GetPolicy();
      }
      locations->SetTempAt(i, loc);
    }
  }
  if (result_location.IsUnallocated()) {
    switch (result_location.GetPolicy()) {
      case Location::kAny:
      case Location::kRequiresRegister:
      case Location::kRequiresFpuRegister:
        result_location = AllocateFreeRegister(instruction->GetType());
        break;
      case Location::kSameAsFirstInput:
        result_location = locations->InAt(0);
        break;
    }
    locations->UpdateOut(result_location);
  }
}

void CodeGenerator::InitLocationsBaseline(HInstruction* instruction) {
  AllocateLocations(instruction);
  if (instruction->GetLocations() == nullptr) {
    if (instruction->IsTemporary()) {
      HInstruction* previous = instruction->GetPrevious();
      Location temp_location = GetTemporaryLocation(instruction->AsTemporary());
      Move(previous, temp_location, instruction);
    }
    return;
  }
  AllocateRegistersLocally(instruction);
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    Location location = instruction->GetLocations()->InAt(i);
    HInstruction* input = instruction->InputAt(i);
    if (location.IsValid()) {
      // Move the input to the desired location.
      if (input->GetNext()->IsTemporary()) {
        // If the input was stored in a temporary, use that temporary to
        // perform the move.
        Move(input->GetNext(), location, instruction);
      } else {
        Move(input, location, instruction);
      }
    }
  }
}

void CodeGenerator::AllocateLocations(HInstruction* instruction) {
  instruction->Accept(GetLocationBuilder());
  DCHECK(CheckTypeConsistency(instruction));
  LocationSummary* locations = instruction->GetLocations();
  if (!instruction->IsSuspendCheckEntry()) {
    if (locations != nullptr && locations->CanCall()) {
      MarkNotLeaf();
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

CodeGenerator* CodeGenerator::Create(HGraph* graph,
                                     InstructionSet instruction_set,
                                     const InstructionSetFeatures& isa_features,
                                     const CompilerOptions& compiler_options,
                                     OptimizingCompilerStats* stats) {
  switch (instruction_set) {
#ifdef ART_ENABLE_CODEGEN_arm
    case kArm:
    case kThumb2: {
      return new arm::CodeGeneratorARM(graph,
                                      *isa_features.AsArmInstructionSetFeatures(),
                                      compiler_options,
                                      stats);
    }
#endif
#ifdef ART_ENABLE_CODEGEN_arm64
    case kArm64: {
      return new arm64::CodeGeneratorARM64(graph,
                                          *isa_features.AsArm64InstructionSetFeatures(),
                                          compiler_options,
                                          stats);
    }
#endif
#ifdef ART_ENABLE_CODEGEN_mips
    case kMips: {
      return new mips::CodeGeneratorMIPS(graph,
                                         *isa_features.AsMipsInstructionSetFeatures(),
                                         compiler_options,
                                         stats);
    }
#endif
#ifdef ART_ENABLE_CODEGEN_mips64
    case kMips64: {
      return new mips64::CodeGeneratorMIPS64(graph,
                                            *isa_features.AsMips64InstructionSetFeatures(),
                                            compiler_options,
                                            stats);
    }
#endif
#ifdef ART_ENABLE_CODEGEN_x86
    case kX86: {
      return new x86::CodeGeneratorX86(graph,
                                      *isa_features.AsX86InstructionSetFeatures(),
                                      compiler_options,
                                      stats);
    }
#endif
#ifdef ART_ENABLE_CODEGEN_x86_64
    case kX86_64: {
      return new x86_64::CodeGeneratorX86_64(graph,
                                            *isa_features.AsX86_64InstructionSetFeatures(),
                                            compiler_options,
                                            stats);
    }
#endif
    default:
      return nullptr;
  }
}

void CodeGenerator::BuildNativeGCMap(
    ArenaVector<uint8_t>* data, const CompilerDriver& compiler_driver) const {
  const std::vector<uint8_t>& gc_map_raw =
      compiler_driver.GetVerifiedMethod(&GetGraph()->GetDexFile(), GetGraph()->GetMethodIdx())
          ->GetDexGcMap();
  verifier::DexPcToReferenceMap dex_gc_map(&(gc_map_raw)[0]);

  uint32_t max_native_offset = stack_map_stream_.ComputeMaxNativePcOffset();

  size_t num_stack_maps = stack_map_stream_.GetNumberOfStackMaps();
  GcMapBuilder builder(data, num_stack_maps, max_native_offset, dex_gc_map.RegWidth());
  for (size_t i = 0; i != num_stack_maps; ++i) {
    const StackMapStream::StackMapEntry& stack_map_entry = stack_map_stream_.GetStackMap(i);
    uint32_t native_offset = stack_map_entry.native_pc_offset;
    uint32_t dex_pc = stack_map_entry.dex_pc;
    const uint8_t* references = dex_gc_map.FindBitMap(dex_pc, false);
    CHECK(references != nullptr) << "Missing ref for dex pc 0x" << std::hex << dex_pc;
    builder.AddEntry(native_offset, references);
  }
}

void CodeGenerator::BuildMappingTable(ArenaVector<uint8_t>* data) const {
  uint32_t pc2dex_data_size = 0u;
  uint32_t pc2dex_entries = stack_map_stream_.GetNumberOfStackMaps();
  uint32_t pc2dex_offset = 0u;
  int32_t pc2dex_dalvik_offset = 0;
  uint32_t dex2pc_data_size = 0u;
  uint32_t dex2pc_entries = 0u;
  uint32_t dex2pc_offset = 0u;
  int32_t dex2pc_dalvik_offset = 0;

  for (size_t i = 0; i < pc2dex_entries; i++) {
    const StackMapStream::StackMapEntry& stack_map_entry = stack_map_stream_.GetStackMap(i);
    pc2dex_data_size += UnsignedLeb128Size(stack_map_entry.native_pc_offset - pc2dex_offset);
    pc2dex_data_size += SignedLeb128Size(stack_map_entry.dex_pc - pc2dex_dalvik_offset);
    pc2dex_offset = stack_map_entry.native_pc_offset;
    pc2dex_dalvik_offset = stack_map_entry.dex_pc;
  }

  // Walk over the blocks and find which ones correspond to catch block entries.
  for (HBasicBlock* block : graph_->GetBlocks()) {
    if (block->IsCatchBlock()) {
      intptr_t native_pc = GetAddressOf(block);
      ++dex2pc_entries;
      dex2pc_data_size += UnsignedLeb128Size(native_pc - dex2pc_offset);
      dex2pc_data_size += SignedLeb128Size(block->GetDexPc() - dex2pc_dalvik_offset);
      dex2pc_offset = native_pc;
      dex2pc_dalvik_offset = block->GetDexPc();
    }
  }

  uint32_t total_entries = pc2dex_entries + dex2pc_entries;
  uint32_t hdr_data_size = UnsignedLeb128Size(total_entries) + UnsignedLeb128Size(pc2dex_entries);
  uint32_t data_size = hdr_data_size + pc2dex_data_size + dex2pc_data_size;
  data->resize(data_size);

  uint8_t* data_ptr = &(*data)[0];
  uint8_t* write_pos = data_ptr;

  write_pos = EncodeUnsignedLeb128(write_pos, total_entries);
  write_pos = EncodeUnsignedLeb128(write_pos, pc2dex_entries);
  DCHECK_EQ(static_cast<size_t>(write_pos - data_ptr), hdr_data_size);
  uint8_t* write_pos2 = write_pos + pc2dex_data_size;

  pc2dex_offset = 0u;
  pc2dex_dalvik_offset = 0u;
  dex2pc_offset = 0u;
  dex2pc_dalvik_offset = 0u;

  for (size_t i = 0; i < pc2dex_entries; i++) {
    const StackMapStream::StackMapEntry& stack_map_entry = stack_map_stream_.GetStackMap(i);
    DCHECK(pc2dex_offset <= stack_map_entry.native_pc_offset);
    write_pos = EncodeUnsignedLeb128(write_pos, stack_map_entry.native_pc_offset - pc2dex_offset);
    write_pos = EncodeSignedLeb128(write_pos, stack_map_entry.dex_pc - pc2dex_dalvik_offset);
    pc2dex_offset = stack_map_entry.native_pc_offset;
    pc2dex_dalvik_offset = stack_map_entry.dex_pc;
  }

  for (HBasicBlock* block : graph_->GetBlocks()) {
    if (block->IsCatchBlock()) {
      intptr_t native_pc = GetAddressOf(block);
      write_pos2 = EncodeUnsignedLeb128(write_pos2, native_pc - dex2pc_offset);
      write_pos2 = EncodeSignedLeb128(write_pos2, block->GetDexPc() - dex2pc_dalvik_offset);
      dex2pc_offset = native_pc;
      dex2pc_dalvik_offset = block->GetDexPc();
    }
  }


  DCHECK_EQ(static_cast<size_t>(write_pos - data_ptr), hdr_data_size + pc2dex_data_size);
  DCHECK_EQ(static_cast<size_t>(write_pos2 - data_ptr), data_size);

  if (kIsDebugBuild) {
    // Verify the encoded table holds the expected data.
    MappingTable table(data_ptr);
    CHECK_EQ(table.TotalSize(), total_entries);
    CHECK_EQ(table.PcToDexSize(), pc2dex_entries);
    auto it = table.PcToDexBegin();
    auto it2 = table.DexToPcBegin();
    for (size_t i = 0; i < pc2dex_entries; i++) {
      const StackMapStream::StackMapEntry& stack_map_entry = stack_map_stream_.GetStackMap(i);
      CHECK_EQ(stack_map_entry.native_pc_offset, it.NativePcOffset());
      CHECK_EQ(stack_map_entry.dex_pc, it.DexPc());
      ++it;
    }
    for (HBasicBlock* block : graph_->GetBlocks()) {
      if (block->IsCatchBlock()) {
        CHECK_EQ(GetAddressOf(block), it2.NativePcOffset());
        CHECK_EQ(block->GetDexPc(), it2.DexPc());
        ++it2;
      }
    }
    CHECK(it == table.PcToDexEnd());
    CHECK(it2 == table.DexToPcEnd());
  }
}

void CodeGenerator::BuildVMapTable(ArenaVector<uint8_t>* data) const {
  Leb128Encoder<ArenaVector<uint8_t>> vmap_encoder(data);
  // We currently don't use callee-saved registers.
  size_t size = 0 + 1 /* marker */ + 0;
  vmap_encoder.Reserve(size + 1u);  // All values are likely to be one byte in ULEB128 (<128).
  vmap_encoder.PushBackUnsigned(size);
  vmap_encoder.PushBackUnsigned(VmapTable::kAdjustedFpMarker);
}

size_t CodeGenerator::ComputeStackMapsSize() {
  return stack_map_stream_.PrepareForFillIn();
}

void CodeGenerator::BuildStackMaps(MemoryRegion region) {
  stack_map_stream_.FillIn(region);
}

void CodeGenerator::RecordPcInfo(HInstruction* instruction,
                                 uint32_t dex_pc,
                                 SlowPathCode* slow_path) {
  if (instruction != nullptr) {
    // The code generated for some type conversions and comparisons
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
    if (instruction->IsTypeConversion() || instruction->IsCompare()) {
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
    // For stack overflow checks.
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
}

bool CodeGenerator::HasStackMapAtCurrentPc() {
  uint32_t pc = GetAssembler()->CodeSize();
  size_t count = stack_map_stream_.GetNumberOfStackMaps();
  return count > 0 && stack_map_stream_.GetStackMap(count - 1).native_pc_offset == pc;
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
    ArenaBitVector* stack_mask = new (arena) ArenaBitVector(arena, 0, /* expandable */ true);

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
