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

bool ImageWriter::Write(Space* space, const char* filename, byte* image_base) {
  image_base_ = image_base;
  if (!Init(space)) {
    return false;
  }
  CalculateNewObjectOffsets();
  CopyAndFixupObjects();

  UniquePtr<File> file(OS::OpenFile(filename, true));
  if (file.get() == NULL) {
    return false;
  }
  return file->WriteFully(image_->GetAddress(), image_top_);
}

bool ImageWriter::Init(Space* space) {
  size_t size = space->Size();
  int prot = PROT_READ | PROT_WRITE;
  size_t length = RoundUp(size, kPageSize);
  image_.reset(MemMap::Map(length, prot));
  if (image_.get() == NULL) {
    return false;
  }
  return true;
}

namespace {

struct InternTableVisitorState {
  int index;
  ObjectArray<const Object>* interned_array;
};

void InternTableVisitor(const Object* obj, void* arg) {
  InternTableVisitorState* state = reinterpret_cast<InternTableVisitorState*>(arg);
  state->interned_array->Set(state->index++, obj);
}

ObjectArray<const Object>* CreateInternedArray() {
  // build a Object[] of the interned strings for reinit
  // TODO: avoid creating this future garbage
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  const InternTable& intern_table = *runtime->GetInternTable();
  size_t size = intern_table.Size();
  CHECK_NE(0U, size);

  Class* object_array_class = class_linker->FindSystemClass("[Ljava/lang/Object;");
  ObjectArray<const Object>* interned_array = ObjectArray<const Object>::Alloc(object_array_class, size);

  InternTableVisitorState state;
  state.index = 0;
  state.interned_array = interned_array;

  intern_table.VisitRoots(InternTableVisitor, &state);

  return interned_array;
}

} // namespace

void ImageWriter::CalculateNewObjectOffsetsCallback(Object* obj, void *arg) {
  DCHECK(obj != NULL);
  DCHECK(arg != NULL);
  ImageWriter* image_writer = reinterpret_cast<ImageWriter*>(arg);
  image_writer->SetImageOffset(obj, image_writer->image_top_);
  image_writer->image_top_ += RoundUp(obj->SizeOf(), 8);  // 64-bit alignment
  DCHECK_LT(image_writer->image_top_, image_writer->image_->GetLength());
}

void ImageWriter::CalculateNewObjectOffsets() {
  ObjectArray<const Object>* interned_array = CreateInternedArray();

  HeapBitmap* heap_bitmap = Heap::GetLiveBits();
  DCHECK(heap_bitmap != NULL);
  DCHECK_EQ(0U, image_top_);

  // leave space for the header, but do not write it yet, we need to
  // know where interned_array is going to end up
  image_top_ += RoundUp(sizeof(ImageHeader), 8); // 64-bit-alignment

  heap_bitmap->Walk(CalculateNewObjectOffsetsCallback, this);
  DCHECK_LT(image_top_, image_->GetLength());

  // return to write header at start of image with future location of interned_array
  ImageHeader image_header(reinterpret_cast<uint32_t>(image_base_),
                           reinterpret_cast<uint32_t>(GetImageAddress(interned_array)));
  memcpy(image_->GetAddress(), &image_header, sizeof(image_header));

  // Note that top_ is left at end of used space
}

void ImageWriter::CopyAndFixupObjects() {
  HeapBitmap* heap_bitmap = Heap::GetLiveBits();
  DCHECK(heap_bitmap != NULL);
  // TODO: heap validation can't handle this fix up pass
  Heap::DisableObjectValidation();
  heap_bitmap->Walk(CopyAndFixupObjectsCallback, this);
}

void ImageWriter::CopyAndFixupObjectsCallback(Object* object, void *arg) {
  DCHECK(object != NULL);
  DCHECK(arg != NULL);
  const Object* obj = object;
  ImageWriter* image_writer = reinterpret_cast<ImageWriter*>(arg);

  size_t offset = image_writer->GetImageOffset(obj);
  byte* dst = image_writer->image_->GetAddress() + offset;
  const byte* src = reinterpret_cast<const byte*>(obj);
  size_t n = obj->SizeOf();
  DCHECK_LT(offset + n, image_writer->image_->GetLength());
  memcpy(dst, src, n);
  Object* copy = reinterpret_cast<Object*>(dst);
  image_writer->FixupObject(obj, copy);
}

