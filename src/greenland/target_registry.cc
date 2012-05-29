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

#include "target_registry.h"

#include "logging.h"

namespace {

// The following arrays contain the real registry data store by Register*()
// methods in TargetRegistry. Should keep sync with enum InstructionSet.
art::greenland::TargetRegistry::TargetCodeGenMachineCtorTy
RegisteredTargetCodeGenMachineCtor[] = {
  NULL, /* kNone   */
  NULL, /* kArm    */
  NULL, /* kThumb2 */
  NULL, /* kX86    */
  NULL, /* kMips   */
};

art::greenland::TargetRegistry::CreateInvokeStubFn
RegisteredInvokeStubCompiler[] = {
  NULL, /* kNone   */
  NULL, /* kArm    */
  NULL, /* kThumb2 */
  NULL, /* kX86    */
  NULL, /* kMips   */
};

} // anonymous namespace

namespace art {
namespace greenland {

void TargetRegistry::RegisterTargetCodeGenMachine(InstructionSet insn_set,
                                            TargetCodeGenMachineCtorTy ctor) {
  CHECK(static_cast<unsigned>(insn_set) <
          (sizeof(RegisteredTargetCodeGenMachineCtor) / sizeof(RegisteredTargetCodeGenMachineCtor[0])));
  RegisteredTargetCodeGenMachineCtor[static_cast<unsigned>(insn_set)] = ctor;
  return;
}

TargetRegistry::TargetCodeGenMachineCtorTy
TargetRegistry::GetTargetCodeGenMachineCtor(InstructionSet insn_set) {
  CHECK(static_cast<unsigned>(insn_set) <
          (sizeof(RegisteredTargetCodeGenMachineCtor) / sizeof(RegisteredTargetCodeGenMachineCtor[0])));
  return RegisteredTargetCodeGenMachineCtor[static_cast<unsigned>(insn_set)];
}

void
TargetRegistry::RegisterInvokeStubCompiler(InstructionSet insn_set,
                                           CreateInvokeStubFn compiler_fn) {
  CHECK(static_cast<unsigned>(insn_set) <
        (sizeof(RegisteredInvokeStubCompiler) / sizeof(RegisteredInvokeStubCompiler[0])));
  RegisteredInvokeStubCompiler[static_cast<unsigned>(insn_set)] = compiler_fn;
  return;
}

TargetRegistry::CreateInvokeStubFn
TargetRegistry::GetInvokeStubCompiler(InstructionSet insn_set) {
  CHECK(static_cast<unsigned>(insn_set) <
        (sizeof(RegisteredInvokeStubCompiler) / sizeof(RegisteredInvokeStubCompiler[0])));
  return RegisteredInvokeStubCompiler[static_cast<unsigned>(insn_set)];
}

} // namespace greenland
} // namespace art
