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
    mirror::ArtMethod* save_method = r->CreateCalleeSaveMethod();
    r->SetCalleeSaveMethod(save_method, type);
    QuickMethodFrameInfo frame_info = save_method->GetQuickFrameInfo();
    EXPECT_EQ(frame_info.FrameSizeInBytes(), save_size) << "Expected and real size differs for "
        << type << " core spills=" << std::hex << frame_info.CoreSpillMask() << " fp spills="
        << frame_info.FpSpillMask() << std::dec;

    t->TransitionFromRunnableToSuspended(ThreadState::kNative);  // So we can shut down.
  }
};

// Common tests are declared next to the constants.
#define ADD_TEST_EQ(x, y) EXPECT_EQ(x, y);
#include "asm_support.h"

TEST_F(ArchTest, CheckCommonOffsetsAndSizes) {
  CheckAsmSupportOffsetsAndSizes();
}

// Grab architecture specific constants.
namespace arm {
#include "arch/arm/asm_support_arm.h"
static constexpr size_t kFrameSizeSaveAllCalleeSave = FRAME_SIZE_SAVE_ALL_CALLEE_SAVE;
#undef FRAME_SIZE_SAVE_ALL_CALLEE_SAVE
static constexpr size_t kFrameSizeRefsOnlyCalleeSave = FRAME_SIZE_REFS_ONLY_CALLEE_SAVE;
#undef FRAME_SIZE_REFS_ONLY_CALLEE_SAVE
static constexpr size_t kFrameSizeRefsAndArgsCalleeSave = FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE;
#undef FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE
}

namespace arm64 {
#include "arch/arm64/asm_support_arm64.h"
static constexpr size_t kFrameSizeSaveAllCalleeSave = FRAME_SIZE_SAVE_ALL_CALLEE_SAVE;
#undef FRAME_SIZE_SAVE_ALL_CALLEE_SAVE
static constexpr size_t kFrameSizeRefsOnlyCalleeSave = FRAME_SIZE_REFS_ONLY_CALLEE_SAVE;
#undef FRAME_SIZE_REFS_ONLY_CALLEE_SAVE
static constexpr size_t kFrameSizeRefsAndArgsCalleeSave = FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE;
#undef FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE
}

namespace mips {
#include "arch/mips/asm_support_mips.h"
static constexpr size_t kFrameSizeSaveAllCalleeSave = FRAME_SIZE_SAVE_ALL_CALLEE_SAVE;
#undef FRAME_SIZE_SAVE_ALL_CALLEE_SAVE
static constexpr size_t kFrameSizeRefsOnlyCalleeSave = FRAME_SIZE_REFS_ONLY_CALLEE_SAVE;
#undef FRAME_SIZE_REFS_ONLY_CALLEE_SAVE
static constexpr size_t kFrameSizeRefsAndArgsCalleeSave = FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE;
#undef FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE
}

namespace x86 {
#include "arch/x86/asm_support_x86.h"
static constexpr size_t kFrameSizeSaveAllCalleeSave = FRAME_SIZE_SAVE_ALL_CALLEE_SAVE;
#undef FRAME_SIZE_SAVE_ALL_CALLEE_SAVE
static constexpr size_t kFrameSizeRefsOnlyCalleeSave = FRAME_SIZE_REFS_ONLY_CALLEE_SAVE;
#undef FRAME_SIZE_REFS_ONLY_CALLEE_SAVE
static constexpr size_t kFrameSizeRefsAndArgsCalleeSave = FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE;
#undef FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE
}

namespace x86_64 {
#include "arch/x86_64/asm_support_x86_64.h"
static constexpr size_t kFrameSizeSaveAllCalleeSave = FRAME_SIZE_SAVE_ALL_CALLEE_SAVE;
#undef FRAME_SIZE_SAVE_ALL_CALLEE_SAVE
static constexpr size_t kFrameSizeRefsOnlyCalleeSave = FRAME_SIZE_REFS_ONLY_CALLEE_SAVE;
#undef FRAME_SIZE_REFS_ONLY_CALLEE_SAVE
static constexpr size_t kFrameSizeRefsAndArgsCalleeSave = FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE;
#undef FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE
}

// Check architecture specific constants are sound.
TEST_F(ArchTest, ARM) {
  CheckFrameSize(InstructionSet::kArm, Runtime::kSaveAll, arm::kFrameSizeSaveAllCalleeSave);
  CheckFrameSize(InstructionSet::kArm, Runtime::kRefsOnly, arm::kFrameSizeRefsOnlyCalleeSave);
  CheckFrameSize(InstructionSet::kArm, Runtime::kRefsAndArgs, arm::kFrameSizeRefsAndArgsCalleeSave);
}


TEST_F(ArchTest, ARM64) {
  CheckFrameSize(InstructionSet::kArm64, Runtime::kSaveAll, arm64::kFrameSizeSaveAllCalleeSave);
  CheckFrameSize(InstructionSet::kArm64, Runtime::kRefsOnly, arm64::kFrameSizeRefsOnlyCalleeSave);
  CheckFrameSize(InstructionSet::kArm64, Runtime::kRefsAndArgs,
                 arm64::kFrameSizeRefsAndArgsCalleeSave);
}

TEST_F(ArchTest, MIPS) {
  CheckFrameSize(InstructionSet::kMips, Runtime::kSaveAll, mips::kFrameSizeSaveAllCalleeSave);
  CheckFrameSize(InstructionSet::kMips, Runtime::kRefsOnly, mips::kFrameSizeRefsOnlyCalleeSave);
  CheckFrameSize(InstructionSet::kMips, Runtime::kRefsAndArgs,
                 mips::kFrameSizeRefsAndArgsCalleeSave);
}

TEST_F(ArchTest, X86) {
  CheckFrameSize(InstructionSet::kX86, Runtime::kSaveAll, x86::kFrameSizeSaveAllCalleeSave);
  CheckFrameSize(InstructionSet::kX86, Runtime::kRefsOnly, x86::kFrameSizeRefsOnlyCalleeSave);
  CheckFrameSize(InstructionSet::kX86, Runtime::kRefsAndArgs, x86::kFrameSizeRefsAndArgsCalleeSave);
}

TEST_F(ArchTest, X86_64) {
  CheckFrameSize(InstructionSet::kX86_64, Runtime::kSaveAll, x86_64::kFrameSizeSaveAllCalleeSave);
  CheckFrameSize(InstructionSet::kX86_64, Runtime::kRefsOnly, x86_64::kFrameSizeRefsOnlyCalleeSave);
  CheckFrameSize(InstructionSet::kX86_64, Runtime::kRefsAndArgs,
                 x86_64::kFrameSizeRefsAndArgsCalleeSave);
}

}  // namespace art
