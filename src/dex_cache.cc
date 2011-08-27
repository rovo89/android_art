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
                    ObjectArray<Class>* types,
                    ObjectArray<Method>* methods,
                    ObjectArray<Field>* fields,
                    CodeAndMethods* code_and_methods) {
  CHECK(location != NULL);
  CHECK(strings != NULL);
  CHECK(types != NULL);
  CHECK(methods != NULL);
  CHECK(fields != NULL);
  CHECK(code_and_methods != NULL);
  Set(kLocation,       location);
  Set(kStrings,        strings);
  Set(kTypes,          types);
  Set(kMethods,        methods);
  Set(kFields,         fields);
  Set(kCodeAndMethods, code_and_methods);
}

}  // namespace art
