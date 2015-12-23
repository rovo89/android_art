/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ART_RUNTIME_JIT_DEBUGGER_INTERFACE_H_
#define ART_RUNTIME_JIT_DEBUGGER_INTERFACE_H_

#include <inttypes.h>

namespace art {

extern "C" {
  struct JITCodeEntry;
}

// Notify native debugger about new JITed code by passing in-memory ELF.
JITCodeEntry* CreateJITCodeEntry(const uint8_t *symfile_addr, uintptr_t symfile_size);

// Notify native debugger that JITed code has been removed.
void DeleteJITCodeEntry(JITCodeEntry* entry);

}  // namespace art

#endif  // ART_RUNTIME_JIT_DEBUGGER_INTERFACE_H_
