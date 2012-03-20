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

#include <sys/mman.h>

#include <vector>

#include "UniquePtr.h"
#include "class_linker.h"
#include "class_loader.h"
#include "compiled_method.h"
#include "compiler.h"
#include "dex_cache.h"
#include "file.h"
#include "globals.h"
#include "heap.h"
#include "image.h"
#include "intern_table.h"
#include "logging.h"
#include "object.h"
#include "object_utils.h"
#include "runtime.h"
#include "space.h"
#include "utils.h"

namespace art {

bool ImageWriter::Write(const std::string& image_filename,
                        uintptr_t image_begin,
                        const std::string& oat_filename,
                        const std::string& oat_location,
                        const Compiler& compiler) {
  CHECK(!image_filename.empty());

  CHECK_NE(image_begin, 0U);
  image_begin_ = reinterpret_cast<byte*>(image_begin);

  Heap* heap = Runtime::Current()->GetHeap();
  source_space_ = heap->GetAllocSpace();

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const std::vector<DexCache*>& all_dex_caches = class_linker->GetDexCaches();
  for (size_t i = 0; i < all_dex_caches.size(); i++) {
    DexCache* dex_cache = all_dex_caches[i];
    if (InSourceSpace(dex_cache)) {
      dex_caches_.insert(dex_cache);
    }
  }

  oat_file_ = OatFile::Open(oat_filename, oat_location, NULL, true);
  if (oat_file_ == NULL) {
    LOG(ERROR) << "Failed to open oat file " << oat_filename;
    return false;
  }
   class_linker->RegisterOatFile(*oat_file_);

  PruneNonImageClasses();  // Remove junk
  ComputeLazyFieldsForImageClasses();  // Add useful information
  ComputeEagerResolvedStrings();
  heap->CollectGarbage(false);  // Remove garbage
  heap->GetAllocSpace()->Trim();  // Trim size of source_space
  if (!AllocMemory()) {
    return false;
  }
#ifndef NDEBUG
  CheckNonImageClassesRemoved();
#endif
  heap->DisableCardMarking();
  CalculateNewObjectOffsets();
  CopyAndFixupObjects();
  PatchOatCodeAndMethods(compiler);

  UniquePtr<File> file(OS::OpenFile(image_filename.c_str(), true));
  if (file.get() == NULL) {
    LOG(ERROR) << "Failed to open image file " << image_filename;
    return false;
  }
  bool success = file->WriteFully(image_->Begin(), image_end_);
  if (!success) {
    PLOG(ERROR) << "Failed to write image file " << image_filename;
    return false;
  }
  return true;
}

bool ImageWriter::AllocMemory() {
  size_t size = source_space_->Size();
  int prot = PROT_READ | PROT_WRITE;
  size_t length = RoundUp(size, kPageSize);
  image_.reset(MemMap::MapAnonymous("image-writer-image", NULL, length, prot));
  if (image_.get() == NULL) {
    LOG(ERROR) << "Failed to allocate memory for image file generation";
    return false;
  }
  return true;
}

void ImageWriter::ComputeLazyFieldsForImageClasses() {
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  class_linker->VisitClasses(ComputeLazyFieldsForClassesVisitor, NULL);

}

bool ImageWriter::ComputeLazyFieldsForClassesVisitor(Class* c, void* /*arg*/) {
  c->ComputeName();
  return true;
}

void ImageWriter::ComputeEagerResolvedStringsCallback(Object* obj, void* arg) {
  if (!obj->GetClass()->IsStringClass()) {
    return;
  }
  String* string = obj->AsString();
  std::string utf8_string(string->ToModifiedUtf8());
  ImageWriter* writer = reinterpret_cast<ImageWriter*>(arg);
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  typedef Set::const_iterator CacheIt;  // TODO: C++0x auto
  for (CacheIt it = writer->dex_caches_.begin(), end = writer->dex_caches_.end(); it != end; ++it) {
    DexCache* dex_cache = *it;
    const DexFile& dex_file = linker->FindDexFile(dex_cache);
    const DexFile::StringId* string_id = dex_file.FindStringId(utf8_string);
    if (string_id != NULL) {
      // This string occurs in this dex file, assign the dex cache entry.
      uint32_t string_idx = dex_file.GetIndexForStringId(*string_id);
      if (dex_cache->GetResolvedString(string_idx) == NULL) {
        dex_cache->SetResolvedString(string_idx, string);
      }
    }
  }
}

void ImageWriter::ComputeEagerResolvedStrings() {
  HeapBitmap* heap_bitmap = Runtime::Current()->GetHeap()->GetLiveBits();
  DCHECK(heap_bitmap != NULL);
  heap_bitmap->Walk(ComputeEagerResolvedStringsCallback, this);  // TODO: add Space-limited Walk
}

bool ImageWriter::IsImageClass(const Class* klass) {
  if (image_classes_ == NULL) {
    return true;
  }
  while (klass->IsArrayClass()) {
    klass = klass->GetComponentType();
  }
  if (klass->IsPrimitive()) {
    return true;
  }
  const std::string descriptor(ClassHelper(klass).GetDescriptor());
  return image_classes_->find(descriptor) != image_classes_->end();
}


struct NonImageClasses {
  ImageWriter* image_writer;
  std::set<std::string>* non_image_classes;
};

void ImageWriter::PruneNonImageClasses() {
  if (image_classes_ == NULL) {
    return;
  }
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();

  std::set<std::string> non_image_classes;
  NonImageClasses context;
  context.image_writer = this;
  context.non_image_classes = &non_image_classes;
  class_linker->VisitClasses(NonImageClassesVisitor, &context);

  typedef std::set<std::string>::const_iterator ClassIt;  // TODO: C++0x auto
  for (ClassIt it = non_image_classes.begin(), end = non_image_classes.end(); it != end; ++it) {
    class_linker->RemoveClass((*it).c_str(), NULL);
  }

  Method* resolution_method = runtime->GetResolutionMethod();
  typedef Set::const_iterator CacheIt;  // TODO: C++0x auto
  for (CacheIt it = dex_caches_.begin(), end = dex_caches_.end(); it != end; ++it) {
    DexCache* dex_cache = *it;
    for (size_t i = 0; i < dex_cache->NumResolvedTypes(); i++) {
      Class* klass = dex_cache->GetResolvedType(i);
      if (klass != NULL && !IsImageClass(klass)) {
        dex_cache->SetResolvedType(i, NULL);
        dex_cache->GetInitializedStaticStorage()->Set(i, NULL);
      }
    }
    for (size_t i = 0; i < dex_cache->NumResolvedMethods(); i++) {
      Method* method = dex_cache->GetResolvedMethod(i);
      if (method != NULL && !IsImageClass(method->GetDeclaringClass())) {
        dex_cache->SetResolvedMethod(i, resolution_method);
      }
    }
    for (size_t i = 0; i < dex_cache->NumResolvedFields(); i++) {
      Field* field = dex_cache->GetResolvedField(i);
      if (field != NULL && !IsImageClass(field->GetDeclaringClass())) {
        dex_cache->SetResolvedField(i, NULL);
      }
    }
  }
}

bool ImageWriter::NonImageClassesVisitor(Class* klass, void* arg) {
  NonImageClasses* context = reinterpret_cast<NonImageClasses*>(arg);
  if (!context->image_writer->IsImageClass(klass)) {
    context->non_image_classes->insert(ClassHelper(klass).GetDescriptor());
  }
  return true;
}

void ImageWriter::CheckNonImageClassesRemoved() {
  if (image_classes_ == NULL) {
    return;
  }
  Runtime::Current()->GetHeap()->GetLiveBits()->Walk(CheckNonImageClassesRemovedCallback, this);
}

void ImageWriter::CheckNonImageClassesRemovedCallback(Object* obj, void* arg) {
  ImageWriter* image_writer = reinterpret_cast<ImageWriter*>(arg);
  if (!obj->IsClass()) {
    return;
  }
  Class* klass = obj->AsClass();
  if (!image_writer->IsImageClass(klass)) {
    image_writer->DumpImageClasses();
    CHECK(image_writer->IsImageClass(klass)) << ClassHelper(klass).GetDescriptor()
                                             << " " << PrettyDescriptor(klass);
  }
}

void ImageWriter::DumpImageClasses() {
  typedef std::set<std::string>::const_iterator It;  // TODO: C++0x auto
  for (It it = image_classes_->begin(), end = image_classes_->end(); it != end; ++it) {
    LOG(INFO) << " " << *it;
  }
}

void ImageWriter::CalculateNewObjectOffsetsCallback(Object* obj, void* arg) {
  DCHECK(obj != NULL);
  DCHECK(arg != NULL);
  ImageWriter* image_writer = reinterpret_cast<ImageWriter*>(arg);
  if (!image_writer->InSourceSpace(obj)) {
    return;
  }

  // if it is a string, we want to intern it if its not interned.
  if (obj->GetClass()->IsStringClass()) {
    // we must be an interned string that was forward referenced and already assigned
    if (image_writer->IsImageOffsetAssigned(obj)) {
      DCHECK_EQ(obj, obj->AsString()->Intern());
      return;
    }
    SirtRef<String> interned(obj->AsString()->Intern());
    if (obj != interned.get()) {
      if (!image_writer->IsImageOffsetAssigned(interned.get())) {
        // interned obj is after us, allocate its location early
        image_writer->AssignImageOffset(interned.get());
      }
      // point those looking for this object to the interned version.
      image_writer->SetImageOffset(obj, image_writer->GetImageOffset(interned.get()));
      return;
    }
    // else (obj == interned), nothing to do but fall through to the normal case
  }

  image_writer->AssignImageOffset(obj);
}

ObjectArray<Object>* ImageWriter::CreateImageRoots() const {
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  Class* object_array_class = class_linker->FindSystemClass("[Ljava/lang/Object;");

  // build an Object[] of all the DexCaches used in the source_space_
  ObjectArray<Object>* dex_caches = ObjectArray<Object>::Alloc(object_array_class,
                                                               dex_caches_.size());
  int i = 0;
  typedef Set::const_iterator It;  // TODO: C++0x auto
  for (It it = dex_caches_.begin(), end = dex_caches_.end(); it != end; ++it, ++i) {
    dex_caches->Set(i, *it);
  }

  // build an Object[] of the roots needed to restore the runtime
  SirtRef<ObjectArray<Object> > image_roots(
      ObjectArray<Object>::Alloc(object_array_class, ImageHeader::kImageRootsMax));
  image_roots->Set(ImageHeader::kJniStubArray, runtime->GetJniDlsymLookupStub());
  image_roots->Set(ImageHeader::kAbstractMethodErrorStubArray,
                   runtime->GetAbstractMethodErrorStubArray());
  image_roots->Set(ImageHeader::kStaticResolutionStubArray,
                   runtime->GetResolutionStubArray(Runtime::kStaticMethod));
  image_roots->Set(ImageHeader::kUnknownMethodResolutionStubArray,
                   runtime->GetResolutionStubArray(Runtime::kUnknownMethod));
  image_roots->Set(ImageHeader::kResolutionMethod, runtime->GetResolutionMethod());
  image_roots->Set(ImageHeader::kCalleeSaveMethod,
                   runtime->GetCalleeSaveMethod(Runtime::kSaveAll));
  image_roots->Set(ImageHeader::kRefsOnlySaveMethod,
                   runtime->GetCalleeSaveMethod(Runtime::kRefsOnly));
  image_roots->Set(ImageHeader::kRefsAndArgsSaveMethod,
                   runtime->GetCalleeSaveMethod(Runtime::kRefsAndArgs));
  image_roots->Set(ImageHeader::kOatLocation,
                   String::AllocFromModifiedUtf8(oat_file_->GetLocation().c_str()));
  image_roots->Set(ImageHeader::kDexCaches,
                   dex_caches);
  image_roots->Set(ImageHeader::kClassRoots,
                   class_linker->GetClassRoots());
  for (int i = 0; i < ImageHeader::kImageRootsMax; i++) {
    CHECK(image_roots->Get(i) != NULL);
  }
  return image_roots.get();
}

void ImageWriter::CalculateNewObjectOffsets() {
  SirtRef<ObjectArray<Object> > image_roots(CreateImageRoots());

  HeapBitmap* heap_bitmap = Runtime::Current()->GetHeap()->GetLiveBits();
  DCHECK(heap_bitmap != NULL);
  DCHECK_EQ(0U, image_end_);

  // leave space for the header, but do not write it yet, we need to
  // know where image_roots is going to end up
  image_end_ += RoundUp(sizeof(ImageHeader), 8); // 64-bit-alignment

  heap_bitmap->InOrderWalk(CalculateNewObjectOffsetsCallback, this);  // TODO: add Space-limited Walk
  DCHECK_LT(image_end_, image_->Size());

  // Note that image_top_ is left at end of used space
  oat_begin_ = image_begin_ +  RoundUp(image_end_, kPageSize);
  const byte* oat_limit = oat_begin_ +  oat_file_->Size();

  // return to write header at start of image with future location of image_roots
  ImageHeader image_header(reinterpret_cast<uint32_t>(image_begin_),
                           reinterpret_cast<uint32_t>(GetImageAddress(image_roots.get())),
                           oat_file_->GetOatHeader().GetChecksum(),
                           reinterpret_cast<uint32_t>(oat_begin_),
                           reinterpret_cast<uint32_t>(oat_limit));
  memcpy(image_->Begin(), &image_header, sizeof(image_header));
}

void ImageWriter::CopyAndFixupObjects() {
  Heap* heap = Runtime::Current()->GetHeap();
  HeapBitmap* heap_bitmap = heap->GetLiveBits();
  DCHECK(heap_bitmap != NULL);
  // TODO: heap validation can't handle this fix up pass
  heap->DisableObjectValidation();
  heap_bitmap->Walk(CopyAndFixupObjectsCallback, this);  // TODO: add Space-limited Walk
}

void ImageWriter::CopyAndFixupObjectsCallback(Object* object, void* arg) {
  DCHECK(object != NULL);
  DCHECK(arg != NULL);
  const Object* obj = object;
  ImageWriter* image_writer = reinterpret_cast<ImageWriter*>(arg);
  if (!image_writer->InSourceSpace(object)) {
    return;
  }

  // see GetLocalAddress for similar computation
  size_t offset = image_writer->GetImageOffset(obj);
  byte* dst = image_writer->image_->Begin() + offset;
  const byte* src = reinterpret_cast<const byte*>(obj);
  size_t n = obj->SizeOf();
  DCHECK_LT(offset + n, image_writer->image_->Size());
  memcpy(dst, src, n);
  Object* copy = reinterpret_cast<Object*>(dst);
  copy->monitor_ = 0; // We may have inflated the lock during compilation.
  image_writer->FixupObject(obj, copy);
}

void ImageWriter::FixupObject(const Object* orig, Object* copy) {
  DCHECK(orig != NULL);
  DCHECK(copy != NULL);
  copy->SetClass(down_cast<Class*>(GetImageAddress(orig->GetClass())));
  // TODO: special case init of pointers to malloc data (or removal of these pointers)
  if (orig->IsClass()) {
    FixupClass(orig->AsClass(), down_cast<Class*>(copy));
  } else if (orig->IsObjectArray()) {
    FixupObjectArray(orig->AsObjectArray<Object>(), down_cast<ObjectArray<Object>*>(copy));
  } else if (orig->IsMethod()) {
    FixupMethod(orig->AsMethod(), down_cast<Method*>(copy));
  } else {
    FixupInstanceFields(orig, copy);
  }
}

void ImageWriter::FixupClass(const Class* orig, Class* copy) {
  FixupInstanceFields(orig, copy);
  FixupStaticFields(orig, copy);
}

void ImageWriter::FixupMethod(const Method* orig, Method* copy) {
  FixupInstanceFields(orig, copy);

  // OatWriter replaces the code_ and invoke_stub_ with offset values.
  // Here we readjust to a pointer relative to oat_begin_

  // Every type of method can have an invoke stub
  uint32_t invoke_stub_offset = orig->GetOatInvokeStubOffset();
  const byte* invoke_stub = GetOatAddress(invoke_stub_offset);
  copy->invoke_stub_ = reinterpret_cast<const Method::InvokeStub*>(invoke_stub);

  if (orig->IsAbstract()) {
    // Abstract methods are pointed to a stub that will throw AbstractMethodError if they are called
    ByteArray* orig_ame_stub_array_ = Runtime::Current()->GetAbstractMethodErrorStubArray();
    ByteArray* copy_ame_stub_array_ = down_cast<ByteArray*>(GetImageAddress(orig_ame_stub_array_));
    copy->code_ = copy_ame_stub_array_->GetData();
    return;
  }

  if (orig == Runtime::Current()->GetResolutionMethod()) {
    // The resolution stub's code points at the unknown resolution trampoline
    ByteArray* orig_res_stub_array_ =
        Runtime::Current()->GetResolutionStubArray(Runtime::kUnknownMethod);
    CHECK(orig->GetCode() == orig_res_stub_array_->GetData());
    ByteArray* copy_res_stub_array_ = down_cast<ByteArray*>(GetImageAddress(orig_res_stub_array_));
    copy->code_ = copy_res_stub_array_->GetData();
    return;
  }

  // Non-abstract methods typically have code
  uint32_t code_offset = orig->GetOatCodeOffset();
  const byte* code = NULL;
  if (orig->IsStatic()) {
    // Static methods may point at the resolution trampoline stub
    ByteArray* orig_res_stub_array_ =
        Runtime::Current()->GetResolutionStubArray(Runtime::kStaticMethod);
    if (reinterpret_cast<int8_t*>(code_offset) == orig_res_stub_array_->GetData()) {
      ByteArray* copy_res_stub_array_ = down_cast<ByteArray*>(GetImageAddress(orig_res_stub_array_));
      code = reinterpret_cast<const byte*>(copy_res_stub_array_->GetData());
    }
  }
  if (code == NULL) {
    code = GetOatAddress(code_offset);
  }
  copy->code_ = code;

  if (orig->IsNative()) {
    // The native method's pointer is directed to a stub to lookup via dlsym.
    // Note this is not the code_ pointer, that is handled above.
    ByteArray* orig_jni_stub_array_ = Runtime::Current()->GetJniDlsymLookupStub();
    ByteArray* copy_jni_stub_array_ = down_cast<ByteArray*>(GetImageAddress(orig_jni_stub_array_));
    copy->native_method_ = copy_jni_stub_array_->GetData();
  } else {
    // normal (non-abstract non-native) methods have mapping tables to relocate
    uint32_t mapping_table_off = orig->GetOatMappingTableOffset();
    const byte* mapping_table = GetOatAddress(mapping_table_off);
    copy->mapping_table_ = reinterpret_cast<const uint32_t*>(mapping_table);

    uint32_t vmap_table_offset = orig->GetOatVmapTableOffset();
    const byte* vmap_table = GetOatAddress(vmap_table_offset);
    copy->vmap_table_ = reinterpret_cast<const uint16_t*>(vmap_table);

    uint32_t gc_map_offset = orig->GetOatGcMapOffset();
    const byte* gc_map = GetOatAddress(gc_map_offset);
    copy->gc_map_ = reinterpret_cast<const uint8_t*>(gc_map);
  }
}

void ImageWriter::FixupObjectArray(const ObjectArray<Object>* orig, ObjectArray<Object>* copy) {
  for (int32_t i = 0; i < orig->GetLength(); ++i) {
    const Object* element = orig->Get(i);
    copy->SetWithoutChecks(i, GetImageAddress(element));
  }
}

void ImageWriter::FixupInstanceFields(const Object* orig, Object* copy) {
  DCHECK(orig != NULL);
  DCHECK(copy != NULL);
  Class* klass = orig->GetClass();
  DCHECK(klass != NULL);
  FixupFields(orig,
              copy,
              klass->GetReferenceInstanceOffsets(),
              false);
}

void ImageWriter::FixupStaticFields(const Class* orig, Class* copy) {
  DCHECK(orig != NULL);
  DCHECK(copy != NULL);
  FixupFields(orig,
              copy,
              orig->GetReferenceStaticOffsets(),
              true);
}

void ImageWriter::FixupFields(const Object* orig,
                              Object* copy,
                              uint32_t ref_offsets,
                              bool is_static) {
  if (ref_offsets != CLASS_WALK_SUPER) {
    // Found a reference offset bitmap.  Fixup the specified offsets.
    while (ref_offsets != 0) {
      size_t right_shift = CLZ(ref_offsets);
      MemberOffset byte_offset = CLASS_OFFSET_FROM_CLZ(right_shift);
      const Object* ref = orig->GetFieldObject<const Object*>(byte_offset, false);
      copy->SetFieldObject(byte_offset, GetImageAddress(ref), false);
      ref_offsets &= ~(CLASS_HIGH_BIT >> right_shift);
    }
  } else {
    // There is no reference offset bitmap.  In the non-static case,
    // walk up the class inheritance hierarchy and find reference
    // offsets the hard way. In the static case, just consider this
    // class.
    for (const Class *klass = is_static ? orig->AsClass() : orig->GetClass();
         klass != NULL;
         klass = is_static ? NULL : klass->GetSuperClass()) {
      size_t num_reference_fields = (is_static
                                     ? klass->NumReferenceStaticFields()
                                     : klass->NumReferenceInstanceFields());
      for (size_t i = 0; i < num_reference_fields; ++i) {
        Field* field = (is_static
                        ? klass->GetStaticField(i)
                        : klass->GetInstanceField(i));
        MemberOffset field_offset = field->GetOffset();
        const Object* ref = orig->GetFieldObject<const Object*>(field_offset, false);
        copy->SetFieldObject(field_offset, GetImageAddress(ref), false);
      }
    }
  }
}

static Method* GetReferrerMethod(const Compiler::PatchInformation* patch) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Method* method = class_linker->ResolveMethod(patch->GetDexFile(),
                                               patch->GetReferrerMethodIdx(),
                                               patch->GetDexCache(),
                                               NULL,
                                               patch->GetReferrerIsDirect());
  CHECK(method != NULL)
    << patch->GetDexFile().GetLocation() << " " << patch->GetReferrerMethodIdx();
  CHECK(!method->IsRuntimeMethod())
    << patch->GetDexFile().GetLocation() << " " << patch->GetReferrerMethodIdx();
  CHECK(patch->GetDexCache()->GetResolvedMethods()->Get(patch->GetReferrerMethodIdx()) == method)
    << patch->GetDexFile().GetLocation() << " " << patch->GetReferrerMethodIdx() << " "
    << PrettyMethod(patch->GetDexCache()->GetResolvedMethods()->Get(patch->GetReferrerMethodIdx())) << " "
    << PrettyMethod(method);
  return method;
}

