// Copyright 2011 Google Inc. All Rights Reserved.
// Author: irogers@google.com (Ian Rogers)

#include "src/calling_convention.h"
#include "src/logging.h"
#include "src/utils.h"

namespace art {

ManagedRegister CallingConvention::MethodRegister() {
  return ManagedRegister::FromCpuRegister(EDI);
}

ManagedRegister CallingConvention::InterproceduralScratchRegister() {
  return ManagedRegister::FromCpuRegister(ECX);
}

ManagedRegister CallingConvention::ReturnRegister() {
  if (GetMethod()->IsReturnAFloatOrDouble()) {
    return ManagedRegister::FromX87Register(ST0);
  } else if (GetMethod()->IsReturnALong()) {
    return ManagedRegister::FromRegisterPair(EAX_EDX);
  } else {
    return ManagedRegister::FromCpuRegister(EAX);
  }
}

// Managed runtime calling convention

bool ManagedRuntimeCallingConvention::IsCurrentParamInRegister() {
  return false;  // Everything is passed by stack
}

bool ManagedRuntimeCallingConvention::IsCurrentParamOnStack() {
  return true;  // Everything is passed by stack
}

ManagedRegister ManagedRuntimeCallingConvention::CurrentParamRegister() {
  LOG(FATAL) << "Should not reach here";
  return ManagedRegister::NoRegister();
}

FrameOffset ManagedRuntimeCallingConvention::CurrentParamStackOffset() {
  int num_slots = itr_position_ + itr_longs_and_doubles_;
  return FrameOffset(displacement_.Int32Value() + (num_slots * kPointerSize));
}

// JNI calling convention

bool JniCallingConvention::IsCurrentParamInRegister() {
  return false;  // Everything is passed by stack
}

bool JniCallingConvention::IsCurrentParamOnStack() {
  return true;  // Everything is passed by stack
}

ManagedRegister JniCallingConvention::CurrentParamRegister() {
  LOG(FATAL) << "Should not reach here";
  return ManagedRegister::NoRegister();
}

FrameOffset JniCallingConvention::CurrentParamStackOffset() {
  int num_slots = itr_position_ + itr_longs_and_doubles_;
  return FrameOffset(displacement_.Int32Value() - OutArgSize() +
                     (num_slots * kPointerSize));
}

size_t JniCallingConvention::NumberOfOutgoingStackArgs() {
  return GetMethod()->NumArgs() + GetMethod()->NumLongOrDoubleArgs();
}

}  // namespace art
