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

#ifndef ART_SRC_GREENLAND_TARGET_CODEGEN_MACHINE_H_
#define ART_SRC_GREENLAND_TARGET_CODEGEN_MACHINE_H_

#include "instruction_set.h"

#include <string>

namespace art {
  class CompiledMethod;
  class Compiler;
}

namespace art {
namespace greenland {

class GBCFunction;
class LIR;
class RegisterAllocator;
class TargetAssembler;
class TargetDataLayout;
class TargetLIRBuilder;
class TargetLIREmitter;
class TargetLIRInfo;
class TargetRegisterInfo;

class TargetCodeGenMachine {
 protected:
  TargetCodeGenMachine() { }

 public:
  virtual ~TargetCodeGenMachine() { }

  virtual TargetLIREmitter* CreateLIREmitter() = 0;

  virtual const TargetDataLayout* GetDataLayout() const = 0;

  virtual const TargetLIRInfo* GetLIRInfo() const = 0;

  virtual const TargetRegisterInfo* GetRegisterInfo() const = 0;

  virtual const char* GetConditionCodeName(unsigned cond) const = 0;

  virtual TargetLIRBuilder* CreateLIRBuilder() = 0;

  virtual RegisterAllocator* GetRegisterAllocator() = 0;

  virtual TargetAssembler* GetAssembler() = 0;

  static TargetCodeGenMachine* Create(InstructionSet insn_set);

 public:
  CompiledMethod* Run(const Compiler& compiler, const GBCFunction& gbc_func);
};

} // namespace greenland
} // namespace art

#endif // ART_SRC_GREENLAND_TARGET_CODEGEN_MACHINE_H_
