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

#ifndef ART_SRC_GREENLAND_LIR_H_
#define ART_SRC_GREENLAND_LIR_H_

#include "logging.h"
#include "macros.h"
#include "object.h"

#include "lir_desc.h"
#include "lir_operand.h"

#include <vector>

#include <llvm/ADT/ilist_node.h>

namespace art {
namespace greenland {

class LIRFunction;

class LIR : public llvm::ilist_node<LIR> {
 private:
  // LIRs are allocated and owned by LIRFunction.
  friend class LIRFunction;

  LIR(const LIRDesc& desc);

  ~LIR() { }

 private:
  LIRFunction* parent_;

  const LIRDesc& desc_; // Instruction descriptor

  std::vector<LIROperand> operands_;

#if 0
  struct {
    bool is_nop:1;          // LIR is optimized away
    bool pc_rel_fixup:1;    // May need pc-relative fixup
    unsigned int age:4;     // default is 0, set lazily by the optimizer
    unsigned int size:5;    // in bytes
    unsigned int unused:21;
  } flags_;

  int alias_info_;          // For Dalvik register & litpool disambiguation
  uint8_t use_mask_;        // Resource mask for use
  uint8_t def_mask_;        // Resource mask for def
#endif

 private:
  // Intrusive list support
  friend struct llvm::ilist_traits<LIR>;
  friend struct llvm::ilist_traits<LIRFunction>;
  void SetParent(LIRFunction *parent) {
    parent_ = parent;
  }

 public:
  const LIRFunction* GetParent() const {
    return parent_;
  }

  LIRFunction* GetParent() {
    return parent_;
  }

  const LIRDesc& GetDesc() const {
    return desc_;
  }

  int GetOpcode() const {
    return desc_.opcode_;
  }

 public:
  //----------------------------------------------------------------------------
  // Operand Operations
  //----------------------------------------------------------------------------
  unsigned GetNumOperands() const {
    return static_cast<unsigned>(operands_.size());
  }

  const LIROperand& GetOperand(unsigned i) const {
    DCHECK(i < GetNumOperands()) << "GetOperand() out of range!";
    return operands_[i];
  }

  LIROperand& GetOperand(unsigned i) {
    DCHECK(i < GetNumOperands()) << "GetOperand() out of range!";
    return operands_[i];
  }

  /// iterator/begin/end - Iterate over all operands of a machine instruction.
  typedef std::vector<LIROperand>::iterator op_iterator;
  typedef std::vector<LIROperand>::const_iterator const_op_iterator;

  op_iterator operands_begin() { return operands_.begin(); }
  op_iterator operands_end() { return operands_.end(); }

  const_op_iterator operands_begin() const { return operands_.begin(); }
  const_op_iterator operands_end() const { return operands_.end(); }

 private:
  DISALLOW_COPY_AND_ASSIGN(LIR);
};

} // namespace greenland
} // namespace art

#endif // ART_SRC_GREENLAND_LIR_H_
