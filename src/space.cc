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

#include "space.h"

#include "UniquePtr.h"
#include "dlmalloc.h"
#include "file.h"
#include "image.h"
#include "logging.h"
#include "os.h"
#include "space_bitmap.h"
#include "stl_util.h"
#include "utils.h"

namespace art {

#ifndef NDEBUG
static const bool kDebugSpaces = true;
#else
static const bool kDebugSpaces = false;
#endif
// Magic padding value that we use to check for buffer overruns.
static const word kPaddingValue = 0xBAC0BAC0;

// TODO: Remove define macro
#define CHECK_MEMORY_CALL(call, args, what) \
  do { \
    int rc = call args; \
    if (UNLIKELY(rc != 0)) { \
      errno = rc; \
      PLOG(FATAL) << # call << " failed for " << what; \
    } \
  } while (false)

Space::Space(const std::string& name, GcRetentionPolicy gc_retention_policy)
    : name_(name),
      gc_retention_policy_(gc_retention_policy) {

}

ContinuousSpace::ContinuousSpace(const std::string& name, byte* begin, byte* end,
                                 GcRetentionPolicy gc_retention_policy)
    : Space(name, gc_retention_policy),
      begin_(begin),
      end_(end) {

}

MemMapSpace::MemMapSpace(const std::string& name, MemMap* mem_map, size_t initial_size,
                         GcRetentionPolicy gc_retention_policy)
    : ContinuousSpace(name, mem_map->Begin(), mem_map->Begin() + initial_size, gc_retention_policy),
      mem_map_(mem_map)
{

}

size_t AllocSpace::bitmap_index_ = 0;

AllocSpace::AllocSpace(const std::string& name, MemMap* mem_map, void* mspace, byte* begin,
                       byte* end, size_t growth_limit)
    : MemMapSpace(name, mem_map, end - begin, GCRP_ALWAYS_COLLECT),
      num_bytes_allocated_(0), num_objects_allocated_(0),
      lock_("allocation space lock", kAllocSpaceLock), mspace_(mspace),
      growth_limit_(growth_limit) {
  CHECK(mspace != NULL);

  size_t bitmap_index = bitmap_index_++;

  static const uintptr_t kGcCardSize = static_cast<uintptr_t>(GC_CARD_SIZE);
  CHECK(reinterpret_cast<uintptr_t>(mem_map->Begin()) % kGcCardSize == 0);
  CHECK(reinterpret_cast<uintptr_t>(mem_map->End()) % kGcCardSize == 0);

  live_bitmap_.reset(SpaceBitmap::Create(
      StringPrintf("allocspace-%s-live-bitmap-%d", name.c_str(), static_cast<int>(bitmap_index)),
      Begin(), Capacity()));
  DCHECK(live_bitmap_.get() != NULL) << "could not create allocspace live bitmap #" << bitmap_index;

  mark_bitmap_.reset(SpaceBitmap::Create(
      StringPrintf("allocspace-%s-mark-bitmap-%d", name.c_str(), static_cast<int>(bitmap_index)),
      Begin(), Capacity()));
  DCHECK(live_bitmap_.get() != NULL) << "could not create allocspace mark bitmap #" << bitmap_index;
}

AllocSpace* AllocSpace::Create(const std::string& name, size_t initial_size, size_t growth_limit,
                               size_t capacity, byte* requested_begin) {
  // Memory we promise to dlmalloc before it asks for morecore.
  // Note: making this value large means that large allocations are unlikely to succeed as dlmalloc
  // will ask for this memory from sys_alloc which will fail as the footprint (this value plus the
  // size of the large allocation) will be greater than the footprint limit.
  size_t starting_size = kPageSize;
  uint64_t start_time = 0;
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    start_time = NanoTime();
    VLOG(startup) << "Space::CreateAllocSpace entering " << name
                  << " initial_size=" << PrettySize(initial_size)
                  << " growth_limit=" << PrettySize(growth_limit)
                  << " capacity=" << PrettySize(capacity)
                  << " requested_begin=" << reinterpret_cast<void*>(requested_begin);
  }

