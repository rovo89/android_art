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

#include "image_writer.h"

#include <sys/stat.h>
#include <lz4.h>
#include <lz4hc.h>

#include <memory>
#include <numeric>
#include <unordered_set>
#include <vector>

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/logging.h"
#include "base/unix_file/fd_file.h"
#include "class_linker-inl.h"
#include "compiled_method.h"
#include "dex_file-inl.h"
#include "driver/compiler_driver.h"
#include "elf_file.h"
#include "elf_utils.h"
#include "elf_writer.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/heap_bitmap.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/heap.h"
#include "gc/space/large_object_space.h"
#include "gc/space/space-inl.h"
#include "globals.h"
#include "image.h"
#include "intern_table.h"
#include "linear_alloc.h"
#include "lock_word.h"
#include "mirror/abstract_method.h"
#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/method.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-inl.h"
#include "oat.h"
#include "oat_file.h"
#include "oat_file_manager.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "handle_scope-inl.h"
#include "utils/dex_cache_arrays_layout-inl.h"

using ::art::mirror::Class;
using ::art::mirror::DexCache;
using ::art::mirror::Object;
using ::art::mirror::ObjectArray;
using ::art::mirror::String;

namespace art {

// Separate objects into multiple bins to optimize dirty memory use.
static constexpr bool kBinObjects = true;

// Return true if an object is already in an image space.
bool ImageWriter::IsInBootImage(const void* obj) const {
  gc::Heap* const heap = Runtime::Current()->GetHeap();
  if (!compile_app_image_) {
    DCHECK(heap->GetBootImageSpaces().empty());
    return false;
  }
  for (gc::space::ImageSpace* boot_image_space : heap->GetBootImageSpaces()) {
    const uint8_t* image_begin = boot_image_space->Begin();
    // Real image end including ArtMethods and ArtField sections.
    const uint8_t* image_end = image_begin + boot_image_space->GetImageHeader().GetImageSize();
    if (image_begin <= obj && obj < image_end) {
      return true;
    }
  }
  return false;
}

bool ImageWriter::IsInBootOatFile(const void* ptr) const {
  gc::Heap* const heap = Runtime::Current()->GetHeap();
  if (!compile_app_image_) {
    DCHECK(heap->GetBootImageSpaces().empty());
    return false;
  }
  for (gc::space::ImageSpace* boot_image_space : heap->GetBootImageSpaces()) {
    const ImageHeader& image_header = boot_image_space->GetImageHeader();
    if (image_header.GetOatFileBegin() <= ptr && ptr < image_header.GetOatFileEnd()) {
      return true;
    }
  }
  return false;
}

static void CheckNoDexObjectsCallback(Object* obj, void* arg ATTRIBUTE_UNUSED)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  Class* klass = obj->GetClass();
  CHECK_NE(PrettyClass(klass), "com.android.dex.Dex");
}

static void CheckNoDexObjects() {
  ScopedObjectAccess soa(Thread::Current());
  Runtime::Current()->GetHeap()->VisitObjects(CheckNoDexObjectsCallback, nullptr);
}

bool ImageWriter::PrepareImageAddressSpace() {
  target_ptr_size_ = InstructionSetPointerSize(compiler_driver_.GetInstructionSet());
  gc::Heap* const heap = Runtime::Current()->GetHeap();
  {
    ScopedObjectAccess soa(Thread::Current());
    PruneNonImageClasses();  // Remove junk
    if (!compile_app_image_) {
      // Avoid for app image since this may increase RAM and image size.
      ComputeLazyFieldsForImageClasses();  // Add useful information
    }
  }
  heap->CollectGarbage(false);  // Remove garbage.

  // Dex caches must not have their dex fields set in the image. These are memory buffers of mapped
  // dex files.
  //
  // We may open them in the unstarted-runtime code for class metadata. Their fields should all be
  // reset in PruneNonImageClasses and the objects reclaimed in the GC. Make sure that's actually
  // true.
  if (kIsDebugBuild) {
    CheckNoDexObjects();
  }

  if (kIsDebugBuild) {
    ScopedObjectAccess soa(Thread::Current());
    CheckNonImageClassesRemoved();
  }

  {
    ScopedObjectAccess soa(Thread::Current());
    CalculateNewObjectOffsets();
  }

  // This needs to happen after CalculateNewObjectOffsets since it relies on intern_table_bytes_ and
  // bin size sums being calculated.
  if (!AllocMemory()) {
    return false;
  }

  return true;
}

bool ImageWriter::Write(int image_fd,
                        const std::vector<const char*>& image_filenames,
                        const std::vector<const char*>& oat_filenames) {
  // If image_fd or oat_fd are not kInvalidFd then we may have empty strings in image_filenames or
  // oat_filenames.
  CHECK(!image_filenames.empty());
  if (image_fd != kInvalidFd) {
    CHECK_EQ(image_filenames.size(), 1u);
  }
  CHECK(!oat_filenames.empty());
  CHECK_EQ(image_filenames.size(), oat_filenames.size());

  {
    ScopedObjectAccess soa(Thread::Current());
    for (size_t i = 0; i < oat_filenames.size(); ++i) {
      CreateHeader(i);
      CopyAndFixupNativeData(i);
    }
  }

  {
    // TODO: heap validation can't handle these fix up passes.
    ScopedObjectAccess soa(Thread::Current());
    Runtime::Current()->GetHeap()->DisableObjectValidation();
    CopyAndFixupObjects();
  }

  for (size_t i = 0; i < image_filenames.size(); ++i) {
    const char* image_filename = image_filenames[i];
    ImageInfo& image_info = GetImageInfo(i);
    std::unique_ptr<File> image_file;
    if (image_fd != kInvalidFd) {
      if (strlen(image_filename) == 0u) {
        image_file.reset(new File(image_fd, unix_file::kCheckSafeUsage));
        // Empty the file in case it already exists.
        if (image_file != nullptr) {
          TEMP_FAILURE_RETRY(image_file->SetLength(0));
          TEMP_FAILURE_RETRY(image_file->Flush());
        }
      } else {
        LOG(ERROR) << "image fd " << image_fd << " name " << image_filename;
      }
    } else {
      image_file.reset(OS::CreateEmptyFile(image_filename));
    }

    if (image_file == nullptr) {
      LOG(ERROR) << "Failed to open image file " << image_filename;
      return false;
    }

    if (!compile_app_image_ && fchmod(image_file->Fd(), 0644) != 0) {
      PLOG(ERROR) << "Failed to make image file world readable: " << image_filename;
      image_file->Erase();
      return EXIT_FAILURE;
    }

    std::unique_ptr<char[]> compressed_data;
    // Image data size excludes the bitmap and the header.
    ImageHeader* const image_header = reinterpret_cast<ImageHeader*>(image_info.image_->Begin());
    const size_t image_data_size = image_header->GetImageSize() - sizeof(ImageHeader);
    char* image_data = reinterpret_cast<char*>(image_info.image_->Begin()) + sizeof(ImageHeader);
    size_t data_size;
    const char* image_data_to_write;
    const uint64_t compress_start_time = NanoTime();

    CHECK_EQ(image_header->storage_mode_, image_storage_mode_);
    switch (image_storage_mode_) {
      case ImageHeader::kStorageModeLZ4HC:  // Fall-through.
      case ImageHeader::kStorageModeLZ4: {
        const size_t compressed_max_size = LZ4_compressBound(image_data_size);
        compressed_data.reset(new char[compressed_max_size]);
        data_size = LZ4_compress(
            reinterpret_cast<char*>(image_info.image_->Begin()) + sizeof(ImageHeader),
            &compressed_data[0],
            image_data_size);

        break;
      }
      /*
       * Disabled due to image_test64 flakyness. Both use same decompression. b/27560444
      case ImageHeader::kStorageModeLZ4HC: {
        // Bound is same as non HC.
        const size_t compressed_max_size = LZ4_compressBound(image_data_size);
        compressed_data.reset(new char[compressed_max_size]);
        data_size = LZ4_compressHC(
            reinterpret_cast<char*>(image_info.image_->Begin()) + sizeof(ImageHeader),
            &compressed_data[0],
            image_data_size);
        break;
      }
      */
      case ImageHeader::kStorageModeUncompressed: {
        data_size = image_data_size;
        image_data_to_write = image_data;
        break;
      }
      default: {
        LOG(FATAL) << "Unsupported";
        UNREACHABLE();
      }
    }

    if (compressed_data != nullptr) {
      image_data_to_write = &compressed_data[0];
      VLOG(compiler) << "Compressed from " << image_data_size << " to " << data_size << " in "
                     << PrettyDuration(NanoTime() - compress_start_time);
      if (kIsDebugBuild) {
        std::unique_ptr<uint8_t[]> temp(new uint8_t[image_data_size]);
        const size_t decompressed_size = LZ4_decompress_safe(
            reinterpret_cast<char*>(&compressed_data[0]),
            reinterpret_cast<char*>(&temp[0]),
            data_size,
            image_data_size);
        CHECK_EQ(decompressed_size, image_data_size);
        CHECK_EQ(memcmp(image_data, &temp[0], image_data_size), 0) << image_storage_mode_;
      }
    }

    // Write out the image + fields + methods.
    const bool is_compressed = compressed_data != nullptr;
    if (!image_file->PwriteFully(image_data_to_write, data_size, sizeof(ImageHeader))) {
      PLOG(ERROR) << "Failed to write image file data " << image_filename;
      image_file->Erase();
      return false;
    }

    // Write out the image bitmap at the page aligned start of the image end, also uncompressed for
    // convenience.
    const ImageSection& bitmap_section = image_header->GetImageSection(
        ImageHeader::kSectionImageBitmap);
    // Align up since data size may be unaligned if the image is compressed.
    size_t bitmap_position_in_file = RoundUp(sizeof(ImageHeader) + data_size, kPageSize);
    if (!is_compressed) {
      CHECK_EQ(bitmap_position_in_file, bitmap_section.Offset());
    }
    if (!image_file->PwriteFully(reinterpret_cast<char*>(image_info.image_bitmap_->Begin()),
                                 bitmap_section.Size(),
                                 bitmap_position_in_file)) {
      PLOG(ERROR) << "Failed to write image file " << image_filename;
      image_file->Erase();
      return false;
    }

    int err = image_file->Flush();
    if (err < 0) {
      PLOG(ERROR) << "Failed to flush image file " << image_filename << " with result " << err;
      image_file->Erase();
      return false;
    }

    // Write header last in case the compiler gets killed in the middle of image writing.
    // We do not want to have a corrupted image with a valid header.
    // The header is uncompressed since it contains whether the image is compressed or not.
    image_header->data_size_ = data_size;
    if (!image_file->PwriteFully(reinterpret_cast<char*>(image_info.image_->Begin()),
                                 sizeof(ImageHeader),
                                 0)) {
      PLOG(ERROR) << "Failed to write image file header " << image_filename;
      image_file->Erase();
      return false;
    }

    CHECK_EQ(bitmap_position_in_file + bitmap_section.Size(),
             static_cast<size_t>(image_file->GetLength()));
    if (image_file->FlushCloseOrErase() != 0) {
      PLOG(ERROR) << "Failed to flush and close image file " << image_filename;
      return false;
    }
  }
  return true;
}

void ImageWriter::SetImageOffset(mirror::Object* object, size_t offset) {
  DCHECK(object != nullptr);
  DCHECK_NE(offset, 0U);

  // The object is already deflated from when we set the bin slot. Just overwrite the lock word.
  object->SetLockWord(LockWord::FromForwardingAddress(offset), false);
  DCHECK_EQ(object->GetLockWord(false).ReadBarrierState(), 0u);
  DCHECK(IsImageOffsetAssigned(object));
}

void ImageWriter::UpdateImageOffset(mirror::Object* obj, uintptr_t offset) {
  DCHECK(IsImageOffsetAssigned(obj)) << obj << " " << offset;
  obj->SetLockWord(LockWord::FromForwardingAddress(offset), false);
  DCHECK_EQ(obj->GetLockWord(false).ReadBarrierState(), 0u);
}

void ImageWriter::AssignImageOffset(mirror::Object* object, ImageWriter::BinSlot bin_slot) {
  DCHECK(object != nullptr);
  DCHECK_NE(image_objects_offset_begin_, 0u);

  size_t oat_index = GetOatIndex(object);
  ImageInfo& image_info = GetImageInfo(oat_index);
  size_t bin_slot_offset = image_info.bin_slot_offsets_[bin_slot.GetBin()];
  size_t new_offset = bin_slot_offset + bin_slot.GetIndex();
  DCHECK_ALIGNED(new_offset, kObjectAlignment);

  SetImageOffset(object, new_offset);
  DCHECK_LT(new_offset, image_info.image_end_);
}

bool ImageWriter::IsImageOffsetAssigned(mirror::Object* object) const {
  // Will also return true if the bin slot was assigned since we are reusing the lock word.
  DCHECK(object != nullptr);
  return object->GetLockWord(false).GetState() == LockWord::kForwardingAddress;
}

size_t ImageWriter::GetImageOffset(mirror::Object* object) const {
  DCHECK(object != nullptr);
  DCHECK(IsImageOffsetAssigned(object));
  LockWord lock_word = object->GetLockWord(false);
  size_t offset = lock_word.ForwardingAddress();
  size_t oat_index = GetOatIndex(object);
  const ImageInfo& image_info = GetImageInfo(oat_index);
  DCHECK_LT(offset, image_info.image_end_);
  return offset;
}

