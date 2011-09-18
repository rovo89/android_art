// Copyright 2011 Google Inc. All Rights Reserved.

#include "calling_convention_arm.h"
#include "logging.h"
#include "managed_register_arm.h"

namespace art {
namespace arm {

// Calling convention

ManagedRegister ArmManagedRuntimeCallingConvention::InterproceduralScratchRegister() {
  return ArmManagedRegister::FromCoreRegister(IP);  // R12
}

ManagedRegister ArmJniCallingConvention::InterproceduralScratchRegister() {
  return ArmManagedRegister::FromCoreRegister(IP);  // R12
}

static ManagedRegister ReturnRegisterForMethod(Method* method) {
  if (method->IsReturnAFloat()) {
    return ArmManagedRegister::FromCoreRegister(R0);
  } else if (method->IsReturnADouble()) {
    return ArmManagedRegister::FromRegisterPair(R0_R1);
  } else if (method->IsReturnALong()) {
    return ArmManagedRegister::FromRegisterPair(R0_R1);
  } else if (method->IsReturnVoid()) {
    return ArmManagedRegister::NoRegister();
  } else {
    return ArmManagedRegister::FromCoreRegister(R0);
  }
}

ManagedRegister ArmManagedRuntimeCallingConvention::ReturnRegister() {
  return ReturnRegisterForMethod(GetMethod());
}

ManagedRegister ArmJniCallingConvention::ReturnRegister() {
  return ReturnRegisterForMethod(GetMethod());
}

// Managed runtime calling convention

ManagedRegister ArmManagedRuntimeCallingConvention::MethodRegister() {
  return ArmManagedRegister::FromCoreRegister(R0);
}

bool ArmManagedRuntimeCallingConvention::IsCurrentParamInRegister() {
  return itr_slots_ < 3;
}

bool ArmManagedRuntimeCallingConvention::IsCurrentParamOnStack() {
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
ManagedRegister ArmManagedRuntimeCallingConvention::CurrentParamRegister() {
  CHECK(IsCurrentParamInRegister());
  const Method* method = GetMethod();
  if (method->IsParamALongOrDouble(itr_args_)) {
    if (itr_slots_ == 0) {
      return ArmManagedRegister::FromRegisterPair(R1_R2);
    } else if (itr_slots_ == 1) {
      return ArmManagedRegister::FromRegisterPair(R2_R3);
    } else {
      // This is a long/double split between registers and the stack
      return ArmManagedRegister::FromCoreRegister(
        kManagedArgumentRegisters[itr_slots_]);
    }
  } else {
    return
      ArmManagedRegister::FromCoreRegister(kManagedArgumentRegisters[itr_slots_]);
  }
}

FrameOffset ArmManagedRuntimeCallingConvention::CurrentParamStackOffset() {
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

ArmJniCallingConvention::ArmJniCallingConvention(Method* method) : JniCallingConvention(method) {
  // Compute padding to ensure longs and doubles are not split in AAPCS
  // TODO: in terms of outgoing argument size this may be overly generous
  // due to padding appearing in the registers
  size_t padding = 0;
  size_t check = method->IsStatic() ? 1 : 0;
  for(size_t i = 0; i < method->NumArgs(); i++) {
    if(((i & 1) == check) && method->IsParamALongOrDouble(i)) {
      padding += 4;
    }
  }
  padding_ = padding;
  if (method->IsSynchronized()) {
    // Preserve callee saves that may be clobbered during monitor enter where
    // we copy across R0 to R3
    if (method->NumArgs() > 0) {
      callee_save_regs_.push_back(ArmManagedRegister::FromCoreRegister(R4));
      if (method->NumArgs() > 1) {
        callee_save_regs_.push_back(ArmManagedRegister::FromCoreRegister(R5));
        if (method->NumArgs() > 2) {
          callee_save_regs_.push_back(ArmManagedRegister::FromCoreRegister(R6));
          if (method->NumArgs() > 3) {
            callee_save_regs_.push_back(ArmManagedRegister::FromCoreRegister(R7));
          }
        }
      }
    }
  }
}

uint32_t ArmJniCallingConvention::CoreSpillMask() const {
  // Compute spill mask to agree with callee saves initialized in the constructor
  uint32_t result = 0;
  Method* method = GetMethod();
  if (method->IsSynchronized()) {
    if (method->NumArgs() > 0) {
      result |= 1 << R4;
      if (method->NumArgs() > 1) {
        result |= 1 << R5;
        if (method->NumArgs() > 2) {
          result |= 1 << R6;
          if (method->NumArgs() > 3) {
            result |= 1 << R7;
          }
        }
      }
    }
  }
  return result;
}

size_t ArmJniCallingConvention::FrameSize() {
  // Method*, LR and callee save area size
  size_t frame_data_size = (2 + CalleeSaveRegisters().size()) * kPointerSize;
  // References plus 2 words for SIRT header
  size_t sirt_size = (ReferenceCount() + 2) * kPointerSize;
  // Plus return value spill area size
  return RoundUp(frame_data_size + sirt_size + SizeOfReturnValue(),
                 kStackAlignment);
}

size_t ArmJniCallingConvention::OutArgSize() {
  return RoundUp(NumberOfOutgoingStackArgs() * kPointerSize + padding_,
                 kStackAlignment);
}

size_t ArmJniCallingConvention::ReturnPcOffset() {
  // Link register is always the first value pushed when the frame is constructed
  return FrameSize() - kPointerSize;
}

// Will reg be crushed by an outgoing argument?
bool ArmJniCallingConvention::IsMethodRegisterClobberedPreCall() {
  return true;  // The method register R0 is always clobbered by the JNIEnv
}

// JniCallingConvention ABI follows AAPCS where longs and doubles must occur
// in even register numbers and stack slots
void ArmJniCallingConvention::Next() {
  JniCallingConvention::Next();
  Method* method = GetMethod();
  size_t arg_pos = itr_args_ - NumberOfExtraArgumentsForJni(method);
  if ((itr_args_ >= 2) &&
      (arg_pos < GetMethod()->NumArgs()) &&
      method->IsParamALongOrDouble(arg_pos)) {
    // itr_slots_ needs to be an even number, according to AAPCS.
    if ((itr_slots_ & 0x1u) != 0) {
      itr_slots_++;
    }
  }
}

bool ArmJniCallingConvention::IsCurrentParamInRegister() {
  return itr_slots_ < 4;
}

bool ArmJniCallingConvention::IsCurrentParamOnStack() {
  return !IsCurrentParamInRegister();
}

static const Register kJniArgumentRegisters[] = {
  R0, R1, R2, R3
};
ManagedRegister ArmJniCallingConvention::CurrentParamRegister() {
  CHECK_LT(itr_slots_, 4u);
  Method* method = GetMethod();
  int arg_pos = itr_args_ - NumberOfExtraArgumentsForJni(method);
  if ((itr_args_ >= 2) && method->IsParamALongOrDouble(arg_pos)) {
    CHECK_EQ(itr_slots_, 2u);
    return ArmManagedRegister::FromRegisterPair(R2_R3);
  } else {
    return
      ArmManagedRegister::FromCoreRegister(kJniArgumentRegisters[itr_slots_]);
  }
}

FrameOffset ArmJniCallingConvention::CurrentParamStackOffset() {
  CHECK_GE(itr_slots_, 4u);
  return FrameOffset(displacement_.Int32Value() - OutArgSize()
                     + ((itr_slots_ - 4) * kPointerSize));
}

size_t ArmJniCallingConvention::NumberOfOutgoingStackArgs() {
  Method* method = GetMethod();
  size_t static_args = method->IsStatic() ? 1 : 0;  // count jclass
  // regular argument parameters and this
  size_t param_args = method->NumArgs() +
                      method->NumLongOrDoubleArgs();
  // count JNIEnv* less arguments in registers
  return static_args + param_args + 1 - 4;
}

}  // namespace arm
}  // namespace art
