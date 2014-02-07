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
  self_ = this;

  // Sanity check that reads from %gs point to this Thread*.
  Thread* self_check;
  CHECK_EQ(THREAD_SELF_OFFSET, OFFSETOF_MEMBER(Thread, self_));
  __asm__ __volatile__("movq %%gs:(%1), %0"
      : "=r"(self_check)  // output
      : "r"(THREAD_SELF_OFFSET)  // input
      :);  // clobber
  CHECK_EQ(self_check, this);

  // Sanity check other offsets.
  CHECK_EQ(THREAD_EXCEPTION_OFFSET, OFFSETOF_MEMBER(Thread, exception_));
  CHECK_EQ(THREAD_CARD_TABLE_OFFSET, OFFSETOF_MEMBER(Thread, card_table_));
  CHECK_EQ(THREAD_ID_OFFSET, OFFSETOF_MEMBER(Thread, thin_lock_thread_id_));
}

void Thread::CleanupCpu() {
  // Sanity check that reads from %gs point to this Thread*.
  Thread* self_check;
  CHECK_EQ(THREAD_SELF_OFFSET, OFFSETOF_MEMBER(Thread, self_));
  __asm__ __volatile__("movq %%gs:(%1), %0"
      : "=r"(self_check)  // output
      : "r"(THREAD_SELF_OFFSET)  // input
      :);  // clobber
  CHECK_EQ(self_check, this);

  // Do nothing.
}

}  // namespace art
