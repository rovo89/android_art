// Copyright 2011 Google Inc. All Rights Reserved.
// Author: irogers@google.com (Ian Rogers)

#include "src/calling_convention.h"
#include "src/logging.h"

namespace art {

ManagedRegister CallingConvention::MethodRegister() {
  return ManagedRegister::FromCoreRegister(R0);
}

ManagedRegister CallingConvention::InterproceduralScratchRegister() {
  return ManagedRegister::FromCoreRegister(R12);
}

ManagedRegister CallingConvention::ReturnRegister() {
  const Method *method = GetMethod();
  if (GetMethod()->IsReturnAFloat()) {
    return ManagedRegister::FromSRegister(S0);
  } else if (GetMethod()->IsReturnADouble()) {
    return ManagedRegister::FromDRegister(D0);
  } else if (method->IsReturnALong()) {
    return ManagedRegister::FromRegisterPair(R0_R1);
  } else if (method->IsReturnVoid()) {
    return ManagedRegister::NoRegister();
  } else {
    return ManagedRegister::FromCoreRegister(R0);
  }
}

// Managed runtime calling convention

bool ManagedRuntimeCallingConvention::IsCurrentParamInRegister() {
  return itr_position_ < 3;
}

bool ManagedRuntimeCallingConvention::IsCurrentParamOnStack() {
  return itr_position_ >= 3;
}

static const Register kManagedArgumentRegisters[] = {
  R1, R2, R3
};
ManagedRegister ManagedRuntimeCallingConvention::CurrentParamRegister() {
  CHECK_LT(itr_position_, 3u);
  return
    ManagedRegister::FromCoreRegister(kManagedArgumentRegisters[itr_position_]);
}

FrameOffset ManagedRuntimeCallingConvention::CurrentParamStackOffset() {
  CHECK_GE(itr_position_, 3u);
  return FrameOffset(displacement_.Int32Value() +
                 ((itr_position_ + itr_longs_and_doubles_ - 3) * kPointerSize));
}

// JNI calling convention

bool JniCallingConvention::IsCurrentParamInRegister() {
  return itr_position_ < 4;
}

bool JniCallingConvention::IsCurrentParamOnStack() {
  return itr_position_ >= 4;
}

static const Register kJniArgumentRegisters[] = {
  R0, R1, R2, R3
};
ManagedRegister JniCallingConvention::CurrentParamRegister() {
  CHECK_LT(itr_position_, 4u);
  return
    ManagedRegister::FromCoreRegister(kJniArgumentRegisters[itr_position_]);
}

FrameOffset JniCallingConvention::CurrentParamStackOffset() {
  CHECK_GE(itr_position_, 4u);
  return FrameOffset(displacement_.Int32Value() - OutArgSize()
               + ((itr_position_ + itr_longs_and_doubles_ - 4) * kPointerSize));
}

size_t JniCallingConvention::NumberOfOutgoingStackArgs() {
  return GetMethod()->NumArgs() + GetMethod()->NumLongOrDoubleArgs() - 2;
}

}  // namespace art
