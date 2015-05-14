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

#include <memory>
#include <numeric>
#include <vector>

#include "art_field-inl.h"
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
#include "mirror/art_method-inl.h"
#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-inl.h"
#include "oat.h"
#include "oat_file.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "handle_scope-inl.h"
#include "utils/dex_cache_arrays_layout-inl.h"

using ::art::mirror::ArtMethod;
using ::art::mirror::Class;
using ::art::mirror::DexCache;
using ::art::mirror::EntryPointFromInterpreter;
using ::art::mirror::Object;
using ::art::mirror::ObjectArray;
using ::art::mirror::String;

namespace art {

// Separate objects into multiple bins to optimize dirty memory use.
static constexpr bool kBinObjects = true;
static constexpr bool kComputeEagerResolvedStrings = false;

static void CheckNoDexObjectsCallback(Object* obj, void* arg ATTRIBUTE_UNUSED)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Class* klass = obj->GetClass();
  CHECK_NE(PrettyClass(klass), "com.android.dex.Dex");
}

static void CheckNoDexObjects() {
  ScopedObjectAccess soa(Thread::Current());
  Runtime::Current()->GetHeap()->VisitObjects(CheckNoDexObjectsCallback, nullptr);
}

bool ImageWriter::PrepareImageAddressSpace() {
  target_ptr_size_ = InstructionSetPointerSize(compiler_driver_.GetInstructionSet());
  {
    Thread::Current()->TransitionFromSuspendedToRunnable();
    PruneNonImageClasses();  // Remove junk
    ComputeLazyFieldsForImageClasses();  // Add useful information

    // Calling this can in theory fill in some resolved strings. However, in practice it seems to
    // never resolve any.
    if (kComputeEagerResolvedStrings) {
      ComputeEagerResolvedStrings();
    }
    Thread::Current()->TransitionFromRunnableToSuspended(kNative);
  }
  gc::Heap* heap = Runtime::Current()->GetHeap();
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

  if (!AllocMemory()) {
    return false;
  }

  if (kIsDebugBuild) {
    ScopedObjectAccess soa(Thread::Current());
    CheckNonImageClassesRemoved();
  }

  Thread::Current()->TransitionFromSuspendedToRunnable();
  CalculateNewObjectOffsets();
  Thread::Current()->TransitionFromRunnableToSuspended(kNative);

  return true;
}

bool ImageWriter::Write(const std::string& image_filename,
                        const std::string& oat_filename,
                        const std::string& oat_location) {
  CHECK(!image_filename.empty());

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  std::unique_ptr<File> oat_file(OS::OpenFileReadWrite(oat_filename.c_str()));
  if (oat_file.get() == nullptr) {
    PLOG(ERROR) << "Failed to open oat file " << oat_filename << " for " << oat_location;
    return false;
  }
  std::string error_msg;
  oat_file_ = OatFile::OpenReadable(oat_file.get(), oat_location, nullptr, &error_msg);
  if (oat_file_ == nullptr) {
    PLOG(ERROR) << "Failed to open writable oat file " << oat_filename << " for " << oat_location
        << ": " << error_msg;
    oat_file->Erase();
    return false;
  }
  CHECK_EQ(class_linker->RegisterOatFile(oat_file_), oat_file_);

  interpreter_to_interpreter_bridge_offset_ =
      oat_file_->GetOatHeader().GetInterpreterToInterpreterBridgeOffset();
  interpreter_to_compiled_code_bridge_offset_ =
      oat_file_->GetOatHeader().GetInterpreterToCompiledCodeBridgeOffset();

  jni_dlsym_lookup_offset_ = oat_file_->GetOatHeader().GetJniDlsymLookupOffset();

  quick_generic_jni_trampoline_offset_ =
      oat_file_->GetOatHeader().GetQuickGenericJniTrampolineOffset();
  quick_imt_conflict_trampoline_offset_ =
      oat_file_->GetOatHeader().GetQuickImtConflictTrampolineOffset();
  quick_resolution_trampoline_offset_ =
      oat_file_->GetOatHeader().GetQuickResolutionTrampolineOffset();
  quick_to_interpreter_bridge_offset_ =
      oat_file_->GetOatHeader().GetQuickToInterpreterBridgeOffset();

  size_t oat_loaded_size = 0;
  size_t oat_data_offset = 0;
  ElfWriter::GetOatElfInformation(oat_file.get(), &oat_loaded_size, &oat_data_offset);

  Thread::Current()->TransitionFromSuspendedToRunnable();
  CreateHeader(oat_loaded_size, oat_data_offset);
  // TODO: heap validation can't handle these fix up passes.
  Runtime::Current()->GetHeap()->DisableObjectValidation();
  CopyAndFixupNativeData();
  CopyAndFixupObjects();
  Thread::Current()->TransitionFromRunnableToSuspended(kNative);

  SetOatChecksumFromElfFile(oat_file.get());

  if (oat_file->FlushCloseOrErase() != 0) {
    LOG(ERROR) << "Failed to flush and close oat file " << oat_filename << " for " << oat_location;
    return false;
  }

  std::unique_ptr<File> image_file(OS::CreateEmptyFile(image_filename.c_str()));
  ImageHeader* image_header = reinterpret_cast<ImageHeader*>(image_->Begin());
  if (image_file.get() == nullptr) {
    LOG(ERROR) << "Failed to open image file " << image_filename;
    return false;
  }
  if (fchmod(image_file->Fd(), 0644) != 0) {
    PLOG(ERROR) << "Failed to make image file world readable: " << image_filename;
    image_file->Erase();
    return EXIT_FAILURE;
  }

  // Write out the image + fields.
  const auto write_count = image_header->GetImageSize() + image_header->GetArtFieldsSize();
  CHECK_EQ(image_end_, image_header->GetImageSize());
  if (!image_file->WriteFully(image_->Begin(), write_count)) {
    PLOG(ERROR) << "Failed to write image file " << image_filename;
    image_file->Erase();
    return false;
  }

  // Write out the image bitmap at the page aligned start of the image end.
  CHECK_ALIGNED(image_header->GetImageBitmapOffset(), kPageSize);
  if (!image_file->Write(reinterpret_cast<char*>(image_bitmap_->Begin()),
                         image_header->GetImageBitmapSize(),
                         image_header->GetImageBitmapOffset())) {
    PLOG(ERROR) << "Failed to write image file " << image_filename;
    image_file->Erase();
    return false;
  }

  CHECK_EQ(image_header->GetImageBitmapOffset() + image_header->GetImageBitmapSize(),
           static_cast<size_t>(image_file->GetLength()));
  if (image_file->FlushCloseOrErase() != 0) {
    PLOG(ERROR) << "Failed to flush and close image file " << image_filename;
    return false;
  }
  return true;
}

