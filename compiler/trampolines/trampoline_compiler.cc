/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include "trampoline_compiler.h"

#include "base/arena_allocator.h"
#include "jni_env_ext.h"

#ifdef ART_ENABLE_CODEGEN_arm
#include "utils/arm/assembler_thumb2.h"
#endif

#ifdef ART_ENABLE_CODEGEN_arm64
#include "utils/arm64/assembler_arm64.h"
#endif

#ifdef ART_ENABLE_CODEGEN_mips
#include "utils/mips/assembler_mips.h"
#endif

#ifdef ART_ENABLE_CODEGEN_mips64
#include "utils/mips64/assembler_mips64.h"
#endif

#ifdef ART_ENABLE_CODEGEN_x86
#include "utils/x86/assembler_x86.h"
#endif

#ifdef ART_ENABLE_CODEGEN_x86_64
#include "utils/x86_64/assembler_x86_64.h"
#endif

#define __ assembler.

namespace art {

#ifdef ART_ENABLE_CODEGEN_arm
namespace arm {
static std::unique_ptr<const std::vector<uint8_t>> CreateTrampoline(
    ArenaAllocator* arena, EntryPointCallingConvention abi, ThreadOffset<4> offset) {
  Thumb2Assembler assembler(arena);

  switch (abi) {
    case kInterpreterAbi:  // Thread* is first argument (R0) in interpreter ABI.
      __ LoadFromOffset(kLoadWord, PC, R0, offset.Int32Value());
      break;
    case kJniAbi:  // Load via Thread* held in JNIEnv* in first argument (R0).
      __ LoadFromOffset(kLoadWord, IP, R0, JNIEnvExt::SelfOffset(4).Int32Value());
      __ LoadFromOffset(kLoadWord, PC, IP, offset.Int32Value());
      break;
    case kQuickAbi:  // R9 holds Thread*.
      __ LoadFromOffset(kLoadWord, PC, R9, offset.Int32Value());
  }
  __ bkpt(0);

  __ FinalizeCode();
  size_t cs = __ CodeSize();
  std::unique_ptr<std::vector<uint8_t>> entry_stub(new std::vector<uint8_t>(cs));
  MemoryRegion code(entry_stub->data(), entry_stub->size());
  __ FinalizeInstructions(code);

  return std::move(entry_stub);
}
}  // namespace arm
#endif  // ART_ENABLE_CODEGEN_arm

#ifdef ART_ENABLE_CODEGEN_arm64
namespace arm64 {
static std::unique_ptr<const std::vector<uint8_t>> CreateTrampoline(
    ArenaAllocator* arena, EntryPointCallingConvention abi, ThreadOffset<8> offset) {
  Arm64Assembler assembler(arena);

  switch (abi) {
    case kInterpreterAbi:  // Thread* is first argument (X0) in interpreter ABI.
      __ JumpTo(Arm64ManagedRegister::FromXRegister(X0), Offset(offset.Int32Value()),
          Arm64ManagedRegister::FromXRegister(IP1));

      break;
    case kJniAbi:  // Load via Thread* held in JNIEnv* in first argument (X0).
      __ LoadRawPtr(Arm64ManagedRegister::FromXRegister(IP1),
                      Arm64ManagedRegister::FromXRegister(X0),
                      Offset(JNIEnvExt::SelfOffset(8).Int32Value()));

      __ JumpTo(Arm64ManagedRegister::FromXRegister(IP1), Offset(offset.Int32Value()),
                Arm64ManagedRegister::FromXRegister(IP0));

      break;
    case kQuickAbi:  // X18 holds Thread*.
      __ JumpTo(Arm64ManagedRegister::FromXRegister(TR), Offset(offset.Int32Value()),
                Arm64ManagedRegister::FromXRegister(IP0));

      break;
  }

  __ FinalizeCode();
  size_t cs = __ CodeSize();
  std::unique_ptr<std::vector<uint8_t>> entry_stub(new std::vector<uint8_t>(cs));
  MemoryRegion code(entry_stub->data(), entry_stub->size());
  __ FinalizeInstructions(code);

  return std::move(entry_stub);
}
}  // namespace arm64
#endif  // ART_ENABLE_CODEGEN_arm64

#ifdef ART_ENABLE_CODEGEN_mips
namespace mips {
static std::unique_ptr<const std::vector<uint8_t>> CreateTrampoline(
    ArenaAllocator* arena, EntryPointCallingConvention abi, ThreadOffset<4> offset) {
  MipsAssembler assembler(arena);

  switch (abi) {
    case kInterpreterAbi:  // Thread* is first argument (A0) in interpreter ABI.
      __ LoadFromOffset(kLoadWord, T9, A0, offset.Int32Value());
      break;
    case kJniAbi:  // Load via Thread* held in JNIEnv* in first argument (A0).
      __ LoadFromOffset(kLoadWord, T9, A0, JNIEnvExt::SelfOffset(4).Int32Value());
      __ LoadFromOffset(kLoadWord, T9, T9, offset.Int32Value());
      break;
    case kQuickAbi:  // S1 holds Thread*.
      __ LoadFromOffset(kLoadWord, T9, S1, offset.Int32Value());
  }
  __ Jr(T9);
  __ Nop();
  __ Break();

  __ FinalizeCode();
  size_t cs = __ CodeSize();
  std::unique_ptr<std::vector<uint8_t>> entry_stub(new std::vector<uint8_t>(cs));
  MemoryRegion code(entry_stub->data(), entry_stub->size());
  __ FinalizeInstructions(code);

  return std::move(entry_stub);
}
}  // namespace mips
#endif  // ART_ENABLE_CODEGEN_mips

#ifdef ART_ENABLE_CODEGEN_mips64
namespace mips64 {
static std::unique_ptr<const std::vector<uint8_t>> CreateTrampoline(
    ArenaAllocator* arena, EntryPointCallingConvention abi, ThreadOffset<8> offset) {
  Mips64Assembler assembler(arena);

  switch (abi) {
    case kInterpreterAbi:  // Thread* is first argument (A0) in interpreter ABI.
      __ LoadFromOffset(kLoadDoubleword, T9, A0, offset.Int32Value());
      break;
    case kJniAbi:  // Load via Thread* held in JNIEnv* in first argument (A0).
      __ LoadFromOffset(kLoadDoubleword, T9, A0, JNIEnvExt::SelfOffset(8).Int32Value());
      __ LoadFromOffset(kLoadDoubleword, T9, T9, offset.Int32Value());
      break;
    case kQuickAbi:  // Fall-through.
      __ LoadFromOffset(kLoadDoubleword, T9, S1, offset.Int32Value());
  }
  __ Jr(T9);
  __ Nop();
  __ Break();

  __ FinalizeCode();
  size_t cs = __ CodeSize();
  std::unique_ptr<std::vector<uint8_t>> entry_stub(new std::vector<uint8_t>(cs));
  MemoryRegion code(entry_stub->data(), entry_stub->size());
  __ FinalizeInstructions(code);

  return std::move(entry_stub);
}
}  // namespace mips64
#endif  // ART_ENABLE_CODEGEN_mips

#ifdef ART_ENABLE_CODEGEN_x86
namespace x86 {
static std::unique_ptr<const std::vector<uint8_t>> CreateTrampoline(ArenaAllocator* arena,
                                                                    ThreadOffset<4> offset) {
  X86Assembler assembler(arena);

  // All x86 trampolines call via the Thread* held in fs.
  __ fs()->jmp(Address::Absolute(offset));
  __ int3();

  __ FinalizeCode();
  size_t cs = __ CodeSize();
  std::unique_ptr<std::vector<uint8_t>> entry_stub(new std::vector<uint8_t>(cs));
  MemoryRegion code(entry_stub->data(), entry_stub->size());
  __ FinalizeInstructions(code);

  return std::move(entry_stub);
}
}  // namespace x86
#endif  // ART_ENABLE_CODEGEN_x86

#ifdef ART_ENABLE_CODEGEN_x86_64
namespace x86_64 {
static std::unique_ptr<const std::vector<uint8_t>> CreateTrampoline(ArenaAllocator* arena,
                                                                    ThreadOffset<8> offset) {
  x86_64::X86_64Assembler assembler(arena);

  // All x86 trampolines call via the Thread* held in gs.
  __ gs()->jmp(x86_64::Address::Absolute(offset, true));
  __ int3();

  __ FinalizeCode();
  size_t cs = __ CodeSize();
  std::unique_ptr<std::vector<uint8_t>> entry_stub(new std::vector<uint8_t>(cs));
  MemoryRegion code(entry_stub->data(), entry_stub->size());
  __ FinalizeInstructions(code);

  return std::move(entry_stub);
}
}  // namespace x86_64
#endif  // ART_ENABLE_CODEGEN_x86_64

std::unique_ptr<const std::vector<uint8_t>> CreateTrampoline64(InstructionSet isa,
                                                               EntryPointCallingConvention abi,
                                                               ThreadOffset<8> offset) {
  ArenaPool pool;
  ArenaAllocator arena(&pool);
  switch (isa) {
#ifdef ART_ENABLE_CODEGEN_arm64
    case kArm64:
      return arm64::CreateTrampoline(&arena, abi, offset);
#endif
#ifdef ART_ENABLE_CODEGEN_mips64
    case kMips64:
      return mips64::CreateTrampoline(&arena, abi, offset);
#endif
#ifdef ART_ENABLE_CODEGEN_x86_64
    case kX86_64:
      return x86_64::CreateTrampoline(&arena, offset);
#endif
    default:
      UNUSED(abi);
      UNUSED(offset);
      LOG(FATAL) << "Unexpected InstructionSet: " << isa;
      UNREACHABLE();
  }
}

std::unique_ptr<const std::vector<uint8_t>> CreateTrampoline32(InstructionSet isa,
                                                               EntryPointCallingConvention abi,
                                                               ThreadOffset<4> offset) {
  ArenaPool pool;
  ArenaAllocator arena(&pool);
  switch (isa) {
#ifdef ART_ENABLE_CODEGEN_arm
    case kArm:
    case kThumb2:
      return arm::CreateTrampoline(&arena, abi, offset);
#endif
#ifdef ART_ENABLE_CODEGEN_mips
    case kMips:
      return mips::CreateTrampoline(&arena, abi, offset);
#endif
#ifdef ART_ENABLE_CODEGEN_x86
    case kX86:
      UNUSED(abi);
      return x86::CreateTrampoline(&arena, offset);
#endif
    default:
      LOG(FATAL) << "Unexpected InstructionSet: " << isa;
      UNREACHABLE();
  }
}

}  // namespace art
