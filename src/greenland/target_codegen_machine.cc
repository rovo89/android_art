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

#include "target_codegen_machine.h"

#include "lir_function.h"
#include "lir_pass_manager.h"
#include "target_lir_emitter.h"
#include "target_lir_opcodes.h"
#include "target_registry.h"

#include "compiled_method.h"
#include "compiler.h"
#include "oat_compilation_unit.h"
#include "utils.h"

#include <UniquePtr.h>

namespace art {
namespace greenland {

TargetCodeGenMachine* TargetCodeGenMachine::Create(InstructionSet insn_set) {
  TargetRegistry::TargetCodeGenMachineCtorTy ctor =
      TargetRegistry::GetTargetCodeGenMachineCtor(insn_set);

  if (ctor == NULL) {
    return NULL;
  }

  return (*ctor)();
}

CompiledMethod* TargetCodeGenMachine::Run(const Compiler& compiler,
                                          const GBCFunction& gbc_func) {
  LIRPassManager lir_pm;

  lir_pm.Add(CreateLIREmitter());

  LIRFunction lir_func(*this, gbc_func);

  if (!lir_pm.Run(lir_func)) {
    return NULL;
  }

  lir_func.Dump();

  // 0x90 is the NOP in x86
  std::vector<uint8_t> code(10, 0x90);

  return new CompiledMethod(compiler.GetInstructionSet(), code,
                            /* frame_size_in_bytes */0,
                            /* core_spill_mask */0,
                            /* fp_spill_mask */0);
}

} // namespace greenland
} // namespace art
