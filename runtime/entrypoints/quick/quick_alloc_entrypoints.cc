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
#include "entrypoints/entrypoint_utils.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"

namespace art {

#define GENERATE_ENTRYPOINTS_FOR_ALLOCATOR_INST(suffix, suffix2, instrumented_bool, allocator_type) \
extern "C" mirror::Object* artAllocObjectFromCode ##suffix##suffix2( \
    uint32_t type_idx, mirror::ArtMethod* method, Thread* self, mirror::ArtMethod** sp) \
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) { \
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly); \
  return AllocObjectFromCode<false, instrumented_bool>(type_idx, method, self, allocator_type); \
} \
extern "C" mirror::Object* artAllocObjectFromCodeWithAccessCheck##suffix##suffix2( \
    uint32_t type_idx, mirror::ArtMethod* method, Thread* self, mirror::ArtMethod** sp) \
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) { \
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly); \
  return AllocObjectFromCode<true, instrumented_bool>(type_idx, method, self, allocator_type); \
} \
extern "C" mirror::Array* artAllocArrayFromCode##suffix##suffix2( \
    uint32_t type_idx, mirror::ArtMethod* method, int32_t component_count, Thread* self, \
    mirror::ArtMethod** sp) \
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) { \
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly); \
  return AllocArrayFromCode<false, instrumented_bool>(type_idx, method, component_count, self, \
                                                      allocator_type); \
} \
extern "C" mirror::Array* artAllocArrayFromCodeWithAccessCheck##suffix##suffix2( \
    uint32_t type_idx, mirror::ArtMethod* method, int32_t component_count, Thread* self, \
    mirror::ArtMethod** sp) \
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) { \
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly); \
  return AllocArrayFromCode<true, instrumented_bool>(type_idx, method, component_count, self, \
                                                     allocator_type); \
} \
extern "C" mirror::Array* artCheckAndAllocArrayFromCode##suffix##suffix2( \
    uint32_t type_idx, mirror::ArtMethod* method, int32_t component_count, Thread* self, \
    mirror::ArtMethod** sp) \
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) { \
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly); \
  if (!instrumented_bool) { \
    return CheckAndAllocArrayFromCode(type_idx, method, component_count, self, false, allocator_type); \
  } else { \
    return CheckAndAllocArrayFromCodeInstrumented(type_idx, method, component_count, self, false, allocator_type); \
  } \
} \
extern "C" mirror::Array* artCheckAndAllocArrayFromCodeWithAccessCheck##suffix##suffix2( \
    uint32_t type_idx, mirror::ArtMethod* method, int32_t component_count, Thread* self, \
    mirror::ArtMethod** sp) \
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) { \
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly); \
  if (!instrumented_bool) { \
    return CheckAndAllocArrayFromCode(type_idx, method, component_count, self, true, allocator_type); \
  } else { \
    return CheckAndAllocArrayFromCodeInstrumented(type_idx, method, component_count, self, true, allocator_type); \
  } \
}

#define GENERATE_ENTRYPOINTS_FOR_ALLOCATOR(suffix, allocator_type) \
    GENERATE_ENTRYPOINTS_FOR_ALLOCATOR_INST(suffix, Instrumented, true, allocator_type) \
    GENERATE_ENTRYPOINTS_FOR_ALLOCATOR_INST(suffix, , false, allocator_type)

GENERATE_ENTRYPOINTS_FOR_ALLOCATOR(, gc::kAllocatorTypeFreeList)
GENERATE_ENTRYPOINTS_FOR_ALLOCATOR(BumpPointer, gc::kAllocatorTypeBumpPointer)

}  // namespace art
