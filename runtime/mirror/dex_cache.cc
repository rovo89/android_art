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

#include "dex_cache.h"

#include "art_method-inl.h"
#include "base/logging.h"
#include "class_linker.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/heap.h"
#include "globals.h"
#include "object.h"
#include "object-inl.h"
#include "object_array-inl.h"
#include "runtime.h"
#include "string.h"

namespace art {
namespace mirror {

void DexCache::Init(const DexFile* dex_file, String* location, ObjectArray<String>* strings,
                    ObjectArray<Class>* resolved_types, PointerArray* resolved_methods,
                    PointerArray* resolved_fields, size_t pointer_size) {
  CHECK(dex_file != nullptr);
  CHECK(location != nullptr);
  CHECK(strings != nullptr);
  CHECK(resolved_types != nullptr);
  CHECK(resolved_methods != nullptr);
  CHECK(resolved_fields != nullptr);

  SetDexFile(dex_file);
  SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(DexCache, location_), location);
  SetFieldObject<false>(StringsOffset(), strings);
  SetFieldObject<false>(ResolvedFieldsOffset(), resolved_fields);
  SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(DexCache, resolved_types_), resolved_types);
  SetFieldObject<false>(ResolvedMethodsOffset(), resolved_methods);

  Runtime* const runtime = Runtime::Current();
  if (runtime->HasResolutionMethod()) {
    // Initialize the resolve methods array to contain trampolines for resolution.
    Fixup(runtime->GetResolutionMethod(), pointer_size);
  }
}

void DexCache::Fixup(ArtMethod* trampoline, size_t pointer_size) {
  // Fixup the resolve methods array to contain trampoline for resolution.
  CHECK(trampoline != nullptr);
  CHECK(trampoline->IsRuntimeMethod());
  auto* resolved_methods = GetResolvedMethods();
  for (size_t i = 0, length = resolved_methods->GetLength(); i < length; i++) {
    if (resolved_methods->GetElementPtrSize<ArtMethod*>(i, pointer_size) == nullptr) {
      resolved_methods->SetElementPtrSize(i, trampoline, pointer_size);
    }
  }
}

}  // namespace mirror
}  // namespace art
