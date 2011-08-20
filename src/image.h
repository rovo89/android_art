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

  ImageHeader(uint32_t base_addr, uint32_t intern_addr)
      : base_addr_(base_addr), intern_addr_(intern_addr) {
    memcpy(magic_, kImageMagic, sizeof(kImageMagic));
    memcpy(version_, kImageVersion, sizeof(kImageVersion));
  }

  bool IsValid() {
    if (memcmp(magic_, kImageMagic, sizeof(kImageMagic) != 0)) {
      return false;
    }
    if (memcmp(version_, kImageVersion, sizeof(kImageVersion) != 0)) {
      return false;
    }
    return true;
  }

  byte* GetBaseAddr() const {
    return reinterpret_cast<byte*>(base_addr_);
  }

  ObjectArray<Object>* GetInternedArray() const {
    return reinterpret_cast<ObjectArray<Object>*>(intern_addr_);
  }

 private:
  static const byte kImageMagic[4];
  static const byte kImageVersion[4];

  byte magic_[4];
  byte version_[4];

  // required base address for mapping the image.
  uint32_t base_addr_;

  // absolute address of an Object[] of Strings to InternTable::Register
  uint32_t intern_addr_;
};

}  // namespace art

#endif  // ART_SRC_IMAGE_H_
