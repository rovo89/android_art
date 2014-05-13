/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "calling_convention.h"

#include "base/logging.h"
#include "jni/quick/arm/calling_convention_arm.h"
#include "jni/quick/arm64/calling_convention_arm64.h"
#include "jni/quick/mips/calling_convention_mips.h"
#include "jni/quick/x86/calling_convention_x86.h"
#include "jni/quick/x86_64/calling_convention_x86_64.h"
#include "utils.h"

namespace art {

// Managed runtime calling convention

ManagedRuntimeCallingConvention* ManagedRuntimeCallingConvention::Create(
    bool is_static, bool is_synchronized, const char* shorty, InstructionSet instruction_set) {
  switch (instruction_set) {
    case kArm:
    case kThumb2:
      return new arm::ArmManagedRuntimeCallingConvention(is_static, is_synchronized, shorty);
    case kArm64:
      return new arm64::Arm64ManagedRuntimeCallingConvention(is_static, is_synchronized, shorty);
    case kMips:
      return new mips::MipsManagedRuntimeCallingConvention(is_static, is_synchronized, shorty);
    case kX86:
      return new x86::X86ManagedRuntimeCallingConvention(is_static, is_synchronized, shorty);
    case kX86_64:
      return new x86_64::X86_64ManagedRuntimeCallingConvention(is_static, is_synchronized, shorty);
    default:
      LOG(FATAL) << "Unknown InstructionSet: " << instruction_set;
      return NULL;
  }
}

bool ManagedRuntimeCallingConvention::HasNext() {
  return itr_args_ < NumArgs();
}

void ManagedRuntimeCallingConvention::Next() {
  CHECK(HasNext());
  if (IsCurrentArgExplicit() &&  // don't query parameter type of implicit args
      IsParamALongOrDouble(itr_args_)) {
    itr_longs_and_doubles_++;
    itr_slots_++;
  }
  if (IsParamAFloatOrDouble(itr_args_)) {
    itr_float_and_doubles_++;
  }
  if (IsCurrentParamAReference()) {
    itr_refs_++;
  }
  itr_args_++;
  itr_slots_++;
}

bool ManagedRuntimeCallingConvention::IsCurrentArgExplicit() {
  // Static methods have no implicit arguments, others implicitly pass this
  return IsStatic() || (itr_args_ != 0);
}

bool ManagedRuntimeCallingConvention::IsCurrentArgPossiblyNull() {
  return IsCurrentArgExplicit();  // any user parameter may be null
}

size_t ManagedRuntimeCallingConvention::CurrentParamSize() {
  return ParamSize(itr_args_);
}

bool ManagedRuntimeCallingConvention::IsCurrentParamAReference() {
  return IsParamAReference(itr_args_);
}

bool ManagedRuntimeCallingConvention::IsCurrentParamAFloatOrDouble() {
  return IsParamAFloatOrDouble(itr_args_);
}

bool ManagedRuntimeCallingConvention::IsCurrentParamADouble() {
  return IsParamADouble(itr_args_);
}

bool ManagedRuntimeCallingConvention::IsCurrentParamALong() {
  return IsParamALong(itr_args_);
}

// JNI calling convention

JniCallingConvention* JniCallingConvention::Create(bool is_static, bool is_synchronized,
                                                   const char* shorty,
                                                   InstructionSet instruction_set) {
  switch (instruction_set) {
    case kArm:
    case kThumb2:
      return new arm::ArmJniCallingConvention(is_static, is_synchronized, shorty);
    case kArm64:
      return new arm64::Arm64JniCallingConvention(is_static, is_synchronized, shorty);
    case kMips:
      return new mips::MipsJniCallingConvention(is_static, is_synchronized, shorty);
    case kX86:
      return new x86::X86JniCallingConvention(is_static, is_synchronized, shorty);
    case kX86_64:
      return new x86_64::X86_64JniCallingConvention(is_static, is_synchronized, shorty);
    default:
      LOG(FATAL) << "Unknown InstructionSet: " << instruction_set;
      return NULL;
  }
}

size_t JniCallingConvention::ReferenceCount() const {
  return NumReferenceArgs() + (IsStatic() ? 1 : 0);
}

FrameOffset JniCallingConvention::SavedLocalReferenceCookieOffset() const {
  size_t references_size = handle_scope_pointer_size_ * ReferenceCount();  // size excluding header
  return FrameOffset(HandleerencesOffset().Int32Value() + references_size);
}

FrameOffset JniCallingConvention::ReturnValueSaveLocation() const {
  // Segment state is 4 bytes long
  return FrameOffset(SavedLocalReferenceCookieOffset().Int32Value() + 4);
}

bool JniCallingConvention::HasNext() {
  if (itr_args_ <= kObjectOrClass) {
    return true;
  } else {
    unsigned int arg_pos = itr_args_ - NumberOfExtraArgumentsForJni();
    return arg_pos < NumArgs();
  }
}

void JniCallingConvention::Next() {
  CHECK(HasNext());
  if (itr_args_ > kObjectOrClass) {
    int arg_pos = itr_args_ - NumberOfExtraArgumentsForJni();
    if (IsParamALongOrDouble(arg_pos)) {
      itr_longs_and_doubles_++;
      itr_slots_++;
    }
  }
  if (IsCurrentParamAFloatOrDouble()) {
    itr_float_and_doubles_++;
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
      int arg_pos = itr_args_ - NumberOfExtraArgumentsForJni();
      return IsParamAReference(arg_pos);
    }
  }
}

