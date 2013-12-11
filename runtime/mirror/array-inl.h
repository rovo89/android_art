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

inline size_t Array::SizeOf() const {
  // This is safe from overflow because the array was already allocated, so we know it's sane.
  size_t component_size = GetClass()->GetComponentSize();
  int32_t component_count = GetLength();
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

  void operator()(mirror::Object* obj) const {
    mirror::Array* array = obj->AsArray();
    DCHECK(array->IsArrayInstance());
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
  return down_cast<Array*>(
      heap->AllocObjectWithAllocator<kIsInstrumented>(self, array_class, size, allocator_type,
                                                      visitor));
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
inline void PrimitiveArray<T>::VisitRoots(RootVisitor* visitor, void* arg) {
  if (array_class_ != nullptr) {
    array_class_ = down_cast<Class*>(visitor(array_class_, arg));
  }
}

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_ARRAY_INL_H_
