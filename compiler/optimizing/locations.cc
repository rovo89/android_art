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

#include "locations.h"

#include "nodes.h"

namespace art {

LocationSummary::LocationSummary(HInstruction* instruction, CallKind call_kind)
    : inputs_(instruction->GetBlock()->GetGraph()->GetArena(), instruction->InputCount()),
      temps_(instruction->GetBlock()->GetGraph()->GetArena(), 0),
      environment_(instruction->GetBlock()->GetGraph()->GetArena(),
                   instruction->EnvironmentSize()),
      call_kind_(call_kind),
      stack_mask_(nullptr),
      register_mask_(0),
      live_registers_() {
  inputs_.SetSize(instruction->InputCount());
  for (size_t i = 0; i < instruction->InputCount(); ++i) {
    inputs_.Put(i, Location());
  }
  environment_.SetSize(instruction->EnvironmentSize());
  for (size_t i = 0; i < instruction->EnvironmentSize(); ++i) {
    environment_.Put(i, Location());
  }
  instruction->SetLocations(this);

  if (NeedsSafepoint()) {
    ArenaAllocator* arena = instruction->GetBlock()->GetGraph()->GetArena();
    stack_mask_ = new (arena) ArenaBitVector(arena, 0, true);
  }
}


Location Location::RegisterOrConstant(HInstruction* instruction) {
  return instruction->IsConstant()
      ? Location::ConstantLocation(instruction->AsConstant())
      : Location::RequiresRegister();
}

}  // namespace art
