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

#ifndef ART_SRC_IMAGE_WRITER_H_
#define ART_SRC_IMAGE_WRITER_H_

#include <stdint.h>

#include <cstddef>
#include <set>
#include <string>

#include "compiler.h"
#include "dex_cache.h"
#include "mem_map.h"
#include "oat_file.h"
#include "object.h"
#include "os.h"
#include "safe_map.h"
#include "space.h"
#include "UniquePtr.h"

namespace art {

// Write a Space built during compilation for use during execution.
class ImageWriter {
 public:
  explicit ImageWriter(const std::set<std::string>* image_classes)
      : oat_file_(NULL), image_end_(0), image_begin_(NULL), image_classes_(image_classes),
        oat_begin_(NULL) {}

  ~ImageWriter() {}

  bool Write(const std::string& image_filename,
             uintptr_t image_begin,
             const std::string& oat_filename,
             const std::string& oat_location,
             const Compiler& compiler)
      LOCKS_EXCLUDED(GlobalSynchronization::mutator_lock_);

 private:
  bool AllocMemory();

  // we use the lock word to store the offset of the object in the image
  void AssignImageOffset(Object* object)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    DCHECK(object != NULL);
    SetImageOffset(object, image_end_);
    image_end_ += RoundUp(object->SizeOf(), 8);  // 64-bit alignment
    DCHECK_LT(image_end_, image_->Size());
  }

  void SetImageOffset(Object* object, size_t offset) {
    DCHECK(object != NULL);
    DCHECK_NE(offset, 0U);
    DCHECK(!IsImageOffsetAssigned(object));
    offsets_.Put(object, offset);
  }

  size_t IsImageOffsetAssigned(const Object* object) const {
    DCHECK(object != NULL);
    return offsets_.find(object) != offsets_.end();
  }

  size_t GetImageOffset(const Object* object) const {
    DCHECK(object != NULL);
    DCHECK(IsImageOffsetAssigned(object));
    return offsets_.find(object)->second;
  }

  bool InSourceSpace(const Object* object) const;

  Object* GetImageAddress(const Object* object) const {
    if (object == NULL) {
      return NULL;
    }
    // if object outside the relocating source_space_, assume unchanged
    if (!InSourceSpace(object)) {
      return const_cast<Object*>(object);
    }
    return reinterpret_cast<Object*>(image_begin_ + GetImageOffset(object));
  }

  Object* GetLocalAddress(const Object* object) const {
    size_t offset = GetImageOffset(object);
    byte* dst = image_->Begin() + offset;
    return reinterpret_cast<Object*>(dst);
  }

  const byte* GetOatAddress(uint32_t offset) const {
    DCHECK_LT(offset, oat_file_->Size());
    if (offset == 0) {
      return NULL;
    }
    return oat_begin_ + offset;
  }

  bool IsImageClass(const Class* klass) SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);
  void DumpImageClasses();

  void ComputeLazyFieldsForImageClasses()
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);
  static bool ComputeLazyFieldsForClassesVisitor(Class* klass, void* arg)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

  // Wire dex cache resolved strings to strings in the image to avoid runtime resolution
  void ComputeEagerResolvedStrings();
  static void ComputeEagerResolvedStringsCallback(Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

  void PruneNonImageClasses() SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);
  static bool NonImageClassesVisitor(Class* c, void* arg)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

  void CheckNonImageClassesRemoved();
  static void CheckNonImageClassesRemovedCallback(Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

  void CalculateNewObjectOffsets() SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);
  ObjectArray<Object>* CreateImageRoots() const
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);
  static void CalculateNewObjectOffsetsCallback(Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

  void CopyAndFixupObjects();
  static void CopyAndFixupObjectsCallback(Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);
  void FixupClass(const Class* orig, Class* copy)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);
  void FixupMethod(const Method* orig, Method* copy)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);
  void FixupObject(const Object* orig, Object* copy)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);
  void FixupObjectArray(const ObjectArray<Object>* orig, ObjectArray<Object>* copy)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);
  void FixupInstanceFields(const Object* orig, Object* copy)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);
  void FixupStaticFields(const Class* orig, Class* copy)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);
  void FixupFields(const Object* orig, Object* copy, uint32_t ref_offsets, bool is_static)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

  void PatchOatCodeAndMethods(const Compiler& compiler)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);
  void SetPatchLocation(const Compiler::PatchInformation* patch, uint32_t value)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

  SafeMap<const Object*, size_t> offsets_;

  // oat file with code for this image
  OatFile* oat_file_;

  // memory mapped for generating the image
  UniquePtr<MemMap> image_;

  // Offset to the free space in image_
  size_t image_end_;

  // Beginning target image address for the output image
  byte* image_begin_;

  // Set of classes to be include in the image, or NULL for all.
  const std::set<std::string>* image_classes_;

  // Beginning target oat address for the pointers from the output image to its oat file
  const byte* oat_begin_;

  // DexCaches seen while scanning for fixing up CodeAndDirectMethods
  typedef std::set<DexCache*> Set;
  Set dex_caches_;
};

}  // namespace art

#endif  // ART_SRC_IMAGE_WRITER_H_
