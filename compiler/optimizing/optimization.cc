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

#include "optimization.h"

#include "base/dumpable.h"
#include "graph_checker.h"

namespace art {

void HOptimization::Execute() {
  Run();
  visualizer_.DumpGraph(pass_name_);
  Check();
}

void HOptimization::Check() {
  if (kIsDebugBuild) {
    if (is_in_ssa_form_) {
      SSAChecker checker(graph_->GetArena(), graph_);
      checker.Run();
      if (!checker.IsValid()) {
        LOG(FATAL) << Dumpable<SSAChecker>(checker);
      }
    } else {
      GraphChecker checker(graph_->GetArena(), graph_);
      checker.Run();
      if (!checker.IsValid()) {
        LOG(FATAL) << Dumpable<GraphChecker>(checker);
      }
    }
  }
}

}  // namespace art
