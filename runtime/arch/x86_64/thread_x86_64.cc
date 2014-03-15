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

#include "thread.h"

#include "asm_support_x86_64.h"
#include "base/macros.h"
#include "thread-inl.h"
#include "thread_list.h"

#include <asm/prctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

namespace art {

static void arch_prctl(int code, void* val) {
  syscall(__NR_arch_prctl, code, val);
}
void Thread::InitCpu() {
  static Mutex modify_ldt_lock("modify_ldt lock");
  MutexLock mu(Thread::Current(), modify_ldt_lock);
  arch_prctl(ARCH_SET_GS, this);

  // Allow easy indirection back to Thread*.
  tlsPtr_.self = this;

  // Sanity check that reads from %gs point to this Thread*.
  Thread* self_check;
  CHECK_EQ(THREAD_SELF_OFFSET, SelfOffset<8>().Int32Value());
  __asm__ __volatile__("movq %%gs:(%1), %0"
      : "=r"(self_check)  // output
      : "r"(THREAD_SELF_OFFSET)  // input
      :);  // clobber
  CHECK_EQ(self_check, this);

  // Sanity check other offsets.
  CHECK_EQ(static_cast<size_t>(RUNTIME_SAVE_ALL_CALLEE_SAVE_FRAME_OFFSET),
           Runtime::GetCalleeSaveMethodOffset(Runtime::kSaveAll));
  CHECK_EQ(static_cast<size_t>(RUNTIME_REFS_ONLY_CALLEE_SAVE_FRAME_OFFSET),
           Runtime::GetCalleeSaveMethodOffset(Runtime::kRefsOnly));
  CHECK_EQ(static_cast<size_t>(RUNTIME_REF_AND_ARGS_CALLEE_SAVE_FRAME_OFFSET),
           Runtime::GetCalleeSaveMethodOffset(Runtime::kRefsAndArgs));
  CHECK_EQ(THREAD_EXCEPTION_OFFSET, ExceptionOffset<8>().Int32Value());
  CHECK_EQ(THREAD_CARD_TABLE_OFFSET, CardTableOffset<8>().Int32Value());
  CHECK_EQ(THREAD_ID_OFFSET, ThinLockIdOffset<8>().Int32Value());
}

void Thread::CleanupCpu() {
  // Sanity check that reads from %gs point to this Thread*.
  Thread* self_check;
  CHECK_EQ(THREAD_SELF_OFFSET, SelfOffset<8>().Int32Value());
  __asm__ __volatile__("movq %%gs:(%1), %0"
      : "=r"(self_check)  // output
      : "r"(THREAD_SELF_OFFSET)  // input
      :);  // clobber
  CHECK_EQ(self_check, this);

  // Do nothing.
}

}  // namespace art
