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

#include "common_arm64.h"
#include "nodes.h"

namespace art {

using arm64::helpers::CanFitInShifterOperand;

void HArm64DataProcWithShifterOp::GetOpInfoFromInstruction(HInstruction* instruction,
                                                           /*out*/OpKind* op_kind,
                                                           /*out*/int* shift_amount) {
  DCHECK(CanFitInShifterOperand(instruction));
  if (instruction->IsShl()) {
    *op_kind = kLSL;
    *shift_amount = instruction->AsShl()->GetRight()->AsIntConstant()->GetValue();
  } else if (instruction->IsShr()) {
    *op_kind = kASR;
    *shift_amount = instruction->AsShr()->GetRight()->AsIntConstant()->GetValue();
  } else if (instruction->IsUShr()) {
    *op_kind = kLSR;
    *shift_amount = instruction->AsUShr()->GetRight()->AsIntConstant()->GetValue();
  } else {
    DCHECK(instruction->IsTypeConversion());
    Primitive::Type result_type = instruction->AsTypeConversion()->GetResultType();
    Primitive::Type input_type = instruction->AsTypeConversion()->GetInputType();
    int result_size = Primitive::ComponentSize(result_type);
    int input_size = Primitive::ComponentSize(input_type);
    int min_size = std::min(result_size, input_size);
    // This follows the logic in
    // `InstructionCodeGeneratorARM64::VisitTypeConversion()`.
    if (result_type == Primitive::kPrimInt && input_type == Primitive::kPrimLong) {
      // There is actually nothing to do. The register will be used as a W
      // register, discarding the top bits. This is represented by the default
      // encoding 'LSL 0'.
      *op_kind = kLSL;
      *shift_amount = 0;
    } else if (result_type == Primitive::kPrimChar ||
               (input_type == Primitive::kPrimChar && input_size < result_size)) {
      *op_kind = kUXTH;
    } else {
      switch (min_size) {
        case 1: *op_kind = kSXTB; break;
        case 2: *op_kind = kSXTH; break;
        case 4: *op_kind = kSXTW; break;
        default:
          LOG(FATAL) << "Unexpected min size " << min_size;
      }
    }
  }
}

std::ostream& operator<<(std::ostream& os, const HArm64DataProcWithShifterOp::OpKind op) {
  switch (op) {
    case HArm64DataProcWithShifterOp::kLSL:  return os << "LSL";
    case HArm64DataProcWithShifterOp::kLSR:  return os << "LSR";
    case HArm64DataProcWithShifterOp::kASR:  return os << "ASR";
    case HArm64DataProcWithShifterOp::kUXTB: return os << "UXTB";
    case HArm64DataProcWithShifterOp::kUXTH: return os << "UXTH";
    case HArm64DataProcWithShifterOp::kUXTW: return os << "UXTW";
    case HArm64DataProcWithShifterOp::kSXTB: return os << "SXTB";
    case HArm64DataProcWithShifterOp::kSXTH: return os << "SXTH";
    case HArm64DataProcWithShifterOp::kSXTW: return os << "SXTW";
    default:
      LOG(FATAL) << "Invalid OpKind " << static_cast<int>(op);
      UNREACHABLE();
  }
}

}  // namespace art
