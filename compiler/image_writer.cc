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
#include <vector>

#include "base/logging.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "compiled_method.h"
#include "dex_file-inl.h"
#include "driver/compiler_driver.h"
#include "elf_file.h"
#include "elf_utils.h"
#include "elf_patcher.h"
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
#include "lock_word.h"
#include "mirror/art_field-inl.h"
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

#include <numeric>

using ::art::mirror::ArtField;
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

bool ImageWriter::Write(const std::string& image_filename,
                        uintptr_t image_begin,
                        const std::string& oat_filename,
                        const std::string& oat_location,
                        bool compile_pic) {
  CHECK(!image_filename.empty());

  CHECK_NE(image_begin, 0U);
  image_begin_ = reinterpret_cast<byte*>(image_begin);
  compile_pic_ = compile_pic;

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  target_ptr_size_ = InstructionSetPointerSize(compiler_driver_.GetInstructionSet());
  std::unique_ptr<File> oat_file(OS::OpenFileReadWrite(oat_filename.c_str()));
  if (oat_file.get() == NULL) {
    LOG(ERROR) << "Failed to open oat file " << oat_filename << " for " << oat_location;
    return false;
  }
  std::string error_msg;
  oat_file_ = OatFile::OpenReadable(oat_file.get(), oat_location, &error_msg);
  if (oat_file_ == nullptr) {
    LOG(ERROR) << "Failed to open writable oat file " << oat_filename << " for " << oat_location
        << ": " << error_msg;
    return false;
  }
  CHECK_EQ(class_linker->RegisterOatFile(oat_file_), oat_file_);

  interpreter_to_interpreter_bridge_offset_ =
      oat_file_->GetOatHeader().GetInterpreterToInterpreterBridgeOffset();
  interpreter_to_compiled_code_bridge_offset_ =
      oat_file_->GetOatHeader().GetInterpreterToCompiledCodeBridgeOffset();

  jni_dlsym_lookup_offset_ = oat_file_->GetOatHeader().GetJniDlsymLookupOffset();

  portable_imt_conflict_trampoline_offset_ =
      oat_file_->GetOatHeader().GetPortableImtConflictTrampolineOffset();
  portable_resolution_trampoline_offset_ =
      oat_file_->GetOatHeader().GetPortableResolutionTrampolineOffset();
  portable_to_interpreter_bridge_offset_ =
      oat_file_->GetOatHeader().GetPortableToInterpreterBridgeOffset();

  quick_generic_jni_trampoline_offset_ =
      oat_file_->GetOatHeader().GetQuickGenericJniTrampolineOffset();
  quick_imt_conflict_trampoline_offset_ =
      oat_file_->GetOatHeader().GetQuickImtConflictTrampolineOffset();
  quick_resolution_trampoline_offset_ =
      oat_file_->GetOatHeader().GetQuickResolutionTrampolineOffset();
  quick_to_interpreter_bridge_offset_ =
      oat_file_->GetOatHeader().GetQuickToInterpreterBridgeOffset();
  {
    Thread::Current()->TransitionFromSuspendedToRunnable();
    PruneNonImageClasses();  // Remove junk
    ComputeLazyFieldsForImageClasses();  // Add useful information
    ProcessStrings();
    Thread::Current()->TransitionFromRunnableToSuspended(kNative);
  }
  gc::Heap* heap = Runtime::Current()->GetHeap();
  heap->CollectGarbage(false);  // Remove garbage.

  if (!AllocMemory()) {
    return false;
  }

  if (kIsDebugBuild) {
    ScopedObjectAccess soa(Thread::Current());
    CheckNonImageClassesRemoved();
  }

  Thread::Current()->TransitionFromSuspendedToRunnable();
  size_t oat_loaded_size = 0;
  size_t oat_data_offset = 0;
  ElfWriter::GetOatElfInformation(oat_file.get(), oat_loaded_size, oat_data_offset);
  CalculateNewObjectOffsets(oat_loaded_size, oat_data_offset);
  CopyAndFixupObjects();

  PatchOatCodeAndMethods(oat_file.get());

  // Before flushing, which might fail, release the mutator lock.
  Thread::Current()->TransitionFromRunnableToSuspended(kNative);

  if (oat_file->FlushCloseOrErase() != 0) {
    LOG(ERROR) << "Failed to flush and close oat file " << oat_filename << " for " << oat_location;
    return false;
  }

  std::unique_ptr<File> image_file(OS::CreateEmptyFile(image_filename.c_str()));
  ImageHeader* image_header = reinterpret_cast<ImageHeader*>(image_->Begin());
  if (image_file.get() == NULL) {
    LOG(ERROR) << "Failed to open image file " << image_filename;
    return false;
  }
  if (fchmod(image_file->Fd(), 0644) != 0) {
    PLOG(ERROR) << "Failed to make image file world readable: " << image_filename;
    image_file->Erase();
    return EXIT_FAILURE;
  }

  // Write out the image.
  CHECK_EQ(image_end_, image_header->GetImageSize());
  if (!image_file->WriteFully(image_->Begin(), image_end_)) {
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

  size_t previous_bin_sizes = GetBinSizeSum(bin_slot.GetBin());  // sum sizes in [0..bin#)
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
      break;
  }
  object->SetLockWord(LockWord::FromForwardingAddress(static_cast<uint32_t>(bin_slot)),
                      false);
  DCHECK(IsImageBinSlotAssigned(object));
}

