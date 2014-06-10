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

#ifndef ART_COMPILER_OPTIMIZING_SSA_TYPE_PROPAGATION_H_
#define ART_COMPILER_OPTIMIZING_SSA_TYPE_PROPAGATION_H_

#include "nodes.h"

namespace art {

// Compute and propagate types of phis in the graph.
class SsaTypePropagation : public ValueObject {
 public:
  explicit SsaTypePropagation(HGraph* graph)
      : graph_(graph), worklist_(graph->GetArena(), kDefaultWorklistSize) {}

  void Run();

 private:
  void VisitBasicBlock(HBasicBlock* block);
  void ProcessWorklist();
  void AddToWorklist(HPhi* phi);
  void AddDependentInstructionsToWorklist(HPhi* phi);

  HGraph* const graph_;
  GrowableArray<HPhi*> worklist_;

  static constexpr size_t kDefaultWorklistSize = 8;

  DISALLOW_COPY_AND_ASSIGN(SsaTypePropagation);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_SSA_TYPE_PROPAGATION_H_
