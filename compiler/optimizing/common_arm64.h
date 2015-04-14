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

#ifndef ART_COMPILER_OPTIMIZING_COMMON_ARM64_H_
#define ART_COMPILER_OPTIMIZING_COMMON_ARM64_H_

#include "locations.h"
#include "nodes.h"
#include "utils/arm64/assembler_arm64.h"
#include "vixl/a64/disasm-a64.h"
#include "vixl/a64/macro-assembler-a64.h"

namespace art {
namespace arm64 {
namespace helpers {

// Convenience helpers to ease conversion to and from VIXL operands.
static_assert((SP == 31) && (WSP == 31) && (XZR == 32) && (WZR == 32),
              "Unexpected values for register codes.");

static inline int VIXLRegCodeFromART(int code) {
  if (code == SP) {
    return vixl::kSPRegInternalCode;
  }
  if (code == XZR) {
    return vixl::kZeroRegCode;
  }
  return code;
}

static inline int ARTRegCodeFromVIXL(int code) {
  if (code == vixl::kSPRegInternalCode) {
    return SP;
  }
  if (code == vixl::kZeroRegCode) {
    return XZR;
  }
  return code;
}

static inline vixl::Register XRegisterFrom(Location location) {
  DCHECK(location.IsRegister());
  return vixl::Register::XRegFromCode(VIXLRegCodeFromART(location.reg()));
}

static inline vixl::Register WRegisterFrom(Location location) {
  DCHECK(location.IsRegister());
  return vixl::Register::WRegFromCode(VIXLRegCodeFromART(location.reg()));
}

static inline vixl::Register RegisterFrom(Location location, Primitive::Type type) {
  DCHECK(type != Primitive::kPrimVoid && !Primitive::IsFloatingPointType(type));
  return type == Primitive::kPrimLong ? XRegisterFrom(location) : WRegisterFrom(location);
}

static inline vixl::Register OutputRegister(HInstruction* instr) {
  return RegisterFrom(instr->GetLocations()->Out(), instr->GetType());
}

static inline vixl::Register InputRegisterAt(HInstruction* instr, int input_index) {
  return RegisterFrom(instr->GetLocations()->InAt(input_index),
                      instr->InputAt(input_index)->GetType());
}

static inline vixl::FPRegister DRegisterFrom(Location location) {
  DCHECK(location.IsFpuRegister());
  return vixl::FPRegister::DRegFromCode(location.reg());
}

static inline vixl::FPRegister SRegisterFrom(Location location) {
  DCHECK(location.IsFpuRegister());
  return vixl::FPRegister::SRegFromCode(location.reg());
}

static inline vixl::FPRegister FPRegisterFrom(Location location, Primitive::Type type) {
  DCHECK(Primitive::IsFloatingPointType(type));
  return type == Primitive::kPrimDouble ? DRegisterFrom(location) : SRegisterFrom(location);
}

static inline vixl::FPRegister OutputFPRegister(HInstruction* instr) {
  return FPRegisterFrom(instr->GetLocations()->Out(), instr->GetType());
}

static inline vixl::FPRegister InputFPRegisterAt(HInstruction* instr, int input_index) {
  return FPRegisterFrom(instr->GetLocations()->InAt(input_index),
                        instr->InputAt(input_index)->GetType());
}

static inline vixl::CPURegister CPURegisterFrom(Location location, Primitive::Type type) {
  return Primitive::IsFloatingPointType(type) ? vixl::CPURegister(FPRegisterFrom(location, type))
                                              : vixl::CPURegister(RegisterFrom(location, type));
}

static inline vixl::CPURegister OutputCPURegister(HInstruction* instr) {
  return Primitive::IsFloatingPointType(instr->GetType())
      ? static_cast<vixl::CPURegister>(OutputFPRegister(instr))
      : static_cast<vixl::CPURegister>(OutputRegister(instr));
}

static inline vixl::CPURegister InputCPURegisterAt(HInstruction* instr, int index) {
  return Primitive::IsFloatingPointType(instr->InputAt(index)->GetType())
      ? static_cast<vixl::CPURegister>(InputFPRegisterAt(instr, index))
      : static_cast<vixl::CPURegister>(InputRegisterAt(instr, index));
}

static inline int64_t Int64ConstantFrom(Location location) {
  HConstant* instr = location.GetConstant();
  if (instr->IsIntConstant()) {
    return instr->AsIntConstant()->GetValue();
  } else if (instr->IsNullConstant()) {
    return 0;
  } else {
    DCHECK(instr->IsLongConstant());
    return instr->AsLongConstant()->GetValue();
  }
}

static inline vixl::Operand OperandFrom(Location location, Primitive::Type type) {
  if (location.IsRegister()) {
    return vixl::Operand(RegisterFrom(location, type));
  } else {
    return vixl::Operand(Int64ConstantFrom(location));
  }
}

static inline vixl::Operand InputOperandAt(HInstruction* instr, int input_index) {
  return OperandFrom(instr->GetLocations()->InAt(input_index),
                     instr->InputAt(input_index)->GetType());
}

static inline vixl::MemOperand StackOperandFrom(Location location) {
  return vixl::MemOperand(vixl::sp, location.GetStackIndex());
}

static inline vixl::MemOperand HeapOperand(const vixl::Register& base, size_t offset = 0) {
  // A heap reference must be 32bit, so fit in a W register.
  DCHECK(base.IsW());
  return vixl::MemOperand(base.X(), offset);
}

static inline vixl::MemOperand HeapOperand(const vixl::Register& base, Offset offset) {
  return HeapOperand(base, offset.SizeValue());
}

static inline vixl::MemOperand HeapOperandFrom(Location location, Offset offset) {
  return HeapOperand(RegisterFrom(location, Primitive::kPrimNot), offset);
}

static inline Location LocationFrom(const vixl::Register& reg) {
  return Location::RegisterLocation(ARTRegCodeFromVIXL(reg.code()));
}

static inline Location LocationFrom(const vixl::FPRegister& fpreg) {
  return Location::FpuRegisterLocation(fpreg.code());
}

static inline vixl::Operand OperandFromMemOperand(const vixl::MemOperand& mem_op) {
  if (mem_op.IsImmediateOffset()) {
    return vixl::Operand(mem_op.offset());
  } else {
    DCHECK(mem_op.IsRegisterOffset());
    if (mem_op.extend() != vixl::NO_EXTEND) {
      return vixl::Operand(mem_op.regoffset(), mem_op.extend(), mem_op.shift_amount());
    } else if (mem_op.shift() != vixl::NO_SHIFT) {
      return vixl::Operand(mem_op.regoffset(), mem_op.shift(), mem_op.shift_amount());
    } else {
      LOG(FATAL) << "Should not reach here";
      UNREACHABLE();
    }
  }
}

static bool CanEncodeConstantAsImmediate(HConstant* constant, HInstruction* instr) {
  DCHECK(constant->IsIntConstant() || constant->IsLongConstant() || constant->IsNullConstant());

  // For single uses we let VIXL handle the constant generation since it will
  // use registers that are not managed by the register allocator (wip0, wip1).
  if (constant->GetUses().HasOnlyOneUse()) {
    return true;
  }

  int64_t value = CodeGenerator::GetInt64ValueOf(constant);

  if (instr->IsAdd() || instr->IsSub() || instr->IsCondition() ||
      instr->IsCompare() || instr->IsBoundsCheck()) {
    // Uses aliases of ADD/SUB instructions.
    return vixl::Assembler::IsImmAddSub(value);
  } else if (instr->IsAnd() || instr->IsOr() || instr->IsXor()) {
    // Uses logical operations.
    return vixl::Assembler::IsImmLogical(value, vixl::kXRegSize);
  } else {
    DCHECK(instr->IsNeg());
    // Uses mov -immediate.
    return vixl::Assembler::IsImmMovn(value, vixl::kXRegSize);
  }
}

static inline Location ARM64EncodableConstantOrRegister(HInstruction* constant,
                                                        HInstruction* instr) {
  if (constant->IsConstant()
      && CanEncodeConstantAsImmediate(constant->AsConstant(), instr)) {
    return Location::ConstantLocation(constant->AsConstant());
  }

  return Location::RequiresRegister();
}

}  // namespace helpers
}  // namespace arm64
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_COMMON_ARM64_H_
