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

#include "arm/assembler_arm32.h"
#include "arm/assembler_thumb2.h"
#include "arm64/assembler_arm64.h"
#include "mips/assembler_mips.h"
#include "x86/assembler_x86.h"
#include "x86_64/assembler_x86_64.h"
#include "globals.h"
#include "memory_region.h"

namespace art {

static byte* NewContents(size_t capacity) {
  return new byte[capacity];
}


AssemblerBuffer::AssemblerBuffer() {
  static const size_t kInitialBufferCapacity = 4 * KB;
  contents_ = NewContents(kInitialBufferCapacity);
  cursor_ = contents_;
  limit_ = ComputeLimit(contents_, kInitialBufferCapacity);
  fixup_ = NULL;
  slow_path_ = NULL;
#ifndef NDEBUG
  has_ensured_capacity_ = false;
  fixups_processed_ = false;
#endif

  // Verify internal state.
  CHECK_EQ(Capacity(), kInitialBufferCapacity);
  CHECK_EQ(Size(), 0U);
}


AssemblerBuffer::~AssemblerBuffer() {
  delete[] contents_;
}


void AssemblerBuffer::ProcessFixups(const MemoryRegion& region) {
  AssemblerFixup* fixup = fixup_;
  while (fixup != NULL) {
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


void AssemblerBuffer::ExtendCapacity() {
  size_t old_size = Size();
  size_t old_capacity = Capacity();
  size_t new_capacity = std::min(old_capacity * 2, old_capacity + 1 * MB);

  // Allocate the new data area and copy contents of the old one to it.
  byte* new_contents = NewContents(new_capacity);
  memmove(reinterpret_cast<void*>(new_contents),
          reinterpret_cast<void*>(contents_),
          old_size);

  // Compute the relocation delta and switch to the new contents area.
  ptrdiff_t delta = new_contents - contents_;
  contents_ = new_contents;

  // Update the cursor and recompute the limit.
  cursor_ += delta;
  limit_ = ComputeLimit(new_contents, new_capacity);

  // Verify internal state.
  CHECK_EQ(Capacity(), new_capacity);
  CHECK_EQ(Size(), old_size);
}


Assembler* Assembler::Create(InstructionSet instruction_set) {
  switch (instruction_set) {
    case kArm:
      return new arm::Arm32Assembler();
    case kThumb2:
      return new arm::Thumb2Assembler();
    case kArm64:
      return new arm64::Arm64Assembler();
    case kMips:
      return new mips::MipsAssembler();
    case kX86:
      return new x86::X86Assembler();
    case kX86_64:
      return new x86_64::X86_64Assembler();
    default:
      LOG(FATAL) << "Unknown InstructionSet: " << instruction_set;
      return NULL;
  }
}

void Assembler::StoreImmediateToThread32(ThreadOffset<4> dest, uint32_t imm,
                                         ManagedRegister scratch) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::StoreImmediateToThread64(ThreadOffset<8> dest, uint32_t imm,
                                         ManagedRegister scratch) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::StoreStackOffsetToThread32(ThreadOffset<4> thr_offs,
                                           FrameOffset fr_offs,
                                           ManagedRegister scratch) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::StoreStackOffsetToThread64(ThreadOffset<8> thr_offs,
                                           FrameOffset fr_offs,
                                           ManagedRegister scratch) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::StoreStackPointerToThread32(ThreadOffset<4> thr_offs) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::StoreStackPointerToThread64(ThreadOffset<8> thr_offs) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::LoadFromThread32(ManagedRegister dest, ThreadOffset<4> src, size_t size) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::LoadFromThread64(ManagedRegister dest, ThreadOffset<8> src, size_t size) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::LoadRawPtrFromThread32(ManagedRegister dest, ThreadOffset<4> offs) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::LoadRawPtrFromThread64(ManagedRegister dest, ThreadOffset<8> offs) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::CopyRawPtrFromThread32(FrameOffset fr_offs, ThreadOffset<4> thr_offs,
                                       ManagedRegister scratch) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::CopyRawPtrFromThread64(FrameOffset fr_offs, ThreadOffset<8> thr_offs,
                                       ManagedRegister scratch) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::CopyRawPtrToThread32(ThreadOffset<4> thr_offs, FrameOffset fr_offs,
                                     ManagedRegister scratch) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::CopyRawPtrToThread64(ThreadOffset<8> thr_offs, FrameOffset fr_offs,
                                     ManagedRegister scratch) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::CallFromThread32(ThreadOffset<4> offset, ManagedRegister scratch) {
  UNIMPLEMENTED(FATAL);
}

void Assembler::CallFromThread64(ThreadOffset<8> offset, ManagedRegister scratch) {
  UNIMPLEMENTED(FATAL);
}

}  // namespace art
