// Copyright 2011 Google Inc. All Rights Reserved.
// Author: irogers@google.com (Ian Rogers)

#include "calling_convention.h"
#include "logging.h"

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
    return ManagedRegister::FromCoreRegister(R0);
  } else if (GetMethod()->IsReturnADouble()) {
    return ManagedRegister::FromRegisterPair(R0_R1);
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
  const Method* method = GetMethod();
  if (method->IsParamALongOrDouble(itr_position_)) {
    // TODO: handle a long/double split between registers and the stack, also
    // itr_position_ 0
    if (itr_position_ != 1u) {
      LOG(WARNING) << "Unimplemented";
    }
    return ManagedRegister::FromRegisterPair(R2_R3);
  } else {
    return
      ManagedRegister::FromCoreRegister(kManagedArgumentRegisters[itr_position_]);
  }
}

FrameOffset ManagedRuntimeCallingConvention::CurrentParamStackOffset() {
  CHECK_GE(itr_position_, 3u);
  return FrameOffset(displacement_.Int32Value() +
                 ((itr_position_ + itr_longs_and_doubles_ - 3) * kPointerSize));
}

// JNI calling convention

// Will reg be crushed by an outgoing argument?
bool JniCallingConvention::IsOutArgRegister(ManagedRegister) {
  // R0 holds the method register and will be crushed by the JNIEnv*
  return true;
}

bool JniCallingConvention::IsCurrentParamInRegister() {
  return (itr_position_ + itr_longs_and_doubles_) < 4;
}

bool JniCallingConvention::IsCurrentParamOnStack() {
  return (itr_position_ + itr_longs_and_doubles_) >= 4;
}

static const Register kJniArgumentRegisters[] = {
  R0, R1, R2, R3
};
ManagedRegister JniCallingConvention::CurrentParamRegister() {
  CHECK_LT(itr_position_ + itr_longs_and_doubles_, 4u);
  const Method* method = GetMethod();
  int arg_pos = itr_position_ - (method->IsStatic() ? 2 : 1);
  if ((itr_position_ >= 2) && method->IsParamALongOrDouble(arg_pos)) {
    // TODO: handle a long/double split between registers and the stack
    if (itr_position_ != 2u) {
      LOG(WARNING) << "Unimplemented";
    }
    return ManagedRegister::FromRegisterPair(R2_R3);
  } else {
    return
      ManagedRegister::FromCoreRegister(kJniArgumentRegisters[itr_position_]);
  }
}

FrameOffset JniCallingConvention::CurrentParamStackOffset() {
  CHECK_GE(itr_position_ + itr_longs_and_doubles_, 4u);
  return FrameOffset(displacement_.Int32Value() - OutArgSize()
               + ((itr_position_ + itr_longs_and_doubles_ - 4) * kPointerSize));
}

size_t JniCallingConvention::NumberOfOutgoingStackArgs() {
  return GetMethod()->NumArgs() + GetMethod()->NumLongOrDoubleArgs() - 2;
}

}  // namespace art
