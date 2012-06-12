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

#ifndef ART_SRC_GREENLAND_ARM_CODEGEN_MACHINE_H_
#define ART_SRC_GREENLAND_ARM_CODEGEN_MACHINE_H_

#include "greenland/target_codegen_machine.h"

namespace art {
namespace greenland {

class ARMCodeGenMachine : public TargetCodeGenMachine {
 private:

 public:
  ARMCodeGenMachine();
  virtual ~ARMCodeGenMachine();

  virtual TargetLIREmitter* CreateLIREmitter() {
    return NULL;
  }

  virtual const TargetDataLayout* GetDataLayout() const {
    return NULL;
  }

  virtual const TargetLIRInfo* GetLIRInfo() const {
    return NULL;
  }

  virtual const TargetRegisterInfo* GetRegisterInfo() const {
    return NULL;
  }

  virtual const char* GetConditionCodeName(unsigned cond) const {
    return NULL;
  }

  virtual TargetLIRBuilder* CreateLIRBuilder() {
    return NULL;
  }

  virtual RegisterAllocator* GetRegisterAllocator() {
    return NULL;
  }

  virtual TargetAssembler* GetAssembler() {
    return NULL;
  }

  virtual std::string PrettyTargeteLIR(const LIR& lir) const {
    return "";
  }
};

} // namespace greenland
} // namespace art

#endif // ART_SRC_GREENLAND_ARM_CODEGEN_MACHINE_H_
