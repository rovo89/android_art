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

#ifndef ART_COMPILER_OPTIMIZING_GRAPH_VISUALIZER_H_
#define ART_COMPILER_OPTIMIZING_GRAPH_VISUALIZER_H_

#include <ostream>

#include "base/value_object.h"

namespace art {

class CodeGenerator;
class DexCompilationUnit;
class HGraph;

/**
 * This class outputs the HGraph in the C1visualizer format.
 * Note: Currently only works if the compiler is single threaded.
 */
class HGraphVisualizer : public ValueObject {
 public:
  HGraphVisualizer(std::ostream* output,
                   HGraph* graph,
                   const CodeGenerator& codegen);

  void PrintHeader(const char* method_name) const;
  void DumpGraph(const char* pass_name, bool is_after_pass = true) const;

 private:
  std::ostream* const output_;
  HGraph* const graph_;
  const CodeGenerator& codegen_;

  DISALLOW_COPY_AND_ASSIGN(HGraphVisualizer);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_GRAPH_VISUALIZER_H_