void ImageWriter::SetImageOffset(mirror::Object* object,
                                 ImageWriter::BinSlot bin_slot,
                                 size_t offset) {
  DCHECK(object != nullptr);
  DCHECK_NE(offset, 0U);
  mirror::Object* obj = reinterpret_cast<mirror::Object*>(image_->Begin() + offset);
  DCHECK_ALIGNED(obj, kObjectAlignment);

  static size_t max_offset = 0;
  max_offset = std::max(max_offset, offset);
  image_bitmap_->Set(obj);  // Mark the obj as mutated, since we will end up changing it.
  {
    // Remember the object-inside-of-the-image's hash code so we can restore it after the copy.
    auto hash_it = saved_hashes_map_.find(bin_slot);
    if (hash_it != saved_hashes_map_.end()) {
      std::pair<BinSlot, uint32_t> slot_hash = *hash_it;
      saved_hashes_.push_back(std::make_pair(obj, slot_hash.second));
      saved_hashes_map_.erase(hash_it);
    }
  }
  // The object is already deflated from when we set the bin slot. Just overwrite the lock word.
  object->SetLockWord(LockWord::FromForwardingAddress(offset), false);
  DCHECK(IsImageOffsetAssigned(object));
}

void ImageWriter::AssignImageOffset(mirror::Object* object, ImageWriter::BinSlot bin_slot) {
  DCHECK(object != nullptr);
  DCHECK_NE(image_objects_offset_begin_, 0u);

  size_t previous_bin_sizes = bin_slot_previous_sizes_[bin_slot.GetBin()];
  size_t new_offset = image_objects_offset_begin_ + previous_bin_sizes + bin_slot.GetIndex();
  DCHECK_ALIGNED(new_offset, kObjectAlignment);

  SetImageOffset(object, bin_slot, new_offset);
  DCHECK_LT(new_offset, image_end_);
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
  DCHECK_LT(offset, image_end_);
  return offset;
}

void ImageWriter::SetImageBinSlot(mirror::Object* object, BinSlot bin_slot) {
  DCHECK(object != nullptr);
  DCHECK(!IsImageOffsetAssigned(object));
  DCHECK(!IsImageBinSlotAssigned(object));

  // Before we stomp over the lock word, save the hash code for later.
  Monitor::Deflate(Thread::Current(), object);;
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
      saved_hashes_map_[bin_slot] = lw.GetHashCode();
      break;
    default:
      LOG(FATAL) << "Unreachable.";
      UNREACHABLE();
  }
  object->SetLockWord(LockWord::FromForwardingAddress(static_cast<uint32_t>(bin_slot)),
                      false);
  DCHECK(IsImageBinSlotAssigned(object));
}

void ImageWriter::PrepareDexCacheArraySlots() {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ReaderMutexLock mu(Thread::Current(), *class_linker->DexLock());
  size_t dex_cache_count = class_linker->GetDexCacheCount();
  uint32_t size = 0u;
  for (size_t idx = 0; idx < dex_cache_count; ++idx) {
    DexCache* dex_cache = class_linker->GetDexCache(idx);
    const DexFile* dex_file = dex_cache->GetDexFile();
    dex_cache_array_starts_.Put(dex_file, size);
    DexCacheArraysLayout layout(target_ptr_size_, dex_file);
    DCHECK(layout.Valid());
    auto types_size = layout.TypesSize(dex_file->NumTypeIds());
    auto methods_size = layout.MethodsSize(dex_file->NumMethodIds());
    auto fields_size = layout.FieldsSize(dex_file->NumFieldIds());
    auto strings_size = layout.StringsSize(dex_file->NumStringIds());
    dex_cache_array_indexes_.Put(
        dex_cache->GetResolvedTypes(),
        DexCacheArrayLocation {size + layout.TypesOffset(), types_size});
    dex_cache_array_indexes_.Put(
        dex_cache->GetResolvedMethods(),
        DexCacheArrayLocation {size + layout.MethodsOffset(), methods_size});
    dex_cache_array_indexes_.Put(
        dex_cache->GetResolvedFields(),
        DexCacheArrayLocation {size + layout.FieldsOffset(), fields_size});
    dex_cache_array_indexes_.Put(
        dex_cache->GetStrings(),
        DexCacheArrayLocation {size + layout.StringsOffset(), strings_size});
    size += layout.Size();
    CHECK_EQ(layout.Size(), types_size + methods_size + fields_size + strings_size);
  }
  // Set the slot size early to avoid DCHECK() failures in IsImageBinSlotAssigned()
  // when AssignImageBinSlot() assigns their indexes out or order.
  bin_slot_sizes_[kBinDexCacheArray] = size;
}

void ImageWriter::AssignImageBinSlot(mirror::Object* object) {
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
    //   special PC-relative addressing, we arbitrarily keep them at the beginning.
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
    } else if (object->IsArtMethod<kVerifyNone>()) {
      mirror::ArtMethod* art_method = down_cast<ArtMethod*>(object);
      if (art_method->IsNative()) {
        bin = kBinArtMethodNative;
      } else {
        mirror::Class* declaring_class = art_method->GetDeclaringClass();
        if (declaring_class->GetStatus() != Class::kStatusInitialized) {
          bin = kBinArtMethodNotInitialized;
        } else {
          // This is highly unlikely to dirty since there's no entry points to mutate.
          bin = kBinArtMethodsManagedInitialized;
        }
      }
    } else if (object->GetClass<kVerifyNone>()->IsStringClass()) {
      bin = kBinString;  // Strings are almost always immutable (except for object header).
    } else if (object->IsArrayInstance()) {
      mirror::Class* klass = object->GetClass<kVerifyNone>();
      auto* component_type = klass->GetComponentType();
      if (!component_type->IsPrimitive() || component_type->IsPrimitiveInt() ||
          component_type->IsPrimitiveLong()) {
        auto it = dex_cache_array_indexes_.find(object);
        if (it != dex_cache_array_indexes_.end()) {
          bin = kBinDexCacheArray;
          // Use prepared offset defined by the DexCacheLayout.
          current_offset = it->second.offset_;
          // Override incase of cross compilation.
          object_size = it->second.length_;
        }  // else bin = kBinRegular
      }
    }  // else bin = kBinRegular
  }

  size_t offset_delta = RoundUp(object_size, kObjectAlignment);  // 64-bit alignment
  if (bin != kBinDexCacheArray) {
    current_offset = bin_slot_sizes_[bin];  // How many bytes the current bin is at (aligned).
    // Move the current bin size up to accomodate the object we just assigned a bin slot.
    bin_slot_sizes_[bin] += offset_delta;
  }

  BinSlot new_bin_slot(bin, current_offset);
  SetImageBinSlot(object, new_bin_slot);

  ++bin_slot_count_[bin];

  DCHECK_LT(GetBinSizeSum(), image_->Size());

  // Grow the image closer to the end by the object we just assigned.
  image_end_ += offset_delta;
  DCHECK_LT(image_end_, image_->Size());
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
    DCHECK_LT(bin_slot.GetIndex(), bin_slot_sizes_[bin_slot.GetBin()])
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
  DCHECK_LT(bin_slot.GetIndex(), bin_slot_sizes_[bin_slot.GetBin()]);

  return bin_slot;
}

