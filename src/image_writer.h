// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_IMAGE_WRITER_H_
#define ART_SRC_IMAGE_WRITER_H_

#include <cstddef>
#include <stdint.h>

#include "mem_map.h"
#include "object.h"
#include "os.h"
#include "scoped_ptr.h"

namespace art {

// Write a Space built during compilation for use during execution.
class ImageWriter {

 public:
  ImageWriter() : image_top_(0), image_base_(NULL) {};
  bool Write(Space* space, const char* filename, byte* image_base);
  ~ImageWriter() {};

 private:

  bool Init(Space* space);

  // we use the lock word to store the offset of the object in the image
  void SetImageOffset(Object* object, size_t offset) {
    DCHECK(object != NULL);
    DCHECK(object->monitor_ == NULL);  // should be no lock
    DCHECK_NE(0U, offset);
    object->monitor_ = reinterpret_cast<Monitor*>(offset);
  }
  size_t GetImageOffset(const Object* object) {
    DCHECK(object != NULL);
    size_t offset = reinterpret_cast<size_t>(object->monitor_);
    DCHECK_NE(0U, offset);
    return offset;
  }
  Object* GetImageAddress(const Object* object) {
    if (object == NULL) {
      return NULL;
    }
    return reinterpret_cast<Object*>(image_base_ + GetImageOffset(object));
  }

  void CalculateNewObjectOffsets();
  static void CalculateNewObjectOffsetsCallback(Object *obj, void *arg);

  void CopyAndFixupObjects();
  static void CopyAndFixupObjectsCallback(Object *obj, void *arg);
  void FixupObject(Object* orig, Object* copy);
  void FixupObjectArray(ObjectArray<Object>* orig, ObjectArray<Object>* copy);
  void FixupInstanceFields(Object* orig, Object* copy);

  // memory mapped for generating the image
  scoped_ptr<MemMap> image_;

  // Offset to the free space in image_
  size_t image_top_;

  // Target base address for the output image
  byte* image_base_;
};

}  // namespace art

#endif  // ART_SRC_IMAGE_WRITER_H_
