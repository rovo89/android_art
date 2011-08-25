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
                    ObjectArray<Field>* fields) {
  Set(kLocation, location);
  Set(kStrings,  strings);
  Set(kTypes,    types);
  Set(kMethods,  methods);
  Set(kFields,   fields);
}

}  // namespace art
