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

LocationSummary::LocationSummary(HInstruction* instruction,
                                 CallKind call_kind,
                                 bool intrinsified)
    : inputs_(instruction->GetBlock()->GetGraph()->GetArena(), instruction->InputCount()),
      temps_(instruction->GetBlock()->GetGraph()->GetArena(), 0),
      output_overlaps_(Location::kOutputOverlap),
      call_kind_(call_kind),
      stack_mask_(nullptr),
      register_mask_(0),
      live_registers_(),
      intrinsified_(intrinsified) {
  inputs_.SetSize(instruction->InputCount());
  for (size_t i = 0; i < instruction->InputCount(); ++i) {
    inputs_.Put(i, Location());
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

Location Location::RegisterOrInt32LongConstant(HInstruction* instruction) {
  if (!instruction->IsConstant() || !instruction->AsConstant()->IsLongConstant()) {
    return Location::RequiresRegister();
  }

  // Does the long constant fit in a 32 bit int?
  int64_t value = instruction->AsConstant()->AsLongConstant()->GetValue();

  return IsInt<32>(value)
      ? Location::ConstantLocation(instruction->AsConstant())
      : Location::RequiresRegister();
}

Location Location::ByteRegisterOrConstant(int reg, HInstruction* instruction) {
  return instruction->IsConstant()
      ? Location::ConstantLocation(instruction->AsConstant())
      : Location::RegisterLocation(reg);
}

std::ostream& operator<<(std::ostream& os, const Location& location) {
  os << location.DebugString();
  if (location.IsRegister() || location.IsFpuRegister()) {
    os << location.reg();
  } else if (location.IsPair()) {
    os << location.low() << ":" << location.high();
  } else if (location.IsStackSlot() || location.IsDoubleStackSlot()) {
    os << location.GetStackIndex();
  }
  return os;
}

}  // namespace art
