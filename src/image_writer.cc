// Copyright 2011 Google Inc. All Rights Reserved.

#include "image_writer.h"

#include <sys/mman.h>

#include <vector>

#include "UniquePtr.h"
#include "class_linker.h"
#include "class_loader.h"
#include "dex_cache.h"
#include "file.h"
#include "globals.h"
#include "heap.h"
#include "image.h"
#include "intern_table.h"
#include "logging.h"
#include "object.h"
#include "runtime.h"
#include "space.h"
#include "utils.h"

namespace art {

bool ImageWriter::Write(const char* image_filename, uintptr_t image_base,
                        const std::string& oat_filename, const std::string& strip_location_prefix) {
  CHECK_NE(image_base, 0U);
  image_base_ = reinterpret_cast<byte*>(image_base);

  const std::vector<Space*>& spaces = Heap::GetSpaces();
  // currently just write the last space, assuming it is the space that was being used for allocation
  CHECK_GE(spaces.size(), 1U);
  source_space_ = spaces[spaces.size()-1];

  oat_file_.reset(OatFile::Open(oat_filename, strip_location_prefix, NULL));
  if (oat_file_.get() == NULL) {
    LOG(ERROR) << "Failed to open oat file " << oat_filename;
    return false;
  }

  if (!Init()) {
    return false;
  }
  Heap::CollectGarbage();
  CalculateNewObjectOffsets();
  CopyAndFixupObjects();

  UniquePtr<File> file(OS::OpenFile(image_filename, true));
  if (file.get() == NULL) {
    LOG(ERROR) << "Failed to open image file " << image_filename;
    return false;
  }
  bool success = file->WriteFully(image_->GetAddress(), image_top_);
  if (!success) {
    PLOG(ERROR) << "Failed to write image file " << image_filename;
    return false;
  }
  return true;
}

bool ImageWriter::Init() {
  size_t size = source_space_->Size();
  int prot = PROT_READ | PROT_WRITE;
  size_t length = RoundUp(size, kPageSize);
  image_.reset(MemMap::Map(length, prot));
  if (image_.get() == NULL) {
    LOG(ERROR) << "Failed to allocate memory for image file generation";
    return false;
  }
  return true;
}

void ImageWriter::CalculateNewObjectOffsetsCallback(Object* obj, void* arg) {
  DCHECK(obj != NULL);
  DCHECK(arg != NULL);
  ImageWriter* image_writer = reinterpret_cast<ImageWriter*>(arg);
  if (!image_writer->InSourceSpace(obj)) {
    return;
  }

  // if it is a string, we want to intern it if its not interned.
  if (obj->IsString()) {
    // we must be an interned string that was forward referenced and already assigned
    if (IsImageOffsetAssigned(obj)) {
      DCHECK_EQ(obj, obj->AsString()->Intern());
      return;
    }
    String* interned = obj->AsString()->Intern();
    if (obj != interned) {
      if (!IsImageOffsetAssigned(interned)) {
        // interned obj is after us, allocate its location early
        image_writer->AssignImageOffset(interned);
      }
      // point those looking for this object to the interned version.
      SetImageOffset(obj, GetImageOffset(interned));
      return;
    }
    // else (obj == interned), nothing to do but fall through to the normal case
  }

  image_writer->AssignImageOffset(obj);

  // sniff out the DexCaches on this pass for use on the next pass
  if (obj->IsClass()) {
    Class* klass = obj->AsClass();
    DexCache* dex_cache = klass->GetDexCache();
    if (dex_cache != NULL) {
      image_writer->dex_caches_.insert(dex_cache);
    } else {
      DCHECK(klass->IsArrayClass() || klass->IsPrimitive()) << PrettyClass(klass);
    }
  }
}

ObjectArray<Object>* ImageWriter::CreateImageRoots() const {
  // build a Object[] of the roots needed to restore the runtime
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  Class* object_array_class = class_linker->FindSystemClass("[Ljava/lang/Object;");
  ObjectArray<Object>* image_roots = ObjectArray<Object>::Alloc(object_array_class,
                                                                ImageHeader::kImageRootsMax);
  image_roots->Set(ImageHeader::kJniStubArray,
                   runtime->GetJniStubArray());
  image_roots->Set(ImageHeader::kAbstractMethodErrorStubArray,
                   runtime->GetAbstractMethodErrorStubArray());
  image_roots->Set(ImageHeader::kCalleeSaveMethod,
                   runtime->GetCalleeSaveMethod());
  image_roots->Set(ImageHeader::kOatLocation,
                   String::AllocFromModifiedUtf8(oat_file_->GetLocation().c_str()));
  for (int i = 0; i < ImageHeader::kImageRootsMax; i++) {
    CHECK(image_roots->Get(i) != NULL);
  }
  return image_roots;
}

void ImageWriter::CalculateNewObjectOffsets() {
  ObjectArray<Object>* image_roots = CreateImageRoots();

  HeapBitmap* heap_bitmap = Heap::GetLiveBits();
  DCHECK(heap_bitmap != NULL);
  DCHECK_EQ(0U, image_top_);

  // leave space for the header, but do not write it yet, we need to
  // know where image_roots is going to end up
  image_top_ += RoundUp(sizeof(ImageHeader), 8); // 64-bit-alignment

  heap_bitmap->Walk(CalculateNewObjectOffsetsCallback, this);  // TODO: add Space-limited Walk
  DCHECK_LT(image_top_, image_->GetLength());

  // Note that image_top_ is left at end of used space
  oat_base_ = image_base_ +  RoundUp(image_top_, kPageSize);
  byte* oat_limit = oat_base_ +  oat_file_->GetSize();

  // return to write header at start of image with future location of image_roots
  ImageHeader image_header(reinterpret_cast<uint32_t>(image_base_),
                           reinterpret_cast<uint32_t>(GetImageAddress(image_roots)),
                           oat_file_->GetOatHeader().GetChecksum(),
                           reinterpret_cast<uint32_t>(oat_base_),
                           reinterpret_cast<uint32_t>(oat_limit));
  memcpy(image_->GetAddress(), &image_header, sizeof(image_header));
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
  byte* dst = image_writer->image_->GetAddress() + offset;
  const byte* src = reinterpret_cast<const byte*>(obj);
  size_t n = obj->SizeOf();
  DCHECK_LT(offset + n, image_writer->image_->GetLength());
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

const void* FixupCode(const ByteArray* copy_code_array, const void* orig_code) {
  // TODO: change to DCHECK when all code compiling
  if (copy_code_array == NULL) {
    return NULL;
  }
  const void* copy_code = copy_code_array->GetData();
  // TODO: remember InstructionSet with each code array so we know if we need to do thumb fixup?
  if ((reinterpret_cast<uintptr_t>(orig_code) % 2) == 1) {
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(copy_code) + 1);
  }
  return copy_code;
}

void ImageWriter::FixupMethod(const Method* orig, Method* copy) {
  FixupInstanceFields(orig, copy);

  // OatWriter clears the code_array_ after writing the code.
  // It replaces the code_ with an offset value we now adjust to be a pointer.
  DCHECK(copy->code_array_ == NULL)
          << PrettyMethod(orig)
          << " orig_code_array_=" << orig->GetCodeArray() << " orig_code_=" << orig->GetCode()
          << " copy_code_array_=" << copy->code_array_ << " orig_code_=" << copy->code_
          << " jni_stub=" << Runtime::Current()->GetJniStubArray()
          << " ame_stub=" << Runtime::Current()->GetAbstractMethodErrorStubArray();
  copy->invoke_stub_ = reinterpret_cast<Method::InvokeStub*>(FixupCode(copy->invoke_stub_array_, reinterpret_cast<void*>(orig->invoke_stub_)));
  if (orig->IsNative()) {
    ByteArray* orig_jni_stub_array_ = Runtime::Current()->GetJniStubArray();
    ByteArray* copy_jni_stub_array_ = down_cast<ByteArray*>(GetImageAddress(orig_jni_stub_array_));
    copy->native_method_ = copy_jni_stub_array_->GetData();
    copy->code_ = oat_base_ + orig->GetOatCodeOffset();
  } else {
    DCHECK(copy->native_method_ == NULL) << copy->native_method_;
    if (orig->IsAbstract()) {
        ByteArray* orig_ame_stub_array_ = Runtime::Current()->GetAbstractMethodErrorStubArray();
        ByteArray* copy_ame_stub_array_ = down_cast<ByteArray*>(GetImageAddress(orig_ame_stub_array_));
        copy->code_ = copy_ame_stub_array_->GetData();
    } else {
        copy->code_ = oat_base_ + orig->GetOatCodeOffset();
    }
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

  CodeAndDirectMethods* orig_cadms = orig->GetCodeAndDirectMethods();
  CodeAndDirectMethods* copy_cadms = down_cast<CodeAndDirectMethods*>(GetLocalAddress(orig_cadms));
  for (size_t i = 0; i < orig->NumResolvedMethods(); i++) {
    Method* orig_method = orig->GetResolvedMethod(i);
    // if it was resolved in the original, resolve it in the copy
    if (orig_method != NULL
        && InSourceSpace(orig_method)
        && orig_method == orig_cadms->GetResolvedMethod(i)) {
      Method* copy_method = down_cast<Method*>(GetLocalAddress(orig_method));
      copy_cadms->Set(CodeAndDirectMethods::CodeIndex(i),
                      reinterpret_cast<int32_t>(copy_method->code_));
      copy_cadms->Set(CodeAndDirectMethods::MethodIndex(i),
                      reinterpret_cast<int32_t>(GetImageAddress(orig_method)));
    }
  }
}

}  // namespace art
