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

#include "x86_codegen_machine.h"

#include "x86_lir_emitter.h"

#include "greenland/target_registry.h"

namespace art {
namespace greenland {

void InitializeX86CodeGenMachine() {
  RegisterTargetCodeGenMachine<X86CodeGenMachine> X(kX86);
}

X86CodeGenMachine::X86CodeGenMachine() : lir_info_() {
}

X86CodeGenMachine::~X86CodeGenMachine() {
}

TargetLIREmitter*
X86CodeGenMachine::CreateLIREmitter(const llvm::Function& func,
                                    const OatCompilationUnit& cunit,
                                    DexLang::Context& dex_lang_ctx) {
  return new X86LIREmitter(func, cunit, dex_lang_ctx, lir_info_);
}

} // namespace greenland
} // namespace art