bool ImageWriter::AllocMemory() {
  auto* runtime = Runtime::Current();
  const size_t heap_size = runtime->GetHeap()->GetTotalMemory();
  // Add linear alloc usage since we need to have room for the ArtFields.
  const size_t length = RoundUp(heap_size + runtime->GetLinearAlloc()->GetUsedMemory(), kPageSize);
  std::string error_msg;
  image_.reset(MemMap::MapAnonymous("image writer image", nullptr, length, PROT_READ | PROT_WRITE,
                                    false, false, &error_msg));
  if (UNLIKELY(image_.get() == nullptr)) {
    LOG(ERROR) << "Failed to allocate memory for image file generation: " << error_msg;
    return false;
  }

  // Create the image bitmap.
  image_bitmap_.reset(gc::accounting::ContinuousSpaceBitmap::Create("image bitmap", image_->Begin(),
                                                                    RoundUp(length, kPageSize)));
  if (image_bitmap_.get() == nullptr) {
    LOG(ERROR) << "Failed to allocate memory for image bitmap";
    return false;
  }
  return true;
}

void ImageWriter::ComputeLazyFieldsForImageClasses() {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  class_linker->VisitClassesWithoutClassesLock(ComputeLazyFieldsForClassesVisitor, nullptr);
}

bool ImageWriter::ComputeLazyFieldsForClassesVisitor(Class* c, void* /*arg*/) {
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  mirror::Class::ComputeName(hs.NewHandle(c));
  return true;
}

// Collect all the java.lang.String in the heap and put them in the output strings_ array.
class StringCollector {
 public:
  StringCollector(Handle<mirror::ObjectArray<mirror::String>> strings, size_t index)
      : strings_(strings), index_(index) {
  }
  static void Callback(Object* obj, void* arg) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    auto* collector = reinterpret_cast<StringCollector*>(arg);
    if (obj->GetClass()->IsStringClass()) {
      collector->strings_->SetWithoutChecks<false>(collector->index_++, obj->AsString());
    }
  }
  size_t GetIndex() const {
    return index_;
  }

 private:
  Handle<mirror::ObjectArray<mirror::String>> strings_;
  size_t index_;
};

// Compare strings based on length, used for sorting strings by length / reverse length.
class LexicographicalStringComparator {
 public:
  bool operator()(const mirror::HeapReference<mirror::String>& lhs,
                  const mirror::HeapReference<mirror::String>& rhs) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::String* lhs_s = lhs.AsMirrorPtr();
    mirror::String* rhs_s = rhs.AsMirrorPtr();
    uint16_t* lhs_begin = lhs_s->GetValue();
    uint16_t* rhs_begin = rhs_s->GetValue();
    return std::lexicographical_compare(lhs_begin, lhs_begin + lhs_s->GetLength(),
                                        rhs_begin, rhs_begin + rhs_s->GetLength());
  }
};

void ImageWriter::ComputeEagerResolvedStringsCallback(Object* obj, void* arg ATTRIBUTE_UNUSED) {
  if (!obj->GetClass()->IsStringClass()) {
    return;
  }
  mirror::String* string = obj->AsString();
  const uint16_t* utf16_string = string->GetValue();
  size_t utf16_length = static_cast<size_t>(string->GetLength());
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ReaderMutexLock mu(Thread::Current(), *class_linker->DexLock());
  size_t dex_cache_count = class_linker->GetDexCacheCount();
  for (size_t i = 0; i < dex_cache_count; ++i) {
    DexCache* dex_cache = class_linker->GetDexCache(i);
    const DexFile& dex_file = *dex_cache->GetDexFile();
    const DexFile::StringId* string_id;
    if (UNLIKELY(utf16_length == 0)) {
      string_id = dex_file.FindStringId("");
    } else {
      string_id = dex_file.FindStringId(utf16_string, utf16_length);
    }
    if (string_id != nullptr) {
      // This string occurs in this dex file, assign the dex cache entry.
      uint32_t string_idx = dex_file.GetIndexForStringId(*string_id);
      if (dex_cache->GetResolvedString(string_idx) == nullptr) {
        dex_cache->SetResolvedString(string_idx, string);
      }
    }
  }
}

void ImageWriter::ComputeEagerResolvedStrings() {
  Runtime::Current()->GetHeap()->VisitObjects(ComputeEagerResolvedStringsCallback, this);
}

bool ImageWriter::IsImageClass(Class* klass) {
  std::string temp;
  return compiler_driver_.IsImageClass(klass->GetDescriptor(&temp));
}

struct NonImageClasses {
  ImageWriter* image_writer;
  std::set<std::string>* non_image_classes;
};

