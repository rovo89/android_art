// Copyright 2011 Google Inc. All Rights Reserved.
// Author: cshapiro@google.com (Carl Shapiro)

#ifndef ART_SRC_HEAP_H_
#define ART_SRC_HEAP_H_

#include "src/globals.h"
#include "src/object.h"

namespace art {

class Heap {
 public:
  static Heap* Create() {
    Heap* new_heap = new Heap();
    // TODO: should return NULL if the heap could not be created.
    return new_heap;
  }

  ~Heap() {}

  static Class* AllocClass(DexFile* dex_file) {
    byte* raw = new byte[sizeof(Class)]();
    Class* klass = reinterpret_cast<Class*>(raw);
    klass->dex_file_ = dex_file;
    return klass;
  }

  static StaticField* AllocStaticField() {
    size_t size = sizeof(StaticField);
    byte* raw = new byte[size]();
    return reinterpret_cast<StaticField*>(raw);
  }

  static InstanceField* AllocInstanceField() {
    size_t size = sizeof(InstanceField);
    byte* raw = new byte[size]();
    return reinterpret_cast<InstanceField*>(raw);
  }

  static Method* AllocMethod() {
    size_t size = sizeof(Method);
    byte* raw = new byte[size]();
    return reinterpret_cast<Method*>(raw);
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

 private:
  Heap() {}

  DISALLOW_COPY_AND_ASSIGN(Heap);
};

}  // namespace art

#endif  // ART_SRC_HEAP_H_
