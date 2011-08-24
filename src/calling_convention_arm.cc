// Copyright 2011 Google Inc. All Rights Reserved.

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
  return itr_slots_ < 3;
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
  return FrameOffset(displacement_.Int32Value() + (itr_slots_ * kPointerSize));
}

// JNI calling convention

size_t JniCallingConvention::FrameSize() {
  // Method* and spill area size
  size_t frame_data_size = kPointerSize + SpillAreaSize();
  // References plus 2 words for SIRT header
  size_t sirt_size = (ReferenceCount() + 2) * kPointerSize;
  // Plus return value spill area size
  return RoundUp(frame_data_size + sirt_size + SizeOfReturnValue(),
                 kStackAlignment);
}

size_t JniCallingConvention::ReturnPcOffset() {
  // Link register is always the last value spilled, skip forward one word for
  // the Method* then skip back one word to get the link register (ie +0)
  return SpillAreaSize();
}

size_t JniCallingConvention::SpillAreaSize() {
  // Space for link register. For synchronized methods we need enough space to
  // save R1, R2 and R3 (R0 is the method register and always preserved)
  return GetMethod()->IsSynchronized() ? (4 * kPointerSize) : kPointerSize;
}

std::vector<ManagedRegister>* JniCallingConvention::ComputeRegsToSpillPreCall()
{
  // A synchronized method will call monitor enter clobbering R1, R2 and R3
  // unless they are spilled.
  std::vector<ManagedRegister>* result = new std::vector<ManagedRegister>();
  if (GetMethod()->IsSynchronized()) {
    result->push_back(ManagedRegister::FromCoreRegister(R1));
    result->push_back(ManagedRegister::FromCoreRegister(R2));
    result->push_back(ManagedRegister::FromCoreRegister(R3));
  }
  return result;
}

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
