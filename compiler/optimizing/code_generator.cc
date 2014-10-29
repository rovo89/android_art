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

size_t CodeGenerator::GetCacheOffset(uint32_t index) {
  return mirror::ObjectArray<mirror::Object>::OffsetOfElement(index).SizeValue();
}

void CodeGenerator::CompileBaseline(CodeAllocator* allocator, bool is_leaf) {
  const GrowableArray<HBasicBlock*>& blocks = GetGraph()->GetBlocks();
  DCHECK(blocks.Get(0) == GetGraph()->GetEntryBlock());
  DCHECK(GoesToNextBlock(GetGraph()->GetEntryBlock(), blocks.Get(1)));
  Initialize();

  DCHECK_EQ(frame_size_, kUninitializedFrameSize);
  if (!is_leaf) {
    MarkNotLeaf();
  }
  ComputeFrameSize(GetGraph()->GetNumberOfLocalVRegs()
                     + GetGraph()->GetNumberOfTemporaries()
                     + 1 /* filler */,
                   0, /* the baseline compiler does not have live registers at slow path */
                   GetGraph()->GetMaximumNumberOfOutVRegs()
                     + 1 /* current method */);
  GenerateFrameEntry();

  HGraphVisitor* location_builder = GetLocationBuilder();
  HGraphVisitor* instruction_visitor = GetInstructionVisitor();
  for (size_t i = 0, e = blocks.Size(); i < e; ++i) {
    HBasicBlock* block = blocks.Get(i);
    Bind(block);
    for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
      HInstruction* current = it.Current();
      current->Accept(location_builder);
      InitLocations(current);
      current->Accept(instruction_visitor);
    }
  }
  GenerateSlowPaths();

  size_t code_size = GetAssembler()->CodeSize();
  uint8_t* buffer = allocator->Allocate(code_size);
  MemoryRegion code(buffer, code_size);
  GetAssembler()->FinalizeInstructions(code);
}

void CodeGenerator::CompileOptimized(CodeAllocator* allocator) {
  // The frame size has already been computed during register allocation.
  DCHECK_NE(frame_size_, kUninitializedFrameSize);
  const GrowableArray<HBasicBlock*>& blocks = GetGraph()->GetBlocks();
  DCHECK(blocks.Get(0) == GetGraph()->GetEntryBlock());
  DCHECK(GoesToNextBlock(GetGraph()->GetEntryBlock(), blocks.Get(1)));
  Initialize();

  GenerateFrameEntry();
  HGraphVisitor* instruction_visitor = GetInstructionVisitor();
  for (size_t i = 0, e = blocks.Size(); i < e; ++i) {
    HBasicBlock* block = blocks.Get(i);
    Bind(block);
    for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
      HInstruction* current = it.Current();
      current->Accept(instruction_visitor);
    }
  }
  GenerateSlowPaths();

  size_t code_size = GetAssembler()->CodeSize();
  uint8_t* buffer = allocator->Allocate(code_size);
  MemoryRegion code(buffer, code_size);
  GetAssembler()->FinalizeInstructions(code);
}

void CodeGenerator::GenerateSlowPaths() {
  for (size_t i = 0, e = slow_paths_.Size(); i < e; ++i) {
    slow_paths_.Get(i)->EmitNativeCode(this);
  }
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
  return -1;
}

size_t CodeGenerator::FindTwoFreeConsecutiveEntries(bool* array, size_t length) {
  for (size_t i = 0; i < length - 1; ++i) {
    if (!array[i] && !array[i + 1]) {
      array[i] = true;
      array[i + 1] = true;
      return i;
    }
  }
  LOG(FATAL) << "Could not find a register in baseline register allocator";
  UNREACHABLE();
  return -1;
}

void CodeGenerator::ComputeFrameSize(size_t number_of_spill_slots,
                                     size_t maximum_number_of_live_registers,
                                     size_t number_of_out_slots) {
  first_register_slot_in_slow_path_ = (number_of_out_slots + number_of_spill_slots) * kVRegSize;

  SetFrameSize(RoundUp(
      number_of_spill_slots * kVRegSize
      + number_of_out_slots * kVRegSize
      + maximum_number_of_live_registers * GetWordSize()
      + FrameEntrySpillSize(),
      kStackAlignment));
}