void ImageWriter::PruneNonImageClasses() {
  if (compiler_driver_.GetImageClasses() == nullptr) {
    return;
  }
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();

  // Make a list of classes we would like to prune.
  std::set<std::string> non_image_classes;
  NonImageClasses context;
  context.image_writer = this;
  context.non_image_classes = &non_image_classes;
  class_linker->VisitClasses(NonImageClassesVisitor, &context);

  // Remove the undesired classes from the class roots.
  for (const std::string& it : non_image_classes) {
    bool result = class_linker->RemoveClass(it.c_str(), nullptr);
    DCHECK(result);
  }

  // Clear references to removed classes from the DexCaches.
  ArtMethod* resolution_method = runtime->GetResolutionMethod();
  ReaderMutexLock mu(Thread::Current(), *class_linker->DexLock());
  size_t dex_cache_count = class_linker->GetDexCacheCount();
  for (size_t idx = 0; idx < dex_cache_count; ++idx) {
    DexCache* dex_cache = class_linker->GetDexCache(idx);
    for (size_t i = 0; i < dex_cache->NumResolvedTypes(); i++) {
      Class* klass = dex_cache->GetResolvedType(i);
      if (klass != nullptr && !IsImageClass(klass)) {
        dex_cache->SetResolvedType(i, nullptr);
      }
    }
    for (size_t i = 0; i < dex_cache->NumResolvedMethods(); i++) {
      ArtMethod* method = dex_cache->GetResolvedMethod(i);
      if (method != nullptr && !IsImageClass(method->GetDeclaringClass())) {
        dex_cache->SetResolvedMethod(i, resolution_method);
      }
    }
    for (size_t i = 0; i < dex_cache->NumResolvedFields(); i++) {
      ArtField* field = dex_cache->GetResolvedField(i, sizeof(void*));
      if (field != nullptr && !IsImageClass(field->GetDeclaringClass())) {
        dex_cache->SetResolvedField(i, nullptr, sizeof(void*));
      }
    }
    // Clean the dex field. It might have been populated during the initialization phase, but
    // contains data only valid during a real run.
    dex_cache->SetFieldObject<false>(mirror::DexCache::DexOffset(), nullptr);
  }
}

bool ImageWriter::NonImageClassesVisitor(Class* klass, void* arg) {
  NonImageClasses* context = reinterpret_cast<NonImageClasses*>(arg);
  if (!context->image_writer->IsImageClass(klass)) {
    std::string temp;
    context->non_image_classes->insert(klass->GetDescriptor(&temp));
  }
  return true;
}

void ImageWriter::CheckNonImageClassesRemoved() {
  if (compiler_driver_.GetImageClasses() != nullptr) {
    gc::Heap* heap = Runtime::Current()->GetHeap();
    heap->VisitObjects(CheckNonImageClassesRemovedCallback, this);
  }
}

