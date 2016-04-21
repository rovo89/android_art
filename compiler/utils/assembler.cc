/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "assembler.h"

#include <algorithm>
#include <vector>

#ifdef ART_ENABLE_CODEGEN_arm
#include "arm/assembler_arm32.h"
#include "arm/assembler_thumb2.h"
#endif
#ifdef ART_ENABLE_CODEGEN_arm64
#include "arm64/assembler_arm64.h"
#endif
#ifdef ART_ENABLE_CODEGEN_mips
#include "mips/assembler_mips.h"
#endif
#ifdef ART_ENABLE_CODEGEN_mips64
#include "mips64/assembler_mips64.h"
#endif
#ifdef ART_ENABLE_CODEGEN_x86
#include "x86/assembler_x86.h"
#endif
#ifdef ART_ENABLE_CODEGEN_x86_64
#include "x86_64/assembler_x86_64.h"
#endif
#include "base/casts.h"
#include "globals.h"
#include "memory_region.h"

namespace art {

AssemblerBuffer::AssemblerBuffer(ArenaAllocator* arena)
    : arena_(arena) {
  static const size_t kInitialBufferCapacity = 4 * KB;
  contents_ = arena_->AllocArray<uint8_t>(kInitialBufferCapacity, kArenaAllocAssembler);
  cursor_ = contents_;
  limit_ = ComputeLimit(contents_, kInitialBufferCapacity);
  fixup_ = nullptr;
  slow_path_ = nullptr;
#ifndef NDEBUG
  has_ensured_capacity_ = false;
  fixups_processed_ = false;
#endif

  // Verify internal state.
  CHECK_EQ(Capacity(), kInitialBufferCapacity);
  CHECK_EQ(Size(), 0U);
}


AssemblerBuffer::~AssemblerBuffer() {
  if (arena_->IsRunningOnMemoryTool()) {
    arena_->MakeInaccessible(contents_, Capacity());
  }
}


void AssemblerBuffer::ProcessFixups(const MemoryRegion& region) {
  AssemblerFixup* fixup = fixup_;
  while (fixup != nullptr) {
    fixup->Process(region, fixup->position());
    fixup = fixup->previous();
  }
}


void AssemblerBuffer::FinalizeInstructions(const MemoryRegion& instructions) {
  // Copy the instructions from the buffer.
  MemoryRegion from(reinterpret_cast<void*>(contents()), Size());
  instructions.CopyFrom(0, from);
  // Process fixups in the instructions.
  ProcessFixups(instructions);
#ifndef NDEBUG
  fixups_processed_ = true;
#endif
}


void AssemblerBuffer::ExtendCapacity(size_t min_capacity) {
  size_t old_size = Size();
  size_t old_capacity = Capacity();
  DCHECK_GT(min_capacity, old_capacity);
  size_t new_capacity = std::min(old_capacity * 2, old_capacity + 1 * MB);
  new_capacity = std::max(new_capacity, min_capacity);

  // Allocate the new data area and copy contents of the old one to it.
  contents_ = reinterpret_cast<uint8_t*>(
      arena_->Realloc(contents_, old_capacity, new_capacity, kArenaAllocAssembler));

  // Update the cursor and recompute the limit.
  cursor_ = contents_ + old_size;
  limit_ = ComputeLimit(contents_, new_capacity);

  // Verify internal state.
  CHECK_EQ(Capacity(), new_capacity);
  CHECK_EQ(Size(), old_size);
}

void DebugFrameOpCodeWriterForAssembler::ImplicitlyAdvancePC() {
  uint32_t pc = dchecked_integral_cast<uint32_t>(assembler_->CodeSize());
  if (delay_emitting_advance_pc_) {
    uint32_t stream_pos = dchecked_integral_cast<uint32_t>(opcodes_.size());
    delayed_advance_pcs_.push_back(DelayedAdvancePC {stream_pos, pc});
  } else {
    AdvancePC(pc);
  }
}

std::unique_ptr<Assembler> Assembler::Create(
    ArenaAllocator* arena,
    InstructionSet instruction_set,
    const InstructionSetFeatures* instruction_set_features) {
  switch (instruction_set) {
#ifdef ART_ENABLE_CODEGEN_arm
    case kArm:
      return std::unique_ptr<Assembler>(new (arena) arm::Arm32Assembler(arena));
    case kThumb2:
      return std::unique_ptr<Assembler>(new (arena) arm::Thumb2Assembler(arena));
#endif
#ifdef ART_ENABLE_CODEGEN_arm64
    case kArm64:
      return std::unique_ptr<Assembler>(new (arena) arm64::Arm64Assembler(arena));
#endif
#ifdef ART_ENABLE_CODEGEN_mips
    case kMips:
      return std::unique_ptr<Assembler>(new (arena) mips::MipsAssembler(
          arena,
          instruction_set_features != nullptr
              ? instruction_set_features->AsMipsInstructionSetFeatures()
              : nullptr));
#endif
#ifdef ART_ENABLE_CODEGEN_mips64
    case kMips64:
      return std::unique_ptr<Assembler>(new (arena) mips64::Mips64Assembler(arena));
#endif
#ifdef ART_ENABLE_CODEGEN_x86
    case kX86:
      return std::unique_ptr<Assembler>(new (arena) x86::X86Assembler(arena));
#endif
#ifdef ART_ENABLE_CODEGEN_x86_64
    case kX86_64:
      return std::unique_ptr<Assembler>(new (arena) x86_64::X86_64Assembler(arena));
#endif
    default:
      LOG(FATAL) << "Unknown InstructionSet: " << instruction_set;
      return nullptr;
  }
}

void Assembler::StoreImmediateToThread32(ThreadOffset<4> dest ATTRIBUTE_UNUSED,
                                         uint32_t imm ATTRIBUTE_UNUSED,
                                         ManagedRegister scratch ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::StoreImmediateToThread64(ThreadOffset<8> dest ATTRIBUTE_UNUSED,
                                         uint32_t imm ATTRIBUTE_UNUSED,
                                         ManagedRegister scratch ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::StoreStackOffsetToThread32(ThreadOffset<4> thr_offs ATTRIBUTE_UNUSED,
                                           FrameOffset fr_offs ATTRIBUTE_UNUSED,
                                           ManagedRegister scratch ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::StoreStackOffsetToThread64(ThreadOffset<8> thr_offs ATTRIBUTE_UNUSED,
                                           FrameOffset fr_offs ATTRIBUTE_UNUSED,
                                           ManagedRegister scratch ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::StoreStackPointerToThread32(ThreadOffset<4> thr_offs ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::StoreStackPointerToThread64(ThreadOffset<8> thr_offs ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::LoadFromThread32(ManagedRegister dest ATTRIBUTE_UNUSED,
                                 ThreadOffset<4> src ATTRIBUTE_UNUSED,
                                 size_t size ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::LoadFromThread64(ManagedRegister dest ATTRIBUTE_UNUSED,
                                 ThreadOffset<8> src ATTRIBUTE_UNUSED,
                                 size_t size ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::LoadRawPtrFromThread32(ManagedRegister dest ATTRIBUTE_UNUSED,
                                       ThreadOffset<4> offs ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::LoadRawPtrFromThread64(ManagedRegister dest ATTRIBUTE_UNUSED,
                                       ThreadOffset<8> offs ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::CopyRawPtrFromThread32(FrameOffset fr_offs ATTRIBUTE_UNUSED,
                                       ThreadOffset<4> thr_offs ATTRIBUTE_UNUSED,
                                       ManagedRegister scratch ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::CopyRawPtrFromThread64(FrameOffset fr_offs ATTRIBUTE_UNUSED,
                                       ThreadOffset<8> thr_offs ATTRIBUTE_UNUSED,
                                       ManagedRegister scratch ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::CopyRawPtrToThread32(ThreadOffset<4> thr_offs ATTRIBUTE_UNUSED,
                                     FrameOffset fr_offs ATTRIBUTE_UNUSED,
                                     ManagedRegister scratch ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::CopyRawPtrToThread64(ThreadOffset<8> thr_offs ATTRIBUTE_UNUSED,
                                     FrameOffset fr_offs ATTRIBUTE_UNUSED,
                                     ManagedRegister scratch ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::CallFromThread32(ThreadOffset<4> offset ATTRIBUTE_UNUSED,
                                 ManagedRegister scratch ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::CallFromThread64(ThreadOffset<8> offset ATTRIBUTE_UNUSED,
                                 ManagedRegister scratch ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL);
}

}  // namespace art
