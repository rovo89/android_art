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

#include <vector>

#include "base/logging.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "compiled_method.h"
#include "dex_file-inl.h"
#include "driver/compiler_driver.h"
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
#include "object_utils.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "handle_scope-inl.h"
#include "UniquePtrCompat.h"
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

  UniquePtr<File> oat_file(OS::OpenFileReadWrite(oat_filename.c_str()));
  if (oat_file.get() == NULL) {
    LOG(ERROR) << "Failed to open oat file " << oat_filename << " for " << oat_location;
    return false;
  }
  std::string error_msg;
  oat_file_ = OatFile::OpenWritable(oat_file.get(), oat_location, &error_msg);
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
  PatchOatCodeAndMethods();
  Thread::Current()->TransitionFromRunnableToSuspended(kNative);

  UniquePtr<File> image_file(OS::CreateEmptyFile(image_filename.c_str()));
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
  for (DexCache* dex_cache : Runtime::Current()->GetClassLinker()->GetDexCaches()) {
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
  return compiler_driver_.IsImageClass(klass->GetDescriptor().c_str());
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
  for (DexCache* dex_cache : class_linker->GetDexCaches()) {
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
    context->non_image_classes->insert(klass->GetDescriptor());
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
      CHECK(image_writer->IsImageClass(klass)) << klass->GetDescriptor()
                                               << " " << PrettyDescriptor(klass);
    }
  }
}

