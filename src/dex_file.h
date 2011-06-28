// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_DEX_FILE_H_
#define ART_SRC_DEX_FILE_H_

#include "src/globals.h"
#include "src/macros.h"
#include "src/object.h"
#include "src/raw_dex_file.h"

namespace art {

class DexFile {
 public:
  // Opens a .dex file from the file system.  Returns NULL on failure.
  static DexFile* OpenFile(const char* filename);

  // Opens a .dex file from a base64 encoded array.  Returns NULL on
  // failure.
  static DexFile* OpenBase64(const char* base64);

  // Opens a .dex file from a RawDexFile.  Takes ownership of the
  // RawDexFile.
  static DexFile* Open(RawDexFile* raw);

  // Close and deallocate.
  ~DexFile();

  size_t NumTypes() {
    return num_classes_;
  }

  size_t NumMethods() {
    return num_methods_;
  }

  Class* LoadClass(const char* descriptor);

  Class* LoadClass(const RawDexFile::ClassDef& class_def);

 private:
  DexFile(RawDexFile* raw) : raw_(raw) {};

  void Init();

  void LoadInterfaces(const RawDexFile::ClassDef& class_def, Class *klass);

  void LoadField(Class* klass, const RawDexFile::Field& src, Field* dst);

  void LoadMethod(Class* klass, const RawDexFile::Method& src, Method* dst);

  // Table of contents for interned String objects.
  String** strings_;
  size_t num_strings_;

  // Table of contents for Class objects.
  Class** classes_;
  size_t num_classes_;

  // Table of contents for methods.
  Method** methods_;
  size_t num_methods_;

  // Table of contents for fields.
  Field** fields_;
  size_t num_fields_;

  // The size of the DEX file, in bytes.
  size_t length_;

  // The underlying dex file.
  scoped_ptr<RawDexFile> raw_;

  DISALLOW_COPY_AND_ASSIGN(DexFile);
};

}  // namespace art

#endif  // ART_SRC_DEX_FILE_H_
