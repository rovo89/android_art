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

#define STRING_VALUE_OFFSET 8
#define STRING_COUNT_OFFSET 12
#define STRING_OFFSET_OFFSET 20
#define STRING_DATA_OFFSET 12

#if defined(__arm__)
#define rSUSPEND r4
#define rSELF r9
// Offset of field Thread::suspend_count_ verified in InitCpu
#define THREAD_FLAGS_OFFSET 0
// Offset of field Thread::exception_ verified in InitCpu
#define THREAD_EXCEPTION_OFFSET 12
#elif defined(__mips__)
#define rSUSPEND $s0
#define rSELF $s1
// Offset of field Thread::suspend_count_ verified in InitCpu
#define THREAD_FLAGS_OFFSET 0
// Offset of field Thread::exception_ verified in InitCpu
#define THREAD_EXCEPTION_OFFSET 12
#elif defined(__i386__)
// Offset of field Thread::self_ verified in InitCpu
#define THREAD_SELF_OFFSET 40
// Offset of field Thread::exception_ verified in InitCpu
#define THREAD_EXCEPTION_OFFSET 12
#endif

#endif  // ART_SRC_ASM_SUPPORT_H_
