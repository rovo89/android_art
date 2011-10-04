// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_IMAGE_WRITER_H_
#define ART_SRC_IMAGE_WRITER_H_

#include <stdint.h>

#include <cstddef>

#include "UniquePtr.h"
#include "dex_cache.h"
#include "mem_map.h"
#include "oat_file.h"
#include "object.h"
#include "os.h"
#include "space.h"

namespace art {

// Write a Space built during compilation for use during execution.
class ImageWriter {

 public:
  ImageWriter() : source_space_(NULL), image_top_(0), image_base_(NULL) {};
  bool Write(const char* image_filename, uintptr_t image_base,
             const std::string& oat_filename, const std::string& strip_location_prefix);
  ~ImageWriter() {};

 private:

  bool Init();

  // we use the lock word to store the offset of the object in the image
  void AssignImageOffset(Object* object) {
    DCHECK(object != NULL);
    DCHECK(object->monitor_ == 0);  // should be no lock
    SetImageOffset(object, image_top_);
    image_top_ += RoundUp(object->SizeOf(), 8);  // 64-bit alignment
    DCHECK_LT(image_top_, image_->GetLength());
  }
  static void SetImageOffset(Object* object, size_t offset) {
    DCHECK(object != NULL);
    // should be no lock (but it might be forward referenced interned string)
    DCHECK(object->monitor_ == 0 || object->IsString());
    DCHECK_NE(0U, offset);
    object->monitor_ = offset;
  }
  static size_t IsImageOffsetAssigned(const Object* object) {
    DCHECK(object != NULL);
    size_t offset = object->monitor_;
    return offset != 0U;
  }
  static size_t GetImageOffset(const Object* object) {
    DCHECK(object != NULL);
    size_t offset = object->monitor_;
    DCHECK_NE(0U, offset);
    return offset;
  }
  static void ResetImageOffset(Object* object) {
    DCHECK(object != NULL);
    DCHECK(object->monitor_ != 0);  // should be an offset
    object->monitor_ = 0;
  }

  bool InSourceSpace(const Object* object) const {
    DCHECK(source_space_ != NULL);
    const byte* o = reinterpret_cast<const byte*>(object);
    return (o >= source_space_->GetBase() && o < source_space_->GetLimit());
  }
  Object* GetImageAddress(const Object* object) const {
    if (object == NULL) {
      return NULL;
    }
    // if object outside the relocating source_space_, assume unchanged
    if (!InSourceSpace(object)) {
      return const_cast<Object*>(object);
    }
    return reinterpret_cast<Object*>(image_base_ + GetImageOffset(object));
  }
  Object* GetLocalAddress(const Object* object) const {
    size_t offset = GetImageOffset(object);
    byte* dst = image_->GetAddress() + offset;
    return reinterpret_cast<Object*>(dst);
  }

  void CalculateNewObjectOffsets();
  ObjectArray<Object>* CreateImageRoots() const;
  static void CalculateNewObjectOffsetsCallback(Object* obj, void* arg);

  void CopyAndFixupObjects();
  static void CopyAndFixupObjectsCallback(Object* obj, void* arg);
  void FixupClass(const Class* orig, Class* copy);
  void FixupMethod(const Method* orig, Method* copy);
  void FixupObject(const Object* orig, Object* copy);
  void FixupObjectArray(const ObjectArray<Object>* orig, ObjectArray<Object>* copy);
  void FixupInstanceFields(const Object* orig, Object* copy);
  void FixupStaticFields(const Class* orig, Class* copy);
  void FixupFields(const Object* orig, Object* copy, uint32_t ref_offsets, bool is_static);

  void FixupDexCaches();
  void FixupDexCache(const DexCache* orig, DexCache* copy);

  // oat file with code for this image
  UniquePtr<OatFile> oat_file_;

  // Space we are writing objects from
  const Space* source_space_;

  // memory mapped for generating the image
  UniquePtr<MemMap> image_;

  // Offset to the free space in image_
  size_t image_top_;

  // Target image base address for the output image
  byte* image_base_;

  // Target oat base address for the pointers from the output image to its oat file
  const byte* oat_base_;

  // DexCaches seen while scanning for fixing up CodeAndDirectMethods
  typedef std::tr1::unordered_set<DexCache*, ObjectIdentityHash> Set;
  Set dex_caches_;
};

}  // namespace art

#endif  // ART_SRC_IMAGE_WRITER_H_
