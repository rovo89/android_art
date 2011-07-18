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

  static Object* AllocRaw(size_t size, Class* klass) {
    byte* raw = new byte[size]();
    Object* object = reinterpret_cast<Object*>(raw);
    object->klass_ = klass;
    return object;
  }

  static Object* AllocObject(Class* klass) {
    return AllocRaw(klass->object_size_, klass);
  }

  static CharArray* AllocCharArray(Class* char_array, size_t length) {
    size_t size = sizeof(Array) + length * sizeof(uint16_t);
    return reinterpret_cast<CharArray*>(AllocRaw(size, char_array));
  }

  static String* AllocString(Class* java_lang_String) {
    return reinterpret_cast<String*>(AllocObject(java_lang_String));
  }

  static String* AllocStringFromModifiedUtf8(Class* java_lang_String, Class* char_array, const char* data) {
    String* string = AllocString(java_lang_String);
    uint32_t count = strlen(data);  // TODO
    CharArray* array = AllocCharArray(char_array, count);
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