void ImageWriter::SetImageBinSlot(mirror::Object* object, BinSlot bin_slot) {
  DCHECK(object != nullptr);
  DCHECK(!IsImageOffsetAssigned(object));
  DCHECK(!IsImageBinSlotAssigned(object));

  // Before we stomp over the lock word, save the hash code for later.
  LockWord lw(object->GetLockWord(false));
  switch (lw.GetState()) {
    case LockWord::kFatLocked: {
      LOG(FATAL) << "Fat locked object " << object << " found during object copy";
      break;
    }
    case LockWord::kThinLocked: {
      LOG(FATAL) << "Thin locked object " << object << " found during object copy";
      break;
    }
    case LockWord::kUnlocked:
      // No hash, don't need to save it.
      break;
    case LockWord::kHashCode:
      DCHECK(saved_hashcode_map_.find(object) == saved_hashcode_map_.end());
      saved_hashcode_map_.emplace(object, lw.GetHashCode());
      break;
    default:
      LOG(FATAL) << "Unreachable.";
      UNREACHABLE();
  }
  object->SetLockWord(LockWord::FromForwardingAddress(bin_slot.Uint32Value()), false);
  DCHECK_EQ(object->GetLockWord(false).ReadBarrierState(), 0u);
  DCHECK(IsImageBinSlotAssigned(object));
}

void ImageWriter::PrepareDexCacheArraySlots() {
  // Prepare dex cache array starts based on the ordering specified in the CompilerDriver.
  // Set the slot size early to avoid DCHECK() failures in IsImageBinSlotAssigned()
  // when AssignImageBinSlot() assigns their indexes out or order.
  for (const DexFile* dex_file : compiler_driver_.GetDexFilesForOatFile()) {
    auto it = dex_file_oat_index_map_.find(dex_file);
    DCHECK(it != dex_file_oat_index_map_.end()) << dex_file->GetLocation();
    ImageInfo& image_info = GetImageInfo(it->second);
    image_info.dex_cache_array_starts_.Put(dex_file, image_info.bin_slot_sizes_[kBinDexCacheArray]);
    DexCacheArraysLayout layout(target_ptr_size_, dex_file);
    image_info.bin_slot_sizes_[kBinDexCacheArray] += layout.Size();
  }

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Thread* const self = Thread::Current();
  ReaderMutexLock mu(self, *class_linker->DexLock());
  for (const ClassLinker::DexCacheData& data : class_linker->GetDexCachesData()) {
    mirror::DexCache* dex_cache =
        down_cast<mirror::DexCache*>(self->DecodeJObject(data.weak_root));
    if (dex_cache == nullptr || IsInBootImage(dex_cache)) {
      continue;
    }
    const DexFile* dex_file = dex_cache->GetDexFile();
    CHECK(dex_file_oat_index_map_.find(dex_file) != dex_file_oat_index_map_.end())
        << "Dex cache should have been pruned " << dex_file->GetLocation()
        << "; possibly in class path";
    DexCacheArraysLayout layout(target_ptr_size_, dex_file);
    DCHECK(layout.Valid());
    size_t oat_index = GetOatIndexForDexCache(dex_cache);
    ImageInfo& image_info = GetImageInfo(oat_index);
    uint32_t start = image_info.dex_cache_array_starts_.Get(dex_file);
    DCHECK_EQ(dex_file->NumTypeIds() != 0u, dex_cache->GetResolvedTypes() != nullptr);
    AddDexCacheArrayRelocation(dex_cache->GetResolvedTypes(),
                               start + layout.TypesOffset(),
                               dex_cache);
    DCHECK_EQ(dex_file->NumMethodIds() != 0u, dex_cache->GetResolvedMethods() != nullptr);
    AddDexCacheArrayRelocation(dex_cache->GetResolvedMethods(),
                               start + layout.MethodsOffset(),
                               dex_cache);
    DCHECK_EQ(dex_file->NumFieldIds() != 0u, dex_cache->GetResolvedFields() != nullptr);
    AddDexCacheArrayRelocation(dex_cache->GetResolvedFields(),
                               start + layout.FieldsOffset(),
                               dex_cache);
    DCHECK_EQ(dex_file->NumStringIds() != 0u, dex_cache->GetStrings() != nullptr);
    AddDexCacheArrayRelocation(dex_cache->GetStrings(), start + layout.StringsOffset(), dex_cache);
  }
}

void ImageWriter::AddDexCacheArrayRelocation(void* array, size_t offset, DexCache* dex_cache) {
  if (array != nullptr) {
    DCHECK(!IsInBootImage(array));
    size_t oat_index = GetOatIndexForDexCache(dex_cache);
    native_object_relocations_.emplace(array,
        NativeObjectRelocation { oat_index, offset, kNativeObjectRelocationTypeDexCacheArray });
  }
}

void ImageWriter::AddMethodPointerArray(mirror::PointerArray* arr) {
  DCHECK(arr != nullptr);
  if (kIsDebugBuild) {
    for (size_t i = 0, len = arr->GetLength(); i < len; i++) {
      ArtMethod* method = arr->GetElementPtrSize<ArtMethod*>(i, target_ptr_size_);
      if (method != nullptr && !method->IsRuntimeMethod()) {
        mirror::Class* klass = method->GetDeclaringClass();
        CHECK(klass == nullptr || KeepClass(klass))
            << PrettyClass(klass) << " should be a kept class";
      }
    }
  }
  // kBinArtMethodClean picked arbitrarily, just required to differentiate between ArtFields and
  // ArtMethods.
  pointer_arrays_.emplace(arr, kBinArtMethodClean);
}

void ImageWriter::AssignImageBinSlot(mirror::Object* object, size_t oat_index) {
  DCHECK(object != nullptr);
  size_t object_size = object->SizeOf();

  // The magic happens here. We segregate objects into different bins based
  // on how likely they are to get dirty at runtime.
  //
  // Likely-to-dirty objects get packed together into the same bin so that
  // at runtime their page dirtiness ratio (how many dirty objects a page has) is
  // maximized.
  //
  // This means more pages will stay either clean or shared dirty (with zygote) and
  // the app will use less of its own (private) memory.
  Bin bin = kBinRegular;
  size_t current_offset = 0u;

  if (kBinObjects) {
    //
    // Changing the bin of an object is purely a memory-use tuning.
    // It has no change on runtime correctness.
    //
    // Memory analysis has determined that the following types of objects get dirtied
    // the most:
    //
    // * Dex cache arrays are stored in a special bin. The arrays for each dex cache have
    //   a fixed layout which helps improve generated code (using PC-relative addressing),
    //   so we pre-calculate their offsets separately in PrepareDexCacheArraySlots().
    //   Since these arrays are huge, most pages do not overlap other objects and it's not
    //   really important where they are for the clean/dirty separation. Due to their
    //   special PC-relative addressing, we arbitrarily keep them at the end.
    // * Class'es which are verified [their clinit runs only at runtime]
    //   - classes in general [because their static fields get overwritten]
    //   - initialized classes with all-final statics are unlikely to be ever dirty,
    //     so bin them separately
    // * Art Methods that are:
    //   - native [their native entry point is not looked up until runtime]
    //   - have declaring classes that aren't initialized
    //            [their interpreter/quick entry points are trampolines until the class
    //             becomes initialized]
    //
    // We also assume the following objects get dirtied either never or extremely rarely:
    //  * Strings (they are immutable)
    //  * Art methods that aren't native and have initialized declared classes
    //
    // We assume that "regular" bin objects are highly unlikely to become dirtied,
    // so packing them together will not result in a noticeably tighter dirty-to-clean ratio.
    //
    if (object->IsClass()) {
      bin = kBinClassVerified;
      mirror::Class* klass = object->AsClass();

      // Add non-embedded vtable to the pointer array table if there is one.
      auto* vtable = klass->GetVTable();
      if (vtable != nullptr) {
        AddMethodPointerArray(vtable);
      }
      auto* iftable = klass->GetIfTable();
      if (iftable != nullptr) {
        for (int32_t i = 0; i < klass->GetIfTableCount(); ++i) {
          if (iftable->GetMethodArrayCount(i) > 0) {
            AddMethodPointerArray(iftable->GetMethodArray(i));
          }
        }
      }

      if (klass->GetStatus() == Class::kStatusInitialized) {
        bin = kBinClassInitialized;

        // If the class's static fields are all final, put it into a separate bin
        // since it's very likely it will stay clean.
        uint32_t num_static_fields = klass->NumStaticFields();
        if (num_static_fields == 0) {
          bin = kBinClassInitializedFinalStatics;
        } else {
          // Maybe all the statics are final?
          bool all_final = true;
          for (uint32_t i = 0; i < num_static_fields; ++i) {
            ArtField* field = klass->GetStaticField(i);
            if (!field->IsFinal()) {
              all_final = false;
              break;
            }
          }

          if (all_final) {
            bin = kBinClassInitializedFinalStatics;
          }
        }
      }
    } else if (object->GetClass<kVerifyNone>()->IsStringClass()) {
      bin = kBinString;  // Strings are almost always immutable (except for object header).
    } else if (object->GetClass<kVerifyNone>() ==
        Runtime::Current()->GetClassLinker()->GetClassRoot(ClassLinker::kJavaLangObject)) {
      // Instance of java lang object, probably a lock object. This means it will be dirty when we
      // synchronize on it.
      bin = kBinMiscDirty;
    } else if (object->IsDexCache()) {
      // Dex file field becomes dirty when the image is loaded.
      bin = kBinMiscDirty;
    }
    // else bin = kBinRegular
  }

  // Assign the oat index too.
  DCHECK(oat_index_map_.find(object) == oat_index_map_.end());
  oat_index_map_.emplace(object, oat_index);

  ImageInfo& image_info = GetImageInfo(oat_index);

  size_t offset_delta = RoundUp(object_size, kObjectAlignment);  // 64-bit alignment
  current_offset = image_info.bin_slot_sizes_[bin];  // How many bytes the current bin is at (aligned).
  // Move the current bin size up to accommodate the object we just assigned a bin slot.
  image_info.bin_slot_sizes_[bin] += offset_delta;

  BinSlot new_bin_slot(bin, current_offset);
  SetImageBinSlot(object, new_bin_slot);

  ++image_info.bin_slot_count_[bin];

  // Grow the image closer to the end by the object we just assigned.
  image_info.image_end_ += offset_delta;
}

bool ImageWriter::WillMethodBeDirty(ArtMethod* m) const {
  if (m->IsNative()) {
    return true;
  }
  mirror::Class* declaring_class = m->GetDeclaringClass();
  // Initialized is highly unlikely to dirty since there's no entry points to mutate.
  return declaring_class == nullptr || declaring_class->GetStatus() != Class::kStatusInitialized;
}

bool ImageWriter::IsImageBinSlotAssigned(mirror::Object* object) const {
  DCHECK(object != nullptr);

  // We always stash the bin slot into a lockword, in the 'forwarding address' state.
  // If it's in some other state, then we haven't yet assigned an image bin slot.
  if (object->GetLockWord(false).GetState() != LockWord::kForwardingAddress) {
    return false;
  } else if (kIsDebugBuild) {
    LockWord lock_word = object->GetLockWord(false);
    size_t offset = lock_word.ForwardingAddress();
    BinSlot bin_slot(offset);
    size_t oat_index = GetOatIndex(object);
    const ImageInfo& image_info = GetImageInfo(oat_index);
    DCHECK_LT(bin_slot.GetIndex(), image_info.bin_slot_sizes_[bin_slot.GetBin()])
        << "bin slot offset should not exceed the size of that bin";
  }
  return true;
}

ImageWriter::BinSlot ImageWriter::GetImageBinSlot(mirror::Object* object) const {
  DCHECK(object != nullptr);
  DCHECK(IsImageBinSlotAssigned(object));

  LockWord lock_word = object->GetLockWord(false);
  size_t offset = lock_word.ForwardingAddress();  // TODO: ForwardingAddress should be uint32_t
  DCHECK_LE(offset, std::numeric_limits<uint32_t>::max());

  BinSlot bin_slot(static_cast<uint32_t>(offset));
  size_t oat_index = GetOatIndex(object);
  const ImageInfo& image_info = GetImageInfo(oat_index);
  DCHECK_LT(bin_slot.GetIndex(), image_info.bin_slot_sizes_[bin_slot.GetBin()]);

  return bin_slot;
}

bool ImageWriter::AllocMemory() {
  for (ImageInfo& image_info : image_infos_) {
    ImageSection unused_sections[ImageHeader::kSectionCount];
    const size_t length = RoundUp(
        image_info.CreateImageSections(unused_sections), kPageSize);

    std::string error_msg;
    image_info.image_.reset(MemMap::MapAnonymous("image writer image",
                                                 nullptr,
                                                 length,
                                                 PROT_READ | PROT_WRITE,
                                                 false,
                                                 false,
                                                 &error_msg));
    if (UNLIKELY(image_info.image_.get() == nullptr)) {
      LOG(ERROR) << "Failed to allocate memory for image file generation: " << error_msg;
      return false;
    }

    // Create the image bitmap, only needs to cover mirror object section which is up to image_end_.
    CHECK_LE(image_info.image_end_, length);
    image_info.image_bitmap_.reset(gc::accounting::ContinuousSpaceBitmap::Create(
        "image bitmap", image_info.image_->Begin(), RoundUp(image_info.image_end_, kPageSize)));
    if (image_info.image_bitmap_.get() == nullptr) {
      LOG(ERROR) << "Failed to allocate memory for image bitmap";
      return false;
    }
  }
  return true;
}

class ComputeLazyFieldsForClassesVisitor : public ClassVisitor {
 public:
  bool operator()(Class* c) OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    StackHandleScope<1> hs(Thread::Current());
    mirror::Class::ComputeName(hs.NewHandle(c));
    return true;
  }
};

void ImageWriter::ComputeLazyFieldsForImageClasses() {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ComputeLazyFieldsForClassesVisitor visitor;
  class_linker->VisitClassesWithoutClassesLock(&visitor);
}

