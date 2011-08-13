// Copyright 2011 Google Inc. All Rights Reserved.
// Author: irogers@google.com (Ian Rogers)

#include "calling_convention.h"
#include "logging.h"
#include "utils.h"

namespace art {

// Offset of Method within the frame
FrameOffset CallingConvention::MethodStackOffset() {
  return displacement_;
}

// Managed runtime calling convention

size_t ManagedRuntimeCallingConvention::FrameSize() {
  UNIMPLEMENTED(FATAL);
  return 0;
}

bool ManagedRuntimeCallingConvention::HasNext() {
  return itr_args_ < GetMethod()->NumArgs();
}

void ManagedRuntimeCallingConvention::Next() {
  CHECK(HasNext());
  if (IsCurrentUserArg() && 
      GetMethod()->IsParamALongOrDouble(itr_args_)) {
    itr_longs_and_doubles_++;
    itr_slots_++;
  }
  itr_args_++;
  itr_slots_++;
}

bool ManagedRuntimeCallingConvention::IsCurrentUserArg() {
  if (GetMethod()->IsStatic()) {
    return true;
  }
  // For a virtual method, "this" should never be NULL.
  return (itr_args_ != 0);
}

size_t ManagedRuntimeCallingConvention::CurrentParamSize() {
  return GetMethod()->ParamSize(itr_args_);
}

bool ManagedRuntimeCallingConvention::IsCurrentParamAReference() {
  return GetMethod()->IsParamAReference(itr_args_);
}

// JNI calling convention

size_t JniCallingConvention::FrameSize() {
  // Return address and Method*
  size_t frame_data_size = 2 * kPointerSize;
  // Handles plus 2 words for SHB header
  size_t handle_area_size = (HandleCount() + 2) * kPointerSize;
  return RoundUp(frame_data_size + handle_area_size + SizeOfReturnValue(), 16);
}

size_t JniCallingConvention::OutArgSize() {
  return RoundUp(NumberOfOutgoingStackArgs() * kPointerSize, 16);
}

size_t JniCallingConvention::HandleCount() {
  const Method* method = GetMethod();
  return method->NumReferenceArgs() + (method->IsStatic() ? 1 : 0);
}

FrameOffset JniCallingConvention::ReturnValueSaveLocation() {
  size_t start_of_shb = ShbLinkOffset().Int32Value() +  kPointerSize;
  size_t handle_size = kPointerSize * HandleCount();  // size excluding header
  return FrameOffset(start_of_shb + handle_size);
}

bool JniCallingConvention::HasNext() {
  if (itr_args_ <= kObjectOrClass) {
    return true;
  } else {
    unsigned int arg_pos = itr_args_ - NumberOfExtraArgumentsForJni(GetMethod());
    return arg_pos < GetMethod()->NumArgs();
  }
}

void JniCallingConvention::Next() {
  CHECK(HasNext());
  if (itr_args_ > kObjectOrClass) {
    int arg_pos = itr_args_ - NumberOfExtraArgumentsForJni(GetMethod());
    if (GetMethod()->IsParamALongOrDouble(arg_pos)) {
      itr_longs_and_doubles_++;
      itr_slots_++;
    }
  }
  itr_args_++;
  itr_slots_++;
}

bool JniCallingConvention::IsCurrentParamAReference() {
  switch (itr_args_) {
    case kJniEnv:
      return false;  // JNIEnv*
    case kObjectOrClass:
      return true;   // jobject or jclass
    default: {
      int arg_pos = itr_args_ - NumberOfExtraArgumentsForJni(GetMethod());
      return GetMethod()->IsParamAReference(arg_pos);
    }
  }
}

// Return position of handle holding reference at the current iterator position
FrameOffset JniCallingConvention::CurrentParamHandleOffset() {
  CHECK(IsCurrentParamAReference());
  CHECK_GT(ShbLinkOffset(), ShbNumRefsOffset());
  // Address of 1st handle
  int result = ShbLinkOffset().Int32Value() + kPointerSize;
  if (itr_args_ != kObjectOrClass) {
    const Method *method = GetMethod();
    int arg_pos = itr_args_ - NumberOfExtraArgumentsForJni(method);
    int previous_refs = GetMethod()->NumReferenceArgsBefore(arg_pos);
    if (method->IsStatic()) {
      previous_refs++;  // account for jclass
    }
    result += previous_refs * kPointerSize;
  }
  CHECK_GT(result, ShbLinkOffset().Int32Value());
  return FrameOffset(result);
}

size_t JniCallingConvention::CurrentParamSize() {
  if (itr_args_ <= kObjectOrClass) {
    return kPointerSize;  // JNIEnv or jobject/jclass
  } else {
    int arg_pos = itr_args_ - NumberOfExtraArgumentsForJni(GetMethod());
    return GetMethod()->ParamSize(arg_pos);
  }
}

size_t JniCallingConvention::NumberOfExtraArgumentsForJni(
    const Method* method) {
  // The first argument is the JNIEnv*.
  // Static methods have an extra argument which is the jclass.
  return (method->IsStatic() ? 2 : 1);
}

}  // namespace art
