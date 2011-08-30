// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_SPACE_H_
#define ART_SRC_SPACE_H_

#include "UniquePtr.h"
#include "globals.h"
#include "image.h"
#include "macros.h"
#include "mem_map.h"

namespace art {

class Object;

// A space contains memory allocated for managed objects.
class Space {
 public:
  // create a Space with the requested sizes requesting a specific base address.
  static Space* Create(size_t initial_size, size_t maximum_size, byte* requested_base);

  // create a Space from an image file. cannot be used for future allocation or collected.
  static Space* Create(const char* image);

  ~Space();

  Object* AllocWithGrowth(size_t num_bytes);

  Object* AllocWithoutGrowth(size_t num_bytes);

  size_t Free(void* ptr);

  void Trim();

  size_t MaxAllowedFootprint();

  void Grow(size_t num_bytes);

  byte* GetBase() const {
    return base_;
  }

  byte* GetLimit() const {
    return limit_;
  }

  size_t Size() const {
    return limit_ - base_;
  }

  const ImageHeader& GetImageHeader() const {
    CHECK(image_header_ != NULL);
    return *image_header_;
  }

  size_t AllocationSize(const Object* obj);

  bool IsCondemned() const {
    return true;  // TODO
  }

 private:
  // The boundary tag overhead.
  static const size_t kChunkOverhead = kWordSize;

  // create a Space from an existing memory mapping, taking ownership of the address space.
  static Space* Create(MemMap* mem_map);

  Space() : mspace_(NULL), maximum_size_(0), image_header_(NULL), base_(0), limit_(0) {}

  // Initializes the space and underlying storage.
  bool Init(size_t initial_size, size_t maximum_size, byte* requested_base);

  // Initializes the space from existing storage, taking ownership of the storage.
  void Init(MemMap* map);

  // Initializes the space from an image file
  bool Init(const char* image_file_name);

  void* CreateMallocSpace(void* base, size_t initial_size, size_t maximum_size);

  static void DontNeed(void* start, void* end, void* num_bytes);

  // TODO: have a Space subclass for non-image Spaces with mspace_ and maximum_size_
  void* mspace_;
  size_t maximum_size_;

  // TODO: have a Space subclass for image Spaces with image_header_
  ImageHeader* image_header_;

  UniquePtr<MemMap> mem_map_;

  byte* base_;

  byte* limit_;

  // bool is_condemned_;  // TODO: with IsCondemned

  DISALLOW_COPY_AND_ASSIGN(Space);
};

}  // namespace art

#endif  // ART_SRC_SPACE_H_
