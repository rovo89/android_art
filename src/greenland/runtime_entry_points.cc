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

#include "runtime_entry_points.h"

namespace art {

// Forward Declarations
namespace greenland {

void InitThreadRuntimes(RuntimeEntryPoints* entry_points);
void InitExceptionRuntimes(RuntimeEntryPoints* entry_points);
void InitAllocRuntimes(RuntimeEntryPoints* entry_points);
void InitDexCacheRuntimes(RuntimeEntryPoints* entry_points);
void InitFieldRuntimes(RuntimeEntryPoints* entry_points);
void InitCastRuntimes(RuntimeEntryPoints* entry_points);

} // namespace greenland

void InitRuntimeEntryPoints(RuntimeEntryPoints* entry_points) {
  // Defined in runtime/support_thread.cc
  greenland::InitThreadRuntimes(entry_points);
  // Defined in runtime/support_exception.cc
  greenland::InitExceptionRuntimes(entry_points);
  // Defined in runtime/support_alloc.cc
  greenland::InitAllocRuntimes(entry_points);
  // Defined in runtime/support_dexcache.cc
  greenland::InitDexCacheRuntimes(entry_points);
  // Defined in runtime/support_field.cc
  greenland::InitFieldRuntimes(entry_points);
  // Defined in runtime/support_case.cc
  greenland::InitCastRuntimes(entry_points);
  return;
}

} // namespace art