void ImageWriter::AssignImageBinSlot(mirror::Object* object) {
  DCHECK(object != nullptr);
  size_t object_size;
  if (object->IsArtMethod()) {
    // Methods are sized based on the target pointer size.
    object_size = mirror::ArtMethod::InstanceSize(target_ptr_size_);
  } else {
    object_size = object->SizeOf();
  }

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

  if (kBinObjects) {
    //
    // Changing the bin of an object is purely a memory-use tuning.
    // It has no change on runtime correctness.
    //
    // Memory analysis has determined that the following types of objects get dirtied
    // the most:
    //
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
    }  // else bin = kBinRegular
  }

  size_t current_offset = bin_slot_sizes_[bin];  // How many bytes the current bin is at (aligned).
  // Move the current bin size up to accomodate the object we just assigned a bin slot.
  size_t offset_delta = RoundUp(object_size, kObjectAlignment);  // 64-bit alignment
  bin_slot_sizes_[bin] += offset_delta;

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
  size_t length = RoundUp(Runtime::Current()->GetHeap()->GetTotalMemory(), kPageSize);
  std::string error_msg;
  image_.reset(MemMap::MapAnonymous("image writer image", NULL, length, PROT_READ | PROT_WRITE,
                                    true, &error_msg));
  if (UNLIKELY(image_.get() == nullptr)) {
    LOG(ERROR) << "Failed to allocate memory for image file generation: " << error_msg;
    return false;
  }

  // Create the image bitmap.
  image_bitmap_.reset(gc::accounting::ContinuousSpaceBitmap::Create("image bitmap", image_->Begin(),
                                                                    length));
  if (image_bitmap_.get() == nullptr) {
    LOG(ERROR) << "Failed to allocate memory for image bitmap";
    return false;
  }
  return true;
}

void ImageWriter::ComputeLazyFieldsForImageClasses() {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  class_linker->VisitClassesWithoutClassesLock(ComputeLazyFieldsForClassesVisitor, NULL);
}

bool ImageWriter::ComputeLazyFieldsForClassesVisitor(Class* c, void* /*arg*/) {
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  mirror::Class::ComputeName(hs.NewHandle(c));
  return true;
}

// Count the number of strings in the heap and put the result in arg as a size_t pointer.
static void CountStringsCallback(Object* obj, void* arg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (obj->GetClass()->IsStringClass()) {
    ++*reinterpret_cast<size_t*>(arg);
  }
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
class StringLengthComparator {
 public:
  explicit StringLengthComparator(Handle<mirror::ObjectArray<mirror::String>> strings)
      : strings_(strings) {
  }
  bool operator()(size_t a, size_t b) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return strings_->GetWithoutChecks(a)->GetLength() < strings_->GetWithoutChecks(b)->GetLength();
  }

 private:
  Handle<mirror::ObjectArray<mirror::String>> strings_;
};

// Normal string < comparison through the chars_ array.
class SubstringComparator {
 public:
  explicit SubstringComparator(const std::vector<uint16_t>* const chars) : chars_(chars) {
  }
  bool operator()(const std::pair<size_t, size_t>& a, const std::pair<size_t, size_t>& b) {
    return std::lexicographical_compare(chars_->begin() + a.first,
                                        chars_->begin() + a.first + a.second,
                                        chars_->begin() + b.first,
                                        chars_->begin() + b.first + b.second);
  }