void ImageWriter::CheckNonImageClassesRemovedCallback(Object* obj, void* arg) {
  ImageWriter* image_writer = reinterpret_cast<ImageWriter*>(arg);
  if (obj->IsClass()) {
    Class* klass = obj->AsClass();
    if (!image_writer->IsImageClass(klass)) {
      image_writer->DumpImageClasses();
      std::string temp;
      CHECK(image_writer->IsImageClass(klass)) << klass->GetDescriptor(&temp)
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

void ImageWriter::CalculateObjectBinSlots(Object* obj) {
  DCHECK(obj != nullptr);
  // if it is a string, we want to intern it if its not interned.
  if (obj->GetClass()->IsStringClass()) {
    // we must be an interned string that was forward referenced and already assigned
    if (IsImageBinSlotAssigned(obj)) {
      DCHECK_EQ(obj, obj->AsString()->Intern());
      return;
    }
    mirror::String* const interned = obj->AsString()->Intern();
    if (obj != interned) {
      if (!IsImageBinSlotAssigned(interned)) {
        // interned obj is after us, allocate its location early
        AssignImageBinSlot(interned);
      }
      // point those looking for this object to the interned version.
      SetImageBinSlot(obj, GetImageBinSlot(interned));
      return;
    }
    // else (obj == interned), nothing to do but fall through to the normal case
  }

  AssignImageBinSlot(obj);
}

ObjectArray<Object>* ImageWriter::CreateImageRoots() const {
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  Thread* self = Thread::Current();
  StackHandleScope<3> hs(self);
  Handle<Class> object_array_class(hs.NewHandle(
      class_linker->FindSystemClass(self, "[Ljava/lang/Object;")));

  // build an Object[] of all the DexCaches used in the source_space_.
  // Since we can't hold the dex lock when allocating the dex_caches
  // ObjectArray, we lock the dex lock twice, first to get the number
  // of dex caches first and then lock it again to copy the dex
  // caches. We check that the number of dex caches does not change.
  size_t dex_cache_count;
  {
    ReaderMutexLock mu(self, *class_linker->DexLock());
    dex_cache_count = class_linker->GetDexCacheCount();
  }
  Handle<ObjectArray<Object>> dex_caches(
      hs.NewHandle(ObjectArray<Object>::Alloc(self, object_array_class.Get(),
                                              dex_cache_count)));
  CHECK(dex_caches.Get() != nullptr) << "Failed to allocate a dex cache array.";
  {
    ReaderMutexLock mu(self, *class_linker->DexLock());
    CHECK_EQ(dex_cache_count, class_linker->GetDexCacheCount())
        << "The number of dex caches changed.";
    for (size_t i = 0; i < dex_cache_count; ++i) {
      dex_caches->Set<false>(i, class_linker->GetDexCache(i));
    }
  }

  // build an Object[] of the roots needed to restore the runtime
  Handle<ObjectArray<Object>> image_roots(hs.NewHandle(
      ObjectArray<Object>::Alloc(self, object_array_class.Get(), ImageHeader::kImageRootsMax)));
  image_roots->Set<false>(ImageHeader::kResolutionMethod, runtime->GetResolutionMethod());
  image_roots->Set<false>(ImageHeader::kImtConflictMethod, runtime->GetImtConflictMethod());
  image_roots->Set<false>(ImageHeader::kImtUnimplementedMethod,
                          runtime->GetImtUnimplementedMethod());
  image_roots->Set<false>(ImageHeader::kDefaultImt, runtime->GetDefaultImt());
  image_roots->Set<false>(ImageHeader::kCalleeSaveMethod,
                          runtime->GetCalleeSaveMethod(Runtime::kSaveAll));
  image_roots->Set<false>(ImageHeader::kRefsOnlySaveMethod,
                          runtime->GetCalleeSaveMethod(Runtime::kRefsOnly));
  image_roots->Set<false>(ImageHeader::kRefsAndArgsSaveMethod,
                          runtime->GetCalleeSaveMethod(Runtime::kRefsAndArgs));
  image_roots->Set<false>(ImageHeader::kDexCaches, dex_caches.Get());
  image_roots->Set<false>(ImageHeader::kClassRoots, class_linker->GetClassRoots());
  for (int i = 0; i < ImageHeader::kImageRootsMax; i++) {
    CHECK(image_roots->Get(i) != nullptr);
  }
  return image_roots.Get();
}

// Walk instance fields of the given Class. Separate function to allow recursion on the super
// class.
void ImageWriter::WalkInstanceFields(mirror::Object* obj, mirror::Class* klass) {
  // Visit fields of parent classes first.
  StackHandleScope<1> hs(Thread::Current());
  Handle<mirror::Class> h_class(hs.NewHandle(klass));
  mirror::Class* super = h_class->GetSuperClass();
  if (super != nullptr) {
    WalkInstanceFields(obj, super);
  }
  //
  size_t num_reference_fields = h_class->NumReferenceInstanceFields();
  MemberOffset field_offset = h_class->GetFirstReferenceInstanceFieldOffset();
  for (size_t i = 0; i < num_reference_fields; ++i) {
    mirror::Object* value = obj->GetFieldObject<mirror::Object>(field_offset);
    if (value != nullptr) {
      WalkFieldsInOrder(value);
    }
    field_offset = MemberOffset(field_offset.Uint32Value() +
                                sizeof(mirror::HeapReference<mirror::Object>));
  }
}

// For an unvisited object, visit it then all its children found via fields.
void ImageWriter::WalkFieldsInOrder(mirror::Object* obj) {
  // Use our own visitor routine (instead of GC visitor) to get better locality between
  // an object and its fields
  if (!IsImageBinSlotAssigned(obj)) {
    // Walk instance fields of all objects
    StackHandleScope<2> hs(Thread::Current());
    Handle<mirror::Object> h_obj(hs.NewHandle(obj));
    Handle<mirror::Class> klass(hs.NewHandle(obj->GetClass()));
    // visit the object itself.
    CalculateObjectBinSlots(h_obj.Get());
    WalkInstanceFields(h_obj.Get(), klass.Get());
    // Walk static fields of a Class.
    if (h_obj->IsClass()) {
      size_t num_reference_static_fields = klass->NumReferenceStaticFields();
      MemberOffset field_offset = klass->GetFirstReferenceStaticFieldOffset();
      for (size_t i = 0; i < num_reference_static_fields; ++i) {
        mirror::Object* value = h_obj->GetFieldObject<mirror::Object>(field_offset);
        if (value != nullptr) {
          WalkFieldsInOrder(value);
        }
        field_offset = MemberOffset(field_offset.Uint32Value() +
                                    sizeof(mirror::HeapReference<mirror::Object>));
      }

      // Visit and assign offsets for fields.
      ArtField* fields[2] = { h_obj->AsClass()->GetSFields(), h_obj->AsClass()->GetIFields() };
      size_t num_fields[2] = { h_obj->AsClass()->NumStaticFields(),
          h_obj->AsClass()->NumInstanceFields() };
      for (size_t i = 0; i < 2; ++i) {
        for (size_t j = 0; j < num_fields[i]; ++j) {
          auto* field = fields[i] + j;
          auto it = art_field_reloc_.find(field);
          CHECK(it == art_field_reloc_.end()) << "Field at index " << i << ":" << j
              << " already assigned " << PrettyField(field);
          art_field_reloc_.emplace(field, bin_slot_sizes_[kBinArtField]);
          bin_slot_sizes_[kBinArtField] += sizeof(ArtField);
        }
      }
    } else if (h_obj->IsObjectArray()) {
      // Walk elements of an object array.
      int32_t length = h_obj->AsObjectArray<mirror::Object>()->GetLength();
      for (int32_t i = 0; i < length; i++) {
        mirror::ObjectArray<mirror::Object>* obj_array = h_obj->AsObjectArray<mirror::Object>();
        mirror::Object* value = obj_array->Get(i);
        if (value != nullptr) {
          WalkFieldsInOrder(value);
        }
      }
    }
  }
}

void ImageWriter::WalkFieldsCallback(mirror::Object* obj, void* arg) {
  ImageWriter* writer = reinterpret_cast<ImageWriter*>(arg);
  DCHECK(writer != nullptr);
  writer->WalkFieldsInOrder(obj);
}

void ImageWriter::UnbinObjectsIntoOffsetCallback(mirror::Object* obj, void* arg) {
  ImageWriter* writer = reinterpret_cast<ImageWriter*>(arg);
  DCHECK(writer != nullptr);
  writer->UnbinObjectsIntoOffset(obj);
}

void ImageWriter::UnbinObjectsIntoOffset(mirror::Object* obj) {
  CHECK(obj != nullptr);

  // We know the bin slot, and the total bin sizes for all objects by now,
  // so calculate the object's final image offset.

  DCHECK(IsImageBinSlotAssigned(obj));
  BinSlot bin_slot = GetImageBinSlot(obj);
  // Change the lockword from a bin slot into an offset
  AssignImageOffset(obj, bin_slot);
}

void ImageWriter::CalculateNewObjectOffsets() {
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<ObjectArray<Object>> image_roots(hs.NewHandle(CreateImageRoots()));

  gc::Heap* heap = Runtime::Current()->GetHeap();
  DCHECK_EQ(0U, image_end_);

  // Leave space for the header, but do not write it yet, we need to
  // know where image_roots is going to end up
  image_end_ += RoundUp(sizeof(ImageHeader), kObjectAlignment);  // 64-bit-alignment

  DCHECK_LT(image_end_, image_->Size());
  image_objects_offset_begin_ = image_end_;
  // Prepare bin slots for dex cache arrays.
  PrepareDexCacheArraySlots();
  // Clear any pre-existing monitors which may have been in the monitor words, assign bin slots.
  heap->VisitObjects(WalkFieldsCallback, this);
  // Calculate cumulative bin slot sizes.
  size_t previous_sizes = 0u;
  for (size_t i = 0; i != kBinSize; ++i) {
    bin_slot_previous_sizes_[i] = previous_sizes;
    previous_sizes += bin_slot_sizes_[i];
  }
  DCHECK_EQ(previous_sizes, GetBinSizeSum());
  DCHECK_EQ(image_end_, GetBinSizeSum(kBinMirrorCount) + image_objects_offset_begin_);

  // Transform each object's bin slot into an offset which will be used to do the final copy.
  heap->VisitObjects(UnbinObjectsIntoOffsetCallback, this);
  DCHECK(saved_hashes_map_.empty());  // All binslot hashes should've been put into vector by now.

  DCHECK_EQ(image_end_, GetBinSizeSum(kBinMirrorCount) + image_objects_offset_begin_);

  image_roots_address_ = PointerToLowMemUInt32(GetImageAddress(image_roots.Get()));

  // Note that image_end_ is left at end of used mirror space
}

void ImageWriter::CreateHeader(size_t oat_loaded_size, size_t oat_data_offset) {
  CHECK_NE(0U, oat_loaded_size);
  const uint8_t* oat_file_begin = GetOatFileBegin();
  const uint8_t* oat_file_end = oat_file_begin + oat_loaded_size;
  oat_data_begin_ = oat_file_begin + oat_data_offset;
  const uint8_t* oat_data_end = oat_data_begin_ + oat_file_->Size();
  // Write out sections.
  size_t cur_pos = image_end_;
  // Add fields.
  auto fields_offset = cur_pos;
  CHECK_EQ(image_objects_offset_begin_ + GetBinSizeSum(kBinArtField), fields_offset);
  auto fields_size = bin_slot_sizes_[kBinArtField];
  cur_pos += fields_size;
  // Return to write header at start of image with future location of image_roots. At this point,
  // image_end_ is the size of the image (excluding bitmaps, ArtFields).
  /*
  const size_t heap_bytes_per_bitmap_byte = kBitsPerByte * kObjectAlignment;
  const size_t bitmap_bytes = RoundUp(image_end_, heap_bytes_per_bitmap_byte) /
      heap_bytes_per_bitmap_byte;
      */
  const size_t bitmap_bytes = image_bitmap_->Size();
  auto bitmap_offset = RoundUp(cur_pos, kPageSize);
  auto bitmap_size = RoundUp(bitmap_bytes, kPageSize);
  cur_pos += bitmap_size;
  new (image_->Begin()) ImageHeader(PointerToLowMemUInt32(image_begin_),
                                    static_cast<uint32_t>(image_end_),
                                    fields_offset, fields_size,
                                    bitmap_offset, bitmap_size,
                                    image_roots_address_,
                                    oat_file_->GetOatHeader().GetChecksum(),
                                    PointerToLowMemUInt32(oat_file_begin),
                                    PointerToLowMemUInt32(oat_data_begin_),
                                    PointerToLowMemUInt32(oat_data_end),
                                    PointerToLowMemUInt32(oat_file_end),
                                    compile_pic_);
}

void ImageWriter::CopyAndFixupNativeData() {
  // Copy ArtFields to their locations and update the array for convenience.
  auto fields_offset = image_objects_offset_begin_ + GetBinSizeSum(kBinArtField);
  for (auto& pair : art_field_reloc_) {
    pair.second += fields_offset;
    auto* dest = image_->Begin() + pair.second;
    DCHECK_GE(dest, image_->Begin() + image_end_);
    memcpy(dest, pair.first, sizeof(ArtField));
    reinterpret_cast<ArtField*>(dest)->SetDeclaringClass(
        down_cast<Class*>(GetImageAddress(pair.first->GetDeclaringClass())));
  }
}

void ImageWriter::CopyAndFixupObjects() {
  gc::Heap* heap = Runtime::Current()->GetHeap();
  heap->VisitObjects(CopyAndFixupObjectsCallback, this);
  // Fix up the object previously had hash codes.
  for (const std::pair<mirror::Object*, uint32_t>& hash_pair : saved_hashes_) {
    Object* obj = hash_pair.first;
    DCHECK_EQ(obj->GetLockWord(false).ReadBarrierState(), 0U);
    obj->SetLockWord(LockWord::FromHashCode(hash_pair.second, 0U), false);
  }
  saved_hashes_.clear();
}

void ImageWriter::CopyAndFixupObjectsCallback(Object* obj, void* arg) {
  DCHECK(obj != nullptr);
  DCHECK(arg != nullptr);
  reinterpret_cast<ImageWriter*>(arg)->CopyAndFixupObject(obj);
}

bool ImageWriter::CopyAndFixupIfDexCacheFieldArray(mirror::Object* dst, mirror::Object* obj,
                                                   mirror::Class* klass) {
  if (!klass->IsArrayClass()) {
    return false;
  }
  auto* component_type = klass->GetComponentType();
  bool is_int_arr = component_type->IsPrimitiveInt();
  bool is_long_arr = component_type->IsPrimitiveLong();
  if (!is_int_arr && !is_long_arr) {
    return false;
  }
  auto it = dex_cache_array_indexes_.find(obj);  // Is this a dex cache array?
  if (it == dex_cache_array_indexes_.end()) {
    return false;
  }
  mirror::Array* arr = obj->AsArray();
  CHECK_EQ(reinterpret_cast<Object*>(
      image_->Begin() + it->second.offset_ + image_objects_offset_begin_), dst);
  dex_cache_array_indexes_.erase(it);
  // Fixup int pointers for the field array.
  CHECK(!arr->IsObjectArray());
  const size_t num_elements = arr->GetLength();
  if (target_ptr_size_ == 4) {
    // Will get fixed up by fixup object.
    dst->SetClass(down_cast<mirror::Class*>(
    GetImageAddress(mirror::IntArray::GetArrayClass())));
  } else {
    DCHECK_EQ(target_ptr_size_, 8u);
    dst->SetClass(down_cast<mirror::Class*>(
    GetImageAddress(mirror::LongArray::GetArrayClass())));
  }
  mirror::Array* dest_array = down_cast<mirror::Array*>(dst);
  dest_array->SetLength(num_elements);
  for (size_t i = 0, count = num_elements; i < count; ++i) {
    ArtField* field = reinterpret_cast<ArtField*>(is_int_arr ?
        arr->AsIntArray()->GetWithoutChecks(i) : arr->AsLongArray()->GetWithoutChecks(i));
    uint8_t* fixup_location = nullptr;
    if (field != nullptr) {
      auto it2 = art_field_reloc_.find(field);
      CHECK(it2 != art_field_reloc_.end()) << "No relocation for field " << PrettyField(field);
      fixup_location = image_begin_ + it2->second;
    }
    if (target_ptr_size_ == 4) {
      down_cast<mirror::IntArray*>(dest_array)->SetWithoutChecks<kVerifyNone>(
          i, static_cast<uint32_t>(reinterpret_cast<uint64_t>(fixup_location)));
    } else {
      down_cast<mirror::LongArray*>(dest_array)->SetWithoutChecks<kVerifyNone>(
          i, reinterpret_cast<uint64_t>(fixup_location));
    }
  }
  dst->SetLockWord(LockWord::Default(), false);
  return true;
}

void ImageWriter::CopyAndFixupObject(Object* obj) {
  // see GetLocalAddress for similar computation
  size_t offset = GetImageOffset(obj);
  auto* dst = reinterpret_cast<Object*>(image_->Begin() + offset);
  const uint8_t* src = reinterpret_cast<const uint8_t*>(obj);
  size_t n;
  mirror::Class* klass = obj->GetClass();

  if (CopyAndFixupIfDexCacheFieldArray(dst, obj, klass)) {
    return;
  }
  if (klass->IsArtMethodClass()) {
    // Size without pointer fields since we don't want to overrun the buffer if target art method
    // is 32 bits but source is 64 bits.
    n = mirror::ArtMethod::SizeWithoutPointerFields(target_ptr_size_);
  } else {
    n = obj->SizeOf();
  }
  DCHECK_LE(offset + n, image_->Size());
  memcpy(dst, src, n);

  // Write in a hash code of objects which have inflated monitors or a hash code in their monitor
  // word.
  dst->SetLockWord(LockWord::Default(), false);
  FixupObject(obj, dst);
}

// Rewrite all the references in the copied object to point to their image address equivalent
class FixupVisitor {
 public:
  FixupVisitor(ImageWriter* image_writer, Object* copy) : image_writer_(image_writer), copy_(copy) {
  }

  void operator()(Object* obj, MemberOffset offset, bool /*is_static*/) const
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    Object* ref = obj->GetFieldObject<Object, kVerifyNone>(offset);
    // Use SetFieldObjectWithoutWriteBarrier to avoid card marking since we are writing to the
    // image.
    copy_->SetFieldObjectWithoutWriteBarrier<false, true, kVerifyNone>(
        offset, image_writer_->GetImageAddress(ref));
  }

  // java.lang.ref.Reference visitor.
  void operator()(mirror::Class* /*klass*/, mirror::Reference* ref) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    copy_->SetFieldObjectWithoutWriteBarrier<false, true, kVerifyNone>(
        mirror::Reference::ReferentOffset(), image_writer_->GetImageAddress(ref->GetReferent()));
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
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    DCHECK(obj->IsClass());
    FixupVisitor::operator()(obj, offset, /*is_static*/false);
  }

  void operator()(mirror::Class* klass ATTRIBUTE_UNUSED,
                  mirror::Reference* ref ATTRIBUTE_UNUSED) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    LOG(FATAL) << "Reference not expected here.";
  }
};