void ImageWriter::DumpImageClasses() {
  CompilerDriver::DescriptorSet* image_classes = compiler_driver_.GetImageClasses();
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

  // build an Object[] of all the DexCaches used in the source_space_
  Handle<ObjectArray<Object>> dex_caches(
      hs.NewHandle(ObjectArray<Object>::Alloc(self, object_array_class.Get(),
                                              class_linker->GetDexCaches().size())));
  int i = 0;
  for (DexCache* dex_cache : class_linker->GetDexCaches()) {
    dex_caches->Set<false>(i++, dex_cache);
  }

  // build an Object[] of the roots needed to restore the runtime
  Handle<ObjectArray<Object> > image_roots(hs.NewHandle(
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

 private:
  ImageWriter* const image_writer_;
  mirror::Object* const copy_;
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
  FixupVisitor visitor(this, copy);
  orig->VisitReferences<true /*visit class*/>(visitor, visitor);
  if (orig->IsArtMethod<kVerifyNone>()) {
    FixupMethod(orig->AsArtMethod<kVerifyNone>(), down_cast<ArtMethod*>(copy));
  }
}

void ImageWriter::FixupMethod(ArtMethod* orig, ArtMethod* copy) {
  // OatWriter replaces the code_ with an offset value. Here we re-adjust to a pointer relative to
  // oat_begin_

  // The resolution method has a special trampoline to call.
  if (UNLIKELY(orig == Runtime::Current()->GetResolutionMethod())) {
    copy->SetEntryPointFromPortableCompiledCode<kVerifyNone>(GetOatAddress(portable_resolution_trampoline_offset_));
    copy->SetEntryPointFromQuickCompiledCode<kVerifyNone>(GetOatAddress(quick_resolution_trampoline_offset_));
  } else if (UNLIKELY(orig == Runtime::Current()->GetImtConflictMethod())) {
    copy->SetEntryPointFromPortableCompiledCode<kVerifyNone>(GetOatAddress(portable_imt_conflict_trampoline_offset_));
    copy->SetEntryPointFromQuickCompiledCode<kVerifyNone>(GetOatAddress(quick_imt_conflict_trampoline_offset_));
  } else {
    // We assume all methods have code. If they don't currently then we set them to the use the
    // resolution trampoline. Abstract methods never have code and so we need to make sure their
    // use results in an AbstractMethodError. We use the interpreter to achieve this.
    if (UNLIKELY(orig->IsAbstract())) {
      copy->SetEntryPointFromPortableCompiledCode<kVerifyNone>(GetOatAddress(portable_to_interpreter_bridge_offset_));
      copy->SetEntryPointFromQuickCompiledCode<kVerifyNone>(GetOatAddress(quick_to_interpreter_bridge_offset_));
      copy->SetEntryPointFromInterpreter<kVerifyNone>(reinterpret_cast<EntryPointFromInterpreter*>
          (const_cast<byte*>(GetOatAddress(interpreter_to_interpreter_bridge_offset_))));
    } else {
      copy->SetEntryPointFromInterpreter<kVerifyNone>(reinterpret_cast<EntryPointFromInterpreter*>
          (const_cast<byte*>(GetOatAddress(interpreter_to_compiled_code_bridge_offset_))));
      // Use original code if it exists. Otherwise, set the code pointer to the resolution
      // trampoline.
      const byte* quick_code = GetOatAddress(orig->GetQuickOatCodeOffset());
      if (quick_code != nullptr &&
          (!orig->IsStatic() || orig->IsConstructor() || orig->GetDeclaringClass()->IsInitialized())) {
        // We have code for a non-static or initialized method, just use the code.
        copy->SetEntryPointFromQuickCompiledCode<kVerifyNone>(quick_code);
      } else if (quick_code == nullptr && orig->IsNative() &&
          (!orig->IsStatic() || orig->GetDeclaringClass()->IsInitialized())) {
        // Non-static or initialized native method missing compiled code, use generic JNI version.
        copy->SetEntryPointFromQuickCompiledCode<kVerifyNone>(GetOatAddress(quick_generic_jni_trampoline_offset_));
      } else if (quick_code == nullptr && !orig->IsNative()) {
        // We don't have code at all for a non-native method, use the interpreter.
        copy->SetEntryPointFromQuickCompiledCode<kVerifyNone>(GetOatAddress(quick_to_interpreter_bridge_offset_));
      } else {
        CHECK(!orig->GetDeclaringClass()->IsInitialized());
        // We have code for a static method, but need to go through the resolution stub for class
        // initialization.
        copy->SetEntryPointFromQuickCompiledCode<kVerifyNone>(GetOatAddress(quick_resolution_trampoline_offset_));
      }
      const byte* portable_code = GetOatAddress(orig->GetPortableOatCodeOffset());
      if (portable_code != nullptr) {
        copy->SetEntryPointFromPortableCompiledCode<kVerifyNone>(portable_code);
      } else {
        copy->SetEntryPointFromPortableCompiledCode<kVerifyNone>(GetOatAddress(portable_resolution_trampoline_offset_));
      }
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
    }
  }
}

static ArtMethod* GetTargetMethod(const CompilerDriver::CallPatchInformation* patch)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  StackHandleScope<2> hs(Thread::Current());
  Handle<mirror::DexCache> dex_cache(
      hs.NewHandle(class_linker->FindDexCache(*patch->GetTargetDexFile())));
  auto class_loader(hs.NewHandle<mirror::ClassLoader>(nullptr));
  ArtMethod* method = class_linker->ResolveMethod(*patch->GetTargetDexFile(),
                                                  patch->GetTargetMethodIdx(),
                                                  dex_cache,
                                                  class_loader,
                                                  NULL,
                                                  patch->GetTargetInvokeType());
  CHECK(method != NULL)
    << patch->GetTargetDexFile()->GetLocation() << " " << patch->GetTargetMethodIdx();
  CHECK(!method->IsRuntimeMethod())
    << patch->GetTargetDexFile()->GetLocation() << " " << patch->GetTargetMethodIdx();
  CHECK(dex_cache->GetResolvedMethods()->Get(patch->GetTargetMethodIdx()) == method)
    << patch->GetTargetDexFile()->GetLocation() << " " << patch->GetReferrerMethodIdx() << " "
    << PrettyMethod(dex_cache->GetResolvedMethods()->Get(patch->GetTargetMethodIdx())) << " "
    << PrettyMethod(method);
  return method;
}

static Class* GetTargetType(const CompilerDriver::TypePatchInformation* patch)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  StackHandleScope<2> hs(Thread::Current());
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(class_linker->FindDexCache(patch->GetDexFile())));
  auto class_loader(hs.NewHandle<mirror::ClassLoader>(nullptr));
  Class* klass = class_linker->ResolveType(patch->GetDexFile(),
                                           patch->GetTargetTypeIdx(),
                                           dex_cache,
                                           class_loader);
  CHECK(klass != NULL)
    << patch->GetDexFile().GetLocation() << " " << patch->GetTargetTypeIdx();
  CHECK(dex_cache->GetResolvedTypes()->Get(patch->GetTargetTypeIdx()) == klass)
    << patch->GetDexFile().GetLocation() << " " << patch->GetReferrerMethodIdx() << " "
    << PrettyClass(dex_cache->GetResolvedTypes()->Get(patch->GetTargetTypeIdx())) << " "
    << PrettyClass(klass);
  return klass;
}

