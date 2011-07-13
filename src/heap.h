// Copyright 2011 Google Inc. All Rights Reserved.
// Author: cshapiro@google.com (Carl Shapiro)

#ifndef ART_SRC_HEAP_H_
#define ART_SRC_HEAP_H_

#include "src/globals.h"
#include "src/object.h"

namespace art {

class Heap {
 public:
  static Class* AllocClass(DexFile* dex_file) {
    byte* raw = new byte[sizeof(Class)]();
    Class* klass = reinterpret_cast<Class*>(raw);
    klass->dex_file_ = dex_file;
    return klass;
  }

  static CharArray* AllocCharArray(size_t length) {
    size_t size = sizeof(Array) + length * sizeof(uint16_t);
    byte* raw = new byte[size]();
    return reinterpret_cast<CharArray*>(raw);
  }

  static String* AllocString() {
    size_t size = sizeof(String);
    byte* raw = new byte[size]();
    return reinterpret_cast<String*>(raw);
  }

  static String* AllocStringFromModifiedUtf8(const char* data) {
    String* string = AllocString();
    uint32_t count = strlen(data);  // TODO
    CharArray* array = AllocCharArray(count);
    string->array_ = array;
    string->count_ = count;
    return string;
  }
};

}  // namespace art

#endif  // ART_SRC_HEAP_H_