  // Sanity check arguments
  if (starting_size > initial_size) {
    initial_size = starting_size;
  }
  if (initial_size > growth_limit) {
    LOG(ERROR) << "Failed to create alloc space (" << name << ") where the initial size ("
        << PrettySize(initial_size) << ") is larger than its capacity ("
        << PrettySize(growth_limit) << ")";
    return NULL;
  }
  if (growth_limit > capacity) {
    LOG(ERROR) << "Failed to create alloc space (" << name << ") where the growth limit capacity ("
        << PrettySize(growth_limit) << ") is larger than the capacity ("
        << PrettySize(capacity) << ")";
    return NULL;
  }

  // Page align growth limit and capacity which will be used to manage mmapped storage
  growth_limit = RoundUp(growth_limit, kPageSize);
  capacity = RoundUp(capacity, kPageSize);

  UniquePtr<MemMap> mem_map(MemMap::MapAnonymous(name.c_str(), requested_begin,
                                                 capacity, PROT_READ | PROT_WRITE));
  if (mem_map.get() == NULL) {
    LOG(ERROR) << "Failed to allocate pages for alloc space (" << name << ") of size "
        << PrettySize(capacity);
    return NULL;
  }

  void* mspace = AllocSpace::CreateMallocSpace(mem_map->Begin(), starting_size, initial_size);
  if (mspace == NULL) {
    LOG(ERROR) << "Failed to initialize mspace for alloc space (" << name << ")";
    return NULL;
  }

  // Protect memory beyond the initial size.
  byte* end = mem_map->Begin() + starting_size;
  if (capacity - initial_size > 0) {
    CHECK_MEMORY_CALL(mprotect, (end, capacity - initial_size, PROT_NONE), name);
  }

  // Everything is set so record in immutable structure and leave
  MemMap* mem_map_ptr = mem_map.release();
  AllocSpace* space = new AllocSpace(name, mem_map_ptr, mspace, mem_map_ptr->Begin(), end,
                                     growth_limit);
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "Space::CreateAllocSpace exiting (" << PrettyDuration(NanoTime() - start_time)
        << " ) " << *space;
  }
  return space;
}

void* AllocSpace::CreateMallocSpace(void* begin, size_t morecore_start, size_t initial_size) {
  // clear errno to allow PLOG on error
  errno = 0;
  // create mspace using our backing storage starting at begin and with a footprint of
  // morecore_start. Don't use an internal dlmalloc lock (as we already hold heap lock). When
  // morecore_start bytes of memory is exhaused morecore will be called.
  void* msp = create_mspace_with_base(begin, morecore_start, false /*locked*/);
  if (msp != NULL) {
    // Do not allow morecore requests to succeed beyond the initial size of the heap
    mspace_set_footprint_limit(msp, initial_size);
  } else {
    PLOG(ERROR) << "create_mspace_with_base failed";
  }
  return msp;
}

void AllocSpace::SwapBitmaps() {
  SpaceBitmap* temp_live_bitmap = live_bitmap_.release();
  live_bitmap_.reset(mark_bitmap_.release());
  mark_bitmap_.reset(temp_live_bitmap);
  // Swap names to get more descriptive diagnostics.
  std::string temp_name = live_bitmap_->GetName();
  live_bitmap_->SetName(mark_bitmap_->GetName());
  mark_bitmap_->SetName(temp_name);
}

Object* AllocSpace::AllocWithoutGrowthLocked(size_t num_bytes) {
  if (kDebugSpaces) {
    num_bytes += sizeof(word);
  }

  Object* result = reinterpret_cast<Object*>(mspace_calloc(mspace_, 1, num_bytes));
  if (kDebugSpaces && result != NULL) {
    CHECK(Contains(result)) << "Allocation (" << reinterpret_cast<void*>(result)
        << ") not in bounds of allocation space " << *this;
    // Put a magic pattern before and after the allocation.
    *reinterpret_cast<word*>(reinterpret_cast<byte*>(result) + AllocationSize(result)
        - sizeof(word) - kChunkOverhead) = kPaddingValue;
  }
  num_bytes_allocated_ += AllocationSize(result);
  ++num_objects_allocated_;
  return result;
}

Object* AllocSpace::AllocWithoutGrowth(Thread* self, size_t num_bytes) {
  MutexLock mu(self, lock_);
  return AllocWithoutGrowthLocked(num_bytes);
}

