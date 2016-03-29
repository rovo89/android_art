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

#include "code_generator.h"
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
  DCHECK(location.IsRegister()) << location;
  return vixl::Register::XRegFromCode(VIXLRegCodeFromART(location.reg()));
}

static inline vixl::Register WRegisterFrom(Location location) {
  DCHECK(location.IsRegister()) << location;
  return vixl::Register::WRegFromCode(VIXLRegCodeFromART(location.reg()));
}

static inline vixl::Register RegisterFrom(Location location, Primitive::Type type) {
  DCHECK(type != Primitive::kPrimVoid && !Primitive::IsFloatingPointType(type)) << type;
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
  DCHECK(location.IsFpuRegister()) << location;
  return vixl::FPRegister::DRegFromCode(location.reg());
}

static inline vixl::FPRegister SRegisterFrom(Location location) {
  DCHECK(location.IsFpuRegister()) << location;
  return vixl::FPRegister::SRegFromCode(location.reg());
}

static inline vixl::FPRegister FPRegisterFrom(Location location, Primitive::Type type) {
  DCHECK(Primitive::IsFloatingPointType(type)) << type;
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
    DCHECK(instr->IsLongConstant()) << instr->DebugName();
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

static inline vixl::MemOperand HeapOperand(const vixl::Register& base,
                                           const vixl::Register& regoffset,
                                           vixl::Shift shift = vixl::LSL,
                                           unsigned shift_amount = 0) {
  // A heap reference must be 32bit, so fit in a W register.
  DCHECK(base.IsW());
  return vixl::MemOperand(base.X(), regoffset, shift, shift_amount);
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
  DCHECK(constant->IsIntConstant() || constant->IsLongConstant() || constant->IsNullConstant())
      << constant->DebugName();

  // For single uses we let VIXL handle the constant generation since it will
  // use registers that are not managed by the register allocator (wip0, wip1).
  if (constant->GetUses().HasExactlyOneElement()) {
    return true;
  }

  // Our code generator ensures shift distances are within an encodable range.
  if (instr->IsRor()) {
    return true;
  }

  int64_t value = CodeGenerator::GetInt64ValueOf(constant);

  if (instr->IsAnd() || instr->IsOr() || instr->IsXor()) {
    // Uses logical operations.
    return vixl::Assembler::IsImmLogical(value, vixl::kXRegSize);
  } else if (instr->IsNeg()) {
    // Uses mov -immediate.
    return vixl::Assembler::IsImmMovn(value, vixl::kXRegSize);
  } else {
    DCHECK(instr->IsAdd() ||
           instr->IsArm64IntermediateAddress() ||
           instr->IsBoundsCheck() ||
           instr->IsCompare() ||
           instr->IsCondition() ||
           instr->IsSub())
        << instr->DebugName();
    // Uses aliases of ADD/SUB instructions.
    // If `value` does not fit but `-value` does, VIXL will automatically use
    // the 'opposite' instruction.
    return vixl::Assembler::IsImmAddSub(value) || vixl::Assembler::IsImmAddSub(-value);
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

// Check if registers in art register set have the same register code in vixl. If the register
// codes are same, we can initialize vixl register list simply by the register masks. Currently,
// only SP/WSP and ZXR/WZR codes are different between art and vixl.
// Note: This function is only used for debug checks.
static inline bool ArtVixlRegCodeCoherentForRegSet(uint32_t art_core_registers,
                                                   size_t num_core,
                                                   uint32_t art_fpu_registers,
                                                   size_t num_fpu) {
  // The register masks won't work if the number of register is larger than 32.
  DCHECK_GE(sizeof(art_core_registers) * 8, num_core);
  DCHECK_GE(sizeof(art_fpu_registers) * 8, num_fpu);
  for (size_t art_reg_code = 0;  art_reg_code < num_core; ++art_reg_code) {
    if (RegisterSet::Contains(art_core_registers, art_reg_code)) {
      if (art_reg_code != static_cast<size_t>(VIXLRegCodeFromART(art_reg_code))) {
        return false;
      }
    }
  }
  // There is no register code translation for float registers.
  return true;
}

static inline vixl::Shift ShiftFromOpKind(HArm64DataProcWithShifterOp::OpKind op_kind) {
  switch (op_kind) {
    case HArm64DataProcWithShifterOp::kASR: return vixl::ASR;
    case HArm64DataProcWithShifterOp::kLSL: return vixl::LSL;
    case HArm64DataProcWithShifterOp::kLSR: return vixl::LSR;
    default:
      LOG(FATAL) << "Unexpected op kind " << op_kind;
      UNREACHABLE();
      return vixl::NO_SHIFT;
  }
}

static inline vixl::Extend ExtendFromOpKind(HArm64DataProcWithShifterOp::OpKind op_kind) {
  switch (op_kind) {
    case HArm64DataProcWithShifterOp::kUXTB: return vixl::UXTB;
    case HArm64DataProcWithShifterOp::kUXTH: return vixl::UXTH;
    case HArm64DataProcWithShifterOp::kUXTW: return vixl::UXTW;
    case HArm64DataProcWithShifterOp::kSXTB: return vixl::SXTB;
    case HArm64DataProcWithShifterOp::kSXTH: return vixl::SXTH;
    case HArm64DataProcWithShifterOp::kSXTW: return vixl::SXTW;
    default:
      LOG(FATAL) << "Unexpected op kind " << op_kind;
      UNREACHABLE();
      return vixl::NO_EXTEND;
  }
}

static inline bool CanFitInShifterOperand(HInstruction* instruction) {
  if (instruction->IsTypeConversion()) {
    HTypeConversion* conversion = instruction->AsTypeConversion();
    Primitive::Type result_type = conversion->GetResultType();
    Primitive::Type input_type = conversion->GetInputType();
    // We don't expect to see the same type as input and result.
    return Primitive::IsIntegralType(result_type) && Primitive::IsIntegralType(input_type) &&
        (result_type != input_type);
  } else {
    return (instruction->IsShl() && instruction->AsShl()->InputAt(1)->IsIntConstant()) ||
        (instruction->IsShr() && instruction->AsShr()->InputAt(1)->IsIntConstant()) ||
        (instruction->IsUShr() && instruction->AsUShr()->InputAt(1)->IsIntConstant());
  }
}

static inline bool HasShifterOperand(HInstruction* instr) {
  // `neg` instructions are an alias of `sub` using the zero register as the
  // first register input.
  bool res = instr->IsAdd() || instr->IsAnd() || instr->IsNeg() ||
      instr->IsOr() || instr->IsSub() || instr->IsXor();
  return res;
}

static inline bool ShifterOperandSupportsExtension(HInstruction* instruction) {
  DCHECK(HasShifterOperand(instruction));
  // Although the `neg` instruction is an alias of the `sub` instruction, `HNeg`
  // does *not* support extension. This is because the `extended register` form
  // of the `sub` instruction interprets the left register with code 31 as the
  // stack pointer and not the zero register. (So does the `immediate` form.) In
  // the other form `shifted register, the register with code 31 is interpreted
  // as the zero register.
  return instruction->IsAdd() || instruction->IsSub();
}

}  // namespace helpers
}  // namespace arm64
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_COMMON_ARM64_H_
