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

#ifndef ART_DISASSEMBLER_DISASSEMBLER_MIPS_H_
#define ART_DISASSEMBLER_DISASSEMBLER_MIPS_H_

#include <vector>

#include "disassembler.h"

namespace art {
namespace mips {

class DisassemblerMips FINAL : public Disassembler {
 public:
  DisassemblerMips(DisassemblerOptions* options, bool is64bit)
      : Disassembler(options),
        is64bit_(is64bit),
        last_ptr_(nullptr),
        last_instr_(0) {}

  size_t Dump(std::ostream& os, const uint8_t* begin) OVERRIDE;
  void Dump(std::ostream& os, const uint8_t* begin, const uint8_t* end) OVERRIDE;

 private:
  const bool is64bit_;

  // Address and encoding of the last disassembled instruction.
  // Needed to produce more readable disassembly of certain 2-instruction sequences.
  const uint8_t* last_ptr_;
  uint32_t last_instr_;

  DISALLOW_COPY_AND_ASSIGN(DisassemblerMips);
};

}  // namespace mips
}  // namespace art

#endif  // ART_DISASSEMBLER_DISASSEMBLER_MIPS_H_
