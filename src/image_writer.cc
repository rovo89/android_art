// Copyright 2011 Google Inc. All Rights Reserved.

#include "image_writer.h"

#include <sys/mman.h>
#include <vector>

#include "file.h"
#include "globals.h"
#include "heap.h"
#include "logging.h"
#include "object.h"
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

  scoped_ptr<File> file(OS::OpenBinaryFile(filename, true));
  if (file == NULL) {
    return false;
  }
  return file->WriteFully(mem_map_->GetAddress(), top_);
}

bool ImageWriter::Init(Space* space) {
  size_t size = space->Size();
  int prot = PROT_READ | PROT_WRITE;
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  size_t length = RoundUp(size, kPageSize);
  mem_map_.reset(MemMap::Map(length, prot, flags));
  if (mem_map_ == NULL) {
    PLOG(ERROR) << "mmap failed";
    return false;
  }
  return true;
}

void ImageWriter::CalculateNewObjectOffsets() {
  HeapBitmap* heap_bitmap = Heap::GetLiveBits();
  DCHECK(heap_bitmap != NULL);
  DCHECK_EQ(0U, top_);
  top_ += sizeof(uint64_t);  // leave a header, ensures objects have non-zero offset for DCHECKs
  heap_bitmap->Walk(CalculateNewObjectOffsetsCallback, this);
  DCHECK_LT(top_, mem_map_->GetLength());
  // Note that top_ is left at end of used space
}

void ImageWriter::CalculateNewObjectOffsetsCallback(Object *obj, void *arg) {
  DCHECK(obj != NULL);
  DCHECK(arg != NULL);
  ImageWriter* image_writer = reinterpret_cast<ImageWriter*>(arg);
  image_writer->SetImageOffset(obj, image_writer->top_);
  image_writer->top_ += RoundUp(obj->Size(), 8);  // 64-bit alignment
  DCHECK_LT(image_writer->top_, image_writer->mem_map_->GetLength());
}

void ImageWriter::CopyAndFixupObjects() {
  HeapBitmap* heap_bitmap = Heap::GetLiveBits();
  DCHECK(heap_bitmap != NULL);
  heap_bitmap->Walk(CopyAndFixupObjectsCallback, this);
}

void ImageWriter::CopyAndFixupObjectsCallback(Object *obj, void *arg) {
  DCHECK(obj != NULL);
  DCHECK(arg != NULL);
  ImageWriter* image_writer = reinterpret_cast<ImageWriter*>(arg);

  size_t offset = image_writer->GetImageOffset(obj);
  byte* dst = image_writer->mem_map_->GetAddress() + offset;
  byte* src = reinterpret_cast<byte*>(obj);
  size_t n = obj->Size();
  DCHECK_LT(offset + n, image_writer->mem_map_->GetLength());
  memcpy(dst, src, n);
  Object* copy = reinterpret_cast<Object*>(dst);
  image_writer->FixupObject(obj, copy);
}

void ImageWriter::FixupObject(Object* orig, Object* copy) {
  DCHECK(orig != NULL);
  DCHECK(copy != NULL);
  copy->klass_ = down_cast<Class*>(GetImageAddress(orig->klass_));
  // TODO specical case init of pointers to malloc data (or removal of these pointers)
  if (orig->IsObjectArray()) {
    FixupObjectArray(orig->AsObjectArray<Object>(), down_cast<ObjectArray<Object>*>(copy));
  } else {
    FixupInstanceFields(orig, copy);
  }
}

void ImageWriter::FixupObjectArray(ObjectArray<Object>* orig, ObjectArray<Object>* copy) {
  for (int32_t i = 0; i < orig->GetLength(); ++i) {
    const Object* element = orig->Get(i);
    copy->Set(i, GetImageAddress(element));
  }
}

void ImageWriter::FixupInstanceFields(Object* orig, Object* copy) {
  uint32_t ref_offsets = orig->GetClass()->GetReferenceOffsets();
  if (ref_offsets != CLASS_WALK_SUPER) {
    // Found a reference offset bitmap.  Fixup the specified offsets.
    while (ref_offsets != 0) {
      size_t right_shift = CLZ(ref_offsets);
      size_t byte_offset = CLASS_OFFSET_FROM_CLZ(right_shift);
      const Object* ref = orig->GetFieldObject(byte_offset);
      copy->SetFieldObject(byte_offset, GetImageAddress(ref));
      ref_offsets &= ~(CLASS_HIGH_BIT >> right_shift);
    }
  } else {
    // There is no reference offset bitmap for this class.  Walk up
    // the class inheritance hierarchy and find reference offsets the
    // hard way.
    for (Class *klass = orig->GetClass();
         klass != NULL;
         klass = klass->GetSuperClass()) {
      for (size_t i = 0; i < klass->NumReferenceInstanceFields(); ++i) {
        size_t field_offset = klass->GetInstanceField(i)->GetOffset();
        const Object* ref = orig->GetFieldObject(field_offset);
        copy->SetFieldObject(field_offset, GetImageAddress(ref));
      }
    }
  }
}

}  // namespace art
