// Copyright 2011 Google Inc. All Rights Reserved.

#include "calling_convention.h"

#include "calling_convention_arm.h"
#include "calling_convention_x86.h"
#include "logging.h"
#include "utils.h"

namespace art {

// Offset of Method within the frame
FrameOffset CallingConvention::MethodStackOffset() {
  return displacement_;
}

// Managed runtime calling convention

ManagedRuntimeCallingConvention* ManagedRuntimeCallingConvention::Create(
    Method* native_method, InstructionSet instruction_set) {
  if (instruction_set == kX86) {
    return new x86::X86ManagedRuntimeCallingConvention(native_method);
  } else {
    CHECK(instruction_set == kArm || instruction_set == kThumb2);
    return new arm::ArmManagedRuntimeCallingConvention(native_method);
  }
}

size_t ManagedRuntimeCallingConvention::FrameSize() {
  return GetMethod()->GetFrameSizeInBytes();
}

bool ManagedRuntimeCallingConvention::HasNext() {
  return itr_args_ < GetMethod()->NumArgs();
}

void ManagedRuntimeCallingConvention::Next() {
  CHECK(HasNext());
  if (IsCurrentArgExplicit() &&  // don't query parameter type of implicit args
      GetMethod()->IsParamALongOrDouble(itr_args_)) {
    itr_longs_and_doubles_++;
    itr_slots_++;
  }
  if (IsCurrentParamAReference()) {
    itr_refs_++;
  }
  itr_args_++;
  itr_slots_++;
}

bool ManagedRuntimeCallingConvention::IsCurrentArgExplicit() {
  // Static methods have no implicit arguments, others implicitly pass this
  return GetMethod()->IsStatic() || (itr_args_ != 0);
}

bool ManagedRuntimeCallingConvention::IsCurrentArgPossiblyNull() {
  return IsCurrentArgExplicit();  // any user parameter may be null
}

size_t ManagedRuntimeCallingConvention::CurrentParamSize() {
  return GetMethod()->ParamSize(itr_args_);
}

bool ManagedRuntimeCallingConvention::IsCurrentParamAReference() {
  return GetMethod()->IsParamAReference(itr_args_);
}

// JNI calling convention

JniCallingConvention* JniCallingConvention::Create(Method* native_method,
                                               InstructionSet instruction_set) {
  if (instruction_set == kX86) {
    return new x86::X86JniCallingConvention(native_method);
  } else {
    CHECK(instruction_set == kArm || instruction_set == kThumb2);
    return new arm::ArmJniCallingConvention(native_method);
  }
}

size_t JniCallingConvention::ReferenceCount() const {
  const Method* method = GetMethod();
  return method->NumReferenceArgs() + (method->IsStatic() ? 1 : 0);
}

FrameOffset JniCallingConvention::SavedLocalReferenceCookieOffset() const {
  size_t start_of_sirt = SirtLinkOffset().Int32Value() +  kPointerSize;
  size_t references_size = kPointerSize * ReferenceCount();  // size excluding header
  return FrameOffset(start_of_sirt + references_size);
}

FrameOffset JniCallingConvention::ReturnValueSaveLocation() const {
  // Segment state is 4 bytes long
  return FrameOffset(SavedLocalReferenceCookieOffset().Int32Value() + 4);
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
  if (IsCurrentParamAReference()) {
    itr_refs_++;
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

// Return position of SIRT entry holding reference at the current iterator
// position
FrameOffset JniCallingConvention::CurrentParamSirtEntryOffset() {
  CHECK(IsCurrentParamAReference());
  CHECK_GT(SirtLinkOffset(), SirtNumRefsOffset());
  // Address of 1st SIRT entry
  int result = SirtLinkOffset().Int32Value() + kPointerSize;
  result += itr_refs_ * kPointerSize;
  CHECK_GT(result, SirtLinkOffset().Int32Value());
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

size_t JniCallingConvention::NumberOfExtraArgumentsForJni(Method* method) {
  // The first argument is the JNIEnv*.
  // Static methods have an extra argument which is the jclass.
  return method->IsStatic() ? 2 : 1;
}

}  // namespace art
