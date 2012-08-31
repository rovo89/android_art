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

#include "heap.h"

#include <sys/types.h>
#include <sys/wait.h>

#include <limits>
#include <vector>

#include "atomic.h"
#include "card_table.h"
#include "debugger.h"
#include "heap_bitmap.h"
#include "image.h"
#include "mark_sweep.h"
#include "mod_union_table.h"
#include "object.h"
#include "object_utils.h"
#include "os.h"
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change.h"
#include "space.h"
#include "stl_util.h"
#include "thread_list.h"
#include "timing_logger.h"
#include "UniquePtr.h"
#include "well_known_classes.h"

namespace art {

static void UpdateFirstAndLastSpace(Space** first_space, Space** last_space, Space* space) {
  if (*first_space == NULL) {
    *first_space = space;
    *last_space = space;
  } else {
    if ((*first_space)->Begin() > space->Begin()) {
      *first_space = space;
    } else if (space->Begin() > (*last_space)->Begin()) {
      *last_space = space;
    }
  }
}

static bool GenerateImage(const std::string& image_file_name) {
  const std::string boot_class_path_string(Runtime::Current()->GetBootClassPathString());
  std::vector<std::string> boot_class_path;
  Split(boot_class_path_string, ':', boot_class_path);
  if (boot_class_path.empty()) {
    LOG(FATAL) << "Failed to generate image because no boot class path specified";
  }

  std::vector<char*> arg_vector;

  std::string dex2oat_string(GetAndroidRoot());
  dex2oat_string += (kIsDebugBuild ? "/bin/dex2oatd" : "/bin/dex2oat");
  const char* dex2oat = dex2oat_string.c_str();
  arg_vector.push_back(strdup(dex2oat));

  std::string image_option_string("--image=");
  image_option_string += image_file_name;
  const char* image_option = image_option_string.c_str();
  arg_vector.push_back(strdup(image_option));

  arg_vector.push_back(strdup("--runtime-arg"));
  arg_vector.push_back(strdup("-Xms64m"));

  arg_vector.push_back(strdup("--runtime-arg"));
  arg_vector.push_back(strdup("-Xmx64m"));

  for (size_t i = 0; i < boot_class_path.size(); i++) {
    std::string dex_file_option_string("--dex-file=");
    dex_file_option_string += boot_class_path[i];
    const char* dex_file_option = dex_file_option_string.c_str();
    arg_vector.push_back(strdup(dex_file_option));
  }

  std::string oat_file_option_string("--oat-file=");
  oat_file_option_string += image_file_name;
  oat_file_option_string.erase(oat_file_option_string.size() - 3);
  oat_file_option_string += "oat";
  const char* oat_file_option = oat_file_option_string.c_str();
  arg_vector.push_back(strdup(oat_file_option));

  arg_vector.push_back(strdup("--base=0x60000000"));

  std::string command_line(Join(arg_vector, ' '));
  LOG(INFO) << command_line;

  arg_vector.push_back(NULL);
  char** argv = &arg_vector[0];

  // fork and exec dex2oat
  pid_t pid = fork();
  if (pid == 0) {
    // no allocation allowed between fork and exec

    // change process groups, so we don't get reaped by ProcessManager
    setpgid(0, 0);

    execv(dex2oat, argv);

    PLOG(FATAL) << "execv(" << dex2oat << ") failed";
    return false;
  } else {
    STLDeleteElements(&arg_vector);

    // wait for dex2oat to finish
    int status;
    pid_t got_pid = TEMP_FAILURE_RETRY(waitpid(pid, &status, 0));
    if (got_pid != pid) {
      PLOG(ERROR) << "waitpid failed: wanted " << pid << ", got " << got_pid;
      return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      LOG(ERROR) << dex2oat << " failed: " << command_line;
      return false;
    }
  }
  return true;
}

Heap::Heap(size_t initial_size, size_t growth_limit, size_t capacity,
           const std::string& original_image_file_name, bool concurrent_gc)
    : alloc_space_(NULL),
      card_table_(NULL),
      concurrent_gc_(concurrent_gc),
      have_zygote_space_(false),
      card_marking_disabled_(false),
      is_gc_running_(false),
      concurrent_start_bytes_(std::numeric_limits<size_t>::max()),
      concurrent_start_size_(128 * KB),
      concurrent_min_free_(256 * KB),
      sticky_gc_count_(0),
      num_bytes_allocated_(0),
      num_objects_allocated_(0),
      last_trim_time_(0),
      try_running_gc_(false),
      requesting_gc_(false),
      reference_referent_offset_(0),
      reference_queue_offset_(0),
      reference_queueNext_offset_(0),
      reference_pendingNext_offset_(0),
      finalizer_reference_zombie_offset_(0),
      target_utilization_(0.5),
      verify_objects_(false) {
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "Heap() entering";
  }

  // Compute the bounds of all spaces for allocating live and mark bitmaps
  // there will be at least one space (the alloc space)
  Space* first_space = NULL;
  Space* last_space = NULL;

  live_bitmap_.reset(new HeapBitmap(this));
  mark_bitmap_.reset(new HeapBitmap(this));

  // Requested begin for the alloc space, to follow the mapped image and oat files
  byte* requested_begin = NULL;
  std::string image_file_name(original_image_file_name);
  if (!image_file_name.empty()) {
    Space* image_space = NULL;

    if (OS::FileExists(image_file_name.c_str())) {
      // If the /system file exists, it should be up-to-date, don't try to generate
      image_space = Space::CreateImageSpace(image_file_name);
    } else {
      // If the /system file didn't exist, we need to use one from the art-cache.
      // If the cache file exists, try to open, but if it fails, regenerate.
      // If it does not exist, generate.
      image_file_name = GetArtCacheFilenameOrDie(image_file_name);
      if (OS::FileExists(image_file_name.c_str())) {
        image_space = Space::CreateImageSpace(image_file_name);
      }
      if (image_space == NULL) {
        if (!GenerateImage(image_file_name)) {
          LOG(FATAL) << "Failed to generate image: " << image_file_name;
        }
        image_space = Space::CreateImageSpace(image_file_name);
      }
    }
    if (image_space == NULL) {
      LOG(FATAL) << "Failed to create space from " << image_file_name;
    }

    AddSpace(image_space);
    UpdateFirstAndLastSpace(&first_space, &last_space, image_space);
    // Oat files referenced by image files immediately follow them in memory, ensure alloc space
    // isn't going to get in the middle
    byte* oat_end_addr = GetImageSpace()->GetImageHeader().GetOatEnd();
    CHECK(oat_end_addr > GetImageSpace()->End());
    if (oat_end_addr > requested_begin) {
      requested_begin = reinterpret_cast<byte*>(RoundUp(reinterpret_cast<uintptr_t>(oat_end_addr),
                                                        kPageSize));
    }
  }

  UniquePtr<AllocSpace> alloc_space(Space::CreateAllocSpace(
      "alloc space", initial_size, growth_limit, capacity, requested_begin));
  alloc_space_ = alloc_space.release();
  CHECK(alloc_space_ != NULL) << "Failed to create alloc space";
  AddSpace(alloc_space_);

  UpdateFirstAndLastSpace(&first_space, &last_space, alloc_space_);
  byte* heap_begin = first_space->Begin();
  size_t heap_capacity = (last_space->Begin() - first_space->Begin()) + last_space->NonGrowthLimitCapacity();

  // Mark image objects in the live bitmap
  for (size_t i = 0; i < spaces_.size(); ++i) {
    Space* space = spaces_[i];
    if (space->IsImageSpace()) {
      space->AsImageSpace()->RecordImageAllocations(space->GetLiveBitmap());
    }
  }