static bool IsBootClassLoaderClass(mirror::Class* klass) SHARED_REQUIRES(Locks::mutator_lock_) {
  return klass->GetClassLoader() == nullptr;
}

bool ImageWriter::IsBootClassLoaderNonImageClass(mirror::Class* klass) {
  return IsBootClassLoaderClass(klass) && !IsInBootImage(klass);
}

bool ImageWriter::PruneAppImageClass(mirror::Class* klass) {
  bool early_exit = false;
  std::unordered_set<mirror::Class*> visited;
  return PruneAppImageClassInternal(klass, &early_exit, &visited);
}

bool ImageWriter::PruneAppImageClassInternal(
    mirror::Class* klass,
    bool* early_exit,
    std::unordered_set<mirror::Class*>* visited) {
  DCHECK(early_exit != nullptr);
  DCHECK(visited != nullptr);
  DCHECK(compile_app_image_);
  if (klass == nullptr || IsInBootImage(klass)) {
    return false;
  }
  auto found = prune_class_memo_.find(klass);
  if (found != prune_class_memo_.end()) {
    // Already computed, return the found value.
    return found->second;
  }
  // Circular dependencies, return false but do not store the result in the memoization table.
  if (visited->find(klass) != visited->end()) {
    *early_exit = true;
    return false;
  }
  visited->emplace(klass);
  bool result = IsBootClassLoaderClass(klass);
  std::string temp;
  // Prune if not an image class, this handles any broken sets of image classes such as having a
  // class in the set but not it's superclass.
  result = result || !compiler_driver_.IsImageClass(klass->GetDescriptor(&temp));
  bool my_early_exit = false;  // Only for ourselves, ignore caller.
  // Remove classes that failed to verify since we don't want to have java.lang.VerifyError in the
  // app image.
  if (klass->GetStatus() == mirror::Class::kStatusError) {
    result = true;
  } else {
    CHECK(klass->GetVerifyError() == nullptr) << PrettyClass(klass);
  }
  if (!result) {
    // Check interfaces since these wont be visited through VisitReferences.)
    mirror::IfTable* if_table = klass->GetIfTable();
    for (size_t i = 0, num_interfaces = klass->GetIfTableCount(); i < num_interfaces; ++i) {
      result = result || PruneAppImageClassInternal(if_table->GetInterface(i),
                                                    &my_early_exit,
                                                    visited);
    }
  }
  if (klass->IsObjectArrayClass()) {
    result = result || PruneAppImageClassInternal(klass->GetComponentType(),
                                                  &my_early_exit,
                                                  visited);
  }
  // Check static fields and their classes.
  size_t num_static_fields = klass->NumReferenceStaticFields();
  if (num_static_fields != 0 && klass->IsResolved()) {
    // Presumably GC can happen when we are cross compiling, it should not cause performance
    // problems to do pointer size logic.
    MemberOffset field_offset = klass->GetFirstReferenceStaticFieldOffset(
        Runtime::Current()->GetClassLinker()->GetImagePointerSize());
    for (size_t i = 0u; i < num_static_fields; ++i) {
      mirror::Object* ref = klass->GetFieldObject<mirror::Object>(field_offset);
      if (ref != nullptr) {
        if (ref->IsClass()) {
          result = result || PruneAppImageClassInternal(ref->AsClass(),
                                                        &my_early_exit,
                                                        visited);
        } else {
          result = result || PruneAppImageClassInternal(ref->GetClass(),
                                                        &my_early_exit,
                                                        visited);
        }
      }
      field_offset = MemberOffset(field_offset.Uint32Value() +
                                  sizeof(mirror::HeapReference<mirror::Object>));
    }
  }
  result = result || PruneAppImageClassInternal(klass->GetSuperClass(),
                                                &my_early_exit,
                                                visited);
  // Erase the element we stored earlier since we are exiting the function.
  auto it = visited->find(klass);
  DCHECK(it != visited->end());
  visited->erase(it);
  // Only store result if it is true or none of the calls early exited due to circular
  // dependencies. If visited is empty then we are the root caller, in this case the cycle was in
  // a child call and we can remember the result.
  if (result == true || !my_early_exit || visited->empty()) {
    prune_class_memo_[klass] = result;
  }
  *early_exit |= my_early_exit;
  return result;
}

bool ImageWriter::KeepClass(Class* klass) {
  if (klass == nullptr) {
    return false;
  }
  if (compile_app_image_ && Runtime::Current()->GetHeap()->ObjectIsInBootImageSpace(klass)) {
    // Already in boot image, return true.
    return true;
  }
  std::string temp;
  if (!compiler_driver_.IsImageClass(klass->GetDescriptor(&temp))) {
    return false;
  }
  if (compile_app_image_) {
    // For app images, we need to prune boot loader classes that are not in the boot image since
    // these may have already been loaded when the app image is loaded.
    // Keep classes in the boot image space since we don't want to re-resolve these.
    return !PruneAppImageClass(klass);
  }
  return true;
}

class NonImageClassesVisitor : public ClassVisitor {
 public:
  explicit NonImageClassesVisitor(ImageWriter* image_writer) : image_writer_(image_writer) {}

  bool operator()(Class* klass) OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    if (!image_writer_->KeepClass(klass)) {
      classes_to_prune_.insert(klass);
    }
    return true;
  }

  std::unordered_set<mirror::Class*> classes_to_prune_;
  ImageWriter* const image_writer_;
};

void ImageWriter::PruneNonImageClasses() {
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  Thread* self = Thread::Current();

  // Clear class table strong roots so that dex caches can get pruned. We require pruning the class
  // path dex caches.
  class_linker->ClearClassTableStrongRoots();

  // Make a list of classes we would like to prune.
  NonImageClassesVisitor visitor(this);
  class_linker->VisitClasses(&visitor);

  // Remove the undesired classes from the class roots.
  VLOG(compiler) << "Pruning " << visitor.classes_to_prune_.size() << " classes";
  for (mirror::Class* klass : visitor.classes_to_prune_) {
    std::string temp;
    const char* name = klass->GetDescriptor(&temp);
    VLOG(compiler) << "Pruning class " << name;
    if (!compile_app_image_) {
      DCHECK(IsBootClassLoaderClass(klass));
    }
    bool result = class_linker->RemoveClass(name, klass->GetClassLoader());
    DCHECK(result);
  }

  // Clear references to removed classes from the DexCaches.
  ArtMethod* resolution_method = runtime->GetResolutionMethod();

  ScopedAssertNoThreadSuspension sa(self, __FUNCTION__);
  ReaderMutexLock mu(self, *Locks::classlinker_classes_lock_);  // For ClassInClassTable
  ReaderMutexLock mu2(self, *class_linker->DexLock());
  for (const ClassLinker::DexCacheData& data : class_linker->GetDexCachesData()) {
    if (self->IsJWeakCleared(data.weak_root)) {
      continue;
    }
    mirror::DexCache* dex_cache = self->DecodeJObject(data.weak_root)->AsDexCache();
    for (size_t i = 0; i < dex_cache->NumResolvedTypes(); i++) {
      Class* klass = dex_cache->GetResolvedType(i);
      if (klass != nullptr && !KeepClass(klass)) {
        dex_cache->SetResolvedType(i, nullptr);
      }
    }
    ArtMethod** resolved_methods = dex_cache->GetResolvedMethods();
    for (size_t i = 0, num = dex_cache->NumResolvedMethods(); i != num; ++i) {
      ArtMethod* method =
          mirror::DexCache::GetElementPtrSize(resolved_methods, i, target_ptr_size_);
      DCHECK(method != nullptr) << "Expected resolution method instead of null method";
      mirror::Class* declaring_class = method->GetDeclaringClass();
      // Copied methods may be held live by a class which was not an image class but have a
      // declaring class which is an image class. Set it to the resolution method to be safe and
      // prevent dangling pointers.
      if (method->IsCopied() || !KeepClass(declaring_class)) {
        mirror::DexCache::SetElementPtrSize(resolved_methods,
                                            i,
                                            resolution_method,
                                            target_ptr_size_);
      } else {
        // Check that the class is still in the classes table.
        DCHECK(class_linker->ClassInClassTable(declaring_class)) << "Class "
            << PrettyClass(declaring_class) << " not in class linker table";
      }
    }
    ArtField** resolved_fields = dex_cache->GetResolvedFields();
    for (size_t i = 0; i < dex_cache->NumResolvedFields(); i++) {
      ArtField* field = mirror::DexCache::GetElementPtrSize(resolved_fields, i, target_ptr_size_);
      if (field != nullptr && !KeepClass(field->GetDeclaringClass())) {
        dex_cache->SetResolvedField(i, nullptr, target_ptr_size_);
      }
    }
    // Clean the dex field. It might have been populated during the initialization phase, but
    // contains data only valid during a real run.
    dex_cache->SetFieldObject<false>(mirror::DexCache::DexOffset(), nullptr);
  }

  // Drop the array class cache in the ClassLinker, as these are roots holding those classes live.
  class_linker->DropFindArrayClassCache();

  // Clear to save RAM.
  prune_class_memo_.clear();
}

void ImageWriter::CheckNonImageClassesRemoved() {
  if (compiler_driver_.GetImageClasses() != nullptr) {
    gc::Heap* heap = Runtime::Current()->GetHeap();
    heap->VisitObjects(CheckNonImageClassesRemovedCallback, this);
  }
}

void ImageWriter::CheckNonImageClassesRemovedCallback(Object* obj, void* arg) {
  ImageWriter* image_writer = reinterpret_cast<ImageWriter*>(arg);
  if (obj->IsClass() && !image_writer->IsInBootImage(obj)) {
    Class* klass = obj->AsClass();
    if (!image_writer->KeepClass(klass)) {
      image_writer->DumpImageClasses();
      std::string temp;
      CHECK(image_writer->KeepClass(klass)) << klass->GetDescriptor(&temp)
                                            << " " << PrettyDescriptor(klass);
    }
  }
}

void ImageWriter::DumpImageClasses() {
  auto image_classes = compiler_driver_.GetImageClasses();
  CHECK(image_classes != nullptr);
  for (const std::string& image_class : *image_classes) {
    LOG(INFO) << " " << image_class;
  }
}

mirror::String* ImageWriter::FindInternedString(mirror::String* string) {
  Thread* const self = Thread::Current();
  for (const ImageInfo& image_info : image_infos_) {
    mirror::String* const found = image_info.intern_table_->LookupStrong(self, string);
    DCHECK(image_info.intern_table_->LookupWeak(self, string) == nullptr)
        << string->ToModifiedUtf8();
    if (found != nullptr) {
      return found;
    }
  }
  if (compile_app_image_) {
    Runtime* const runtime = Runtime::Current();
    mirror::String* found = runtime->GetInternTable()->LookupStrong(self, string);
    // If we found it in the runtime intern table it could either be in the boot image or interned
    // during app image compilation. If it was in the boot image return that, otherwise return null
    // since it belongs to another image space.
    if (found != nullptr && runtime->GetHeap()->ObjectIsInBootImageSpace(found)) {
      return found;
    }
    DCHECK(runtime->GetInternTable()->LookupWeak(self, string) == nullptr)
        << string->ToModifiedUtf8();
  }
  return nullptr;
}


ObjectArray<Object>* ImageWriter::CreateImageRoots(size_t oat_index) const {
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  Thread* self = Thread::Current();
  StackHandleScope<3> hs(self);
  Handle<Class> object_array_class(hs.NewHandle(
      class_linker->FindSystemClass(self, "[Ljava/lang/Object;")));

  std::unordered_set<const DexFile*> image_dex_files;
  for (auto& pair : dex_file_oat_index_map_) {
    const DexFile* image_dex_file = pair.first;
    size_t image_oat_index = pair.second;
    if (oat_index == image_oat_index) {
      image_dex_files.insert(image_dex_file);
    }
  }

  // build an Object[] of all the DexCaches used in the source_space_.
  // Since we can't hold the dex lock when allocating the dex_caches
  // ObjectArray, we lock the dex lock twice, first to get the number
  // of dex caches first and then lock it again to copy the dex
  // caches. We check that the number of dex caches does not change.
  size_t dex_cache_count = 0;
  {
    ReaderMutexLock mu(self, *class_linker->DexLock());
    // Count number of dex caches not in the boot image.
    for (const ClassLinker::DexCacheData& data : class_linker->GetDexCachesData()) {
      mirror::DexCache* dex_cache =
          down_cast<mirror::DexCache*>(self->DecodeJObject(data.weak_root));
      if (dex_cache == nullptr) {
        continue;
      }
      const DexFile* dex_file = dex_cache->GetDexFile();
      if (!IsInBootImage(dex_cache)) {
        dex_cache_count += image_dex_files.find(dex_file) != image_dex_files.end() ? 1u : 0u;
      }
    }
  }
  Handle<ObjectArray<Object>> dex_caches(
      hs.NewHandle(ObjectArray<Object>::Alloc(self, object_array_class.Get(), dex_cache_count)));
  CHECK(dex_caches.Get() != nullptr) << "Failed to allocate a dex cache array.";
  {
    ReaderMutexLock mu(self, *class_linker->DexLock());
    size_t non_image_dex_caches = 0;
    // Re-count number of non image dex caches.
    for (const ClassLinker::DexCacheData& data : class_linker->GetDexCachesData()) {
      mirror::DexCache* dex_cache =
          down_cast<mirror::DexCache*>(self->DecodeJObject(data.weak_root));
      if (dex_cache == nullptr) {
        continue;
      }
      const DexFile* dex_file = dex_cache->GetDexFile();
      if (!IsInBootImage(dex_cache)) {
        non_image_dex_caches += image_dex_files.find(dex_file) != image_dex_files.end() ? 1u : 0u;
      }
    }
    CHECK_EQ(dex_cache_count, non_image_dex_caches)
        << "The number of non-image dex caches changed.";
    size_t i = 0;
    for (const ClassLinker::DexCacheData& data : class_linker->GetDexCachesData()) {
      mirror::DexCache* dex_cache =
          down_cast<mirror::DexCache*>(self->DecodeJObject(data.weak_root));
      if (dex_cache == nullptr) {
        continue;
      }
      const DexFile* dex_file = dex_cache->GetDexFile();
      if (!IsInBootImage(dex_cache) && image_dex_files.find(dex_file) != image_dex_files.end()) {
        dex_caches->Set<false>(i, dex_cache);
        ++i;
      }
    }
  }

  // build an Object[] of the roots needed to restore the runtime
  auto image_roots(hs.NewHandle(
      ObjectArray<Object>::Alloc(self, object_array_class.Get(), ImageHeader::kImageRootsMax)));
  image_roots->Set<false>(ImageHeader::kDexCaches, dex_caches.Get());
  image_roots->Set<false>(ImageHeader::kClassRoots, class_linker->GetClassRoots());
  for (int i = 0; i < ImageHeader::kImageRootsMax; i++) {
    CHECK(image_roots->Get(i) != nullptr);
  }
  return image_roots.Get();
}