 private:
  const std::vector<uint16_t>* const chars_;
};

void ImageWriter::ProcessStrings() {
  size_t total_strings = 0;
  gc::Heap* heap = Runtime::Current()->GetHeap();
  ClassLinker* cl = Runtime::Current()->GetClassLinker();
  {
    ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
    heap->VisitObjects(CountStringsCallback, &total_strings);  // Count the strings.
  }
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  auto strings = hs.NewHandle(cl->AllocStringArray(self, total_strings));
  StringCollector string_collector(strings, 0U);
  {
    ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
    // Read strings into the array.
    heap->VisitObjects(StringCollector::Callback, &string_collector);
  }
  // Some strings could have gotten freed if AllocStringArray caused a GC.
  CHECK_LE(string_collector.GetIndex(), total_strings);
  total_strings = string_collector.GetIndex();
  size_t total_length = 0;
  std::vector<size_t> reverse_sorted_strings;
  for (size_t i = 0; i < total_strings; ++i) {
    mirror::String* s = strings->GetWithoutChecks(i);
    // Look up the string in the array.
    total_length += s->GetLength();
    reverse_sorted_strings.push_back(i);
  }
  // Sort by reverse length.
  StringLengthComparator comparator(strings);
  std::sort(reverse_sorted_strings.rbegin(), reverse_sorted_strings.rend(), comparator);
  // Deduplicate prefixes and add strings to the char array.
  std::vector<uint16_t> combined_chars(total_length, 0U);
  size_t num_chars = 0;
  // Characters of strings which are non equal prefix of another string (not the same string).
  // We don't count the savings from equal strings since these would get interned later anyways.
  size_t prefix_saved_chars = 0;
  std::set<std::pair<size_t, size_t>, SubstringComparator> existing_strings((
      SubstringComparator(&combined_chars)));
  for (size_t i = 0; i < total_strings; ++i) {
    mirror::String* s = strings->GetWithoutChecks(reverse_sorted_strings[i]);
    // Add the string to the end of the char array.
    size_t length = s->GetLength();
    for (size_t j = 0; j < length; ++j) {
      combined_chars[num_chars++] = s->CharAt(j);
    }
    // Try to see if the string exists as a prefix of an existing string.
    size_t new_offset = 0;
    std::pair<size_t, size_t> new_string(num_chars - length, length);
    auto it = existing_strings.lower_bound(new_string);
    bool is_prefix = true;
    if (it == existing_strings.end()) {
      is_prefix = false;
    } else {
      CHECK_LE(length, it->second);
      for (size_t j = 0; j < length; ++j) {
        if (combined_chars[it->first + j] != s->CharAt(j)) {
          is_prefix = false;
          break;
        }
      }
    }
    if (is_prefix) {
      // Shares a prefix, set the offset to where the new offset will be.
      new_offset = it->first;
      // Remove the added chars.
      num_chars -= length;
      if (it->second != length) {
        prefix_saved_chars += length;
      }
    } else {
      new_offset = new_string.first;
      existing_strings.insert(new_string);
    }
    s->SetOffset(new_offset);
  }
  // Allocate and update the char arrays.
  auto* array = mirror::CharArray::Alloc(self, num_chars);
  for (size_t i = 0; i < num_chars; ++i) {
    array->SetWithoutChecks<false>(i, combined_chars[i]);
  }
  for (size_t i = 0; i < total_strings; ++i) {
    strings->GetWithoutChecks(i)->SetArray(array);
  }
  if (kIsDebugBuild || VLOG_IS_ON(compiler)) {
    LOG(INFO) << "Total # image strings=" << total_strings << " combined length="
        << total_length << " prefix saved chars=" << prefix_saved_chars;
  }
  ComputeEagerResolvedStrings();
}

