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

#include "constant_propagation.h"

namespace art {

void ConstantPropagation::Run() {
  // Process basic blocks in reverse post-order in the dominator tree,
  // so that an instruction turned into a constant, used as input of
  // another instruction, may possibly be used to turn that second
  // instruction into a constant as well.
  for (HReversePostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    // Traverse this block's instructions in (forward) order and
    // replace the ones that can be statically evaluated by a
    // compile-time counterpart.
    for (HInstructionIterator it(block->GetInstructions());
         !it.Done(); it.Advance()) {
      HInstruction* inst = it.Current();
      // Constant folding: replace `c <- a op b' with a compile-time
      // evaluation of `a op b' if `a' and `b' are constant.
      if (inst->IsBinaryOperation()) {
        HConstant* constant =
          inst->AsBinaryOperation()->TryStaticEvaluation(graph_->GetArena());
        if (constant != nullptr) {
          inst->GetBlock()->ReplaceAndRemoveInstructionWith(inst, constant);
        }
      }
    }
  }
}

}  // namespace art