mirror::Object* ImageWriter::TryAssignBinSlot(WorkStack& work_stack,
                                              mirror::Object* obj,
                                              size_t oat_index) {
  if (obj == nullptr || IsInBootImage(obj)) {
    // Object is null or already in the image, there is no work to do.
    return obj;
  }
  if (!IsImageBinSlotAssigned(obj)) {
    // We want to intern all strings but also assign offsets for the source string. Since the
    // pruning phase has already happened, if we intern a string to one in the image we still
    // end up copying an unreachable string.
    if (obj->IsString()) {
      // Need to check if the string is already interned in another image info so that we don't have
      // the intern tables of two different images contain the same string.
      mirror::String* interned = FindInternedString(obj->AsString());
      if (interned == nullptr) {
        // Not in another image space, insert to our table.
        interned = GetImageInfo(oat_index).intern_table_->InternStrongImageString(obj->AsString());
        DCHECK_EQ(interned, obj);
      }
    } else if (obj->IsDexCache()) {
      oat_index = GetOatIndexForDexCache(obj->AsDexCache());
    } else if (obj->IsClass()) {
      // Visit and assign offsets for fields and field arrays.
      mirror::Class* as_klass = obj->AsClass();
      mirror::DexCache* dex_cache = as_klass->GetDexCache();
      DCHECK_NE(as_klass->GetStatus(), mirror::Class::kStatusError);
      if (compile_app_image_) {
        // Extra sanity, no boot loader classes should be left!
        CHECK(!IsBootClassLoaderClass(as_klass)) << PrettyClass(as_klass);
      }
      LengthPrefixedArray<ArtField>* fields[] = {
          as_klass->GetSFieldsPtr(), as_klass->GetIFieldsPtr(),
      };
      // Overwrite the oat index value since the class' dex cache is more accurate of where it
      // belongs.
      oat_index = GetOatIndexForDexCache(dex_cache);
      ImageInfo& image_info = GetImageInfo(oat_index);
      {
        // Note: This table is only accessed from the image writer, avoid locking to prevent lock
        // order violations from root visiting.
        image_info.class_table_->InsertWithoutLocks(as_klass);
      }
      for (LengthPrefixedArray<ArtField>* cur_fields : fields) {
        // Total array length including header.
        if (cur_fields != nullptr) {
          const size_t header_size = LengthPrefixedArray<ArtField>::ComputeSize(0);
          // Forward the entire array at once.
          auto it = native_object_relocations_.find(cur_fields);
          CHECK(it == native_object_relocations_.end()) << "Field array " << cur_fields
                                                  << " already forwarded";
          size_t& offset = image_info.bin_slot_sizes_[kBinArtField];
          DCHECK(!IsInBootImage(cur_fields));
          native_object_relocations_.emplace(
              cur_fields,
              NativeObjectRelocation {
                  oat_index, offset, kNativeObjectRelocationTypeArtFieldArray
              });
          offset += header_size;
          // Forward individual fields so that we can quickly find where they belong.
          for (size_t i = 0, count = cur_fields->size(); i < count; ++i) {
            // Need to forward arrays separate of fields.
            ArtField* field = &cur_fields->At(i);
            auto it2 = native_object_relocations_.find(field);
            CHECK(it2 == native_object_relocations_.end()) << "Field at index=" << i
                << " already assigned " << PrettyField(field) << " static=" << field->IsStatic();
            DCHECK(!IsInBootImage(field));
            native_object_relocations_.emplace(
                field,
                NativeObjectRelocation { oat_index, offset, kNativeObjectRelocationTypeArtField });
            offset += sizeof(ArtField);
          }
        }
      }
      // Visit and assign offsets for methods.
      size_t num_methods = as_klass->NumMethods();
      if (num_methods != 0) {
        bool any_dirty = false;
        for (auto& m : as_klass->GetMethods(target_ptr_size_)) {
          if (WillMethodBeDirty(&m)) {
            any_dirty = true;
            break;
          }
        }
        NativeObjectRelocationType type = any_dirty
            ? kNativeObjectRelocationTypeArtMethodDirty
            : kNativeObjectRelocationTypeArtMethodClean;
        Bin bin_type = BinTypeForNativeRelocationType(type);
        // Forward the entire array at once, but header first.
        const size_t method_alignment = ArtMethod::Alignment(target_ptr_size_);
        const size_t method_size = ArtMethod::Size(target_ptr_size_);
        const size_t header_size = LengthPrefixedArray<ArtMethod>::ComputeSize(0,
                                                                               method_size,
                                                                               method_alignment);
        LengthPrefixedArray<ArtMethod>* array = as_klass->GetMethodsPtr();
        auto it = native_object_relocations_.find(array);
        CHECK(it == native_object_relocations_.end())
            << "Method array " << array << " already forwarded";
        size_t& offset = image_info.bin_slot_sizes_[bin_type];
        DCHECK(!IsInBootImage(array));
        native_object_relocations_.emplace(array,
            NativeObjectRelocation {
                oat_index,
                offset,
                any_dirty ? kNativeObjectRelocationTypeArtMethodArrayDirty
                          : kNativeObjectRelocationTypeArtMethodArrayClean });
        offset += header_size;
        for (auto& m : as_klass->GetMethods(target_ptr_size_)) {
          AssignMethodOffset(&m, type, oat_index);
        }
        (any_dirty ? dirty_methods_ : clean_methods_) += num_methods;
      }
      // Assign offsets for all runtime methods in the IMT since these may hold conflict tables
      // live.
      if (as_klass->ShouldHaveImt()) {
        ImTable* imt = as_klass->GetImt(target_ptr_size_);
        for (size_t i = 0; i < ImTable::kSize; ++i) {
          ArtMethod* imt_method = imt->Get(i, target_ptr_size_);
          DCHECK(imt_method != nullptr);
          if (imt_method->IsRuntimeMethod() &&
              !IsInBootImage(imt_method) &&
              !NativeRelocationAssigned(imt_method)) {
            AssignMethodOffset(imt_method, kNativeObjectRelocationTypeRuntimeMethod, oat_index);
          }
        }
      }

      if (as_klass->ShouldHaveImt()) {
        ImTable* imt = as_klass->GetImt(target_ptr_size_);
        TryAssignImTableOffset(imt, oat_index);
      }
    } else if (obj->IsClassLoader()) {
      // Register the class loader if it has a class table.
      // The fake boot class loader should not get registered and we should end up with only one
      // class loader.
      mirror::ClassLoader* class_loader = obj->AsClassLoader();
      if (class_loader->GetClassTable() != nullptr) {
        class_loaders_.insert(class_loader);
      }
    }
    AssignImageBinSlot(obj, oat_index);
    work_stack.emplace(obj, oat_index);
  }
  if (obj->IsString()) {
    // Always return the interned string if there exists one.
    mirror::String* interned = FindInternedString(obj->AsString());
    if (interned != nullptr) {
      return interned;
    }
  }
  return obj;
}

bool ImageWriter::NativeRelocationAssigned(void* ptr) const {
  return native_object_relocations_.find(ptr) != native_object_relocations_.end();
}

void ImageWriter::TryAssignImTableOffset(ImTable* imt, size_t oat_index) {
  // No offset, or already assigned.
  if (imt == nullptr || IsInBootImage(imt) || NativeRelocationAssigned(imt)) {
    return;
  }
  // If the method is a conflict method we also want to assign the conflict table offset.
  ImageInfo& image_info = GetImageInfo(oat_index);
  const size_t size = ImTable::SizeInBytes(target_ptr_size_);
  native_object_relocations_.emplace(
      imt,
      NativeObjectRelocation {
          oat_index,
          image_info.bin_slot_sizes_[kBinImTable],
          kNativeObjectRelocationTypeIMTable});
  image_info.bin_slot_sizes_[kBinImTable] += size;
}

void ImageWriter::TryAssignConflictTableOffset(ImtConflictTable* table, size_t oat_index) {
  // No offset, or already assigned.
  if (table == nullptr || NativeRelocationAssigned(table)) {
    return;
  }
  CHECK(!IsInBootImage(table));
  // If the method is a conflict method we also want to assign the conflict table offset.
  ImageInfo& image_info = GetImageInfo(oat_index);
  const size_t size = table->ComputeSize(target_ptr_size_);
  native_object_relocations_.emplace(
      table,
      NativeObjectRelocation {
          oat_index,
          image_info.bin_slot_sizes_[kBinIMTConflictTable],
          kNativeObjectRelocationTypeIMTConflictTable});
  image_info.bin_slot_sizes_[kBinIMTConflictTable] += size;
}

void ImageWriter::AssignMethodOffset(ArtMethod* method,
                                     NativeObjectRelocationType type,
                                     size_t oat_index) {
  DCHECK(!IsInBootImage(method));
  CHECK(!NativeRelocationAssigned(method)) << "Method " << method << " already assigned "
      << PrettyMethod(method);
  if (method->IsRuntimeMethod()) {
    TryAssignConflictTableOffset(method->GetImtConflictTable(target_ptr_size_), oat_index);
  }
  ImageInfo& image_info = GetImageInfo(oat_index);
  size_t& offset = image_info.bin_slot_sizes_[BinTypeForNativeRelocationType(type)];
  native_object_relocations_.emplace(method, NativeObjectRelocation { oat_index, offset, type });
  offset += ArtMethod::Size(target_ptr_size_);
}

void ImageWriter::EnsureBinSlotAssignedCallback(mirror::Object* obj, void* arg) {
  ImageWriter* writer = reinterpret_cast<ImageWriter*>(arg);
  DCHECK(writer != nullptr);
  if (!Runtime::Current()->GetHeap()->ObjectIsInBootImageSpace(obj)) {
    CHECK(writer->IsImageBinSlotAssigned(obj)) << PrettyTypeOf(obj) << " " << obj;
  }
}

void ImageWriter::DeflateMonitorCallback(mirror::Object* obj, void* arg ATTRIBUTE_UNUSED) {
  Monitor::Deflate(Thread::Current(), obj);
}

void ImageWriter::UnbinObjectsIntoOffsetCallback(mirror::Object* obj, void* arg) {
  ImageWriter* writer = reinterpret_cast<ImageWriter*>(arg);
  DCHECK(writer != nullptr);
  if (!writer->IsInBootImage(obj)) {
    writer->UnbinObjectsIntoOffset(obj);
  }
}

void ImageWriter::UnbinObjectsIntoOffset(mirror::Object* obj) {
  DCHECK(!IsInBootImage(obj));
  CHECK(obj != nullptr);

  // We know the bin slot, and the total bin sizes for all objects by now,
  // so calculate the object's final image offset.

  DCHECK(IsImageBinSlotAssigned(obj));
  BinSlot bin_slot = GetImageBinSlot(obj);
  // Change the lockword from a bin slot into an offset
  AssignImageOffset(obj, bin_slot);
}

class ImageWriter::VisitReferencesVisitor {
 public:
  VisitReferencesVisitor(ImageWriter* image_writer, WorkStack* work_stack, size_t oat_index)
      : image_writer_(image_writer), work_stack_(work_stack), oat_index_(oat_index) {}

  // Fix up separately since we also need to fix up method entrypoints.
  ALWAYS_INLINE void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    if (!root->IsNull()) {
      VisitRoot(root);
    }
  }

  ALWAYS_INLINE void VisitRoot(mirror::CompressedReference<mirror::Object>* root) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    root->Assign(VisitReference(root->AsMirrorPtr()));
  }

  ALWAYS_INLINE void operator() (mirror::Object* obj,
                                 MemberOffset offset,
                                 bool is_static ATTRIBUTE_UNUSED) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    mirror::Object* ref =
        obj->GetFieldObject<mirror::Object, kVerifyNone, kWithoutReadBarrier>(offset);
    obj->SetFieldObject</*kTransactionActive*/false>(offset, VisitReference(ref));
  }

  ALWAYS_INLINE void operator() (mirror::Class* klass ATTRIBUTE_UNUSED,
                                 mirror::Reference* ref) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    ref->SetReferent</*kTransactionActive*/false>(
        VisitReference(ref->GetReferent<kWithoutReadBarrier>()));
  }

 private:
  mirror::Object* VisitReference(mirror::Object* ref) const SHARED_REQUIRES(Locks::mutator_lock_) {
    return image_writer_->TryAssignBinSlot(*work_stack_, ref, oat_index_);
  }

  ImageWriter* const image_writer_;
  WorkStack* const work_stack_;
  const size_t oat_index_;
};

class ImageWriter::GetRootsVisitor : public RootVisitor  {
 public:
  explicit GetRootsVisitor(std::vector<mirror::Object*>* roots) : roots_(roots) {}