  // Allocate the card table.
  card_table_.reset(CardTable::Create(heap_begin, heap_capacity));
  CHECK(card_table_.get() != NULL) << "Failed to create card table";

  mod_union_table_.reset(new ModUnionTableToZygoteAllocspace<ModUnionTableReferenceCache>(this));
  CHECK(mod_union_table_.get() != NULL) << "Failed to create mod-union table";

  zygote_mod_union_table_.reset(new ModUnionTableCardCache(this));
  CHECK(zygote_mod_union_table_.get() != NULL) << "Failed to create Zygote mod-union table";

  // TODO: Count objects in the image space here.
  num_bytes_allocated_ = 0;
  num_objects_allocated_ = 0;

  // Max stack size in bytes.
  static const size_t max_stack_size = capacity / SpaceBitmap::kAlignment * kWordSize;

  // TODO: Rename MarkStack to a more generic name?
  mark_stack_.reset(MarkStack::Create("dalvik-mark-stack", max_stack_size));
  allocation_stack_.reset(MarkStack::Create("dalvik-allocation-stack", max_stack_size));
  live_stack_.reset(MarkStack::Create("dalvik-live-stack", max_stack_size));

  // It's still too early to take a lock because there are no threads yet,
  // but we can create the heap lock now. We don't create it earlier to
  // make it clear that you can't use locks during heap initialization.
  gc_complete_lock_ =  new Mutex("GC complete lock");
  gc_complete_cond_.reset(new ConditionVariable("GC complete condition variable"));

  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "Heap() exiting";
  }
}

// Sort spaces based on begin address
class SpaceSorter {
 public:
  bool operator () (const Space* a, const Space* b) const {
    return a->Begin() < b->Begin();
  }
};

void Heap::AddSpace(Space* space) {
  WriterMutexLock mu(*GlobalSynchronization::heap_bitmap_lock_);
  DCHECK(space != NULL);
  DCHECK(space->GetLiveBitmap() != NULL);
  live_bitmap_->AddSpaceBitmap(space->GetLiveBitmap());
  DCHECK(space->GetMarkBitmap() != NULL);
  mark_bitmap_->AddSpaceBitmap(space->GetMarkBitmap());
  spaces_.push_back(space);
  if (space->IsAllocSpace()) {
    alloc_space_ = space->AsAllocSpace();
  }

  // Ensure that spaces remain sorted in increasing order of start address (required for CMS finger)
  std::sort(spaces_.begin(), spaces_.end(), SpaceSorter());

  // Ensure that ImageSpaces < ZygoteSpaces < AllocSpaces so that we can do address based checks to
  // avoid redundant marking.
  bool seen_zygote = false, seen_alloc = false;
  for (Spaces::const_iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
    Space* space = *it;
    if (space->IsImageSpace()) {
      DCHECK(!seen_zygote);
      DCHECK(!seen_alloc);
    } if (space->IsZygoteSpace()) {
      DCHECK(!seen_alloc);
      seen_zygote = true;
    } else if (space->IsAllocSpace()) {
      seen_alloc = true;
    }
  }
}

Heap::~Heap() {
  // If we don't reset then the mark stack complains in it's destructor.
  allocation_stack_->Reset();
  live_stack_->Reset();

  VLOG(heap) << "~Heap()";
  // We can't take the heap lock here because there might be a daemon thread suspended with the
  // heap lock held. We know though that no non-daemon threads are executing, and we know that
  // all daemon threads are suspended, and we also know that the threads list have been deleted, so
  // those threads can't resume. We're the only running thread, and we can do whatever we like...
  STLDeleteElements(&spaces_);
  delete gc_complete_lock_;

}

Space* Heap::FindSpaceFromObject(const Object* obj) const {
  // TODO: C++0x auto
  for (Spaces::const_iterator cur = spaces_.begin(); cur != spaces_.end(); ++cur) {
    if ((*cur)->Contains(obj)) {
      return *cur;
    }
  }
  LOG(FATAL) << "object " << reinterpret_cast<const void*>(obj) << " not inside any spaces!";
  return NULL;
}

ImageSpace* Heap::GetImageSpace() {
  // TODO: C++0x auto
  for (Spaces::const_iterator cur = spaces_.begin(); cur != spaces_.end(); ++cur) {
    if ((*cur)->IsImageSpace()) {
      return (*cur)->AsImageSpace();
    }
  }
  return NULL;
}

AllocSpace* Heap::GetAllocSpace() {
  return alloc_space_;
}

static void MSpaceChunkCallback(void* start, void* end, size_t used_bytes, void* arg) {
  size_t& max_contiguous_allocation = *reinterpret_cast<size_t*>(arg);

  size_t chunk_size = static_cast<size_t>(reinterpret_cast<uint8_t*>(end) - reinterpret_cast<uint8_t*>(start));
  size_t chunk_free_bytes = 0;
  if (used_bytes < chunk_size) {
    chunk_free_bytes = chunk_size - used_bytes;
  }

  if (chunk_free_bytes > max_contiguous_allocation) {
    max_contiguous_allocation = chunk_free_bytes;
  }
}

Object* Heap::AllocObject(Class* c, size_t byte_count) {
  DCHECK(c == NULL || (c->IsClassClass() && byte_count >= sizeof(Class)) ||
         (c->IsVariableSize() || c->GetObjectSize() == byte_count) ||
         strlen(ClassHelper(c).GetDescriptor()) == 0);
  DCHECK_GE(byte_count, sizeof(Object));
  Object* obj = Allocate(alloc_space_, byte_count);
  if (LIKELY(obj != NULL)) {
#if VERIFY_OBJECT_ENABLED
    WriterMutexLock mu(*GlobalSynchronization::heap_bitmap_lock_);
    // Verify objects doesn't like objects in allocation stack not being marked as live.
    live_bitmap_->Set(obj);
#endif

    obj->SetClass(c);

    // Record allocation after since we want to use the atomic add for the atomic fence to guard
    // the SetClass since we do not want the class to appear NULL in another thread.
    RecordAllocation(alloc_space_, obj);

    if (Dbg::IsAllocTrackingEnabled()) {
      Dbg::RecordAllocation(c, byte_count);
    }
    const bool request_concurrent_gc = num_bytes_allocated_ >= concurrent_start_bytes_;
    if (request_concurrent_gc) {
      // The SirtRef is necessary since the calls in RequestConcurrentGC are a safepoint.
      SirtRef<Object> ref(obj);
      RequestConcurrentGC();
    }
    VerifyObject(obj);

    // Additional verification to ensure that we did not allocate into a zygote space.
    DCHECK(!have_zygote_space_ || !FindSpaceFromObject(obj)->IsZygoteSpace());

    return obj;
  }
  int64_t total_bytes_free = GetFreeMemory();
  size_t max_contiguous_allocation = 0;
  // TODO: C++0x auto
  for (Spaces::const_iterator cur = spaces_.begin(); cur != spaces_.end(); ++cur) {
    if ((*cur)->IsAllocSpace()) {
      (*cur)->AsAllocSpace()->Walk(MSpaceChunkCallback, &max_contiguous_allocation);
    }
  }

  std::string msg(StringPrintf("Failed to allocate a %zd-byte %s (%lld total bytes free; largest possible contiguous allocation %zd bytes)",
                               byte_count,
                               PrettyDescriptor(c).c_str(),
                               total_bytes_free, max_contiguous_allocation));
  Thread::Current()->ThrowOutOfMemoryError(msg.c_str());
  return NULL;
}