void ImageWriter::ComputeEagerResolvedStringsCallback(Object* obj, void* arg) {
  if (!obj->GetClass()->IsStringClass()) {
    return;
  }
  mirror::String* string = obj->AsString();
  const uint16_t* utf16_string = string->GetCharArray()->GetData() + string->GetOffset();
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ReaderMutexLock mu(Thread::Current(), *class_linker->DexLock());
  size_t dex_cache_count = class_linker->GetDexCacheCount();
  for (size_t i = 0; i < dex_cache_count; ++i) {
    DexCache* dex_cache = class_linker->GetDexCache(i);
    const DexFile& dex_file = *dex_cache->GetDexFile();
    const DexFile::StringId* string_id;
    if (UNLIKELY(string->GetLength() == 0)) {
      string_id = dex_file.FindStringId("");
    } else {
      string_id = dex_file.FindStringId(utf16_string);
    }
    if (string_id != nullptr) {
      // This string occurs in this dex file, assign the dex cache entry.
      uint32_t string_idx = dex_file.GetIndexForStringId(*string_id);
      if (dex_cache->GetResolvedString(string_idx) == NULL) {
        dex_cache->SetResolvedString(string_idx, string);
      }
    }
  }
}

void ImageWriter::ComputeEagerResolvedStrings() {
  ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
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
  if (compiler_driver_.GetImageClasses() == NULL) {
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
    bool result = class_linker->RemoveClass(it.c_str(), NULL);
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
      if (klass != NULL && !IsImageClass(klass)) {
        dex_cache->SetResolvedType(i, NULL);
      }
    }
    for (size_t i = 0; i < dex_cache->NumResolvedMethods(); i++) {
      ArtMethod* method = dex_cache->GetResolvedMethod(i);
      if (method != NULL && !IsImageClass(method->GetDeclaringClass())) {
        dex_cache->SetResolvedMethod(i, resolution_method);
      }
    }
    for (size_t i = 0; i < dex_cache->NumResolvedFields(); i++) {
      ArtField* field = dex_cache->GetResolvedField(i);
      if (field != NULL && !IsImageClass(field->GetDeclaringClass())) {
        dex_cache->SetResolvedField(i, NULL);
      }
    }
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
    ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
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
  const std::set<std::string>* image_classes = compiler_driver_.GetImageClasses();
  CHECK(image_classes != NULL);
  for (const std::string& image_class : *image_classes) {
    LOG(INFO) << " " << image_class;
  }
}

