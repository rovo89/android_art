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
#include "base/memory_tool.h"

#include <cstddef>
#include <memory>
#include <set>
#include <stack>
#include <string>
#include <ostream>

#include "base/bit_utils.h"
#include "base/dchecked_vector.h"
#include "base/length_prefixed_array.h"
#include "base/macros.h"
#include "driver/compiler_driver.h"
#include "gc/space/space.h"
#include "image.h"
#include "lock_word.h"
#include "mem_map.h"
#include "oat_file.h"
#include "mirror/dex_cache.h"
#include "os.h"
#include "safe_map.h"
#include "utils.h"

namespace art {
namespace gc {
namespace space {
class ImageSpace;
}  // namespace space
}  // namespace gc

class ClassTable;

static constexpr int kInvalidFd = -1;

// Write a Space built during compilation for use during execution.
class ImageWriter FINAL {
 public:
  ImageWriter(const CompilerDriver& compiler_driver,
              uintptr_t image_begin,
              bool compile_pic,
              bool compile_app_image,
              ImageHeader::StorageMode image_storage_mode,
              const std::vector<const char*>& oat_filenames,
              const std::unordered_map<const DexFile*, size_t>& dex_file_oat_index_map);

  bool PrepareImageAddressSpace();

  bool IsImageAddressSpaceReady() const {
    DCHECK(!image_infos_.empty());
    for (const ImageInfo& image_info : image_infos_) {
      if (image_info.image_roots_address_ == 0u) {
        return false;
      }
    }
    return true;
  }

  template <typename T>
  T* GetImageAddress(T* object) const SHARED_REQUIRES(Locks::mutator_lock_) {
    if (object == nullptr || IsInBootImage(object)) {
      return object;
    } else {
      size_t oat_index = GetOatIndex(object);
      const ImageInfo& image_info = GetImageInfo(oat_index);
      return reinterpret_cast<T*>(image_info.image_begin_ + GetImageOffset(object));
    }
  }

  ArtMethod* GetImageMethodAddress(ArtMethod* method) SHARED_REQUIRES(Locks::mutator_lock_);

  template <typename PtrType>
  PtrType GetDexCacheArrayElementImageAddress(const DexFile* dex_file, uint32_t offset)
      const SHARED_REQUIRES(Locks::mutator_lock_) {
    auto oat_it = dex_file_oat_index_map_.find(dex_file);
    DCHECK(oat_it != dex_file_oat_index_map_.end());
    const ImageInfo& image_info = GetImageInfo(oat_it->second);
    auto it = image_info.dex_cache_array_starts_.find(dex_file);
    DCHECK(it != image_info.dex_cache_array_starts_.end());
    return reinterpret_cast<PtrType>(
        image_info.image_begin_ + image_info.bin_slot_offsets_[kBinDexCacheArray] +
            it->second + offset);
  }

  size_t GetOatFileOffset(size_t oat_index) const {
    return GetImageInfo(oat_index).oat_offset_;
  }

  const uint8_t* GetOatFileBegin(size_t oat_index) const {
    return GetImageInfo(oat_index).oat_file_begin_;
  }

  // If image_fd is not kInvalidFd, then we use that for the image file. Otherwise we open
  // the names in image_filenames.
  // If oat_fd is not kInvalidFd, then we use that for the oat file. Otherwise we open
  // the names in oat_filenames.
  bool Write(int image_fd,
             const std::vector<const char*>& image_filenames,
             const std::vector<const char*>& oat_filenames)
      REQUIRES(!Locks::mutator_lock_);

  uintptr_t GetOatDataBegin(size_t oat_index) {
    return reinterpret_cast<uintptr_t>(GetImageInfo(oat_index).oat_data_begin_);
  }

  // Get the index of the oat file containing the dex file.
  //
  // This "oat_index" is used to retrieve information about the the memory layout
  // of the oat file and its associated image file, needed for link-time patching
  // of references to the image or across oat files.
  size_t GetOatIndexForDexFile(const DexFile* dex_file) const;

