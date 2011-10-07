// Copyright 2011 Google Inc. All Rights Reserved.

#include "thread.h"

#include "asm_support.h"
#include "macros.h"

namespace art {

void Thread::InitCpu() {
  CHECK_EQ(THREAD_SUSPEND_COUNT_OFFSET, OFFSETOF_MEMBER(Thread, suspend_count_));
}

}  // namespace art
