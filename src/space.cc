// Copyright 2011 Google Inc. All Rights Reserved.

#include "space.h"

#include <sys/mman.h>

#include "UniquePtr.h"
#include "dlmalloc.h"
#include "file.h"
#include "image.h"
#include "logging.h"
#include "os.h"
#include "utils.h"

namespace art {

#ifndef NDEBUG
#define DEBUG_SPACES 1
#endif

#define CHECK_MEMORY_CALL(call, args, what) \
  do { \
    int rc = call args; \
    if (UNLIKELY(rc != 0)) { \
      errno = rc; \
      PLOG(FATAL) << # call << " failed for " << what; \
    } \
  } while (false)

AllocSpace* Space::CreateAllocSpace(const std::string& name, size_t initial_size,
                                    size_t growth_limit, size_t capacity,
                                    byte* requested_begin) {
  uint64_t start_time = 0;
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    start_time = NanoTime();
    VLOG(startup) << "Space::CreateAllocSpace entering " << name
                  << " initial_size=" << (initial_size / KB) << "KiB"
                  << " growth_limit=" << (growth_limit / KB) << "KiB"
                  << " capacity=" << (capacity / KB) << "KiB"
                  << " requested_begin=" << reinterpret_cast<void*>(requested_begin);
  }

  // Sanity check arguments
  if (initial_size > growth_limit) {
    LOG(ERROR) << "Failed to create alloc space (" << name << ") where the initial size ("
        << initial_size << ") is larger than its capacity (" << growth_limit << ")";
    return NULL;
  }
  if (growth_limit > capacity) {
    LOG(ERROR) << "Failed to create alloc space (" << name << ") where the growth limit capacity"
        " (" << growth_limit << ") is larger than the capacity (" << capacity << ")";
    return NULL;
  }

  // Page align growth limit and capacity which will be used to manage mmapped storage
  growth_limit = RoundUp(growth_limit, kPageSize);
  capacity = RoundUp(capacity, kPageSize);

  UniquePtr<MemMap> mem_map(MemMap::MapAnonymous(name.c_str(), requested_begin,
                                                 capacity, PROT_READ | PROT_WRITE));
  if (mem_map.get() == NULL) {
    LOG(ERROR) << "Failed to allocate pages for alloc space (" << name << ") of size "
        << capacity << " bytes";
    return NULL;
  }

  void* mspace = AllocSpace::CreateMallocSpace(mem_map->Begin(), initial_size, capacity);
  if (mspace == NULL) {
    LOG(ERROR) << "Failed to initialize mspace for alloc space (" << name << ")";
    return NULL;
  }

  // Protect memory beyond the initial size
  byte* end = mem_map->Begin() + initial_size;
  if (capacity - initial_size > 0) {
    CHECK_MEMORY_CALL(mprotect, (end, capacity - initial_size, PROT_NONE), name);
  }

  // Everything is set so record in immutable structure and leave
  AllocSpace* space = new AllocSpace(name, mem_map.release(), mspace, end, growth_limit);
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    uint64_t duration_ms = (NanoTime() - start_time)/1000/1000;
    LOG(INFO) << "Space::CreateAllocSpace exiting (" << duration_ms << " ms) " << *space;
  }
  return space;
}

void* AllocSpace::CreateMallocSpace(void* begin, size_t size, size_t capacity) {
  // clear errno to allow PLOG on error
  errno = 0;
  // create mspace using our backing storage starting at begin and of half the specified size.
  // Don't use an internal dlmalloc lock (as we already hold heap lock). When size is exhaused
  // morecore will be called.
  void* msp = create_mspace_with_base(begin, size, false /*locked*/);
  if (msp != NULL) {
    // Do not allow morecore requests to succeed beyond the initial size of the heap
    mspace_set_footprint_limit(msp, size);
  } else {
    PLOG(ERROR) << "create_mspace_with_base failed";
  }
  return msp;
}

Object* AllocSpace::AllocWithoutGrowth(size_t num_bytes) {
  Object* result = reinterpret_cast<Object*>(mspace_calloc(mspace_, 1, num_bytes));
#if DEBUG_SPACES
  if (result != NULL) {
    CHECK(Contains(result)) << "Allocation (" << reinterpret_cast<void*>(result)
        << ") not in bounds of heap " << *this;
  }
#endif
  return result;
}

