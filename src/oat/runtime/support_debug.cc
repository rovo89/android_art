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

#include "callee_save_frame.h"
#include "debugger.h"

namespace art {

/*
 * Report location to debugger.  Note: dex_pc is the current offset within
 * the method.  However, because the offset alone cannot distinguish between
 * method entry and offset 0 within the method, we'll use an offset of -1
 * to denote method entry.
 */
extern "C" void artUpdateDebuggerFromCode(int32_t dex_pc, Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp,  Runtime::kRefsAndArgs);
  Dbg::UpdateDebugger(dex_pc, self, sp);
}

// Temporary debugging hook for compiler.
extern void DebugMe(Method* method, uint32_t info) {
  LOG(INFO) << "DebugMe";
  if (method != NULL) {
    LOG(INFO) << PrettyMethod(method);
  }
  LOG(INFO) << "Info: " << info;
}

}  // namespace art
