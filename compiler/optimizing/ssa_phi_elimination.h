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

#ifndef ART_COMPILER_OPTIMIZING_SSA_PHI_ELIMINATION_H_
#define ART_COMPILER_OPTIMIZING_SSA_PHI_ELIMINATION_H_

#include "nodes.h"

namespace art {

/**
 * Optimization phase that removes dead phis from the graph. Dead phis are unused
 * phis, or phis only used by other phis.
 */
class SsaDeadPhiElimination : public ValueObject {
 public:
  explicit SsaDeadPhiElimination(HGraph* graph)
      : graph_(graph), worklist_(graph->GetArena(), kDefaultWorklistSize) {}

  void Run();

 private:
  HGraph* const graph_;
  GrowableArray<HPhi*> worklist_;

  static constexpr size_t kDefaultWorklistSize = 8;

  DISALLOW_COPY_AND_ASSIGN(SsaDeadPhiElimination);
};

/**
 * Removes redundant phis that may have been introduced when doing SSA conversion.
 * For example, when entering a loop, we create phis for all live registers. These
 * registers might be updated with the same value, or not updated at all. We can just
 * replace the phi with the value when entering the loop.
 */
class SsaRedundantPhiElimination : public ValueObject {
 public:
  explicit SsaRedundantPhiElimination(HGraph* graph)
      : graph_(graph), worklist_(graph->GetArena(), kDefaultWorklistSize) {}

  void Run();

 private:
  HGraph* const graph_;
  GrowableArray<HPhi*> worklist_;

  static constexpr size_t kDefaultWorklistSize = 8;

  DISALLOW_COPY_AND_ASSIGN(SsaRedundantPhiElimination);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_SSA_PHI_ELIMINATION_H_
