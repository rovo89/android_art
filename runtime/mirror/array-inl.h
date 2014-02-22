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

#ifndef ART_RUNTIME_MIRROR_ARRAY_INL_H_
#define ART_RUNTIME_MIRROR_ARRAY_INL_H_

#include "array.h"

#include "class.h"
#include "gc/heap-inl.h"
#include "thread.h"
#include "utils.h"

namespace art {
namespace mirror {

template<VerifyObjectFlags kVerifyFlags>
inline size_t Array::SizeOf() {
  // This is safe from overflow because the array was already allocated, so we know it's sane.
  size_t component_size = GetClass<kVerifyFlags>()->GetComponentSize();
  // Don't need to check this since we already check this in GetClass.
  int32_t component_count =
      GetLength<static_cast<VerifyObjectFlags>(kVerifyFlags & ~kVerifyThis)>();
  size_t header_size = sizeof(Object) + (component_size == sizeof(int64_t) ? 8 : 4);
  size_t data_size = component_count * component_size;
  return header_size + data_size;
}

static inline size_t ComputeArraySize(Thread* self, Class* array_class, int32_t component_count,
                                      size_t component_size)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(array_class != NULL);
  DCHECK_GE(component_count, 0);
  DCHECK(array_class->IsArrayClass());

  size_t header_size = sizeof(Object) + (component_size == sizeof(int64_t) ? 8 : 4);
  size_t data_size = component_count * component_size;
  size_t size = header_size + data_size;

  // Check for overflow and throw OutOfMemoryError if this was an unreasonable request.
  size_t component_shift = sizeof(size_t) * 8 - 1 - CLZ(component_size);
  if (UNLIKELY(data_size >> component_shift != size_t(component_count) || size < data_size)) {
    self->ThrowOutOfMemoryError(StringPrintf("%s of length %d would overflow",
                                             PrettyDescriptor(array_class).c_str(),
                                             component_count).c_str());
    return 0;  // failure
  }
  return size;
}

// Used for setting the array length in the allocation code path to ensure it is guarded by a CAS.
class SetLengthVisitor {
 public:
  explicit SetLengthVisitor(int32_t length) : length_(length) {
  }

  void operator()(Object* obj) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // Avoid AsArray as object is not yet in live bitmap or allocation stack.
    Array* array = down_cast<Array*>(obj);
    // DCHECK(array->IsArrayInstance());
    array->SetLength(length_);
  }

 private:
  const int32_t length_;
};

template <bool kIsInstrumented>
inline Array* Array::Alloc(Thread* self, Class* array_class, int32_t component_count,
                           size_t component_size, gc::AllocatorType allocator_type) {
  size_t size = ComputeArraySize(self, array_class, component_count, component_size);
  if (UNLIKELY(size == 0)) {
    return nullptr;
  }
  gc::Heap* heap = Runtime::Current()->GetHeap();
  SetLengthVisitor visitor(component_count);
  DCHECK(allocator_type != gc::kAllocatorTypeLOS);
  return down_cast<Array*>(
      heap->AllocObjectWithAllocator<kIsInstrumented, true>(self, array_class, size,
                                                            allocator_type, visitor));
}

template <bool kIsInstrumented>
inline Array* Array::Alloc(Thread* self, Class* array_class, int32_t component_count,
                           gc::AllocatorType allocator_type) {
  DCHECK(array_class->IsArrayClass());
  return Alloc<kIsInstrumented>(self, array_class, component_count, array_class->GetComponentSize(),
                                allocator_type);
}
template <bool kIsInstrumented>
inline Array* Array::Alloc(Thread* self, Class* array_class, int32_t component_count) {
  return Alloc<kIsInstrumented>(self, array_class, component_count,
               Runtime::Current()->GetHeap()->GetCurrentAllocator());
}

template <bool kIsInstrumented>
inline Array* Array::Alloc(Thread* self, Class* array_class, int32_t component_count,
                           size_t component_size) {
  return Alloc<kIsInstrumented>(self, array_class, component_count, component_size,
               Runtime::Current()->GetHeap()->GetCurrentAllocator());
}

template<class T>
inline void PrimitiveArray<T>::VisitRoots(RootCallback* callback, void* arg) {
  if (array_class_ != nullptr) {
    callback(reinterpret_cast<mirror::Object**>(&array_class_), arg, 0, kRootStickyClass);
  }
}

// Similar to memmove except elements are of aligned appropriately for T, count is in T sized units
// copies are guaranteed not to tear when T is less-than 64bit.
template<typename T>
static inline void ArrayBackwardCopy(T* d, const T* s, int32_t count) {
  d += count;
  s += count;
  for (int32_t i = 0; i < count; ++i) {
    d--;
    s--;
    *d = *s;
  }
}

