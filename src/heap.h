// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_HEAP_H_
#define ART_SRC_HEAP_H_

#include <vector>

#include "globals.h"
#include "object_bitmap.h"
#include "thread.h"

namespace art {

class Class;
class Object;
class Space;
class HeapBitmap;

class Heap {
 public:
  static const size_t kInitialSize = 16 * MB;

  static const size_t kMaximumSize = 64 * MB;

  typedef void (RootVistor)(Object* root, void* arg);

  // Create a heap with the requested sizes. optional boot image may
  // be NULL, otherwise it is an image filename created by ImageWriter.
  static bool Init(size_t starting_size, size_t maximum_size, const char* boot_image_file_name);

  static void Destroy();

  // Allocates and initializes storage for an object instance.
  static Object* AllocObject(Class* klass, size_t num_bytes);

  // Initiates an explicit garbage collection.
  static void CollectGarbage();

  // Blocks the caller until the garbage collector becomes idle.
  static void WaitForConcurrentGcToComplete();

  static Mutex* GetLock() {
    return lock_;
  }

  static const std::vector<Space*>& GetSpaces() {
    return spaces_;
  }

  static HeapBitmap* GetLiveBits() {
    return live_bitmap_;
  }

  static HeapBitmap* GetMarkBits() {
    return mark_bitmap_;
  }

 private:
  // Allocates uninitialized storage.
  static Object* Allocate(size_t num_bytes);
  static Object* Allocate(Space* space, size_t num_bytes);

  static void RecordAllocation(Space* space, const Object* object);
  static void RecordFree(Space* space, const Object* object);
  static void RecordImageAllocations(Space* space);

  static void CollectGarbageInternal();

  static void GrowForUtilization();

  static Mutex* lock_;

  static std::vector<Space*> spaces_;

  // default Space for allocations
  static Space* alloc_space_;

  static HeapBitmap* mark_bitmap_;

  static HeapBitmap* live_bitmap_;

  // The maximum size of the heap in bytes.
  static size_t maximum_size_;

  // True while the garbage collector is running.
  static bool is_gc_running_;

  // Number of bytes allocated.  Adjusted after each allocation and
  // free.
  static size_t num_bytes_allocated_;

  // Number of objects allocated.  Adjusted after each allocation and
  // free.
  static size_t num_objects_allocated_;

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
