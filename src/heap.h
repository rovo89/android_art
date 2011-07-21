// Copyright 2011 Google Inc. All Rights Reserved.
// Author: cshapiro@google.com (Carl Shapiro)

#ifndef ART_SRC_HEAP_H_
#define ART_SRC_HEAP_H_

#include "globals.h"
#include "object.h"
#include "object_bitmap.h"
#include "thread.h"

namespace art {

class Space;
class HeapBitmap;

class Heap {
 public:
  static const size_t kStartupSize = 1 * MB;

  static const size_t kMaximumSize = 16 * MB;

  static bool Init() {
    return Init(kStartupSize, kMaximumSize);
  }

  static bool Init(size_t staring_size, size_t maximum_size);

  static void Destroy();

  static Object* AllocObject(Class* klass) {
    return AllocObject(klass, klass->object_size_);
  }

  static Object* AllocObject(Class* klass, size_t num_bytes) {
    Object* obj = Allocate(num_bytes);
    if (obj != NULL) {
      obj->klass_ = klass;
    }
    return obj;
  }

  static Array* AllocArray(Class* array_class, size_t component_count, size_t component_size) {
    size_t size = sizeof(Array) + component_count * component_size;
    Array* array = down_cast<Array*>(AllocObject(array_class, size));
    if (array != NULL) {
      array->SetLength(component_count);
    }
    return array;
  }

  static ObjectArray* AllocObjectArray(Class* object_array_class, size_t length) {
    return down_cast<ObjectArray*>(AllocArray(object_array_class, length, sizeof(uint32_t)));
  }

  static CharArray* AllocCharArray(Class* char_array_class, size_t length) {
    return down_cast<CharArray*>(AllocArray(char_array_class, length, sizeof(uint16_t)));
  }

  static String* AllocString(Class* java_lang_String) {
    return down_cast<String*>(AllocObject(java_lang_String));
  }

  static String* AllocStringFromModifiedUtf8(Class* java_lang_String,
                                             Class* char_array,
                                             const char* data);

  // Initiates an explicit garbage collection.
  static void CollectGarbage();

  // Blocks the caller until the garbage collector becomes idle.
  static void WaitForConcurrentGcToComplete();

  static Mutex* GetLock() {
    return lock_;
  }

 private:

  static Object* Allocate(size_t num_bytes);

  static void CollectGarbageInternal();

  static void GrowForUtilization();

  static Mutex* lock_;

  static Space* space_;

  static HeapBitmap* mark_bitmap_;

  static HeapBitmap* live_bitmap_;

  static size_t startup_size_;

  static size_t maximum_size_;

  static bool is_gc_running_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Heap);
};

class HeapLock {
 public:
  HeapLock(Heap* heap) : lock_(heap->GetLock()) {
    lock_->Lock();
  }
  ~HeapLock() {
    lock_->Unlock();
  }
 private:
  Mutex* lock_;
  DISALLOW_COPY_AND_ASSIGN(HeapLock);
};

}  // namespace art

#endif  // ART_SRC_HEAP_H_
