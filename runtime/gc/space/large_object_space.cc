/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "large_object_space.h"

#include "base/logging.h"
#include "base/stl_util.h"
#include "UniquePtr.h"
#include "image.h"
#include "os.h"
#include "thread.h"
#include "utils.h"

namespace art {
namespace gc {
namespace space {

void LargeObjectSpace::SwapBitmaps() {
  live_objects_.swap(mark_objects_);
  // Swap names to get more descriptive diagnostics.
  std::string temp_name = live_objects_->GetName();
  live_objects_->SetName(mark_objects_->GetName());
  mark_objects_->SetName(temp_name);
}

LargeObjectSpace::LargeObjectSpace(const std::string& name)
    : DiscontinuousSpace(name, kGcRetentionPolicyAlwaysCollect),
      num_bytes_allocated_(0), num_objects_allocated_(0), total_bytes_allocated_(0),
      total_objects_allocated_(0) {
}


void LargeObjectSpace::CopyLiveToMarked() {
  mark_objects_->CopyFrom(*live_objects_.get());
}

LargeObjectMapSpace::LargeObjectMapSpace(const std::string& name)
    : LargeObjectSpace(name),
      lock_("large object map space lock", kAllocSpaceLock) {}

LargeObjectMapSpace* LargeObjectMapSpace::Create(const std::string& name) {
  return new LargeObjectMapSpace(name);
}

mirror::Object* LargeObjectMapSpace::Alloc(Thread* self, size_t num_bytes) {
  MemMap* mem_map = MemMap::MapAnonymous("large object space allocation", NULL, num_bytes,
                                         PROT_READ | PROT_WRITE);
  if (mem_map == NULL) {
    return NULL;
  }
  MutexLock mu(self, lock_);
  mirror::Object* obj = reinterpret_cast<mirror::Object*>(mem_map->Begin());
  large_objects_.push_back(obj);
  mem_maps_.Put(obj, mem_map);
  size_t allocation_size = mem_map->Size();
  num_bytes_allocated_ += allocation_size;
  total_bytes_allocated_ += allocation_size;
  ++num_objects_allocated_;
  ++total_objects_allocated_;
  return obj;
}

size_t LargeObjectMapSpace::Free(Thread* self, mirror::Object* ptr) {
  MutexLock mu(self, lock_);
  MemMaps::iterator found = mem_maps_.find(ptr);
  CHECK(found != mem_maps_.end()) << "Attempted to free large object which was not live";
  DCHECK_GE(num_bytes_allocated_, found->second->Size());
  size_t allocation_size = found->second->Size();
  num_bytes_allocated_ -= allocation_size;
  --num_objects_allocated_;
  delete found->second;
  mem_maps_.erase(found);
  return allocation_size;
}

size_t LargeObjectMapSpace::AllocationSize(const mirror::Object* obj) {
  MutexLock mu(Thread::Current(), lock_);
  MemMaps::iterator found = mem_maps_.find(const_cast<mirror::Object*>(obj));
  CHECK(found != mem_maps_.end()) << "Attempted to get size of a large object which is not live";
  return found->second->Size();
}

size_t LargeObjectSpace::FreeList(Thread* self, size_t num_ptrs, mirror::Object** ptrs) {
  size_t total = 0;
  for (size_t i = 0; i < num_ptrs; ++i) {
    if (kDebugSpaces) {
      CHECK(Contains(ptrs[i]));
    }
    total += Free(self, ptrs[i]);
  }
  return total;
}

void LargeObjectMapSpace::Walk(DlMallocSpace::WalkCallback callback, void* arg) {
  MutexLock mu(Thread::Current(), lock_);
  for (MemMaps::iterator it = mem_maps_.begin(); it != mem_maps_.end(); ++it) {
    MemMap* mem_map = it->second;
    callback(mem_map->Begin(), mem_map->End(), mem_map->Size(), arg);
    callback(NULL, NULL, 0, arg);
  }
}

bool LargeObjectMapSpace::Contains(const mirror::Object* obj) const {
  Thread* self = Thread::Current();
  if (lock_.IsExclusiveHeld(self)) {
    // We hold lock_ so do the check.
    return mem_maps_.find(const_cast<mirror::Object*>(obj)) != mem_maps_.end();
  } else {
    MutexLock mu(self, lock_);
    return mem_maps_.find(const_cast<mirror::Object*>(obj)) != mem_maps_.end();
  }
}

FreeListSpace* FreeListSpace::Create(const std::string& name, byte* requested_begin, size_t size) {
  CHECK(size % kAlignment == 0);
  MemMap* mem_map = MemMap::MapAnonymous(name.c_str(), requested_begin, size,
                                         PROT_READ | PROT_WRITE);
  CHECK(mem_map != NULL) << "Failed to allocate large object space mem map";
  return new FreeListSpace(name, mem_map, mem_map->Begin(), mem_map->End());
}

FreeListSpace::FreeListSpace(const std::string& name, MemMap* mem_map, byte* begin, byte* end)
    : LargeObjectSpace(name),
      begin_(begin),
      end_(end),
      mem_map_(mem_map),
      lock_("free list space lock", kAllocSpaceLock) {
  chunks_.resize(Size() / kAlignment + 1);
  // Add a dummy chunk so we don't need to handle chunks having no next chunk.
  chunks_.back().SetSize(kAlignment, false);
  // Start out with one large free chunk.
  AddFreeChunk(begin_, end_ - begin_, NULL);
}

FreeListSpace::~FreeListSpace() {}

void FreeListSpace::AddFreeChunk(void* address, size_t size, Chunk* previous) {
  Chunk* chunk = ChunkFromAddr(address);
  chunk->SetSize(size, true);
  chunk->SetPrevious(previous);
  Chunk* next_chunk = GetNextChunk(chunk);
  next_chunk->SetPrevious(chunk);
  free_chunks_.insert(chunk);
}

FreeListSpace::Chunk* FreeListSpace::ChunkFromAddr(void* address) {
  size_t offset = reinterpret_cast<byte*>(address) - Begin();
  DCHECK(IsAligned<kAlignment>(offset));
  DCHECK_LT(offset, Size());
  return &chunks_[offset / kAlignment];
}

void* FreeListSpace::AddrFromChunk(Chunk* chunk) {
  return reinterpret_cast<void*>(Begin() + (chunk - &chunks_.front()) * kAlignment);
}

void FreeListSpace::RemoveFreeChunk(Chunk* chunk) {
  // TODO: C++0x
  // TODO: Improve performance, this might be slow.
  std::pair<FreeChunks::iterator, FreeChunks::iterator> range = free_chunks_.equal_range(chunk);
  for (FreeChunks::iterator it = range.first; it != range.second; ++it) {
    if (*it == chunk) {
      free_chunks_.erase(it);
      return;
    }
  }
}

void FreeListSpace::Walk(DlMallocSpace::WalkCallback callback, void* arg) {
  MutexLock mu(Thread::Current(), lock_);
  for (Chunk* chunk = &chunks_.front(); chunk < &chunks_.back(); ) {
    if (!chunk->IsFree()) {
      size_t size = chunk->GetSize();
      void* begin = AddrFromChunk(chunk);
      void* end = reinterpret_cast<void*>(reinterpret_cast<byte*>(begin) + size);
      callback(begin, end, size, arg);
      callback(NULL, NULL, 0, arg);
    }
    chunk = GetNextChunk(chunk);
  }
}

size_t FreeListSpace::Free(Thread* self, mirror::Object* obj) {
  MutexLock mu(self, lock_);
  CHECK(Contains(obj));
  // Check adjacent chunks to see if we need to combine.
  Chunk* chunk = ChunkFromAddr(obj);
  CHECK(!chunk->IsFree());

  size_t allocation_size = chunk->GetSize();
  if (kIsDebugBuild) {
    memset(obj, 0xEB, allocation_size);
  }
  madvise(obj, allocation_size, MADV_DONTNEED);
  num_objects_allocated_--;
  num_bytes_allocated_ -= allocation_size;
  Chunk* prev = chunk->GetPrevious();
  Chunk* next = GetNextChunk(chunk);

  // Combine any adjacent free chunks
  size_t extra_size = chunk->GetSize();
  if (next->IsFree()) {
    extra_size += next->GetSize();
    RemoveFreeChunk(next);
  }
  if (prev != NULL && prev->IsFree()) {
    RemoveFreeChunk(prev);
    AddFreeChunk(AddrFromChunk(prev), prev->GetSize() + extra_size, prev->GetPrevious());
  } else {
    AddFreeChunk(AddrFromChunk(chunk), extra_size, prev);
  }
  return allocation_size;
}

bool FreeListSpace::Contains(const mirror::Object* obj) const {
  return mem_map_->HasAddress(obj);
}

FreeListSpace::Chunk* FreeListSpace::GetNextChunk(Chunk* chunk) {
  return chunk + chunk->GetSize() / kAlignment;
}

size_t FreeListSpace::AllocationSize(const mirror::Object* obj) {
  Chunk* chunk = ChunkFromAddr(const_cast<mirror::Object*>(obj));
  CHECK(!chunk->IsFree());
  return chunk->GetSize();
}

mirror::Object* FreeListSpace::Alloc(Thread* self, size_t num_bytes) {
  MutexLock mu(self, lock_);
  num_bytes = RoundUp(num_bytes, kAlignment);
  Chunk temp;
  temp.SetSize(num_bytes);
  // Find the smallest chunk at least num_bytes in size.
  FreeChunks::iterator found = free_chunks_.lower_bound(&temp);
  if (found == free_chunks_.end()) {
    // Out of memory, or too much fragmentation.
    return NULL;
  }
  Chunk* chunk = *found;
  free_chunks_.erase(found);
  CHECK(chunk->IsFree());
  void* addr = AddrFromChunk(chunk);
  size_t chunk_size = chunk->GetSize();
  chunk->SetSize(num_bytes);
  if (chunk_size > num_bytes) {
    // Split the chunk into two chunks.
    Chunk* new_chunk = GetNextChunk(chunk);
    AddFreeChunk(AddrFromChunk(new_chunk), chunk_size - num_bytes, chunk);
  }

  num_objects_allocated_++;
  total_objects_allocated_++;
  num_bytes_allocated_ += num_bytes;
  total_bytes_allocated_ += num_bytes;
  return reinterpret_cast<mirror::Object*>(addr);
}

void FreeListSpace::Dump(std::ostream& os) const {
  os << GetName() << " -"
     << " begin: " << reinterpret_cast<void*>(Begin())
     << " end: " << reinterpret_cast<void*>(End());
}

}  // namespace space
}  // namespace gc
}  // namespace art
