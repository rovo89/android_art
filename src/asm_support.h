// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_ASM_SUPPORT_H_
#define ART_SRC_ASM_SUPPORT_H_

#if defined(__arm__)
#define rSUSPEND r4
#define rSELF r9
#define rLR r14
#define SUSPEND_CHECK_INTERVAL (1000)
// Offset of field Thread::suspend_count_ verified in InitCpu
#define THREAD_SUSPEND_COUNT_OFFSET 388
// Offset of field Thread::suspend_count_ verified in InitCpu
#define THREAD_EXCEPTION_OFFSET 384

#elif defined(__i386__)
// Offset of field Thread::self_ verified in InitCpu
#define THREAD_SELF_OFFSET 376
#endif

#endif  // ART_SRC_ASM_SUPPORT_H_