bool Heap::IsHeapAddress(const Object* obj) {
  // Note: we deliberately don't take the lock here, and mustn't test anything that would
  // require taking the lock.
  if (obj == NULL) {
    return true;
  }
  if (!IsAligned<kObjectAlignment>(obj)) {
    return false;
  }
  for (size_t i = 0; i < spaces_.size(); ++i) {
    if (spaces_[i]->Contains(obj)) {
      return true;
    }
  }
  return false;
}

bool Heap::IsLiveObjectLocked(const Object* obj) {
  GlobalSynchronization::heap_bitmap_lock_->AssertReaderHeld();
  return IsHeapAddress(obj) && GetLiveBitmap()->Test(obj);
}

#if VERIFY_OBJECT_ENABLED
void Heap::VerifyObject(const Object* obj) {
  if (obj == NULL || this == NULL || !verify_objects_ || Runtime::Current()->IsShuttingDown() ||
      Thread::Current() == NULL ||
      Runtime::Current()->GetThreadList()->GetLockOwner() == Thread::Current()->GetTid()) {
    return;
  }
  VerifyObjectBody(obj);
}
#endif

void Heap::DumpSpaces() {
  // TODO: C++0x auto
  for (Spaces::iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
    Space* space = *it;
    LOG(INFO) << *space;
    LOG(INFO) << *space->GetLiveBitmap();
    LOG(INFO) << *space->GetMarkBitmap();
  }
}

// We want to avoid bit rotting.
void Heap::VerifyObjectBody(const Object* obj) {
  if (!IsAligned<kObjectAlignment>(obj)) {
    LOG(FATAL) << "Object isn't aligned: " << obj;
  } else if (!GetLiveBitmap()->Test(obj)) {
    DumpSpaces();
    LOG(FATAL) << "Object is dead: " << obj;
  }

  // Ignore early dawn of the universe verifications
  if (!VERIFY_OBJECT_FAST && num_objects_allocated_ > 10) {
    const byte* raw_addr = reinterpret_cast<const byte*>(obj) +
        Object::ClassOffset().Int32Value();
    const Class* c = *reinterpret_cast<Class* const *>(raw_addr);
    if (c == NULL) {
      LOG(FATAL) << "Null class in object: " << obj;
    } else if (!IsAligned<kObjectAlignment>(c)) {
      LOG(FATAL) << "Class isn't aligned: " << c << " in object: " << obj;
    } else if (!GetLiveBitmap()->Test(c)) {
      LOG(FATAL) << "Class of object is dead: " << c << " in object: " << obj;
    }
    // Check obj.getClass().getClass() == obj.getClass().getClass().getClass()
    // Note: we don't use the accessors here as they have internal sanity checks
    // that we don't want to run
    raw_addr = reinterpret_cast<const byte*>(c) + Object::ClassOffset().Int32Value();
    const Class* c_c = *reinterpret_cast<Class* const *>(raw_addr);
    raw_addr = reinterpret_cast<const byte*>(c_c) + Object::ClassOffset().Int32Value();
    const Class* c_c_c = *reinterpret_cast<Class* const *>(raw_addr);
    CHECK_EQ(c_c, c_c_c);
  }
}

void Heap::VerificationCallback(Object* obj, void* arg) {
  DCHECK(obj != NULL);
  reinterpret_cast<Heap*>(arg)->VerifyObjectBody(obj);
}

void Heap::VerifyHeap() {
  ReaderMutexLock mu(*GlobalSynchronization::heap_bitmap_lock_);
  GetLiveBitmap()->Walk(Heap::VerificationCallback, this);
}

void Heap::RecordAllocation(AllocSpace* space, const Object* obj) {
  {
    size_t size = space->AllocationSize(obj);
    DCHECK_GT(size, 0u);
    COMPILE_ASSERT(sizeof(size_t) == sizeof(int32_t),
                   int32_t_must_be_same_size_as_size_t_for_used_atomic_operations);
    android_atomic_add(size, reinterpret_cast<volatile int32_t*>(
        reinterpret_cast<size_t>(&num_bytes_allocated_)));
    android_atomic_add(1, reinterpret_cast<volatile int32_t*>(
        reinterpret_cast<size_t>(&num_objects_allocated_)));

    if (Runtime::Current()->HasStatsEnabled()) {
      RuntimeStats* global_stats = Runtime::Current()->GetStats();
      RuntimeStats* thread_stats = Thread::Current()->GetStats();
      ++global_stats->allocated_objects;
      ++thread_stats->allocated_objects;
      global_stats->allocated_bytes += size;
      thread_stats->allocated_bytes += size;
    }
  }

  DCHECK(obj);

  allocation_stack_->AtomicPush(obj);
}

void Heap::RecordFree(size_t freed_objects, size_t freed_bytes) {
  COMPILE_ASSERT(sizeof(size_t) == sizeof(int32_t),
                 int32_t_must_be_same_size_as_size_t_for_used_atomic_operations);
  DCHECK_LE(freed_objects, num_objects_allocated_);
  android_atomic_add(-static_cast<int32_t>(freed_objects),
                        reinterpret_cast<volatile int32_t*>(
                            reinterpret_cast<size_t>(&num_objects_allocated_)));

  DCHECK_LE(freed_bytes, num_bytes_allocated_);
  android_atomic_add(-static_cast<int32_t>(freed_bytes),
                        reinterpret_cast<volatile int32_t*>(
                            reinterpret_cast<size_t>(&num_bytes_allocated_)));

  if (Runtime::Current()->HasStatsEnabled()) {
    RuntimeStats* global_stats = Runtime::Current()->GetStats();
    RuntimeStats* thread_stats = Thread::Current()->GetStats();
    global_stats->freed_objects += freed_objects;
    thread_stats->freed_objects += freed_objects;
    global_stats->freed_bytes += freed_bytes;
    thread_stats->freed_bytes += freed_bytes;
  }
}

