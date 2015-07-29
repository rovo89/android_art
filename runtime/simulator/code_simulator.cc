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

#include "simulator/code_simulator.h"
#include "simulator/code_simulator_arm64.h"

namespace art {

CodeSimulator* CodeSimulator::CreateCodeSimulator(InstructionSet target_isa) {
  DCHECK(CanSimulate(target_isa));
  switch (target_isa) {
    case kArm64:
      return new arm64::CodeSimulatorArm64();
    default:
      UNREACHABLE();
  }
}

bool CodeSimulator::CanSimulate(InstructionSet target_isa) {
  switch (target_isa) {
    case kArm64:
      return arm64::CodeSimulatorArm64::CanSimulateArm64();
    default:
      // No simulator support for target.
      return false;
  }
}

}  // namespace art
