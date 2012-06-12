/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_GREENLAND_RUNTIME_UTILS_H_
#define ART_SRC_GREENLAND_RUNTIME_UTILS_H_

#include "asm_support.h"
#include "thread.h"

namespace art {
namespace greenland {

static inline Thread* art_get_current_thread() {
#if defined(__i386__)
  Thread* ptr;
  __asm__ __volatile__("movl %%fs:(%1), %0"
      : "=r"(ptr)  // output
      : "r"(THREAD_SELF_OFFSET)  // input
      :);  // clobber
  return ptr;
#else
  return Thread::Current();
#endif
}

} // namespace greenland
} // namespace art

#endif // ART_SRC_GREENLAND_RUNTIME_UTILS_H_
