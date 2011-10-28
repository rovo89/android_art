/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_SRC_SPACE_H_
#define ART_SRC_SPACE_H_

#include <string>

#include "UniquePtr.h"
#include "globals.h"
#include "image.h"
#include "macros.h"
#include "mem_map.h"
#include "mspace.h"

namespace art {

class Object;

// A space contains memory allocated for managed objects.
class Space {
 public:
  // Create a Space with the requested sizes. The requested
  // base address is not guaranteed to be granted, if it is required,
  // the caller should call GetBase on the returned space to confirm
  // the request was granted.
  static Space* Create(const std::string& name, size_t initial_size,
      size_t maximum_size, size_t growth_size, byte* requested_base);

  // create a Space from an image file. cannot be used for future allocation or collected.
  static Space* CreateFromImage(const std::string& image);

  ~Space();

  Object* AllocWithGrowth(size_t num_bytes);

  Object* AllocWithoutGrowth(size_t num_bytes);

  size_t Free(void* ptr);

  void Trim();

  size_t GetMaxAllowedFootprint();
  void SetMaxAllowedFootprint(size_t limit);

  void Grow(size_t num_bytes);

  byte* GetBase() const {
    return base_;
  }

  byte* GetLimit() const {
    return growth_limit_;
  }

  const std::string& GetName() const {
    return name_;
  }

  size_t Size() const {
    return growth_limit_ - base_;
  }

  bool IsImageSpace() const {
    return (image_header_ != NULL);
  }

  const ImageHeader& GetImageHeader() const {
    CHECK(IsImageSpace());
    return *image_header_;
  }

  const std::string& GetImageFilename() const {
    CHECK(IsImageSpace());
    return name_;
  }

  size_t AllocationSize(const Object* obj);

  void ClearGrowthLimit() {
    CHECK_GE(maximum_size_, growth_size_);
    CHECK_GE(limit_, growth_limit_);
    growth_size_ = maximum_size_;
    growth_limit_ = limit_;
  }

  void Walk(void(*callback)(const void*, size_t, const void*, size_t, void*), void* arg);

 private:
  // The boundary tag overhead.
  static const size_t kChunkOverhead = kWordSize;

  // create a Space from an existing memory mapping, taking ownership of the address space.
  static Space* Create(MemMap* mem_map);

  explicit Space(const std::string& name)
      : name_(name), mspace_(NULL), maximum_size_(0), growth_size_(0),
        image_header_(NULL), base_(0), limit_(0), growth_limit_(0) {
  }

  // Initializes the space and underlying storage.
  bool Init(size_t initial_size, size_t maximum_size, size_t growth_size, byte* requested_base);

  // Initializes the space from existing storage, taking ownership of the storage.
  void Init(MemMap* map);

  // Initializes the space from an image file
  bool InitFromImage(const std::string& image_file_name);

  void* CreateMallocSpace(void* base, size_t initial_size, size_t maximum_size);

  static void DontNeed(void* start, void* end, void* num_bytes);

  std::string name_;

  // TODO: have a Space subclass for non-image Spaces with mspace_ and maximum_size_
  void* mspace_;
  size_t maximum_size_;
  size_t growth_size_;

  // TODO: have a Space subclass for image Spaces with image_header_
  ImageHeader* image_header_;

  UniquePtr<MemMap> mem_map_;

  byte* base_;
  byte* limit_;
  byte* growth_limit_;

  DISALLOW_COPY_AND_ASSIGN(Space);
};

}  // namespace art

#endif  // ART_SRC_SPACE_H_
