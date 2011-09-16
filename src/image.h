// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_IMAGE_H_
#define ART_SRC_IMAGE_H_

#include <string.h>

#include "globals.h"
#include "object.h"

namespace art {

// header of image files written by ImageWriter, read and validated by Space.
class ImageHeader {
 public:
  ImageHeader() {}

  ImageHeader(uint32_t base_addr, uint32_t image_roots)
      : base_addr_(base_addr), image_roots_(image_roots) {
    memcpy(magic_, kImageMagic, sizeof(kImageMagic));
    memcpy(version_, kImageVersion, sizeof(kImageVersion));
  }

  bool IsValid() const {
    if (memcmp(magic_, kImageMagic, sizeof(kImageMagic) != 0)) {
      return false;
    }
    if (memcmp(version_, kImageVersion, sizeof(kImageVersion) != 0)) {
      return false;
    }
    return true;
  }

  const char* GetMagic() const {
    CHECK(IsValid());
    return reinterpret_cast<const char*>(magic_);
  }

  byte* GetBaseAddr() const {
    return reinterpret_cast<byte*>(base_addr_);
  }

  enum ImageRoot {
    kJniStubArray,
    kImageRootsMax,
  };

  Object* GetImageRoot(ImageRoot image_root) const {
    return reinterpret_cast<ObjectArray<Object>*>(image_roots_)->Get(image_root);
  }

 private:
  static const byte kImageMagic[4];
  static const byte kImageVersion[4];

  byte magic_[4];
  byte version_[4];

  // required base address for mapping the image.
  uint32_t base_addr_;

  // absolute address of an Object[] of objects needed to reinitialize from an image
  uint32_t image_roots_;

  friend class ImageWriter;
};

}  // namespace art

#endif  // ART_SRC_IMAGE_H_
