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

#include <stdint.h>

#include "common_runtime_test.h"
#include "mirror/art_method-inl.h"
#include "quick/quick_method_frame_info.h"

namespace art {

class ArchTest : public CommonRuntimeTest {
 protected:
  static void CheckFrameSize(InstructionSet isa, Runtime::CalleeSaveType type, uint32_t save_size)
      NO_THREAD_SAFETY_ANALYSIS {
    Runtime* r = Runtime::Current();

    Thread* t = Thread::Current();
    t->TransitionFromSuspendedToRunnable();  // So we can create callee-save methods.

    r->SetInstructionSet(isa);
    mirror::ArtMethod* save_method = r->CreateCalleeSaveMethod(type);
    r->SetCalleeSaveMethod(save_method, type);
    QuickMethodFrameInfo frame_info = save_method->GetQuickFrameInfo();
    EXPECT_EQ(frame_info.FrameSizeInBytes(), save_size) << "Expected and real size differs for "
        << type << " core spills=" << std::hex << frame_info.CoreSpillMask() << " fp spills="
        << frame_info.FpSpillMask() << std::dec;

    t->TransitionFromRunnableToSuspended(ThreadState::kNative);  // So we can shut down.
  }
};


TEST_F(ArchTest, ARM) {
#include "arch/arm/asm_support_arm.h"
#undef ART_RUNTIME_ARCH_ARM_ASM_SUPPORT_ARM_H_


#ifdef FRAME_SIZE_SAVE_ALL_CALLEE_SAVE
  CheckFrameSize(InstructionSet::kArm, Runtime::kSaveAll, FRAME_SIZE_SAVE_ALL_CALLEE_SAVE);
#else
  LOG(WARNING) << "No frame size for SaveAll";
#endif
#ifdef FRAME_SIZE_REFS_ONLY_CALLEE_SAVE
  CheckFrameSize(InstructionSet::kArm, Runtime::kRefsOnly, FRAME_SIZE_REFS_ONLY_CALLEE_SAVE);
#else
  LOG(WARNING) << "No frame size for RefsOnly";
#endif
#ifdef FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE
  CheckFrameSize(InstructionSet::kArm, Runtime::kRefsAndArgs, FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE);
#else
  LOG(WARNING) << "No frame size for RefsAndArgs";
#endif


#ifdef RUNTIME_SAVE_ALL_CALLEE_SAVE_FRAME_OFFSET
#undef RUNTIME_SAVE_ALL_CALLEE_SAVE_FRAME_OFFSET
#endif
#ifdef RUNTIME_REFS_ONLY_CALLEE_SAVE_FRAME_OFFSET
#undef RUNTIME_REFS_ONLY_CALLEE_SAVE_FRAME_OFFSET
#endif
#ifdef RUNTIME_REF_AND_ARGS_CALLEE_SAVE_FRAME_OFFSET
#undef RUNTIME_REF_AND_ARGS_CALLEE_SAVE_FRAME_OFFSET
#endif
#ifdef THREAD_SELF_OFFSET
#undef THREAD_SELF_OFFSET
#endif
#ifdef THREAD_CARD_TABLE_OFFSET
#undef THREAD_CARD_TABLE_OFFSET
#endif
#ifdef THREAD_EXCEPTION_OFFSET
#undef THREAD_EXCEPTION_OFFSET
#endif
#ifdef THREAD_ID_OFFSET
#undef THREAD_ID_OFFSET
#endif
#ifdef FRAME_SIZE_SAVE_ALL_CALLEE_SAVE
#undef FRAME_SIZE_SAVE_ALL_CALLEE_SAVE
#endif
#ifdef FRAME_SIZE_REFS_ONLY_CALLEE_SAVE
#undef FRAME_SIZE_REFS_ONLY_CALLEE_SAVE
#endif
#ifdef FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE
#undef FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE
#endif
#ifdef HEAP_REFERENCE_SIZE
#undef HEAP_REFERENCE_SIZE
#endif
}


TEST_F(ArchTest, ARM64) {
#include "arch/arm64/asm_support_arm64.h"
#undef ART_RUNTIME_ARCH_ARM64_ASM_SUPPORT_ARM64_H_


#ifdef FRAME_SIZE_SAVE_ALL_CALLEE_SAVE
  CheckFrameSize(InstructionSet::kArm64, Runtime::kSaveAll, FRAME_SIZE_SAVE_ALL_CALLEE_SAVE);
#else
  LOG(WARNING) << "No frame size for SaveAll";
#endif
#ifdef FRAME_SIZE_REFS_ONLY_CALLEE_SAVE
  CheckFrameSize(InstructionSet::kArm64, Runtime::kRefsOnly, FRAME_SIZE_REFS_ONLY_CALLEE_SAVE);
#else
  LOG(WARNING) << "No frame size for RefsOnly";
#endif
#ifdef FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE
  CheckFrameSize(InstructionSet::kArm64, Runtime::kRefsAndArgs, FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE);
#else
  LOG(WARNING) << "No frame size for RefsAndArgs";
#endif


#ifdef RUNTIME_SAVE_ALL_CALLEE_SAVE_FRAME_OFFSET
#undef RUNTIME_SAVE_ALL_CALLEE_SAVE_FRAME_OFFSET
#endif
#ifdef RUNTIME_REFS_ONLY_CALLEE_SAVE_FRAME_OFFSET
#undef RUNTIME_REFS_ONLY_CALLEE_SAVE_FRAME_OFFSET
#endif
#ifdef RUNTIME_REF_AND_ARGS_CALLEE_SAVE_FRAME_OFFSET
#undef RUNTIME_REF_AND_ARGS_CALLEE_SAVE_FRAME_OFFSET
#endif
#ifdef THREAD_SELF_OFFSET
#undef THREAD_SELF_OFFSET
#endif
#ifdef THREAD_CARD_TABLE_OFFSET
#undef THREAD_CARD_TABLE_OFFSET
#endif
#ifdef THREAD_EXCEPTION_OFFSET
#undef THREAD_EXCEPTION_OFFSET
#endif
#ifdef THREAD_ID_OFFSET
#undef THREAD_ID_OFFSET
#endif
#ifdef FRAME_SIZE_SAVE_ALL_CALLEE_SAVE
#undef FRAME_SIZE_SAVE_ALL_CALLEE_SAVE
#endif
#ifdef FRAME_SIZE_REFS_ONLY_CALLEE_SAVE
#undef FRAME_SIZE_REFS_ONLY_CALLEE_SAVE
#endif
#ifdef FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE
#undef FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE
#endif
#ifdef HEAP_REFERENCE_SIZE
#undef HEAP_REFERENCE_SIZE
#endif
}


TEST_F(ArchTest, MIPS) {
#include "arch/mips/asm_support_mips.h"
#undef ART_RUNTIME_ARCH_MIPS_ASM_SUPPORT_MIPS_H_


#ifdef FRAME_SIZE_SAVE_ALL_CALLEE_SAVE
  CheckFrameSize(InstructionSet::kMips, Runtime::kSaveAll, FRAME_SIZE_SAVE_ALL_CALLEE_SAVE);
#else
  LOG(WARNING) << "No frame size for SaveAll";
#endif
#ifdef FRAME_SIZE_REFS_ONLY_CALLEE_SAVE
  CheckFrameSize(InstructionSet::kMips, Runtime::kRefsOnly, FRAME_SIZE_REFS_ONLY_CALLEE_SAVE);
#else
  LOG(WARNING) << "No frame size for RefsOnly";
#endif
#ifdef FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE
  CheckFrameSize(InstructionSet::kMips, Runtime::kRefsAndArgs, FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE);
#else
  LOG(WARNING) << "No frame size for RefsAndArgs";
#endif


#ifdef RUNTIME_SAVE_ALL_CALLEE_SAVE_FRAME_OFFSET
#undef RUNTIME_SAVE_ALL_CALLEE_SAVE_FRAME_OFFSET
#endif
#ifdef RUNTIME_REFS_ONLY_CALLEE_SAVE_FRAME_OFFSET
#undef RUNTIME_REFS_ONLY_CALLEE_SAVE_FRAME_OFFSET
#endif
#ifdef RUNTIME_REF_AND_ARGS_CALLEE_SAVE_FRAME_OFFSET
#undef RUNTIME_REF_AND_ARGS_CALLEE_SAVE_FRAME_OFFSET
#endif
#ifdef THREAD_SELF_OFFSET
#undef THREAD_SELF_OFFSET
#endif
#ifdef THREAD_CARD_TABLE_OFFSET
#undef THREAD_CARD_TABLE_OFFSET
#endif
#ifdef THREAD_EXCEPTION_OFFSET
#undef THREAD_EXCEPTION_OFFSET
#endif
#ifdef THREAD_ID_OFFSET
#undef THREAD_ID_OFFSET
#endif
#ifdef FRAME_SIZE_SAVE_ALL_CALLEE_SAVE
#undef FRAME_SIZE_SAVE_ALL_CALLEE_SAVE
#endif
#ifdef FRAME_SIZE_REFS_ONLY_CALLEE_SAVE
#undef FRAME_SIZE_REFS_ONLY_CALLEE_SAVE
#endif
#ifdef FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE
#undef FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE
#endif
#ifdef HEAP_REFERENCE_SIZE
#undef HEAP_REFERENCE_SIZE
#endif
}


TEST_F(ArchTest, X86) {
#include "arch/x86/asm_support_x86.h"
#undef ART_RUNTIME_ARCH_X86_ASM_SUPPORT_X86_H_


#ifdef FRAME_SIZE_SAVE_ALL_CALLEE_SAVE
  CheckFrameSize(InstructionSet::kX86, Runtime::kSaveAll, FRAME_SIZE_SAVE_ALL_CALLEE_SAVE);
#else
  LOG(WARNING) << "No frame size for SaveAll";
#endif
#ifdef FRAME_SIZE_REFS_ONLY_CALLEE_SAVE
  CheckFrameSize(InstructionSet::kX86, Runtime::kRefsOnly, FRAME_SIZE_REFS_ONLY_CALLEE_SAVE);
#else
  LOG(WARNING) << "No frame size for RefsOnly";
#endif
#ifdef FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE
  CheckFrameSize(InstructionSet::kX86, Runtime::kRefsAndArgs, FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE);
#else
  LOG(WARNING) << "No frame size for RefsAndArgs";
#endif


#ifdef RUNTIME_SAVE_ALL_CALLEE_SAVE_FRAME_OFFSET
#undef RUNTIME_SAVE_ALL_CALLEE_SAVE_FRAME_OFFSET
#endif
#ifdef RUNTIME_REFS_ONLY_CALLEE_SAVE_FRAME_OFFSET
#undef RUNTIME_REFS_ONLY_CALLEE_SAVE_FRAME_OFFSET
#endif
#ifdef RUNTIME_REF_AND_ARGS_CALLEE_SAVE_FRAME_OFFSET
#undef RUNTIME_REF_AND_ARGS_CALLEE_SAVE_FRAME_OFFSET
#endif
#ifdef THREAD_SELF_OFFSET
#undef THREAD_SELF_OFFSET
#endif
#ifdef THREAD_CARD_TABLE_OFFSET
#undef THREAD_CARD_TABLE_OFFSET
#endif
#ifdef THREAD_EXCEPTION_OFFSET
#undef THREAD_EXCEPTION_OFFSET
#endif
#ifdef THREAD_ID_OFFSET
#undef THREAD_ID_OFFSET
#endif
#ifdef FRAME_SIZE_SAVE_ALL_CALLEE_SAVE
#undef FRAME_SIZE_SAVE_ALL_CALLEE_SAVE
#endif
#ifdef FRAME_SIZE_REFS_ONLY_CALLEE_SAVE
#undef FRAME_SIZE_REFS_ONLY_CALLEE_SAVE
#endif
#ifdef FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE
#undef FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE
#endif
#ifdef HEAP_REFERENCE_SIZE
#undef HEAP_REFERENCE_SIZE
#endif
}


TEST_F(ArchTest, X86_64) {
#include "arch/x86_64/asm_support_x86_64.h"
#undef ART_RUNTIME_ARCH_X86_64_ASM_SUPPORT_X86_64_H_


#ifdef FRAME_SIZE_SAVE_ALL_CALLEE_SAVE
  CheckFrameSize(InstructionSet::kX86_64, Runtime::kSaveAll, FRAME_SIZE_SAVE_ALL_CALLEE_SAVE);
#else
  LOG(WARNING) << "No frame size for SaveAll";
#endif
#ifdef FRAME_SIZE_REFS_ONLY_CALLEE_SAVE
  CheckFrameSize(InstructionSet::kX86_64, Runtime::kRefsOnly, FRAME_SIZE_REFS_ONLY_CALLEE_SAVE);
#else
  LOG(WARNING) << "No frame size for RefsOnly";
#endif
#ifdef FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE
  CheckFrameSize(InstructionSet::kX86_64, Runtime::kRefsAndArgs, FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE);
#else
  LOG(WARNING) << "No frame size for RefsAndArgs";
#endif


#ifdef RUNTIME_SAVE_ALL_CALLEE_SAVE_FRAME_OFFSET
#undef RUNTIME_SAVE_ALL_CALLEE_SAVE_FRAME_OFFSET
#endif
#ifdef RUNTIME_REFS_ONLY_CALLEE_SAVE_FRAME_OFFSET
#undef RUNTIME_REFS_ONLY_CALLEE_SAVE_FRAME_OFFSET
#endif
#ifdef RUNTIME_REF_AND_ARGS_CALLEE_SAVE_FRAME_OFFSET
#undef RUNTIME_REF_AND_ARGS_CALLEE_SAVE_FRAME_OFFSET
#endif
#ifdef THREAD_SELF_OFFSET
#undef THREAD_SELF_OFFSET
#endif
#ifdef THREAD_CARD_TABLE_OFFSET
#undef THREAD_CARD_TABLE_OFFSET
#endif
#ifdef THREAD_EXCEPTION_OFFSET
#undef THREAD_EXCEPTION_OFFSET
#endif
#ifdef THREAD_ID_OFFSET
#undef THREAD_ID_OFFSET
#endif
#ifdef FRAME_SIZE_SAVE_ALL_CALLEE_SAVE
#undef FRAME_SIZE_SAVE_ALL_CALLEE_SAVE
#endif
#ifdef FRAME_SIZE_REFS_ONLY_CALLEE_SAVE
#undef FRAME_SIZE_REFS_ONLY_CALLEE_SAVE
#endif
#ifdef FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE
#undef FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE
#endif
#ifdef HEAP_REFERENCE_SIZE
#undef HEAP_REFERENCE_SIZE
#endif
}


// The following tests are all for the running architecture. So we get away
// with just including it and not undefining it every time.


#if defined(__arm__)
#include "arch/arm/asm_support_arm.h"
#undef ART_RUNTIME_ARCH_ARM_ASM_SUPPORT_ARM_H_
#elif defined(__aarch64__)
#include "arch/arm64/asm_support_arm64.h"
#undef ART_RUNTIME_ARCH_ARM64_ASM_SUPPORT_ARM64_H_
#elif defined(__mips__)
#include "arch/mips/asm_support_mips.h"
#undef ART_RUNTIME_ARCH_MIPS_ASM_SUPPORT_MIPS_H_
#elif defined(__i386__)
#include "arch/x86/asm_support_x86.h"
#undef ART_RUNTIME_ARCH_X86_ASM_SUPPORT_X86_H_
#elif defined(__x86_64__)
#include "arch/x86_64/asm_support_x86_64.h"
#undef ART_RUNTIME_ARCH_X86_64_ASM_SUPPORT_X86_64_H_
#else
  // This happens for the host test.
#ifdef __LP64__
#include "arch/x86_64/asm_support_x86_64.h"
#undef ART_RUNTIME_ARCH_X86_64_ASM_SUPPORT_X86_64_H_
#else
#include "arch/x86/asm_support_x86.h"
#undef ART_RUNTIME_ARCH_X86_ASM_SUPPORT_X86_H_
#endif
#endif


TEST_F(ArchTest, ThreadOffsets) {
  // Ugly hack, change when possible.
#ifdef __LP64__
#define POINTER_SIZE 8
#else
#define POINTER_SIZE 4
#endif

#if defined(THREAD_SELF_OFFSET)
  ThreadOffset<POINTER_SIZE> self_offset = Thread::SelfOffset<POINTER_SIZE>();
  EXPECT_EQ(self_offset.Int32Value(), THREAD_SELF_OFFSET);
#else
  LOG(INFO) << "No Thread Self Offset found.";
#endif

#if defined(THREAD_CARD_TABLE_OFFSET)
  ThreadOffset<POINTER_SIZE> card_offset = Thread::CardTableOffset<POINTER_SIZE>();
  EXPECT_EQ(card_offset.Int32Value(), THREAD_CARD_TABLE_OFFSET);
#else
  LOG(INFO) << "No Thread Card Table Offset found.";
#endif

#if defined(THREAD_EXCEPTION_OFFSET)
  ThreadOffset<POINTER_SIZE> exc_offset = Thread::ExceptionOffset<POINTER_SIZE>();
    EXPECT_EQ(exc_offset.Int32Value(), THREAD_EXCEPTION_OFFSET);
#else
  LOG(INFO) << "No Thread Exception Offset found.";
#endif

#if defined(THREAD_ID_OFFSET)
  ThreadOffset<POINTER_SIZE> id_offset = Thread::ThinLockIdOffset<POINTER_SIZE>();
  EXPECT_EQ(id_offset.Int32Value(), THREAD_ID_OFFSET);
#else
  LOG(INFO) << "No Thread ID Offset found.";
#endif
}


TEST_F(ArchTest, CalleeSaveMethodOffsets) {
#if defined(RUNTIME_SAVE_ALL_CALLEE_SAVE_FRAME_OFFSET)
  EXPECT_EQ(Runtime::GetCalleeSaveMethodOffset(Runtime::kSaveAll),
            static_cast<size_t>(RUNTIME_SAVE_ALL_CALLEE_SAVE_FRAME_OFFSET));
#else
  LOG(INFO) << "No Runtime Save-all Offset found.";
#endif

#if defined(RUNTIME_REFS_ONLY_CALLEE_SAVE_FRAME_OFFSET)
  EXPECT_EQ(Runtime::GetCalleeSaveMethodOffset(Runtime::kRefsOnly),
            static_cast<size_t>(RUNTIME_REFS_ONLY_CALLEE_SAVE_FRAME_OFFSET));
#else
  LOG(INFO) << "No Runtime Refs-only Offset found.";
#endif

#if defined(RUNTIME_REF_AND_ARGS_CALLEE_SAVE_FRAME_OFFSET)
  EXPECT_EQ(Runtime::GetCalleeSaveMethodOffset(Runtime::kRefsAndArgs),
            static_cast<size_t>(RUNTIME_REF_AND_ARGS_CALLEE_SAVE_FRAME_OFFSET));
#else
  LOG(INFO) << "No Runtime Refs-and-Args Offset found.";
#endif
}


TEST_F(ArchTest, HeapReferenceSize) {
#if defined(HEAP_REFERENCE_SIZE)
  EXPECT_EQ(sizeof(mirror::HeapReference<mirror::Object>),
            static_cast<size_t>(HEAP_REFERENCE_SIZE));
#else
  LOG(INFO) << "No expected HeapReference Size found.";
#endif
}

}  // namespace art