Object* Heap::Allocate(AllocSpace* space, size_t alloc_size) {
  Thread* self = Thread::Current();
  // Since allocation can cause a GC which will need to SuspendAll, make sure all allocations are
  // done in the runnable state where suspension is expected.
#ifndef NDEBUG
  {
    MutexLock mu(*GlobalSynchronization::thread_suspend_count_lock_);
    CHECK_EQ(self->GetState(), kRunnable);
  }
  self->AssertThreadSuspensionIsAllowable();
#endif

  Object* ptr = space->AllocWithoutGrowth(alloc_size);
  if (ptr != NULL) {
    return ptr;
  }

  // The allocation failed.  If the GC is running, block until it completes else request a
  // foreground partial collection.
  if (!WaitForConcurrentGcToComplete()) {
    // No concurrent GC so perform a foreground collection.
    if (Runtime::Current()->HasStatsEnabled()) {
      ++Runtime::Current()->GetStats()->gc_for_alloc_count;
      ++Thread::Current()->GetStats()->gc_for_alloc_count;
    }
    self->TransitionFromRunnableToSuspended(kWaitingPerformingGc);
    CollectGarbageInternal(have_zygote_space_ ? GC_PARTIAL : GC_FULL, false);
    self->TransitionFromSuspendedToRunnable();
  }

  ptr = space->AllocWithoutGrowth(alloc_size);
  if (ptr != NULL) {
    return ptr;
  }

  const size_t alloc_space_size = alloc_space_->Size();
  if (alloc_space_size > kMinAllocSpaceSizeForStickyGC &&
      alloc_space_->Capacity() - alloc_space_size < kMinRemainingSpaceForStickyGC) {
    // Partial GC didn't free enough memory, try a full GC.
    if (Runtime::Current()->HasStatsEnabled()) {
      ++Runtime::Current()->GetStats()->gc_for_alloc_count;
      ++Thread::Current()->GetStats()->gc_for_alloc_count;
    }

    // Don't bother trying a young GC unless we have a few MB AllocSpace.
    self->TransitionFromRunnableToSuspended(kWaitingPerformingGc);
    CollectGarbageInternal(GC_STICKY, false);
    self->TransitionFromSuspendedToRunnable();

    ptr = space->AllocWithoutGrowth(alloc_size);
    if (ptr != NULL) {
      return ptr;
    }
  }

  if (!have_zygote_space_) {
    // Partial GC didn't free enough memory, try a full GC.
    if (Runtime::Current()->HasStatsEnabled()) {
      ++Runtime::Current()->GetStats()->gc_for_alloc_count;
      ++Thread::Current()->GetStats()->gc_for_alloc_count;
    }
    self->TransitionFromRunnableToSuspended(kWaitingPerformingGc);
    CollectGarbageInternal(GC_PARTIAL, false);
    self->TransitionFromSuspendedToRunnable();

    ptr = space->AllocWithoutGrowth(alloc_size);
    if (ptr != NULL) {
      return ptr;
    }
  }

  // Allocations have failed after GCs;  this is an exceptional state.
  // Try harder, growing the heap if necessary.
  ptr = space->AllocWithGrowth(alloc_size);
  if (ptr != NULL) {
    size_t new_footprint = space->GetFootprintLimit();
    // OLD-TODO: may want to grow a little bit more so that the amount of
    //       free space is equal to the old free space + the
    //       utilization slop for the new allocation.
    VLOG(gc) << "Grow heap (frag case) to " << PrettySize(new_footprint)
             << " for a " << PrettySize(alloc_size) << " allocation";
    return ptr;
  }

  // Most allocations should have succeeded by now, so the heap is really full, really fragmented,
  // or the requested size is really big. Do another GC, collecting SoftReferences this time. The
  // VM spec requires that all SoftReferences have been collected and cleared before throwing OOME.

  // OLD-TODO: wait for the finalizers from the previous GC to finish
  VLOG(gc) << "Forcing collection of SoftReferences for " << PrettySize(alloc_size) << " allocation";

  if (Runtime::Current()->HasStatsEnabled()) {
    ++Runtime::Current()->GetStats()->gc_for_alloc_count;
    ++Thread::Current()->GetStats()->gc_for_alloc_count;
  }
  // We don't need a WaitForConcurrentGcToComplete here either.
  self->TransitionFromRunnableToSuspended(kWaitingPerformingGc);
  CollectGarbageInternal(GC_FULL, true);
  self->TransitionFromSuspendedToRunnable();
  return space->AllocWithGrowth(alloc_size);
}

int64_t Heap::GetMaxMemory() {
  size_t total = 0;
  // TODO: C++0x auto
  for (Spaces::const_iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
    Space* space = *it;
    if (space->IsAllocSpace()) {
      total += space->AsAllocSpace()->Capacity();
    }
  }
  return total;
}

int64_t Heap::GetTotalMemory() {
  return GetMaxMemory();
}

int64_t Heap::GetFreeMemory() {
  return GetMaxMemory() - num_bytes_allocated_;
}

class InstanceCounter {
 public:
  InstanceCounter(Class* c, bool count_assignable)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_)
      : class_(c), count_assignable_(count_assignable), count_(0) {
  }

  size_t GetCount() {
    return count_;
  }

  static void Callback(Object* o, void* arg)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    reinterpret_cast<InstanceCounter*>(arg)->VisitInstance(o);
  }

 private:
  void VisitInstance(Object* o) SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    Class* instance_class = o->GetClass();
    if (count_assignable_) {
      if (instance_class == class_) {
        ++count_;
      }
    } else {
      if (instance_class != NULL && class_->IsAssignableFrom(instance_class)) {
        ++count_;
      }
    }
  }

  Class* class_;
  bool count_assignable_;
  size_t count_;
};

int64_t Heap::CountInstances(Class* c, bool count_assignable) {
  ReaderMutexLock mu(*GlobalSynchronization::heap_bitmap_lock_);
  InstanceCounter counter(c, count_assignable);
  GetLiveBitmap()->Walk(InstanceCounter::Callback, &counter);
  return counter.GetCount();
}

void Heap::CollectGarbage(bool clear_soft_references) {
  // If we just waited for a GC to complete then we do not need to do another
  // GC unless we clear soft references.
  if (!WaitForConcurrentGcToComplete() || clear_soft_references) {
    ScopedThreadStateChange tsc(Thread::Current(), kWaitingPerformingGc);
    CollectGarbageInternal(have_zygote_space_ ? GC_PARTIAL : GC_FULL, clear_soft_references);
  }
}

void Heap::PreZygoteFork() {
  static Mutex zygote_creation_lock_("zygote creation lock", kZygoteCreationLock);
  MutexLock mu(zygote_creation_lock_);

  // Try to see if we have any Zygote spaces.
  if (have_zygote_space_) {
    return;
  }

  VLOG(heap) << "Starting PreZygoteFork with alloc space size " << PrettySize(alloc_space_->Size());

  {
    // Flush the alloc stack.
    WriterMutexLock mu(*GlobalSynchronization::heap_bitmap_lock_);
    FlushAllocStack();
  }

  // Replace the first alloc space we find with a zygote space.
  // TODO: C++0x auto
  for (Spaces::iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
    if ((*it)->IsAllocSpace()) {
      AllocSpace* zygote_space = (*it)->AsAllocSpace();

      // Turns the current alloc space into a Zygote space and obtain the new alloc space composed
      // of the remaining available heap memory.
      alloc_space_ = zygote_space->CreateZygoteSpace();

      // Change the GC retention policy of the zygote space to only collect when full.
      zygote_space->SetGcRetentionPolicy(GCRP_FULL_COLLECT);
      AddSpace(alloc_space_);
      have_zygote_space_ = true;
      break;
    }
  }

  // Reset this since we now count the ZygoteSpace in the total heap size.
  num_bytes_allocated_ = 0;
}

void Heap::FlushAllocStack() {
  MarkStackAsLive(allocation_stack_.get());
  allocation_stack_->Reset();
}

size_t Heap::GetUsedMemorySize() const {
  size_t total = num_bytes_allocated_;
  for (Spaces::const_iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
    if ((*it)->IsZygoteSpace()) {
      total += (*it)->AsAllocSpace()->Size();
    }
  }
  return total;
}

void Heap::MarkStackAsLive(MarkStack* alloc_stack) {
  // We can just assume everything is inside the alloc_space_'s bitmap since we should only have
  // fresh allocations.
  SpaceBitmap* live_bitmap = alloc_space_->GetLiveBitmap();

  // Empty the allocation stack.
  const size_t count = alloc_stack->Size();
  for (size_t i = 0; i < count; ++i) {
    const Object* obj = alloc_stack->Get(i);
    DCHECK(obj != NULL);
    live_bitmap->Set(obj);
  }
}

void Heap::UnMarkStack(MarkStack* alloc_stack) {
  SpaceBitmap* mark_bitmap = alloc_space_->GetMarkBitmap();

  // Clear all of the things in the AllocStack.
  size_t count = alloc_stack->Size();
  for (size_t i = 0;i < count;++i) {
    const Object* obj = alloc_stack->Get(i);
    DCHECK(obj != NULL);
    if (mark_bitmap->Test(obj)) {
      mark_bitmap->Clear(obj);
    }
  }
}

