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

void art_check_put_array_element_from_code(const Object* element,
                                           const Object* array) {
  if (element == NULL) {
    return;
  }
  DCHECK(array != NULL);
  Class* array_class = array->GetClass();
  DCHECK(array_class != NULL);
  Class* component_type = array_class->GetComponentType();
  Class* element_class = element->GetClass();
  if (UNLIKELY(!component_type->IsAssignableFrom(element_class))) {
    Thread* thread = art_get_current_thread();
    thread->ThrowNewExceptionF("Ljava/lang/ArrayStoreException;",
                               "%s cannot be stored in an array of type %s",
                               PrettyDescriptor(element_class).c_str(),
                               PrettyDescriptor(array_class).c_str());
  }
  return;
}

} // anonymous namespace

namespace art {
namespace greenland {

void InitCastRuntimes(RuntimeEntryPoints* entry_points) {
  entry_points->CheckPutArrayElement = art_check_put_array_element_from_code;
}

} // namespace greenland
} // namespace art
