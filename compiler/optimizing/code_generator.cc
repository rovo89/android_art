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
#include "code_generator_x86.h"
#include "dex/verified_method.h"
#include "driver/dex_compilation_unit.h"
#include "gc_map_builder.h"
#include "leb128.h"
#include "mapping_table.h"
#include "utils/assembler.h"
#include "verifier/dex_gc_map.h"
#include "vmap_table.h"

namespace art {

void CodeGenerator::Compile(CodeAllocator* allocator) {
  frame_size_ = GetGraph()->GetMaximumNumberOfOutVRegs() * kWordSize;
  const GrowableArray<HBasicBlock*>* blocks = GetGraph()->GetBlocks();
  DCHECK(blocks->Get(0) == GetGraph()->GetEntryBlock());
  DCHECK(GoesToNextBlock(GetGraph()->GetEntryBlock(), blocks->Get(1)));
  CompileEntryBlock();
  for (size_t i = 1; i < blocks->Size(); i++) {
    CompileBlock(blocks->Get(i));
  }
  size_t code_size = GetAssembler()->CodeSize();
  uint8_t* buffer = allocator->Allocate(code_size);
  MemoryRegion code(buffer, code_size);
  GetAssembler()->FinalizeInstructions(code);
}

void CodeGenerator::CompileEntryBlock() {
  HGraphVisitor* location_builder = GetLocationBuilder();
  HGraphVisitor* instruction_visitor = GetInstructionVisitor();
  // The entry block contains all locals for this method. By visiting the entry block,
  // we're computing the required frame size.
  for (HInstructionIterator it(GetGraph()->GetEntryBlock()); !it.Done(); it.Advance()) {
    HInstruction* current = it.Current();
    // Instructions in the entry block should not generate code.
    if (kIsDebugBuild) {
      current->Accept(location_builder);
      DCHECK(current->GetLocations() == nullptr);
    }
    current->Accept(instruction_visitor);
  }
  GenerateFrameEntry();
}

void CodeGenerator::CompileBlock(HBasicBlock* block) {
  Bind(GetLabelOf(block));
  HGraphVisitor* location_builder = GetLocationBuilder();
  HGraphVisitor* instruction_visitor = GetInstructionVisitor();
  for (HInstructionIterator it(block); !it.Done(); it.Advance()) {
    // For each instruction, we emulate a stack-based machine, where the inputs are popped from
    // the runtime stack, and the result is pushed on the stack. We currently can do this because
    // we do not perform any code motion, and the Dex format does not reference individual
    // instructions but uses registers instead (our equivalent of HLocal).
    HInstruction* current = it.Current();
    current->Accept(location_builder);
    InitLocations(current);
    current->Accept(instruction_visitor);
  }
}

void CodeGenerator::InitLocations(HInstruction* instruction) {
  if (instruction->GetLocations() == nullptr) return;
  for (int i = 0; i < instruction->InputCount(); i++) {
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

Label* CodeGenerator::GetLabelOf(HBasicBlock* block) const {
  return block_labels_.GetRawStorage() + block->GetBlockId();
}

CodeGenerator* CodeGenerator::Create(ArenaAllocator* allocator,
                                     HGraph* graph,
                                     InstructionSet instruction_set) {
  switch (instruction_set) {
    case kArm:
    case kThumb2: {
      return new (allocator) arm::CodeGeneratorARM(graph);
    }
    case kMips:
      return nullptr;
    case kX86: {
      return new (allocator) x86::CodeGeneratorX86(graph);
    }
    case kX86_64: {
      return new (allocator) x86::CodeGeneratorX86(graph);
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

void CodeGenerator::BuildMappingTable(std::vector<uint8_t>* data) const {
  uint32_t pc2dex_data_size = 0u;
  uint32_t pc2dex_entries = pc_infos_.Size();
  uint32_t pc2dex_offset = 0u;
  int32_t pc2dex_dalvik_offset = 0;
  uint32_t dex2pc_data_size = 0u;
  uint32_t dex2pc_entries = 0u;

  // We currently only have pc2dex entries.
  for (size_t i = 0; i < pc2dex_entries; i++) {
    struct PcInfo pc_info = pc_infos_.Get(i);
    pc2dex_data_size += UnsignedLeb128Size(pc_info.native_pc - pc2dex_offset);
    pc2dex_data_size += SignedLeb128Size(pc_info.dex_pc - pc2dex_dalvik_offset);
    pc2dex_offset = pc_info.native_pc;
    pc2dex_dalvik_offset = pc_info.dex_pc;
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

}  // namespace art
