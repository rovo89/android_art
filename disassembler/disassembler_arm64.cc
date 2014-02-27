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

#include "disassembler_arm64.h"

#include <inttypes.h>

#include <iostream>

#include "base/logging.h"
#include "base/stringprintf.h"
#include "thread.h"

namespace art {
namespace arm64 {

static uint32_t ReadU32(const uint8_t* ptr) {
  return *((const uint32_t*)ptr);
}

size_t DisassemblerArm64::Dump(std::ostream& os, const uint8_t* begin) {
  uint32_t instruction = ReadU32(begin);
  decoder.Decode(reinterpret_cast<vixl::Instruction*>(&instruction));
  os << StringPrintf("%p: %08x\t%s\n", begin, instruction, disasm.GetOutput());
  return vixl::kInstructionSize;
}

void DisassemblerArm64::Dump(std::ostream& os, const uint8_t* begin, const uint8_t* end) {
  for (const uint8_t* cur = begin; cur < end; cur += vixl::kInstructionSize) {
    Dump(os, cur);
  }
}

}  // namespace arm64
}  // namespace art