Object* AllocSpace::AllocWithGrowth(Thread* self, size_t num_bytes) {
  MutexLock mu(self, lock_);
  // Grow as much as possible within the mspace.
  size_t max_allowed = Capacity();
  mspace_set_footprint_limit(mspace_, max_allowed);
  // Try the allocation.
  void* ptr = AllocWithoutGrowthLocked(num_bytes);
  // Shrink back down as small as possible.
  size_t footprint = mspace_footprint(mspace_);
  mspace_set_footprint_limit(mspace_, footprint);
  // Return the new allocation or NULL.
  Object* result = reinterpret_cast<Object*>(ptr);
  CHECK(!kDebugSpaces || result == NULL || Contains(result));
  return result;
}

void AllocSpace::SetGrowthLimit(size_t growth_limit) {
  growth_limit = RoundUp(growth_limit, kPageSize);
  growth_limit_ = growth_limit;
  if (Size() > growth_limit_) {
    end_ = begin_ + growth_limit;
  }
}

AllocSpace* AllocSpace::CreateZygoteSpace() {
  end_ = reinterpret_cast<byte*>(RoundUp(reinterpret_cast<uintptr_t>(end_), kPageSize));
  DCHECK(IsAligned<GC_CARD_SIZE>(begin_));
  DCHECK(IsAligned<GC_CARD_SIZE>(end_));
  DCHECK(IsAligned<kPageSize>(begin_));
  DCHECK(IsAligned<kPageSize>(end_));
  size_t size = RoundUp(Size(), kPageSize);
  // Trim the heap so that we minimize the size of the Zygote space.
  Trim();
  // Trim our mem-map to free unused pages.
  GetMemMap()->UnMapAtEnd(end_);
  // TODO: Not hardcode these in?
  const size_t starting_size = kPageSize;
  const size_t initial_size = 2 * MB;
  // Remaining size is for the new alloc space.
  const size_t growth_limit = growth_limit_ - size;
  const size_t capacity = Capacity() - size;
  VLOG(heap) << "Begin " << reinterpret_cast<const void*>(begin_);
  VLOG(heap) << "End " << reinterpret_cast<const void*>(end_);
  VLOG(heap) << "Size " << size;
  VLOG(heap) << "GrowthLimit " << growth_limit_;
  VLOG(heap) << "Capacity " << Capacity();
  SetGrowthLimit(RoundUp(size, kPageSize));
  SetFootprintLimit(RoundUp(size, kPageSize));
  // FIXME: Do we need reference counted pointers here?
  // Make the two spaces share the same mark bitmaps since the bitmaps span both of the spaces.
  VLOG(heap) << "Creating new AllocSpace: ";
  VLOG(heap) << "Size " << GetMemMap()->Size();
  VLOG(heap) << "GrowthLimit " << PrettySize(growth_limit);
  VLOG(heap) << "Capacity " << PrettySize(capacity);
  UniquePtr<MemMap> mem_map(MemMap::MapAnonymous(GetName().c_str(), End(), capacity, PROT_READ | PROT_WRITE));
  void* mspace = CreateMallocSpace(end_, starting_size, initial_size);
  // Protect memory beyond the initial size.
  byte* end = mem_map->Begin() + starting_size;
  if (capacity - initial_size > 0) {
    CHECK_MEMORY_CALL(mprotect, (end, capacity - initial_size, PROT_NONE), name_.c_str());
  }
  AllocSpace* alloc_space = new AllocSpace(name_, mem_map.release(), mspace, end_, end, growth_limit);
  live_bitmap_->SetHeapLimit(reinterpret_cast<uintptr_t>(End()));
  CHECK_EQ(live_bitmap_->HeapLimit(), reinterpret_cast<uintptr_t>(End()));
  mark_bitmap_->SetHeapLimit(reinterpret_cast<uintptr_t>(End()));
  CHECK_EQ(mark_bitmap_->HeapLimit(), reinterpret_cast<uintptr_t>(End()));
  name_ += "-zygote-transformed";
  VLOG(heap) << "zygote space creation done";
  return alloc_space;
}

