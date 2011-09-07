// Copyright 2011 Google Inc. All Rights Reserved.

#include "calling_convention.h"
#include "logging.h"

namespace art {

ManagedRegister CallingConvention::MethodRegister() {
  return ManagedRegister::FromCoreRegister(R0);
}

ManagedRegister CallingConvention::ThreadRegister() {
  return ManagedRegister::FromCoreRegister(TR);
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
  if (itr_slots_ < 2) {
    return false;
  } else if (itr_slots_ > 2) {
    return true;
  } else {
    // handle funny case of a long/double straddling registers and the stack
    return GetMethod()->IsParamALongOrDouble(itr_args_);
  }
}

static const Register kManagedArgumentRegisters[] = {
  R1, R2, R3
};
ManagedRegister ManagedRuntimeCallingConvention::CurrentParamRegister() {
  CHECK(IsCurrentParamInRegister());
  const Method* method = GetMethod();
  if (method->IsParamALongOrDouble(itr_args_)) {
    if (itr_slots_ == 0) {
      return ManagedRegister::FromRegisterPair(R1_R2);
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
  CHECK(IsCurrentParamOnStack());
  FrameOffset result =
      FrameOffset(displacement_.Int32Value() +   // displacement
                  kPointerSize +                 // Method*
                  (itr_slots_ * kPointerSize));  // offset into in args
  if (itr_slots_ == 2) {
    // the odd spanning case, bump the offset to skip the first half of the
    // input which is in a register
    CHECK(IsCurrentParamInRegister());
    result = FrameOffset(result.Int32Value() + 4);
  }
  return result;
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

size_t JniCallingConvention::OutArgSize() {
  const Method* method = GetMethod();
  size_t padding;  // padding to ensure longs and doubles are not split in AAPCS
  if (method->IsStatic()) {
    padding = (method->NumArgs() > 1) && !method->IsParamALongOrDouble(0) &&
              method->IsParamALongOrDouble(1) ? 4 : 0;
  } else {
    padding = (method->NumArgs() > 2) && !method->IsParamALongOrDouble(1) &&
              method->IsParamALongOrDouble(2) ? 4 : 0;
  }
  return RoundUp(NumberOfOutgoingStackArgs() * kPointerSize + padding,
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

void JniCallingConvention::ComputeRegsToSpillPreCall(std::vector<ManagedRegister>& regs) {
  // A synchronized method will call monitor enter clobbering R1, R2 and R3
  // unless they are spilled.
  if (GetMethod()->IsSynchronized()) {
    regs.push_back(ManagedRegister::FromCoreRegister(R1));
    regs.push_back(ManagedRegister::FromCoreRegister(R2));
    regs.push_back(ManagedRegister::FromCoreRegister(R3));
  }
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
  const Method* method = GetMethod();
  size_t static_args = method->IsStatic() ? 1 : 0;  // count jclass
  // regular argument parameters and this
  size_t param_args = method->NumArgs() +
                      method->NumLongOrDoubleArgs();
  // count JNIEnv* less arguments in registers
  return static_args + param_args + 1 - 4;
}

}  // namespace art
