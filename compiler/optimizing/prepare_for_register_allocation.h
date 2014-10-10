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

#ifndef ART_COMPILER_OPTIMIZING_PREPARE_FOR_REGISTER_ALLOCATION_H_
#define ART_COMPILER_OPTIMIZING_PREPARE_FOR_REGISTER_ALLOCATION_H_

#include "nodes.h"

namespace art {

/**
 * A simplification pass over the graph before doing register allocation.
 * For example it changes uses of null checks and bounds checks to the original
 * objects, to avoid creating a live range for these checks.
 */
class PrepareForRegisterAllocation : public HGraphDelegateVisitor {
 public:
  explicit PrepareForRegisterAllocation(HGraph* graph) : HGraphDelegateVisitor(graph) {}

  void Run();

 private:
  virtual void VisitNullCheck(HNullCheck* check) OVERRIDE;
  virtual void VisitBoundsCheck(HBoundsCheck* check) OVERRIDE;
  virtual void VisitCondition(HCondition* condition) OVERRIDE;

  DISALLOW_COPY_AND_ASSIGN(PrepareForRegisterAllocation);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_PREPARE_FOR_REGISTER_ALLOCATION_H_