void Heap::CollectGarbageInternal(GcType gc_type, bool clear_soft_references) {
  GlobalSynchronization::mutator_lock_->AssertNotHeld();
#ifndef NDEBUG
  {
    MutexLock mu(*GlobalSynchronization::thread_suspend_count_lock_);
    CHECK_EQ(Thread::Current()->GetState(), kWaitingPerformingGc);
  }
#endif

  // Ensure there is only one GC at a time.
  bool start_collect = false;
  while (!start_collect) {
    {
      MutexLock mu(*gc_complete_lock_);
      if (!is_gc_running_) {
        is_gc_running_ = true;
        start_collect = true;
      }
    }
    if (!start_collect) {
      WaitForConcurrentGcToComplete();
      // TODO: if another thread beat this one to do the GC, perhaps we should just return here?
      //       Not doing at the moment to ensure soft references are cleared.
    }
  }
  gc_complete_lock_->AssertNotHeld();

  // We need to do partial GCs every now and then to avoid the heap growing too much and
  // fragmenting.
  if (gc_type == GC_STICKY && ++sticky_gc_count_ > kPartialGCFrequency) {
    gc_type = GC_PARTIAL;
  }
  if (gc_type != GC_STICKY) {
    sticky_gc_count_ = 0;
  }

  if (concurrent_gc_) {
    CollectGarbageConcurrentMarkSweepPlan(gc_type, clear_soft_references);
  } else {
    CollectGarbageMarkSweepPlan(gc_type, clear_soft_references);
  }

  {
    MutexLock mu(*gc_complete_lock_);
    is_gc_running_ = false;
    // Wake anyone who may have been waiting for the GC to complete.
    gc_complete_cond_->Broadcast();
  }
  // Inform DDMS that a GC completed.
  Dbg::GcDidFinish();
}

void Heap::CollectGarbageMarkSweepPlan(GcType gc_type, bool clear_soft_references) {
  TimingLogger timings("CollectGarbageInternal", true);

  // Suspend all threads are get exclusive access to the heap.
  uint64_t start_time = NanoTime();
  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  thread_list->SuspendAll();
  timings.AddSplit("SuspendAll");
  GlobalSynchronization::mutator_lock_->AssertExclusiveHeld();

  size_t bytes_freed = 0;
  Object* cleared_references = NULL;
  {
    MarkSweep mark_sweep(mark_stack_.get());

    mark_sweep.Init();
    timings.AddSplit("Init");

    // Make sure that the tables have the correct pointer for the mark sweep.
    mod_union_table_->Init(&mark_sweep);
    zygote_mod_union_table_->Init(&mark_sweep);

    // Swap allocation stack and live stack, enabling us to have new allocations during this GC.
    MarkStack* temp = allocation_stack_.release();
    allocation_stack_.reset(live_stack_.release());
    live_stack_.reset(temp);

    // We will need to know which cards were dirty for doing concurrent processing of dirty cards.
    // TODO: Investigate using a mark stack instead of a vector.
    std::vector<byte*> dirty_cards;
    if (gc_type == GC_STICKY) {
      for (Spaces::iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
        card_table_->GetDirtyCards(*it, dirty_cards);
      }
    }

    // Clear image space cards and keep track of cards we cleared in the mod-union table.
    for (Spaces::iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
      Space* space = *it;
      if (space->IsImageSpace()) {
        mod_union_table_->ClearCards(*it);
        timings.AddSplit("ClearModUnionCards");
      } else if (space->GetGcRetentionPolicy() == GCRP_FULL_COLLECT) {
        zygote_mod_union_table_->ClearCards(space);
        timings.AddSplit("ClearZygoteCards");
      } else {
        card_table_->ClearSpaceCards(space);
        timings.AddSplit("ClearCards");
      }
    }

#if VERIFY_MOD_UNION
    mod_union_table_->Verify();
    zygote_mod_union_table_->Verify();
#endif

    WriterMutexLock mu(*GlobalSynchronization::heap_bitmap_lock_);
    if (gc_type == GC_PARTIAL) {
      // Copy the mark bits over from the live bits, do this as early as possible or else we can
      // accidentally un-mark roots.
      // Needed for scanning dirty objects.
      for (Spaces::iterator it = spaces_.begin();it != spaces_.end(); ++it) {
        if ((*it)->GetGcRetentionPolicy() == GCRP_FULL_COLLECT) {
          mark_sweep.CopyMarkBits(*it);
        }
      }
      timings.AddSplit("CopyMarkBits");

      // We can assume that everything < alloc_space_ start is marked at this point.
      mark_sweep.SetCondemned(reinterpret_cast<Object*>(alloc_space_->Begin()));
    } else if (gc_type == GC_STICKY) {
      for (Spaces::iterator it = spaces_.begin();it != spaces_.end(); ++it) {
        if ((*it)->GetGcRetentionPolicy() != GCRP_NEVER_COLLECT) {
          mark_sweep.CopyMarkBits(*it);
        }
      }
      timings.AddSplit("CopyMarkBits");

      if (VERIFY_OBJECT_ENABLED) {
        UnMarkStack(live_stack_.get());
      }

      mark_sweep.SetCondemned(reinterpret_cast<Object*>(alloc_space_->Begin()));
    }

    MarkStackAsLive(live_stack_.get());

    mark_sweep.MarkRoots();
    timings.AddSplit("MarkRoots");

    // Roots are marked on the bitmap and the mark_stack is empty.
    DCHECK(mark_sweep.IsMarkStackEmpty());

    // Update zygote mod union table.
    zygote_mod_union_table_->Update();
    timings.AddSplit("UpdateZygoteModUnionTable");

    zygote_mod_union_table_->MarkReferences();
    timings.AddSplit("ZygoteMarkReferences");

    // Processes the cards we cleared earlier and adds their objects into the mod-union table.
    mod_union_table_->Update();
    timings.AddSplit("UpdateModUnionTable");

    // Scans all objects in the mod-union table.
    mod_union_table_->MarkReferences();
    timings.AddSplit("MarkImageToAllocSpaceReferences");

    // Recursively mark all the non-image bits set in the mark bitmap.
    if (gc_type != GC_STICKY) {
      live_stack_->Reset();
      mark_sweep.RecursiveMark(gc_type == GC_PARTIAL, timings);
    } else {
      mark_sweep.RecursiveMarkCards(card_table_.get(), dirty_cards, timings);
    }

    // Need to process references the swap since it uses IsMarked.
    mark_sweep.ProcessReferences(clear_soft_references);
    timings.AddSplit("ProcessReferences");

    // This doesn't work with mutators unpaused for some reason, TODO: Fix.
    mark_sweep.SweepSystemWeaks(false);
    timings.AddSplit("SweepSystemWeaks");

    // Need to swap for VERIFY_OBJECT_ENABLED since we put things in the live bitmap after they
    // have been allocated.
    const bool swap = true;

    if (swap) {
      // Swap the live and mark bitmaps for each alloc space. This is needed since sweep re-swaps
      // these bitmaps. Doing this enables us to sweep with the heap unlocked since new allocations
      // set the live bit, but since we have the bitmaps reversed at this point, this sets the mark bit
      // instead, resulting in no new allocated objects being incorrectly freed by sweep.
      for (Spaces::iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
        Space* space = *it;
        // We only allocate into AllocSpace, so we only need to swap AllocSpaces.
        if (space->GetGcRetentionPolicy() == GCRP_ALWAYS_COLLECT) {
          live_bitmap_->ReplaceBitmap(space->GetLiveBitmap(), space->GetMarkBitmap());
          mark_bitmap_->ReplaceBitmap(space->GetMarkBitmap(), space->GetLiveBitmap());
          space->AsAllocSpace()->SwapBitmaps();
        }
      }
    }

#ifndef NDEBUG
    // Verify that we only reach marked objects from the image space
    mark_sweep.VerifyImageRoots();
    timings.AddSplit("VerifyImageRoots");
#endif

    if (gc_type != GC_STICKY) {
      mark_sweep.Sweep(gc_type == GC_PARTIAL, swap);
    } else {
      mark_sweep.SweepArray(timings, live_stack_.get(), swap);
    }
    timings.AddSplit("Sweep");

    cleared_references = mark_sweep.GetClearedReferences();
    bytes_freed = mark_sweep.GetFreedBytes();
  }

  GrowForUtilization();
  timings.AddSplit("GrowForUtilization");

  thread_list->ResumeAll();
  timings.AddSplit("ResumeAll");

  EnqueueClearedReferences(&cleared_references);
  RequestHeapTrim();
  timings.AddSplit("Finish");

  // If the GC was slow, then print timings in the log.
  uint64_t duration = (NanoTime() - start_time) / 1000 * 1000;
  if (duration > MsToNs(50)) {
    const size_t percent_free = GetPercentFree();
    const size_t current_heap_size = GetUsedMemorySize();
    const size_t total_memory = GetTotalMemory();
    LOG(INFO) << (gc_type == GC_PARTIAL ? "Partial " : (gc_type == GC_STICKY ? "Sticky " : ""))
              << "GC freed " << PrettySize(bytes_freed) << ", " << percent_free << "% free, "
              << PrettySize(current_heap_size) << "/" << PrettySize(total_memory) << ", "
              << "paused " << PrettyDuration(duration);
  }

  if (VLOG_IS_ON(heap)) {
    timings.Dump();
  }
}

