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

#include "class_linker.h"
#include "dex_cache.h"
#include "heap.h"
#include "globals.h"
#include "logging.h"
#include "object.h"

namespace art {

void DexCache::Init(String* location,
                    ObjectArray<String>* strings,
                    ObjectArray<Class>* resolved_types,
                    ObjectArray<Method>* resolved_methods,
                    ObjectArray<Field>* resolved_fields,
                    ObjectArray<StaticStorageBase>* initialized_static_storage) {
  CHECK(location != NULL);
  CHECK(strings != NULL);
  CHECK(resolved_types != NULL);
  CHECK(resolved_methods != NULL);
  CHECK(resolved_fields != NULL);
  CHECK(initialized_static_storage != NULL);
  Set(kLocation,                 location);
  Set(kStrings,                  strings);
  Set(kResolvedTypes,            resolved_types);
  Set(kResolvedMethods,          resolved_methods);
  Set(kResolvedFields,           resolved_fields);
  Set(kInitializedStaticStorage, initialized_static_storage);

  Runtime* runtime = Runtime::Current();
  if (runtime->HasResolutionMethod()) {
    // Initialize the resolve methods array to contain trampolines for resolution.
    Method* trampoline = runtime->GetResolutionMethod();
    size_t length = resolved_methods->GetLength();
    for (size_t i = 0; i < length; i++) {
      resolved_methods->SetWithoutChecks(i, trampoline);
    }
  }
}

void DexCache::Fixup(Method* trampoline) {
  // Fixup the resolve methods array to contain trampoline for resolution.
  CHECK(trampoline != NULL);
  ObjectArray<Method>* resolved_methods = down_cast<ObjectArray<Method>*>(Get(kResolvedMethods));
  size_t length = resolved_methods->GetLength();
  for (size_t i = 0; i < length; i++) {
    if (resolved_methods->GetWithoutChecks(i) == NULL) {
      resolved_methods->SetWithoutChecks(i, trampoline);
    }
  }
}

}  // namespace art