  void VisitRoots(mirror::Object*** roots,
                  size_t count,
                  const RootInfo& info ATTRIBUTE_UNUSED) OVERRIDE
      SHARED_REQUIRES(Locks::mutator_lock_) {
    for (size_t i = 0; i < count; ++i) {
      roots_->push_back(*roots[i]);
    }
  }

  void VisitRoots(mirror::CompressedReference<mirror::Object>** roots,
                  size_t count,
                  const RootInfo& info ATTRIBUTE_UNUSED) OVERRIDE
      SHARED_REQUIRES(Locks::mutator_lock_) {
    for (size_t i = 0; i < count; ++i) {
      roots_->push_back(roots[i]->AsMirrorPtr());
    }
  }

 private:
  std::vector<mirror::Object*>* const roots_;
};

void ImageWriter::ProcessWorkStack(WorkStack* work_stack) {
  while (!work_stack->empty()) {
    std::pair<mirror::Object*, size_t> pair(work_stack->top());
    work_stack->pop();
    VisitReferencesVisitor visitor(this, work_stack, /*oat_index*/ pair.second);
    // Walk references and assign bin slots for them.
    pair.first->VisitReferences</*kVisitNativeRoots*/true, kVerifyNone, kWithoutReadBarrier>(
        visitor,
        visitor);
  }
}

void ImageWriter::CalculateNewObjectOffsets() {
  Thread* const self = Thread::Current();
  StackHandleScopeCollection handles(self);
  std::vector<Handle<ObjectArray<Object>>> image_roots;
  for (size_t i = 0, size = oat_filenames_.size(); i != size; ++i) {
    image_roots.push_back(handles.NewHandle(CreateImageRoots(i)));
  }

  Runtime* const runtime = Runtime::Current();
  gc::Heap* const heap = runtime->GetHeap();

  // Leave space for the header, but do not write it yet, we need to
  // know where image_roots is going to end up
  image_objects_offset_begin_ = RoundUp(sizeof(ImageHeader), kObjectAlignment);  // 64-bit-alignment

  const size_t method_alignment = ArtMethod::Alignment(target_ptr_size_);
  // Write the image runtime methods.
  image_methods_[ImageHeader::kResolutionMethod] = runtime->GetResolutionMethod();
  image_methods_[ImageHeader::kImtConflictMethod] = runtime->GetImtConflictMethod();
  image_methods_[ImageHeader::kImtUnimplementedMethod] = runtime->GetImtUnimplementedMethod();
  image_methods_[ImageHeader::kCalleeSaveMethod] = runtime->GetCalleeSaveMethod(Runtime::kSaveAll);
  image_methods_[ImageHeader::kRefsOnlySaveMethod] =
      runtime->GetCalleeSaveMethod(Runtime::kRefsOnly);
  image_methods_[ImageHeader::kRefsAndArgsSaveMethod] =
      runtime->GetCalleeSaveMethod(Runtime::kRefsAndArgs);
  // Visit image methods first to have the main runtime methods in the first image.
  for (auto* m : image_methods_) {
    CHECK(m != nullptr);
    CHECK(m->IsRuntimeMethod());
    DCHECK_EQ(compile_app_image_, IsInBootImage(m)) << "Trampolines should be in boot image";
    if (!IsInBootImage(m)) {
      AssignMethodOffset(m, kNativeObjectRelocationTypeRuntimeMethod, GetDefaultOatIndex());
    }
  }

  // Deflate monitors before we visit roots since deflating acquires the monitor lock. Acquiring
  // this lock while holding other locks may cause lock order violations.
  heap->VisitObjects(DeflateMonitorCallback, this);

  // Work list of <object, oat_index> for objects. Everything on the stack must already be
  // assigned a bin slot.
  WorkStack work_stack;

  // Special case interned strings to put them in the image they are likely to be resolved from.
  for (const DexFile* dex_file : compiler_driver_.GetDexFilesForOatFile()) {
    auto it = dex_file_oat_index_map_.find(dex_file);
    DCHECK(it != dex_file_oat_index_map_.end()) << dex_file->GetLocation();
    const size_t oat_index = it->second;
    InternTable* const intern_table = runtime->GetInternTable();
    for (size_t i = 0, count = dex_file->NumStringIds(); i < count; ++i) {
      uint32_t utf16_length;
      const char* utf8_data = dex_file->StringDataAndUtf16LengthByIdx(i, &utf16_length);
      mirror::String* string = intern_table->LookupStrong(self, utf16_length, utf8_data);
      TryAssignBinSlot(work_stack, string, oat_index);
    }
  }

  // Get the GC roots and then visit them separately to avoid lock violations since the root visitor
  // visits roots while holding various locks.
  {
    std::vector<mirror::Object*> roots;
    GetRootsVisitor root_visitor(&roots);
    runtime->VisitRoots(&root_visitor);
    for (mirror::Object* obj : roots) {
      TryAssignBinSlot(work_stack, obj, GetDefaultOatIndex());
    }
  }
  ProcessWorkStack(&work_stack);

  // For app images, there may be objects that are only held live by the by the boot image. One
  // example is finalizer references. Forward these objects so that EnsureBinSlotAssignedCallback
  // does not fail any checks. TODO: We should probably avoid copying these objects.
  if (compile_app_image_) {
    for (gc::space::ImageSpace* space : heap->GetBootImageSpaces()) {
      DCHECK(space->IsImageSpace());
      gc::accounting::ContinuousSpaceBitmap* live_bitmap = space->GetLiveBitmap();
      live_bitmap->VisitMarkedRange(reinterpret_cast<uintptr_t>(space->Begin()),
                                    reinterpret_cast<uintptr_t>(space->Limit()),
                                    [this, &work_stack](mirror::Object* obj)
          SHARED_REQUIRES(Locks::mutator_lock_) {
        VisitReferencesVisitor visitor(this, &work_stack, GetDefaultOatIndex());
        // Visit all references and try to assign bin slots for them (calls TryAssignBinSlot).
        obj->VisitReferences</*kVisitNativeRoots*/true, kVerifyNone, kWithoutReadBarrier>(
            visitor,
            visitor);
      });
    }
    // Process the work stack in case anything was added by TryAssignBinSlot.
    ProcessWorkStack(&work_stack);
  }

  // Verify that all objects have assigned image bin slots.
  heap->VisitObjects(EnsureBinSlotAssignedCallback, this);

  // Calculate size of the dex cache arrays slot and prepare offsets.
  PrepareDexCacheArraySlots();

  // Calculate the sizes of the intern tables and class tables.
  for (ImageInfo& image_info : image_infos_) {
    // Calculate how big the intern table will be after being serialized.
    InternTable* const intern_table = image_info.intern_table_.get();
    CHECK_EQ(intern_table->WeakSize(), 0u) << " should have strong interned all the strings";
    image_info.intern_table_bytes_ = intern_table->WriteToMemory(nullptr);
    // Calculate the size of the class table.
    ReaderMutexLock mu(self, *Locks::classlinker_classes_lock_);
    image_info.class_table_bytes_ += image_info.class_table_->WriteToMemory(nullptr);
  }

  // Calculate bin slot offsets.
  for (ImageInfo& image_info : image_infos_) {
    size_t bin_offset = image_objects_offset_begin_;
    for (size_t i = 0; i != kBinSize; ++i) {
      switch (i) {
        case kBinArtMethodClean:
        case kBinArtMethodDirty: {
          bin_offset = RoundUp(bin_offset, method_alignment);
          break;
        }
        case kBinImTable:
        case kBinIMTConflictTable: {
          bin_offset = RoundUp(bin_offset, target_ptr_size_);
          break;
        }
        default: {
          // Normal alignment.
        }
      }
      image_info.bin_slot_offsets_[i] = bin_offset;
      bin_offset += image_info.bin_slot_sizes_[i];
    }
    // NOTE: There may be additional padding between the bin slots and the intern table.
    DCHECK_EQ(image_info.image_end_,
              GetBinSizeSum(image_info, kBinMirrorCount) + image_objects_offset_begin_);
  }

  // Calculate image offsets.
  size_t image_offset = 0;
  for (ImageInfo& image_info : image_infos_) {
    image_info.image_begin_ = global_image_begin_ + image_offset;
    image_info.image_offset_ = image_offset;
    ImageSection unused_sections[ImageHeader::kSectionCount];
    image_info.image_size_ = RoundUp(image_info.CreateImageSections(unused_sections), kPageSize);
    // There should be no gaps until the next image.
    image_offset += image_info.image_size_;
  }

  // Transform each object's bin slot into an offset which will be used to do the final copy.
  heap->VisitObjects(UnbinObjectsIntoOffsetCallback, this);

  // DCHECK_EQ(image_end_, GetBinSizeSum(kBinMirrorCount) + image_objects_offset_begin_);

  size_t i = 0;
  for (ImageInfo& image_info : image_infos_) {
    image_info.image_roots_address_ = PointerToLowMemUInt32(GetImageAddress(image_roots[i].Get()));
    i++;
  }

  // Update the native relocations by adding their bin sums.
  for (auto& pair : native_object_relocations_) {
    NativeObjectRelocation& relocation = pair.second;
    Bin bin_type = BinTypeForNativeRelocationType(relocation.type);
    ImageInfo& image_info = GetImageInfo(relocation.oat_index);
    relocation.offset += image_info.bin_slot_offsets_[bin_type];
  }

  // Note that image_info.image_end_ is left at end of used mirror object section.
}

size_t ImageWriter::ImageInfo::CreateImageSections(ImageSection* out_sections) const {
  DCHECK(out_sections != nullptr);

  // Do not round up any sections here that are represented by the bins since it will break
  // offsets.

  // Objects section
  ImageSection* objects_section = &out_sections[ImageHeader::kSectionObjects];
  *objects_section = ImageSection(0u, image_end_);

  // Add field section.
  ImageSection* field_section = &out_sections[ImageHeader::kSectionArtFields];
  *field_section = ImageSection(bin_slot_offsets_[kBinArtField], bin_slot_sizes_[kBinArtField]);
  CHECK_EQ(bin_slot_offsets_[kBinArtField], field_section->Offset());

  // Add method section.
  ImageSection* methods_section = &out_sections[ImageHeader::kSectionArtMethods];
  *methods_section = ImageSection(
      bin_slot_offsets_[kBinArtMethodClean],
      bin_slot_sizes_[kBinArtMethodClean] + bin_slot_sizes_[kBinArtMethodDirty]);

  // IMT section.
  ImageSection* imt_section = &out_sections[ImageHeader::kSectionImTables];
  *imt_section = ImageSection(bin_slot_offsets_[kBinImTable], bin_slot_sizes_[kBinImTable]);

  // Conflict tables section.
  ImageSection* imt_conflict_tables_section = &out_sections[ImageHeader::kSectionIMTConflictTables];
  *imt_conflict_tables_section = ImageSection(bin_slot_offsets_[kBinIMTConflictTable],
                                              bin_slot_sizes_[kBinIMTConflictTable]);

  // Runtime methods section.
  ImageSection* runtime_methods_section = &out_sections[ImageHeader::kSectionRuntimeMethods];
  *runtime_methods_section = ImageSection(bin_slot_offsets_[kBinRuntimeMethod],
                                          bin_slot_sizes_[kBinRuntimeMethod]);

  // Add dex cache arrays section.
  ImageSection* dex_cache_arrays_section = &out_sections[ImageHeader::kSectionDexCacheArrays];
  *dex_cache_arrays_section = ImageSection(bin_slot_offsets_[kBinDexCacheArray],
                                           bin_slot_sizes_[kBinDexCacheArray]);

  // Round up to the alignment the string table expects. See HashSet::WriteToMemory.
  size_t cur_pos = RoundUp(dex_cache_arrays_section->End(), sizeof(uint64_t));
  // Calculate the size of the interned strings.
  ImageSection* interned_strings_section = &out_sections[ImageHeader::kSectionInternedStrings];
  *interned_strings_section = ImageSection(cur_pos, intern_table_bytes_);
  cur_pos = interned_strings_section->End();
  // Round up to the alignment the class table expects. See HashSet::WriteToMemory.
  cur_pos = RoundUp(cur_pos, sizeof(uint64_t));
  // Calculate the size of the class table section.
  ImageSection* class_table_section = &out_sections[ImageHeader::kSectionClassTable];
  *class_table_section = ImageSection(cur_pos, class_table_bytes_);
  cur_pos = class_table_section->End();
  // Image end goes right before the start of the image bitmap.
  return cur_pos;
}

