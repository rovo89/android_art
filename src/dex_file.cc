// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/dex_file.h"
#include "src/heap.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/object.h"
#include "src/raw_dex_file.h"

namespace art {

DexFile* DexFile::OpenFile(const char* filename) {
  RawDexFile* raw = RawDexFile::OpenFile(filename);
  return Open(raw);
}

DexFile* DexFile::OpenBase64(const char* base64) {
  RawDexFile* raw = RawDexFile::OpenBase64(base64);
  return Open(raw);
}

DexFile* DexFile::Open(RawDexFile* raw) {
  if (raw == NULL) {
    return NULL;
  }
  DexFile* dex_file = new DexFile(raw);
  dex_file->Init();
  return dex_file;
}

DexFile::~DexFile() {
  delete[] strings_;
  delete[] classes_;
  delete[] methods_;
  delete[] fields_;
}

void DexFile::Init() {
  num_strings_ = raw_->NumStringIds();
  strings_ = new String*[num_strings_]();

  num_classes_ = raw_->NumTypeIds();
  classes_ = new Class*[num_classes_]();

  num_methods_ = raw_->NumMethodIds();
  methods_ = new Method*[num_methods_]();

  num_fields_ = raw_->NumFieldIds();
  fields_ = new Field*[num_fields_]();
}

}  // namespace art
