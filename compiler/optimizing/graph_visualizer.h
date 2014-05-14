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

#include "utils/allocation.h"

namespace art {

class DexCompilationUnit;
class HGraph;

/**
 * If enabled, emits compilation information suitable for the c1visualizer tool
 * and IRHydra.
 * Currently only works if the compiler is single threaded.
 */
class HGraphVisualizer : public ValueObject {
 public:
  /**
   * If output is not null, and the method name of the dex compilation
   * unit contains `string_filter`, the compilation information will be
   * emitted.
   */
  HGraphVisualizer(std::ostream* output,
                   HGraph* graph,
                   const char* string_filter,
                   const DexCompilationUnit& cu);

  /**
   * If this visualizer is enabled, emit the compilation information
   * in `output_`.
   */
  void DumpGraph(const char* pass_name);

 private:
  std::ostream* const output_;
  HGraph* const graph_;

  // Is true when `output_` is not null, and the compiled method's name
  // contains the string_filter given in the constructor.
  bool is_enabled_;

  DISALLOW_COPY_AND_ASSIGN(HGraphVisualizer);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_GRAPH_VISUALIZER_H_
