/*
 * Copyright (C) 2008 The Android Open Source Project
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


#include "fault_handler.h"
#include <sys/ucontext.h>
#include "base/macros.h"
#include "globals.h"
#include "base/logging.h"
#include "base/hex_dump.h"


//
// Mips specific fault handler functions.
//

namespace art {

void FaultManager::HandleNestedSignal(int sig ATTRIBUTE_UNUSED, siginfo_t* info ATTRIBUTE_UNUSED,
                                      void* context ATTRIBUTE_UNUSED) {
}

void FaultManager::GetMethodAndReturnPcAndSp(siginfo_t* siginfo ATTRIBUTE_UNUSED,
                                             void* context ATTRIBUTE_UNUSED,
                                             mirror::ArtMethod** out_method ATTRIBUTE_UNUSED,
                                             uintptr_t* out_return_pc ATTRIBUTE_UNUSED,
                                             uintptr_t* out_sp ATTRIBUTE_UNUSED) {
}

bool NullPointerHandler::Action(int sig ATTRIBUTE_UNUSED, siginfo_t* info ATTRIBUTE_UNUSED,
                                void* context ATTRIBUTE_UNUSED) {
  return false;
}

bool SuspensionHandler::Action(int sig ATTRIBUTE_UNUSED, siginfo_t* info ATTRIBUTE_UNUSED,
                               void* context ATTRIBUTE_UNUSED) {
  return false;
}

bool StackOverflowHandler::Action(int sig ATTRIBUTE_UNUSED, siginfo_t* info ATTRIBUTE_UNUSED,
                                  void* context ATTRIBUTE_UNUSED) {
  return false;
}
}       // namespace art
