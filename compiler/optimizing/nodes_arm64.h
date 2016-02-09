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

#ifndef ART_COMPILER_OPTIMIZING_NODES_ARM64_H_
#define ART_COMPILER_OPTIMIZING_NODES_ARM64_H_

#include "nodes.h"

namespace art {

class HArm64DataProcWithShifterOp : public HExpression<2> {
 public:
  enum OpKind {
    kLSL,   // Logical shift left.
    kLSR,   // Logical shift right.
    kASR,   // Arithmetic shift right.
    kUXTB,  // Unsigned extend byte.
    kUXTH,  // Unsigned extend half-word.
    kUXTW,  // Unsigned extend word.
    kSXTB,  // Signed extend byte.
    kSXTH,  // Signed extend half-word.
    kSXTW,  // Signed extend word.

    // Aliases.
    kFirstShiftOp = kLSL,
    kLastShiftOp = kASR,
    kFirstExtensionOp = kUXTB,
    kLastExtensionOp = kSXTW
  };
  HArm64DataProcWithShifterOp(HInstruction* instr,
                              HInstruction* left,
                              HInstruction* right,
                              OpKind op,
                              // The shift argument is unused if the operation
                              // is an extension.
                              int shift = 0,
                              uint32_t dex_pc = kNoDexPc)
      : HExpression(instr->GetType(), SideEffects::None(), dex_pc),
        instr_kind_(instr->GetKind()), op_kind_(op), shift_amount_(shift) {
    DCHECK(!instr->HasSideEffects());
    SetRawInputAt(0, left);
    SetRawInputAt(1, right);
  }

  bool CanBeMoved() const OVERRIDE { return true; }
  bool InstructionDataEquals(HInstruction* other_instr) const OVERRIDE {
    HArm64DataProcWithShifterOp* other = other_instr->AsArm64DataProcWithShifterOp();
    return instr_kind_ == other->instr_kind_ &&
        op_kind_ == other->op_kind_ &&
        shift_amount_ == other->shift_amount_;
  }

  static bool IsShiftOp(OpKind op_kind) {
    return kFirstShiftOp <= op_kind && op_kind <= kLastShiftOp;
  }

  static bool IsExtensionOp(OpKind op_kind) {
    return kFirstExtensionOp <= op_kind && op_kind <= kLastExtensionOp;
  }

  // Find the operation kind and shift amount from a bitfield move instruction.
  static void GetOpInfoFromInstruction(HInstruction* bitfield_op,
                                       /*out*/OpKind* op_kind,
                                       /*out*/int* shift_amount);

  InstructionKind GetInstrKind() const { return instr_kind_; }
  OpKind GetOpKind() const { return op_kind_; }
  int GetShiftAmount() const { return shift_amount_; }

  DECLARE_INSTRUCTION(Arm64DataProcWithShifterOp);

 private:
  InstructionKind instr_kind_;
  OpKind op_kind_;
  int shift_amount_;

  friend std::ostream& operator<<(std::ostream& os, OpKind op);

  DISALLOW_COPY_AND_ASSIGN(HArm64DataProcWithShifterOp);
};

std::ostream& operator<<(std::ostream& os, const HArm64DataProcWithShifterOp::OpKind op);

// This instruction computes an intermediate address pointing in the 'middle' of an object. The
// result pointer cannot be handled by GC, so extra care is taken to make sure that this value is
// never used across anything that can trigger GC.
class HArm64IntermediateAddress : public HExpression<2> {
 public:
  HArm64IntermediateAddress(HInstruction* base_address, HInstruction* offset, uint32_t dex_pc)
      : HExpression(Primitive::kPrimNot, SideEffects::DependsOnGC(), dex_pc) {
    SetRawInputAt(0, base_address);
    SetRawInputAt(1, offset);
  }

  bool CanBeMoved() const OVERRIDE { return true; }
  bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const OVERRIDE { return true; }
  bool IsActualObject() const OVERRIDE { return false; }

  HInstruction* GetBaseAddress() const { return InputAt(0); }
  HInstruction* GetOffset() const { return InputAt(1); }

  DECLARE_INSTRUCTION(Arm64IntermediateAddress);

 private:
  DISALLOW_COPY_AND_ASSIGN(HArm64IntermediateAddress);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_NODES_ARM64_H_
