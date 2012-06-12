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

#ifndef ART_SRC_GREENLAND_TARGET_REGISTER_INFO_H_
#define ART_SRC_GREENLAND_TARGET_REGISTER_INFO_H_

#include "lir_function.h"
#include "lir_reg.h"

#include "target_lir_builder.h"
#include "target_lir_info.h"

#include <string>

namespace art {
namespace greenland {

class TargetRegisterInfo {
 protected:
  const TargetLIRInfo& lir_info_;
  TargetRegisterInfo(const TargetLIRInfo& info) :lir_info_(info) { }

 public:
  virtual const char* GetPhyRegName(unsigned reg) const = 0;

  std::string GetRegName(unsigned reg) const {
    if (LIRReg::IsVirtualReg(reg)) {
      return LIRReg::GetVirtualRegName(reg);
    } else {
      return GetPhyRegName(reg);
    }
  }

  virtual LIR*
  CreateLoadStack(LIRFunction& lir_func,
                  unsigned reg,
                  int frame_idx) const = 0;

  virtual LIR*
  CreateStoreStack(LIRFunction& lir_func,
                   unsigned reg,
                   int frame_idx) const = 0;

  virtual LIR*
  CreateMoveReg(LIRFunction& lir_func,
                unsigned dst,
                unsigned src) const = 0;

  virtual bool
  IsLoadIncomingArgs(const LIR* lir) const = 0;

  virtual LIR*
  CreateCopy(LIRFunction& lir_func,
             unsigned dst,
             const LIROperand& src) const = 0;

  virtual std::list<unsigned> GetAllocatableList() const = 0;
  virtual unsigned GetTempRegsiter(unsigned idx) const = 0;

  virtual ~TargetRegisterInfo() { }
};

} // namespace greenland
} // namespace art

#endif // ART_SRC_GREENLAND_TARGET_REGISTER_INFO_H_
