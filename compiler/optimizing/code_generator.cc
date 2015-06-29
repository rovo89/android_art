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

#include "code_generator_arm.h"
#include "code_generator_arm64.h"
#include "code_generator_x86.h"
#include "code_generator_x86_64.h"
#include "code_generator_mips64.h"
#include "compiled_method.h"
#include "dex/verified_method.h"
#include "driver/dex_compilation_unit.h"
#include "gc_map_builder.h"
#include "leb128.h"
#include "mapping_table.h"
#include "mirror/array-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/object_reference.h"
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
  return mirror::ObjectArray<mirror::Object>::OffsetOfElement(index).SizeValue();
}

size_t CodeGenerator::GetCachePointerOffset(uint32_t index) {
  auto pointer_size = InstructionSetPointerSize(GetInstructionSet());
  return mirror::Array::DataOffset(pointer_size).Uint32Value() + pointer_size * index;
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
  DCHECK_EQ(block_order_->Get(current_block_index_), current);
  return GetNextBlockToEmit() == FirstNonEmptyBlock(next);
}

HBasicBlock* CodeGenerator::GetNextBlockToEmit() const {
  for (size_t i = current_block_index_ + 1; i < block_order_->Size(); ++i) {
    HBasicBlock* block = block_order_->Get(i);
    if (!block->IsSingleGoto()) {
      return block;
    }
  }
  return nullptr;
}

HBasicBlock* CodeGenerator::FirstNonEmptyBlock(HBasicBlock* block) const {
  while (block->IsSingleGoto()) {
    block = block->GetSuccessors().Get(0);
  }
  return block;
}

