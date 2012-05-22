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

#include "greenland.h"
#include "target_lir_emitter.h"
#include "target_registry.h"

#include "compiled_method.h"
#include "compiler.h"

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

CompiledMethod* TargetCodeGenMachine::Run(const Greenland& parent,
                                          const llvm::Function& func,
                                          const OatCompilationUnit& cunit,
                                          DexLang::Context& dex_lang_ctx) {
#if 0
  UniquePtr<TargetLIREmitter> emitter(CreateLIREmitter(func, cunit,
                                                       dex_lang_ctx));
  LIRFunction* lir_func = emitter->Emit();
  if (lir_func == NULL) {
    return NULL;
  }
#endif

  // 0x90 is the NOP in x86
  std::vector<uint8_t> code(10, 0x90);

  return new CompiledMethod(parent.GetCompiler().GetInstructionSet(), code,
                            /* frame_size_in_bytes */0,
                            /* core_spill_mask */0,
                            /* fp_spill_mask */0);
}

} // namespace greenland
} // namespace art
