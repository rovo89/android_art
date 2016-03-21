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
#include "code_generator.h"

namespace art {

LocationSummary::LocationSummary(HInstruction* instruction,
                                 CallKind call_kind,
                                 bool intrinsified)
    : inputs_(instruction->InputCount(),
              instruction->GetBlock()->GetGraph()->GetArena()->Adapter(kArenaAllocLocationSummary)),
      temps_(instruction->GetBlock()->GetGraph()->GetArena()->Adapter(kArenaAllocLocationSummary)),
      output_overlaps_(Location::kOutputOverlap),
      call_kind_(call_kind),
      stack_mask_(nullptr),
      register_mask_(0),
      live_registers_(),
      intrinsified_(intrinsified) {
  instruction->SetLocations(this);

  if (NeedsSafepoint()) {
    ArenaAllocator* arena = instruction->GetBlock()->GetGraph()->GetArena();
    stack_mask_ = ArenaBitVector::Create(arena, 0, true, kArenaAllocLocationSummary);
  }
}


Location Location::RegisterOrConstant(HInstruction* instruction) {
  return instruction->IsConstant()
      ? Location::ConstantLocation(instruction->AsConstant())
      : Location::RequiresRegister();
}

Location Location::RegisterOrInt32Constant(HInstruction* instruction) {
  HConstant* constant = instruction->AsConstant();
  if (constant != nullptr) {
    int64_t value = CodeGenerator::GetInt64ValueOf(constant);
    if (IsInt<32>(value)) {
      return Location::ConstantLocation(constant);
    }
  }
  return Location::RequiresRegister();
}

Location Location::FpuRegisterOrInt32Constant(HInstruction* instruction) {
  HConstant* constant = instruction->AsConstant();
  if (constant != nullptr) {
    int64_t value = CodeGenerator::GetInt64ValueOf(constant);
    if (IsInt<32>(value)) {
      return Location::ConstantLocation(constant);
    }
  }
  return Location::RequiresFpuRegister();
}

Location Location::ByteRegisterOrConstant(int reg, HInstruction* instruction) {
  return instruction->IsConstant()
      ? Location::ConstantLocation(instruction->AsConstant())
      : Location::RegisterLocation(reg);
}

Location Location::FpuRegisterOrConstant(HInstruction* instruction) {
  return instruction->IsConstant()
      ? Location::ConstantLocation(instruction->AsConstant())
      : Location::RequiresFpuRegister();
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
