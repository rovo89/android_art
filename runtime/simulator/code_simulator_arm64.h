/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ART_RUNTIME_SIMULATOR_CODE_SIMULATOR_ARM64_H_
#define ART_RUNTIME_SIMULATOR_CODE_SIMULATOR_ARM64_H_

#include "memory"
#include "simulator/code_simulator.h"
// TODO: make vixl clean wrt -Wshadow.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmissing-noreturn"
#include "vixl/a64/simulator-a64.h"
#pragma GCC diagnostic pop

namespace art {
namespace arm64 {

class CodeSimulatorArm64 : public CodeSimulator {
 public:
  CodeSimulatorArm64();
  virtual ~CodeSimulatorArm64();

  static constexpr bool CanSimulateArm64() {
    return kCanSimulate;
  }

  void RunFrom(intptr_t code_buffer) OVERRIDE;

  bool GetCReturnBool() OVERRIDE;
  int32_t GetCReturnInt32() OVERRIDE;
  int64_t GetCReturnInt64() OVERRIDE;

 private:
  vixl::Decoder* decoder_;
  vixl::Simulator* simulator_;

  // TODO: Enable CodeSimulatorArm64 for more host ISAs once vixl::Simulator supports
  // them.
  static constexpr bool kCanSimulate = (kRuntimeISA == kX86_64);
};

}  // namespace arm64
}  // namespace art

#endif  // ART_RUNTIME_SIMULATOR_CODE_SIMULATOR_ARM64_H_
