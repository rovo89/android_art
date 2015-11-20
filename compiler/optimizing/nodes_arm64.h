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

namespace art {

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

  HInstruction* GetBaseAddress() const { return InputAt(0); }
  HInstruction* GetOffset() const { return InputAt(1); }

  DECLARE_INSTRUCTION(Arm64IntermediateAddress);

 private:
  DISALLOW_COPY_AND_ASSIGN(HArm64IntermediateAddress);
};

class HArm64MultiplyAccumulate : public HExpression<3> {
 public:
  HArm64MultiplyAccumulate(Primitive::Type type,
                           InstructionKind op,
                           HInstruction* accumulator,
                           HInstruction* mul_left,
                           HInstruction* mul_right,
                           uint32_t dex_pc = kNoDexPc)
      : HExpression(type, SideEffects::None(), dex_pc), op_kind_(op) {
    SetRawInputAt(kInputAccumulatorIndex, accumulator);
    SetRawInputAt(kInputMulLeftIndex, mul_left);
    SetRawInputAt(kInputMulRightIndex, mul_right);
  }

  static constexpr int kInputAccumulatorIndex = 0;
  static constexpr int kInputMulLeftIndex = 1;
  static constexpr int kInputMulRightIndex = 2;

  bool CanBeMoved() const OVERRIDE { return true; }
  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    return op_kind_ == other->AsArm64MultiplyAccumulate()->op_kind_;
  }

  InstructionKind GetOpKind() const { return op_kind_; }

  DECLARE_INSTRUCTION(Arm64MultiplyAccumulate);

 private:
  // Indicates if this is a MADD or MSUB.
  InstructionKind op_kind_;

  DISALLOW_COPY_AND_ASSIGN(HArm64MultiplyAccumulate);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_NODES_ARM64_H_
