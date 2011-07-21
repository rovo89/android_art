// Copyright 2011 Google Inc. All Rights Reserved.

#include "dex_file.h"
#include "heap.h"
#include "globals.h"
#include "logging.h"
#include "object.h"
#include "raw_dex_file.h"

namespace art {

void DexFile::Init(ObjectArray* strings, ObjectArray* classes, ObjectArray* methods, ObjectArray* fields) {
  Set(kStrings, strings);
  Set(kClasses, classes);
  Set(kMethods, methods);
  Set(kFields,  fields);
}

}  // namespace art
