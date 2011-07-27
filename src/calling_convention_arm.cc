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
  if (itr_slots_ < 3) {
    return true;
  }
  return false;  // everything else on the stack
}

bool ManagedRuntimeCallingConvention::IsCurrentParamOnStack() {
  return !IsCurrentParamInRegister();
}

static const Register kManagedArgumentRegisters[] = {
  R1, R2, R3
};
ManagedRegister ManagedRuntimeCallingConvention::CurrentParamRegister() {
  CHECK_LT(itr_slots_, 3u); // Otherwise, should have gone through stack
  const Method* method = GetMethod();
  if (method->IsParamALongOrDouble(itr_args_)) {
    if (itr_slots_ == 0) {
      // return R1 to denote R1_R2
      return ManagedRegister::FromCoreRegister(kManagedArgumentRegisters
                                               [itr_slots_]);
    } else if (itr_slots_ == 1) {
      return ManagedRegister::FromRegisterPair(R2_R3);
    } else {
      // This is a long/double split between registers and the stack
      return ManagedRegister::FromCoreRegister(
        kManagedArgumentRegisters[itr_slots_]);
    }
  } else {
    return
      ManagedRegister::FromCoreRegister(kManagedArgumentRegisters[itr_slots_]);
  }
}

FrameOffset ManagedRuntimeCallingConvention::CurrentParamStackOffset() {
  CHECK_GE(itr_slots_, 3u);
  return FrameOffset(displacement_.Int32Value() +
             ((itr_slots_ - 3) * kPointerSize));
}

// JNI calling convention

// Will reg be crushed by an outgoing argument?
bool JniCallingConvention::IsOutArgRegister(ManagedRegister) {
  // R0 holds the method register and will be crushed by the JNIEnv*
  return true;
}

// JniCallingConvention ABI follows AAPCS
//
// In processing each parameter, we know that IsCurrentParamInRegister()
// or IsCurrentParamOnStack() will be called first.
// Both functions will ensure that we conform to AAPCS.
//
bool JniCallingConvention::IsCurrentParamInRegister() {
  // AAPCS processing
  const Method* method = GetMethod();
  int arg_pos = itr_args_ - NumberOfExtraArgumentsForJni(method);
  if ((itr_args_ >= 2) && method->IsParamALongOrDouble(arg_pos)) {
    // itr_slots_ needs to be an even number, according to AAPCS.
    if (itr_slots_ & 0x1u) {
      itr_slots_++;
    }
  }

  return itr_slots_ < 4;
}

bool JniCallingConvention::IsCurrentParamOnStack() {
  return !IsCurrentParamInRegister();
}

static const Register kJniArgumentRegisters[] = {
  R0, R1, R2, R3
};
ManagedRegister JniCallingConvention::CurrentParamRegister() {
  CHECK_LT(itr_slots_, 4u);
  const Method* method = GetMethod();
  int arg_pos = itr_args_ - NumberOfExtraArgumentsForJni(method);
  if ((itr_args_ >= 2) && method->IsParamALongOrDouble(arg_pos)) {
    CHECK_EQ(itr_slots_, 2u);
    return ManagedRegister::FromRegisterPair(R2_R3);
  } else {
    return
      ManagedRegister::FromCoreRegister(kJniArgumentRegisters[itr_slots_]);
  }
}

FrameOffset JniCallingConvention::CurrentParamStackOffset() {
  CHECK_GE(itr_slots_, 4u);
  return FrameOffset(displacement_.Int32Value() - OutArgSize()
               + ((itr_slots_ - 4) * kPointerSize));
}

size_t JniCallingConvention::NumberOfOutgoingStackArgs() {
  return GetMethod()->NumArgs() + GetMethod()->NumLongOrDoubleArgs() - 2;
}

}  // namespace art