void ImageWriter::FixupClass(mirror::Class* orig, mirror::Class* copy) {
  // Copy and fix up ArtFields in the class.
  ArtField* fields[2] = { orig->AsClass()->GetSFields(), orig->AsClass()->GetIFields() };
  size_t num_fields[2] = { orig->AsClass()->NumStaticFields(),
      orig->AsClass()->NumInstanceFields() };
  // Update the arrays.
  for (size_t i = 0; i < 2; ++i) {
    if (num_fields[i] == 0) {
      CHECK(fields[i] == nullptr);
      continue;
    }
    auto it = art_field_reloc_.find(fields[i]);
    CHECK(it != art_field_reloc_.end()) << PrettyClass(orig->AsClass()) << " : "
        << PrettyField(fields[i]);
    auto* image_fields = reinterpret_cast<ArtField*>(image_begin_ + it->second);
    if (i == 0) {
      down_cast<Class*>(copy)->SetSFieldsUnchecked(image_fields);
    } else {
      down_cast<Class*>(copy)->SetIFieldsUnchecked(image_fields);
    }
  }
  FixupClassVisitor visitor(this, copy);
  static_cast<mirror::Object*>(orig)->VisitReferences<true /*visit class*/>(visitor, visitor);
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
  if (orig->IsClass()) {
    FixupClass(orig->AsClass<kVerifyNone>(), down_cast<mirror::Class*>(copy));
  } else {
    FixupVisitor visitor(this, copy);
    orig->VisitReferences<true /*visit class*/>(visitor, visitor);
  }
  if (orig->IsArtMethod<kVerifyNone>()) {
    FixupMethod(orig->AsArtMethod<kVerifyNone>(), down_cast<ArtMethod*>(copy));
  }
}

