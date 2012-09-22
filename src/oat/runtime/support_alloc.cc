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

extern "C" Object* artAllocObjectFromCode(uint32_t type_idx, AbstractMethod* method,
                                          Thread* self, AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  return AllocObjectFromCode(type_idx, method, self, false);
}

extern "C" Object* artAllocObjectFromCodeWithAccessCheck(uint32_t type_idx, AbstractMethod* method,
                                                         Thread* self, AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  return AllocObjectFromCode(type_idx, method, self, true);
}

extern "C" Array* artAllocArrayFromCode(uint32_t type_idx, AbstractMethod* method, int32_t component_count,
                                        Thread* self, AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  return AllocArrayFromCode(type_idx, method, component_count, false);
}

extern "C" Array* artAllocArrayFromCodeWithAccessCheck(uint32_t type_idx, AbstractMethod* method,
                                                       int32_t component_count,
                                                       Thread* self, AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  return AllocArrayFromCode(type_idx, method, component_count, true);
}

extern "C" Array* artCheckAndAllocArrayFromCode(uint32_t type_idx, AbstractMethod* method,
                                                int32_t component_count, Thread* self,
                                                AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  return CheckAndAllocArrayFromCode(type_idx, method, component_count, self, false);
}

extern "C" Array* artCheckAndAllocArrayFromCodeWithAccessCheck(uint32_t type_idx,
                                                               AbstractMethod* method,
                                                               int32_t component_count,
                                                               Thread* self, AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  return CheckAndAllocArrayFromCode(type_idx, method, component_count, self, true);
}

}  // namespace art
