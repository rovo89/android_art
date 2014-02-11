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
#include "thread_list.h"

namespace art {
namespace gc {
namespace space {

BumpPointerSpace* BumpPointerSpace::Create(const std::string& name, size_t capacity,
                                           byte* requested_begin) {
  capacity = RoundUp(capacity, kPageSize);
  std::string error_msg;
  UniquePtr<MemMap> mem_map(MemMap::MapAnonymous(name.c_str(), requested_begin, capacity,
                                                 PROT_READ | PROT_WRITE, true, &error_msg));
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
      growth_end_(limit),
      objects_allocated_(0), bytes_allocated_(0),
      block_lock_("Block lock"),
      num_blocks_(0) {
  CHECK_GE(Capacity(), sizeof(BlockHeader));
  end_ += sizeof(BlockHeader);
}

BumpPointerSpace::BumpPointerSpace(const std::string& name, MemMap* mem_map)
    : ContinuousMemMapAllocSpace(name, mem_map, mem_map->Begin(), mem_map->Begin(), mem_map->End(),
                                 kGcRetentionPolicyAlwaysCollect),
      growth_end_(mem_map->End()),
      objects_allocated_(0), bytes_allocated_(0),
      block_lock_("Block lock"),
      num_blocks_(0) {
  CHECK_GE(Capacity(), sizeof(BlockHeader));
  end_ += sizeof(BlockHeader);
}

mirror::Object* BumpPointerSpace::Alloc(Thread*, size_t num_bytes, size_t* bytes_allocated) {
  num_bytes = RoundUp(num_bytes, kAlignment);
  mirror::Object* ret = AllocNonvirtual(num_bytes);
  if (LIKELY(ret != nullptr)) {
    *bytes_allocated = num_bytes;
  }
  return ret;
}

size_t BumpPointerSpace::AllocationSize(mirror::Object* obj) {
  return AllocationSizeNonvirtual(obj);
}

void BumpPointerSpace::Clear() {
  // Release the pages back to the operating system.
  CHECK_NE(madvise(Begin(), Limit() - Begin(), MADV_DONTNEED), -1) << "madvise failed";
  // Reset the end of the space back to the beginning, we move the end forward as we allocate
  // objects.
  SetEnd(Begin() + sizeof(BlockHeader));
  objects_allocated_ = 0;
  bytes_allocated_ = 0;
  growth_end_ = Limit();
  {
    MutexLock mu(Thread::Current(), block_lock_);
    num_blocks_ = 0;
  }
}

void BumpPointerSpace::Dump(std::ostream& os) const {
  os << reinterpret_cast<void*>(Begin()) << "-" << reinterpret_cast<void*>(End()) << " - "
     << reinterpret_cast<void*>(Limit());
}

mirror::Object* BumpPointerSpace::GetNextObject(mirror::Object* obj) {
  const uintptr_t position = reinterpret_cast<uintptr_t>(obj) + obj->SizeOf();
  return reinterpret_cast<mirror::Object*>(RoundUp(position, kAlignment));
}

void BumpPointerSpace::RevokeThreadLocalBuffers(Thread* thread) {
  MutexLock mu(Thread::Current(), block_lock_);
  RevokeThreadLocalBuffersLocked(thread);
}

void BumpPointerSpace::RevokeAllThreadLocalBuffers() {
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::runtime_shutdown_lock_);
  MutexLock mu2(self, *Locks::thread_list_lock_);
  // TODO: Not do a copy of the thread list?
  std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
  for (Thread* thread : thread_list) {
    RevokeThreadLocalBuffers(thread);
  }
}

void BumpPointerSpace::UpdateMainBlock() {
  BlockHeader* header = reinterpret_cast<BlockHeader*>(Begin());
  header->size_ = Size() - sizeof(BlockHeader);
  DCHECK_EQ(num_blocks_, 0U);
}

// Returns the start of the storage.
byte* BumpPointerSpace::AllocBlock(size_t bytes) {
  bytes = RoundUp(bytes, kAlignment);
  if (!num_blocks_) {
    UpdateMainBlock();
  }
  byte* storage = reinterpret_cast<byte*>(
      AllocNonvirtualWithoutAccounting(bytes + sizeof(BlockHeader)));
  if (LIKELY(storage != nullptr)) {
    BlockHeader* header = reinterpret_cast<BlockHeader*>(storage);
    header->size_ = bytes;  // Write out the block header.
    storage += sizeof(BlockHeader);
    ++num_blocks_;
  }
  return storage;
}

void BumpPointerSpace::Walk(ObjectCallback* callback, void* arg) {
  byte* pos = Begin();

  {
    MutexLock mu(Thread::Current(), block_lock_);
    // If we have 0 blocks then we need to update the main header since we have bump pointer style
    // allocation into an unbounded region (actually bounded by Capacity()).
    if (num_blocks_ == 0) {
      UpdateMainBlock();
    }
  }

  while (pos < End()) {
    BlockHeader* header = reinterpret_cast<BlockHeader*>(pos);
    size_t block_size = header->size_;
    pos += sizeof(BlockHeader);  // Skip the header so that we know where the objects
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(pos);
    const mirror::Object* end = reinterpret_cast<const mirror::Object*>(pos + block_size);
    CHECK_LE(reinterpret_cast<const byte*>(end), End());
    // We don't know how many objects are allocated in the current block. When we hit a null class
    // assume its the end. TODO: Have a thread update the header when it flushes the block?
    while (obj < end && obj->GetClass() != nullptr) {
      callback(obj, arg);
      obj = GetNextObject(obj);
    }
    pos += block_size;
  }
}

bool BumpPointerSpace::IsEmpty() const {
  return Size() == sizeof(BlockHeader);
}

uint64_t BumpPointerSpace::GetBytesAllocated() {
  // Start out pre-determined amount (blocks which are not being allocated into).
  uint64_t total = static_cast<uint64_t>(bytes_allocated_.Load());
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::runtime_shutdown_lock_);
  MutexLock mu2(self, *Locks::thread_list_lock_);
  std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
  MutexLock mu3(Thread::Current(), block_lock_);
  // If we don't have any blocks, we don't have any thread local buffers. This check is required
  // since there can exist multiple bump pointer spaces which exist at the same time.
  if (num_blocks_ > 0) {
    for (Thread* thread : thread_list) {
      total += thread->thread_local_pos_ - thread->thread_local_start_;
    }
  }
  return total;
}

uint64_t BumpPointerSpace::GetObjectsAllocated() {
  // Start out pre-determined amount (blocks which are not being allocated into).
  uint64_t total = static_cast<uint64_t>(objects_allocated_.Load());
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::runtime_shutdown_lock_);
  MutexLock mu2(self, *Locks::thread_list_lock_);
  std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
  MutexLock mu3(Thread::Current(), block_lock_);
  // If we don't have any blocks, we don't have any thread local buffers. This check is required
  // since there can exist multiple bump pointer spaces which exist at the same time.
  if (num_blocks_ > 0) {
    for (Thread* thread : thread_list) {
      total += thread->thread_local_objects_;
    }
  }
  return total;
}

void BumpPointerSpace::RevokeThreadLocalBuffersLocked(Thread* thread) {
  objects_allocated_.FetchAndAdd(thread->thread_local_objects_);
  bytes_allocated_.FetchAndAdd(thread->thread_local_pos_ - thread->thread_local_start_);
  thread->SetTlab(nullptr, nullptr);
}

bool BumpPointerSpace::AllocNewTlab(Thread* self, size_t bytes) {
  MutexLock mu(Thread::Current(), block_lock_);
  RevokeThreadLocalBuffersLocked(self);
  byte* start = AllocBlock(bytes);
  if (start == nullptr) {
    return false;
  }
  self->SetTlab(start, start + bytes);
  return true;
}

}  // namespace space
}  // namespace gc
}  // namespace art