void AllocSpace::Free(Thread* self, Object* ptr) {
  MutexLock mu(self, lock_);
  if (kDebugSpaces) {
    CHECK(ptr != NULL);
    CHECK(Contains(ptr)) << "Free (" << ptr << ") not in bounds of heap " << *this;
    CHECK_EQ(
        *reinterpret_cast<word*>(reinterpret_cast<byte*>(ptr) + AllocationSize(ptr) -
            sizeof(word) - kChunkOverhead), kPaddingValue);
  }
  num_bytes_allocated_ -= AllocationSize(ptr);
  --num_objects_allocated_;
  mspace_free(mspace_, ptr);
}

void AllocSpace::FreeList(Thread* self, size_t num_ptrs, Object** ptrs) {
  MutexLock mu(self, lock_);
  if (kDebugSpaces) {
    CHECK(ptrs != NULL);
    size_t num_broken_ptrs = 0;
    for (size_t i = 0; i < num_ptrs; i++) {
      if (!Contains(ptrs[i])) {
        num_broken_ptrs++;
        LOG(ERROR) << "FreeList[" << i << "] (" << ptrs[i] << ") not in bounds of heap " << *this;
      } else {
        size_t size = mspace_usable_size(ptrs[i]);
        memset(ptrs[i], 0xEF, size);
      }
    }
    CHECK_EQ(num_broken_ptrs, 0u);
  }
  for (size_t i = 0; i < num_ptrs; i++) {
    num_bytes_allocated_ -= AllocationSize(ptrs[i]);
  }
  num_objects_allocated_ -= num_ptrs;
  mspace_bulk_free(mspace_, reinterpret_cast<void**>(ptrs), num_ptrs);
}

// Callback from dlmalloc when it needs to increase the footprint
extern "C" void* art_heap_morecore(void* mspace, intptr_t increment) {
  Heap* heap = Runtime::Current()->GetHeap();
  DCHECK_EQ(heap->GetAllocSpace()->GetMspace(), mspace);
  return heap->GetAllocSpace()->MoreCore(increment);
}

void* AllocSpace::MoreCore(intptr_t increment) {
  lock_.AssertHeld(Thread::Current());
  byte* original_end = end_;
  if (increment != 0) {
    VLOG(heap) << "AllocSpace::MoreCore " << PrettySize(increment);
    byte* new_end = original_end + increment;
    if (increment > 0) {
#if DEBUG_SPACES
      // Should never be asked to increase the allocation beyond the capacity of the space. Enforced
      // by mspace_set_footprint_limit.
      CHECK_LE(new_end, Begin() + Capacity());
#endif
      CHECK_MEMORY_CALL(mprotect, (original_end, increment, PROT_READ | PROT_WRITE), GetName());
    } else {
#if DEBUG_SPACES
      // Should never be asked for negative footprint (ie before begin)
      CHECK_GT(original_end + increment, Begin());
#endif
      // Advise we don't need the pages and protect them
      // TODO: by removing permissions to the pages we may be causing TLB shoot-down which can be
      // expensive (note the same isn't true for giving permissions to a page as the protected
      // page shouldn't be in a TLB). We should investigate performance impact of just
      // removing ignoring the memory protection change here and in Space::CreateAllocSpace. It's
      // likely just a useful debug feature.
      size_t size = -increment;
      CHECK_MEMORY_CALL(madvise, (new_end, size, MADV_DONTNEED), GetName());
      CHECK_MEMORY_CALL(mprotect, (new_end, size, PROT_NONE), GetName());
    }
    // Update end_
    end_ = new_end;
  }
  return original_end;
}

size_t AllocSpace::AllocationSize(const Object* obj) {
  return mspace_usable_size(const_cast<void*>(reinterpret_cast<const void*>(obj))) +
      kChunkOverhead;
}

void MspaceMadviseCallback(void* start, void* end, size_t used_bytes, void* /* arg */) {
  // Is this chunk in use?
  if (used_bytes != 0) {
    return;
  }
  // Do we have any whole pages to give back?
  start = reinterpret_cast<void*>(RoundUp(reinterpret_cast<uintptr_t>(start), kPageSize));
  end = reinterpret_cast<void*>(RoundDown(reinterpret_cast<uintptr_t>(end), kPageSize));
  if (end > start) {
    size_t length = reinterpret_cast<byte*>(end) - reinterpret_cast<byte*>(start);
    CHECK_MEMORY_CALL(madvise, (start, length, MADV_DONTNEED), "trim");
  }
}