  // Get the index of the oat file containing the dex file served by the dex cache.
  size_t GetOatIndexForDexCache(mirror::DexCache* dex_cache) const
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Update the oat layout for the given oat file.
  // This will make the oat_offset for the next oat file valid.
  void UpdateOatFileLayout(size_t oat_index,
                           size_t oat_loaded_size,
                           size_t oat_data_offset,
                           size_t oat_data_size);
  // Update information about the oat header, i.e. checksum and trampoline offsets.
  void UpdateOatFileHeader(size_t oat_index, const OatHeader& oat_header);

 private:
  using WorkStack = std::stack<std::pair<mirror::Object*, size_t>>;

  bool AllocMemory();

  // Mark the objects defined in this space in the given live bitmap.
  void RecordImageAllocations() SHARED_REQUIRES(Locks::mutator_lock_);

  // Classify different kinds of bins that objects end up getting packed into during image writing.
  // Ordered from dirtiest to cleanest (until ArtMethods).
  enum Bin {
    kBinMiscDirty,                // Dex caches, object locks, etc...
    kBinClassVerified,            // Class verified, but initializers haven't been run
    // Unknown mix of clean/dirty:
    kBinRegular,
    kBinClassInitialized,         // Class initializers have been run
    // All classes get their own bins since their fields often dirty
    kBinClassInitializedFinalStatics,  // Class initializers have been run, no non-final statics
    // Likely-clean:
    kBinString,                        // [String] Almost always immutable (except for obj header).
    // Add more bins here if we add more segregation code.
    // Non mirror fields must be below.
    // ArtFields should be always clean.
    kBinArtField,
    // If the class is initialized, then the ArtMethods are probably clean.
    kBinArtMethodClean,
    // ArtMethods may be dirty if the class has native methods or a declaring class that isn't
    // initialized.
    kBinArtMethodDirty,
    // IMT (clean)
    kBinImTable,
    // Conflict tables (clean).
    kBinIMTConflictTable,
    // Runtime methods (always clean, do not have a length prefix array).
    kBinRuntimeMethod,
    // Dex cache arrays have a special slot for PC-relative addressing. Since they are
    // huge, and as such their dirtiness is not important for the clean/dirty separation,
    // we arbitrarily keep them at the end of the native data.
    kBinDexCacheArray,            // Arrays belonging to dex cache.
    kBinSize,
    // Number of bins which are for mirror objects.
    kBinMirrorCount = kBinArtField,
  };
  friend std::ostream& operator<<(std::ostream& stream, const Bin& bin);

  enum NativeObjectRelocationType {
    kNativeObjectRelocationTypeArtField,
    kNativeObjectRelocationTypeArtFieldArray,
    kNativeObjectRelocationTypeArtMethodClean,
    kNativeObjectRelocationTypeArtMethodArrayClean,
    kNativeObjectRelocationTypeArtMethodDirty,
    kNativeObjectRelocationTypeArtMethodArrayDirty,
    kNativeObjectRelocationTypeRuntimeMethod,
    kNativeObjectRelocationTypeIMTable,
    kNativeObjectRelocationTypeIMTConflictTable,
    kNativeObjectRelocationTypeDexCacheArray,
  };
  friend std::ostream& operator<<(std::ostream& stream, const NativeObjectRelocationType& type);

  enum OatAddress {
    kOatAddressInterpreterToInterpreterBridge,
    kOatAddressInterpreterToCompiledCodeBridge,
    kOatAddressJNIDlsymLookup,
    kOatAddressQuickGenericJNITrampoline,
    kOatAddressQuickIMTConflictTrampoline,
    kOatAddressQuickResolutionTrampoline,
    kOatAddressQuickToInterpreterBridge,
    // Number of elements in the enum.
    kOatAddressCount,
  };
  friend std::ostream& operator<<(std::ostream& stream, const OatAddress& oat_address);

  static constexpr size_t kBinBits = MinimumBitsToStore<uint32_t>(kBinMirrorCount - 1);
  // uint32 = typeof(lockword_)
  // Subtract read barrier bits since we want these to remain 0, or else it may result in DCHECK
  // failures due to invalid read barrier bits during object field reads.
  static const size_t kBinShift = BitSizeOf<uint32_t>() - kBinBits -
      LockWord::kReadBarrierStateSize;
  // 111000.....0
  static const size_t kBinMask = ((static_cast<size_t>(1) << kBinBits) - 1) << kBinShift;

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
    uint32_t Uint32Value() const { return lockword_; }
    // Comparison operator for map support
    bool operator<(const BinSlot& other) const  { return lockword_ < other.lockword_; }

