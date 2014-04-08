/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "driver/compiler_driver.h"

namespace art {
  void CompilerDriver::BuildArmEntrypointTrampolineCall(ThreadOffset<4> thread_offset) {
    // Thumb2 instruction encoding of:
    // ldr pc,[r9,#offset]

    // TODO: we don't currently have a Thumb2 assembler, when we do use that
    //       in preference to the hand generated code below.
    uint32_t offset = thread_offset.Uint32Value();
    uint32_t instruction = 0xf8d0f000 | (9 << 16) | (offset & 0xfff);
    entrypoint_trampoline_code_.push_back((instruction >> 16) & 0xff);
    entrypoint_trampoline_code_.push_back((instruction >> 24) & 0xff);
    entrypoint_trampoline_code_.push_back((instruction >> 0) & 0xff);
    entrypoint_trampoline_code_.push_back((instruction >> 8) & 0xff);
  }
}   // namespace art
