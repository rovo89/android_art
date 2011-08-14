// Copyright 2011 Google Inc. All Rights Reserved.

#include "calling_convention.h"
#include "logging.h"
#include "utils.h"

namespace art {

ManagedRegister CallingConvention::MethodRegister() {
  return ManagedRegister::FromCpuRegister(EDI);
}

ManagedRegister CallingConvention::InterproceduralScratchRegister() {
  return ManagedRegister::FromCpuRegister(ECX);
}

ManagedRegister CallingConvention::ReturnRegister() {
  const Method *method = GetMethod();
  if (method->IsReturnAFloatOrDouble()) {
    return ManagedRegister::FromX87Register(ST0);
  } else if (method->IsReturnALong()) {
    return ManagedRegister::FromRegisterPair(EAX_EDX);
  } else if (method->IsReturnVoid()) {
    return ManagedRegister::NoRegister();
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
  return FrameOffset(displacement_.Int32Value() + (itr_slots_ * kPointerSize));
}

// JNI calling convention

bool JniCallingConvention::IsOutArgRegister(ManagedRegister) {
  return false;  // Everything is passed by stack
}

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
  return FrameOffset(displacement_.Int32Value() - OutArgSize() +
                     (itr_slots_ * kPointerSize));
}

size_t JniCallingConvention::NumberOfOutgoingStackArgs() {
  return GetMethod()->NumArgs() + GetMethod()->NumLongOrDoubleArgs();
}

}  // namespace art
