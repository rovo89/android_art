/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "callee_save_frame.h"
#include "runtime_support.h"

namespace art {

static uint64_t artInvokeCommon(uint32_t method_idx, Object* this_object, Method* caller_method,
                                Thread* self, Method** sp, bool access_check, InvokeType type)
    SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
  Method* method = FindMethodFast(method_idx, this_object, caller_method, access_check, type);
  if (UNLIKELY(method == NULL)) {
    FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsAndArgs);
    if (UNLIKELY(this_object == NULL && type != kDirect && type != kStatic)) {
      ThrowNullPointerExceptionForMethodAccess(caller_method, method_idx, type);
      return 0;  // failure
    }
    method = FindMethodFromCode(method_idx, this_object, caller_method, self, access_check, type);
    if (UNLIKELY(method == NULL)) {
      CHECK(self->IsExceptionPending());
      return 0;  // failure
    }
  }
  DCHECK(!self->IsExceptionPending());
  const void* code = method->GetCode();

  // When we return, the caller will branch to this address, so it had better not be 0!
  if (UNLIKELY(code == NULL)) {
      MethodHelper mh(method);
      LOG(FATAL) << "Code was NULL in method: " << PrettyMethod(method)
                 << " location: " << mh.GetDexFile().GetLocation();
  }


  uint32_t method_uint = reinterpret_cast<uint32_t>(method);
  uint64_t code_uint = reinterpret_cast<uint32_t>(code);
  uint64_t result = ((code_uint << 32) | method_uint);
  return result;
}

// See comments in runtime_support_asm.S
extern "C" uint64_t artInvokeInterfaceTrampoline(uint32_t method_idx, Object* this_object,
                                                 Method* caller_method, Thread* self,
                                                 Method** sp)
    SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
  return artInvokeCommon(method_idx, this_object, caller_method, self, sp, false, kInterface);
}

extern "C" uint64_t artInvokeInterfaceTrampolineWithAccessCheck(uint32_t method_idx,
                                                                Object* this_object,
                                                                Method* caller_method, Thread* self,
                                                                Method** sp)
    SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
  return artInvokeCommon(method_idx, this_object, caller_method, self, sp, true, kInterface);
}


extern "C" uint64_t artInvokeDirectTrampolineWithAccessCheck(uint32_t method_idx,
                                                             Object* this_object,
                                                             Method* caller_method, Thread* self,
                                                             Method** sp)
    SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
  return artInvokeCommon(method_idx, this_object, caller_method, self, sp, true, kDirect);
}

extern "C" uint64_t artInvokeStaticTrampolineWithAccessCheck(uint32_t method_idx,
                                                            Object* this_object,
                                                            Method* caller_method, Thread* self,
                                                            Method** sp)
    SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
  return artInvokeCommon(method_idx, this_object, caller_method, self, sp, true, kStatic);
}

extern "C" uint64_t artInvokeSuperTrampolineWithAccessCheck(uint32_t method_idx,
                                                            Object* this_object,
                                                            Method* caller_method, Thread* self,
                                                            Method** sp)
    SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
  return artInvokeCommon(method_idx, this_object, caller_method, self, sp, true, kSuper);
}

extern "C" uint64_t artInvokeVirtualTrampolineWithAccessCheck(uint32_t method_idx,
                                                              Object* this_object,
                                                              Method* caller_method, Thread* self,
                                                              Method** sp)
    SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
  return artInvokeCommon(method_idx, this_object, caller_method, self, sp, true, kVirtual);
}

}  // namespace art
