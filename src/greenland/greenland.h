/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_GREENLAND_GREENLAND_H_
#define ART_SRC_GREENLAND_GREENLAND_H_

#include "macros.h"
#include "object.h"

namespace art {
  class CompiledMethod;
  class Compiler;
  class OatCompilationUnit;
}

namespace art {
namespace greenland {

class GBCContext;
class TargetCodeGenMachine;

class Greenland {
 public:
  Greenland(art::Compiler& compiler);
  ~Greenland();

  CompiledMethod* Compile(OatCompilationUnit& cunit);

  const Compiler& GetCompiler() const {
    return compiler_;
  }
  Compiler& GetCompiler() {
    return compiler_;
  }

 private:
  Compiler& compiler_;

  TargetCodeGenMachine* codegen_machine_;

  Mutex lock_;

  // NOTE: Ensure that the lock_ is held before altering cur_gbc_ctx
  GBCContext *cur_gbc_ctx_;

  GBCContext& GetGBCContext();
  void ResetGBCContextIfThresholdReached();

  DISALLOW_COPY_AND_ASSIGN(Greenland);
};

} // namespace greenland
} // namespace art

#endif // ART_SRC_GREENLAND_GREENLAND_H_
