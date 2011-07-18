// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_DEX_FILE_H_
#define ART_SRC_DEX_FILE_H_

#include "src/globals.h"
#include "src/macros.h"
#include "src/raw_dex_file.h"

namespace art {

class Class;
class Field;
class Method;
class String;
union JValue;

class DexFile {
 public:
  // Opens a .dex file from the file system.  Returns NULL on failure.
  static DexFile* OpenFile(const char* filename);

  // Opens a .dex file from a base64 encoded array.  Returns NULL on
  // failure.
  // TODO: move this into the DexFile unit test
  static DexFile* OpenBase64(const char* base64);

  // Opens a .dex file from a RawDexFile.  Takes ownership of the
  // RawDexFile.
  static DexFile* Open(RawDexFile* raw);

  // Close and deallocate.
  ~DexFile();

  size_t NumTypes() const {
    return num_classes_;
  }

  size_t NumMethods() const {
    return num_methods_;
  }

  bool HasClass(const StringPiece& descriptor) {
    return raw_->FindClassDef(descriptor) != NULL;
  }

  RawDexFile* GetRaw() const {
    return raw_.get();
  }

  String* GetResolvedString(uint32_t string_idx) const {
    CHECK_LT(string_idx, num_strings_);
    return strings_[string_idx];
  }

  void SetResolvedString(String* resolved, uint32_t string_idx) {
    CHECK_LT(string_idx, num_strings_);
    strings_[string_idx] = resolved;
  }

  Class* GetResolvedClass(uint32_t class_idx) const {
    CHECK_LT(class_idx, num_classes_);
    return classes_[class_idx];
  }

  void SetResolvedClass(Class* resolved, uint32_t class_idx) {
    CHECK_LT(class_idx, num_classes_);
    classes_[class_idx] = resolved;
  }

 private:
  DexFile(RawDexFile* raw) : raw_(raw) {};

  void Init();

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
