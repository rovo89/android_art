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

#ifndef ART_SRC_GREENLAND_TARGET_REGISTRY_H_
#define ART_SRC_GREENLAND_TARGET_REGISTRY_H_

#include "instruction_set.h"

#include <stdint.h>

namespace art {
  class Compiler;
  class CompiledInvokeStub;
}

namespace art {
namespace greenland {

class TargetCodeGenMachine;

class TargetRegistry {
 public:
  typedef TargetCodeGenMachine* (*TargetCodeGenMachineCtorTy)();
  typedef CompiledInvokeStub* (*CreateInvokeStubFn)(Compiler& compiler,
                                                    bool is_static,
                                                    const char* shorty,
                                                    uint32_t shorty_len);

  static void RegisterTargetCodeGenMachine(InstructionSet insn_set,
                                           TargetCodeGenMachineCtorTy ctor);
  static TargetCodeGenMachineCtorTy GetTargetCodeGenMachineCtor(InstructionSet insn_set);

  static void RegisterInvokeStubCompiler(InstructionSet insn_set,
                                         CreateInvokeStubFn compiler_fn);
  static CreateInvokeStubFn GetInvokeStubCompiler(InstructionSet insn_set);
};

template<class TargetCodeGenMachineImpl>
class RegisterTargetCodeGenMachine {
 private:
  static TargetCodeGenMachine* Allocator() {
    return new TargetCodeGenMachineImpl();
  }

 public:
  RegisterTargetCodeGenMachine(InstructionSet insn_set) {
    TargetRegistry::RegisterTargetCodeGenMachine(insn_set, &Allocator);
  }
};


} // namespace greenland
} // namespace art

#endif // ART_SRC_GREENLAND_TARGET_REGISTRY_H_