Location CodeGenerator::GetTemporaryLocation(HTemporary* temp) const {
  uint16_t number_of_locals = GetGraph()->GetNumberOfLocalVRegs();
  // Use the temporary region (right below the dex registers).
  int32_t slot = GetFrameSize() - FrameEntrySpillSize()
                                - kVRegSize  // filler
                                - (number_of_locals * kVRegSize)
                                - ((1 + temp->GetIndex()) * kVRegSize);
  return Location::StackSlot(slot);
}

int32_t CodeGenerator::GetStackSlot(HLocal* local) const {
  uint16_t reg_number = local->GetRegNumber();
  uint16_t number_of_locals = GetGraph()->GetNumberOfLocalVRegs();
  if (reg_number >= number_of_locals) {
    // Local is a parameter of the method. It is stored in the caller's frame.
    return GetFrameSize() + kVRegSize  // ART method
                          + (reg_number - number_of_locals) * kVRegSize;
  } else {
    // Local is a temporary in this method. It is stored in this method's frame.
    return GetFrameSize() - FrameEntrySpillSize()
                          - kVRegSize  // filler.
                          - (number_of_locals * kVRegSize)
                          + (reg_number * kVRegSize);
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
    Location loc = locations->InAt(i);
    // The DCHECKS below check that a register is not specified twice in
    // the summary.
    if (loc.IsRegister()) {
      DCHECK(!blocked_core_registers_[loc.reg()]);
      blocked_core_registers_[loc.reg()] = true;
    } else if (loc.IsFpuRegister()) {
      DCHECK(!blocked_fpu_registers_[loc.reg()]);
      blocked_fpu_registers_[loc.reg()] = true;
    } else if (loc.IsFpuRegisterPair()) {
      DCHECK(!blocked_fpu_registers_[loc.AsFpuRegisterPairLow<int>()]);
      blocked_fpu_registers_[loc.AsFpuRegisterPairLow<int>()] = true;
      DCHECK(!blocked_fpu_registers_[loc.AsFpuRegisterPairHigh<int>()]);
      blocked_fpu_registers_[loc.AsFpuRegisterPairHigh<int>()] = true;
    } else if (loc.IsRegisterPair()) {
      DCHECK(!blocked_core_registers_[loc.AsRegisterPairLow<int>()]);
      blocked_core_registers_[loc.AsRegisterPairLow<int>()] = true;
      DCHECK(!blocked_core_registers_[loc.AsRegisterPairHigh<int>()]);
      blocked_core_registers_[loc.AsRegisterPairHigh<int>()] = true;
    }
  }

  for (size_t i = 0, e = locations->GetTempCount(); i < e; ++i) {
    Location loc = locations->GetTemp(i);
    if (loc.IsRegister()) {
      // Check that a register is not specified twice in the summary.
      DCHECK(!blocked_core_registers_[loc.reg()]);
      blocked_core_registers_[loc.reg()] = true;
    } else {
      DCHECK_EQ(loc.GetPolicy(), Location::kRequiresRegister);
    }
  }

  SetupBlockedRegisters();

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
      DCHECK_EQ(loc.GetPolicy(), Location::kRequiresRegister);
      // TODO: Adjust handling of temps. We currently consider temps to use
      // core registers. They may also use floating point registers at some point.
      loc = AllocateFreeRegister(Primitive::kPrimInt);
      locations->SetTempAt(i, loc);
    }
  }
  Location result_location = locations->Out();
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
    locations->SetOut(result_location);
  }
}

void CodeGenerator::InitLocations(HInstruction* instruction) {
  if (instruction->GetLocations() == nullptr) {
    if (instruction->IsTemporary()) {
      HInstruction* previous = instruction->GetPrevious();
      Location temp_location = GetTemporaryLocation(instruction->AsTemporary());
      Move(previous, temp_location, instruction);
      previous->GetLocations()->SetOut(temp_location);
    }
    return;
  }
  AllocateRegistersLocally(instruction);
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    Location location = instruction->GetLocations()->InAt(i);
    if (location.IsValid()) {
      // Move the input to the desired location.
      Move(instruction->InputAt(i), location, instruction);
    }
  }
}

