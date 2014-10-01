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

#ifndef ART_COMPILER_OPTIMIZING_CONSTANT_PROPAGATION_H_
#define ART_COMPILER_OPTIMIZING_CONSTANT_PROPAGATION_H_

#include "nodes.h"

namespace art {

/**
 * Optimization pass performing a simple constant propagation on the
 * SSA form.
 */
class ConstantPropagation : public ValueObject {
 public:
  explicit ConstantPropagation(HGraph* graph)
    : graph_(graph) {}

  void Run();

 private:
  HGraph* const graph_;

  DISALLOW_COPY_AND_ASSIGN(ConstantPropagation);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CONSTANT_PROPAGATION_H_
