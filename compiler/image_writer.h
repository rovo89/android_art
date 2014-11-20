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

#ifndef ART_COMPILER_IMAGE_WRITER_H_
#define ART_COMPILER_IMAGE_WRITER_H_

#include <stdint.h>

#include <cstddef>
#include <memory>
#include <set>
#include <string>

#include "base/macros.h"
#include "driver/compiler_driver.h"
#include "gc/space/space.h"
#include "mem_map.h"
#include "oat_file.h"
#include "mirror/dex_cache.h"
#include "os.h"
#include "safe_map.h"
#include "gc/space/space.h"
#include "utils.h"

namespace art {

// Write a Space built during compilation for use during execution.
class ImageWriter FINAL {
 public:
  explicit ImageWriter(const CompilerDriver& compiler_driver)
      : compiler_driver_(compiler_driver), oat_file_(NULL), image_end_(0),
        image_objects_offset_begin_(0), image_begin_(NULL),
        oat_data_begin_(NULL), interpreter_to_interpreter_bridge_offset_(0),
        interpreter_to_compiled_code_bridge_offset_(0), portable_imt_conflict_trampoline_offset_(0),
        portable_resolution_trampoline_offset_(0), quick_generic_jni_trampoline_offset_(0),
        quick_imt_conflict_trampoline_offset_(0), quick_resolution_trampoline_offset_(0),
        compile_pic_(false), target_ptr_size_(0), bin_slot_sizes_(), bin_slot_count_() {}

  ~ImageWriter() {}

  bool Write(const std::string& image_filename,
             uintptr_t image_begin,
             const std::string& oat_filename,
             const std::string& oat_location,
             bool compile_pic)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  uintptr_t GetOatDataBegin() {
    return reinterpret_cast<uintptr_t>(oat_data_begin_);
  }

 private:
  bool AllocMemory();

  // Mark the objects defined in this space in the given live bitmap.
  void RecordImageAllocations() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Classify different kinds of bins that objects end up getting packed into during image writing.
  enum Bin {
    // Likely-clean:
    kBinString,                        // [String] Almost always immutable (except for obj header).
    kBinArtMethodsManagedInitialized,  // [ArtMethod] Not-native, and initialized. Unlikely to dirty
    // Unknown mix of clean/dirty:
    kBinRegular,
    // Likely-dirty:
    // All classes get their own bins since their fields often dirty
    kBinClassInitializedFinalStatics,  // Class initializers have been run, no non-final statics
    kBinClassInitialized,         // Class initializers have been run
    kBinClassVerified,            // Class verified, but initializers haven't been run
    kBinArtMethodNative,          // Art method that is actually native
    kBinArtMethodNotInitialized,  // Art method with a declaring class that wasn't initialized
    // Don't care about other art methods since they don't dirty
    // Add more bins here if we add more segregation code.
    kBinSize,
  };

  static constexpr size_t kBinBits = MinimumBitsToStore(kBinSize - 1);
  // uint32 = typeof(lockword_)
  static constexpr size_t kBinShift = BitSizeOf<uint32_t>() - kBinBits;
  // 111000.....0
  static constexpr size_t kBinMask = ((static_cast<size_t>(1) << kBinBits) - 1) << kBinShift;

  // We use the lock word to store the bin # and bin index of the object in the image.
  //
  // The struct size must be exactly sizeof(LockWord), currently 32-bits, since this will end up
  // stored in the lock word bit-for-bit when object forwarding addresses are being calculated.
  struct BinSlot {
    explicit BinSlot(uint32_t lockword);
    BinSlot(Bin bin, uint32_t index);

    // The bin an object belongs to, i.e. regular, class/verified, class/initialized, etc.
    Bin GetBin() const;
    // The offset in bytes from the beginning of the bin. Aligned to object size.
    uint32_t GetIndex() const;
    // Pack into a single uint32_t, for storing into a lock word.
    explicit operator uint32_t() const { return lockword_; }
    // Comparison operator for map support
    bool operator<(const BinSlot& other) const  { return lockword_ < other.lockword_; }

  private:
    // Must be the same size as LockWord, any larger and we would truncate the data.
    const uint32_t lockword_;
  };

  // We use the lock word to store the offset of the object in the image.
  void AssignImageOffset(mirror::Object* object, BinSlot bin_slot)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void SetImageOffset(mirror::Object* object, BinSlot bin_slot, size_t offset)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool IsImageOffsetAssigned(mirror::Object* object) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  size_t GetImageOffset(mirror::Object* object) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void AssignImageBinSlot(mirror::Object* object) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void SetImageBinSlot(mirror::Object* object, BinSlot bin_slot)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool IsImageBinSlotAssigned(mirror::Object* object) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  BinSlot GetImageBinSlot(mirror::Object* object) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static void* GetImageAddressCallback(void* writer, mirror::Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return reinterpret_cast<ImageWriter*>(writer)->GetImageAddress(obj);
  }

  mirror::Object* GetImageAddress(mirror::Object* object) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (object == NULL) {
      return NULL;
    }
    return reinterpret_cast<mirror::Object*>(image_begin_ + GetImageOffset(object));
  }

