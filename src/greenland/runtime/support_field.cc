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

Object* art_get_obj_static_from_code(uint32_t field_idx, Method* referrer) {
  Field* field = FindFieldFast(field_idx, referrer, false, false, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    return field->GetObj(NULL);
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread(),
                            true, false, false, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    return field->GetObj(NULL);
  }
  return 0;
}

} // anonymous namespace

namespace art {
namespace greenland {

void InitFieldRuntimes(RuntimeEntryPoints* entry_points) {
  entry_points->GetObjectStatic = art_get_obj_static_from_code;
}

} // namespace greenland
} // namespace art