Object* AllocSpace::AllocWithGrowth(size_t num_bytes) {
  // Grow as much as possible within the mspace.
  size_t max_allowed = Capacity();
  mspace_set_footprint_limit(mspace_, max_allowed);
  // Try the allocation.
  void* ptr = AllocWithoutGrowth(num_bytes);
  // Shrink back down as small as possible.
  size_t footprint = mspace_footprint(mspace_);
  mspace_set_footprint_limit(mspace_, footprint);
  // Return the new allocation or NULL.
  Object* result = reinterpret_cast<Object*>(ptr);
  CHECK(result == NULL || Contains(result));
  return result;
}

void AllocSpace::Free(Object* ptr) {
#if DEBUG_SPACES
  CHECK(ptr != NULL);
  CHECK(Contains(ptr)) << "Free (" << ptr << ") not in bounds of heap " << *this;
#endif
  mspace_free(mspace_, ptr);
}

void AllocSpace::FreeList(size_t num_ptrs, Object** ptrs) {
#if DEBUG_SPACES
  CHECK(ptrs != NULL);
  size_t num_broken_ptrs = 0;
  for (size_t i = 0; i < num_ptrs; i++) {
    if(!Contains(ptrs[i])) {
      num_broken_ptrs++;
      LOG(ERROR) << "FreeList[" << i << "] (" << ptrs[i] << ") not in bounds of heap " << *this;
    }
  }
  CHECK_EQ(num_broken_ptrs, 0u);
#endif
  mspace_bulk_free(mspace_, reinterpret_cast<void**>(ptrs), num_ptrs);
}

// Callback from dlmalloc when it needs to increase the footprint
extern "C" void* art_heap_morecore(void* mspace, intptr_t increment) {
  AllocSpace* space = Heap::GetAllocSpace();
  if (LIKELY(space->GetMspace() == mspace)) {
    return space->MoreCore(increment);
  } else {
    // Exhaustively search alloc spaces
    const std::vector<Space*>& spaces = Heap::GetSpaces();
    for (size_t i = 0; i < spaces.size(); i++) {
      if (spaces[i]->IsAllocSpace()) {
        AllocSpace* space = spaces[i]->AsAllocSpace();
        if (mspace == space->GetMspace()) {
          return space->MoreCore(increment);
        }
      }
    }
    LOG(FATAL) << "Unexpected call to art_heap_morecore. mspace: " << mspace
        << " increment: " << increment;
    return NULL;
  }
}

void* AllocSpace::MoreCore(intptr_t increment) {
  byte* original_end = end_;
  if (increment != 0) {
    VLOG(heap) << "AllocSpace::MoreCore " << (increment/KB) << "KiB";
    byte* new_end = original_end + increment;
    if (increment > 0) {
#if DEBUG_SPACES
      // Should never be asked to increase the allocation beyond the capacity of the space. Enforced
      // by mspace_set_footprint_limit.
      CHECK_LE(new_end, Begin() + Capacity());
#endif
      CHECK_MEMORY_CALL(mprotect, (original_end, increment, PROT_READ | PROT_WRITE), GetSpaceName());
    } else {
#if DEBUG_SPACES
      // Should never be asked for negative footprint (ie before begin)
      CHECK_GT(original_end + increment, Begin());
#endif
      // Advise we don't need the pages and protect them
      size_t size = -increment;
      CHECK_MEMORY_CALL(madvise, (new_end, size, MADV_DONTNEED), GetSpaceName());
      CHECK_MEMORY_CALL(mprotect, (new_end, size, PROT_NONE), GetSpaceName());
    }
    // Update end_
    end_ = new_end;
  }
  return original_end;
}

size_t AllocSpace::AllocationSize(const Object* obj) {
  return mspace_usable_size(const_cast<void*>(reinterpret_cast<const void*>(obj))) + kChunkOverhead;
}

