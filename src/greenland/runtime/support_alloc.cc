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

#include "greenland/runtime_entry_points.h"

#include "runtime_utils.h"
#include "runtime_support.h"

using namespace art;
using namespace art::greenland;

namespace {

Object* art_alloc_array_from_code(uint32_t type_idx,
                                  AbstractMethod* referrer,
                                  uint32_t length,
                                  Thread* thread) {
  return AllocArrayFromCode(type_idx, referrer, length, thread, false);
}

Object* art_alloc_array_from_code_with_access_check(uint32_t type_idx,
                                                    AbstractMethod* referrer,
                                                    uint32_t length,
                                                    Thread* thread) {
  return AllocArrayFromCode(type_idx, referrer, length, thread, true);
}

Object* art_check_and_alloc_array_from_code(uint32_t type_idx,
                                            AbstractMethod* referrer,
                                            uint32_t length,
                                            Thread* thread) {
  return CheckAndAllocArrayFromCode(type_idx, referrer, length, thread, false);
}

Object* art_check_and_alloc_array_from_code_with_access_check(uint32_t type_idx,
                                                              AbstractMethod* referrer,
                                                              uint32_t length,
                                                              Thread* thread) {
  return CheckAndAllocArrayFromCode(type_idx, referrer, length, thread, true);
}

} // anonymous namespace

namespace art {
namespace greenland {

void InitAllocRuntimes(RuntimeEntryPoints* entry_points) {
  entry_points->AllocArray = art_alloc_array_from_code;
  entry_points->AllocArrayWithAccessCheck = art_alloc_array_from_code_with_access_check;
  entry_points->CheckAndAllocArray = art_check_and_alloc_array_from_code;
  entry_points->CheckAndAllocArrayWithAccessCheck = art_check_and_alloc_array_from_code_with_access_check;
  return;
}

} // namespace greenland
} // namespace art
