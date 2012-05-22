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

#ifndef ART_SRC_GREENLAND_LIR_OPERAND_H_
#define ART_SRC_GREENLAND_LIR_OPERAND_H_

#include "logging.h"

namespace llvm {
  class ConstantFP;
}

namespace art {
namespace greenland {

class LIR;

class LIROperand {
 public:
  enum Type {
    UnknownType,
    RegisterType,
    ImmediateType,
    FPImmediateType,
    LabelType,
  };

 private:
  Type type_;

  union {
    // RegisterType
    unsigned reg_no_;

    // ImmediateType
    int64_t imm_val_;

    // FPImmediateType
    const llvm::ConstantFP *fp_imm_val_;

    // LabelType
    const LIR* target_;
  } contents_;

  friend class LIR;
  LIROperand() : type_(UnknownType) { }

  void SetType(enum Type type) {
    type_ = type;
    return;
  }

 public:
  enum Type GetType() const {
    return type_;
  }

  bool IsReg() const {
    return (type_ == RegisterType);
  }
  bool IsImm() const {
    return (type_ == ImmediateType);
  }
  bool IsFPImm() const {
    return (type_ == FPImmediateType);
  }
  bool IsLabel() const {
    return (type_ == LabelType);
  }

  //----------------------------------------------------------------------------
  //  Accessors
  //----------------------------------------------------------------------------
  unsigned GetReg() const {
    CHECK(IsReg()) << "This is not a register operand!";
    return contents_.reg_no_;
  }

  int64_t GetImm() const {
    CHECK(IsImm()) << "This is not a immediate operand!";
    return contents_.imm_val_;
  }

  const llvm::ConstantFP* GetFPImm() const {
    CHECK(IsFPImm()) << "This is not a FP immediate operand!";
    return contents_.fp_imm_val_;
  }

  const LIR* GetLabelTarget() const {
    CHECK(IsFPImm()) << "This is not a label operand!";
    return contents_.target_;
  }

  //----------------------------------------------------------------------------
  //  Mutators
  //----------------------------------------------------------------------------
  void SetReg(unsigned reg_no) {
    if (type_ == UnknownType) {
      type_ = RegisterType;
    }
    CHECK(IsReg()) << "This is not a register operand!";
    contents_.reg_no_ = reg_no;
    return;
  }

  void SetImm(int64_t imm_val) {
    if (type_ == UnknownType) {
      type_ = ImmediateType;
    }
    CHECK(IsImm()) << "This is not a immediate operand!";
    contents_.imm_val_ = imm_val;
    return;
  }

  void SetFPImm(const llvm::ConstantFP* fp_imm_val) {
    if (type_ == UnknownType) {
      type_ = FPImmediateType;
    }
    CHECK(IsFPImm()) << "This is not a FP immediate operand!";
    contents_.fp_imm_val_ = fp_imm_val;
    return;
  }

  void SetLabelTarget(LIR* target) {
    if (type_ == UnknownType) {
      type_ = LabelType;
    }
    CHECK(IsLabel()) << "This is not a label operand!";
    contents_.target_ = target;
    return;
  }

};

} // namespace greenland
} // namespace art

#endif // ART_SRC_GREENLAND_LIR_OPERAND_H_