// Call back from mspace_inspect_all returning the start and end of chunks and the bytes used,
// if used_bytes is 0 then it indicates the range isn't in use and we madvise to the system that
// we don't need it
static void DontNeed(void* start, void* end, size_t used_bytes, void* num_bytes) {
  if (used_bytes == 0) {
    start = reinterpret_cast<void*>(RoundUp((uintptr_t)start, kPageSize));
    end = reinterpret_cast<void*>(RoundDown((uintptr_t)end, kPageSize));
    if (end > start) {
      // We have a page aligned region to madvise on
      size_t length = reinterpret_cast<byte*>(end) - reinterpret_cast<byte*>(start);
      CHECK_MEMORY_CALL(madvise, (start, length, MADV_DONTNEED), "trim");
    }
  }
}

void AllocSpace::Trim() {
  // Trim to release memory at the end of the space
  mspace_trim(mspace_, 0);
  // Visit space looking for page size holes to advise we don't need
  size_t num_bytes_released = 0;
  mspace_inspect_all(mspace_, DontNeed, &num_bytes_released);
}


void AllocSpace::Walk(void(*callback)(void *start, void *end, size_t num_bytes, void* callback_arg),
                      void* arg) {
  mspace_inspect_all(mspace_, callback, arg);
}

size_t AllocSpace::GetFootprintLimit() {
  return mspace_footprint_limit(mspace_);
}

void AllocSpace::SetFootprintLimit(size_t new_size) {
  VLOG(heap) << "AllocSpace::SetFootprintLimit " << (new_size/KB) << "KiB";
  // Compare against the actual footprint, rather than the Size(), because the heap may not have
  // grown all the way to the allowed size yet.
  //
  size_t current_space_size = mspace_footprint(mspace_);
  if (new_size < current_space_size) {
    // Don't let the space grow any more.
    new_size = current_space_size;
  }
  mspace_set_footprint_limit(mspace_, new_size);
}

ImageSpace* Space::CreateImageSpace(const std::string& image_file_name) {
  CHECK(image_file_name != NULL);

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

  Object* resolution_stub_array = image_header.GetImageRoot(ImageHeader::kInstanceResolutionStubArray);
  runtime->SetResolutionStubArray(
      down_cast<ByteArray*>(resolution_stub_array), Runtime::kInstanceMethod);
  resolution_stub_array = image_header.GetImageRoot(ImageHeader::kStaticResolutionStubArray);
  runtime->SetResolutionStubArray(
      down_cast<ByteArray*>(resolution_stub_array), Runtime::kStaticMethod);
  resolution_stub_array = image_header.GetImageRoot(ImageHeader::kUnknownMethodResolutionStubArray);
  runtime->SetResolutionStubArray(
      down_cast<ByteArray*>(resolution_stub_array), Runtime::kUnknownMethod);

  Object* callee_save_method = image_header.GetImageRoot(ImageHeader::kCalleeSaveMethod);
  runtime->SetCalleeSaveMethod(down_cast<Method*>(callee_save_method), Runtime::kSaveAll);
  callee_save_method = image_header.GetImageRoot(ImageHeader::kRefsOnlySaveMethod);
  runtime->SetCalleeSaveMethod(down_cast<Method*>(callee_save_method), Runtime::kRefsOnly);
  callee_save_method = image_header.GetImageRoot(ImageHeader::kRefsAndArgsSaveMethod);
  runtime->SetCalleeSaveMethod(down_cast<Method*>(callee_save_method), Runtime::kRefsAndArgs);

  ImageSpace* space = new ImageSpace(image_file_name, map.release());
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    uint64_t duration_ms = (NanoTime() - start_time)/1000/1000;
    LOG(INFO) << "Space::CreateImageSpace exiting (" << duration_ms << " ms) " << *space;
  }
  return space;
}

void ImageSpace::RecordImageAllocations(HeapBitmap* live_bitmap) const {
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
    uint64_t duration_ms = (NanoTime() - start_time)/1000/1000;
    LOG(INFO) << "ImageSpace::RecordImageAllocations exiting (" << duration_ms << " ms)";
  }
}

std::ostream& operator<<(std::ostream& os, const Space& space) {
  os << (space.IsImageSpace() ? "Image" : "Alloc") << "Space["
      << "begin=" << reinterpret_cast<void*>(space.Begin())
      << ",end=" << reinterpret_cast<void*>(space.End())
      << ",size=" << (space.Size()/KB) << "KiB"
      << ",capacity=" << (space.Capacity()/KB) << "KiB"
      << ",name=\"" << space.GetSpaceName() << "\"]";
  return os;
}

}  // namespace art