void ImageWriter::FixupObject(const Object* orig, Object* copy) {
  DCHECK(orig != NULL);
  DCHECK(copy != NULL);
  copy->SetClass(down_cast<Class*>(GetImageAddress(orig->GetClass())));
  // TODO: special case init of pointers to malloc data (or removal of these pointers)
  if (orig->IsClass()) {
    FixupClass(orig->AsClass(), down_cast<Class*>(copy));
  } else if (orig->IsMethod()) {
    FixupMethod(orig->AsMethod(), down_cast<Method*>(copy));
  } else if (orig->IsField()) {
    FixupField(orig->AsField(), down_cast<Field*>(copy));
  } else if (orig->IsObjectArray()) {
    FixupObjectArray(orig->AsObjectArray<Object>(), down_cast<ObjectArray<Object>*>(copy));
  } else {
    FixupInstanceFields(orig, copy);
  }
}

void ImageWriter::FixupClass(const Class* orig, Class* copy) {
  FixupInstanceFields(orig, copy);
  copy->descriptor_ = down_cast<String*>(GetImageAddress(orig->descriptor_));
  copy->dex_cache_ = down_cast<DexCache*>(GetImageAddress(orig->dex_cache_));
  copy->verify_error_class_ = down_cast<Class*>(GetImageAddress(orig->verify_error_class_));
  copy->component_type_ = down_cast<Class*>(GetImageAddress(orig->component_type_));
  copy->super_class_ = down_cast<Class*>(GetImageAddress(orig->super_class_));
  copy->class_loader_ = down_cast<ClassLoader*>(GetImageAddress(orig->class_loader_));
  copy->interfaces_ = down_cast<ObjectArray<Class>*>(GetImageAddress(orig->interfaces_));
  copy->direct_methods_ = down_cast<ObjectArray<Method>*>(GetImageAddress(orig->direct_methods_));
  copy->virtual_methods_ = down_cast<ObjectArray<Method>*>(GetImageAddress(orig->virtual_methods_));
  copy->vtable_ = down_cast<ObjectArray<Method>*>(GetImageAddress(orig->vtable_));
  // TODO: convert iftable_ to heap allocated storage
  // TODO: convert ifvi_pool_ to heap allocated storage
  copy->ifields_ = down_cast<ObjectArray<Field>*>(GetImageAddress(orig->ifields_));
  // TODO: convert source_file_ to heap allocated storage
  copy->sfields_ = down_cast<ObjectArray<Field>*>(GetImageAddress(orig->sfields_));
  copy->interfaces_type_idx_ = down_cast<IntArray*>(GetImageAddress(orig->interfaces_type_idx_));
  FixupStaticFields(orig, copy);
}

// TODO: remove this slow path
void ImageWriter::FixupMethod(const Method* orig, Method* copy) {
  FixupInstanceFields(orig, copy);
  // TODO: remove need for this by adding "signature" to java.lang.reflect.Method
  copy->signature_ = down_cast<String*>(GetImageAddress(orig->signature_));
  DCHECK(copy->signature_ != NULL);
  copy->dex_cache_strings_ = down_cast<ObjectArray<String>*>(GetImageAddress(orig->dex_cache_strings_));
  copy->dex_cache_resolved_types_ = down_cast<ObjectArray<Class>*>(GetImageAddress(orig->dex_cache_resolved_types_));
  copy->dex_cache_resolved_methods_ = down_cast<ObjectArray<Method>*>(GetImageAddress(orig->dex_cache_resolved_methods_));
  copy->dex_cache_resolved_fields_ = down_cast<ObjectArray<Field>*>(GetImageAddress(orig->dex_cache_resolved_fields_));
  copy->dex_cache_code_and_direct_methods_ = down_cast<CodeAndDirectMethods*>(GetImageAddress(orig->dex_cache_code_and_direct_methods_));
  copy->dex_cache_initialized_static_storage_ = down_cast<ObjectArray<StaticStorageBase>*>(GetImageAddress(orig->dex_cache_initialized_static_storage_));

  // TODO: convert shorty_ to heap allocated storage
}

void ImageWriter::FixupField(const Field* orig, Field* copy) {
  FixupInstanceFields(orig, copy);
  // TODO: convert descriptor_ to heap allocated storage
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

}  // namespace art
