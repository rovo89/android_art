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

#ifndef ART_SRC_ASM_SUPPORT_H_
#define ART_SRC_ASM_SUPPORT_H_

#define SUSPEND_CHECK_INTERVAL (1000)

#if defined(__arm__)
#define rSUSPEND r4
#define rSELF r9
#define rLR r14
// Offset of field Thread::suspend_count_ verified in InitCpu
#define THREAD_SUSPEND_COUNT_OFFSET 420
// Offset of field Thread::exception_ verified in InitCpu
#define THREAD_EXCEPTION_OFFSET 416

#elif defined(__i386__)
// Offset of field Thread::self_ verified in InitCpu
#define THREAD_SELF_OFFSET 408
#endif

#endif  // ART_SRC_ASM_SUPPORT_H_
