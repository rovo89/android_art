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

class HBitwiseNegatedRight : public HBinaryOperation {
 public:
  HBitwiseNegatedRight(Primitive::Type result_type,
                            InstructionKind op,
                            HInstruction* left,
                            HInstruction* right,
                            uint32_t dex_pc = kNoDexPc)
    : HBinaryOperation(result_type, left, right, SideEffects::None(), dex_pc),
      op_kind_(op) {
    DCHECK(op == HInstruction::kAnd || op == HInstruction::kOr || op == HInstruction::kXor) << op;
  }

  template <typename T, typename U>
  auto Compute(T x, U y) const -> decltype(x & ~y) {
    static_assert(std::is_same<decltype(x & ~y), decltype(x | ~y)>::value &&
                  std::is_same<decltype(x & ~y), decltype(x ^ ~y)>::value,
                  "Inconsistent negated bitwise types");
    switch (op_kind_) {
      case HInstruction::kAnd:
        return x & ~y;
      case HInstruction::kOr:
        return x | ~y;
      case HInstruction::kXor:
        return x ^ ~y;
      default:
        LOG(FATAL) << "Unreachable";
        UNREACHABLE();
    }
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HFloatConstant* x ATTRIBUTE_UNUSED,
                      HFloatConstant* y ATTRIBUTE_UNUSED) const OVERRIDE {
    LOG(FATAL) << DebugName() << " is not defined for float values";
    UNREACHABLE();
  }
  HConstant* Evaluate(HDoubleConstant* x ATTRIBUTE_UNUSED,
                      HDoubleConstant* y ATTRIBUTE_UNUSED) const OVERRIDE {
    LOG(FATAL) << DebugName() << " is not defined for double values";
    UNREACHABLE();
  }

  InstructionKind GetOpKind() const { return op_kind_; }

  DECLARE_INSTRUCTION(BitwiseNegatedRight);

 private:
  // Specifies the bitwise operation, which will be then negated.
  const InstructionKind op_kind_;

  DISALLOW_COPY_AND_ASSIGN(HBitwiseNegatedRight);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_NODES_SHARED_H_