const uint8_t* ImageWriter::GetQuickCode(mirror::ArtMethod* method, bool* quick_is_interpreted) {
  DCHECK(!method->IsResolutionMethod() && !method->IsImtConflictMethod() &&
         !method->IsImtUnimplementedMethod() && !method->IsAbstract()) << PrettyMethod(method);

  // Use original code if it exists. Otherwise, set the code pointer to the resolution
  // trampoline.

  // Quick entrypoint:
  uint32_t quick_oat_code_offset = PointerToLowMemUInt32(
      method->GetEntryPointFromQuickCompiledCodePtrSize(target_ptr_size_));
  const uint8_t* quick_code = GetOatAddress(quick_oat_code_offset);
  *quick_is_interpreted = false;
  if (quick_code != nullptr &&
      (!method->IsStatic() || method->IsConstructor() || method->GetDeclaringClass()->IsInitialized())) {
    // We have code for a non-static or initialized method, just use the code.
  } else if (quick_code == nullptr && method->IsNative() &&
      (!method->IsStatic() || method->GetDeclaringClass()->IsInitialized())) {
    // Non-static or initialized native method missing compiled code, use generic JNI version.
    quick_code = GetOatAddress(quick_generic_jni_trampoline_offset_);
  } else if (quick_code == nullptr && !method->IsNative()) {
    // We don't have code at all for a non-native method, use the interpreter.
    quick_code = GetOatAddress(quick_to_interpreter_bridge_offset_);
    *quick_is_interpreted = true;
  } else {
    CHECK(!method->GetDeclaringClass()->IsInitialized());
    // We have code for a static method, but need to go through the resolution stub for class
    // initialization.
    quick_code = GetOatAddress(quick_resolution_trampoline_offset_);
  }
  return quick_code;
}

