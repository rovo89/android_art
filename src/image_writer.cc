// Copyright 2011 Google Inc. All Rights Reserved.

#include "image_writer.h"

#include <sys/mman.h>

#include <vector>

#include "UniquePtr.h"
#include "class_linker.h"
#include "class_loader.h"
#include "compiled_method.h"
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

bool ImageWriter::Write(const char* image_filename,
                        uintptr_t image_begin,
                        const std::string& oat_filename,
                        const std::string& strip_location_prefix) {
  CHECK(image_filename != NULL);

  CHECK_NE(image_begin, 0U);
  image_begin_ = reinterpret_cast<byte*>(image_begin);

  const std::vector<Space*>& spaces = Heap::GetSpaces();
  // currently just write the last space, assuming it is the space that was being used for allocation
  CHECK_GE(spaces.size(), 1U);
  source_space_ = spaces[spaces.size()-1];
  CHECK(!source_space_->IsImageSpace());

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const std::vector<DexCache*>& all_dex_caches = class_linker->GetDexCaches();
  for (size_t i = 0; i < all_dex_caches.size(); i++) {
    DexCache* dex_cache = all_dex_caches[i];
    if (InSourceSpace(dex_cache)) {
      dex_caches_.insert(dex_cache);
    }
  }

  oat_file_.reset(OatFile::Open(oat_filename, strip_location_prefix, NULL));
  if (oat_file_.get() == NULL) {
    LOG(ERROR) << "Failed to open oat file " << oat_filename;
    return false;
  }

  if (!AllocMemory()) {
    return false;
  }
  PruneNonImageClasses();
  Heap::CollectGarbage(false);
#ifndef NDEBUG
  CheckNonImageClassesRemoved();
#endif
  Heap::DisableCardMarking();
  CalculateNewObjectOffsets();
  CopyAndFixupObjects();

  UniquePtr<File> file(OS::OpenFile(image_filename, true));
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
        dex_cache->SetResolvedMethod(i, NULL);
        Runtime::TrampolineType type = Runtime::GetTrampolineType(method);
        ByteArray* res_trampoline = runtime->GetResolutionStubArray(type);
        dex_cache->GetCodeAndDirectMethods()->SetResolvedDirectMethodTrampoline(i, res_trampoline);
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
  Heap::GetLiveBits()->Walk(CheckNonImageClassesRemovedCallback, this);
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
    if (IsImageOffsetAssigned(obj)) {
      DCHECK_EQ(obj, obj->AsString()->Intern());
      return;
    }
    SirtRef<String> interned(obj->AsString()->Intern());
    if (obj != interned.get()) {
      if (!IsImageOffsetAssigned(interned.get())) {
        // interned obj is after us, allocate its location early
        image_writer->AssignImageOffset(interned.get());
      }
      // point those looking for this object to the interned version.
      SetImageOffset(obj, GetImageOffset(interned.get()));
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
  image_roots->Set(ImageHeader::kInstanceResolutionStubArray,
                   runtime->GetResolutionStubArray(Runtime::kInstanceMethod));
  image_roots->Set(ImageHeader::kStaticResolutionStubArray,
                   runtime->GetResolutionStubArray(Runtime::kStaticMethod));
  image_roots->Set(ImageHeader::kUnknownMethodResolutionStubArray,
                   runtime->GetResolutionStubArray(Runtime::kUnknownMethod));
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

  HeapBitmap* heap_bitmap = Heap::GetLiveBits();
  DCHECK(heap_bitmap != NULL);
  DCHECK_EQ(0U, image_end_);

  // leave space for the header, but do not write it yet, we need to
  // know where image_roots is going to end up
  image_end_ += RoundUp(sizeof(ImageHeader), 8); // 64-bit-alignment

  heap_bitmap->Walk(CalculateNewObjectOffsetsCallback, this);  // TODO: add Space-limited Walk
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
  HeapBitmap* heap_bitmap = Heap::GetLiveBits();
  DCHECK(heap_bitmap != NULL);
  // TODO: heap validation can't handle this fix up pass
  Heap::DisableObjectValidation();
  heap_bitmap->Walk(CopyAndFixupObjectsCallback, this);  // TODO: add Space-limited Walk
  FixupDexCaches();
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
  ResetImageOffset(copy);
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

static uint32_t FixupCode(const ByteArray* copy_code_array, uint32_t orig_code) {
  // TODO: change to DCHECK when all code compiling
  if (copy_code_array == NULL) {
    return 0;
  }
  uint32_t copy_code = reinterpret_cast<uint32_t>(copy_code_array->GetData());
  // TODO: remember InstructionSet with each code array so we know if we need to do thumb fixup?
  if ((orig_code % 2) == 1) {
    return copy_code + 1;
  }
  return copy_code;
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

  // Non-abstract methods typically have code
  uint32_t code_offset = orig->GetOatCodeOffset();
  const byte* code = GetOatAddress(code_offset);
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

void ImageWriter::FixupDexCaches() {
  typedef Set::const_iterator It;  // TODO: C++0x auto
  for (It it = dex_caches_.begin(), end = dex_caches_.end(); it != end; ++it) {
    DexCache* orig = *it;
    DexCache* copy = down_cast<DexCache*>(GetLocalAddress(orig));
    FixupDexCache(orig, copy);
  }
}

void ImageWriter::FixupDexCache(const DexCache* orig, DexCache* copy) {
  CHECK(orig != NULL);
  CHECK(copy != NULL);

  // The original array value
  CodeAndDirectMethods* orig_cadms = orig->GetCodeAndDirectMethods();
  // The compacted object in local memory but not at the correct image address
  CodeAndDirectMethods* copy_cadms = down_cast<CodeAndDirectMethods*>(GetLocalAddress(orig_cadms));

  Runtime* runtime = Runtime::Current();
  for (size_t i = 0; i < orig->NumResolvedMethods(); i++) {
    Method* orig_method = orig->GetResolvedMethod(i);
    if (orig_method != NULL && !InSourceSpace(orig_method)) {
      continue;
    }
    // if it was unresolved or a resolved static method in an uninit class, use a resolution stub
    // we need to use the stub in the static method case to ensure <clinit> is run.
    if (orig_method == NULL
        || (orig_method->IsStatic() && !orig_method->GetDeclaringClass()->IsInitialized())) {
      uint32_t orig_res_stub_code = orig_cadms->Get(CodeAndDirectMethods::CodeIndex(i));
      if (orig_res_stub_code == 0) {
        continue;  // NULL maps the same in the image and the original
      }
      Runtime::TrampolineType type = Runtime::GetTrampolineType(orig_method);  // Type of trampoline
      ByteArray* orig_res_stub_array = runtime->GetResolutionStubArray(type);
      // Do we need to relocate this for this space?
      if (!InSourceSpace(orig_res_stub_array)) {
        continue;
      }
      // Compute address in image of resolution stub and the code address
      ByteArray* image_res_stub_array = down_cast<ByteArray*>(GetImageAddress(orig_res_stub_array));
      uint32_t image_res_stub_code = FixupCode(image_res_stub_array, orig_res_stub_code);
      // Put the image code address in the array
      copy_cadms->Set(CodeAndDirectMethods::CodeIndex(i), image_res_stub_code);
    } else if (orig_method->IsDirect()) {
      // if it was resolved in the original, resolve it in the copy
      Method* copy_method = down_cast<Method*>(GetLocalAddress(orig_method));
      copy_cadms->Set(CodeAndDirectMethods::CodeIndex(i),
                      reinterpret_cast<int32_t>(copy_method->code_));
      copy_cadms->Set(CodeAndDirectMethods::MethodIndex(i),
                      reinterpret_cast<int32_t>(GetImageAddress(orig_method)));
    }
  }
}

}  // namespace art