  mirror::Object* GetLocalAddress(mirror::Object* object) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    size_t offset = GetImageOffset(object);
    byte* dst = image_->Begin() + offset;
    return reinterpret_cast<mirror::Object*>(dst);
  }

  const byte* GetOatAddress(uint32_t offset) const {
#if !defined(ART_USE_PORTABLE_COMPILER)
    // With Quick, code is within the OatFile, as there are all in one
    // .o ELF object. However with Portable, the code is always in
    // different .o ELF objects.
    DCHECK_LT(offset, oat_file_->Size());
#endif
    if (offset == 0u) {
      return nullptr;
    }
    return oat_data_begin_ + offset;
  }

  // Returns true if the class was in the original requested image classes list.
  bool IsImageClass(mirror::Class* klass) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Debug aid that list of requested image classes.
  void DumpImageClasses();

  // Preinitializes some otherwise lazy fields (such as Class name) to avoid runtime image dirtying.
  void ComputeLazyFieldsForImageClasses()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static bool ComputeLazyFieldsForClassesVisitor(mirror::Class* klass, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Wire dex cache resolved strings to strings in the image to avoid runtime resolution.
  void ComputeEagerResolvedStrings() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void ComputeEagerResolvedStringsCallback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Combine string char arrays.
  void ProcessStrings() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Remove unwanted classes from various roots.
  void PruneNonImageClasses() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static bool NonImageClassesVisitor(mirror::Class* c, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Verify unwanted classes removed.
  void CheckNonImageClassesRemoved() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void CheckNonImageClassesRemovedCallback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Lays out where the image objects will be at runtime.
  void CalculateNewObjectOffsets(size_t oat_loaded_size, size_t oat_data_offset)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::ObjectArray<mirror::Object>* CreateImageRoots() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void CalculateObjectBinSlots(mirror::Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void UnbinObjectsIntoOffset(mirror::Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void WalkInstanceFields(mirror::Object* obj, mirror::Class* klass)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void WalkFieldsInOrder(mirror::Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void WalkFieldsCallback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void UnbinObjectsIntoOffsetCallback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Creates the contiguous image in memory and adjusts pointers.
  void CopyAndFixupObjects() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void CopyAndFixupObjectsCallback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void FixupMethod(mirror::ArtMethod* orig, mirror::ArtMethod* copy)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void FixupObject(mirror::Object* orig, mirror::Object* copy)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Get quick code for non-resolution/imt_conflict/abstract method.
  const byte* GetQuickCode(mirror::ArtMethod* method, bool* quick_is_interpreted)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  const byte* GetQuickEntryPoint(mirror::ArtMethod* method)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Patches references in OatFile to expect runtime addresses.
  void PatchOatCodeAndMethods(File* elf_file)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Calculate the sum total of the bin slot sizes in [0, up_to). Defaults to all bins.
  size_t GetBinSizeSum(Bin up_to = kBinSize) const;

  const CompilerDriver& compiler_driver_;

  // oat file with code for this image
  OatFile* oat_file_;

  // Memory mapped for generating the image.
  std::unique_ptr<MemMap> image_;

  // Offset to the free space in image_.
  size_t image_end_;

  // Offset from image_begin_ to where the first object is in image_.
  size_t image_objects_offset_begin_;

  // Beginning target image address for the output image.
  byte* image_begin_;

  // Saved hashes (objects are inside of the image so that they don't move).
  std::vector<std::pair<mirror::Object*, uint32_t>> saved_hashes_;

  // Saved hashes (objects are bin slots to inside of the image, not yet allocated an address).
  std::map<BinSlot, uint32_t> saved_hashes_map_;

  // Beginning target oat address for the pointers from the output image to its oat file.
  const byte* oat_data_begin_;

  // Image bitmap which lets us know where the objects inside of the image reside.
  std::unique_ptr<gc::accounting::ContinuousSpaceBitmap> image_bitmap_;

  // Offset from oat_data_begin_ to the stubs.
  uint32_t interpreter_to_interpreter_bridge_offset_;
  uint32_t interpreter_to_compiled_code_bridge_offset_;
  uint32_t jni_dlsym_lookup_offset_;
  uint32_t portable_imt_conflict_trampoline_offset_;
  uint32_t portable_resolution_trampoline_offset_;
  uint32_t portable_to_interpreter_bridge_offset_;
  uint32_t quick_generic_jni_trampoline_offset_;
  uint32_t quick_imt_conflict_trampoline_offset_;
  uint32_t quick_resolution_trampoline_offset_;
  uint32_t quick_to_interpreter_bridge_offset_;
  bool compile_pic_;

  // Size of pointers on the target architecture.
  size_t target_ptr_size_;

  // Bin slot tracking for dirty object packing
  size_t bin_slot_sizes_[kBinSize];  // Number of bytes in a bin
  size_t bin_slot_count_[kBinSize];  // Number of objects in a bin

  friend class FixupVisitor;
  friend class FixupClassVisitor;
  DISALLOW_COPY_AND_ASSIGN(ImageWriter);
};

}  // namespace art

#endif  // ART_COMPILER_IMAGE_WRITER_H_