void Heap::CollectGarbageConcurrentMarkSweepPlan(GcType gc_type, bool clear_soft_references) {
  TimingLogger timings("ConcurrentCollectGarbageInternal", true);
  uint64_t root_begin = NanoTime(), root_end = 0, dirty_begin = 0, dirty_end = 0;

  // Suspend all threads are get exclusive access to the heap.
  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  thread_list->SuspendAll();
  timings.AddSplit("SuspendAll");
  GlobalSynchronization::mutator_lock_->AssertExclusiveHeld();

  size_t bytes_freed = 0;
  Object* cleared_references = NULL;
  {
    MarkSweep mark_sweep(mark_stack_.get());
    timings.AddSplit("ctor");

    mark_sweep.Init();
    timings.AddSplit("Init");

    // Swap the stacks, this is safe sunce all the mutators are suspended at this point.
    MarkStack* temp = allocation_stack_.release();
    allocation_stack_.reset(live_stack_.release());
    live_stack_.reset(temp);

    // We will need to know which cards were dirty for doing concurrent processing of dirty cards.
    // TODO: Investigate using a mark stack instead of a vector.
    std::vector<byte*> dirty_cards;
    if (gc_type == GC_STICKY) {
      for (Spaces::iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
        card_table_->GetDirtyCards(*it, dirty_cards);
      }
    }

    // Make sure that the tables have the correct pointer for the mark sweep.
    mod_union_table_->Init(&mark_sweep);
    zygote_mod_union_table_->Init(&mark_sweep);

    // Clear image space cards and keep track of cards we cleared in the mod-union table.
    for (Spaces::iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
      Space* space = *it;
      if (space->IsImageSpace()) {
        mod_union_table_->ClearCards(*it);
        timings.AddSplit("ModUnionClearCards");
      } else if (space->GetGcRetentionPolicy() == GCRP_FULL_COLLECT) {
        zygote_mod_union_table_->ClearCards(space);
        timings.AddSplit("ZygoteModUnionClearCards");
      } else {
        card_table_->ClearSpaceCards(space);
        timings.AddSplit("ClearCards");
      }
    }

#if VERIFY_MOD_UNION
    mod_union_table_->Verify();
    zygote_mod_union_table_->Verify();
#endif


    {
      WriterMutexLock mu(*GlobalSynchronization::heap_bitmap_lock_);

      if (gc_type == GC_PARTIAL) {
        // Copy the mark bits over from the live bits, do this as early as possible or else we can
        // accidentally un-mark roots.
        // Needed for scanning dirty objects.
        for (Spaces::iterator it = spaces_.begin();it != spaces_.end(); ++it) {
          if ((*it)->GetGcRetentionPolicy() == GCRP_FULL_COLLECT) {
            mark_sweep.CopyMarkBits(*it);
          }
        }
        timings.AddSplit("CopyMarkBits");
        mark_sweep.SetCondemned(reinterpret_cast<Object*>(alloc_space_->Begin()));
      } else if (gc_type == GC_STICKY) {
        for (Spaces::iterator it = spaces_.begin();it != spaces_.end(); ++it) {
          if ((*it)->GetGcRetentionPolicy() != GCRP_NEVER_COLLECT) {
            mark_sweep.CopyMarkBits(*it);
          }
        }
        timings.AddSplit("CopyMarkBits");
        // We need to unmark the new objects since we marked them as live earlier to avoid verify
        // objects failing.
        if (VERIFY_OBJECT_ENABLED) {
          UnMarkStack(live_stack_.get());
        }
        mark_sweep.SetCondemned(reinterpret_cast<Object*>(alloc_space_->Begin()));
      }

      // TODO: Investigate whether or not this is really necessary for sticky mark bits.
      MarkStackAsLive(live_stack_.get());

      if (gc_type != GC_STICKY) {
        live_stack_->Reset();
        mark_sweep.MarkRoots();
        timings.AddSplit("MarkRoots");
      }
    }

    // Roots are marked on the bitmap and the mark_stack is empty.
    DCHECK(mark_sweep.IsMarkStackEmpty());

    // Allow mutators to go again, acquire share on mutator_lock_ to continue.
    thread_list->ResumeAll();
    {
      ReaderMutexLock reader_lock(*GlobalSynchronization::mutator_lock_);
      root_end = NanoTime();
      timings.AddSplit("RootEnd");

      WriterMutexLock mu(*GlobalSynchronization::heap_bitmap_lock_);
      if (gc_type != GC_STICKY) {
        // Update zygote mod union table.
        if (gc_type == GC_PARTIAL) {
          zygote_mod_union_table_->Update();
          timings.AddSplit("UpdateZygoteModUnionTable");

          zygote_mod_union_table_->MarkReferences();
          timings.AddSplit("ZygoteMarkReferences");
        }

        // Processes the cards we cleared earlier and adds their objects into the mod-union table.
        mod_union_table_->Update();
        timings.AddSplit("UpdateModUnionTable");

        // Scans all objects in the mod-union table.
        mod_union_table_->MarkReferences();
        timings.AddSplit("MarkImageToAllocSpaceReferences");

        // Recursively mark all the non-image bits set in the mark bitmap.
        mark_sweep.RecursiveMark(gc_type == GC_PARTIAL, timings);
      } else {
        mark_sweep.RecursiveMarkCards(card_table_.get(), dirty_cards, timings);
        mark_sweep.DisableFinger();
      }
    }
    // Release share on mutator_lock_ and then get exclusive access.
    dirty_begin = NanoTime();
    thread_list->SuspendAll();
    timings.AddSplit("ReSuspend");
    GlobalSynchronization::mutator_lock_->AssertExclusiveHeld();

    {
      WriterMutexLock mu(*GlobalSynchronization::heap_bitmap_lock_);

      // Re-mark root set.
      mark_sweep.ReMarkRoots();
      timings.AddSplit("ReMarkRoots");

      // Scan dirty objects, this is only required if we are not doing concurrent GC.
      mark_sweep.RecursiveMarkDirtyObjects(false);
      timings.AddSplit("RecursiveMarkDirtyObjects");
    }
    {
      ReaderMutexLock mu(*GlobalSynchronization::heap_bitmap_lock_);
      mark_sweep.ProcessReferences(clear_soft_references);
      timings.AddSplit("ProcessReferences");

      // This doesn't work with mutators unpaused for some reason, TODO: Fix.
      mark_sweep.SweepSystemWeaks(false);
      timings.AddSplit("SweepSystemWeaks");
    }
    // Swap the live and mark bitmaps for each alloc space. This is needed since sweep re-swaps
    // these bitmaps. Doing this enables us to sweep with the heap unlocked since new allocations
    // set the live bit, but since we have the bitmaps reversed at this point, this sets the mark
    // bit instead, resulting in no new allocated objects being incorrectly freed by sweep.
    bool swap = true;
    if (swap) {
      WriterMutexLock mu(*GlobalSynchronization::heap_bitmap_lock_);
      for (Spaces::iterator it = spaces_.begin(); it != spaces_.end(); ++it) {
        Space* space = *it;
        // We never allocate into zygote spaces.
        if (space->GetGcRetentionPolicy() == GCRP_ALWAYS_COLLECT) {
          live_bitmap_->ReplaceBitmap(space->GetLiveBitmap(), space->GetMarkBitmap());
          mark_bitmap_->ReplaceBitmap(space->GetMarkBitmap(), space->GetLiveBitmap());
          space->AsAllocSpace()->SwapBitmaps();
        }
      }
    }

    if (kIsDebugBuild) {
      // Verify that we only reach marked objects from the image space.
      ReaderMutexLock mu(*GlobalSynchronization::heap_bitmap_lock_);
      mark_sweep.VerifyImageRoots();
      timings.AddSplit("VerifyImageRoots");
    }

    thread_list->ResumeAll();
    dirty_end = NanoTime();
    GlobalSynchronization::mutator_lock_->AssertNotHeld();

    {
      // TODO: this lock shouldn't be necessary (it's why we did the bitmap flip above).
      WriterMutexLock mu(*GlobalSynchronization::heap_bitmap_lock_);
      if (gc_type != GC_STICKY) {
        mark_sweep.Sweep(gc_type == GC_PARTIAL, swap);
      } else {
        mark_sweep.SweepArray(timings, live_stack_.get(), swap);
      }
      timings.AddSplit("Sweep");
    }

    cleared_references = mark_sweep.GetClearedReferences();
    bytes_freed = mark_sweep.GetFreedBytes();
  }

  GrowForUtilization();
  timings.AddSplit("GrowForUtilization");

  EnqueueClearedReferences(&cleared_references);
  RequestHeapTrim();
  timings.AddSplit("Finish");

  // If the GC was slow, then print timings in the log.
  uint64_t pause_roots = (root_end - root_begin) / 1000 * 1000;
  uint64_t pause_dirty = (dirty_end - dirty_begin) / 1000 * 1000;
  uint64_t duration = (NanoTime() - root_begin) / 1000 * 1000;
  if (pause_roots > MsToNs(5) || pause_dirty > MsToNs(5)) {
    const size_t percent_free = GetPercentFree();
    const size_t current_heap_size = GetUsedMemorySize();
    const size_t total_memory = GetTotalMemory();
    LOG(INFO) << (gc_type == GC_PARTIAL ? "Partial " : (gc_type == GC_STICKY ? "Sticky " : ""))
              << "Concurrent GC freed " << PrettySize(bytes_freed) << ", " << percent_free
              << "% free, " << PrettySize(current_heap_size) << "/"
              << PrettySize(total_memory) << ", " << "paused " << PrettyDuration(pause_roots)
              << "+" << PrettyDuration(pause_dirty) << " total " << PrettyDuration(duration);
  }

  if (VLOG_IS_ON(heap)) {
    timings.Dump();
  }
}

