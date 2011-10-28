// Copyright 2011 Google Inc. All Rights Reserved.

#include "space.h"

#include <sys/mman.h>

#include "UniquePtr.h"
#include "file.h"
#include "image.h"
#include "logging.h"
#include "os.h"
#include "utils.h"

namespace art {

Space* Space::Create(const std::string& name, size_t initial_size, size_t maximum_size, size_t growth_size, byte* requested_base) {
  UniquePtr<Space> space(new Space(name));
  bool success = space->Init(initial_size, maximum_size, growth_size, requested_base);
  if (!success) {
    return NULL;
  } else {
    return space.release();
  }
}

Space* Space::CreateFromImage(const std::string& image_file_name) {
  CHECK(image_file_name != NULL);
  UniquePtr<Space> space(new Space(image_file_name));
  bool success = space->InitFromImage(image_file_name);
  if (!success) {
    return NULL;
  } else {
    return space.release();
  }
}

Space::~Space() {}

void* Space::CreateMallocSpace(void* base,
                               size_t initial_size,
                               size_t maximum_size) {
  errno = 0;
  bool is_locked = false;
  size_t commit_size = initial_size / 2;
  void* msp = create_contiguous_mspace_with_base(commit_size, maximum_size,
                                                 is_locked, base);
  if (msp != NULL) {
    // Do not permit the heap grow past the starting size without our
    // intervention.
    mspace_set_max_allowed_footprint(msp, initial_size);
  } else {
    // There is no guarantee that errno has meaning when the call
    // fails, but it often does.
    PLOG(ERROR) << "create_contiguous_mspace_with_base failed";
  }
  return msp;
}

bool Space::Init(size_t initial_size, size_t maximum_size, size_t growth_size, byte* requested_base) {
  const Runtime* runtime = Runtime::Current();
  if (runtime->IsVerboseStartup()) {
    LOG(INFO) << "Space::Init entering"
              << " initial_size=" << initial_size
              << " maximum_size=" << maximum_size
              << " growth_size=" << growth_size
              << " requested_base=" << reinterpret_cast<void*>(requested_base);
  }
  if (initial_size > growth_size) {
    LOG(ERROR) << "Failed to create space with initial size > growth size ("
               << initial_size << ">" << growth_size << ")";
    return false;
  }
  if (growth_size > maximum_size) {
    LOG(ERROR) << "Failed to create space with growth size > maximum size ("
               << growth_size << ">" << maximum_size << ")";
    return false;
  }
  size_t length = RoundUp(maximum_size, kPageSize);
  int prot = PROT_READ | PROT_WRITE;
  UniquePtr<MemMap> mem_map(MemMap::Map(requested_base, length, prot));
  if (mem_map.get() == NULL) {
    LOG(WARNING) << "Failed to allocate " << length << " bytes for space";
    return false;
  }
  Init(mem_map.release());
  maximum_size_ = maximum_size;
  size_t growth_length = RoundUp(growth_size, kPageSize);
  growth_size_ = growth_size;
  growth_limit_ = base_ + growth_length;
  mspace_ = CreateMallocSpace(base_, initial_size, maximum_size);
  if (mspace_ == NULL) {
    LOG(WARNING) << "Failed to create mspace for space";
    return false;
  }
  if (runtime->IsVerboseStartup()) {
    LOG(INFO) << "Space::Init exiting";
  }
  return true;
}

void Space::Init(MemMap* mem_map) {
  mem_map_.reset(mem_map);
  base_ = mem_map_->GetAddress();
  limit_ = base_ + mem_map->GetLength();
}


bool Space::InitFromImage(const std::string& image_file_name) {
  Runtime* runtime = Runtime::Current();
  if (runtime->IsVerboseStartup()) {
    LOG(INFO) << "Space::InitFromImage entering"
              << " image_file_name=" << image_file_name;
  }
  UniquePtr<File> file(OS::OpenFile(image_file_name.c_str(), false));
  if (file.get() == NULL) {
    LOG(WARNING) << "Failed to open " << image_file_name;
    return false;
  }
  ImageHeader image_header;
  bool success = file->ReadFully(&image_header, sizeof(image_header));
  if (!success || !image_header.IsValid()) {
    LOG(WARNING) << "Invalid image header " << image_file_name;
    return false;
  }
  UniquePtr<MemMap> map(MemMap::Map(image_header.GetImageBaseAddr(),
                                    file->Length(),
                                    // TODO: selectively PROT_EXEC an image subset containing stubs
                                    PROT_READ | PROT_WRITE | PROT_EXEC,
                                    MAP_PRIVATE | MAP_FIXED,
                                    file->Fd(),
                                    0));
  if (map.get() == NULL) {
    LOG(WARNING) << "Failed to map " << image_file_name;
    return false;
  }
  CHECK_EQ(image_header.GetImageBaseAddr(), map->GetAddress());
  image_header_ = reinterpret_cast<ImageHeader*>(map->GetAddress());
  DCHECK_EQ(0, memcmp(&image_header, image_header_, sizeof(ImageHeader)));

  Object* jni_stub_array = image_header.GetImageRoot(ImageHeader::kJniStubArray);
  runtime->SetJniStubArray(down_cast<ByteArray*>(jni_stub_array));

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

  Init(map.release());
  growth_limit_ = limit_;
  if (runtime->IsVerboseStartup()) {
    LOG(INFO) << "Space::InitFromImage exiting";
  }
  return true;
}

Object* Space::AllocWithoutGrowth(size_t num_bytes) {
  DCHECK(mspace_ != NULL);
  return reinterpret_cast<Object*>(mspace_calloc(mspace_, 1, num_bytes));
}

Object* Space::AllocWithGrowth(size_t num_bytes) {
  DCHECK(mspace_ != NULL);
  // Grow as much as possible within the mspace.
  size_t max_allowed = growth_size_;
  mspace_set_max_allowed_footprint(mspace_, max_allowed);
  // Try the allocation.
  void* ptr = AllocWithoutGrowth(num_bytes);
  // Shrink back down as small as possible.
  size_t footprint = mspace_footprint(mspace_);
  mspace_set_max_allowed_footprint(mspace_, footprint);
  // Return the new allocation or NULL.
  return reinterpret_cast<Object*>(ptr);
}

size_t Space::Free(void* ptr) {
  DCHECK(mspace_ != NULL);
  DCHECK(ptr != NULL);
  size_t num_bytes = mspace_usable_size(mspace_, ptr);
  mspace_free(mspace_, ptr);
  return num_bytes;
}

size_t Space::AllocationSize(const Object* obj) {
  DCHECK(mspace_ != NULL);
  return mspace_usable_size(mspace_, obj) + kChunkOverhead;
}

void Space::DontNeed(void* start, void* end, void* num_bytes) {
  start = (void*)RoundUp((uintptr_t)start, kPageSize);
  end = (void*)RoundDown((uintptr_t)end, kPageSize);
  if (start >= end) {
    return;
  }
  size_t length = reinterpret_cast<byte*>(end) - reinterpret_cast<byte*>(start);
  int result = madvise(start, length, MADV_DONTNEED);
  if (result == -1) {
    PLOG(WARNING) << "madvise failed";
  } else {
    *reinterpret_cast<size_t*>(num_bytes) += length;
  }
}

void Space::Trim() {
  CHECK(mspace_ != NULL);
  mspace_trim(mspace_, 0);
  size_t num_bytes_released = 0;
  mspace_walk_free_pages(mspace_, DontNeed, &num_bytes_released);
}

void Space::Walk(void(*callback)(const void*, size_t, const void*, size_t, void*), void* arg) {
  if (mspace_ != NULL) {
    mspace_walk_heap(mspace_, callback, arg);
  }
}

size_t Space::GetMaxAllowedFootprint() {
  DCHECK(mspace_ != NULL);
  return mspace_max_allowed_footprint(mspace_);
}

void Space::SetMaxAllowedFootprint(size_t limit) {
  DCHECK(mspace_ != NULL);

  // Compare against the actual footprint, rather than the
  // max_allowed, because the heap may not have grown all the
  // way to the allowed size yet.
  //
  size_t current_space_size = mspace_footprint(mspace_);
  if (limit < current_space_size) {
    // Don't let the space grow any more.
    mspace_set_max_allowed_footprint(mspace_, current_space_size);
  } else {
    // Let the heap grow to the requested limit.
    mspace_set_max_allowed_footprint(mspace_, limit);
  }
}

void Space::Grow(size_t new_size) {
  UNIMPLEMENTED(FATAL);
}

}  // namespace art