void CodeGenerator::CompileInternal(CodeAllocator* allocator, bool is_baseline) {
  is_baseline_ = is_baseline;
  HGraphVisitor* instruction_visitor = GetInstructionVisitor();
  DCHECK_EQ(current_block_index_, 0u);
  GenerateFrameEntry();
  DCHECK_EQ(GetAssembler()->cfi().GetCurrentCFAOffset(), static_cast<int>(frame_size_));
  for (size_t e = block_order_->Size(); current_block_index_ < e; ++current_block_index_) {
    HBasicBlock* block = block_order_->Get(current_block_index_);
    // Don't generate code for an empty block. Its predecessors will branch to its successor
    // directly. Also, the label of that block will not be emitted, so this helps catch
    // errors where we reference that label.
    if (block->IsSingleGoto()) continue;
    Bind(block);
    for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
      HInstruction* current = it.Current();
      if (is_baseline) {
        InitLocationsBaseline(current);
      }
      DCHECK(CheckTypeConsistency(current));
      current->Accept(instruction_visitor);
    }
  }

  // Generate the slow paths.
  for (size_t i = 0, e = slow_paths_.Size(); i < e; ++i) {
    slow_paths_.Get(i)->EmitNativeCode(this);
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
                                             size_t maximum_number_of_live_fp_registers,
                                             size_t number_of_out_slots,
                                             const GrowableArray<HBasicBlock*>& block_order) {
  block_order_ = &block_order;
  DCHECK(block_order_->Get(0) == GetGraph()->GetEntryBlock());
  ComputeSpillMask();
  first_register_slot_in_slow_path_ = (number_of_out_slots + number_of_spill_slots) * kVRegSize;

  if (number_of_spill_slots == 0
      && !HasAllocatedCalleeSaveRegisters()
      && IsLeafMethod()
      && !RequiresCurrentMethod()) {
    DCHECK_EQ(maximum_number_of_live_core_registers, 0u);
    DCHECK_EQ(maximum_number_of_live_fp_registers, 0u);
    SetFrameSize(CallPushesPC() ? GetWordSize() : 0);
  } else {
    SetFrameSize(RoundUp(
        number_of_spill_slots * kVRegSize
        + number_of_out_slots * kVRegSize
        + maximum_number_of_live_core_registers * GetWordSize()
        + maximum_number_of_live_fp_registers * GetFloatingPointSpillSlotSize()
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

CodeGenerator* CodeGenerator::Create(HGraph* graph,
                                     InstructionSet instruction_set,
                                     const InstructionSetFeatures& isa_features,
                                     const CompilerOptions& compiler_options) {
  switch (instruction_set) {
    case kArm:
    case kThumb2: {
      return new arm::CodeGeneratorARM(graph,
          *isa_features.AsArmInstructionSetFeatures(),
          compiler_options);
    }
    case kArm64: {
      return new arm64::CodeGeneratorARM64(graph,
          *isa_features.AsArm64InstructionSetFeatures(),
          compiler_options);
    }
    case kMips:
      return nullptr;
    case kMips64: {
      return new mips64::CodeGeneratorMIPS64(graph,
          *isa_features.AsMips64InstructionSetFeatures(),
          compiler_options);
    }
    case kX86: {
      return new x86::CodeGeneratorX86(graph,
           *isa_features.AsX86InstructionSetFeatures(),
           compiler_options);
    }
    case kX86_64: {
      return new x86_64::CodeGeneratorX86_64(graph,
          *isa_features.AsX86_64InstructionSetFeatures(),
          compiler_options);
    }
    default:
      return nullptr;
  }
}

void CodeGenerator::BuildNativeGCMap(
    std::vector<uint8_t>* data, const DexCompilationUnit& dex_compilation_unit) const {
  const std::vector<uint8_t>& gc_map_raw =
      dex_compilation_unit.GetVerifiedMethod()->GetDexGcMap();
  verifier::DexPcToReferenceMap dex_gc_map(&(gc_map_raw)[0]);

  uint32_t max_native_offset = 0;
  for (size_t i = 0; i < pc_infos_.Size(); i++) {
    uint32_t native_offset = pc_infos_.Get(i).native_pc;
    if (native_offset > max_native_offset) {
      max_native_offset = native_offset;
    }
  }

  GcMapBuilder builder(data, pc_infos_.Size(), max_native_offset, dex_gc_map.RegWidth());
  for (size_t i = 0; i < pc_infos_.Size(); i++) {
    struct PcInfo pc_info = pc_infos_.Get(i);
    uint32_t native_offset = pc_info.native_pc;
    uint32_t dex_pc = pc_info.dex_pc;
    const uint8_t* references = dex_gc_map.FindBitMap(dex_pc, false);
    CHECK(references != nullptr) << "Missing ref for dex pc 0x" << std::hex << dex_pc;
    builder.AddEntry(native_offset, references);
  }
}

void CodeGenerator::BuildSourceMap(DefaultSrcMap* src_map) const {
  for (size_t i = 0; i < pc_infos_.Size(); i++) {
    struct PcInfo pc_info = pc_infos_.Get(i);
    uint32_t pc2dex_offset = pc_info.native_pc;
    int32_t pc2dex_dalvik_offset = pc_info.dex_pc;
    src_map->push_back(SrcMapElem({pc2dex_offset, pc2dex_dalvik_offset}));
  }
}

void CodeGenerator::BuildMappingTable(std::vector<uint8_t>* data) const {
  uint32_t pc2dex_data_size = 0u;
  uint32_t pc2dex_entries = pc_infos_.Size();
  uint32_t pc2dex_offset = 0u;
  int32_t pc2dex_dalvik_offset = 0;
  uint32_t dex2pc_data_size = 0u;
  uint32_t dex2pc_entries = 0u;
  uint32_t dex2pc_offset = 0u;
  int32_t dex2pc_dalvik_offset = 0;

  for (size_t i = 0; i < pc2dex_entries; i++) {
    struct PcInfo pc_info = pc_infos_.Get(i);
    pc2dex_data_size += UnsignedLeb128Size(pc_info.native_pc - pc2dex_offset);
    pc2dex_data_size += SignedLeb128Size(pc_info.dex_pc - pc2dex_dalvik_offset);
    pc2dex_offset = pc_info.native_pc;
    pc2dex_dalvik_offset = pc_info.dex_pc;
  }

  // Walk over the blocks and find which ones correspond to catch block entries.
  for (size_t i = 0; i < graph_->GetBlocks().Size(); ++i) {
    HBasicBlock* block = graph_->GetBlocks().Get(i);
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
    struct PcInfo pc_info = pc_infos_.Get(i);
    DCHECK(pc2dex_offset <= pc_info.native_pc);
    write_pos = EncodeUnsignedLeb128(write_pos, pc_info.native_pc - pc2dex_offset);
    write_pos = EncodeSignedLeb128(write_pos, pc_info.dex_pc - pc2dex_dalvik_offset);
    pc2dex_offset = pc_info.native_pc;
    pc2dex_dalvik_offset = pc_info.dex_pc;
  }

  for (size_t i = 0; i < graph_->GetBlocks().Size(); ++i) {
    HBasicBlock* block = graph_->GetBlocks().Get(i);
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
      struct PcInfo pc_info = pc_infos_.Get(i);
      CHECK_EQ(pc_info.native_pc, it.NativePcOffset());
      CHECK_EQ(pc_info.dex_pc, it.DexPc());
      ++it;
    }
    for (size_t i = 0; i < graph_->GetBlocks().Size(); ++i) {
      HBasicBlock* block = graph_->GetBlocks().Get(i);
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

void CodeGenerator::BuildVMapTable(std::vector<uint8_t>* data) const {
  Leb128EncodingVector vmap_encoder;
  // We currently don't use callee-saved registers.
  size_t size = 0 + 1 /* marker */ + 0;
  vmap_encoder.Reserve(size + 1u);  // All values are likely to be one byte in ULEB128 (<128).
  vmap_encoder.PushBackUnsigned(size);
  vmap_encoder.PushBackUnsigned(VmapTable::kAdjustedFpMarker);

  *data = vmap_encoder.GetData();
}

void CodeGenerator::BuildStackMaps(std::vector<uint8_t>* data) {
  uint32_t size = stack_map_stream_.PrepareForFillIn();
  data->resize(size);
  MemoryRegion region(data->data(), size);
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

  // Collect PC infos for the mapping table.
  struct PcInfo pc_info;
  pc_info.dex_pc = dex_pc;
  pc_info.native_pc = GetAssembler()->CodeSize();
  pc_infos_.Add(pc_info);

  uint32_t inlining_depth = 0;

  if (instruction == nullptr) {
    // For stack overflow checks.
    stack_map_stream_.BeginStackMapEntry(dex_pc, pc_info.native_pc, 0, 0, 0, inlining_depth);
    stack_map_stream_.EndStackMapEntry();
    return;
  }
  LocationSummary* locations = instruction->GetLocations();
  HEnvironment* environment = instruction->GetEnvironment();
  size_t environment_size = instruction->EnvironmentSize();

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
  stack_map_stream_.BeginStackMapEntry(dex_pc,
                                       pc_info.native_pc,
                                       register_mask,
                                       locations->GetStackMask(),
                                       environment_size,
                                       inlining_depth);
  if (environment != nullptr) {
    // TODO: Handle parent environment.
    DCHECK(environment->GetParent() == nullptr);
    DCHECK_EQ(environment->GetDexPc(), dex_pc);
  }

  // Walk over the environment, and record the location of dex registers.
  for (size_t i = 0; i < environment_size; ++i) {
    HInstruction* current = environment->GetInstructionAt(i);
    if (current == nullptr) {
      stack_map_stream_.AddDexRegisterEntry(i, DexRegisterLocation::Kind::kNone, 0);
      continue;
    }

    Location location = environment->GetLocationAt(i);
    switch (location.GetKind()) {
      case Location::kConstant: {
        DCHECK_EQ(current, location.GetConstant());
        if (current->IsLongConstant()) {
          int64_t value = current->AsLongConstant()->GetValue();
          stack_map_stream_.AddDexRegisterEntry(
              i, DexRegisterLocation::Kind::kConstant, Low32Bits(value));
          stack_map_stream_.AddDexRegisterEntry(
              ++i, DexRegisterLocation::Kind::kConstant, High32Bits(value));
          DCHECK_LT(i, environment_size);
        } else if (current->IsDoubleConstant()) {
          int64_t value = bit_cast<int64_t, double>(current->AsDoubleConstant()->GetValue());
          stack_map_stream_.AddDexRegisterEntry(
              i, DexRegisterLocation::Kind::kConstant, Low32Bits(value));
          stack_map_stream_.AddDexRegisterEntry(
              ++i, DexRegisterLocation::Kind::kConstant, High32Bits(value));
          DCHECK_LT(i, environment_size);
        } else if (current->IsIntConstant()) {
          int32_t value = current->AsIntConstant()->GetValue();
          stack_map_stream_.AddDexRegisterEntry(i, DexRegisterLocation::Kind::kConstant, value);
        } else if (current->IsNullConstant()) {
          stack_map_stream_.AddDexRegisterEntry(i, DexRegisterLocation::Kind::kConstant, 0);
        } else {
          DCHECK(current->IsFloatConstant()) << current->DebugName();
          int32_t value = bit_cast<int32_t, float>(current->AsFloatConstant()->GetValue());
          stack_map_stream_.AddDexRegisterEntry(i, DexRegisterLocation::Kind::kConstant, value);
        }
        break;
      }

      case Location::kStackSlot: {
        stack_map_stream_.AddDexRegisterEntry(
            i, DexRegisterLocation::Kind::kInStack, location.GetStackIndex());
        break;
      }

      case Location::kDoubleStackSlot: {
        stack_map_stream_.AddDexRegisterEntry(
            i, DexRegisterLocation::Kind::kInStack, location.GetStackIndex());
        stack_map_stream_.AddDexRegisterEntry(
            ++i, DexRegisterLocation::Kind::kInStack, location.GetHighStackIndex(kVRegSize));
        DCHECK_LT(i, environment_size);
        break;
      }

      case Location::kRegister : {
        int id = location.reg();
        if (slow_path != nullptr && slow_path->IsCoreRegisterSaved(id)) {
          uint32_t offset = slow_path->GetStackOffsetOfCoreRegister(id);
          stack_map_stream_.AddDexRegisterEntry(i, DexRegisterLocation::Kind::kInStack, offset);
          if (current->GetType() == Primitive::kPrimLong) {
            stack_map_stream_.AddDexRegisterEntry(
                ++i, DexRegisterLocation::Kind::kInStack, offset + kVRegSize);
            DCHECK_LT(i, environment_size);
          }
        } else {
          stack_map_stream_.AddDexRegisterEntry(i, DexRegisterLocation::Kind::kInRegister, id);
          if (current->GetType() == Primitive::kPrimLong) {
            stack_map_stream_.AddDexRegisterEntry(++i, DexRegisterLocation::Kind::kInRegister, id);
            DCHECK_LT(i, environment_size);
          }
        }
        break;
      }

      case Location::kFpuRegister : {
        int id = location.reg();
        if (slow_path != nullptr && slow_path->IsFpuRegisterSaved(id)) {
          uint32_t offset = slow_path->GetStackOffsetOfFpuRegister(id);
          stack_map_stream_.AddDexRegisterEntry(i, DexRegisterLocation::Kind::kInStack, offset);
          if (current->GetType() == Primitive::kPrimDouble) {
            stack_map_stream_.AddDexRegisterEntry(
                ++i, DexRegisterLocation::Kind::kInStack, offset + kVRegSize);
            DCHECK_LT(i, environment_size);
          }
        } else {
          stack_map_stream_.AddDexRegisterEntry(i, DexRegisterLocation::Kind::kInFpuRegister, id);
          if (current->GetType() == Primitive::kPrimDouble) {
            stack_map_stream_.AddDexRegisterEntry(
                ++i, DexRegisterLocation::Kind::kInFpuRegister, id);
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
          stack_map_stream_.AddDexRegisterEntry(i, DexRegisterLocation::Kind::kInStack, offset);
        } else {
          stack_map_stream_.AddDexRegisterEntry(i, DexRegisterLocation::Kind::kInFpuRegister, low);
        }
        if (slow_path != nullptr && slow_path->IsFpuRegisterSaved(high)) {
          uint32_t offset = slow_path->GetStackOffsetOfFpuRegister(high);
          stack_map_stream_.AddDexRegisterEntry(++i, DexRegisterLocation::Kind::kInStack, offset);
        } else {
          stack_map_stream_.AddDexRegisterEntry(
              ++i, DexRegisterLocation::Kind::kInFpuRegister, high);
        }
        DCHECK_LT(i, environment_size);
        break;
      }

      case Location::kRegisterPair : {
        int low = location.low();
        int high = location.high();
        if (slow_path != nullptr && slow_path->IsCoreRegisterSaved(low)) {
          uint32_t offset = slow_path->GetStackOffsetOfCoreRegister(low);
          stack_map_stream_.AddDexRegisterEntry(i, DexRegisterLocation::Kind::kInStack, offset);
        } else {
          stack_map_stream_.AddDexRegisterEntry(i, DexRegisterLocation::Kind::kInRegister, low);
        }
        if (slow_path != nullptr && slow_path->IsCoreRegisterSaved(high)) {
          uint32_t offset = slow_path->GetStackOffsetOfCoreRegister(high);
          stack_map_stream_.AddDexRegisterEntry(++i, DexRegisterLocation::Kind::kInStack, offset);
        } else {
          stack_map_stream_.AddDexRegisterEntry(
              ++i, DexRegisterLocation::Kind::kInRegister, high);
        }
        DCHECK_LT(i, environment_size);
        break;
      }

      case Location::kInvalid: {
        stack_map_stream_.AddDexRegisterEntry(i, DexRegisterLocation::Kind::kNone, 0);
        break;
      }

      default:
        LOG(FATAL) << "Unexpected kind " << location.GetKind();
    }
  }
  stack_map_stream_.EndStackMapEntry();
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

  if (!compiler_options_.GetImplicitNullChecks()) {
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
    // TODO: The parallel moves modify the environment. Their changes need to be reverted
    // otherwise the stack maps at the throw point will not be correct.
    RecordPcInfo(null_check, null_check->GetDexPc());
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

void SlowPathCode::RecordPcInfo(CodeGenerator* codegen, HInstruction* instruction, uint32_t dex_pc) {
  codegen->RecordPcInfo(instruction, dex_pc, this);
}

void SlowPathCode::SaveLiveRegisters(CodeGenerator* codegen, LocationSummary* locations) {
  RegisterSet* register_set = locations->GetLiveRegisters();
  size_t stack_offset = codegen->GetFirstRegisterSlotInSlowPath();
  for (size_t i = 0, e = codegen->GetNumberOfCoreRegisters(); i < e; ++i) {
    if (!codegen->IsCoreCalleeSaveRegister(i)) {
      if (register_set->ContainsCoreRegister(i)) {
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
      if (register_set->ContainsFloatingPointRegister(i)) {
        DCHECK_LT(stack_offset, codegen->GetFrameSize() - codegen->FrameEntrySpillSize());
        DCHECK_LT(i, kMaximumNumberOfExpectedRegisters);
        saved_fpu_stack_offsets_[i] = stack_offset;
        stack_offset += codegen->SaveFloatingPointRegister(stack_offset, i);
      }
    }
  }
}

void SlowPathCode::RestoreLiveRegisters(CodeGenerator* codegen, LocationSummary* locations) {
  RegisterSet* register_set = locations->GetLiveRegisters();
  size_t stack_offset = codegen->GetFirstRegisterSlotInSlowPath();
  for (size_t i = 0, e = codegen->GetNumberOfCoreRegisters(); i < e; ++i) {
    if (!codegen->IsCoreCalleeSaveRegister(i)) {
      if (register_set->ContainsCoreRegister(i)) {
        DCHECK_LT(stack_offset, codegen->GetFrameSize() - codegen->FrameEntrySpillSize());
        stack_offset += codegen->RestoreCoreRegister(stack_offset, i);
      }
    }
  }

  for (size_t i = 0, e = codegen->GetNumberOfFloatingPointRegisters(); i < e; ++i) {
    if (!codegen->IsFloatingPointCalleeSaveRegister(i)) {
      if (register_set->ContainsFloatingPointRegister(i)) {
        DCHECK_LT(stack_offset, codegen->GetFrameSize() - codegen->FrameEntrySpillSize());
        stack_offset += codegen->RestoreFloatingPointRegister(stack_offset, i);
      }
    }
  }
}

}  // namespace art