void ImageWriter::CalculateObjectBinSlots(Object* obj) {
  DCHECK(obj != NULL);
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
    ReaderMutexLock mu(Thread::Current(), *class_linker->DexLock());
    dex_cache_count = class_linker->GetDexCacheCount();
  }
  Handle<ObjectArray<Object>> dex_caches(
      hs.NewHandle(ObjectArray<Object>::Alloc(self, object_array_class.Get(),
                                              dex_cache_count)));
  CHECK(dex_caches.Get() != nullptr) << "Failed to allocate a dex cache array.";
  {
    ReaderMutexLock mu(Thread::Current(), *class_linker->DexLock());
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
    CHECK(image_roots->Get(i) != NULL);
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
      size_t num_static_fields = klass->NumReferenceStaticFields();
      MemberOffset field_offset = klass->GetFirstReferenceStaticFieldOffset();
      for (size_t i = 0; i < num_static_fields; ++i) {
        mirror::Object* value = h_obj->GetFieldObject<mirror::Object>(field_offset);
        if (value != nullptr) {
          WalkFieldsInOrder(value);
        }
        field_offset = MemberOffset(field_offset.Uint32Value() +
                                    sizeof(mirror::HeapReference<mirror::Object>));
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

void ImageWriter::CalculateNewObjectOffsets(size_t oat_loaded_size, size_t oat_data_offset) {
  CHECK_NE(0U, oat_loaded_size);
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<ObjectArray<Object>> image_roots(hs.NewHandle(CreateImageRoots()));

  gc::Heap* heap = Runtime::Current()->GetHeap();
  DCHECK_EQ(0U, image_end_);

  // Leave space for the header, but do not write it yet, we need to
  // know where image_roots is going to end up
  image_end_ += RoundUp(sizeof(ImageHeader), kObjectAlignment);  // 64-bit-alignment

  {
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    // TODO: Image spaces only?
    const char* old = self->StartAssertNoThreadSuspension("ImageWriter");
    DCHECK_LT(image_end_, image_->Size());
    image_objects_offset_begin_ = image_end_;
    // Clear any pre-existing monitors which may have been in the monitor words, assign bin slots.
    heap->VisitObjects(WalkFieldsCallback, this);
    // Transform each object's bin slot into an offset which will be used to do the final copy.
    heap->VisitObjects(UnbinObjectsIntoOffsetCallback, this);
    DCHECK(saved_hashes_map_.empty());  // All binslot hashes should've been put into vector by now.
    self->EndAssertNoThreadSuspension(old);
  }

  DCHECK_GT(image_end_, GetBinSizeSum());

  if (kIsDebugBuild) {
    LOG(INFO) << "Bin summary (total size: " << GetBinSizeSum() << "): ";
    for (size_t bin = 0; bin < kBinSize; ++bin) {
      LOG(INFO) << "  bin# " << bin << ", number objects: " << bin_slot_count_[bin] << ", "
                << " total byte size: " << bin_slot_sizes_[bin];
    }
  }

  const byte* oat_file_begin = image_begin_ + RoundUp(image_end_, kPageSize);
  const byte* oat_file_end = oat_file_begin + oat_loaded_size;
  oat_data_begin_ = oat_file_begin + oat_data_offset;
  const byte* oat_data_end = oat_data_begin_ + oat_file_->Size();

  // Return to write header at start of image with future location of image_roots. At this point,
  // image_end_ is the size of the image (excluding bitmaps).
  const size_t heap_bytes_per_bitmap_byte = kBitsPerByte * kObjectAlignment;
  const size_t bitmap_bytes = RoundUp(image_end_, heap_bytes_per_bitmap_byte) /
      heap_bytes_per_bitmap_byte;
  ImageHeader image_header(PointerToLowMemUInt32(image_begin_),
                           static_cast<uint32_t>(image_end_),
                           RoundUp(image_end_, kPageSize),
                           RoundUp(bitmap_bytes, kPageSize),
                           PointerToLowMemUInt32(GetImageAddress(image_roots.Get())),
                           oat_file_->GetOatHeader().GetChecksum(),
                           PointerToLowMemUInt32(oat_file_begin),
                           PointerToLowMemUInt32(oat_data_begin_),
                           PointerToLowMemUInt32(oat_data_end),
                           PointerToLowMemUInt32(oat_file_end),
                           compile_pic_);
  memcpy(image_->Begin(), &image_header, sizeof(image_header));

  // Note that image_end_ is left at end of used space
}

void ImageWriter::CopyAndFixupObjects() {
  Thread* self = Thread::Current();
  const char* old_cause = self->StartAssertNoThreadSuspension("ImageWriter");
  gc::Heap* heap = Runtime::Current()->GetHeap();
  // TODO: heap validation can't handle this fix up pass
  heap->DisableObjectValidation();
  // TODO: Image spaces only?
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  heap->VisitObjects(CopyAndFixupObjectsCallback, this);
  // Fix up the object previously had hash codes.
  for (const std::pair<mirror::Object*, uint32_t>& hash_pair : saved_hashes_) {
    hash_pair.first->SetLockWord(LockWord::FromHashCode(hash_pair.second), false);
  }
  saved_hashes_.clear();
  self->EndAssertNoThreadSuspension(old_cause);
}

void ImageWriter::CopyAndFixupObjectsCallback(Object* obj, void* arg) {
  DCHECK(obj != nullptr);
  DCHECK(arg != nullptr);
  ImageWriter* image_writer = reinterpret_cast<ImageWriter*>(arg);
  // see GetLocalAddress for similar computation
  size_t offset = image_writer->GetImageOffset(obj);
  byte* dst = image_writer->image_->Begin() + offset;
  const byte* src = reinterpret_cast<const byte*>(obj);
  size_t n;
  if (obj->IsArtMethod()) {
    // Size without pointer fields since we don't want to overrun the buffer if target art method
    // is 32 bits but source is 64 bits.
    n = mirror::ArtMethod::SizeWithoutPointerFields(sizeof(void*));
  } else {
    n = obj->SizeOf();
  }
  DCHECK_LT(offset + n, image_writer->image_->Size());
  memcpy(dst, src, n);
  Object* copy = reinterpret_cast<Object*>(dst);
  // Write in a hash code of objects which have inflated monitors or a hash code in their monitor
  // word.
  copy->SetLockWord(LockWord(), false);
  image_writer->FixupObject(obj, copy);
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

  void operator()(Object* obj, MemberOffset offset, bool /*is_static*/) const
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    DCHECK(obj->IsClass());
    FixupVisitor::operator()(obj, offset, /*is_static*/false);

    // TODO: Remove dead code
    if (offset.Uint32Value() < mirror::Class::EmbeddedVTableOffset().Uint32Value()) {
      return;
    }
  }

  void operator()(mirror::Class* /*klass*/, mirror::Reference* ref) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    LOG(FATAL) << "Reference not expected here.";
  }
};

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
  if (orig->IsClass() && orig->AsClass()->ShouldHaveEmbeddedImtAndVTable()) {
    FixupClassVisitor visitor(this, copy);
    orig->VisitReferences<true /*visit class*/>(visitor, visitor);
  } else {
    FixupVisitor visitor(this, copy);
    orig->VisitReferences<true /*visit class*/>(visitor, visitor);
  }
  if (orig->IsArtMethod<kVerifyNone>()) {
    FixupMethod(orig->AsArtMethod<kVerifyNone>(), down_cast<ArtMethod*>(copy));
  } else if (orig->IsClass() && orig->AsClass()->IsArtMethodClass()) {
    // Set the right size for the target.
    size_t size = mirror::ArtMethod::InstanceSize(target_ptr_size_);
    down_cast<mirror::Class*>(copy)->SetObjectSizeWithoutChecks(size);
  }
}

