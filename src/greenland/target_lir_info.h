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

#ifndef ART_SRC_GREENLAND_TARGET_LIR_INFO_H_
#define ART_SRC_GREENLAND_TARGET_LIR_INFO_H_

#include "lir_desc.h"

#include "logging.h"

namespace art {
namespace greenland {

class TargetLIRInfo {
 private:
  // An array of target's LIR instruction description
  const LIRDesc* desc_;

  unsigned num_desc_;

 public:
  TargetLIRInfo(const LIRDesc* desc, unsigned num_desc)
      : desc_(desc), num_desc_(num_desc) {
  }

  virtual ~TargetLIRInfo() { }

  const LIRDesc& GetLIRDesc(unsigned opcode) {
    DCHECK(opcode < num_desc_) << "Invalid opcode: " << opcode;
    return desc_[opcode];
  }
};

} // namespace greenland
} // namespace art

#endif // ART_SRC_GREENLAND_TARGET_LIR_INFO_H_
