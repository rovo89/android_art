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
#include "utils.h"

using ::art::mirror::ArtField;
using ::art::mirror::ArtMethod;
using ::art::mirror::Class;
using ::art::mirror::DexCache;
using ::art::mirror::EntryPointFromInterpreter;
using ::art::mirror::Object;
using ::art::mirror::ObjectArray;
using ::art::mirror::String;

namespace art {

bool ImageWriter::Write(const std::string& image_filename,
                        uintptr_t image_begin,
                        const std::string& oat_filename,
                        const std::string& oat_location) {
  CHECK(!image_filename.empty());

  CHECK_NE(image_begin, 0U);
  image_begin_ = reinterpret_cast<byte*>(image_begin);

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

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
    ComputeEagerResolvedStrings();
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
  Thread::Current()->TransitionFromRunnableToSuspended(kNative);

  std::unique_ptr<File> image_file(OS::CreateEmptyFile(image_filename.c_str()));
  ImageHeader* image_header = reinterpret_cast<ImageHeader*>(image_->Begin());
  if (image_file.get() == NULL) {
    LOG(ERROR) << "Failed to open image file " << image_filename;
    return false;
  }
  if (fchmod(image_file->Fd(), 0644) != 0) {
    PLOG(ERROR) << "Failed to make image file world readable: " << image_filename;
    return EXIT_FAILURE;
  }

  // Write out the image.
  CHECK_EQ(image_end_, image_header->GetImageSize());
  if (!image_file->WriteFully(image_->Begin(), image_end_)) {
    PLOG(ERROR) << "Failed to write image file " << image_filename;
    return false;
  }

  // Write out the image bitmap at the page aligned start of the image end.
  CHECK_ALIGNED(image_header->GetImageBitmapOffset(), kPageSize);
  if (!image_file->Write(reinterpret_cast<char*>(image_bitmap_->Begin()),
                         image_header->GetImageBitmapSize(),
                         image_header->GetImageBitmapOffset())) {
    PLOG(ERROR) << "Failed to write image file " << image_filename;
    return false;
  }

  return true;
}

void ImageWriter::SetImageOffset(mirror::Object* object, size_t offset) {
  DCHECK(object != nullptr);
  DCHECK_NE(offset, 0U);
  DCHECK(!IsImageOffsetAssigned(object));
  mirror::Object* obj = reinterpret_cast<mirror::Object*>(image_->Begin() + offset);
  DCHECK_ALIGNED(obj, kObjectAlignment);
  image_bitmap_->Set(obj);
  // Before we stomp over the lock word, save the hash code for later.
  Monitor::Deflate(Thread::Current(), object);;
  LockWord lw(object->GetLockWord(false));
  switch (lw.GetState()) {
    case LockWord::kFatLocked: {
      LOG(FATAL) << "Fat locked object " << obj << " found during object copy";
      break;
    }
    case LockWord::kThinLocked: {
      LOG(FATAL) << "Thin locked object " << obj << " found during object copy";
      break;
    }
    case LockWord::kUnlocked:
      // No hash, don't need to save it.
      break;
    case LockWord::kHashCode:
      saved_hashes_.push_back(std::make_pair(obj, lw.GetHashCode()));
      break;
    default:
      LOG(FATAL) << "Unreachable.";
      break;
  }
  object->SetLockWord(LockWord::FromForwardingAddress(offset), false);
  DCHECK(IsImageOffsetAssigned(object));
}

void ImageWriter::AssignImageOffset(mirror::Object* object) {
  DCHECK(object != nullptr);
  SetImageOffset(object, image_end_);
  image_end_ += RoundUp(object->SizeOf(), 8);  // 64-bit alignment
  DCHECK_LT(image_end_, image_->Size());
}

bool ImageWriter::IsImageOffsetAssigned(mirror::Object* object) const {
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

void ImageWriter::ComputeEagerResolvedStrings() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
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
    class_linker->RemoveClass(it.c_str(), NULL);
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

void ImageWriter::CheckNonImageClassesRemoved()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
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

void ImageWriter::CalculateObjectOffsets(Object* obj) {
  DCHECK(obj != NULL);
  // if it is a string, we want to intern it if its not interned.
  if (obj->GetClass()->IsStringClass()) {
    // we must be an interned string that was forward referenced and already assigned
    if (IsImageOffsetAssigned(obj)) {
      DCHECK_EQ(obj, obj->AsString()->Intern());
      return;
    }
    mirror::String* const interned = obj->AsString()->Intern();
    if (obj != interned) {
      if (!IsImageOffsetAssigned(interned)) {
        // interned obj is after us, allocate its location early
        AssignImageOffset(interned);
      }
      // point those looking for this object to the interned version.
      SetImageOffset(obj, GetImageOffset(interned));
      return;
    }
    // else (obj == interned), nothing to do but fall through to the normal case
  }

  AssignImageOffset(obj);
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
  for (size_t i = 0; i < num_reference_fields; ++i) {
    mirror::ArtField* field = h_class->GetInstanceField(i);
    MemberOffset field_offset = field->GetOffset();
    mirror::Object* value = obj->GetFieldObject<mirror::Object>(field_offset);
    if (value != nullptr) {
      WalkFieldsInOrder(value);
    }
  }
}

// For an unvisited object, visit it then all its children found via fields.
void ImageWriter::WalkFieldsInOrder(mirror::Object* obj) {
  if (!IsImageOffsetAssigned(obj)) {
    // Walk instance fields of all objects
    StackHandleScope<2> hs(Thread::Current());
    Handle<mirror::Object> h_obj(hs.NewHandle(obj));
    Handle<mirror::Class> klass(hs.NewHandle(obj->GetClass()));
    // visit the object itself.
    CalculateObjectOffsets(h_obj.Get());
    WalkInstanceFields(h_obj.Get(), klass.Get());
    // Walk static fields of a Class.
    if (h_obj->IsClass()) {
      size_t num_static_fields = klass->NumReferenceStaticFields();
      for (size_t i = 0; i < num_static_fields; ++i) {
        mirror::ArtField* field = klass->GetStaticField(i);
        MemberOffset field_offset = field->GetOffset();
        mirror::Object* value = h_obj->GetFieldObject<mirror::Object>(field_offset);
        if (value != nullptr) {
          WalkFieldsInOrder(value);
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

void ImageWriter::CalculateNewObjectOffsets(size_t oat_loaded_size, size_t oat_data_offset) {
  CHECK_NE(0U, oat_loaded_size);
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<ObjectArray<Object>> image_roots(hs.NewHandle(CreateImageRoots()));

  gc::Heap* heap = Runtime::Current()->GetHeap();
  DCHECK_EQ(0U, image_end_);

  // Leave space for the header, but do not write it yet, we need to
  // know where image_roots is going to end up
  image_end_ += RoundUp(sizeof(ImageHeader), 8);  // 64-bit-alignment

  {
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    // TODO: Image spaces only?
    const char* old = self->StartAssertNoThreadSuspension("ImageWriter");
    DCHECK_LT(image_end_, image_->Size());
    // Clear any pre-existing monitors which may have been in the monitor words.
    heap->VisitObjects(WalkFieldsCallback, this);
    self->EndAssertNoThreadSuspension(old);
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
                           PointerToLowMemUInt32(oat_file_end));
  memcpy(image_->Begin(), &image_header, sizeof(image_header));

  // Note that image_end_ is left at end of used space
}

void ImageWriter::CopyAndFixupObjects()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
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
  size_t n = obj->SizeOf();
  DCHECK_LT(offset + n, image_writer->image_->Size());
  memcpy(dst, src, n);
  Object* copy = reinterpret_cast<Object*>(dst);
  // Write in a hash code of objects which have inflated monitors or a hash code in their monitor
  // word.
  copy->SetLockWord(LockWord(), false);
  image_writer->FixupObject(obj, copy);
}

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
    FixupVisitor::operator()(obj, offset, false);

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
  }
}

const byte* ImageWriter::GetQuickCode(mirror::ArtMethod* method, bool* quick_is_interpreted) {
  DCHECK(!method->IsResolutionMethod() && !method->IsImtConflictMethod() &&
         !method->IsAbstract()) << PrettyMethod(method);

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
  if (UNLIKELY(method == Runtime::Current()->GetResolutionMethod())) {
    return GetOatAddress(quick_resolution_trampoline_offset_);
  } else if (UNLIKELY(method == Runtime::Current()->GetImtConflictMethod())) {
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

  // The resolution method has a special trampoline to call.
  if (UNLIKELY(orig == Runtime::Current()->GetResolutionMethod())) {
#if defined(ART_USE_PORTABLE_COMPILER)
    copy->SetEntryPointFromPortableCompiledCode<kVerifyNone>(GetOatAddress(portable_resolution_trampoline_offset_));
#endif
    copy->SetEntryPointFromQuickCompiledCode<kVerifyNone>(GetOatAddress(quick_resolution_trampoline_offset_));
  } else if (UNLIKELY(orig == Runtime::Current()->GetImtConflictMethod())) {
#if defined(ART_USE_PORTABLE_COMPILER)
    copy->SetEntryPointFromPortableCompiledCode<kVerifyNone>(GetOatAddress(portable_imt_conflict_trampoline_offset_));
#endif
    copy->SetEntryPointFromQuickCompiledCode<kVerifyNone>(GetOatAddress(quick_imt_conflict_trampoline_offset_));
  } else {
    // We assume all methods have code. If they don't currently then we set them to the use the
    // resolution trampoline. Abstract methods never have code and so we need to make sure their
    // use results in an AbstractMethodError. We use the interpreter to achieve this.
    if (UNLIKELY(orig->IsAbstract())) {
#if defined(ART_USE_PORTABLE_COMPILER)
      copy->SetEntryPointFromPortableCompiledCode<kVerifyNone>(GetOatAddress(portable_to_interpreter_bridge_offset_));
#endif
      copy->SetEntryPointFromQuickCompiledCode<kVerifyNone>(GetOatAddress(quick_to_interpreter_bridge_offset_));
      copy->SetEntryPointFromInterpreter<kVerifyNone>(reinterpret_cast<EntryPointFromInterpreter*>
          (const_cast<byte*>(GetOatAddress(interpreter_to_interpreter_bridge_offset_))));
    } else {
      bool quick_is_interpreted;
      const byte* quick_code = GetQuickCode(orig, &quick_is_interpreted);
      copy->SetEntryPointFromQuickCompiledCode<kVerifyNone>(quick_code);

      // Portable entrypoint:
      bool portable_is_interpreted = false;
#if defined(ART_USE_PORTABLE_COMPILER)
      const byte* portable_code = GetOatAddress(orig->GetPortableOatCodeOffset());
      if (portable_code != nullptr &&
          (!orig->IsStatic() || orig->IsConstructor() || orig->GetDeclaringClass()->IsInitialized())) {
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
      copy->SetEntryPointFromPortableCompiledCode<kVerifyNone>(portable_code);
#endif
      // JNI entrypoint:
      if (orig->IsNative()) {
        // The native method's pointer is set to a stub to lookup via dlsym.
        // Note this is not the code_ pointer, that is handled above.
        copy->SetNativeMethod<kVerifyNone>(GetOatAddress(jni_dlsym_lookup_offset_));
      } else {
        // Normal (non-abstract non-native) methods have various tables to relocate.
        uint32_t native_gc_map_offset = orig->GetOatNativeGcMapOffset();
        const byte* native_gc_map = GetOatAddress(native_gc_map_offset);
        copy->SetNativeGcMap<kVerifyNone>(reinterpret_cast<const uint8_t*>(native_gc_map));
      }

      // Interpreter entrypoint:
      // Set the interpreter entrypoint depending on whether there is compiled code or not.
      uint32_t interpreter_code = (quick_is_interpreted && portable_is_interpreted)
          ? interpreter_to_interpreter_bridge_offset_
          : interpreter_to_compiled_code_bridge_offset_;
      copy->SetEntryPointFromInterpreter<kVerifyNone>(
          reinterpret_cast<EntryPointFromInterpreter*>(
              const_cast<byte*>(GetOatAddress(interpreter_code))));
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

}  // namespace art