void ImageWriter::CreateHeader(size_t oat_index) {
  ImageInfo& image_info = GetImageInfo(oat_index);
  const uint8_t* oat_file_begin = image_info.oat_file_begin_;
  const uint8_t* oat_file_end = oat_file_begin + image_info.oat_loaded_size_;
  const uint8_t* oat_data_end = image_info.oat_data_begin_ + image_info.oat_size_;

  // Create the image sections.
  ImageSection sections[ImageHeader::kSectionCount];
  const size_t image_end = image_info.CreateImageSections(sections);

  // Finally bitmap section.
  const size_t bitmap_bytes = image_info.image_bitmap_->Size();
  auto* bitmap_section = &sections[ImageHeader::kSectionImageBitmap];
  *bitmap_section = ImageSection(RoundUp(image_end, kPageSize), RoundUp(bitmap_bytes, kPageSize));
  if (VLOG_IS_ON(compiler)) {
    LOG(INFO) << "Creating header for " << oat_filenames_[oat_index];
    size_t idx = 0;
    for (const ImageSection& section : sections) {
      LOG(INFO) << static_cast<ImageHeader::ImageSections>(idx) << " " << section;
      ++idx;
    }
    LOG(INFO) << "Methods: clean=" << clean_methods_ << " dirty=" << dirty_methods_;
    LOG(INFO) << "Image roots address=" << std::hex << image_info.image_roots_address_ << std::dec;
    LOG(INFO) << "Image begin=" << std::hex << reinterpret_cast<uintptr_t>(global_image_begin_)
              << " Image offset=" << image_info.image_offset_ << std::dec;
    LOG(INFO) << "Oat file begin=" << std::hex << reinterpret_cast<uintptr_t>(oat_file_begin)
              << " Oat data begin=" << reinterpret_cast<uintptr_t>(image_info.oat_data_begin_)
              << " Oat data end=" << reinterpret_cast<uintptr_t>(oat_data_end)
              << " Oat file end=" << reinterpret_cast<uintptr_t>(oat_file_end);
  }
  // Store boot image info for app image so that we can relocate.
  uint32_t boot_image_begin = 0;
  uint32_t boot_image_end = 0;
  uint32_t boot_oat_begin = 0;
  uint32_t boot_oat_end = 0;
  gc::Heap* const heap = Runtime::Current()->GetHeap();
  heap->GetBootImagesSize(&boot_image_begin, &boot_image_end, &boot_oat_begin, &boot_oat_end);

  // Create the header, leave 0 for data size since we will fill this in as we are writing the
  // image.
  new (image_info.image_->Begin()) ImageHeader(PointerToLowMemUInt32(image_info.image_begin_),
                                               image_end,
                                               sections,
                                               image_info.image_roots_address_,
                                               image_info.oat_checksum_,
                                               PointerToLowMemUInt32(oat_file_begin),
                                               PointerToLowMemUInt32(image_info.oat_data_begin_),
                                               PointerToLowMemUInt32(oat_data_end),
                                               PointerToLowMemUInt32(oat_file_end),
                                               boot_image_begin,
                                               boot_image_end - boot_image_begin,
                                               boot_oat_begin,
                                               boot_oat_end - boot_oat_begin,
                                               target_ptr_size_,
                                               compile_pic_,
                                               /*is_pic*/compile_app_image_,
                                               image_storage_mode_,
                                               /*data_size*/0u);
}

ArtMethod* ImageWriter::GetImageMethodAddress(ArtMethod* method) {
  auto it = native_object_relocations_.find(method);
  CHECK(it != native_object_relocations_.end()) << PrettyMethod(method) << " @ " << method;
  size_t oat_index = GetOatIndex(method->GetDexCache());
  ImageInfo& image_info = GetImageInfo(oat_index);
  CHECK_GE(it->second.offset, image_info.image_end_) << "ArtMethods should be after Objects";
  return reinterpret_cast<ArtMethod*>(image_info.image_begin_ + it->second.offset);
}

class FixupRootVisitor : public RootVisitor {
 public:
  explicit FixupRootVisitor(ImageWriter* image_writer) : image_writer_(image_writer) {
  }

  void VisitRoots(mirror::Object*** roots, size_t count, const RootInfo& info ATTRIBUTE_UNUSED)
      OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    for (size_t i = 0; i < count; ++i) {
      *roots[i] = image_writer_->GetImageAddress(*roots[i]);
    }
  }

  void VisitRoots(mirror::CompressedReference<mirror::Object>** roots, size_t count,
                  const RootInfo& info ATTRIBUTE_UNUSED)
      OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    for (size_t i = 0; i < count; ++i) {
      roots[i]->Assign(image_writer_->GetImageAddress(roots[i]->AsMirrorPtr()));
    }
  }

 private:
  ImageWriter* const image_writer_;
};

void ImageWriter::CopyAndFixupImTable(ImTable* orig, ImTable* copy) {
  for (size_t i = 0; i < ImTable::kSize; ++i) {
    ArtMethod* method = orig->Get(i, target_ptr_size_);
    copy->Set(i, NativeLocationInImage(method), target_ptr_size_);
  }
}

void ImageWriter::CopyAndFixupImtConflictTable(ImtConflictTable* orig, ImtConflictTable* copy) {
  const size_t count = orig->NumEntries(target_ptr_size_);
  for (size_t i = 0; i < count; ++i) {
    ArtMethod* interface_method = orig->GetInterfaceMethod(i, target_ptr_size_);
    ArtMethod* implementation_method = orig->GetImplementationMethod(i, target_ptr_size_);
    copy->SetInterfaceMethod(i, target_ptr_size_, NativeLocationInImage(interface_method));
    copy->SetImplementationMethod(i,
                                  target_ptr_size_,
                                  NativeLocationInImage(implementation_method));
  }
}

void ImageWriter::CopyAndFixupNativeData(size_t oat_index) {
  const ImageInfo& image_info = GetImageInfo(oat_index);
  // Copy ArtFields and methods to their locations and update the array for convenience.
  for (auto& pair : native_object_relocations_) {
    NativeObjectRelocation& relocation = pair.second;
    // Only work with fields and methods that are in the current oat file.
    if (relocation.oat_index != oat_index) {
      continue;
    }
    auto* dest = image_info.image_->Begin() + relocation.offset;
    DCHECK_GE(dest, image_info.image_->Begin() + image_info.image_end_);
    DCHECK(!IsInBootImage(pair.first));
    switch (relocation.type) {
      case kNativeObjectRelocationTypeArtField: {
        memcpy(dest, pair.first, sizeof(ArtField));
        reinterpret_cast<ArtField*>(dest)->SetDeclaringClass(
            GetImageAddress(reinterpret_cast<ArtField*>(pair.first)->GetDeclaringClass()));
        break;
      }
      case kNativeObjectRelocationTypeRuntimeMethod:
      case kNativeObjectRelocationTypeArtMethodClean:
      case kNativeObjectRelocationTypeArtMethodDirty: {
        CopyAndFixupMethod(reinterpret_cast<ArtMethod*>(pair.first),
                           reinterpret_cast<ArtMethod*>(dest),
                           image_info);
        break;
      }
      // For arrays, copy just the header since the elements will get copied by their corresponding
      // relocations.
      case kNativeObjectRelocationTypeArtFieldArray: {
        memcpy(dest, pair.first, LengthPrefixedArray<ArtField>::ComputeSize(0));
        break;
      }
      case kNativeObjectRelocationTypeArtMethodArrayClean:
      case kNativeObjectRelocationTypeArtMethodArrayDirty: {
        size_t size = ArtMethod::Size(target_ptr_size_);
        size_t alignment = ArtMethod::Alignment(target_ptr_size_);
        memcpy(dest, pair.first, LengthPrefixedArray<ArtMethod>::ComputeSize(0, size, alignment));
        // Clear padding to avoid non-deterministic data in the image (and placate valgrind).
        reinterpret_cast<LengthPrefixedArray<ArtMethod>*>(dest)->ClearPadding(size, alignment);
        break;
      }
      case kNativeObjectRelocationTypeDexCacheArray:
        // Nothing to copy here, everything is done in FixupDexCache().
        break;
      case kNativeObjectRelocationTypeIMTable: {
        ImTable* orig_imt = reinterpret_cast<ImTable*>(pair.first);
        ImTable* dest_imt = reinterpret_cast<ImTable*>(dest);
        CopyAndFixupImTable(orig_imt, dest_imt);
        break;
      }
      case kNativeObjectRelocationTypeIMTConflictTable: {
        auto* orig_table = reinterpret_cast<ImtConflictTable*>(pair.first);
        CopyAndFixupImtConflictTable(
            orig_table,
            new(dest)ImtConflictTable(orig_table->NumEntries(target_ptr_size_), target_ptr_size_));
        break;
      }
    }
  }
  // Fixup the image method roots.
  auto* image_header = reinterpret_cast<ImageHeader*>(image_info.image_->Begin());
  for (size_t i = 0; i < ImageHeader::kImageMethodsCount; ++i) {
    ArtMethod* method = image_methods_[i];
    CHECK(method != nullptr);
    if (!IsInBootImage(method)) {
      method = NativeLocationInImage(method);
    }
    image_header->SetImageMethod(static_cast<ImageHeader::ImageMethod>(i), method);
  }
  FixupRootVisitor root_visitor(this);

  // Write the intern table into the image.
  if (image_info.intern_table_bytes_ > 0) {
    const ImageSection& intern_table_section = image_header->GetImageSection(
        ImageHeader::kSectionInternedStrings);
    InternTable* const intern_table = image_info.intern_table_.get();
    uint8_t* const intern_table_memory_ptr =
        image_info.image_->Begin() + intern_table_section.Offset();
    const size_t intern_table_bytes = intern_table->WriteToMemory(intern_table_memory_ptr);
    CHECK_EQ(intern_table_bytes, image_info.intern_table_bytes_);
    // Fixup the pointers in the newly written intern table to contain image addresses.
    InternTable temp_intern_table;
    // Note that we require that ReadFromMemory does not make an internal copy of the elements so that
    // the VisitRoots() will update the memory directly rather than the copies.
    // This also relies on visit roots not doing any verification which could fail after we update
    // the roots to be the image addresses.
    temp_intern_table.AddTableFromMemory(intern_table_memory_ptr);
    CHECK_EQ(temp_intern_table.Size(), intern_table->Size());
    temp_intern_table.VisitRoots(&root_visitor, kVisitRootFlagAllRoots);
  }
  // Write the class table(s) into the image. class_table_bytes_ may be 0 if there are multiple
  // class loaders. Writing multiple class tables into the image is currently unsupported.
  if (image_info.class_table_bytes_ > 0u) {
    const ImageSection& class_table_section = image_header->GetImageSection(
        ImageHeader::kSectionClassTable);
    uint8_t* const class_table_memory_ptr =
        image_info.image_->Begin() + class_table_section.Offset();
    ReaderMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);

    ClassTable* table = image_info.class_table_.get();
    CHECK(table != nullptr);
    const size_t class_table_bytes = table->WriteToMemory(class_table_memory_ptr);
    CHECK_EQ(class_table_bytes, image_info.class_table_bytes_);
    // Fixup the pointers in the newly written class table to contain image addresses. See
    // above comment for intern tables.
    ClassTable temp_class_table;
    temp_class_table.ReadFromMemory(class_table_memory_ptr);
    CHECK_EQ(temp_class_table.NumZygoteClasses(), table->NumNonZygoteClasses() +
             table->NumZygoteClasses());
    BufferedRootVisitor<kDefaultBufferedRootCount> buffered_visitor(&root_visitor,
                                                                    RootInfo(kRootUnknown));
    temp_class_table.VisitRoots(buffered_visitor);
  }
}

void ImageWriter::CopyAndFixupObjects() {
  gc::Heap* heap = Runtime::Current()->GetHeap();
  heap->VisitObjects(CopyAndFixupObjectsCallback, this);
  // Fix up the object previously had hash codes.
  for (const auto& hash_pair : saved_hashcode_map_) {
    Object* obj = hash_pair.first;
    DCHECK_EQ(obj->GetLockWord<kVerifyNone>(false).ReadBarrierState(), 0U);
    obj->SetLockWord<kVerifyNone>(LockWord::FromHashCode(hash_pair.second, 0U), false);
  }
  saved_hashcode_map_.clear();
}

void ImageWriter::CopyAndFixupObjectsCallback(Object* obj, void* arg) {
  DCHECK(obj != nullptr);
  DCHECK(arg != nullptr);
  reinterpret_cast<ImageWriter*>(arg)->CopyAndFixupObject(obj);
}

void ImageWriter::FixupPointerArray(mirror::Object* dst, mirror::PointerArray* arr,
                                    mirror::Class* klass, Bin array_type) {
  CHECK(klass->IsArrayClass());
  CHECK(arr->IsIntArray() || arr->IsLongArray()) << PrettyClass(klass) << " " << arr;
  // Fixup int and long pointers for the ArtMethod or ArtField arrays.
  const size_t num_elements = arr->GetLength();
  dst->SetClass(GetImageAddress(arr->GetClass()));
  auto* dest_array = down_cast<mirror::PointerArray*>(dst);
  for (size_t i = 0, count = num_elements; i < count; ++i) {
    void* elem = arr->GetElementPtrSize<void*>(i, target_ptr_size_);
    if (elem != nullptr && !IsInBootImage(elem)) {
      auto it = native_object_relocations_.find(elem);
      if (UNLIKELY(it == native_object_relocations_.end())) {
        if (it->second.IsArtMethodRelocation()) {
          auto* method = reinterpret_cast<ArtMethod*>(elem);
          LOG(FATAL) << "No relocation entry for ArtMethod " << PrettyMethod(method) << " @ "
              << method << " idx=" << i << "/" << num_elements << " with declaring class "
              << PrettyClass(method->GetDeclaringClass());
        } else {
          CHECK_EQ(array_type, kBinArtField);
          auto* field = reinterpret_cast<ArtField*>(elem);
          LOG(FATAL) << "No relocation entry for ArtField " << PrettyField(field) << " @ "
              << field << " idx=" << i << "/" << num_elements << " with declaring class "
              << PrettyClass(field->GetDeclaringClass());
        }
        UNREACHABLE();
      } else {
        ImageInfo& image_info = GetImageInfo(it->second.oat_index);
        elem = image_info.image_begin_ + it->second.offset;
      }
    }
    dest_array->SetElementPtrSize<false, true>(i, elem, target_ptr_size_);
  }
}

void ImageWriter::CopyAndFixupObject(Object* obj) {
  if (IsInBootImage(obj)) {
    return;
  }
  size_t offset = GetImageOffset(obj);
  size_t oat_index = GetOatIndex(obj);
  ImageInfo& image_info = GetImageInfo(oat_index);
  auto* dst = reinterpret_cast<Object*>(image_info.image_->Begin() + offset);
  DCHECK_LT(offset, image_info.image_end_);
  const auto* src = reinterpret_cast<const uint8_t*>(obj);

  image_info.image_bitmap_->Set(dst);  // Mark the obj as live.

  const size_t n = obj->SizeOf();
  DCHECK_LE(offset + n, image_info.image_->Size());
  memcpy(dst, src, n);

  // Write in a hash code of objects which have inflated monitors or a hash code in their monitor
  // word.
  const auto it = saved_hashcode_map_.find(obj);
  dst->SetLockWord(it != saved_hashcode_map_.end() ?
      LockWord::FromHashCode(it->second, 0u) : LockWord::Default(), false);
  FixupObject(obj, dst);
}

