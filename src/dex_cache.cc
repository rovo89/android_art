// Copyright 2011 Google Inc. All Rights Reserved.

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
}

}  // namespace art
