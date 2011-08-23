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
  return FrameOffset(displacement_.Int32Value() +   // displacement
                     kPointerSize +                 // Method*
                     (itr_slots_ * kPointerSize));  // offset into in args
}

// JNI calling convention

size_t JniCallingConvention::FrameSize() {
  // Return address and Method*
  size_t frame_data_size = 2 * kPointerSize;
  // Handles plus 2 words for SHB header
  size_t handle_area_size = (HandleCount() + 2) * kPointerSize;
  // Plus return value spill area size
  return RoundUp(frame_data_size + handle_area_size + SizeOfReturnValue(), 16);
}

size_t JniCallingConvention::ReturnPcOffset() {
  // Return PC is pushed at the top of the frame by the call into the method
  return FrameSize() - kPointerSize;
}


size_t JniCallingConvention::SpillAreaSize() {
  // No spills, return address was pushed at the top of the frame
  return 0;
}

std::vector<ManagedRegister>* JniCallingConvention::ComputeRegsToSpillPreCall()
{
  // No live values in registers (everything is on the stack) so never anything
  // to preserve.
  return  new std::vector<ManagedRegister>();
}

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
