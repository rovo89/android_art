// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_SPACE_H_
#define ART_SRC_SPACE_H_

#include "globals.h"
#include "macros.h"

namespace art {

class Object;

// A space contains memory allocated for managed objects.
class Space {
 public:
  static Space* Create(size_t startup_size, size_t maximum_size);

  ~Space();

  Object* AllocWithGrowth(size_t num_bytes);

  Object* AllocWithoutGrowth(size_t num_bytes);

  size_t Free(void* ptr);

  void Trim();

  size_t MaxAllowedFootprint();

  void Grow(size_t num_bytes);

  byte* GetBase() {
    return base_;
  }

  byte* GetLimit() {
    return limit_;
  }

  size_t Size() {
    return limit_ - base_;
  }

  size_t AllocationSize(const Object* obj);

  bool IsCondemned() {
    return true;  // TODO
  }

 private:
  // The boundary tag overhead.
  static const size_t kChunkOverhead = kWordSize;

  Space(size_t startup_size, size_t maximum_size) :
      mspace_(NULL),
      base_(NULL),
      startup_size_(startup_size),
      maximum_size_(maximum_size) {
  }

  // Initializes the space and underlying storage.
  bool Init();

  void* CreateMallocSpace(void* base, size_t startup_size,
                          size_t maximum_size);

  static void DontNeed(void* start, void* end, void* num_bytes);

  void* mspace_;

  byte* base_;

  byte* limit_;

  size_t startup_size_;

  size_t maximum_size_;

  bool is_condemned_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Space);
};

}  // namespace art

#endif  // ART_SRC_SPACE_H_
