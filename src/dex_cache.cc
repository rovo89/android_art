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

void CodeAndDirectMethods::SetResolvedDirectMethodTraceEntry(uint32_t method_idx, const void* pcode) {
  CHECK(pcode != NULL);
  int32_t code = reinterpret_cast<int32_t>(pcode);
  Set(CodeIndex(method_idx), code);
}

void CodeAndDirectMethods::SetResolvedDirectMethod(uint32_t method_idx, Method* method) {
  CHECK(method != NULL);
  CHECK(method->IsDirect()) << PrettyMethod(method);
  CHECK(method->GetCode() != NULL) << PrettyMethod(method);
  Set(CodeIndex(method_idx),   reinterpret_cast<int32_t>(method->GetCode()));
  Set(MethodIndex(method_idx), reinterpret_cast<int32_t>(method));
}

void DexCache::Init(String* location,
                    ObjectArray<String>* strings,
                    ObjectArray<Class>* resolved_types,
                    ObjectArray<Method>* resolved_methods,
                    ObjectArray<Field>* resolved_fields,
                    CodeAndDirectMethods* code_and_direct_methods,
                    ObjectArray<StaticStorageBase>* initialized_static_storage) {
  CHECK(location != NULL);
  CHECK(strings != NULL);
  CHECK(resolved_types != NULL);
  CHECK(resolved_methods != NULL);
  CHECK(resolved_fields != NULL);
  CHECK(code_and_direct_methods != NULL);
  CHECK(initialized_static_storage != NULL);
  Set(kLocation,                 location);
  Set(kStrings,                  strings);
  Set(kResolvedTypes,            resolved_types);
  Set(kResolvedMethods,          resolved_methods);
  Set(kResolvedFields,           resolved_fields);
  Set(kCodeAndDirectMethods,     code_and_direct_methods);
  Set(kInitializedStaticStorage, initialized_static_storage);

  Runtime* runtime = Runtime::Current();
  if (runtime->IsStarted()) {
    Runtime::TrampolineType unknown_method_resolution_type = Runtime::GetTrampolineType(NULL);
    ByteArray* res_trampoline = runtime->GetResolutionStubArray(unknown_method_resolution_type);
    for (size_t i = 0; i < NumResolvedMethods(); i++) {
      code_and_direct_methods->SetResolvedDirectMethodTrampoline(i, res_trampoline);
    }
  }
}

}  // namespace art
