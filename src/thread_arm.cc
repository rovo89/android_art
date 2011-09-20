// Copyright 2011 Google Inc. All Rights Reserved.

#include "thread.h"

#include "asm_support.h"
#include "macros.h"

namespace art {

void Thread::InitCpu() {
  CHECK_EQ(THREAD_TOP_OF_MANAGED_STACK_OFFSET, OFFSETOF_MEMBER(Thread, top_of_managed_stack_));
  CHECK_EQ(THREAD_TOP_OF_MANAGED_STACK_PC_OFFSET,
           OFFSETOF_MEMBER(Thread, top_of_managed_stack_pc_));
}

}  // namespace art