void AllocSpace::Trim() {
  MutexLock mu(Thread::Current(), lock_);
  // Trim to release memory at the end of the space.
  mspace_trim(mspace_, 0);
  // Visit space looking for page-sized holes to advise the kernel we don't need.
  mspace_inspect_all(mspace_, MspaceMadviseCallback, NULL);
}

void AllocSpace::Walk(void(*callback)(void *start, void *end, size_t num_bytes, void* callback_arg),
                      void* arg) {
  MutexLock mu(Thread::Current(), lock_);
  mspace_inspect_all(mspace_, callback, arg);
  callback(NULL, NULL, 0, arg);  // Indicate end of a space.
}

size_t AllocSpace::GetFootprintLimit() {
  MutexLock mu(Thread::Current(), lock_);
  return mspace_footprint_limit(mspace_);
}

void AllocSpace::SetFootprintLimit(size_t new_size) {
  MutexLock mu(Thread::Current(), lock_);
  VLOG(heap) << "AllocSpace::SetFootprintLimit " << PrettySize(new_size);
  // Compare against the actual footprint, rather than the Size(), because the heap may not have
  // grown all the way to the allowed size yet.
  size_t current_space_size = mspace_footprint(mspace_);
  if (new_size < current_space_size) {
    // Don't let the space grow any more.
    new_size = current_space_size;
  }
  mspace_set_footprint_limit(mspace_, new_size);
}

size_t ImageSpace::bitmap_index_ = 0;

ImageSpace::ImageSpace(const std::string& name, MemMap* mem_map)
    : MemMapSpace(name, mem_map, mem_map->Size(), GCRP_NEVER_COLLECT) {
  const size_t bitmap_index = bitmap_index_++;
  live_bitmap_.reset(SpaceBitmap::Create(
      StringPrintf("imagespace-%s-live-bitmap-%d", name.c_str(), static_cast<int>(bitmap_index)),
      Begin(), Capacity()));
  DCHECK(live_bitmap_.get() != NULL) << "could not create imagespace live bitmap #" << bitmap_index;
}

ImageSpace* ImageSpace::Create(const std::string& image_file_name) {
  CHECK(!image_file_name.empty());

  uint64_t start_time = 0;
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    start_time = NanoTime();
    LOG(INFO) << "Space::CreateImageSpace entering" << " image_file_name=" << image_file_name;
  }

  UniquePtr<File> file(OS::OpenFile(image_file_name.c_str(), false));
  if (file.get() == NULL) {
    LOG(ERROR) << "Failed to open " << image_file_name;
    return NULL;
  }
  ImageHeader image_header;
  bool success = file->ReadFully(&image_header, sizeof(image_header));
  if (!success || !image_header.IsValid()) {
    LOG(ERROR) << "Invalid image header " << image_file_name;
    return NULL;
  }
  UniquePtr<MemMap> map(MemMap::MapFileAtAddress(image_header.GetImageBegin(),
                                                 file->Length(),
                                                 // TODO: selectively PROT_EXEC stubs
                                                 PROT_READ | PROT_WRITE | PROT_EXEC,
                                                 MAP_PRIVATE | MAP_FIXED,
                                                 file->Fd(),
                                                 0));
  if (map.get() == NULL) {
    LOG(ERROR) << "Failed to map " << image_file_name;
    return NULL;
  }
  CHECK_EQ(image_header.GetImageBegin(), map->Begin());
  DCHECK_EQ(0, memcmp(&image_header, map->Begin(), sizeof(ImageHeader)));

  Runtime* runtime = Runtime::Current();
  Object* jni_stub_array = image_header.GetImageRoot(ImageHeader::kJniStubArray);
  runtime->SetJniDlsymLookupStub(down_cast<ByteArray*>(jni_stub_array));

  Object* ame_stub_array = image_header.GetImageRoot(ImageHeader::kAbstractMethodErrorStubArray);
  runtime->SetAbstractMethodErrorStubArray(down_cast<ByteArray*>(ame_stub_array));

  Object* resolution_stub_array =
      image_header.GetImageRoot(ImageHeader::kStaticResolutionStubArray);
  runtime->SetResolutionStubArray(
      down_cast<ByteArray*>(resolution_stub_array), Runtime::kStaticMethod);
  resolution_stub_array = image_header.GetImageRoot(ImageHeader::kUnknownMethodResolutionStubArray);
  runtime->SetResolutionStubArray(
      down_cast<ByteArray*>(resolution_stub_array), Runtime::kUnknownMethod);

  Object* resolution_method = image_header.GetImageRoot(ImageHeader::kResolutionMethod);
  runtime->SetResolutionMethod(down_cast<AbstractMethod*>(resolution_method));

  Object* callee_save_method = image_header.GetImageRoot(ImageHeader::kCalleeSaveMethod);
  runtime->SetCalleeSaveMethod(down_cast<AbstractMethod*>(callee_save_method), Runtime::kSaveAll);
  callee_save_method = image_header.GetImageRoot(ImageHeader::kRefsOnlySaveMethod);
  runtime->SetCalleeSaveMethod(down_cast<AbstractMethod*>(callee_save_method), Runtime::kRefsOnly);
  callee_save_method = image_header.GetImageRoot(ImageHeader::kRefsAndArgsSaveMethod);
  runtime->SetCalleeSaveMethod(down_cast<AbstractMethod*>(callee_save_method), Runtime::kRefsAndArgs);

  ImageSpace* space = new ImageSpace(image_file_name, map.release());
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "Space::CreateImageSpace exiting (" << PrettyDuration(NanoTime() - start_time)
        << ") " << *space;
  }
  return space;
}

