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

#ifndef ART_RUNTIME_MIRROR_STRING_INL_H_
#define ART_RUNTIME_MIRROR_STRING_INL_H_

#include "array.h"
#include "class.h"
#include "gc/heap-inl.h"
#include "intern_table.h"
#include "runtime.h"
#include "string.h"
#include "thread.h"
#include "utf.h"

namespace art {
namespace mirror {

inline uint32_t String::ClassSize() {
  uint32_t vtable_entries = Object::kVTableLength + 52;
  return Class::ComputeClassSize(true, vtable_entries, 0, 1, 0, 1, 2);
}

// Sets string count in the allocation code path to ensure it is guarded by a CAS.
class SetStringCountVisitor {
 public:
  explicit SetStringCountVisitor(int32_t count) : count_(count) {
  }

  void operator()(Object* obj, size_t usable_size ATTRIBUTE_UNUSED) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // Avoid AsString as object is not yet in live bitmap or allocation stack.
    String* string = down_cast<String*>(obj);
    string->SetCount(count_);
  }

 private:
  const int32_t count_;
};

// Sets string count and value in the allocation code path to ensure it is guarded by a CAS.
class SetStringCountAndBytesVisitor {
 public:
  SetStringCountAndBytesVisitor(int32_t count, uint8_t* src, int32_t high_byte)
      : count_(count), src_(src), high_byte_(high_byte) {
  }

  void operator()(Object* obj, size_t usable_size ATTRIBUTE_UNUSED) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // Avoid AsString as object is not yet in live bitmap or allocation stack.
    String* string = down_cast<String*>(obj);
    string->SetCount(count_);
    uint16_t* value = string->GetValue();
    for (int i = 0; i < count_; i++) {
      value[i] = high_byte_ + (src_[i] & 0xFF);
    }
  }

 private:
  const int32_t count_;
  const uint8_t* const src_;
  const int32_t high_byte_;
};

// Sets string count and value in the allocation code path to ensure it is guarded by a CAS.
class SetStringCountAndValueVisitor {
 public:
  SetStringCountAndValueVisitor(int32_t count, uint16_t* src) : count_(count), src_(src) {
  }

  void operator()(Object* obj, size_t usable_size) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    UNUSED(usable_size);
    // Avoid AsString as object is not yet in live bitmap or allocation stack.
    String* string = down_cast<String*>(obj);
    string->SetCount(count_);
    memcpy(string->GetValue(), src_, count_ * sizeof(uint16_t));
  }

 private:
  const int32_t count_;
  const uint16_t* const src_;
};

inline String* String::Intern() {
  return Runtime::Current()->GetInternTable()->InternWeak(this);
}

inline uint16_t String::CharAt(int32_t index) {
  int32_t count = GetField32(OFFSET_OF_OBJECT_MEMBER(String, count_));
  if (UNLIKELY((index < 0) || (index >= count))) {
    Thread* self = Thread::Current();
    self->ThrowNewExceptionF("Ljava/lang/StringIndexOutOfBoundsException;",
                             "length=%i; index=%i", count, index);
    return 0;
  }
  return GetValue()[index];
}

template<VerifyObjectFlags kVerifyFlags>
inline size_t String::SizeOf() {
  return sizeof(String) + (sizeof(uint16_t) * GetLength<kVerifyFlags>());
}

template <bool kIsInstrumented, typename PreFenceVisitor>
inline String* String::Alloc(Thread* self, int32_t utf16_length, gc::AllocatorType allocator_type,
                             const PreFenceVisitor& pre_fence_visitor) {
  size_t header_size = sizeof(String);
  size_t data_size = sizeof(uint16_t) * utf16_length;
  size_t size = header_size + data_size;
  Class* string_class = GetJavaLangString();

  // Check for overflow and throw OutOfMemoryError if this was an unreasonable request.
  if (UNLIKELY(size < data_size)) {
    self->ThrowOutOfMemoryError(StringPrintf("%s of length %d would overflow",
                                             PrettyDescriptor(string_class).c_str(),
                                             utf16_length).c_str());
    return nullptr;
  }
  gc::Heap* heap = Runtime::Current()->GetHeap();
  return down_cast<String*>(
      heap->AllocObjectWithAllocator<kIsInstrumented, false>(self, string_class, size,
                                                             allocator_type, pre_fence_visitor));
}

template <bool kIsInstrumented>
inline String* String::AllocFromByteArray(Thread* self, int32_t byte_length,
                                          Handle<ByteArray> array, int32_t offset,
                                          int32_t high_byte, gc::AllocatorType allocator_type) {
  uint8_t* data = reinterpret_cast<uint8_t*>(array->GetData()) + offset;
  SetStringCountAndBytesVisitor visitor(byte_length, data, high_byte << 8);
  String* string = Alloc<kIsInstrumented>(self, byte_length, allocator_type, visitor);
  return string;
}

template <bool kIsInstrumented>
inline String* String::AllocFromCharArray(Thread* self, int32_t array_length,
                                          Handle<CharArray> array, int32_t offset,
                                          gc::AllocatorType allocator_type) {
  uint16_t* data = array->GetData() + offset;
  SetStringCountAndValueVisitor visitor(array_length, data);
  String* new_string = Alloc<kIsInstrumented>(self, array_length, allocator_type, visitor);
  return new_string;
}

template <bool kIsInstrumented>
inline String* String::AllocFromString(Thread* self, int32_t string_length, Handle<String> string,
                                       int32_t offset, gc::AllocatorType allocator_type) {
  uint16_t* data = string->GetValue() + offset;
  SetStringCountAndValueVisitor visitor(string_length, data);
  String* new_string = Alloc<kIsInstrumented>(self, string_length, allocator_type, visitor);
  return new_string;
}

inline int32_t String::GetHashCode() {
  int32_t result = GetField32(OFFSET_OF_OBJECT_MEMBER(String, hash_code_));
  if (UNLIKELY(result == 0)) {
    result = ComputeHashCode();
  }
  DCHECK(result != 0 || ComputeUtf16Hash(GetValue(), GetLength()) == 0)
      << ToModifiedUtf8() << " " << result;
  return result;
}

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_STRING_INL_H_
