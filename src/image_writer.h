// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_IMAGE_WRITER_H_
#define ART_SRC_IMAGE_WRITER_H_

#include <stdint.h>

#include <cstddef>

#include "UniquePtr.h"
#include "dex_cache.h"
#include "mem_map.h"
#include "object.h"
#include "os.h"
#include "space.h"

namespace art {

// Write a Space built during compilation for use during execution.
class ImageWriter {

 public:
  ImageWriter() : source_space_(NULL), image_top_(0), image_base_(NULL) {};
  bool Write(const char* filename, uintptr_t image_base);
  ~ImageWriter() {};

 private:

  bool Init();

  // we use the lock word to store the offset of the object in the image
  static void SetImageOffset(Object* object, size_t offset) {
    DCHECK(object != NULL);
    DCHECK(object->GetMonitor() == NULL);  // should be no lock
    DCHECK_NE(0U, offset);
    object->SetMonitor(reinterpret_cast<Monitor*>(offset));
  }
  static size_t GetImageOffset(const Object* object) {
    DCHECK(object != NULL);
    size_t offset = reinterpret_cast<size_t>(object->GetMonitor());
    DCHECK_NE(0U, offset);
    return offset;
  }
  static void ResetImageOffset(Object* object) {
    DCHECK(object != NULL);
    DCHECK(object->GetMonitor() != NULL);  // should be an offset
    object->SetMonitor(reinterpret_cast<Monitor*>(0));
  }

  bool InSourceSpace(const Object* object) {
    DCHECK(source_space_ != NULL);
    const byte* o = reinterpret_cast<const byte*>(object);
    return (o >= source_space_->GetBase() && o < source_space_->GetLimit());
  }
  Object* GetImageAddress(const Object* object) {
    if (object == NULL) {
      return NULL;
    }
    // if object outside the relocating source_space_, assume unchanged
    if (!InSourceSpace(object)) {
      return const_cast<Object*>(object);
    }
    return reinterpret_cast<Object*>(image_base_ + GetImageOffset(object));
  }
  Object* GetLocalAddress(const Object* object) {
    size_t offset = GetImageOffset(object);
    byte* dst = image_->GetAddress() + offset;
    return reinterpret_cast<Object*>(dst);
  }

  void CalculateNewObjectOffsets();
  static void CalculateNewObjectOffsetsCallback(Object* obj, void *arg);

  void CopyAndFixupObjects();
  static void CopyAndFixupObjectsCallback(Object* obj, void *arg);
  void FixupClass(const Class* orig, Class* copy);
  void FixupMethod(const Method* orig, Method* copy);
  void FixupField(const Field* orig, Field* copy);
  void FixupObject(const Object* orig, Object* copy);
  void FixupObjectArray(const ObjectArray<Object>* orig, ObjectArray<Object>* copy);
  void FixupInstanceFields(const Object* orig, Object* copy);
  void FixupStaticFields(const Class* orig, Class* copy);
  void FixupFields(const Object* orig, Object* copy, uint32_t ref_offsets, bool is_static);

  void FixupDexCaches();
  void FixupDexCache(const DexCache* orig, DexCache* copy);

  // Space we are writing objects from
  const Space* source_space_;

  // memory mapped for generating the image
  UniquePtr<MemMap> image_;

  // Offset to the free space in image_
  size_t image_top_;

  // Target base address for the output image
  byte* image_base_;

  // DexCaches seen while scanning for fixing up CodeAndDirectMethods
  typedef std::tr1::unordered_set<DexCache*, DexCacheHash> Set;
  Set dex_caches_;
};

}  // namespace art

#endif  // ART_SRC_IMAGE_WRITER_H_