const byte* ImageWriter::GetQuickCode(mirror::ArtMethod* method, bool* quick_is_interpreted) {
  DCHECK(!method->IsResolutionMethod() && !method->IsImtConflictMethod() &&
         !method->IsImtUnimplementedMethod() && !method->IsAbstract()) << PrettyMethod(method);

  // Use original code if it exists. Otherwise, set the code pointer to the resolution
  // trampoline.

  // Quick entrypoint:
  const byte* quick_code = GetOatAddress(method->GetQuickOatCodeOffset());
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

const byte* ImageWriter::GetQuickEntryPoint(mirror::ArtMethod* method) {
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
#if defined(ART_USE_PORTABLE_COMPILER)
  copy->SetEntryPointFromPortableCompiledCodePtrSize<kVerifyNone>(
      orig->GetEntryPointFromPortableCompiledCode(), target_ptr_size_);
#endif
  copy->SetEntryPointFromInterpreterPtrSize<kVerifyNone>(orig->GetEntryPointFromInterpreter(),
                                                         target_ptr_size_);
  copy->SetEntryPointFromJniPtrSize<kVerifyNone>(orig->GetEntryPointFromJni(), target_ptr_size_);
  copy->SetEntryPointFromQuickCompiledCodePtrSize<kVerifyNone>(
      orig->GetEntryPointFromQuickCompiledCode(), target_ptr_size_);

  // The resolution method has a special trampoline to call.
  Runtime* runtime = Runtime::Current();
  if (UNLIKELY(orig == runtime->GetResolutionMethod())) {
#if defined(ART_USE_PORTABLE_COMPILER)
    copy->SetEntryPointFromPortableCompiledCodePtrSize<kVerifyNone>(
        GetOatAddress(portable_resolution_trampoline_offset_), target_ptr_size_);
#endif
    copy->SetEntryPointFromQuickCompiledCodePtrSize<kVerifyNone>(
        GetOatAddress(quick_resolution_trampoline_offset_), target_ptr_size_);
  } else if (UNLIKELY(orig == runtime->GetImtConflictMethod() ||
                      orig == runtime->GetImtUnimplementedMethod())) {
#if defined(ART_USE_PORTABLE_COMPILER)
    copy->SetEntryPointFromPortableCompiledCode<kVerifyNone>(
        GetOatAddress(portable_imt_conflict_trampoline_offset_), target_ptr_size_);
#endif
    copy->SetEntryPointFromQuickCompiledCodePtrSize<kVerifyNone>(
        GetOatAddress(quick_imt_conflict_trampoline_offset_), target_ptr_size_);
  } else {
    // We assume all methods have code. If they don't currently then we set them to the use the
    // resolution trampoline. Abstract methods never have code and so we need to make sure their
    // use results in an AbstractMethodError. We use the interpreter to achieve this.
    if (UNLIKELY(orig->IsAbstract())) {
#if defined(ART_USE_PORTABLE_COMPILER)
      copy->SetEntryPointFromPortableCompiledCode<kVerifyNone>(
          GetOatAddress(portable_to_interpreter_bridge_offset_), target_ptr_size_);
#endif
      copy->SetEntryPointFromQuickCompiledCodePtrSize<kVerifyNone>(
          GetOatAddress(quick_to_interpreter_bridge_offset_), target_ptr_size_);
      copy->SetEntryPointFromInterpreterPtrSize<kVerifyNone>(
          reinterpret_cast<EntryPointFromInterpreter*>(const_cast<byte*>(
              GetOatAddress(interpreter_to_interpreter_bridge_offset_))), target_ptr_size_);
    } else {
      bool quick_is_interpreted;
      const byte* quick_code = GetQuickCode(orig, &quick_is_interpreted);
      copy->SetEntryPointFromQuickCompiledCodePtrSize<kVerifyNone>(quick_code, target_ptr_size_);

      // Portable entrypoint:
      bool portable_is_interpreted = false;
#if defined(ART_USE_PORTABLE_COMPILER)
      const byte* portable_code = GetOatAddress(orig->GetPortableOatCodeOffset());
      if (portable_code != nullptr && (!orig->IsStatic() || orig->IsConstructor() ||
          orig->GetDeclaringClass()->IsInitialized())) {
        // We have code for a non-static or initialized method, just use the code.
      } else if (portable_code == nullptr && orig->IsNative() &&
          (!orig->IsStatic() || orig->GetDeclaringClass()->IsInitialized())) {
        // Non-static or initialized native method missing compiled code, use generic JNI version.
        // TODO: generic JNI support for LLVM.
        portable_code = GetOatAddress(portable_resolution_trampoline_offset_);
      } else if (portable_code == nullptr && !orig->IsNative()) {
        // We don't have code at all for a non-native method, use the interpreter.
        portable_code = GetOatAddress(portable_to_interpreter_bridge_offset_);
        portable_is_interpreted = true;
      } else {
        CHECK(!orig->GetDeclaringClass()->IsInitialized());
        // We have code for a static method, but need to go through the resolution stub for class
        // initialization.
        portable_code = GetOatAddress(portable_resolution_trampoline_offset_);
      }
      copy->SetEntryPointFromPortableCompiledCodePtrSize<kVerifyNone>(
          portable_code, target_ptr_size_);
#endif
      // JNI entrypoint:
      if (orig->IsNative()) {
        // The native method's pointer is set to a stub to lookup via dlsym.
        // Note this is not the code_ pointer, that is handled above.
        copy->SetEntryPointFromJniPtrSize<kVerifyNone>(GetOatAddress(jni_dlsym_lookup_offset_),
                                                       target_ptr_size_);
      }

      // Interpreter entrypoint:
      // Set the interpreter entrypoint depending on whether there is compiled code or not.
      uint32_t interpreter_code = (quick_is_interpreted && portable_is_interpreted)
          ? interpreter_to_interpreter_bridge_offset_
          : interpreter_to_compiled_code_bridge_offset_;
      EntryPointFromInterpreter* interpreter_entrypoint =
          reinterpret_cast<EntryPointFromInterpreter*>(
              const_cast<byte*>(GetOatAddress(interpreter_code)));
      copy->SetEntryPointFromInterpreterPtrSize<kVerifyNone>(
          interpreter_entrypoint, target_ptr_size_);
    }
  }
}

static OatHeader* GetOatHeaderFromElf(ElfFile* elf) {
  Elf32_Shdr* data_sec = elf->FindSectionByName(".rodata");
  if (data_sec == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<OatHeader*>(elf->Begin() + data_sec->sh_offset);
}

void ImageWriter::PatchOatCodeAndMethods(File* elf_file) {
  std::string error_msg;
  std::unique_ptr<ElfFile> elf(ElfFile::Open(elf_file, PROT_READ|PROT_WRITE,
                                             MAP_SHARED, &error_msg));
  if (elf.get() == nullptr) {
    LOG(FATAL) << "Unable patch oat file: " << error_msg;
    return;
  }
  if (!ElfPatcher::Patch(&compiler_driver_, elf.get(), oat_file_,
                         reinterpret_cast<uintptr_t>(oat_data_begin_),
                         GetImageAddressCallback, reinterpret_cast<void*>(this),
                         &error_msg)) {
    LOG(FATAL) << "unable to patch oat file: " << error_msg;
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
  static_assert(kBinBits == 3, "wrong number of bin bits");
  static_assert(kBinShift == 29, "wrong number of shift");
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

}  // namespace art