void ImageWriter::PatchOatCodeAndMethods() {
  Thread* self = Thread::Current();
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const char* old_cause = self->StartAssertNoThreadSuspension("ImageWriter");

  typedef std::vector<const CompilerDriver::CallPatchInformation*> CallPatches;
  const CallPatches& code_to_patch = compiler_driver_.GetCodeToPatch();
  for (size_t i = 0; i < code_to_patch.size(); i++) {
    const CompilerDriver::CallPatchInformation* patch = code_to_patch[i];
    ArtMethod* target = GetTargetMethod(patch);
    uintptr_t quick_code = reinterpret_cast<uintptr_t>(class_linker->GetQuickOatCodeFor(target));
    uintptr_t code_base = reinterpret_cast<uintptr_t>(&oat_file_->GetOatHeader());
    uintptr_t code_offset = quick_code - code_base;
    if (patch->IsRelative()) {
      // value to patch is relative to the location being patched
      const void* quick_oat_code =
        class_linker->GetQuickOatCodeFor(patch->GetDexFile(),
                                         patch->GetReferrerClassDefIdx(),
                                         patch->GetReferrerMethodIdx());
      uintptr_t base = reinterpret_cast<uintptr_t>(quick_oat_code);
      uintptr_t patch_location = base + patch->GetLiteralOffset();
      uintptr_t value = quick_code - patch_location + patch->RelativeOffset();
      SetPatchLocation(patch, value);
    } else {
      if (quick_code == reinterpret_cast<uintptr_t>(GetQuickToInterpreterBridge()) ||
          quick_code == reinterpret_cast<uintptr_t>(class_linker->GetQuickGenericJniTrampoline())) {
        if (target->IsNative()) {
          // generic JNI, not interpreter bridge from GetQuickOatCodeFor().
          code_offset = quick_generic_jni_trampoline_offset_;
        } else {
          code_offset = quick_to_interpreter_bridge_offset_;
        }
      }
      SetPatchLocation(patch, PointerToLowMemUInt32(GetOatAddress(code_offset)));
    }
  }

  const CallPatches& methods_to_patch = compiler_driver_.GetMethodsToPatch();
  for (size_t i = 0; i < methods_to_patch.size(); i++) {
    const CompilerDriver::CallPatchInformation* patch = methods_to_patch[i];
    ArtMethod* target = GetTargetMethod(patch);
    SetPatchLocation(patch, PointerToLowMemUInt32(GetImageAddress(target)));
  }

  const std::vector<const CompilerDriver::TypePatchInformation*>& classes_to_patch =
      compiler_driver_.GetClassesToPatch();
  for (size_t i = 0; i < classes_to_patch.size(); i++) {
    const CompilerDriver::TypePatchInformation* patch = classes_to_patch[i];
    Class* target = GetTargetType(patch);
    SetPatchLocation(patch, PointerToLowMemUInt32(GetImageAddress(target)));
  }

  // Update the image header with the new checksum after patching
  ImageHeader* image_header = reinterpret_cast<ImageHeader*>(image_->Begin());
  image_header->SetOatChecksum(oat_file_->GetOatHeader().GetChecksum());
  self->EndAssertNoThreadSuspension(old_cause);
}

void ImageWriter::SetPatchLocation(const CompilerDriver::PatchInformation* patch, uint32_t value) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const void* quick_oat_code = class_linker->GetQuickOatCodeFor(patch->GetDexFile(),
                                                                patch->GetReferrerClassDefIdx(),
                                                                patch->GetReferrerMethodIdx());
  OatHeader& oat_header = const_cast<OatHeader&>(oat_file_->GetOatHeader());
  // TODO: make this Thumb2 specific
  uint8_t* base = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(quick_oat_code) & ~0x1);
  uint32_t* patch_location = reinterpret_cast<uint32_t*>(base + patch->GetLiteralOffset());
  if (kIsDebugBuild) {
    if (patch->IsCall()) {
      const CompilerDriver::CallPatchInformation* cpatch = patch->AsCall();
      const DexFile::MethodId& id = cpatch->GetTargetDexFile()->GetMethodId(cpatch->GetTargetMethodIdx());
      uint32_t expected = reinterpret_cast<uintptr_t>(&id) & 0xFFFFFFFF;
      uint32_t actual = *patch_location;
      CHECK(actual == expected || actual == value) << std::hex
          << "actual=" << actual
          << "expected=" << expected
          << "value=" << value;
    }
    if (patch->IsType()) {
      const CompilerDriver::TypePatchInformation* tpatch = patch->AsType();
      const DexFile::TypeId& id = tpatch->GetDexFile().GetTypeId(tpatch->GetTargetTypeIdx());
      uint32_t expected = reinterpret_cast<uintptr_t>(&id) & 0xFFFFFFFF;
      uint32_t actual = *patch_location;
      CHECK(actual == expected || actual == value) << std::hex
          << "actual=" << actual
          << "expected=" << expected
          << "value=" << value;
    }
  }
  *patch_location = value;
  oat_header.UpdateChecksum(patch_location, sizeof(value));
}

}  // namespace art
