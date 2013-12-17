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

#include "entrypoints/quick/quick_entrypoints.h"
#include "gc/heap.h"

#define GENERATE_ENTRYPOINTS(suffix) \
extern "C" void* art_quick_alloc_array##suffix(uint32_t, void*, int32_t); \
extern "C" void* art_quick_alloc_array_with_access_check##suffix(uint32_t, void*, int32_t); \
extern "C" void* art_quick_alloc_object##suffix(uint32_t type_idx, void* method); \
extern "C" void* art_quick_alloc_object_with_access_check##suffix(uint32_t type_idx, void* method); \
extern "C" void* art_quick_check_and_alloc_array##suffix(uint32_t, void*, int32_t); \
extern "C" void* art_quick_check_and_alloc_array_with_access_check##suffix(uint32_t, void*, int32_t); \
extern "C" void* art_quick_alloc_array##suffix##_instrumented(uint32_t, void*, int32_t); \
extern "C" void* art_quick_alloc_array_with_access_check##suffix##_instrumented(uint32_t, void*, int32_t); \
extern "C" void* art_quick_alloc_object##suffix##_instrumented(uint32_t type_idx, void* method); \
extern "C" void* art_quick_alloc_object_with_access_check##suffix##_instrumented(uint32_t type_idx, void* method); \
extern "C" void* art_quick_check_and_alloc_array##suffix##_instrumented(uint32_t, void*, int32_t); \
extern "C" void* art_quick_check_and_alloc_array_with_access_check##suffix##_instrumented(uint32_t, void*, int32_t); \
void SetQuickAllocEntryPoints##suffix(QuickEntryPoints* qpoints, bool instrumented) { \
  if (instrumented) { \
    qpoints->pAllocArray = art_quick_alloc_array##suffix##_instrumented; \
    qpoints->pAllocArrayWithAccessCheck = art_quick_alloc_array_with_access_check##suffix##_instrumented; \
    qpoints->pAllocObject = art_quick_alloc_object##suffix##_instrumented; \
    qpoints->pAllocObjectWithAccessCheck = art_quick_alloc_object_with_access_check##suffix##_instrumented; \
    qpoints->pCheckAndAllocArray = art_quick_check_and_alloc_array##suffix##_instrumented; \
    qpoints->pCheckAndAllocArrayWithAccessCheck = art_quick_check_and_alloc_array_with_access_check##suffix##_instrumented; \
  } else { \
    qpoints->pAllocArray = art_quick_alloc_array##suffix; \
    qpoints->pAllocArrayWithAccessCheck = art_quick_alloc_array_with_access_check##suffix; \
    qpoints->pAllocObject = art_quick_alloc_object##suffix; \
    qpoints->pAllocObjectWithAccessCheck = art_quick_alloc_object_with_access_check##suffix; \
    qpoints->pCheckAndAllocArray = art_quick_check_and_alloc_array##suffix; \
    qpoints->pCheckAndAllocArrayWithAccessCheck = art_quick_check_and_alloc_array_with_access_check##suffix; \
  } \
}

namespace art {

// Generate the entrypoint functions.
GENERATE_ENTRYPOINTS();
GENERATE_ENTRYPOINTS(_bump_pointer);
GENERATE_ENTRYPOINTS(_tlab);

static bool entry_points_instrumented = false;
static gc::AllocatorType entry_points_allocator = kMovingCollector ?
    gc::kAllocatorTypeBumpPointer : gc::kAllocatorTypeFreeList;

void SetQuickAllocEntryPointsAllocator(gc::AllocatorType allocator) {
  entry_points_allocator = allocator;
}

void SetQuickAllocEntryPointsInstrumented(bool instrumented) {
  entry_points_instrumented = instrumented;
}

void ResetQuickAllocEntryPoints(QuickEntryPoints* qpoints) {
  switch (entry_points_allocator) {
    case gc::kAllocatorTypeFreeList: {
      SetQuickAllocEntryPoints(qpoints, entry_points_instrumented);
      break;
    }
    case gc::kAllocatorTypeBumpPointer: {
      SetQuickAllocEntryPoints_bump_pointer(qpoints, entry_points_instrumented);
      break;
    }
    case gc::kAllocatorTypeTLAB: {
      SetQuickAllocEntryPoints_tlab(qpoints, entry_points_instrumented);
      break;
    }
    default: {
      LOG(FATAL) << "Unimplemented";
    }
  }
}

}  // namespace art
