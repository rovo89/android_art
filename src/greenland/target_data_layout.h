/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_GREENLAND_TARGET_DATA_LAYOUT_H_
#define ART_SRC_GREENLAND_TARGET_DATA_LAYOUT_H_

#include "lir.h"

#include "logging.h"

namespace art {
namespace greenland {

class TargetDataLayout {
 private:
  // Size of pointer in bytes
  unsigned pointer_size_;

  // Stack alignment in bytes
  unsigned stack_alignment_;

 public:
  TargetDataLayout(unsigned pointer_size, unsigned stack_alignment)
      : pointer_size_(pointer_size), stack_alignment_(stack_alignment) { }

  unsigned GetPointerSize() const {
    return pointer_size_;
  }
  unsigned GetPointerSizeInBits() const {
    return (pointer_size_ << 3);
  }

  unsigned GetStackAlignment() const {
    return stack_alignment_;
  }

  unsigned GetOpTypeSize(LIR::OperationType op_type) const {
    switch (op_type) {
      case LIR::kUInt8TypeOp:   return 1;   break;
      case LIR::kSInt8TypeOp:   return 1;   break;
      case LIR::kUInt16TypeOp:  return 2;   break;
      case LIR::kSInt16TypeOp:  return 2;   break;
      case LIR::kInt32TypeOp:   return 4;   break;
      case LIR::kInt64TypeOp:   return 8;   break;
      case LIR::kFloatTypeOp:   return 4;   break;
      case LIR::kDoubleTypeOp:  return 8;   break;
      case LIR::kPointerTypeOp: return pointer_size_;   break;
      case LIR::kUnknownTypeOp:
      default: {
        LOG(FATAL) << "Unknown operation type: " << op_type;
        return 0;
      }
    }
    // unreachable
  }
};

} // namespace greenland
} // namespace art

#endif // ART_SRC_GREENLAND_TARGET_DATA_LAYOUT_H_
