// Copyright 2011 Google Inc. All Rights Reserved.

#include "dex_cache.h"
#include "heap.h"
#include "globals.h"
#include "logging.h"
#include "object.h"

namespace art {

void DexCache::Init(ObjectArray<String>* strings,
                    ObjectArray<Class>* classes,
                    ObjectArray<Method>* methods,
                    ObjectArray<Field>* fields) {
  Set(kStrings, strings);
  Set(kClasses, classes);
  Set(kMethods, methods);
  Set(kFields,  fields);
}

}  // namespace art