bool JniCallingConvention::IsCurrentParamJniEnv() {
  return (itr_args_ == kJniEnv);
}

bool JniCallingConvention::IsCurrentParamAFloatOrDouble() {
  switch (itr_args_) {
    case kJniEnv:
      return false;  // JNIEnv*
    case kObjectOrClass:
      return false;   // jobject or jclass
    default: {
      int arg_pos = itr_args_ - NumberOfExtraArgumentsForJni();
      return IsParamAFloatOrDouble(arg_pos);
    }
  }
}

bool JniCallingConvention::IsCurrentParamADouble() {
  switch (itr_args_) {
    case kJniEnv:
      return false;  // JNIEnv*
    case kObjectOrClass:
      return false;   // jobject or jclass
    default: {
      int arg_pos = itr_args_ - NumberOfExtraArgumentsForJni();
      return IsParamADouble(arg_pos);
    }
  }
}

bool JniCallingConvention::IsCurrentParamALong() {
  switch (itr_args_) {
    case kJniEnv:
      return false;  // JNIEnv*
    case kObjectOrClass:
      return false;   // jobject or jclass
    default: {
      int arg_pos = itr_args_ - NumberOfExtraArgumentsForJni();
      return IsParamALong(arg_pos);
    }
  }
}

// Return position of handle scope entry holding reference at the current iterator
// position
FrameOffset JniCallingConvention::CurrentParamHandleScopeEntryOffset() {
  CHECK(IsCurrentParamAReference());
  CHECK_LT(HandleScopeLinkOffset(), HandleScopeNumRefsOffset());
  int result = HandleerencesOffset().Int32Value() + itr_refs_ * handle_scope_pointer_size_;
  CHECK_GT(result, HandleScopeNumRefsOffset().Int32Value());
  return FrameOffset(result);
}

size_t JniCallingConvention::CurrentParamSize() {
  if (itr_args_ <= kObjectOrClass) {
    return frame_pointer_size_;  // JNIEnv or jobject/jclass
  } else {
    int arg_pos = itr_args_ - NumberOfExtraArgumentsForJni();
    return ParamSize(arg_pos);
  }
}

size_t JniCallingConvention::NumberOfExtraArgumentsForJni() {
  // The first argument is the JNIEnv*.
  // Static methods have an extra argument which is the jclass.
  return IsStatic() ? 2 : 1;
}

}  // namespace art