// Rewrite all the references in the copied object to point to their image address equivalent
class FixupVisitor {
 public:
  FixupVisitor(ImageWriter* image_writer, Object* copy) : image_writer_(image_writer), copy_(copy) {
  }

  // Ignore class roots since we don't have a way to map them to the destination. These are handled
  // with other logic.
  void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root ATTRIBUTE_UNUSED)
      const {}
  void VisitRoot(mirror::CompressedReference<mirror::Object>* root ATTRIBUTE_UNUSED) const {}


  void operator()(Object* obj, MemberOffset offset, bool is_static ATTRIBUTE_UNUSED) const
      REQUIRES(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    Object* ref = obj->GetFieldObject<Object, kVerifyNone>(offset);
    // Use SetFieldObjectWithoutWriteBarrier to avoid card marking since we are writing to the
    // image.
    copy_->SetFieldObjectWithoutWriteBarrier<false, true, kVerifyNone>(
        offset,
        image_writer_->GetImageAddress(ref));
  }

  // java.lang.ref.Reference visitor.
  void operator()(mirror::Class* klass ATTRIBUTE_UNUSED, mirror::Reference* ref) const
      SHARED_REQUIRES(Locks::mutator_lock_) REQUIRES(Locks::heap_bitmap_lock_) {
    copy_->SetFieldObjectWithoutWriteBarrier<false, true, kVerifyNone>(
        mirror::Reference::ReferentOffset(),
        image_writer_->GetImageAddress(ref->GetReferent()));
  }

 protected:
  ImageWriter* const image_writer_;
  mirror::Object* const copy_;
};

class FixupClassVisitor FINAL : public FixupVisitor {
 public:
  FixupClassVisitor(ImageWriter* image_writer, Object* copy) : FixupVisitor(image_writer, copy) {
  }

  void operator()(Object* obj, MemberOffset offset, bool is_static ATTRIBUTE_UNUSED) const
      REQUIRES(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    DCHECK(obj->IsClass());
    FixupVisitor::operator()(obj, offset, /*is_static*/false);
  }

  void operator()(mirror::Class* klass ATTRIBUTE_UNUSED,
                  mirror::Reference* ref ATTRIBUTE_UNUSED) const
      SHARED_REQUIRES(Locks::mutator_lock_) REQUIRES(Locks::heap_bitmap_lock_) {
    LOG(FATAL) << "Reference not expected here.";
  }
};

uintptr_t ImageWriter::NativeOffsetInImage(void* obj) {
  DCHECK(obj != nullptr);
  DCHECK(!IsInBootImage(obj));
  auto it = native_object_relocations_.find(obj);
  CHECK(it != native_object_relocations_.end()) << obj << " spaces "
      << Runtime::Current()->GetHeap()->DumpSpaces();
  const NativeObjectRelocation& relocation = it->second;
  return relocation.offset;
}

template <typename T>
std::string PrettyPrint(T* ptr) SHARED_REQUIRES(Locks::mutator_lock_) {
  std::ostringstream oss;
  oss << ptr;
  return oss.str();
}

template <>
std::string PrettyPrint(ArtMethod* method) SHARED_REQUIRES(Locks::mutator_lock_) {
  return PrettyMethod(method);
}

template <typename T>
T* ImageWriter::NativeLocationInImage(T* obj) {
  if (obj == nullptr || IsInBootImage(obj)) {
    return obj;
  } else {
    auto it = native_object_relocations_.find(obj);
    CHECK(it != native_object_relocations_.end()) << obj << " " << PrettyPrint(obj)
        << " spaces " << Runtime::Current()->GetHeap()->DumpSpaces();
    const NativeObjectRelocation& relocation = it->second;
    ImageInfo& image_info = GetImageInfo(relocation.oat_index);
    return reinterpret_cast<T*>(image_info.image_begin_ + relocation.offset);
  }
}

template <typename T>
T* ImageWriter::NativeCopyLocation(T* obj, mirror::DexCache* dex_cache) {
  if (obj == nullptr || IsInBootImage(obj)) {
    return obj;
  } else {
    size_t oat_index = GetOatIndexForDexCache(dex_cache);
    ImageInfo& image_info = GetImageInfo(oat_index);
    return reinterpret_cast<T*>(image_info.image_->Begin() + NativeOffsetInImage(obj));
  }
}

class NativeLocationVisitor {
 public:
  explicit NativeLocationVisitor(ImageWriter* image_writer) : image_writer_(image_writer) {}

  template <typename T>
  T* operator()(T* ptr) const SHARED_REQUIRES(Locks::mutator_lock_) {
    return image_writer_->NativeLocationInImage(ptr);
  }

 private:
  ImageWriter* const image_writer_;
};

void ImageWriter::FixupClass(mirror::Class* orig, mirror::Class* copy) {
  orig->FixupNativePointers(copy, target_ptr_size_, NativeLocationVisitor(this));
  FixupClassVisitor visitor(this, copy);
  static_cast<mirror::Object*>(orig)->VisitReferences(visitor, visitor);

  // Remove the clinitThreadId. This is required for image determinism.
  copy->SetClinitThreadId(static_cast<pid_t>(0));
}

void ImageWriter::FixupObject(Object* orig, Object* copy) {
  DCHECK(orig != nullptr);
  DCHECK(copy != nullptr);
  if (kUseBakerOrBrooksReadBarrier) {
    orig->AssertReadBarrierPointer();
    if (kUseBrooksReadBarrier) {
      // Note the address 'copy' isn't the same as the image address of 'orig'.
      copy->SetReadBarrierPointer(GetImageAddress(orig));
      DCHECK_EQ(copy->GetReadBarrierPointer(), GetImageAddress(orig));
    }
  }
  auto* klass = orig->GetClass();
  if (klass->IsIntArrayClass() || klass->IsLongArrayClass()) {
    // Is this a native pointer array?
    auto it = pointer_arrays_.find(down_cast<mirror::PointerArray*>(orig));
    if (it != pointer_arrays_.end()) {
      // Should only need to fixup every pointer array exactly once.
      FixupPointerArray(copy, down_cast<mirror::PointerArray*>(orig), klass, it->second);
      pointer_arrays_.erase(it);
      return;
    }
  }
  if (orig->IsClass()) {
    FixupClass(orig->AsClass<kVerifyNone>(), down_cast<mirror::Class*>(copy));
  } else {
    if (klass == mirror::Method::StaticClass() || klass == mirror::Constructor::StaticClass()) {
      // Need to go update the ArtMethod.
      auto* dest = down_cast<mirror::AbstractMethod*>(copy);
      auto* src = down_cast<mirror::AbstractMethod*>(orig);
      ArtMethod* src_method = src->GetArtMethod();
      auto it = native_object_relocations_.find(src_method);
      CHECK(it != native_object_relocations_.end())
          << "Missing relocation for AbstractMethod.artMethod " << PrettyMethod(src_method);
      dest->SetArtMethod(
          reinterpret_cast<ArtMethod*>(global_image_begin_ + it->second.offset));
    } else if (!klass->IsArrayClass()) {
      ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
      if (klass == class_linker->GetClassRoot(ClassLinker::kJavaLangDexCache)) {
        FixupDexCache(down_cast<mirror::DexCache*>(orig), down_cast<mirror::DexCache*>(copy));
      } else if (klass->IsClassLoaderClass()) {
        mirror::ClassLoader* copy_loader = down_cast<mirror::ClassLoader*>(copy);
        // If src is a ClassLoader, set the class table to null so that it gets recreated by the
        // ClassLoader.
        copy_loader->SetClassTable(nullptr);
        // Also set allocator to null to be safe. The allocator is created when we create the class
        // table. We also never expect to unload things in the image since they are held live as
        // roots.
        copy_loader->SetAllocator(nullptr);
      }
    }
    FixupVisitor visitor(this, copy);
    orig->VisitReferences(visitor, visitor);
  }
}


class ImageAddressVisitor {
 public:
  explicit ImageAddressVisitor(ImageWriter* image_writer) : image_writer_(image_writer) {}

  template <typename T>
  T* operator()(T* ptr) const SHARED_REQUIRES(Locks::mutator_lock_) {
    return image_writer_->GetImageAddress(ptr);
  }

 private:
  ImageWriter* const image_writer_;
};


void ImageWriter::FixupDexCache(mirror::DexCache* orig_dex_cache,
                                mirror::DexCache* copy_dex_cache) {
  // Though the DexCache array fields are usually treated as native pointers, we set the full
  // 64-bit values here, clearing the top 32 bits for 32-bit targets. The zero-extension is
  // done by casting to the unsigned type uintptr_t before casting to int64_t, i.e.
  //     static_cast<int64_t>(reinterpret_cast<uintptr_t>(image_begin_ + offset))).
  GcRoot<mirror::String>* orig_strings = orig_dex_cache->GetStrings();
  if (orig_strings != nullptr) {
    copy_dex_cache->SetFieldPtrWithSize<false>(mirror::DexCache::StringsOffset(),
                                               NativeLocationInImage(orig_strings),
                                               /*pointer size*/8u);
    orig_dex_cache->FixupStrings(NativeCopyLocation(orig_strings, orig_dex_cache),
                                 ImageAddressVisitor(this));
  }
  GcRoot<mirror::Class>* orig_types = orig_dex_cache->GetResolvedTypes();
  if (orig_types != nullptr) {
    copy_dex_cache->SetFieldPtrWithSize<false>(mirror::DexCache::ResolvedTypesOffset(),
                                               NativeLocationInImage(orig_types),
                                               /*pointer size*/8u);
    orig_dex_cache->FixupResolvedTypes(NativeCopyLocation(orig_types, orig_dex_cache),
                                       ImageAddressVisitor(this));
  }
  ArtMethod** orig_methods = orig_dex_cache->GetResolvedMethods();
  if (orig_methods != nullptr) {
    copy_dex_cache->SetFieldPtrWithSize<false>(mirror::DexCache::ResolvedMethodsOffset(),
                                               NativeLocationInImage(orig_methods),
                                               /*pointer size*/8u);
    ArtMethod** copy_methods = NativeCopyLocation(orig_methods, orig_dex_cache);
    for (size_t i = 0, num = orig_dex_cache->NumResolvedMethods(); i != num; ++i) {
      ArtMethod* orig = mirror::DexCache::GetElementPtrSize(orig_methods, i, target_ptr_size_);
      // NativeLocationInImage also handles runtime methods since these have relocation info.
      ArtMethod* copy = NativeLocationInImage(orig);
      mirror::DexCache::SetElementPtrSize(copy_methods, i, copy, target_ptr_size_);
    }
  }
  ArtField** orig_fields = orig_dex_cache->GetResolvedFields();
  if (orig_fields != nullptr) {
    copy_dex_cache->SetFieldPtrWithSize<false>(mirror::DexCache::ResolvedFieldsOffset(),
                                               NativeLocationInImage(orig_fields),
                                               /*pointer size*/8u);
    ArtField** copy_fields = NativeCopyLocation(orig_fields, orig_dex_cache);
    for (size_t i = 0, num = orig_dex_cache->NumResolvedFields(); i != num; ++i) {
      ArtField* orig = mirror::DexCache::GetElementPtrSize(orig_fields, i, target_ptr_size_);
      ArtField* copy = NativeLocationInImage(orig);
      mirror::DexCache::SetElementPtrSize(copy_fields, i, copy, target_ptr_size_);
    }
  }

  // Remove the DexFile pointers. They will be fixed up when the runtime loads the oat file. Leaving
  // compiler pointers in here will make the output non-deterministic.
  copy_dex_cache->SetDexFile(nullptr);
}

const uint8_t* ImageWriter::GetOatAddress(OatAddress type) const {
  DCHECK_LT(type, kOatAddressCount);
  // If we are compiling an app image, we need to use the stubs of the boot image.
  if (compile_app_image_) {
    // Use the current image pointers.
    const std::vector<gc::space::ImageSpace*>& image_spaces =
        Runtime::Current()->GetHeap()->GetBootImageSpaces();
    DCHECK(!image_spaces.empty());
    const OatFile* oat_file = image_spaces[0]->GetOatFile();
    CHECK(oat_file != nullptr);
    const OatHeader& header = oat_file->GetOatHeader();
    switch (type) {
      // TODO: We could maybe clean this up if we stored them in an array in the oat header.
      case kOatAddressQuickGenericJNITrampoline:
        return static_cast<const uint8_t*>(header.GetQuickGenericJniTrampoline());
      case kOatAddressInterpreterToInterpreterBridge:
        return static_cast<const uint8_t*>(header.GetInterpreterToInterpreterBridge());
      case kOatAddressInterpreterToCompiledCodeBridge:
        return static_cast<const uint8_t*>(header.GetInterpreterToCompiledCodeBridge());
      case kOatAddressJNIDlsymLookup:
        return static_cast<const uint8_t*>(header.GetJniDlsymLookup());
      case kOatAddressQuickIMTConflictTrampoline:
        return static_cast<const uint8_t*>(header.GetQuickImtConflictTrampoline());
      case kOatAddressQuickResolutionTrampoline:
        return static_cast<const uint8_t*>(header.GetQuickResolutionTrampoline());
      case kOatAddressQuickToInterpreterBridge:
        return static_cast<const uint8_t*>(header.GetQuickToInterpreterBridge());
      default:
        UNREACHABLE();
    }
  }
  const ImageInfo& primary_image_info = GetImageInfo(0);
  return GetOatAddressForOffset(primary_image_info.oat_address_offsets_[type], primary_image_info);
}