  private:
    // Must be the same size as LockWord, any larger and we would truncate the data.
    const uint32_t lockword_;
  };

  struct ImageInfo {
    ImageInfo();
    ImageInfo(ImageInfo&&) = default;

    // Create the image sections into the out sections variable, returns the size of the image
    // excluding the bitmap.
    size_t CreateImageSections(ImageSection* out_sections) const;

    std::unique_ptr<MemMap> image_;  // Memory mapped for generating the image.

    // Target begin of this image. Notes: It is not valid to write here, this is the address
    // of the target image, not necessarily where image_ is mapped. The address is only valid
    // after layouting (otherwise null).
    uint8_t* image_begin_ = nullptr;

    // Offset to the free space in image_, initially size of image header.
    size_t image_end_ = RoundUp(sizeof(ImageHeader), kObjectAlignment);
    uint32_t image_roots_address_ = 0;  // The image roots address in the image.
    size_t image_offset_ = 0;  // Offset of this image from the start of the first image.

    // Image size is the *address space* covered by this image. As the live bitmap is aligned
    // to the page size, the live bitmap will cover more address space than necessary. But live
    // bitmaps may not overlap, so an image has a "shadow," which is accounted for in the size.
    // The next image may only start at image_begin_ + image_size_ (which is guaranteed to be
    // page-aligned).
    size_t image_size_ = 0;

    // Oat data.
    // Offset of the oat file for this image from start of oat files. This is
    // valid when the previous oat file has been written.
    size_t oat_offset_ = 0;
    // Layout of the loaded ELF file containing the oat file, valid after UpdateOatFileLayout().
    const uint8_t* oat_file_begin_ = nullptr;
    size_t oat_loaded_size_ = 0;
    const uint8_t* oat_data_begin_ = nullptr;
    size_t oat_size_ = 0;  // Size of the corresponding oat data.
    // The oat header checksum, valid after UpdateOatFileHeader().
    uint32_t oat_checksum_ = 0u;

    // Image bitmap which lets us know where the objects inside of the image reside.
    std::unique_ptr<gc::accounting::ContinuousSpaceBitmap> image_bitmap_;

    // The start offsets of the dex cache arrays.
    SafeMap<const DexFile*, size_t> dex_cache_array_starts_;

    // Offset from oat_data_begin_ to the stubs.
    uint32_t oat_address_offsets_[kOatAddressCount] = {};

    // Bin slot tracking for dirty object packing.
    size_t bin_slot_sizes_[kBinSize] = {};  // Number of bytes in a bin.
    size_t bin_slot_offsets_[kBinSize] = {};  // Number of bytes in previous bins.
    size_t bin_slot_count_[kBinSize] = {};  // Number of objects in a bin.

    // Cached size of the intern table for when we allocate memory.
    size_t intern_table_bytes_ = 0;

    // Number of image class table bytes.
    size_t class_table_bytes_ = 0;

    // Intern table associated with this image for serialization.
    std::unique_ptr<InternTable> intern_table_;

    // Class table associated with this image for serialization.
    std::unique_ptr<ClassTable> class_table_;
  };

  // We use the lock word to store the offset of the object in the image.
  void AssignImageOffset(mirror::Object* object, BinSlot bin_slot)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void SetImageOffset(mirror::Object* object, size_t offset)
      SHARED_REQUIRES(Locks::mutator_lock_);
  bool IsImageOffsetAssigned(mirror::Object* object) const
      SHARED_REQUIRES(Locks::mutator_lock_);
  size_t GetImageOffset(mirror::Object* object) const SHARED_REQUIRES(Locks::mutator_lock_);
  void UpdateImageOffset(mirror::Object* obj, uintptr_t offset)
      SHARED_REQUIRES(Locks::mutator_lock_);

