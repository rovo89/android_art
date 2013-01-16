/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_SRC_OAT_RUNTIME_ARGUMENT_VISITOR_H_
#define ART_SRC_OAT_RUNTIME_ARGUMENT_VISITOR_H_

#include "object_utils.h"

namespace art {

// Visits the arguments as saved to the stack by a Runtime::kRefAndArgs callee save frame.
class ArgumentVisitor {
 public:
// Offset to first (not the Method*) argument in a Runtime::kRefAndArgs callee save frame.
// Size of Runtime::kRefAndArgs callee save frame.
// Size of Method* and register parameters in out stack arguments.
#if defined(__arm__)
#define CALLEE_SAVE_FRAME__REF_AND_ARGS__R1_OFFSET 8
#define CALLEE_SAVE_FRAME__REF_AND_ARGS__FRAME_SIZE 48
#define STACK_ARG_SKIP 16
#elif defined(__mips__)
#define CALLEE_SAVE_FRAME__REF_AND_ARGS__R1_OFFSET 4
#define CALLEE_SAVE_FRAME__REF_AND_ARGS__FRAME_SIZE 48
#define STACK_ARG_SKIP 16
#elif defined(__i386__)
#define CALLEE_SAVE_FRAME__REF_AND_ARGS__R1_OFFSET 4
#define CALLEE_SAVE_FRAME__REF_AND_ARGS__FRAME_SIZE 32
#define STACK_ARG_SKIP 16
#else
#error "Unsupported architecture"
#define CALLEE_SAVE_FRAME__REF_AND_ARGS__R1_OFFSET 0
#define CALLEE_SAVE_FRAME__REF_AND_ARGS__FRAME_SIZE 0
#define STACK_ARG_SKIP 0
#endif

  ArgumentVisitor(MethodHelper& caller_mh, AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) :
    caller_mh_(caller_mh),
    args_in_regs_(ComputeArgsInRegs(caller_mh)),
    num_params_(caller_mh.NumArgs()),
    reg_args_(reinterpret_cast<byte*>(sp) + CALLEE_SAVE_FRAME__REF_AND_ARGS__R1_OFFSET),
    stack_args_(reinterpret_cast<byte*>(sp) + CALLEE_SAVE_FRAME__REF_AND_ARGS__FRAME_SIZE
                + STACK_ARG_SKIP),
    cur_args_(reg_args_),
    cur_arg_index_(0),
    param_index_(0),
    is_split_long_or_double_(false) {
  }

  virtual ~ArgumentVisitor() {}

  virtual void Visit() = 0;

  bool IsParamAReference() const {
    return caller_mh_.IsParamAReference(param_index_);
  }

  bool IsParamALongOrDouble() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return caller_mh_.IsParamALongOrDouble(param_index_);
  }

  Primitive::Type GetParamPrimitiveType() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return caller_mh_.GetParamPrimitiveType(param_index_);
  }

  byte* GetParamAddress() const {
    return cur_args_ + (cur_arg_index_ * kPointerSize);
  }

  bool IsSplitLongOrDouble() const {
    return is_split_long_or_double_;
  }

  uint64_t ReadSplitLongParam() const {
    DCHECK(IsSplitLongOrDouble());
    uint64_t low_half = *reinterpret_cast<uint32_t*>(GetParamAddress());
    uint64_t high_half = *reinterpret_cast<uint32_t*>(stack_args_);
    return (low_half & 0xffffffffULL) | (high_half << 32);

  }

  void VisitArguments() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    for (cur_arg_index_ = 0;  cur_arg_index_ < args_in_regs_ && param_index_ < num_params_; ) {
      is_split_long_or_double_ = (cur_arg_index_ == 2) && IsParamALongOrDouble();
      Visit();
      cur_arg_index_ += (caller_mh_.IsParamALongOrDouble(param_index_) ? 2 : 1);
      param_index_++;
    }
    cur_args_ = stack_args_;
    cur_arg_index_ = is_split_long_or_double_ ? 1 : 0;
    is_split_long_or_double_ = false;
    while (param_index_ < num_params_) {
      Visit();
      cur_arg_index_ += (caller_mh_.IsParamALongOrDouble(param_index_) ? 2 : 1);
      param_index_++;
    }
  }

 private:
  static size_t ComputeArgsInRegs(MethodHelper& mh) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    size_t args_in_regs = 0;
    size_t num_params = mh.NumArgs();
    for (size_t i = 0; i < num_params; i++) {
      args_in_regs = args_in_regs + (mh.IsParamALongOrDouble(i) ? 2 : 1);
      if (args_in_regs > 3) {
        args_in_regs = 3;
        break;
      }
    }
    return args_in_regs;
  }
  MethodHelper& caller_mh_;
  const size_t args_in_regs_;
  const size_t num_params_;
  byte* const reg_args_;
  byte* const stack_args_;
  byte* cur_args_;
  size_t cur_arg_index_;
  size_t param_index_;
  // Does a 64bit parameter straddle the register and stack arguments?
  bool is_split_long_or_double_;
};

}

#endif  // ART_SRC_OAT_RUNTIME_ARGUMENT_VISITOR_H_
