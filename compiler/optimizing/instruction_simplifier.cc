/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "instruction_simplifier.h"

namespace art {

void InstructionSimplifier::Run() {
  VisitInsertionOrder();
}

void InstructionSimplifier::VisitSuspendCheck(HSuspendCheck* check) {
  HBasicBlock* block = check->GetBlock();
  // Currently always keep the suspend check at entry.
  if (block->IsEntryBlock()) return;

  // Currently always keep suspend checks at loop entry.
  if (block->IsLoopHeader() && block->GetFirstInstruction() == check) {
    DCHECK(block->GetLoopInformation()->GetSuspendCheck() == check);
    return;
  }

  // Remove the suspend check that was added at build time for the baseline
  // compiler.
  block->RemoveInstruction(check);
}

}  // namespace art