bool Heap::WaitForConcurrentGcToComplete() {
  if (concurrent_gc_) {
    bool do_wait = false;
    uint64_t wait_start;
    {
      // Check if GC is running holding gc_complete_lock_.
      MutexLock mu(*gc_complete_lock_);
      if (is_gc_running_) {
        wait_start = NanoTime();
        do_wait = true;
      }
    }
    if (do_wait) {
      // We must wait, change thread state then sleep on gc_complete_cond_;
      ScopedThreadStateChange tsc(Thread::Current(), kWaitingForGcToComplete);
      {
        MutexLock mu(*gc_complete_lock_);
        while (is_gc_running_) {
          gc_complete_cond_->Wait(*gc_complete_lock_);
        }
      }
      uint64_t wait_time = NanoTime() - wait_start;
      if (wait_time > MsToNs(5)) {
        LOG(INFO) << "WaitForConcurrentGcToComplete blocked for " << PrettyDuration(wait_time);
      }
      return true;
    }
  }
  return false;
}

void Heap::DumpForSigQuit(std::ostream& os) {
  os << "Heap: " << GetPercentFree() << "% free, "
     << PrettySize(num_bytes_allocated_) << "/" << PrettySize(GetTotalMemory())
     << "; " << num_objects_allocated_ << " objects\n";
}

size_t Heap::GetPercentFree() {
  size_t total = GetTotalMemory();
  return 100 - static_cast<size_t>(100.0f * static_cast<float>(num_bytes_allocated_) / total);
}

void Heap::SetIdealFootprint(size_t max_allowed_footprint) {
  AllocSpace* alloc_space = alloc_space_;
  // TODO: Behavior for multiple alloc spaces?
  size_t alloc_space_capacity = alloc_space->Capacity();
  if (max_allowed_footprint > alloc_space_capacity) {
    VLOG(gc) << "Clamp target GC heap from " << PrettySize(max_allowed_footprint)
             << " to " << PrettySize(alloc_space_capacity);
    max_allowed_footprint = alloc_space_capacity;
  }
  alloc_space->SetFootprintLimit(max_allowed_footprint);
}

// kHeapIdealFree is the ideal maximum free size, when we grow the heap for utilization.
static const size_t kHeapIdealFree = 2 * MB;
// kHeapMinFree guarantees that you always have at least 512 KB free, when you grow for utilization,
// regardless of target utilization ratio.
static const size_t kHeapMinFree = kHeapIdealFree / 4;

void Heap::GrowForUtilization() {
  size_t target_size;
  bool use_footprint_limit = false;
  {
    // We know what our utilization is at this moment.
    // This doesn't actually resize any memory. It just lets the heap grow more when necessary.
    target_size = num_bytes_allocated_ / Heap::GetTargetHeapUtilization();

    if (target_size > num_bytes_allocated_ + kHeapIdealFree) {
      target_size = num_bytes_allocated_ + kHeapIdealFree;
    } else if (target_size < num_bytes_allocated_ + kHeapMinFree) {
      target_size = num_bytes_allocated_ + kHeapMinFree;
    }

    // Calculate when to perform the next ConcurrentGC.
    if (GetTotalMemory() - GetUsedMemorySize() < concurrent_min_free_) {
      // Not enough free memory to perform concurrent GC.
      concurrent_start_bytes_ = std::numeric_limits<size_t>::max();
    } else {
      // Compute below to avoid holding both the statistics and the alloc space lock
      use_footprint_limit = true;
    }
  }

  if (use_footprint_limit) {
    size_t foot_print_limit = alloc_space_->GetFootprintLimit();
    concurrent_start_bytes_ = foot_print_limit - concurrent_start_size_;
  }
  SetIdealFootprint(target_size);
}

void Heap::ClearGrowthLimit() {
  WaitForConcurrentGcToComplete();
  alloc_space_->ClearGrowthLimit();
}

