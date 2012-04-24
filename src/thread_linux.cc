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

namespace art {

void Thread::DumpNativeStack(std::ostream&) const {
  // TODO: use libcorkscrew; backtrace(3) only works for the calling thread.
}

void Thread::SetNativePriority(int) {
  // Do nothing.
}

int Thread::GetNativePriority() {
  return kNormThreadPriority;
}

}  // namespace art
