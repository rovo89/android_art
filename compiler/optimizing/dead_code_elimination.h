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

#ifndef ART_COMPILER_OPTIMIZING_DEAD_CODE_ELIMINATION_H_
#define ART_COMPILER_OPTIMIZING_DEAD_CODE_ELIMINATION_H_

#include "nodes.h"

namespace art {

/**
 * Optimization pass performing dead code elimination (removal of
 * unused variables/instructions) on the SSA form.
 */
class DeadCodeElimination : public ValueObject {
 public:
  explicit DeadCodeElimination(HGraph* graph)
      : graph_(graph) {}

  void Run();

 private:
  HGraph* const graph_;

  DISALLOW_COPY_AND_ASSIGN(DeadCodeElimination);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_DEAD_CODE_ELIMINATION_H_