const uint8_t* ImageWriter::GetQuickCode(ArtMethod* method,
                                         const ImageInfo& image_info,
                                         bool* quick_is_interpreted) {
  DCHECK(!method->IsResolutionMethod()) << PrettyMethod(method);
  DCHECK_NE(method, Runtime::Current()->GetImtConflictMethod()) << PrettyMethod(method);
  DCHECK(!method->IsImtUnimplementedMethod()) << PrettyMethod(method);
  DCHECK(method->IsInvokable()) << PrettyMethod(method);
  DCHECK(!IsInBootImage(method)) << PrettyMethod(method);

  // Use original code if it exists. Otherwise, set the code pointer to the resolution
  // trampoline.

  // Quick entrypoint:
  const void* quick_oat_entry_point =
      method->GetEntryPointFromQuickCompiledCodePtrSize(target_ptr_size_);
  const uint8_t* quick_code;

  if (UNLIKELY(IsInBootImage(method->GetDeclaringClass()))) {
    DCHECK(method->IsCopied());
    // If the code is not in the oat file corresponding to this image (e.g. default methods)
    quick_code = reinterpret_cast<const uint8_t*>(quick_oat_entry_point);
  } else {
    uint32_t quick_oat_code_offset = PointerToLowMemUInt32(quick_oat_entry_point);
    quick_code = GetOatAddressForOffset(quick_oat_code_offset, image_info);
  }

  *quick_is_interpreted = false;
  if (quick_code != nullptr && (!method->IsStatic() || method->IsConstructor() ||
      method->GetDeclaringClass()->IsInitialized())) {
    // We have code for a non-static or initialized method, just use the code.
  } else if (quick_code == nullptr && method->IsNative() &&
      (!method->IsStatic() || method->GetDeclaringClass()->IsInitialized())) {
    // Non-static or initialized native method missing compiled code, use generic JNI version.
    quick_code = GetOatAddress(kOatAddressQuickGenericJNITrampoline);
  } else if (quick_code == nullptr && !method->IsNative()) {
    // We don't have code at all for a non-native method, use the interpreter.
    quick_code = GetOatAddress(kOatAddressQuickToInterpreterBridge);
    *quick_is_interpreted = true;
  } else {
    CHECK(!method->GetDeclaringClass()->IsInitialized());
    // We have code for a static method, but need to go through the resolution stub for class
    // initialization.
    quick_code = GetOatAddress(kOatAddressQuickResolutionTrampoline);
  }
  if (!IsInBootOatFile(quick_code)) {
    // DCHECK_GE(quick_code, oat_data_begin_);
  }
  return quick_code;
}

void ImageWriter::CopyAndFixupMethod(ArtMethod* orig,
                                     ArtMethod* copy,
                                     const ImageInfo& image_info) {
  memcpy(copy, orig, ArtMethod::Size(target_ptr_size_));

  copy->SetDeclaringClass(GetImageAddress(orig->GetDeclaringClassUnchecked()));
  ArtMethod** orig_resolved_methods = orig->GetDexCacheResolvedMethods(target_ptr_size_);
  copy->SetDexCacheResolvedMethods(NativeLocationInImage(orig_resolved_methods), target_ptr_size_);
  GcRoot<mirror::Class>* orig_resolved_types = orig->GetDexCacheResolvedTypes(target_ptr_size_);
  copy->SetDexCacheResolvedTypes(NativeLocationInImage(orig_resolved_types), target_ptr_size_);

  // OatWriter replaces the code_ with an offset value. Here we re-adjust to a pointer relative to
  // oat_begin_

  // The resolution method has a special trampoline to call.
  Runtime* runtime = Runtime::Current();
  if (orig->IsRuntimeMethod()) {
    ImtConflictTable* orig_table = orig->GetImtConflictTable(target_ptr_size_);
    if (orig_table != nullptr) {
      // Special IMT conflict method, normal IMT conflict method or unimplemented IMT method.
      copy->SetEntryPointFromQuickCompiledCodePtrSize(
          GetOatAddress(kOatAddressQuickIMTConflictTrampoline), target_ptr_size_);
      copy->SetImtConflictTable(NativeLocationInImage(orig_table), target_ptr_size_);
    } else if (UNLIKELY(orig == runtime->GetResolutionMethod())) {
      copy->SetEntryPointFromQuickCompiledCodePtrSize(
          GetOatAddress(kOatAddressQuickResolutionTrampoline), target_ptr_size_);
    } else {
      bool found_one = false;
      for (size_t i = 0; i < static_cast<size_t>(Runtime::kLastCalleeSaveType); ++i) {
        auto idx = static_cast<Runtime::CalleeSaveType>(i);
        if (runtime->HasCalleeSaveMethod(idx) && runtime->GetCalleeSaveMethod(idx) == orig) {
          found_one = true;
          break;
        }
      }
      CHECK(found_one) << "Expected to find callee save method but got " << PrettyMethod(orig);
      CHECK(copy->IsRuntimeMethod());
    }
  } else {
    // We assume all methods have code. If they don't currently then we set them to the use the
    // resolution trampoline. Abstract methods never have code and so we need to make sure their
    // use results in an AbstractMethodError. We use the interpreter to achieve this.
    if (UNLIKELY(!orig->IsInvokable())) {
      copy->SetEntryPointFromQuickCompiledCodePtrSize(
          GetOatAddress(kOatAddressQuickToInterpreterBridge), target_ptr_size_);
    } else {
      bool quick_is_interpreted;
      const uint8_t* quick_code = GetQuickCode(orig, image_info, &quick_is_interpreted);
      copy->SetEntryPointFromQuickCompiledCodePtrSize(quick_code, target_ptr_size_);

      // JNI entrypoint:
      if (orig->IsNative()) {
        // The native method's pointer is set to a stub to lookup via dlsym.
        // Note this is not the code_ pointer, that is handled above.
        copy->SetEntryPointFromJniPtrSize(
            GetOatAddress(kOatAddressJNIDlsymLookup), target_ptr_size_);
      }
    }
  }
}

size_t ImageWriter::GetBinSizeSum(ImageWriter::ImageInfo& image_info, ImageWriter::Bin up_to) const {
  DCHECK_LE(up_to, kBinSize);
  return std::accumulate(&image_info.bin_slot_sizes_[0],
                         &image_info.bin_slot_sizes_[up_to],
                         /*init*/0);
}

ImageWriter::BinSlot::BinSlot(uint32_t lockword) : lockword_(lockword) {
  // These values may need to get updated if more bins are added to the enum Bin
  static_assert(kBinBits == 3, "wrong number of bin bits");
  static_assert(kBinShift == 27, "wrong number of shift");
  static_assert(sizeof(BinSlot) == sizeof(LockWord), "BinSlot/LockWord must have equal sizes");

  DCHECK_LT(GetBin(), kBinSize);
  DCHECK_ALIGNED(GetIndex(), kObjectAlignment);
}

ImageWriter::BinSlot::BinSlot(Bin bin, uint32_t index)
    : BinSlot(index | (static_cast<uint32_t>(bin) << kBinShift)) {
  DCHECK_EQ(index, GetIndex());
}

ImageWriter::Bin ImageWriter::BinSlot::GetBin() const {
  return static_cast<Bin>((lockword_ & kBinMask) >> kBinShift);
}

uint32_t ImageWriter::BinSlot::GetIndex() const {
  return lockword_ & ~kBinMask;
}

ImageWriter::Bin ImageWriter::BinTypeForNativeRelocationType(NativeObjectRelocationType type) {
  switch (type) {
    case kNativeObjectRelocationTypeArtField:
    case kNativeObjectRelocationTypeArtFieldArray:
      return kBinArtField;
    case kNativeObjectRelocationTypeArtMethodClean:
    case kNativeObjectRelocationTypeArtMethodArrayClean:
      return kBinArtMethodClean;
    case kNativeObjectRelocationTypeArtMethodDirty:
    case kNativeObjectRelocationTypeArtMethodArrayDirty:
      return kBinArtMethodDirty;
    case kNativeObjectRelocationTypeDexCacheArray:
      return kBinDexCacheArray;
    case kNativeObjectRelocationTypeRuntimeMethod:
      return kBinRuntimeMethod;
    case kNativeObjectRelocationTypeIMTable:
      return kBinImTable;
    case kNativeObjectRelocationTypeIMTConflictTable:
      return kBinIMTConflictTable;
  }
  UNREACHABLE();
}

size_t ImageWriter::GetOatIndex(mirror::Object* obj) const {
  if (!IsMultiImage()) {
    return GetDefaultOatIndex();
  }
  auto it = oat_index_map_.find(obj);
  DCHECK(it != oat_index_map_.end());
  return it->second;
}

size_t ImageWriter::GetOatIndexForDexFile(const DexFile* dex_file) const {
  if (!IsMultiImage()) {
    return GetDefaultOatIndex();
  }
  auto it = dex_file_oat_index_map_.find(dex_file);
  DCHECK(it != dex_file_oat_index_map_.end()) << dex_file->GetLocation();
  return it->second;
}

size_t ImageWriter::GetOatIndexForDexCache(mirror::DexCache* dex_cache) const {
  if (dex_cache == nullptr) {
    return GetDefaultOatIndex();
  } else {
    return GetOatIndexForDexFile(dex_cache->GetDexFile());
  }
}

void ImageWriter::UpdateOatFileLayout(size_t oat_index,
                                      size_t oat_loaded_size,
                                      size_t oat_data_offset,
                                      size_t oat_data_size) {
  const uint8_t* images_end = image_infos_.back().image_begin_ + image_infos_.back().image_size_;
  for (const ImageInfo& info : image_infos_) {
    DCHECK_LE(info.image_begin_ + info.image_size_, images_end);
  }
  DCHECK(images_end != nullptr);  // Image space must be ready.

  ImageInfo& cur_image_info = GetImageInfo(oat_index);
  cur_image_info.oat_file_begin_ = images_end + cur_image_info.oat_offset_;
  cur_image_info.oat_loaded_size_ = oat_loaded_size;
  cur_image_info.oat_data_begin_ = cur_image_info.oat_file_begin_ + oat_data_offset;
  cur_image_info.oat_size_ = oat_data_size;

  if (compile_app_image_) {
    CHECK_EQ(oat_filenames_.size(), 1u) << "App image should have no next image.";
    return;
  }

  // Update the oat_offset of the next image info.
  if (oat_index + 1u != oat_filenames_.size()) {
    // There is a following one.
    ImageInfo& next_image_info = GetImageInfo(oat_index + 1u);
    next_image_info.oat_offset_ = cur_image_info.oat_offset_ + oat_loaded_size;
  }
}

void ImageWriter::UpdateOatFileHeader(size_t oat_index, const OatHeader& oat_header) {
  ImageInfo& cur_image_info = GetImageInfo(oat_index);
  cur_image_info.oat_checksum_ = oat_header.GetChecksum();

  if (oat_index == GetDefaultOatIndex()) {
    // Primary oat file, read the trampolines.
    cur_image_info.oat_address_offsets_[kOatAddressInterpreterToInterpreterBridge] =
        oat_header.GetInterpreterToInterpreterBridgeOffset();
    cur_image_info.oat_address_offsets_[kOatAddressInterpreterToCompiledCodeBridge] =
        oat_header.GetInterpreterToCompiledCodeBridgeOffset();
    cur_image_info.oat_address_offsets_[kOatAddressJNIDlsymLookup] =
        oat_header.GetJniDlsymLookupOffset();
    cur_image_info.oat_address_offsets_[kOatAddressQuickGenericJNITrampoline] =
        oat_header.GetQuickGenericJniTrampolineOffset();
    cur_image_info.oat_address_offsets_[kOatAddressQuickIMTConflictTrampoline] =
        oat_header.GetQuickImtConflictTrampolineOffset();
    cur_image_info.oat_address_offsets_[kOatAddressQuickResolutionTrampoline] =
        oat_header.GetQuickResolutionTrampolineOffset();
    cur_image_info.oat_address_offsets_[kOatAddressQuickToInterpreterBridge] =
        oat_header.GetQuickToInterpreterBridgeOffset();
  }
}

ImageWriter::ImageWriter(
    const CompilerDriver& compiler_driver,
    uintptr_t image_begin,
    bool compile_pic,
    bool compile_app_image,
    ImageHeader::StorageMode image_storage_mode,
    const std::vector<const char*>& oat_filenames,
    const std::unordered_map<const DexFile*, size_t>& dex_file_oat_index_map)
    : compiler_driver_(compiler_driver),
      global_image_begin_(reinterpret_cast<uint8_t*>(image_begin)),
      image_objects_offset_begin_(0),
      compile_pic_(compile_pic),
      compile_app_image_(compile_app_image),
      target_ptr_size_(InstructionSetPointerSize(compiler_driver_.GetInstructionSet())),
      image_infos_(oat_filenames.size()),
      dirty_methods_(0u),
      clean_methods_(0u),
      image_storage_mode_(image_storage_mode),
      oat_filenames_(oat_filenames),
      dex_file_oat_index_map_(dex_file_oat_index_map) {
  CHECK_NE(image_begin, 0U);
  std::fill_n(image_methods_, arraysize(image_methods_), nullptr);
  CHECK_EQ(compile_app_image, !Runtime::Current()->GetHeap()->GetBootImageSpaces().empty())
      << "Compiling a boot image should occur iff there are no boot image spaces loaded";
}

ImageWriter::ImageInfo::ImageInfo()
    : intern_table_(new InternTable),
      class_table_(new ClassTable) {}

}  // namespace art