void Heap::SetReferenceOffsets(MemberOffset reference_referent_offset,
    MemberOffset reference_queue_offset,
    MemberOffset reference_queueNext_offset,
    MemberOffset reference_pendingNext_offset,
    MemberOffset finalizer_reference_zombie_offset) {
  reference_referent_offset_ = reference_referent_offset;
  reference_queue_offset_ = reference_queue_offset;
  reference_queueNext_offset_ = reference_queueNext_offset;
  reference_pendingNext_offset_ = reference_pendingNext_offset;
  finalizer_reference_zombie_offset_ = finalizer_reference_zombie_offset;
  CHECK_NE(reference_referent_offset_.Uint32Value(), 0U);
  CHECK_NE(reference_queue_offset_.Uint32Value(), 0U);
  CHECK_NE(reference_queueNext_offset_.Uint32Value(), 0U);
  CHECK_NE(reference_pendingNext_offset_.Uint32Value(), 0U);
  CHECK_NE(finalizer_reference_zombie_offset_.Uint32Value(), 0U);
}

Object* Heap::GetReferenceReferent(Object* reference) {
  DCHECK(reference != NULL);
  DCHECK_NE(reference_referent_offset_.Uint32Value(), 0U);
  return reference->GetFieldObject<Object*>(reference_referent_offset_, true);
}

void Heap::ClearReferenceReferent(Object* reference) {
  DCHECK(reference != NULL);
  DCHECK_NE(reference_referent_offset_.Uint32Value(), 0U);
  reference->SetFieldObject(reference_referent_offset_, NULL, true);
}

// Returns true if the reference object has not yet been enqueued.
bool Heap::IsEnqueuable(const Object* ref) {
  DCHECK(ref != NULL);
  const Object* queue = ref->GetFieldObject<Object*>(reference_queue_offset_, false);
  const Object* queue_next = ref->GetFieldObject<Object*>(reference_queueNext_offset_, false);
  return (queue != NULL) && (queue_next == NULL);
}

void Heap::EnqueueReference(Object* ref, Object** cleared_reference_list) {
  DCHECK(ref != NULL);
  CHECK(ref->GetFieldObject<Object*>(reference_queue_offset_, false) != NULL);
  CHECK(ref->GetFieldObject<Object*>(reference_queueNext_offset_, false) == NULL);
  EnqueuePendingReference(ref, cleared_reference_list);
}

void Heap::EnqueuePendingReference(Object* ref, Object** list) {
  DCHECK(ref != NULL);
  DCHECK(list != NULL);

  if (*list == NULL) {
    ref->SetFieldObject(reference_pendingNext_offset_, ref, false);
    *list = ref;
  } else {
    Object* head = (*list)->GetFieldObject<Object*>(reference_pendingNext_offset_, false);
    ref->SetFieldObject(reference_pendingNext_offset_, head, false);
    (*list)->SetFieldObject(reference_pendingNext_offset_, ref, false);
  }
}

Object* Heap::DequeuePendingReference(Object** list) {
  DCHECK(list != NULL);
  DCHECK(*list != NULL);
  Object* head = (*list)->GetFieldObject<Object*>(reference_pendingNext_offset_, false);
  Object* ref;
  if (*list == head) {
    ref = *list;
    *list = NULL;
  } else {
    Object* next = head->GetFieldObject<Object*>(reference_pendingNext_offset_, false);
    (*list)->SetFieldObject(reference_pendingNext_offset_, next, false);
    ref = head;
  }
  ref->SetFieldObject(reference_pendingNext_offset_, NULL, false);
  return ref;
}

void Heap::AddFinalizerReference(Thread* self, Object* object) {
  ScopedObjectAccess soa(self);
  JValue args[1];
  args[0].SetL(object);
  soa.DecodeMethod(WellKnownClasses::java_lang_ref_FinalizerReference_add)->Invoke(self,
                                                                                  NULL, args, NULL);
}

size_t Heap::GetBytesAllocated() const {
  return num_bytes_allocated_;
}

size_t Heap::GetObjectsAllocated() const {
  return num_objects_allocated_;
}

size_t Heap::GetConcurrentStartSize() const {
  return concurrent_start_size_;
}

size_t Heap::GetConcurrentMinFree() const {
  return concurrent_min_free_;
}

void Heap::EnqueueClearedReferences(Object** cleared) {
  DCHECK(cleared != NULL);
  if (*cleared != NULL) {
    ScopedObjectAccess soa(Thread::Current());
    JValue args[1];
    args[0].SetL(*cleared);
    soa.DecodeMethod(WellKnownClasses::java_lang_ref_ReferenceQueue_add)->Invoke(soa.Self(),
                                                                                 NULL, args, NULL);
    *cleared = NULL;
  }
}

void Heap::RequestConcurrentGC() {
  // Make sure that we can do a concurrent GC.
  if (requesting_gc_ ||
      !Runtime::Current()->IsFinishedStarting() ||
      Runtime::Current()->IsShuttingDown() ||
      !Runtime::Current()->IsConcurrentGcEnabled()) {
    return;
  }

  requesting_gc_ = true;
  JNIEnv* env = Thread::Current()->GetJniEnv();
  DCHECK(WellKnownClasses::java_lang_Daemons != NULL);
  DCHECK(WellKnownClasses::java_lang_Daemons_requestGC != NULL);
  env->CallStaticVoidMethod(WellKnownClasses::java_lang_Daemons,
                            WellKnownClasses::java_lang_Daemons_requestGC);
  CHECK(!env->ExceptionCheck());
  requesting_gc_ = false;
}

void Heap::ConcurrentGC() {
  if (Runtime::Current()->IsShuttingDown() || !concurrent_gc_) {
    return;
  }

  // TODO: We shouldn't need a WaitForConcurrentGcToComplete here since only
  //       concurrent GC resumes threads before the GC is completed and this function
  //       is only called within the GC daemon thread.
  if (!WaitForConcurrentGcToComplete()) {
    // Start a concurrent GC as one wasn't in progress
    ScopedThreadStateChange tsc(Thread::Current(), kWaitingPerformingGc);
    if (alloc_space_->Size() > kMinAllocSpaceSizeForStickyGC) {
      CollectGarbageInternal(GC_STICKY, false);
    } else {
      CollectGarbageInternal(GC_PARTIAL, false);
    }
  }
}

void Heap::Trim(AllocSpace* alloc_space) {
  WaitForConcurrentGcToComplete();
  alloc_space->Trim();
}

void Heap::RequestHeapTrim() {
  // We don't have a good measure of how worthwhile a trim might be. We can't use the live bitmap
  // because that only marks object heads, so a large array looks like lots of empty space. We
  // don't just call dlmalloc all the time, because the cost of an _attempted_ trim is proportional
  // to utilization (which is probably inversely proportional to how much benefit we can expect).
  // We could try mincore(2) but that's only a measure of how many pages we haven't given away,
  // not how much use we're making of those pages.
  uint64_t ms_time = NsToMs(NanoTime());
  {
    float utilization = static_cast<float>(num_bytes_allocated_) / alloc_space_->Size();
    if ((utilization > 0.75f) || ((ms_time - last_trim_time_) < 2 * 1000)) {
      // Don't bother trimming the heap if it's more than 75% utilized, or if a
      // heap trim occurred in the last two seconds.
      return;
    }
  }
  if (!Runtime::Current()->IsFinishedStarting() || Runtime::Current()->IsShuttingDown()) {
    // Heap trimming isn't supported without a Java runtime or Daemons (such as at dex2oat time)
    // Also: we do not wish to start a heap trim if the runtime is shutting down.
    return;
  }
  last_trim_time_ = ms_time;
  JNIEnv* env = Thread::Current()->GetJniEnv();
  DCHECK(WellKnownClasses::java_lang_Daemons != NULL);
  DCHECK(WellKnownClasses::java_lang_Daemons_requestHeapTrim != NULL);
  env->CallStaticVoidMethod(WellKnownClasses::java_lang_Daemons,
                            WellKnownClasses::java_lang_Daemons_requestHeapTrim);
  CHECK(!env->ExceptionCheck());
}

}  // namespace art
