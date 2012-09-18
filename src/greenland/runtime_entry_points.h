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

#ifndef ART_SRC_GREENLAND_RUNTIME_ENTRY_POINTS_H_
#define ART_SRC_GREENLAND_RUNTIME_ENTRY_POINTS_H_

#include "macros.h"

#include <stdint.h>

#define RUNTIME_ENTRYPOINT(x) \
  (static_cast<uintptr_t>(OFFSETOF_MEMBER(Thread, runtime_entry_points_)) + \
   static_cast<uintptr_t>(OFFSETOF_MEMBER(RuntimeEntryPoints, x)))

namespace art {

class AbstractMethod;
class Object;
class Thread;

struct PACKED RuntimeEntryPoints {
  //----------------------------------------------------------------------------
  // Thread
  //----------------------------------------------------------------------------
  void (*TestSuspend)(Thread* thread);

  //----------------------------------------------------------------------------
  // Exception
  //----------------------------------------------------------------------------
  int32_t (*FindCatchBlock)(AbstractMethod* current_method, uint32_t ti_offset);
  void (*ThrowIndexOutOfBounds)(int32_t length, int32_t index);
  void (*ThrowNullPointerException)(unsigned dex_pc);

  //----------------------------------------------------------------------------
  // Alloc
  //----------------------------------------------------------------------------
  Object* (*AllocArray)(uint32_t type_idx, AbstractMethod* referrer,
                        uint32_t length, Thread* thread);

  Object* (*AllocArrayWithAccessCheck)(uint32_t type_idx, AbstractMethod* referrer,
                                       uint32_t length, Thread* thread);

  Object* (*CheckAndAllocArray)(uint32_t type_idx, AbstractMethod* referrer,
                                uint32_t length, Thread* thread);

  Object* (*CheckAndAllocArrayWithAccessCheck)(uint32_t type_idx,
                                               AbstractMethod* referrer,
                                               uint32_t length,
                                               Thread* thread);

  //----------------------------------------------------------------------------
  // DexCache
  //----------------------------------------------------------------------------
  Object* (*ResolveString)(AbstractMethod* referrer, uint32_t string_idx);

  //----------------------------------------------------------------------------
  // Field
  //----------------------------------------------------------------------------
  Object* (*GetObjectStatic)(uint32_t field_idx, AbstractMethod* referrer);

  //----------------------------------------------------------------------------
  // Cast
  //----------------------------------------------------------------------------
  void (*CheckPutArrayElement)(const Object* element, const Object* array);

  //----------------------------------------------------------------------------
  // JNI
  //----------------------------------------------------------------------------
};

// Initialize an entry point data structure.
void InitRuntimeEntryPoints(RuntimeEntryPoints* entry_points);

} // namespace art

#endif // ART_SRC_GREENLAND_RUNTIME_ENTRY_POINTS_H_
