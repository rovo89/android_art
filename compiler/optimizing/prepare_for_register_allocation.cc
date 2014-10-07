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

#include "prepare_for_register_allocation.h"

namespace art {

void PrepareForRegisterAllocation::Run() {
  // Order does not matter.
  for (HReversePostOrderIterator it(*GetGraph()); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    // No need to visit the phis.
    for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
      it.Current()->Accept(this);
    }
  }
}

void PrepareForRegisterAllocation::VisitNullCheck(HNullCheck* check) {
  check->ReplaceWith(check->InputAt(0));
}

void PrepareForRegisterAllocation::VisitBoundsCheck(HBoundsCheck* check) {
  check->ReplaceWith(check->InputAt(0));
}

}  // namespace art
