/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "greenland/target_registry.h"

#include "logging.h"

namespace art {
  class Compiler;
  class CompiledInvokeStub;
}

namespace {

art::CompiledInvokeStub* MipsInvokeStubCompiler(art::Compiler& /*compiler*/,
                                                bool is_static,
                                                const char* shorty,
                                                uint32_t shorty_len) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

} // anonymous namespace

namespace art {
namespace greenland {

void InitializeMipsInvokeStubCompiler() {
  TargetRegistry::RegisterInvokeStubCompiler(kMips, MipsInvokeStubCompiler);
}

} // namespace greenland
} // namespace art
