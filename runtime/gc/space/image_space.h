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

#ifndef ART_SRC_GC_SPACE_IMAGE_SPACE_H_
#define ART_SRC_GC_SPACE_IMAGE_SPACE_H_

#include "space.h"

namespace art {
namespace gc {
namespace space {

// An image space is a space backed with a memory mapped image.
class ImageSpace : public MemMapSpace {
 public:
  bool CanAllocateInto() const {
    return false;
  }

  SpaceType GetType() const {
    return kSpaceTypeImageSpace;
  }

  // create a Space from an image file. cannot be used for future allocation or collected.
  static ImageSpace* Create(const std::string& image)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  const ImageHeader& GetImageHeader() const {
    return *reinterpret_cast<ImageHeader*>(Begin());
  }

  const std::string GetImageFilename() const {
    return GetName();
  }

  // Mark the objects defined in this space in the given live bitmap
  void RecordImageAllocations(accounting::SpaceBitmap* live_bitmap) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  accounting::SpaceBitmap* GetLiveBitmap() const {
    return live_bitmap_.get();
  }

  accounting::SpaceBitmap* GetMarkBitmap() const {
    // ImageSpaces have the same bitmap for both live and marked. This helps reduce the number of
    // special cases to test against.
    return live_bitmap_.get();
  }

  void Dump(std::ostream& os) const;

 private:
  friend class Space;

  static size_t bitmap_index_;

  UniquePtr<accounting::SpaceBitmap> live_bitmap_;

  ImageSpace(const std::string& name, MemMap* mem_map);

  DISALLOW_COPY_AND_ASSIGN(ImageSpace);
};

}  // namespace space
}  // namespace gc
}  // namespace art

#endif  // ART_SRC_GC_SPACE_IMAGE_SPACE_H_