  void PrepareDexCacheArraySlots() SHARED_REQUIRES(Locks::mutator_lock_);
  void AssignImageBinSlot(mirror::Object* object, size_t oat_index)
      SHARED_REQUIRES(Locks::mutator_lock_);
  mirror::Object* TryAssignBinSlot(WorkStack& work_stack, mirror::Object* obj, size_t oat_index)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void SetImageBinSlot(mirror::Object* object, BinSlot bin_slot)
      SHARED_REQUIRES(Locks::mutator_lock_);
  bool IsImageBinSlotAssigned(mirror::Object* object) const
      SHARED_REQUIRES(Locks::mutator_lock_);
  BinSlot GetImageBinSlot(mirror::Object* object) const SHARED_REQUIRES(Locks::mutator_lock_);

  void AddDexCacheArrayRelocation(void* array, size_t offset, mirror::DexCache* dex_cache)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void AddMethodPointerArray(mirror::PointerArray* arr) SHARED_REQUIRES(Locks::mutator_lock_);

  static void* GetImageAddressCallback(void* writer, mirror::Object* obj)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    return reinterpret_cast<ImageWriter*>(writer)->GetImageAddress(obj);
  }

  mirror::Object* GetLocalAddress(mirror::Object* object) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    size_t offset = GetImageOffset(object);
    size_t oat_index = GetOatIndex(object);
    const ImageInfo& image_info = GetImageInfo(oat_index);
    uint8_t* dst = image_info.image_->Begin() + offset;
    return reinterpret_cast<mirror::Object*>(dst);
  }

  // Returns the address in the boot image if we are compiling the app image.
  const uint8_t* GetOatAddress(OatAddress type) const;

  const uint8_t* GetOatAddressForOffset(uint32_t offset, const ImageInfo& image_info) const {
    // With Quick, code is within the OatFile, as there are all in one
    // .o ELF object. But interpret it as signed.
    DCHECK_LE(static_cast<int32_t>(offset), static_cast<int32_t>(image_info.oat_size_));
    DCHECK(image_info.oat_data_begin_ != nullptr);
    return offset == 0u ? nullptr : image_info.oat_data_begin_ + static_cast<int32_t>(offset);
  }

  // Returns true if the class was in the original requested image classes list.
  bool KeepClass(mirror::Class* klass) SHARED_REQUIRES(Locks::mutator_lock_);

  // Debug aid that list of requested image classes.
  void DumpImageClasses();

  // Preinitializes some otherwise lazy fields (such as Class name) to avoid runtime image dirtying.
  void ComputeLazyFieldsForImageClasses()
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Remove unwanted classes from various roots.
  void PruneNonImageClasses() SHARED_REQUIRES(Locks::mutator_lock_);

  // Verify unwanted classes removed.
  void CheckNonImageClassesRemoved() SHARED_REQUIRES(Locks::mutator_lock_);
  static void CheckNonImageClassesRemovedCallback(mirror::Object* obj, void* arg)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Lays out where the image objects will be at runtime.
  void CalculateNewObjectOffsets()
      SHARED_REQUIRES(Locks::mutator_lock_);
  void ProcessWorkStack(WorkStack* work_stack)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void CreateHeader(size_t oat_index)
      SHARED_REQUIRES(Locks::mutator_lock_);
  mirror::ObjectArray<mirror::Object>* CreateImageRoots(size_t oat_index) const
      SHARED_REQUIRES(Locks::mutator_lock_);
  void UnbinObjectsIntoOffset(mirror::Object* obj)
      SHARED_REQUIRES(Locks::mutator_lock_);

  static void EnsureBinSlotAssignedCallback(mirror::Object* obj, void* arg)
      SHARED_REQUIRES(Locks::mutator_lock_);
  static void DeflateMonitorCallback(mirror::Object* obj, void* arg)
      SHARED_REQUIRES(Locks::mutator_lock_);
  static void UnbinObjectsIntoOffsetCallback(mirror::Object* obj, void* arg)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Creates the contiguous image in memory and adjusts pointers.
  void CopyAndFixupNativeData(size_t oat_index) SHARED_REQUIRES(Locks::mutator_lock_);
  void CopyAndFixupObjects() SHARED_REQUIRES(Locks::mutator_lock_);
  static void CopyAndFixupObjectsCallback(mirror::Object* obj, void* arg)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void CopyAndFixupObject(mirror::Object* obj) SHARED_REQUIRES(Locks::mutator_lock_);
  void CopyAndFixupMethod(ArtMethod* orig, ArtMethod* copy, const ImageInfo& image_info)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void CopyAndFixupImTable(ImTable* orig, ImTable* copy) SHARED_REQUIRES(Locks::mutator_lock_);
  void CopyAndFixupImtConflictTable(ImtConflictTable* orig, ImtConflictTable* copy)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void FixupClass(mirror::Class* orig, mirror::Class* copy)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void FixupObject(mirror::Object* orig, mirror::Object* copy)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void FixupDexCache(mirror::DexCache* orig_dex_cache, mirror::DexCache* copy_dex_cache)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void FixupPointerArray(mirror::Object* dst,
                         mirror::PointerArray* arr,
                         mirror::Class* klass,
                         Bin array_type)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Get quick code for non-resolution/imt_conflict/abstract method.
  const uint8_t* GetQuickCode(ArtMethod* method,
                              const ImageInfo& image_info,
                              bool* quick_is_interpreted)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Calculate the sum total of the bin slot sizes in [0, up_to). Defaults to all bins.
  size_t GetBinSizeSum(ImageInfo& image_info, Bin up_to = kBinSize) const;

  // Return true if a method is likely to be dirtied at runtime.
  bool WillMethodBeDirty(ArtMethod* m) const SHARED_REQUIRES(Locks::mutator_lock_);

  // Assign the offset for an ArtMethod.
  void AssignMethodOffset(ArtMethod* method,
                          NativeObjectRelocationType type,
                          size_t oat_index)
      SHARED_REQUIRES(Locks::mutator_lock_);

  void TryAssignImTableOffset(ImTable* imt, size_t oat_index) SHARED_REQUIRES(Locks::mutator_lock_);

  // Assign the offset for an IMT conflict table. Does nothing if the table already has a native
  // relocation.
  void TryAssignConflictTableOffset(ImtConflictTable* table, size_t oat_index)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Return true if klass is loaded by the boot class loader but not in the boot image.
  bool IsBootClassLoaderNonImageClass(mirror::Class* klass) SHARED_REQUIRES(Locks::mutator_lock_);

  // Return true if klass depends on a boot class loader non image class. We want to prune these
  // classes since we do not want any boot class loader classes in the image. This means that
  // we also cannot have any classes which refer to these boot class loader non image classes.
  // PruneAppImageClass also prunes if klass depends on a non-image class according to the compiler
  // driver.
  bool PruneAppImageClass(mirror::Class* klass)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // early_exit is true if we had a cyclic dependency anywhere down the chain.
  bool PruneAppImageClassInternal(mirror::Class* klass,
                                  bool* early_exit,
                                  std::unordered_set<mirror::Class*>* visited)
      SHARED_REQUIRES(Locks::mutator_lock_);

  bool IsMultiImage() const {
    return image_infos_.size() > 1;
  }

  static Bin BinTypeForNativeRelocationType(NativeObjectRelocationType type);

  uintptr_t NativeOffsetInImage(void* obj) SHARED_REQUIRES(Locks::mutator_lock_);

  // Location of where the object will be when the image is loaded at runtime.
  template <typename T>
  T* NativeLocationInImage(T* obj) SHARED_REQUIRES(Locks::mutator_lock_);

  // Location of where the temporary copy of the object currently is.
  template <typename T>
  T* NativeCopyLocation(T* obj, mirror::DexCache* dex_cache) SHARED_REQUIRES(Locks::mutator_lock_);

  // Return true of obj is inside of the boot image space. This may only return true if we are
  // compiling an app image.
  bool IsInBootImage(const void* obj) const;

  // Return true if ptr is within the boot oat file.
  bool IsInBootOatFile(const void* ptr) const;

  // Get the index of the oat file associated with the object.
  size_t GetOatIndex(mirror::Object* object) const SHARED_REQUIRES(Locks::mutator_lock_);

  // The oat index for shared data in multi-image and all data in single-image compilation.
  size_t GetDefaultOatIndex() const {
    return 0u;
  }

  ImageInfo& GetImageInfo(size_t oat_index) {
    return image_infos_[oat_index];
  }

  const ImageInfo& GetImageInfo(size_t oat_index) const {
    return image_infos_[oat_index];
  }

  // Find an already strong interned string in the other images or in the boot image. Used to
  // remove duplicates in the multi image and app image case.
  mirror::String* FindInternedString(mirror::String* string) SHARED_REQUIRES(Locks::mutator_lock_);

  // Return true if there already exists a native allocation for an object.
  bool NativeRelocationAssigned(void* ptr) const;

  const CompilerDriver& compiler_driver_;

  // Beginning target image address for the first image.
  uint8_t* global_image_begin_;

  // Offset from image_begin_ to where the first object is in image_.
  size_t image_objects_offset_begin_;

  // Pointer arrays that need to be updated. Since these are only some int and long arrays, we need
  // to keep track. These include vtable arrays, iftable arrays, and dex caches.
  std::unordered_map<mirror::PointerArray*, Bin> pointer_arrays_;

  // Saved hash codes. We use these to restore lockwords which were temporarily used to have
  // forwarding addresses as well as copying over hash codes.
  std::unordered_map<mirror::Object*, uint32_t> saved_hashcode_map_;

  // Oat index map for objects.
  std::unordered_map<mirror::Object*, uint32_t> oat_index_map_;

  // Boolean flags.
  const bool compile_pic_;
  const bool compile_app_image_;

  // Size of pointers on the target architecture.
  size_t target_ptr_size_;

  // Image data indexed by the oat file index.
  dchecked_vector<ImageInfo> image_infos_;

  // ArtField, ArtMethod relocating map. These are allocated as array of structs but we want to
  // have one entry per art field for convenience. ArtFields are placed right after the end of the
  // image objects (aka sum of bin_slot_sizes_). ArtMethods are placed right after the ArtFields.
  struct NativeObjectRelocation {
    size_t oat_index;
    uintptr_t offset;
    NativeObjectRelocationType type;

    bool IsArtMethodRelocation() const {
      return type == kNativeObjectRelocationTypeArtMethodClean ||
          type == kNativeObjectRelocationTypeArtMethodDirty ||
          type == kNativeObjectRelocationTypeRuntimeMethod;
    }
  };
  std::unordered_map<void*, NativeObjectRelocation> native_object_relocations_;

  // Runtime ArtMethods which aren't reachable from any Class but need to be copied into the image.
  ArtMethod* image_methods_[ImageHeader::kImageMethodsCount];

  // Counters for measurements, used for logging only.
  uint64_t dirty_methods_;
  uint64_t clean_methods_;

  // Prune class memoization table to speed up ContainsBootClassLoaderNonImageClass.
  std::unordered_map<mirror::Class*, bool> prune_class_memo_;

  // Class loaders with a class table to write out. There should only be one class loader because
  // dex2oat loads the dex files to be compiled into a single class loader. For the boot image,
  // null is a valid entry.
  std::unordered_set<mirror::ClassLoader*> class_loaders_;

  // Which mode the image is stored as, see image.h
  const ImageHeader::StorageMode image_storage_mode_;

  // The file names of oat files.
  const std::vector<const char*>& oat_filenames_;

  // Map of dex files to the indexes of oat files that they were compiled into.
  const std::unordered_map<const DexFile*, size_t>& dex_file_oat_index_map_;

  friend class ContainsBootClassLoaderNonImageClassVisitor;
  friend class FixupClassVisitor;
  friend class FixupRootVisitor;
  friend class FixupVisitor;
  class GetRootsVisitor;
  friend class NativeLocationVisitor;
  friend class NonImageClassesVisitor;
  class VisitReferencesVisitor;
  DISALLOW_COPY_AND_ASSIGN(ImageWriter);
};

}  // namespace art

#endif  // ART_COMPILER_IMAGE_WRITER_H_