void ImageSpace::RecordImageAllocations(SpaceBitmap* live_bitmap) const {
  uint64_t start_time = 0;
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "ImageSpace::RecordImageAllocations entering";
    start_time = NanoTime();
  }
  DCHECK(!Runtime::Current()->IsStarted());
  CHECK(live_bitmap != NULL);
  byte* current = Begin() + RoundUp(sizeof(ImageHeader), kObjectAlignment);
  byte* end = End();
  while (current < end) {
    DCHECK_ALIGNED(current, kObjectAlignment);
    const Object* obj = reinterpret_cast<const Object*>(current);
    live_bitmap->Set(obj);
    current += RoundUp(obj->SizeOf(), kObjectAlignment);
  }
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "ImageSpace::RecordImageAllocations exiting ("
        << PrettyDuration(NanoTime() - start_time) << ")";
  }
}

std::ostream& operator<<(std::ostream& os, const Space& space) {
  space.Dump(os);
  return os;
}

void AllocSpace::Dump(std::ostream& os) const {
  os << GetType()
      << "begin=" << reinterpret_cast<void*>(Begin())
      << ",end=" << reinterpret_cast<void*>(End())
      << ",size=" << PrettySize(Size()) << ",capacity=" << PrettySize(Capacity())
      << ",name=\"" << GetName() << "\"]";
}

void ImageSpace::Dump(std::ostream& os) const {
  os << GetType()
      << "begin=" << reinterpret_cast<void*>(Begin())
      << ",end=" << reinterpret_cast<void*>(End())
      << ",size=" << PrettySize(Size())
      << ",name=\"" << GetName() << "\"]";
}

void LargeObjectSpace::SwapBitmaps() {
  SpaceSetMap* temp_live_objects = live_objects_.release();
  live_objects_.reset(mark_objects_.release());
  mark_objects_.reset(temp_live_objects);
  // Swap names to get more descriptive diagnostics.
  std::string temp_name = live_objects_->GetName();
  live_objects_->SetName(mark_objects_->GetName());
  mark_objects_->SetName(temp_name);
}

DiscontinuousSpace::DiscontinuousSpace(const std::string& name,
                                       GcRetentionPolicy gc_retention_policy)
    : Space(name, gc_retention_policy) {

}

LargeObjectSpace::LargeObjectSpace(const std::string& name)
    : DiscontinuousSpace(name, GCRP_ALWAYS_COLLECT),
      num_bytes_allocated_(0),
      num_objects_allocated_(0) {
  live_objects_.reset(new SpaceSetMap("large live objects"));
  mark_objects_.reset(new SpaceSetMap("large marked objects"));
}