const uint8_t* ImageWriter::GetQuickEntryPoint(mirror::ArtMethod* method) {
  // Calculate the quick entry point following the same logic as FixupMethod() below.
  // The resolution method has a special trampoline to call.
  Runtime* runtime = Runtime::Current();
  if (UNLIKELY(method == runtime->GetResolutionMethod())) {
    return GetOatAddress(quick_resolution_trampoline_offset_);
  } else if (UNLIKELY(method == runtime->GetImtConflictMethod() ||
                      method == runtime->GetImtUnimplementedMethod())) {
    return GetOatAddress(quick_imt_conflict_trampoline_offset_);
  } else {
    // We assume all methods have code. If they don't currently then we set them to the use the
    // resolution trampoline. Abstract methods never have code and so we need to make sure their
    // use results in an AbstractMethodError. We use the interpreter to achieve this.
    if (UNLIKELY(method->IsAbstract())) {
      return GetOatAddress(quick_to_interpreter_bridge_offset_);
    } else {
      bool quick_is_interpreted;
      return GetQuickCode(method, &quick_is_interpreted);
    }
  }
}

void ImageWriter::FixupMethod(ArtMethod* orig, ArtMethod* copy) {
  // OatWriter replaces the code_ with an offset value. Here we re-adjust to a pointer relative to
  // oat_begin_
  // For 64 bit targets we need to repack the current runtime pointer sized fields to the right
  // locations.
  // Copy all of the fields from the runtime methods to the target methods first since we did a
  // bytewise copy earlier.
  copy->SetEntryPointFromInterpreterPtrSize<kVerifyNone>(
      orig->GetEntryPointFromInterpreterPtrSize(target_ptr_size_), target_ptr_size_);
  copy->SetEntryPointFromJniPtrSize<kVerifyNone>(
      orig->GetEntryPointFromJniPtrSize(target_ptr_size_), target_ptr_size_);
  copy->SetEntryPointFromQuickCompiledCodePtrSize<kVerifyNone>(
      orig->GetEntryPointFromQuickCompiledCodePtrSize(target_ptr_size_), target_ptr_size_);

  // The resolution method has a special trampoline to call.
  Runtime* runtime = Runtime::Current();
  if (UNLIKELY(orig == runtime->GetResolutionMethod())) {
    copy->SetEntryPointFromQuickCompiledCodePtrSize<kVerifyNone>(
        GetOatAddress(quick_resolution_trampoline_offset_), target_ptr_size_);
  } else if (UNLIKELY(orig == runtime->GetImtConflictMethod() ||
                      orig == runtime->GetImtUnimplementedMethod())) {
    copy->SetEntryPointFromQuickCompiledCodePtrSize<kVerifyNone>(
        GetOatAddress(quick_imt_conflict_trampoline_offset_), target_ptr_size_);
  } else {
    // We assume all methods have code. If they don't currently then we set them to the use the
    // resolution trampoline. Abstract methods never have code and so we need to make sure their
    // use results in an AbstractMethodError. We use the interpreter to achieve this.
    if (UNLIKELY(orig->IsAbstract())) {
      copy->SetEntryPointFromQuickCompiledCodePtrSize<kVerifyNone>(
          GetOatAddress(quick_to_interpreter_bridge_offset_), target_ptr_size_);
      copy->SetEntryPointFromInterpreterPtrSize<kVerifyNone>(
          reinterpret_cast<EntryPointFromInterpreter*>(const_cast<uint8_t*>(
                  GetOatAddress(interpreter_to_interpreter_bridge_offset_))), target_ptr_size_);
    } else {
      bool quick_is_interpreted;
      const uint8_t* quick_code = GetQuickCode(orig, &quick_is_interpreted);
      copy->SetEntryPointFromQuickCompiledCodePtrSize<kVerifyNone>(quick_code, target_ptr_size_);

      // JNI entrypoint:
      if (orig->IsNative()) {
        // The native method's pointer is set to a stub to lookup via dlsym.
        // Note this is not the code_ pointer, that is handled above.
        copy->SetEntryPointFromJniPtrSize<kVerifyNone>(GetOatAddress(jni_dlsym_lookup_offset_),
                                                       target_ptr_size_);
      }

      // Interpreter entrypoint:
      // Set the interpreter entrypoint depending on whether there is compiled code or not.
      uint32_t interpreter_code = (quick_is_interpreted)
          ? interpreter_to_interpreter_bridge_offset_
          : interpreter_to_compiled_code_bridge_offset_;
      EntryPointFromInterpreter* interpreter_entrypoint =
          reinterpret_cast<EntryPointFromInterpreter*>(
              const_cast<uint8_t*>(GetOatAddress(interpreter_code)));
      copy->SetEntryPointFromInterpreterPtrSize<kVerifyNone>(
          interpreter_entrypoint, target_ptr_size_);
    }
  }
}

static OatHeader* GetOatHeaderFromElf(ElfFile* elf) {
  uint64_t data_sec_offset;
  bool has_data_sec = elf->GetSectionOffsetAndSize(".rodata", &data_sec_offset, nullptr);
  if (!has_data_sec) {
    return nullptr;
  }
  return reinterpret_cast<OatHeader*>(elf->Begin() + data_sec_offset);
}

void ImageWriter::SetOatChecksumFromElfFile(File* elf_file) {
  std::string error_msg;
  std::unique_ptr<ElfFile> elf(ElfFile::Open(elf_file, PROT_READ|PROT_WRITE,
                                             MAP_SHARED, &error_msg));
  if (elf.get() == nullptr) {
    LOG(FATAL) << "Unable open oat file: " << error_msg;
    return;
  }
  OatHeader* oat_header = GetOatHeaderFromElf(elf.get());
  CHECK(oat_header != nullptr);
  CHECK(oat_header->IsValid());

  ImageHeader* image_header = reinterpret_cast<ImageHeader*>(image_->Begin());
  image_header->SetOatChecksum(oat_header->GetChecksum());
}

size_t ImageWriter::GetBinSizeSum(ImageWriter::Bin up_to) const {
  DCHECK_LE(up_to, kBinSize);
  return std::accumulate(&bin_slot_sizes_[0], &bin_slot_sizes_[up_to], /*init*/0);
}

ImageWriter::BinSlot::BinSlot(uint32_t lockword) : lockword_(lockword) {
  // These values may need to get updated if more bins are added to the enum Bin
  static_assert(kBinBits == 4, "wrong number of bin bits");
  static_assert(kBinShift == 28, "wrong number of shift");
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

void ImageWriter::FreeStringDataArray() {
  if (string_data_array_ != nullptr) {
    gc::space::LargeObjectSpace* los = Runtime::Current()->GetHeap()->GetLargeObjectsSpace();
    if (los != nullptr) {
      los->Free(Thread::Current(), reinterpret_cast<mirror::Object*>(string_data_array_));
    }
  }
}

}  // namespace art