bool CodeGenerator::GoesToNextBlock(HBasicBlock* current, HBasicBlock* next) const {
  // We currently iterate over the block in insertion order.
  return current->GetBlockId() + 1 == next->GetBlockId();
}

CodeGenerator* CodeGenerator::Create(ArenaAllocator* allocator,
                                     HGraph* graph,
                                     InstructionSet instruction_set) {
  switch (instruction_set) {
    case kArm:
    case kThumb2: {
      return new (allocator) arm::CodeGeneratorARM(graph);
    }
    case kArm64: {
      return new (allocator) arm64::CodeGeneratorARM64(graph);
    }
    case kMips:
      return nullptr;
    case kX86: {
      return new (allocator) x86::CodeGeneratorX86(graph);
    }
    case kX86_64: {
      return new (allocator) x86_64::CodeGeneratorX86_64(graph);
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
    CHECK(references != NULL) << "Missing ref for dex pc 0x" << std::hex << dex_pc;
    builder.AddEntry(native_offset, references);
  }
}

void CodeGenerator::BuildMappingTable(std::vector<uint8_t>* data, SrcMap* src_map) const {
  uint32_t pc2dex_data_size = 0u;
  uint32_t pc2dex_entries = pc_infos_.Size();
  uint32_t pc2dex_offset = 0u;
  int32_t pc2dex_dalvik_offset = 0;
  uint32_t dex2pc_data_size = 0u;
  uint32_t dex2pc_entries = 0u;

  if (src_map != nullptr) {
    src_map->reserve(pc2dex_entries);
  }

  // We currently only have pc2dex entries.
  for (size_t i = 0; i < pc2dex_entries; i++) {
    struct PcInfo pc_info = pc_infos_.Get(i);
    pc2dex_data_size += UnsignedLeb128Size(pc_info.native_pc - pc2dex_offset);
    pc2dex_data_size += SignedLeb128Size(pc_info.dex_pc - pc2dex_dalvik_offset);
    pc2dex_offset = pc_info.native_pc;
    pc2dex_dalvik_offset = pc_info.dex_pc;
    if (src_map != nullptr) {
      src_map->push_back(SrcMapElem({pc2dex_offset, pc2dex_dalvik_offset}));
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
  for (size_t i = 0; i < pc2dex_entries; i++) {
    struct PcInfo pc_info = pc_infos_.Get(i);
    DCHECK(pc2dex_offset <= pc_info.native_pc);
    write_pos = EncodeUnsignedLeb128(write_pos, pc_info.native_pc - pc2dex_offset);
    write_pos = EncodeSignedLeb128(write_pos, pc_info.dex_pc - pc2dex_dalvik_offset);
    pc2dex_offset = pc_info.native_pc;
    pc2dex_dalvik_offset = pc_info.dex_pc;
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
  uint32_t size = stack_map_stream_.ComputeNeededSize();
  data->resize(size);
  MemoryRegion region(data->data(), size);
  stack_map_stream_.FillIn(region);
}

void CodeGenerator::RecordPcInfo(HInstruction* instruction, uint32_t dex_pc) {
  // Collect PC infos for the mapping table.
  struct PcInfo pc_info;
  pc_info.dex_pc = dex_pc;
  pc_info.native_pc = GetAssembler()->CodeSize();
  pc_infos_.Add(pc_info);

  // Populate stack map information.

  if (instruction == nullptr) {
    // For stack overflow checks.
    stack_map_stream_.AddStackMapEntry(dex_pc, pc_info.native_pc, 0, 0, 0, 0);
    return;
  }

  LocationSummary* locations = instruction->GetLocations();
  HEnvironment* environment = instruction->GetEnvironment();

  size_t environment_size = instruction->EnvironmentSize();

  size_t register_mask = 0;
  size_t inlining_depth = 0;
  stack_map_stream_.AddStackMapEntry(
      dex_pc, pc_info.native_pc, register_mask,
      locations->GetStackMask(), environment_size, inlining_depth);

  // Walk over the environment, and record the location of dex registers.
  for (size_t i = 0; i < environment_size; ++i) {
    HInstruction* current = environment->GetInstructionAt(i);
    if (current == nullptr) {
      stack_map_stream_.AddDexRegisterEntry(DexRegisterMap::kNone, 0);
      continue;
    }

    Location location = locations->GetEnvironmentAt(i);
    switch (location.GetKind()) {
      case Location::kConstant: {
        DCHECK(current == location.GetConstant());
        if (current->IsLongConstant()) {
          int64_t value = current->AsLongConstant()->GetValue();
          stack_map_stream_.AddDexRegisterEntry(DexRegisterMap::kConstant, Low32Bits(value));
          stack_map_stream_.AddDexRegisterEntry(DexRegisterMap::kConstant, High32Bits(value));
          ++i;
          DCHECK_LT(i, environment_size);
        } else {
          DCHECK(current->IsIntConstant());
          int32_t value = current->AsIntConstant()->GetValue();
          stack_map_stream_.AddDexRegisterEntry(DexRegisterMap::kConstant, value);
        }
        break;
      }

      case Location::kStackSlot: {
        stack_map_stream_.AddDexRegisterEntry(DexRegisterMap::kInStack, location.GetStackIndex());
        break;
      }

      case Location::kDoubleStackSlot: {
        stack_map_stream_.AddDexRegisterEntry(DexRegisterMap::kInStack, location.GetStackIndex());
        stack_map_stream_.AddDexRegisterEntry(DexRegisterMap::kInStack,
                                              location.GetHighStackIndex(kVRegSize));
        ++i;
        DCHECK_LT(i, environment_size);
        break;
      }

      case Location::kRegister : {
        int id = location.reg();
        stack_map_stream_.AddDexRegisterEntry(DexRegisterMap::kInRegister, id);
        if (current->GetType() == Primitive::kPrimLong) {
          stack_map_stream_.AddDexRegisterEntry(DexRegisterMap::kInRegister, id);
          ++i;
          DCHECK_LT(i, environment_size);
        }
        break;
      }

      case Location::kFpuRegister : {
        int id = location.reg();
        stack_map_stream_.AddDexRegisterEntry(DexRegisterMap::kInFpuRegister, id);
        if (current->GetType() == Primitive::kPrimDouble) {
          stack_map_stream_.AddDexRegisterEntry(DexRegisterMap::kInFpuRegister, id);
          ++i;
          DCHECK_LT(i, environment_size);
        }
        break;
      }

      default:
        LOG(FATAL) << "Unexpected kind " << location.GetKind();
    }
  }
}

void CodeGenerator::SaveLiveRegisters(LocationSummary* locations) {
  RegisterSet* register_set = locations->GetLiveRegisters();
  size_t stack_offset = first_register_slot_in_slow_path_;
  for (size_t i = 0, e = GetNumberOfCoreRegisters(); i < e; ++i) {
    if (register_set->ContainsCoreRegister(i)) {
      // If the register holds an object, update the stack mask.
      if (locations->RegisterContainsObject(i)) {
        locations->SetStackBit(stack_offset / kVRegSize);
      }
      stack_offset += SaveCoreRegister(stack_offset, i);
    }
  }

  for (size_t i = 0, e = GetNumberOfFloatingPointRegisters(); i < e; ++i) {
    if (register_set->ContainsFloatingPointRegister(i)) {
      stack_offset += SaveFloatingPointRegister(stack_offset, i);
    }
  }
}

void CodeGenerator::RestoreLiveRegisters(LocationSummary* locations) {
  RegisterSet* register_set = locations->GetLiveRegisters();
  size_t stack_offset = first_register_slot_in_slow_path_;
  for (size_t i = 0, e = GetNumberOfCoreRegisters(); i < e; ++i) {
    if (register_set->ContainsCoreRegister(i)) {
      stack_offset += RestoreCoreRegister(stack_offset, i);
    }
  }

  for (size_t i = 0, e = GetNumberOfFloatingPointRegisters(); i < e; ++i) {
    if (register_set->ContainsFloatingPointRegister(i)) {
      stack_offset += RestoreFloatingPointRegister(stack_offset, i);
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

}  // namespace art
