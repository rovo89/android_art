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

#ifndef ART_COMPILER_OPTIMIZING_SSA_BUILDER_H_
#define ART_COMPILER_OPTIMIZING_SSA_BUILDER_H_

#include "nodes.h"

namespace art {

static constexpr int kDefaultNumberOfLoops = 2;

class SsaBuilder : public HGraphVisitor {
 public:
  explicit SsaBuilder(HGraph* graph)
      : HGraphVisitor(graph),
        current_locals_(nullptr),
        loop_headers_(graph->GetArena(), kDefaultNumberOfLoops),
        locals_for_(graph->GetArena(), graph->GetBlocks().Size()) {
    locals_for_.SetSize(graph->GetBlocks().Size());
  }

  void BuildSsa();

  GrowableArray<HInstruction*>* GetLocalsFor(HBasicBlock* block) {
    HEnvironment* env = locals_for_.Get(block->GetBlockId());
    if (env == nullptr) {
      env = new (GetGraph()->GetArena()) HEnvironment(
          GetGraph()->GetArena(), GetGraph()->GetNumberOfVRegs());
      locals_for_.Put(block->GetBlockId(), env);
    }
    return env->GetVRegs();
  }

  HInstruction* ValueOfLocal(HBasicBlock* block, size_t local);

  void VisitBasicBlock(HBasicBlock* block);
  void VisitLoadLocal(HLoadLocal* load);
  void VisitStoreLocal(HStoreLocal* store);
  void VisitInstruction(HInstruction* instruction);

 private:
  // Locals for the current block being visited.
  GrowableArray<HInstruction*>* current_locals_;

  // Keep track of loop headers found. The last phase of the analysis iterates
  // over these blocks to set the inputs of their phis.
  GrowableArray<HBasicBlock*> loop_headers_;

  // HEnvironment for each block.
  GrowableArray<HEnvironment*> locals_for_;

  DISALLOW_COPY_AND_ASSIGN(SsaBuilder);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_SSA_BUILDER_H_