static Method* GetTargetMethod(const Compiler::PatchInformation* patch) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Method* method = class_linker->ResolveMethod(patch->GetDexFile(),
                                               patch->GetTargetMethodIdx(),
                                               patch->GetDexCache(),
                                               NULL,
                                               patch->GetTargetIsDirect());
  CHECK(method != NULL)
    << patch->GetDexFile().GetLocation() << " " << patch->GetTargetMethodIdx();
  CHECK(!method->IsRuntimeMethod())
    << patch->GetDexFile().GetLocation() << " " << patch->GetTargetMethodIdx();
  CHECK(patch->GetDexCache()->GetResolvedMethods()->Get(patch->GetTargetMethodIdx()) == method)
    << patch->GetDexFile().GetLocation() << " " << patch->GetReferrerMethodIdx() << " "
    << PrettyMethod(patch->GetDexCache()->GetResolvedMethods()->Get(patch->GetTargetMethodIdx())) << " "
    << PrettyMethod(method);
  return method;
}

void ImageWriter::PatchOatCodeAndMethods(const Compiler& compiler) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  const std::vector<const Compiler::PatchInformation*>& code_to_patch = compiler.GetCodeToPatch();
  for (size_t i = 0; i < code_to_patch.size(); i++) {
    const Compiler::PatchInformation* patch = code_to_patch[i];
    Method* target = GetTargetMethod(patch);
    uint32_t code = reinterpret_cast<uint32_t>(class_linker->GetOatCodeFor(target));
    uint32_t code_base = reinterpret_cast<uint32_t>(&oat_file_->GetOatHeader());
    uint32_t code_offset = code - code_base;
    SetPatchLocation(patch, reinterpret_cast<uint32_t>(GetOatAddress(code_offset)));
  }

  const std::vector<const Compiler::PatchInformation*>& methods_to_patch
      = compiler.GetMethodsToPatch();
  for (size_t i = 0; i < methods_to_patch.size(); i++) {
    const Compiler::PatchInformation* patch = methods_to_patch[i];
    Method* target = GetTargetMethod(patch);
    SetPatchLocation(patch, reinterpret_cast<uint32_t>(GetImageAddress(target)));
  }
}

void ImageWriter::SetPatchLocation(const Compiler::PatchInformation* patch, uint32_t value) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Method* method = GetReferrerMethod(patch);
  // Goodbye const, we are about to modify some code.
  void* code = const_cast<void*>(class_linker->GetOatCodeFor(method));
  // TODO: make this Thumb2 specific
  uint8_t* base = reinterpret_cast<uint8_t*>(reinterpret_cast<uint32_t>(code) & ~0x1);
  uint32_t* patch_location = reinterpret_cast<uint32_t*>(base + patch->GetLiteralOffset());
#ifndef NDEBUG
  const DexFile::MethodId& id = patch->GetDexFile().GetMethodId(patch->GetTargetMethodIdx());
  uint32_t expected = reinterpret_cast<uint32_t>(&id);
  uint32_t actual = *patch_location;
  CHECK(actual == expected || actual == value) << std::hex
    << "actual=" << actual
    << "expected=" << expected
    << "value=" << value;
#endif
   *patch_location = value;
}

}  // namespace art
