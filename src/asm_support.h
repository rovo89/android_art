// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_ASM_SUPPORT_H_
#define ART_SRC_ASM_SUPPORT_H_

#if defined(__arm__)
#define rSUSPEND r4
#define rSELF r9
#define rLR r14
#define SUSPEND_CHECK_INTERVAL (1000)
// Offset of field Thread::top_of_managed_stack_ verified in InitCpu
#define THREAD_TOP_OF_MANAGED_STACK_OFFSET 341
// Offset of field Thread::top_of_managed_stack_pc_ verified in InitCpu
#define THREAD_TOP_OF_MANAGED_STACK_PC_OFFSET 345

#elif defined(__i386__)
// Offset of field Thread::self_ verified in InitCpu
#define THREAD_SELF_OFFSET 365
#endif

#endif  // ART_SRC_ASM_SUPPORT_H_
