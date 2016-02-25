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

#ifndef ART_COMPILER_OPTIMIZING_NODES_SHARED_H_
#define ART_COMPILER_OPTIMIZING_NODES_SHARED_H_

namespace art {

class HMultiplyAccumulate : public HExpression<3> {
 public:
  HMultiplyAccumulate(Primitive::Type type,
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
    return op_kind_ == other->AsMultiplyAccumulate()->op_kind_;
  }

  InstructionKind GetOpKind() const { return op_kind_; }

  DECLARE_INSTRUCTION(MultiplyAccumulate);

 private:
  // Indicates if this is a MADD or MSUB.
  const InstructionKind op_kind_;

  DISALLOW_COPY_AND_ASSIGN(HMultiplyAccumulate);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_NODES_SHARED_H_