template<class T>
void PrimitiveArray<T>::Memmove(int32_t dst_pos, PrimitiveArray<T>* src, int32_t src_pos,
                                int32_t count) {
  if (UNLIKELY(count == 0)) {
    return;
  }
  DCHECK_GE(dst_pos, 0);
  DCHECK_GE(src_pos, 0);
  DCHECK_GT(count, 0);
  DCHECK(src != nullptr);
  DCHECK_LT(dst_pos, GetLength());
  DCHECK_LE(dst_pos, GetLength() - count);
  DCHECK_LT(src_pos, src->GetLength());
  DCHECK_LE(src_pos, src->GetLength() - count);

  // Note for non-byte copies we can't rely on standard libc functions like memcpy(3) and memmove(3)
  // in our implementation, because they may copy byte-by-byte.
  if (LIKELY(src != this) || (dst_pos < src_pos) || (dst_pos - src_pos >= count)) {
    // Forward copy ok.
    Memcpy(dst_pos, src, src_pos, count);
  } else {
    // Backward copy necessary.
    void* dst_raw = GetRawData(sizeof(T), dst_pos);
    const void* src_raw = src->GetRawData(sizeof(T), src_pos);
    if (sizeof(T) == sizeof(uint8_t)) {
      // TUNING: use memmove here?
      uint8_t* d = reinterpret_cast<uint8_t*>(dst_raw);
      const uint8_t* s = reinterpret_cast<const uint8_t*>(src_raw);
      ArrayBackwardCopy<uint8_t>(d, s, count);
    } else if (sizeof(T) == sizeof(uint16_t)) {
      uint16_t* d = reinterpret_cast<uint16_t*>(dst_raw);
      const uint16_t* s = reinterpret_cast<const uint16_t*>(src_raw);
      ArrayBackwardCopy<uint16_t>(d, s, count);
    } else if (sizeof(T) == sizeof(uint32_t)) {
      uint32_t* d = reinterpret_cast<uint32_t*>(dst_raw);
      const uint32_t* s = reinterpret_cast<const uint32_t*>(src_raw);
      ArrayBackwardCopy<uint32_t>(d, s, count);
    } else {
      DCHECK_EQ(sizeof(T), sizeof(uint64_t));
      uint64_t* d = reinterpret_cast<uint64_t*>(dst_raw);
      const uint64_t* s = reinterpret_cast<const uint64_t*>(src_raw);
      ArrayBackwardCopy<uint64_t>(d, s, count);
    }
  }
}

// Similar to memcpy except elements are of aligned appropriately for T, count is in T sized units
// copies are guaranteed not to tear when T is less-than 64bit.
template<typename T>
static inline void ArrayForwardCopy(T* d, const T* s, int32_t count) {
  for (int32_t i = 0; i < count; ++i) {
    *d = *s;
    d++;
    s++;
  }
}


template<class T>
void PrimitiveArray<T>::Memcpy(int32_t dst_pos, PrimitiveArray<T>* src, int32_t src_pos,
                               int32_t count) {
  if (UNLIKELY(count == 0)) {
    return;
  }
  DCHECK_GE(dst_pos, 0);
  DCHECK_GE(src_pos, 0);
  DCHECK_GT(count, 0);
  DCHECK(src != nullptr);
  DCHECK_LT(dst_pos, GetLength());
  DCHECK_LE(dst_pos, GetLength() - count);
  DCHECK_LT(src_pos, src->GetLength());
  DCHECK_LE(src_pos, src->GetLength() - count);

  // Note for non-byte copies we can't rely on standard libc functions like memcpy(3) and memmove(3)
  // in our implementation, because they may copy byte-by-byte.
  void* dst_raw = GetRawData(sizeof(T), dst_pos);
  const void* src_raw = src->GetRawData(sizeof(T), src_pos);
  if (sizeof(T) == sizeof(uint8_t)) {
    memcpy(dst_raw, src_raw, count);
  } else if (sizeof(T) == sizeof(uint16_t)) {
    uint16_t* d = reinterpret_cast<uint16_t*>(dst_raw);
    const uint16_t* s = reinterpret_cast<const uint16_t*>(src_raw);
    ArrayForwardCopy<uint16_t>(d, s, count);
  } else if (sizeof(T) == sizeof(uint32_t)) {
    uint32_t* d = reinterpret_cast<uint32_t*>(dst_raw);
    const uint32_t* s = reinterpret_cast<const uint32_t*>(src_raw);
    ArrayForwardCopy<uint32_t>(d, s, count);
  } else {
    DCHECK_EQ(sizeof(T), sizeof(uint64_t));
    uint64_t* d = reinterpret_cast<uint64_t*>(dst_raw);
    const uint64_t* s = reinterpret_cast<const uint64_t*>(src_raw);
    ArrayForwardCopy<uint64_t>(d, s, count);
  }
}

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_ARRAY_INL_H_