void LargeObjectSpace::CopyLiveToMarked() {
  mark_objects_->CopyFrom(*live_objects_.get());
}

LargeObjectMapSpace::LargeObjectMapSpace(const std::string& name)
    : LargeObjectSpace(name),
      lock_("large object space lock", kAllocSpaceLock)
{

}

LargeObjectMapSpace* LargeObjectMapSpace::Create(const std::string& name) {
  return new LargeObjectMapSpace(name);
}

Object* LargeObjectMapSpace::Alloc(Thread* self, size_t num_bytes) {
  MemMap* mem_map = MemMap::MapAnonymous("allocation", NULL, num_bytes, PROT_READ | PROT_WRITE);
  if (mem_map == NULL) {
    return NULL;
  }
  MutexLock mu(self, lock_);
  Object* obj = reinterpret_cast<Object*>(mem_map->Begin());
  large_objects_.push_back(obj);
  mem_maps_.Put(obj, mem_map);
  num_bytes_allocated_ += mem_map->Size();
  ++num_objects_allocated_;
  return obj;
}

void LargeObjectMapSpace::Free(Thread* self, Object* ptr) {
  MutexLock mu(self, lock_);
  MemMaps::iterator found = mem_maps_.find(ptr);
  CHECK(found != mem_maps_.end()) << "Attempted to free large object which was not live";
  DCHECK_GE(num_bytes_allocated_, found->second->Size());
  num_bytes_allocated_ -= found->second->Size();
  --num_objects_allocated_;
  delete found->second;
  mem_maps_.erase(found);
}

size_t LargeObjectMapSpace::AllocationSize(const Object* obj) {
  MutexLock mu(Thread::Current(), lock_);
  MemMaps::iterator found = mem_maps_.find(const_cast<Object*>(obj));
  CHECK(found != mem_maps_.end()) << "Attempted to get size of a large object which is not live";
  return found->second->Size();
}

void LargeObjectMapSpace::Walk(AllocSpace::WalkCallback callback, void* arg) {
  MutexLock mu(Thread::Current(), lock_);
  for (MemMaps::iterator it = mem_maps_.begin(); it != mem_maps_.end(); ++it) {
    MemMap* mem_map = it->second;
    callback(mem_map->Begin(), mem_map->End(), mem_map->Size(), arg);
    callback(NULL, NULL, 0, arg);
  }
}

bool LargeObjectMapSpace::Contains(const Object* obj) const {
  MutexLock mu(Thread::Current(), lock_);
  return mem_maps_.find(const_cast<Object*>(obj)) != mem_maps_.end();
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

FreeListSpace::~FreeListSpace() {

}

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

void FreeListSpace::Walk(AllocSpace::WalkCallback callback, void* arg) {
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

void FreeListSpace::Free(Thread* self, Object* obj) {
  MutexLock mu(self, lock_);
  CHECK(Contains(obj));
  // Check adjacent chunks to see if we need to combine.
  Chunk* chunk = ChunkFromAddr(obj);
  CHECK(!chunk->IsFree());

  size_t allocation_size = chunk->GetSize();
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
}

bool FreeListSpace::Contains(const Object* obj) const {
  return mem_map_->HasAddress(obj);
}

FreeListSpace::Chunk* FreeListSpace::GetNextChunk(Chunk* chunk) {
  return chunk + chunk->GetSize() / kAlignment;
}

size_t FreeListSpace::AllocationSize(const Object* obj) {
  Chunk* chunk = ChunkFromAddr(const_cast<Object*>(obj));
  CHECK(!chunk->IsFree());
  return chunk->GetSize();
}

Object* FreeListSpace::Alloc(Thread* self, size_t num_bytes) {
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
  num_bytes_allocated_ += num_bytes;
  return reinterpret_cast<Object*>(addr);
}

void FreeListSpace::FreeList(Thread* self, size_t num_ptrs, Object** ptrs) {
  for (size_t i = 0; i < num_ptrs; ++i) {
    Free(self, ptrs[i]);
  }
}

}  // namespace art
