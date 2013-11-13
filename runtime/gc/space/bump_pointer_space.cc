/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include "bump_pointer_space.h"
#include "bump_pointer_space-inl.h"
#include "mirror/object-inl.h"
#include "mirror/class-inl.h"

namespace art {
namespace gc {
namespace space {

BumpPointerSpace* BumpPointerSpace::Create(const std::string& name, size_t capacity,
                                           byte* requested_begin) {
  capacity = RoundUp(capacity, kPageSize);
  std::string error_msg;
  UniquePtr<MemMap> mem_map(MemMap::MapAnonymous(name.c_str(), requested_begin, capacity,
                                                 PROT_READ | PROT_WRITE, &error_msg));
  if (mem_map.get() == nullptr) {
    LOG(ERROR) << "Failed to allocate pages for alloc space (" << name << ") of size "
        << PrettySize(capacity) << " with message " << error_msg;
    return nullptr;
  }
  return new BumpPointerSpace(name, mem_map.release());
}

BumpPointerSpace::BumpPointerSpace(const std::string& name, byte* begin, byte* limit)
    : ContinuousMemMapAllocSpace(name, nullptr, begin, begin, limit,
                                 kGcRetentionPolicyAlwaysCollect),
      num_objects_allocated_(0), total_bytes_allocated_(0), total_objects_allocated_(0),
      growth_end_(limit) {
}

BumpPointerSpace::BumpPointerSpace(const std::string& name, MemMap* mem_map)
    : ContinuousMemMapAllocSpace(name, mem_map, mem_map->Begin(), mem_map->Begin(), mem_map->End(),
                                 kGcRetentionPolicyAlwaysCollect),
      num_objects_allocated_(0), total_bytes_allocated_(0), total_objects_allocated_(0),
      growth_end_(mem_map->End()) {
}

mirror::Object* BumpPointerSpace::Alloc(Thread*, size_t num_bytes, size_t* bytes_allocated) {
  mirror::Object* ret = AllocNonvirtual(num_bytes);
  if (LIKELY(ret != nullptr)) {
    *bytes_allocated = num_bytes;
  }
  return ret;
}

size_t BumpPointerSpace::AllocationSize(const mirror::Object* obj) {
  return AllocationSizeNonvirtual(obj);
}

void BumpPointerSpace::Clear() {
  // Release the pages back to the operating system.
  CHECK_NE(madvise(Begin(), Limit() - Begin(), MADV_DONTNEED), -1) << "madvise failed";
  // Reset the end of the space back to the beginning, we move the end forward as we allocate
  // objects.
  SetEnd(Begin());
  growth_end_ = Limit();
  num_objects_allocated_ = 0;
}

void BumpPointerSpace::Dump(std::ostream& os) const {
  os << reinterpret_cast<void*>(Begin()) << "-" << reinterpret_cast<void*>(End()) << " - "
     << reinterpret_cast<void*>(Limit());
}

mirror::Object* BumpPointerSpace::GetNextObject(mirror::Object* obj) {
  const uintptr_t position = reinterpret_cast<uintptr_t>(obj) + obj->SizeOf();
  return reinterpret_cast<mirror::Object*>(RoundUp(position, kAlignment));
}

}  // namespace space
}  // namespace gc
}  // namespace art
