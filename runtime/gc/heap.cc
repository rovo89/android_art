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

#define ATRACE_TAG ATRACE_TAG_DALVIK
#include <cutils/trace.h>

#include <limits>
#include <vector>
#include <valgrind.h>

#include "base/stl_util.h"
#include "common_throws.h"
#include "cutils/sched_policy.h"
#include "debugger.h"
#include "gc/accounting/atomic_stack.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/heap_bitmap-inl.h"
#include "gc/accounting/mod_union_table.h"
#include "gc/accounting/mod_union_table-inl.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/collector/mark_sweep-inl.h"
#include "gc/collector/partial_mark_sweep.h"
#include "gc/collector/semi_space.h"
#include "gc/collector/sticky_mark_sweep.h"
#include "gc/space/bump_pointer_space.h"
#include "gc/space/dlmalloc_space-inl.h"
#include "gc/space/image_space.h"
#include "gc/space/large_object_space.h"
#include "gc/space/rosalloc_space-inl.h"
#include "gc/space/space-inl.h"
#include "heap-inl.h"
#include "image.h"
#include "invoke_arg_array_builder.h"
#include "mirror/art_field-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "object_utils.h"
#include "os.h"
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change.h"
#include "sirt_ref.h"
#include "thread_list.h"
#include "UniquePtr.h"
#include "well_known_classes.h"

namespace art {
namespace gc {

static constexpr bool kGCALotMode = false;
static constexpr size_t kGcAlotInterval = KB;
static constexpr bool kDumpGcPerformanceOnShutdown = false;
// Minimum amount of remaining bytes before a concurrent GC is triggered.
static constexpr size_t kMinConcurrentRemainingBytes = 128 * KB;

Heap::Heap(size_t initial_size, size_t growth_limit, size_t min_free, size_t max_free,
           double target_utilization, size_t capacity, const std::string& image_file_name,
           bool concurrent_gc, size_t parallel_gc_threads, size_t conc_gc_threads,
           bool low_memory_mode, size_t long_pause_log_threshold, size_t long_gc_log_threshold,
           bool ignore_max_footprint)
    : non_moving_space_(nullptr),
      concurrent_gc_(!kMovingCollector && concurrent_gc),
      parallel_gc_threads_(parallel_gc_threads),
      conc_gc_threads_(conc_gc_threads),
      low_memory_mode_(low_memory_mode),
      long_pause_log_threshold_(long_pause_log_threshold),
      long_gc_log_threshold_(long_gc_log_threshold),
      ignore_max_footprint_(ignore_max_footprint),
      have_zygote_space_(false),
      soft_reference_queue_(this),
      weak_reference_queue_(this),
      finalizer_reference_queue_(this),
      phantom_reference_queue_(this),
      cleared_references_(this),
      is_gc_running_(false),
      last_gc_type_(collector::kGcTypeNone),
      next_gc_type_(collector::kGcTypePartial),
      capacity_(capacity),
      growth_limit_(growth_limit),
      max_allowed_footprint_(initial_size),
      native_footprint_gc_watermark_(initial_size),
      native_footprint_limit_(2 * initial_size),
      native_need_to_run_finalization_(false),
      activity_thread_class_(NULL),
      application_thread_class_(NULL),
      activity_thread_(NULL),
      application_thread_(NULL),
      last_process_state_id_(NULL),
      // Initially care about pauses in case we never get notified of process states, or if the JNI
      // code becomes broken.
      care_about_pause_times_(true),
      concurrent_start_bytes_(concurrent_gc_ ? initial_size - kMinConcurrentRemainingBytes
          :  std::numeric_limits<size_t>::max()),
      total_bytes_freed_ever_(0),
      total_objects_freed_ever_(0),
      num_bytes_allocated_(0),
      native_bytes_allocated_(0),
      gc_memory_overhead_(0),
      verify_missing_card_marks_(false),
      verify_system_weaks_(false),
      verify_pre_gc_heap_(false),
      verify_post_gc_heap_(false),
      verify_mod_union_table_(false),
      min_alloc_space_size_for_sticky_gc_(2 * MB),
      min_remaining_space_for_sticky_gc_(1 * MB),
      last_trim_time_ms_(0),
      allocation_rate_(0),
      /* For GC a lot mode, we limit the allocations stacks to be kGcAlotInterval allocations. This
       * causes a lot of GC since we do a GC for alloc whenever the stack is full. When heap
       * verification is enabled, we limit the size of allocation stacks to speed up their
       * searching.
       */
      max_allocation_stack_size_(kGCALotMode ? kGcAlotInterval
          : (kDesiredHeapVerification > kVerifyAllFast) ? KB : MB),
      current_allocator_(kMovingCollector ? kAllocatorTypeBumpPointer : kAllocatorTypeFreeList),
      current_non_moving_allocator_(kAllocatorTypeFreeList),
      bump_pointer_space_(nullptr),
      temp_space_(nullptr),
      reference_referent_offset_(0),
      reference_queue_offset_(0),
      reference_queueNext_offset_(0),
      reference_pendingNext_offset_(0),
      finalizer_reference_zombie_offset_(0),
      min_free_(min_free),
      max_free_(max_free),
      target_utilization_(target_utilization),
      total_wait_time_(0),
      total_allocation_time_(0),
      verify_object_mode_(kHeapVerificationNotPermitted),
      gc_disable_count_(0),
      running_on_valgrind_(RUNNING_ON_VALGRIND) {
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "Heap() entering";
  }

  live_bitmap_.reset(new accounting::HeapBitmap(this));
  mark_bitmap_.reset(new accounting::HeapBitmap(this));

  // Requested begin for the alloc space, to follow the mapped image and oat files
  byte* requested_alloc_space_begin = NULL;
  if (!image_file_name.empty()) {
    space::ImageSpace* image_space = space::ImageSpace::Create(image_file_name.c_str());
    CHECK(image_space != NULL) << "Failed to create space for " << image_file_name;
    AddSpace(image_space);
    // Oat files referenced by image files immediately follow them in memory, ensure alloc space
    // isn't going to get in the middle
    byte* oat_file_end_addr = image_space->GetImageHeader().GetOatFileEnd();
    CHECK_GT(oat_file_end_addr, image_space->End());
    if (oat_file_end_addr > requested_alloc_space_begin) {
      requested_alloc_space_begin =
          reinterpret_cast<byte*>(RoundUp(reinterpret_cast<uintptr_t>(oat_file_end_addr),
                                          kPageSize));
    }
  }

  const char* name = Runtime::Current()->IsZygote() ? "zygote space" : "alloc space";
  if (!kUseRosAlloc) {
    non_moving_space_ = space::DlMallocSpace::Create(name, initial_size, growth_limit, capacity,
                                                     requested_alloc_space_begin);
  } else {
    non_moving_space_ = space::RosAllocSpace::Create(name, initial_size, growth_limit, capacity,
                                                     requested_alloc_space_begin);
  }
  if (kMovingCollector) {
    // TODO: Place bump-pointer spaces somewhere to minimize size of card table.
    // TODO: Having 3+ spaces as big as the large heap size can cause virtual memory fragmentation
    // issues.
    const size_t bump_pointer_space_size = std::min(non_moving_space_->Capacity(), 128 * MB);
    bump_pointer_space_ = space::BumpPointerSpace::Create("Bump pointer space",
                                                          bump_pointer_space_size, nullptr);
    CHECK(bump_pointer_space_ != nullptr) << "Failed to create bump pointer space";
    AddSpace(bump_pointer_space_);
    temp_space_ = space::BumpPointerSpace::Create("Bump pointer space 2", bump_pointer_space_size,
                                                  nullptr);
    CHECK(temp_space_ != nullptr) << "Failed to create bump pointer space";
    AddSpace(temp_space_);
  }

  CHECK(non_moving_space_ != NULL) << "Failed to create non-moving space";
  non_moving_space_->SetFootprintLimit(non_moving_space_->Capacity());
  AddSpace(non_moving_space_);

  // Allocate the large object space.
  const bool kUseFreeListSpaceForLOS = false;
  if (kUseFreeListSpaceForLOS) {
    large_object_space_ = space::FreeListSpace::Create("large object space", NULL, capacity);
  } else {
    large_object_space_ = space::LargeObjectMapSpace::Create("large object space");
  }
  CHECK(large_object_space_ != NULL) << "Failed to create large object space";
  AddSpace(large_object_space_);

  // Compute heap capacity. Continuous spaces are sorted in order of Begin().
  CHECK(!continuous_spaces_.empty());
  // Relies on the spaces being sorted.
  byte* heap_begin = continuous_spaces_.front()->Begin();
  byte* heap_end = continuous_spaces_.back()->Limit();
  size_t heap_capacity = heap_end - heap_begin;

  // Allocate the card table.
  card_table_.reset(accounting::CardTable::Create(heap_begin, heap_capacity));
  CHECK(card_table_.get() != NULL) << "Failed to create card table";

  // Card cache for now since it makes it easier for us to update the references to the copying
  // spaces.
  accounting::ModUnionTable* mod_union_table =
      new accounting::ModUnionTableCardCache("Image mod-union table", this, GetImageSpace());
  CHECK(mod_union_table != nullptr) << "Failed to create image mod-union table";
  AddModUnionTable(mod_union_table);

  // TODO: Count objects in the image space here.
  num_bytes_allocated_ = 0;

  // Default mark stack size in bytes.
  static const size_t default_mark_stack_size = 64 * KB;
  mark_stack_.reset(accounting::ObjectStack::Create("mark stack", default_mark_stack_size));
  allocation_stack_.reset(accounting::ObjectStack::Create("allocation stack",
                                                          max_allocation_stack_size_));
  live_stack_.reset(accounting::ObjectStack::Create("live stack",
                                                    max_allocation_stack_size_));

  // It's still too early to take a lock because there are no threads yet, but we can create locks
  // now. We don't create it earlier to make it clear that you can't use locks during heap
  // initialization.
  gc_complete_lock_ = new Mutex("GC complete lock");
  gc_complete_cond_.reset(new ConditionVariable("GC complete condition variable",
                                                *gc_complete_lock_));
  last_gc_time_ns_ = NanoTime();
  last_gc_size_ = GetBytesAllocated();

  if (ignore_max_footprint_) {
    SetIdealFootprint(std::numeric_limits<size_t>::max());
    concurrent_start_bytes_ = std::numeric_limits<size_t>::max();
  }
  CHECK_NE(max_allowed_footprint_, 0U);

  // Create our garbage collectors.
  if (!kMovingCollector) {
    for (size_t i = 0; i < 2; ++i) {
      const bool concurrent = i != 0;
      garbage_collectors_.push_back(new collector::MarkSweep(this, concurrent));
      garbage_collectors_.push_back(new collector::PartialMarkSweep(this, concurrent));
      garbage_collectors_.push_back(new collector::StickyMarkSweep(this, concurrent));
    }
    gc_plan_.push_back(collector::kGcTypeSticky);
    gc_plan_.push_back(collector::kGcTypePartial);
    gc_plan_.push_back(collector::kGcTypeFull);
  } else {
    semi_space_collector_ = new collector::SemiSpace(this);
    garbage_collectors_.push_back(semi_space_collector_);
    gc_plan_.push_back(collector::kGcTypeFull);
  }

  if (running_on_valgrind_) {
    Runtime::Current()->GetInstrumentation()->InstrumentQuickAllocEntryPoints();
  }

  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "Heap() exiting";
  }
}

bool Heap::IsCompilingBoot() const {
  for (const auto& space : continuous_spaces_) {
    if (space->IsImageSpace()) {
      return false;
    } else if (space->IsZygoteSpace()) {
      return false;
    }
  }
  return true;
}

bool Heap::HasImageSpace() const {
  for (const auto& space : continuous_spaces_) {
    if (space->IsImageSpace()) {
      return true;
    }
  }
  return false;
}

void Heap::IncrementDisableGC(Thread* self) {
  // Need to do this holding the lock to prevent races where the GC is about to run / running when
  // we attempt to disable it.
  ScopedThreadStateChange tsc(self, kWaitingForGcToComplete);
  MutexLock mu(self, *gc_complete_lock_);
  WaitForGcToCompleteLocked(self);
  ++gc_disable_count_;
}

void Heap::DecrementDisableGC(Thread* self) {
  MutexLock mu(self, *gc_complete_lock_);
  CHECK_GE(gc_disable_count_, 0U);
  --gc_disable_count_;
}

void Heap::CreateThreadPool() {
  const size_t num_threads = std::max(parallel_gc_threads_, conc_gc_threads_);
  if (num_threads != 0) {
    thread_pool_.reset(new ThreadPool("Heap thread pool", num_threads));
  }
}

void Heap::VisitObjects(ObjectVisitorCallback callback, void* arg) {
  // Visit objects in bump pointer space.
  Thread* self = Thread::Current();
  // TODO: Use reference block.
  std::vector<SirtRef<mirror::Object>*> saved_refs;
  if (bump_pointer_space_ != nullptr) {
    // Need to put all these in sirts since the callback may trigger a GC. TODO: Use a better data
    // structure.
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(bump_pointer_space_->Begin());
    const mirror::Object* end = reinterpret_cast<const mirror::Object*>(
        bump_pointer_space_->End());
    while (obj < end) {
      saved_refs.push_back(new SirtRef<mirror::Object>(self, obj));
      obj = space::BumpPointerSpace::GetNextObject(obj);
    }
  }
  // TODO: Switch to standard begin and end to use ranged a based loop.
  for (mirror::Object** it = allocation_stack_->Begin(), **end = allocation_stack_->End();
      it < end; ++it) {
    mirror::Object* obj = *it;
    // Objects in the allocation stack might be in a movable space.
    saved_refs.push_back(new SirtRef<mirror::Object>(self, obj));
  }
  GetLiveBitmap()->Walk(callback, arg);
  for (const auto& ref : saved_refs) {
    callback(ref->get(), arg);
  }
  // Need to free the sirts in reverse order they were allocated.
  for (size_t i = saved_refs.size(); i != 0; --i) {
    delete saved_refs[i - 1];
  }
}

void Heap::MarkAllocStackAsLive(accounting::ObjectStack* stack) {
  MarkAllocStack(non_moving_space_->GetLiveBitmap(), large_object_space_->GetLiveObjects(), stack);
}

void Heap::DeleteThreadPool() {
  thread_pool_.reset(nullptr);
}

static bool ReadStaticInt(JNIEnvExt* env, jclass clz, const char* name, int* out_value) {
  DCHECK(out_value != NULL);
  jfieldID field = env->GetStaticFieldID(clz, name, "I");
  if (field == NULL) {
    env->ExceptionClear();
    return false;
  }
  *out_value = env->GetStaticIntField(clz, field);
  return true;
}

void Heap::ListenForProcessStateChange() {
  VLOG(heap) << "Heap notified of process state change";

  Thread* self = Thread::Current();
  JNIEnvExt* env = self->GetJniEnv();

  if (!have_zygote_space_) {
    return;
  }

  if (activity_thread_class_ == NULL) {
    jclass clz = env->FindClass("android/app/ActivityThread");
    if (clz == NULL) {
      env->ExceptionClear();
      LOG(WARNING) << "Could not find activity thread class in process state change";
      return;
    }
    activity_thread_class_ = reinterpret_cast<jclass>(env->NewGlobalRef(clz));
  }

  if (activity_thread_class_ != NULL && activity_thread_ == NULL) {
    jmethodID current_activity_method = env->GetStaticMethodID(activity_thread_class_,
                                                               "currentActivityThread",
                                                               "()Landroid/app/ActivityThread;");
    if (current_activity_method == NULL) {
      env->ExceptionClear();
      LOG(WARNING) << "Could not get method for currentActivityThread";
      return;
    }

    jobject obj = env->CallStaticObjectMethod(activity_thread_class_, current_activity_method);
    if (obj == NULL) {
      env->ExceptionClear();
      LOG(WARNING) << "Could not get current activity";
      return;
    }
    activity_thread_ = env->NewGlobalRef(obj);
  }

  if (process_state_cares_about_pause_time_.empty()) {
    // Just attempt to do this the first time.
    jclass clz = env->FindClass("android/app/ActivityManager");
    if (clz == NULL) {
      LOG(WARNING) << "Activity manager class is null";
      return;
    }
    ScopedLocalRef<jclass> activity_manager(env, clz);
    std::vector<const char*> care_about_pauses;
    care_about_pauses.push_back("PROCESS_STATE_TOP");
    care_about_pauses.push_back("PROCESS_STATE_IMPORTANT_BACKGROUND");
    // Attempt to read the constants and classify them as whether or not we care about pause times.
    for (size_t i = 0; i < care_about_pauses.size(); ++i) {
      int process_state = 0;
      if (ReadStaticInt(env, activity_manager.get(), care_about_pauses[i], &process_state)) {
        process_state_cares_about_pause_time_.insert(process_state);
        VLOG(heap) << "Adding process state " << process_state
                   << " to set of states which care about pause time";
      }
    }
  }

  if (application_thread_class_ == NULL) {
    jclass clz = env->FindClass("android/app/ActivityThread$ApplicationThread");
    if (clz == NULL) {
      env->ExceptionClear();
      LOG(WARNING) << "Could not get application thread class";
      return;
    }
    application_thread_class_ = reinterpret_cast<jclass>(env->NewGlobalRef(clz));
    last_process_state_id_ = env->GetFieldID(application_thread_class_, "mLastProcessState", "I");
    if (last_process_state_id_ == NULL) {
      env->ExceptionClear();
      LOG(WARNING) << "Could not get last process state member";
      return;
    }
  }

  if (application_thread_class_ != NULL && application_thread_ == NULL) {
    jmethodID get_application_thread =
        env->GetMethodID(activity_thread_class_, "getApplicationThread",
                         "()Landroid/app/ActivityThread$ApplicationThread;");
    if (get_application_thread == NULL) {
      LOG(WARNING) << "Could not get method ID for get application thread";
      return;
    }

    jobject obj = env->CallObjectMethod(activity_thread_, get_application_thread);
    if (obj == NULL) {
      LOG(WARNING) << "Could not get application thread";
      return;
    }

    application_thread_ = env->NewGlobalRef(obj);
  }

  if (application_thread_ != NULL && last_process_state_id_ != NULL) {
    int process_state = env->GetIntField(application_thread_, last_process_state_id_);
    env->ExceptionClear();

    care_about_pause_times_ = process_state_cares_about_pause_time_.find(process_state) !=
        process_state_cares_about_pause_time_.end();

    VLOG(heap) << "New process state " << process_state
               << " care about pauses " << care_about_pause_times_;
  }
}

void Heap::AddSpace(space::Space* space) {
  DCHECK(space != NULL);
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  if (space->IsContinuousSpace()) {
    DCHECK(!space->IsDiscontinuousSpace());
    space::ContinuousSpace* continuous_space = space->AsContinuousSpace();
    // Continuous spaces don't necessarily have bitmaps.
    accounting::SpaceBitmap* live_bitmap = continuous_space->GetLiveBitmap();
    accounting::SpaceBitmap* mark_bitmap = continuous_space->GetMarkBitmap();
    if (live_bitmap != nullptr) {
      DCHECK(mark_bitmap != nullptr);
      live_bitmap_->AddContinuousSpaceBitmap(live_bitmap);
      mark_bitmap_->AddContinuousSpaceBitmap(mark_bitmap);
    }

    continuous_spaces_.push_back(continuous_space);
    if (continuous_space->IsMallocSpace()) {
      non_moving_space_ = continuous_space->AsMallocSpace();
    }

    // Ensure that spaces remain sorted in increasing order of start address.
    std::sort(continuous_spaces_.begin(), continuous_spaces_.end(),
              [](const space::ContinuousSpace* a, const space::ContinuousSpace* b) {
      return a->Begin() < b->Begin();
    });
    // Ensure that ImageSpaces < ZygoteSpaces < AllocSpaces so that we can do address based checks to
    // avoid redundant marking.
    bool seen_zygote = false, seen_alloc = false;
    for (const auto& space : continuous_spaces_) {
      if (space->IsImageSpace()) {
        CHECK(!seen_zygote);
        CHECK(!seen_alloc);
      } else if (space->IsZygoteSpace()) {
        CHECK(!seen_alloc);
        seen_zygote = true;
      } else if (space->IsMallocSpace()) {
        seen_alloc = true;
      }
    }
  } else {
    DCHECK(space->IsDiscontinuousSpace());
    space::DiscontinuousSpace* discontinuous_space = space->AsDiscontinuousSpace();
    DCHECK(discontinuous_space->GetLiveObjects() != nullptr);
    live_bitmap_->AddDiscontinuousObjectSet(discontinuous_space->GetLiveObjects());
    DCHECK(discontinuous_space->GetMarkObjects() != nullptr);
    mark_bitmap_->AddDiscontinuousObjectSet(discontinuous_space->GetMarkObjects());
    discontinuous_spaces_.push_back(discontinuous_space);
  }
  if (space->IsAllocSpace()) {
    alloc_spaces_.push_back(space->AsAllocSpace());
  }
}

void Heap::RegisterGCAllocation(size_t bytes) {
  if (this != nullptr) {
    gc_memory_overhead_.fetch_add(bytes);
  }
}

void Heap::RegisterGCDeAllocation(size_t bytes) {
  if (this != nullptr) {
    gc_memory_overhead_.fetch_sub(bytes);
  }
}

void Heap::DumpGcPerformanceInfo(std::ostream& os) {
  // Dump cumulative timings.
  os << "Dumping cumulative Gc timings\n";
  uint64_t total_duration = 0;

  // Dump cumulative loggers for each GC type.
  uint64_t total_paused_time = 0;
  for (const auto& collector : garbage_collectors_) {
    CumulativeLogger& logger = collector->GetCumulativeTimings();
    if (logger.GetTotalNs() != 0) {
      os << Dumpable<CumulativeLogger>(logger);
      const uint64_t total_ns = logger.GetTotalNs();
      const uint64_t total_pause_ns = collector->GetTotalPausedTimeNs();
      double seconds = NsToMs(logger.GetTotalNs()) / 1000.0;
      const uint64_t freed_bytes = collector->GetTotalFreedBytes();
      const uint64_t freed_objects = collector->GetTotalFreedObjects();
      os << collector->GetName() << " total time: " << PrettyDuration(total_ns) << "\n"
         << collector->GetName() << " paused time: " << PrettyDuration(total_pause_ns) << "\n"
         << collector->GetName() << " freed: " << freed_objects
         << " objects with total size " << PrettySize(freed_bytes) << "\n"
         << collector->GetName() << " throughput: " << freed_objects / seconds << "/s / "
         << PrettySize(freed_bytes / seconds) << "/s\n";
      total_duration += total_ns;
      total_paused_time += total_pause_ns;
    }
  }
  uint64_t allocation_time = static_cast<uint64_t>(total_allocation_time_) * kTimeAdjust;
  size_t total_objects_allocated = GetObjectsAllocatedEver();
  size_t total_bytes_allocated = GetBytesAllocatedEver();
  if (total_duration != 0) {
    const double total_seconds = static_cast<double>(total_duration / 1000) / 1000000.0;
    os << "Total time spent in GC: " << PrettyDuration(total_duration) << "\n";
    os << "Mean GC size throughput: "
       << PrettySize(GetBytesFreedEver() / total_seconds) << "/s\n";
    os << "Mean GC object throughput: "
       << (GetObjectsFreedEver() / total_seconds) << " objects/s\n";
  }
  os << "Total number of allocations: " << total_objects_allocated << "\n";
  os << "Total bytes allocated " << PrettySize(total_bytes_allocated) << "\n";
  if (kMeasureAllocationTime) {
    os << "Total time spent allocating: " << PrettyDuration(allocation_time) << "\n";
    os << "Mean allocation time: " << PrettyDuration(allocation_time / total_objects_allocated)
       << "\n";
  }
  os << "Total mutator paused time: " << PrettyDuration(total_paused_time) << "\n";
  os << "Total time waiting for GC to complete: " << PrettyDuration(total_wait_time_) << "\n";
  os << "Approximate GC data structures memory overhead: " << gc_memory_overhead_;
}

Heap::~Heap() {
  VLOG(heap) << "Starting ~Heap()";
  if (kDumpGcPerformanceOnShutdown) {
    DumpGcPerformanceInfo(LOG(INFO));
  }
  STLDeleteElements(&garbage_collectors_);
  // If we don't reset then the mark stack complains in its destructor.
  allocation_stack_->Reset();
  live_stack_->Reset();
  STLDeleteValues(&mod_union_tables_);
  STLDeleteElements(&continuous_spaces_);
  STLDeleteElements(&discontinuous_spaces_);
  delete gc_complete_lock_;
  VLOG(heap) << "Finished ~Heap()";
}

space::ContinuousSpace* Heap::FindContinuousSpaceFromObject(const mirror::Object* obj,
                                                            bool fail_ok) const {
  for (const auto& space : continuous_spaces_) {
    if (space->Contains(obj)) {
      return space;
    }
  }
  if (!fail_ok) {
    LOG(FATAL) << "object " << reinterpret_cast<const void*>(obj) << " not inside any spaces!";
  }
  return NULL;
}

space::DiscontinuousSpace* Heap::FindDiscontinuousSpaceFromObject(const mirror::Object* obj,
                                                                  bool fail_ok) const {
  for (const auto& space : discontinuous_spaces_) {
    if (space->Contains(obj)) {
      return space;
    }
  }
  if (!fail_ok) {
    LOG(FATAL) << "object " << reinterpret_cast<const void*>(obj) << " not inside any spaces!";
  }
  return NULL;
}

space::Space* Heap::FindSpaceFromObject(const mirror::Object* obj, bool fail_ok) const {
  space::Space* result = FindContinuousSpaceFromObject(obj, true);
  if (result != NULL) {
    return result;
  }
  return FindDiscontinuousSpaceFromObject(obj, true);
}

struct SoftReferenceArgs {
  RootVisitor* is_marked_callback_;
  RootVisitor* recursive_mark_callback_;
  void* arg_;
};

mirror::Object* Heap::PreserveSoftReferenceCallback(mirror::Object* obj, void* arg) {
  SoftReferenceArgs* args  = reinterpret_cast<SoftReferenceArgs*>(arg);
  // TODO: Not preserve all soft references.
  return args->recursive_mark_callback_(obj, args->arg_);
}

// Process reference class instances and schedule finalizations.
void Heap::ProcessReferences(TimingLogger& timings, bool clear_soft,
                             RootVisitor* is_marked_callback,
                             RootVisitor* recursive_mark_object_callback, void* arg) {
  // Unless we are in the zygote or required to clear soft references with white references,
  // preserve some white referents.
  if (!clear_soft && !Runtime::Current()->IsZygote()) {
    SoftReferenceArgs soft_reference_args;
    soft_reference_args.is_marked_callback_ = is_marked_callback;
    soft_reference_args.recursive_mark_callback_ = recursive_mark_object_callback;
    soft_reference_args.arg_ = arg;
    soft_reference_queue_.PreserveSomeSoftReferences(&PreserveSoftReferenceCallback,
                                                     &soft_reference_args);
  }
  timings.StartSplit("ProcessReferences");
  // Clear all remaining soft and weak references with white referents.
  soft_reference_queue_.ClearWhiteReferences(cleared_references_, is_marked_callback, arg);
  weak_reference_queue_.ClearWhiteReferences(cleared_references_, is_marked_callback, arg);
  timings.EndSplit();
  // Preserve all white objects with finalize methods and schedule them for finalization.
  timings.StartSplit("EnqueueFinalizerReferences");
  finalizer_reference_queue_.EnqueueFinalizerReferences(cleared_references_, is_marked_callback,
                                                        recursive_mark_object_callback, arg);
  timings.EndSplit();
  timings.StartSplit("ProcessReferences");
  // Clear all f-reachable soft and weak references with white referents.
  soft_reference_queue_.ClearWhiteReferences(cleared_references_, is_marked_callback, arg);
  weak_reference_queue_.ClearWhiteReferences(cleared_references_, is_marked_callback, arg);
  // Clear all phantom references with white referents.
  phantom_reference_queue_.ClearWhiteReferences(cleared_references_, is_marked_callback, arg);
  // At this point all reference queues other than the cleared references should be empty.
  DCHECK(soft_reference_queue_.IsEmpty());
  DCHECK(weak_reference_queue_.IsEmpty());
  DCHECK(finalizer_reference_queue_.IsEmpty());
  DCHECK(phantom_reference_queue_.IsEmpty());
  timings.EndSplit();
}

bool Heap::IsEnqueued(mirror::Object* ref) const {
  // Since the references are stored as cyclic lists it means that once enqueued, the pending next
  // will always be non-null.
  return ref->GetFieldObject<mirror::Object*>(GetReferencePendingNextOffset(), false) != nullptr;
}

bool Heap::IsEnqueuable(const mirror::Object* ref) const {
  DCHECK(ref != nullptr);
  const mirror::Object* queue =
      ref->GetFieldObject<mirror::Object*>(GetReferenceQueueOffset(), false);
  const mirror::Object* queue_next =
      ref->GetFieldObject<mirror::Object*>(GetReferenceQueueNextOffset(), false);
  return queue != nullptr && queue_next == nullptr;
}

// Process the "referent" field in a java.lang.ref.Reference.  If the referent has not yet been
// marked, put it on the appropriate list in the heap for later processing.
void Heap::DelayReferenceReferent(mirror::Class* klass, mirror::Object* obj,
                                  RootVisitor mark_visitor, void* arg) {
  DCHECK(klass != nullptr);
  DCHECK(klass->IsReferenceClass());
  DCHECK(obj != nullptr);
  mirror::Object* referent = GetReferenceReferent(obj);
  if (referent != nullptr) {
    mirror::Object* forward_address = mark_visitor(referent, arg);
    // Null means that the object is not currently marked.
    if (forward_address == nullptr) {
      Thread* self = Thread::Current();
      // TODO: Remove these locks, and use atomic stacks for storing references?
      // We need to check that the references haven't already been enqueued since we can end up
      // scanning the same reference multiple times due to dirty cards.
      if (klass->IsSoftReferenceClass()) {
        soft_reference_queue_.AtomicEnqueueIfNotEnqueued(self, obj);
      } else if (klass->IsWeakReferenceClass()) {
        weak_reference_queue_.AtomicEnqueueIfNotEnqueued(self, obj);
      } else if (klass->IsFinalizerReferenceClass()) {
        finalizer_reference_queue_.AtomicEnqueueIfNotEnqueued(self, obj);
      } else if (klass->IsPhantomReferenceClass()) {
        phantom_reference_queue_.AtomicEnqueueIfNotEnqueued(self, obj);
      } else {
        LOG(FATAL) << "Invalid reference type " << PrettyClass(klass) << " " << std::hex
                   << klass->GetAccessFlags();
      }
    } else if (referent != forward_address) {
      // Referent is already marked and we need to update it.
      SetReferenceReferent(obj, forward_address);
    }
  }
}

space::ImageSpace* Heap::GetImageSpace() const {
  for (const auto& space : continuous_spaces_) {
    if (space->IsImageSpace()) {
      return space->AsImageSpace();
    }
  }
  return NULL;
}

static void MSpaceChunkCallback(void* start, void* end, size_t used_bytes, void* arg) {
  size_t chunk_size = reinterpret_cast<uint8_t*>(end) - reinterpret_cast<uint8_t*>(start);
  if (used_bytes < chunk_size) {
    size_t chunk_free_bytes = chunk_size - used_bytes;
    size_t& max_contiguous_allocation = *reinterpret_cast<size_t*>(arg);
    max_contiguous_allocation = std::max(max_contiguous_allocation, chunk_free_bytes);
  }
}

void Heap::ThrowOutOfMemoryError(Thread* self, size_t byte_count, bool large_object_allocation) {
  std::ostringstream oss;
  int64_t total_bytes_free = GetFreeMemory();
  oss << "Failed to allocate a " << byte_count << " byte allocation with " << total_bytes_free
      << " free bytes";
  // If the allocation failed due to fragmentation, print out the largest continuous allocation.
  if (!large_object_allocation && total_bytes_free >= byte_count) {
    size_t max_contiguous_allocation = 0;
    for (const auto& space : continuous_spaces_) {
      if (space->IsMallocSpace()) {
        // To allow the Walk/InspectAll() to exclusively-lock the mutator
        // lock, temporarily release the shared access to the mutator
        // lock here by transitioning to the suspended state.
        Locks::mutator_lock_->AssertSharedHeld(self);
        self->TransitionFromRunnableToSuspended(kSuspended);
        space->AsMallocSpace()->Walk(MSpaceChunkCallback, &max_contiguous_allocation);
        self->TransitionFromSuspendedToRunnable();
        Locks::mutator_lock_->AssertSharedHeld(self);
      }
    }
    oss << "; failed due to fragmentation (largest possible contiguous allocation "
        <<  max_contiguous_allocation << " bytes)";
  }
  self->ThrowOutOfMemoryError(oss.str().c_str());
}

void Heap::Trim() {
  uint64_t start_ns = NanoTime();
  // Trim the managed spaces.
  uint64_t total_alloc_space_allocated = 0;
  uint64_t total_alloc_space_size = 0;
  uint64_t managed_reclaimed = 0;
  for (const auto& space : continuous_spaces_) {
    if (space->IsMallocSpace() && !space->IsZygoteSpace()) {
      gc::space::MallocSpace* alloc_space = space->AsMallocSpace();
      total_alloc_space_size += alloc_space->Size();
      managed_reclaimed += alloc_space->Trim();
    }
  }
  total_alloc_space_allocated = GetBytesAllocated() - large_object_space_->GetBytesAllocated() -
      bump_pointer_space_->GetBytesAllocated();
  const float managed_utilization = static_cast<float>(total_alloc_space_allocated) /
      static_cast<float>(total_alloc_space_size);
  uint64_t gc_heap_end_ns = NanoTime();
  // Trim the native heap.
  dlmalloc_trim(0);
  size_t native_reclaimed = 0;
  dlmalloc_inspect_all(DlmallocMadviseCallback, &native_reclaimed);
  uint64_t end_ns = NanoTime();
  VLOG(heap) << "Heap trim of managed (duration=" << PrettyDuration(gc_heap_end_ns - start_ns)
      << ", advised=" << PrettySize(managed_reclaimed) << ") and native (duration="
      << PrettyDuration(end_ns - gc_heap_end_ns) << ", advised=" << PrettySize(native_reclaimed)
      << ") heaps. Managed heap utilization of " << static_cast<int>(100 * managed_utilization)
      << "%.";
}

bool Heap::IsValidObjectAddress(const mirror::Object* obj) const {
  // Note: we deliberately don't take the lock here, and mustn't test anything that would require
  // taking the lock.
  if (obj == nullptr) {
    return true;
  }
  return IsAligned<kObjectAlignment>(obj) && IsHeapAddress(obj);
}

bool Heap::IsHeapAddress(const mirror::Object* obj) const {
  if (kMovingCollector && bump_pointer_space_->HasAddress(obj)) {
    return true;
  }
  // TODO: This probably doesn't work for large objects.
  return FindSpaceFromObject(obj, true) != nullptr;
}

bool Heap::IsLiveObjectLocked(const mirror::Object* obj, bool search_allocation_stack,
                              bool search_live_stack, bool sorted) {
  // Locks::heap_bitmap_lock_->AssertReaderHeld(Thread::Current());
  if (obj == nullptr || UNLIKELY(!IsAligned<kObjectAlignment>(obj))) {
    return false;
  }
  space::ContinuousSpace* c_space = FindContinuousSpaceFromObject(obj, true);
  space::DiscontinuousSpace* d_space = NULL;
  if (c_space != NULL) {
    if (c_space->GetLiveBitmap()->Test(obj)) {
      return true;
    }
  } else if (bump_pointer_space_->Contains(obj) || temp_space_->Contains(obj)) {
      return true;
  } else {
    d_space = FindDiscontinuousSpaceFromObject(obj, true);
    if (d_space != NULL) {
      if (d_space->GetLiveObjects()->Test(obj)) {
        return true;
      }
    }
  }
  // This is covering the allocation/live stack swapping that is done without mutators suspended.
  for (size_t i = 0; i < (sorted ? 1 : 5); ++i) {
    if (i > 0) {
      NanoSleep(MsToNs(10));
    }
    if (search_allocation_stack) {
      if (sorted) {
        if (allocation_stack_->ContainsSorted(const_cast<mirror::Object*>(obj))) {
          return true;
        }
      } else if (allocation_stack_->Contains(const_cast<mirror::Object*>(obj))) {
        return true;
      }
    }

    if (search_live_stack) {
      if (sorted) {
        if (live_stack_->ContainsSorted(const_cast<mirror::Object*>(obj))) {
          return true;
        }
      } else if (live_stack_->Contains(const_cast<mirror::Object*>(obj))) {
        return true;
      }
    }
  }
  // We need to check the bitmaps again since there is a race where we mark something as live and
  // then clear the stack containing it.
  if (c_space != NULL) {
    if (c_space->GetLiveBitmap()->Test(obj)) {
      return true;
    }
  } else {
    d_space = FindDiscontinuousSpaceFromObject(obj, true);
    if (d_space != NULL && d_space->GetLiveObjects()->Test(obj)) {
      return true;
    }
  }
  return false;
}

void Heap::VerifyObjectImpl(const mirror::Object* obj) {
  if (Thread::Current() == NULL ||
      Runtime::Current()->GetThreadList()->GetLockOwner() == Thread::Current()->GetTid()) {
    return;
  }
  VerifyObjectBody(obj);
}

void Heap::DumpSpaces(std::ostream& stream) {
  for (const auto& space : continuous_spaces_) {
    accounting::SpaceBitmap* live_bitmap = space->GetLiveBitmap();
    accounting::SpaceBitmap* mark_bitmap = space->GetMarkBitmap();
    stream << space << " " << *space << "\n";
    if (live_bitmap != nullptr) {
      stream << live_bitmap << " " << *live_bitmap << "\n";
    }
    if (mark_bitmap != nullptr) {
      stream << mark_bitmap << " " << *mark_bitmap << "\n";
    }
  }
  for (const auto& space : discontinuous_spaces_) {
    stream << space << " " << *space << "\n";
  }
}

void Heap::VerifyObjectBody(const mirror::Object* obj) {
  CHECK(IsAligned<kObjectAlignment>(obj)) << "Object isn't aligned: " << obj;
  // Ignore early dawn of the universe verifications.
  if (UNLIKELY(static_cast<size_t>(num_bytes_allocated_.load()) < 10 * KB)) {
    return;
  }
  const byte* raw_addr = reinterpret_cast<const byte*>(obj) +
      mirror::Object::ClassOffset().Int32Value();
  const mirror::Class* c = *reinterpret_cast<mirror::Class* const *>(raw_addr);
  if (UNLIKELY(c == NULL)) {
    LOG(FATAL) << "Null class in object: " << obj;
  } else if (UNLIKELY(!IsAligned<kObjectAlignment>(c))) {
    LOG(FATAL) << "Class isn't aligned: " << c << " in object: " << obj;
  }
  // Check obj.getClass().getClass() == obj.getClass().getClass().getClass()
  // Note: we don't use the accessors here as they have internal sanity checks
  // that we don't want to run
  raw_addr = reinterpret_cast<const byte*>(c) + mirror::Object::ClassOffset().Int32Value();
  const mirror::Class* c_c = *reinterpret_cast<mirror::Class* const *>(raw_addr);
  raw_addr = reinterpret_cast<const byte*>(c_c) + mirror::Object::ClassOffset().Int32Value();
  const mirror::Class* c_c_c = *reinterpret_cast<mirror::Class* const *>(raw_addr);
  CHECK_EQ(c_c, c_c_c);

  if (verify_object_mode_ > kVerifyAllFast) {
    // TODO: the bitmap tests below are racy if VerifyObjectBody is called without the
    //       heap_bitmap_lock_.
    if (!IsLiveObjectLocked(obj)) {
      DumpSpaces();
      LOG(FATAL) << "Object is dead: " << obj;
    }
    if (!IsLiveObjectLocked(c)) {
      LOG(FATAL) << "Class of object is dead: " << c << " in object: " << obj;
    }
  }
}

void Heap::VerificationCallback(mirror::Object* obj, void* arg) {
  DCHECK(obj != NULL);
  reinterpret_cast<Heap*>(arg)->VerifyObjectBody(obj);
}

void Heap::VerifyHeap() {
  ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  GetLiveBitmap()->Walk(Heap::VerificationCallback, this);
}

void Heap::RecordFree(size_t freed_objects, size_t freed_bytes) {
  DCHECK_LE(freed_bytes, static_cast<size_t>(num_bytes_allocated_));
  num_bytes_allocated_.fetch_sub(freed_bytes);

  if (Runtime::Current()->HasStatsEnabled()) {
    RuntimeStats* thread_stats = Thread::Current()->GetStats();
    thread_stats->freed_objects += freed_objects;
    thread_stats->freed_bytes += freed_bytes;

    // TODO: Do this concurrently.
    RuntimeStats* global_stats = Runtime::Current()->GetStats();
    global_stats->freed_objects += freed_objects;
    global_stats->freed_bytes += freed_bytes;
  }
}

mirror::Object* Heap::AllocateInternalWithGc(Thread* self, AllocatorType allocator,
                                             size_t alloc_size, size_t* bytes_allocated) {
  mirror::Object* ptr = nullptr;
  // The allocation failed. If the GC is running, block until it completes, and then retry the
  // allocation.
  collector::GcType last_gc = WaitForGcToComplete(self);
  if (last_gc != collector::kGcTypeNone) {
    // A GC was in progress and we blocked, retry allocation now that memory has been freed.
    ptr = TryToAllocate<true>(self, allocator, alloc_size, false, bytes_allocated);
  }

  // Loop through our different Gc types and try to Gc until we get enough free memory.
  for (collector::GcType gc_type : gc_plan_) {
    if (ptr != nullptr) {
      break;
    }
    // Attempt to run the collector, if we succeed, re-try the allocation.
    if (CollectGarbageInternal(gc_type, kGcCauseForAlloc, false) != collector::kGcTypeNone) {
      // Did we free sufficient memory for the allocation to succeed?
      ptr = TryToAllocate<true>(self, allocator, alloc_size, false, bytes_allocated);
    }
  }
  // Allocations have failed after GCs;  this is an exceptional state.
  if (ptr == nullptr) {
    // Try harder, growing the heap if necessary.
    ptr = TryToAllocate<true>(self, allocator, alloc_size, true, bytes_allocated);
  }
  if (ptr == nullptr) {
    // Most allocations should have succeeded by now, so the heap is really full, really fragmented,
    // or the requested size is really big. Do another GC, collecting SoftReferences this time. The
    // VM spec requires that all SoftReferences have been collected and cleared before throwing
    // OOME.
    VLOG(gc) << "Forcing collection of SoftReferences for " << PrettySize(alloc_size)
             << " allocation";
    // TODO: Run finalization, but this may cause more allocations to occur.
    // We don't need a WaitForGcToComplete here either.
    DCHECK(!gc_plan_.empty());
    CollectGarbageInternal(gc_plan_.back(), kGcCauseForAlloc, true);
    ptr = TryToAllocate<true>(self, allocator, alloc_size, true, bytes_allocated);
    if (ptr == nullptr) {
      ThrowOutOfMemoryError(self, alloc_size, false);
    }
  }
  return ptr;
}

void Heap::SetTargetHeapUtilization(float target) {
  DCHECK_GT(target, 0.0f);  // asserted in Java code
  DCHECK_LT(target, 1.0f);
  target_utilization_ = target;
}

size_t Heap::GetObjectsAllocated() const {
  size_t total = 0;
  for (space::AllocSpace* space : alloc_spaces_) {
    total += space->GetObjectsAllocated();
  }
  return total;
}

size_t Heap::GetObjectsAllocatedEver() const {
  size_t total = 0;
  for (space::AllocSpace* space : alloc_spaces_) {
    total += space->GetTotalObjectsAllocated();
  }
  return total;
}

size_t Heap::GetBytesAllocatedEver() const {
  size_t total = 0;
  for (space::AllocSpace* space : alloc_spaces_) {
    total += space->GetTotalBytesAllocated();
  }
  return total;
}

class InstanceCounter {
 public:
  InstanceCounter(const std::vector<mirror::Class*>& classes, bool use_is_assignable_from, uint64_t* counts)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : classes_(classes), use_is_assignable_from_(use_is_assignable_from), counts_(counts) {
  }

  void operator()(const mirror::Object* o) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    for (size_t i = 0; i < classes_.size(); ++i) {
      const mirror::Class* instance_class = o->GetClass();
      if (use_is_assignable_from_) {
        if (instance_class != NULL && classes_[i]->IsAssignableFrom(instance_class)) {
          ++counts_[i];
        }
      } else {
        if (instance_class == classes_[i]) {
          ++counts_[i];
        }
      }
    }
  }

 private:
  const std::vector<mirror::Class*>& classes_;
  bool use_is_assignable_from_;
  uint64_t* const counts_;

  DISALLOW_COPY_AND_ASSIGN(InstanceCounter);
};

void Heap::CountInstances(const std::vector<mirror::Class*>& classes, bool use_is_assignable_from,
                          uint64_t* counts) {
  // We only want reachable instances, so do a GC. This also ensures that the alloc stack
  // is empty, so the live bitmap is the only place we need to look.
  Thread* self = Thread::Current();
  self->TransitionFromRunnableToSuspended(kNative);
  CollectGarbage(false);
  self->TransitionFromSuspendedToRunnable();

  InstanceCounter counter(classes, use_is_assignable_from, counts);
  ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
  GetLiveBitmap()->Visit(counter);
}

class InstanceCollector {
 public:
  InstanceCollector(mirror::Class* c, int32_t max_count, std::vector<mirror::Object*>& instances)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : class_(c), max_count_(max_count), instances_(instances) {
  }

  void operator()(const mirror::Object* o) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const mirror::Class* instance_class = o->GetClass();
    if (instance_class == class_) {
      if (max_count_ == 0 || instances_.size() < max_count_) {
        instances_.push_back(const_cast<mirror::Object*>(o));
      }
    }
  }

 private:
  mirror::Class* class_;
  uint32_t max_count_;
  std::vector<mirror::Object*>& instances_;

  DISALLOW_COPY_AND_ASSIGN(InstanceCollector);
};

void Heap::GetInstances(mirror::Class* c, int32_t max_count,
                        std::vector<mirror::Object*>& instances) {
  // We only want reachable instances, so do a GC. This also ensures that the alloc stack
  // is empty, so the live bitmap is the only place we need to look.
  Thread* self = Thread::Current();
  self->TransitionFromRunnableToSuspended(kNative);
  CollectGarbage(false);
  self->TransitionFromSuspendedToRunnable();

  InstanceCollector collector(c, max_count, instances);
  ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
  GetLiveBitmap()->Visit(collector);
}

class ReferringObjectsFinder {
 public:
  ReferringObjectsFinder(mirror::Object* object, int32_t max_count,
                         std::vector<mirror::Object*>& referring_objects)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : object_(object), max_count_(max_count), referring_objects_(referring_objects) {
  }

  // For bitmap Visit.
  // TODO: Fix lock analysis to not use NO_THREAD_SAFETY_ANALYSIS, requires support for
  // annotalysis on visitors.
  void operator()(const mirror::Object* o) const NO_THREAD_SAFETY_ANALYSIS {
    collector::MarkSweep::VisitObjectReferences(const_cast<mirror::Object*>(o), *this, true);
  }

  // For MarkSweep::VisitObjectReferences.
  void operator()(mirror::Object* referrer, mirror::Object* object,
                  const MemberOffset&, bool) const {
    if (object == object_ && (max_count_ == 0 || referring_objects_.size() < max_count_)) {
      referring_objects_.push_back(referrer);
    }
  }

 private:
  mirror::Object* object_;
  uint32_t max_count_;
  std::vector<mirror::Object*>& referring_objects_;

  DISALLOW_COPY_AND_ASSIGN(ReferringObjectsFinder);
};

void Heap::GetReferringObjects(mirror::Object* o, int32_t max_count,
                               std::vector<mirror::Object*>& referring_objects) {
  // We only want reachable instances, so do a GC. This also ensures that the alloc stack
  // is empty, so the live bitmap is the only place we need to look.
  Thread* self = Thread::Current();
  self->TransitionFromRunnableToSuspended(kNative);
  CollectGarbage(false);
  self->TransitionFromSuspendedToRunnable();

  ReferringObjectsFinder finder(o, max_count, referring_objects);
  ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
  GetLiveBitmap()->Visit(finder);
}

void Heap::CollectGarbage(bool clear_soft_references) {
  // Even if we waited for a GC we still need to do another GC since weaks allocated during the
  // last GC will not have necessarily been cleared.
  CollectGarbageInternal(collector::kGcTypeFull, kGcCauseExplicit, clear_soft_references);
}

void Heap::PreZygoteFork() {
  static Mutex zygote_creation_lock_("zygote creation lock", kZygoteCreationLock);
  Thread* self = Thread::Current();
  MutexLock mu(self, zygote_creation_lock_);
  // Try to see if we have any Zygote spaces.
  if (have_zygote_space_) {
    return;
  }
  VLOG(heap) << "Starting PreZygoteFork";
  // Do this before acquiring the zygote creation lock so that we don't get lock order violations.
  CollectGarbageInternal(collector::kGcTypeFull, kGcCauseBackground, false);
  // Trim the pages at the end of the non moving space.
  non_moving_space_->Trim();
  non_moving_space_->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
  // Create a new bump pointer space which we will compact into.
  if (semi_space_collector_ != nullptr) {
    space::BumpPointerSpace target_space("zygote bump space", non_moving_space_->End(),
                                         non_moving_space_->Limit());
    // Compact the bump pointer space to a new zygote bump pointer space.
    temp_space_->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
    Compact(&target_space, bump_pointer_space_);
    CHECK_EQ(temp_space_->GetBytesAllocated(), 0U);
    total_objects_freed_ever_ += semi_space_collector_->GetFreedObjects();
    total_bytes_freed_ever_ += semi_space_collector_->GetFreedBytes();
    // Update the end and write out image.
    non_moving_space_->SetEnd(target_space.End());
    non_moving_space_->SetLimit(target_space.Limit());
    accounting::SpaceBitmap* bitmap = non_moving_space_->GetLiveBitmap();
    // Record the allocations in the bitmap.
    VLOG(heap) << "Recording zygote allocations";
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(target_space.Begin());
    const mirror::Object* end = reinterpret_cast<const mirror::Object*>(target_space.End());
    while (obj < end) {
      bitmap->Set(obj);
      obj = space::BumpPointerSpace::GetNextObject(obj);
    }
  }
  // Turn the current alloc space into a zygote space and obtain the new alloc space composed of
  // the remaining available heap memory.
  space::MallocSpace* zygote_space = non_moving_space_;
  non_moving_space_ = zygote_space->CreateZygoteSpace("alloc space");
  non_moving_space_->SetFootprintLimit(non_moving_space_->Capacity());
  // Change the GC retention policy of the zygote space to only collect when full.
  zygote_space->SetGcRetentionPolicy(space::kGcRetentionPolicyFullCollect);
  AddSpace(non_moving_space_);
  have_zygote_space_ = true;
  zygote_space->InvalidateAllocator();
  // Create the zygote space mod union table.
  accounting::ModUnionTable* mod_union_table =
      new accounting::ModUnionTableCardCache("zygote space mod-union table", this, zygote_space);
  CHECK(mod_union_table != nullptr) << "Failed to create zygote space mod-union table";
  AddModUnionTable(mod_union_table);
  // Reset the cumulative loggers since we now have a few additional timing phases.
  for (const auto& collector : garbage_collectors_) {
    collector->ResetCumulativeStatistics();
  }
}

void Heap::FlushAllocStack() {
  MarkAllocStack(non_moving_space_->GetLiveBitmap(), large_object_space_->GetLiveObjects(),
                 allocation_stack_.get());
  allocation_stack_->Reset();
}

void Heap::MarkAllocStack(accounting::SpaceBitmap* bitmap, accounting::SpaceSetMap* large_objects,
                          accounting::ObjectStack* stack) {
  mirror::Object** limit = stack->End();
  for (mirror::Object** it = stack->Begin(); it != limit; ++it) {
    const mirror::Object* obj = *it;
    DCHECK(obj != NULL);
    if (LIKELY(bitmap->HasAddress(obj))) {
      bitmap->Set(obj);
    } else {
      large_objects->Set(obj);
    }
  }
}

const char* PrettyCause(GcCause cause) {
  switch (cause) {
    case kGcCauseForAlloc: return "Alloc";
    case kGcCauseBackground: return "Background";
    case kGcCauseExplicit: return "Explicit";
    default:
      LOG(FATAL) << "Unreachable";
  }
  return "";
}

void Heap::SwapSemiSpaces() {
  // Swap the spaces so we allocate into the space which we just evacuated.
  std::swap(bump_pointer_space_, temp_space_);
}

void Heap::Compact(space::ContinuousMemMapAllocSpace* target_space,
                   space::ContinuousMemMapAllocSpace* source_space) {
  CHECK(kMovingCollector);
  CHECK_NE(target_space, source_space) << "In-place compaction unsupported";
  if (target_space != source_space) {
    semi_space_collector_->SetFromSpace(source_space);
    semi_space_collector_->SetToSpace(target_space);
    semi_space_collector_->Run(false);
  }
}

collector::GcType Heap::CollectGarbageInternal(collector::GcType gc_type, GcCause gc_cause,
                                               bool clear_soft_references) {
  Thread* self = Thread::Current();
  Runtime* runtime = Runtime::Current();
  // If the heap can't run the GC, silently fail and return that no GC was run.
  switch (gc_type) {
    case collector::kGcTypeSticky: {
      const size_t alloc_space_size = non_moving_space_->Size();
      if (alloc_space_size < min_alloc_space_size_for_sticky_gc_ ||
        non_moving_space_->Capacity() - alloc_space_size < min_remaining_space_for_sticky_gc_) {
        return collector::kGcTypeNone;
      }
      break;
    }
    case collector::kGcTypePartial: {
      if (!have_zygote_space_) {
        return collector::kGcTypeNone;
      }
      break;
    }
    default: {
      // Other GC types don't have any special cases which makes them not runnable. The main case
      // here is full GC.
    }
  }
  ScopedThreadStateChange tsc(self, kWaitingPerformingGc);
  Locks::mutator_lock_->AssertNotHeld(self);
  if (self->IsHandlingStackOverflow()) {
    LOG(WARNING) << "Performing GC on a thread that is handling a stack overflow.";
  }
  {
    gc_complete_lock_->AssertNotHeld(self);
    MutexLock mu(self, *gc_complete_lock_);
    // Ensure there is only one GC at a time.
    WaitForGcToCompleteLocked(self);
    // TODO: if another thread beat this one to do the GC, perhaps we should just return here?
    //       Not doing at the moment to ensure soft references are cleared.
    // GC can be disabled if someone has a used GetPrimitiveArrayCritical.
    if (gc_disable_count_ != 0) {
      LOG(WARNING) << "Skipping GC due to disable count " << gc_disable_count_;
      return collector::kGcTypeNone;
    }
    is_gc_running_ = true;
  }
  if (gc_cause == kGcCauseForAlloc && runtime->HasStatsEnabled()) {
    ++runtime->GetStats()->gc_for_alloc_count;
    ++self->GetStats()->gc_for_alloc_count;
  }
  uint64_t gc_start_time_ns = NanoTime();
  uint64_t gc_start_size = GetBytesAllocated();
  // Approximate allocation rate in bytes / second.
  uint64_t ms_delta = NsToMs(gc_start_time_ns - last_gc_time_ns_);
  // Back to back GCs can cause 0 ms of wait time in between GC invocations.
  if (LIKELY(ms_delta != 0)) {
    allocation_rate_ = ((gc_start_size - last_gc_size_) * 1000) / ms_delta;
    VLOG(heap) << "Allocation rate: " << PrettySize(allocation_rate_) << "/s";
  }

  DCHECK_LT(gc_type, collector::kGcTypeMax);
  DCHECK_NE(gc_type, collector::kGcTypeNone);

  collector::GarbageCollector* collector = nullptr;
  if (kMovingCollector) {
    gc_type = semi_space_collector_->GetGcType();
    CHECK_EQ(temp_space_->GetObjectsAllocated(), 0U);
    semi_space_collector_->SetFromSpace(bump_pointer_space_);
    semi_space_collector_->SetToSpace(temp_space_);
    mprotect(temp_space_->Begin(), temp_space_->Capacity(), PROT_READ | PROT_WRITE);
  }
  for (const auto& cur_collector : garbage_collectors_) {
    if (cur_collector->IsConcurrent() == concurrent_gc_ &&
        cur_collector->GetGcType() == gc_type) {
      collector = cur_collector;
      break;
    }
  }
  if (kMovingCollector) {
    gc_type = collector::kGcTypeFull;
  }
  CHECK(collector != NULL)
      << "Could not find garbage collector with concurrent=" << concurrent_gc_
      << " and type=" << gc_type;

  ATRACE_BEGIN(StringPrintf("%s %s GC", PrettyCause(gc_cause), collector->GetName()).c_str());

  collector->Run(clear_soft_references);
  total_objects_freed_ever_ += collector->GetFreedObjects();
  total_bytes_freed_ever_ += collector->GetFreedBytes();

  // Enqueue cleared references.
  EnqueueClearedReferences();

  // Grow the heap so that we know when to perform the next GC.
  GrowForUtilization(gc_type, collector->GetDurationNs());

  if (care_about_pause_times_) {
    const size_t duration = collector->GetDurationNs();
    std::vector<uint64_t> pauses = collector->GetPauseTimes();
    // GC for alloc pauses the allocating thread, so consider it as a pause.
    bool was_slow = duration > long_gc_log_threshold_ ||
            (gc_cause == kGcCauseForAlloc && duration > long_pause_log_threshold_);
    if (!was_slow) {
      for (uint64_t pause : pauses) {
        was_slow = was_slow || pause > long_pause_log_threshold_;
      }
    }
    if (was_slow) {
        const size_t percent_free = GetPercentFree();
        const size_t current_heap_size = GetBytesAllocated();
        const size_t total_memory = GetTotalMemory();
        std::ostringstream pause_string;
        for (size_t i = 0; i < pauses.size(); ++i) {
            pause_string << PrettyDuration((pauses[i] / 1000) * 1000)
                         << ((i != pauses.size() - 1) ? ", " : "");
        }
        LOG(INFO) << gc_cause << " " << collector->GetName()
                  << " GC freed "  <<  collector->GetFreedObjects() << "("
                  << PrettySize(collector->GetFreedBytes()) << ") AllocSpace objects, "
                  << collector->GetFreedLargeObjects() << "("
                  << PrettySize(collector->GetFreedLargeObjectBytes()) << ") LOS objects, "
                  << percent_free << "% free, " << PrettySize(current_heap_size) << "/"
                  << PrettySize(total_memory) << ", " << "paused " << pause_string.str()
                  << " total " << PrettyDuration((duration / 1000) * 1000);
        if (VLOG_IS_ON(heap)) {
            LOG(INFO) << Dumpable<TimingLogger>(collector->GetTimings());
        }
    }
  }

  {
      MutexLock mu(self, *gc_complete_lock_);
      is_gc_running_ = false;
      last_gc_type_ = gc_type;
      // Wake anyone who may have been waiting for the GC to complete.
      gc_complete_cond_->Broadcast(self);
  }

  ATRACE_END();

  // Inform DDMS that a GC completed.
  Dbg::GcDidFinish();
  return gc_type;
}

static mirror::Object* RootMatchesObjectVisitor(mirror::Object* root, void* arg) {
  mirror::Object* obj = reinterpret_cast<mirror::Object*>(arg);
  if (root == obj) {
    LOG(INFO) << "Object " << obj << " is a root";
  }
  return root;
}

class ScanVisitor {
 public:
  void operator()(const mirror::Object* obj) const {
    LOG(ERROR) << "Would have rescanned object " << obj;
  }
};

// Verify a reference from an object.
class VerifyReferenceVisitor {
 public:
  explicit VerifyReferenceVisitor(Heap* heap)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_)
      : heap_(heap), failed_(false) {}

  bool Failed() const {
    return failed_;
  }

  // TODO: Fix lock analysis to not use NO_THREAD_SAFETY_ANALYSIS, requires support for smarter
  // analysis on visitors.
  void operator()(const mirror::Object* obj, const mirror::Object* ref,
                  const MemberOffset& offset, bool /* is_static */) const
      NO_THREAD_SAFETY_ANALYSIS {
    // Verify that the reference is live.
    if (UNLIKELY(ref != NULL && !IsLive(ref))) {
      accounting::CardTable* card_table = heap_->GetCardTable();
      accounting::ObjectStack* alloc_stack = heap_->allocation_stack_.get();
      accounting::ObjectStack* live_stack = heap_->live_stack_.get();
      if (!failed_) {
        // Print message on only on first failure to prevent spam.
        LOG(ERROR) << "!!!!!!!!!!!!!!Heap corruption detected!!!!!!!!!!!!!!!!!!!";
        failed_ = true;
      }
      if (obj != nullptr) {
        byte* card_addr = card_table->CardFromAddr(obj);
        LOG(ERROR) << "Object " << obj << " references dead object " << ref << " at offset "
                   << offset << "\n card value = " << static_cast<int>(*card_addr);
        if (heap_->IsValidObjectAddress(obj->GetClass())) {
          LOG(ERROR) << "Obj type " << PrettyTypeOf(obj);
        } else {
          LOG(ERROR) << "Object " << obj << " class(" << obj->GetClass() << ") not a heap address";
        }

        // Attmept to find the class inside of the recently freed objects.
        space::ContinuousSpace* ref_space = heap_->FindContinuousSpaceFromObject(ref, true);
        if (ref_space != nullptr && ref_space->IsMallocSpace()) {
          space::MallocSpace* space = ref_space->AsMallocSpace();
          mirror::Class* ref_class = space->FindRecentFreedObject(ref);
          if (ref_class != nullptr) {
            LOG(ERROR) << "Reference " << ref << " found as a recently freed object with class "
                       << PrettyClass(ref_class);
          } else {
            LOG(ERROR) << "Reference " << ref << " not found as a recently freed object";
          }
        }

        if (ref->GetClass() != nullptr && heap_->IsValidObjectAddress(ref->GetClass()) &&
            ref->GetClass()->IsClass()) {
          LOG(ERROR) << "Ref type " << PrettyTypeOf(ref);
        } else {
          LOG(ERROR) << "Ref " << ref << " class(" << ref->GetClass()
                     << ") is not a valid heap address";
        }

        card_table->CheckAddrIsInCardTable(reinterpret_cast<const byte*>(obj));
        void* cover_begin = card_table->AddrFromCard(card_addr);
        void* cover_end = reinterpret_cast<void*>(reinterpret_cast<size_t>(cover_begin) +
            accounting::CardTable::kCardSize);
        LOG(ERROR) << "Card " << reinterpret_cast<void*>(card_addr) << " covers " << cover_begin
            << "-" << cover_end;
        accounting::SpaceBitmap* bitmap = heap_->GetLiveBitmap()->GetContinuousSpaceBitmap(obj);

        // Print out how the object is live.
        if (bitmap != NULL && bitmap->Test(obj)) {
          LOG(ERROR) << "Object " << obj << " found in live bitmap";
        }
        if (alloc_stack->Contains(const_cast<mirror::Object*>(obj))) {
          LOG(ERROR) << "Object " << obj << " found in allocation stack";
        }
        if (live_stack->Contains(const_cast<mirror::Object*>(obj))) {
          LOG(ERROR) << "Object " << obj << " found in live stack";
        }
        if (alloc_stack->Contains(const_cast<mirror::Object*>(ref))) {
          LOG(ERROR) << "Ref " << ref << " found in allocation stack";
        }
        if (live_stack->Contains(const_cast<mirror::Object*>(ref))) {
          LOG(ERROR) << "Ref " << ref << " found in live stack";
        }
        // Attempt to see if the card table missed the reference.
        ScanVisitor scan_visitor;
        byte* byte_cover_begin = reinterpret_cast<byte*>(card_table->AddrFromCard(card_addr));
        card_table->Scan(bitmap, byte_cover_begin,
                         byte_cover_begin + accounting::CardTable::kCardSize, scan_visitor);

        // Search to see if any of the roots reference our object.
        void* arg = const_cast<void*>(reinterpret_cast<const void*>(obj));
        Runtime::Current()->VisitRoots(&RootMatchesObjectVisitor, arg, false, false);

        // Search to see if any of the roots reference our reference.
        arg = const_cast<void*>(reinterpret_cast<const void*>(ref));
        Runtime::Current()->VisitRoots(&RootMatchesObjectVisitor, arg, false, false);
      } else {
        LOG(ERROR) << "Root references dead object " << ref << "\nRef type " << PrettyTypeOf(ref);
      }
    }
  }

  bool IsLive(const mirror::Object* obj) const NO_THREAD_SAFETY_ANALYSIS {
    return heap_->IsLiveObjectLocked(obj, true, false, true);
  }

  static mirror::Object* VerifyRoots(mirror::Object* root, void* arg) {
    VerifyReferenceVisitor* visitor = reinterpret_cast<VerifyReferenceVisitor*>(arg);
    (*visitor)(nullptr, root, MemberOffset(0), true);
    return root;
  }

 private:
  Heap* const heap_;
  mutable bool failed_;
};

// Verify all references within an object, for use with HeapBitmap::Visit.
class VerifyObjectVisitor {
 public:
  explicit VerifyObjectVisitor(Heap* heap) : heap_(heap), failed_(false) {}

  void operator()(mirror::Object* obj) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    // Note: we are verifying the references in obj but not obj itself, this is because obj must
    // be live or else how did we find it in the live bitmap?
    VerifyReferenceVisitor visitor(heap_);
    // The class doesn't count as a reference but we should verify it anyways.
    collector::MarkSweep::VisitObjectReferences(obj, visitor, true);
    if (obj->GetClass()->IsReferenceClass()) {
      visitor(obj, heap_->GetReferenceReferent(obj), MemberOffset(0), false);
    }
    failed_ = failed_ || visitor.Failed();
  }

  static void VisitCallback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    VerifyObjectVisitor* visitor = reinterpret_cast<VerifyObjectVisitor*>(arg);
    visitor->operator()(obj);
  }

  bool Failed() const {
    return failed_;
  }

 private:
  Heap* const heap_;
  mutable bool failed_;
};

// Must do this with mutators suspended since we are directly accessing the allocation stacks.
bool Heap::VerifyHeapReferences() {
  Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());
  // Lets sort our allocation stacks so that we can efficiently binary search them.
  allocation_stack_->Sort();
  live_stack_->Sort();
  VerifyObjectVisitor visitor(this);
  // Verify objects in the allocation stack since these will be objects which were:
  // 1. Allocated prior to the GC (pre GC verification).
  // 2. Allocated during the GC (pre sweep GC verification).
  // We don't want to verify the objects in the live stack since they themselves may be
  // pointing to dead objects if they are not reachable.
  VisitObjects(VerifyObjectVisitor::VisitCallback, &visitor);
  // Verify the roots:
  Runtime::Current()->VisitRoots(VerifyReferenceVisitor::VerifyRoots, &visitor, false, false);
  if (visitor.Failed()) {
    // Dump mod-union tables.
    for (const auto& table_pair : mod_union_tables_) {
      accounting::ModUnionTable* mod_union_table = table_pair.second;
      mod_union_table->Dump(LOG(ERROR) << mod_union_table->GetName() << ": ");
    }
    DumpSpaces();
    return false;
  }
  return true;
}

class VerifyReferenceCardVisitor {
 public:
  VerifyReferenceCardVisitor(Heap* heap, bool* failed)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_,
                            Locks::heap_bitmap_lock_)
      : heap_(heap), failed_(failed) {
  }

  // TODO: Fix lock analysis to not use NO_THREAD_SAFETY_ANALYSIS, requires support for
  // annotalysis on visitors.
  void operator()(const mirror::Object* obj, const mirror::Object* ref, const MemberOffset& offset,
                  bool is_static) const NO_THREAD_SAFETY_ANALYSIS {
    // Filter out class references since changing an object's class does not mark the card as dirty.
    // Also handles large objects, since the only reference they hold is a class reference.
    if (ref != NULL && !ref->IsClass()) {
      accounting::CardTable* card_table = heap_->GetCardTable();
      // If the object is not dirty and it is referencing something in the live stack other than
      // class, then it must be on a dirty card.
      if (!card_table->AddrIsInCardTable(obj)) {
        LOG(ERROR) << "Object " << obj << " is not in the address range of the card table";
        *failed_ = true;
      } else if (!card_table->IsDirty(obj)) {
        // Card should be either kCardDirty if it got re-dirtied after we aged it, or
        // kCardDirty - 1 if it didnt get touched since we aged it.
        accounting::ObjectStack* live_stack = heap_->live_stack_.get();
        if (live_stack->ContainsSorted(const_cast<mirror::Object*>(ref))) {
          if (live_stack->ContainsSorted(const_cast<mirror::Object*>(obj))) {
            LOG(ERROR) << "Object " << obj << " found in live stack";
          }
          if (heap_->GetLiveBitmap()->Test(obj)) {
            LOG(ERROR) << "Object " << obj << " found in live bitmap";
          }
          LOG(ERROR) << "Object " << obj << " " << PrettyTypeOf(obj)
                    << " references " << ref << " " << PrettyTypeOf(ref) << " in live stack";

          // Print which field of the object is dead.
          if (!obj->IsObjectArray()) {
            const mirror::Class* klass = is_static ? obj->AsClass() : obj->GetClass();
            CHECK(klass != NULL);
            const mirror::ObjectArray<mirror::ArtField>* fields = is_static ? klass->GetSFields()
                                                                            : klass->GetIFields();
            CHECK(fields != NULL);
            for (int32_t i = 0; i < fields->GetLength(); ++i) {
              const mirror::ArtField* cur = fields->Get(i);
              if (cur->GetOffset().Int32Value() == offset.Int32Value()) {
                LOG(ERROR) << (is_static ? "Static " : "") << "field in the live stack is "
                          << PrettyField(cur);
                break;
              }
            }
          } else {
            const mirror::ObjectArray<mirror::Object>* object_array =
                obj->AsObjectArray<mirror::Object>();
            for (int32_t i = 0; i < object_array->GetLength(); ++i) {
              if (object_array->Get(i) == ref) {
                LOG(ERROR) << (is_static ? "Static " : "") << "obj[" << i << "] = ref";
              }
            }
          }

          *failed_ = true;
        }
      }
    }
  }

 private:
  Heap* const heap_;
  bool* const failed_;
};

class VerifyLiveStackReferences {
 public:
  explicit VerifyLiveStackReferences(Heap* heap)
      : heap_(heap),
        failed_(false) {}

  void operator()(mirror::Object* obj) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    VerifyReferenceCardVisitor visitor(heap_, const_cast<bool*>(&failed_));
    collector::MarkSweep::VisitObjectReferences(const_cast<mirror::Object*>(obj), visitor, true);
  }

  bool Failed() const {
    return failed_;
  }

 private:
  Heap* const heap_;
  bool failed_;
};

bool Heap::VerifyMissingCardMarks() {
  Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());

  // We need to sort the live stack since we binary search it.
  live_stack_->Sort();
  VerifyLiveStackReferences visitor(this);
  GetLiveBitmap()->Visit(visitor);

  // We can verify objects in the live stack since none of these should reference dead objects.
  for (mirror::Object** it = live_stack_->Begin(); it != live_stack_->End(); ++it) {
    visitor(*it);
  }

  if (visitor.Failed()) {
    DumpSpaces();
    return false;
  }
  return true;
}

void Heap::SwapStacks() {
  allocation_stack_.swap(live_stack_);
}

accounting::ModUnionTable* Heap::FindModUnionTableFromSpace(space::Space* space) {
  auto it = mod_union_tables_.find(space);
  if (it == mod_union_tables_.end()) {
    return nullptr;
  }
  return it->second;
}

void Heap::ProcessCards(TimingLogger& timings) {
  // Clear cards and keep track of cards cleared in the mod-union table.
  for (const auto& space : continuous_spaces_) {
    accounting::ModUnionTable* table = FindModUnionTableFromSpace(space);
    if (table != nullptr) {
      const char* name = space->IsZygoteSpace() ? "ZygoteModUnionClearCards" :
          "ImageModUnionClearCards";
      TimingLogger::ScopedSplit split(name, &timings);
      table->ClearCards();
    } else if (space->GetType() != space::kSpaceTypeBumpPointerSpace) {
      TimingLogger::ScopedSplit split("AllocSpaceClearCards", &timings);
      // No mod union table for the AllocSpace. Age the cards so that the GC knows that these cards
      // were dirty before the GC started.
      // TODO: Don't need to use atomic.
      // The races are we either end up with: Aged card, unaged card. Since we have the checkpoint
      // roots and then we scan / update mod union tables after. We will always scan either card.//
      // If we end up with the non aged card, we scan it it in the pause.
      card_table_->ModifyCardsAtomic(space->Begin(), space->End(), AgeCardVisitor(), VoidFunctor());
    }
  }
}

static mirror::Object* IdentityCallback(mirror::Object* obj, void*) {
  return obj;
}

void Heap::PreGcVerification(collector::GarbageCollector* gc) {
  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  Thread* self = Thread::Current();

  if (verify_pre_gc_heap_) {
    thread_list->SuspendAll();
    {
      ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
      if (!VerifyHeapReferences()) {
        LOG(FATAL) << "Pre " << gc->GetName() << " heap verification failed";
      }
    }
    thread_list->ResumeAll();
  }

  // Check that all objects which reference things in the live stack are on dirty cards.
  if (verify_missing_card_marks_) {
    thread_list->SuspendAll();
    {
      ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
      SwapStacks();
      // Sort the live stack so that we can quickly binary search it later.
      if (!VerifyMissingCardMarks()) {
        LOG(FATAL) << "Pre " << gc->GetName() << " missing card mark verification failed";
      }
      SwapStacks();
    }
    thread_list->ResumeAll();
  }

  if (verify_mod_union_table_) {
    thread_list->SuspendAll();
    ReaderMutexLock reader_lock(self, *Locks::heap_bitmap_lock_);
    for (const auto& table_pair : mod_union_tables_) {
      accounting::ModUnionTable* mod_union_table = table_pair.second;
      mod_union_table->UpdateAndMarkReferences(IdentityCallback, nullptr);
      mod_union_table->Verify();
    }
    thread_list->ResumeAll();
  }
}

void Heap::PreSweepingGcVerification(collector::GarbageCollector* gc) {
  // Called before sweeping occurs since we want to make sure we are not going so reclaim any
  // reachable objects.
  if (verify_post_gc_heap_) {
    Thread* self = Thread::Current();
    CHECK_NE(self->GetState(), kRunnable);
    {
      WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
      // Swapping bound bitmaps does nothing.
      gc->SwapBitmaps();
      if (!VerifyHeapReferences()) {
        LOG(FATAL) << "Pre sweeping " << gc->GetName() << " GC verification failed";
      }
      gc->SwapBitmaps();
    }
  }
}

void Heap::PostGcVerification(collector::GarbageCollector* gc) {
  if (verify_system_weaks_) {
    Thread* self = Thread::Current();
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    collector::MarkSweep* mark_sweep = down_cast<collector::MarkSweep*>(gc);
    mark_sweep->VerifySystemWeaks();
  }
}

collector::GcType Heap::WaitForGcToComplete(Thread* self) {
  ScopedThreadStateChange tsc(self, kWaitingForGcToComplete);
  MutexLock mu(self, *gc_complete_lock_);
  return WaitForGcToCompleteLocked(self);
}

collector::GcType Heap::WaitForGcToCompleteLocked(Thread* self) {
  collector::GcType last_gc_type = collector::kGcTypeNone;
  uint64_t wait_start = NanoTime();
  while (is_gc_running_) {
    ATRACE_BEGIN("GC: Wait For Completion");
    // We must wait, change thread state then sleep on gc_complete_cond_;
    gc_complete_cond_->Wait(self);
    last_gc_type = last_gc_type_;
    ATRACE_END();
  }
  uint64_t wait_time = NanoTime() - wait_start;
  total_wait_time_ += wait_time;
  if (wait_time > long_pause_log_threshold_) {
    LOG(INFO) << "WaitForGcToComplete blocked for " << PrettyDuration(wait_time);
  }
  return last_gc_type;
}

void Heap::DumpForSigQuit(std::ostream& os) {
  os << "Heap: " << GetPercentFree() << "% free, " << PrettySize(GetBytesAllocated()) << "/"
     << PrettySize(GetTotalMemory()) << "; " << GetObjectsAllocated() << " objects\n";
  DumpGcPerformanceInfo(os);
}

size_t Heap::GetPercentFree() {
  return static_cast<size_t>(100.0f * static_cast<float>(GetFreeMemory()) / GetTotalMemory());
}

void Heap::SetIdealFootprint(size_t max_allowed_footprint) {
  if (max_allowed_footprint > GetMaxMemory()) {
    VLOG(gc) << "Clamp target GC heap from " << PrettySize(max_allowed_footprint) << " to "
             << PrettySize(GetMaxMemory());
    max_allowed_footprint = GetMaxMemory();
  }
  max_allowed_footprint_ = max_allowed_footprint;
}

bool Heap::IsMovableObject(const mirror::Object* obj) const {
  if (kMovingCollector) {
    DCHECK(!IsInTempSpace(obj));
    if (bump_pointer_space_->HasAddress(obj)) {
      return true;
    }
  }
  return false;
}

bool Heap::IsInTempSpace(const mirror::Object* obj) const {
  if (temp_space_->HasAddress(obj) && !temp_space_->Contains(obj)) {
    return true;
  }
  return false;
}

void Heap::UpdateMaxNativeFootprint() {
  size_t native_size = native_bytes_allocated_;
  // TODO: Tune the native heap utilization to be a value other than the java heap utilization.
  size_t target_size = native_size / GetTargetHeapUtilization();
  if (target_size > native_size + max_free_) {
    target_size = native_size + max_free_;
  } else if (target_size < native_size + min_free_) {
    target_size = native_size + min_free_;
  }
  native_footprint_gc_watermark_ = target_size;
  native_footprint_limit_ = 2 * target_size - native_size;
}

void Heap::GrowForUtilization(collector::GcType gc_type, uint64_t gc_duration) {
  // We know what our utilization is at this moment.
  // This doesn't actually resize any memory. It just lets the heap grow more when necessary.
  const size_t bytes_allocated = GetBytesAllocated();
  last_gc_size_ = bytes_allocated;
  last_gc_time_ns_ = NanoTime();

  size_t target_size;
  if (gc_type != collector::kGcTypeSticky) {
    // Grow the heap for non sticky GC.
    target_size = bytes_allocated / GetTargetHeapUtilization();
    if (target_size > bytes_allocated + max_free_) {
      target_size = bytes_allocated + max_free_;
    } else if (target_size < bytes_allocated + min_free_) {
      target_size = bytes_allocated + min_free_;
    }
    native_need_to_run_finalization_ = true;
    next_gc_type_ = collector::kGcTypeSticky;
  } else {
    // Based on how close the current heap size is to the target size, decide
    // whether or not to do a partial or sticky GC next.
    if (bytes_allocated + min_free_ <= max_allowed_footprint_) {
      next_gc_type_ = collector::kGcTypeSticky;
    } else {
      next_gc_type_ = collector::kGcTypePartial;
    }

    // If we have freed enough memory, shrink the heap back down.
    if (bytes_allocated + max_free_ < max_allowed_footprint_) {
      target_size = bytes_allocated + max_free_;
    } else {
      target_size = std::max(bytes_allocated, max_allowed_footprint_);
    }
  }

  if (!ignore_max_footprint_) {
    SetIdealFootprint(target_size);

    if (concurrent_gc_) {
      // Calculate when to perform the next ConcurrentGC.
      // Calculate the estimated GC duration.
      double gc_duration_seconds = NsToMs(gc_duration) / 1000.0;
      // Estimate how many remaining bytes we will have when we need to start the next GC.
      size_t remaining_bytes = allocation_rate_ * gc_duration_seconds;
      remaining_bytes = std::max(remaining_bytes, kMinConcurrentRemainingBytes);
      if (UNLIKELY(remaining_bytes > max_allowed_footprint_)) {
        // A never going to happen situation that from the estimated allocation rate we will exceed
        // the applications entire footprint with the given estimated allocation rate. Schedule
        // another GC straight away.
        concurrent_start_bytes_ = bytes_allocated;
      } else {
        // Start a concurrent GC when we get close to the estimated remaining bytes. When the
        // allocation rate is very high, remaining_bytes could tell us that we should start a GC
        // right away.
        concurrent_start_bytes_ = std::max(max_allowed_footprint_ - remaining_bytes, bytes_allocated);
      }
      DCHECK_LE(concurrent_start_bytes_, max_allowed_footprint_);
      DCHECK_LE(max_allowed_footprint_, growth_limit_);
    }
  }
}

void Heap::ClearGrowthLimit() {
  growth_limit_ = capacity_;
  non_moving_space_->ClearGrowthLimit();
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

void Heap::SetReferenceReferent(mirror::Object* reference, mirror::Object* referent) {
  DCHECK(reference != NULL);
  DCHECK_NE(reference_referent_offset_.Uint32Value(), 0U);
  reference->SetFieldObject(reference_referent_offset_, referent, true);
}

mirror::Object* Heap::GetReferenceReferent(mirror::Object* reference) {
  DCHECK(reference != NULL);
  DCHECK_NE(reference_referent_offset_.Uint32Value(), 0U);
  return reference->GetFieldObject<mirror::Object*>(reference_referent_offset_, true);
}

void Heap::AddFinalizerReference(Thread* self, mirror::Object* object) {
  ScopedObjectAccess soa(self);
  JValue result;
  ArgArray arg_array(NULL, 0);
  arg_array.Append(reinterpret_cast<uint32_t>(object));
  soa.DecodeMethod(WellKnownClasses::java_lang_ref_FinalizerReference_add)->Invoke(self,
      arg_array.GetArray(), arg_array.GetNumBytes(), &result, 'V');
}

void Heap::PrintReferenceQueue(std::ostream& os, mirror::Object** queue) {
  os << "Refernece queue " << queue << "\n";
  if (queue != nullptr) {
    mirror::Object* list = *queue;
    if (list != nullptr) {
      mirror::Object* cur = list;
      do {
        mirror::Object* pending_next =
            cur->GetFieldObject<mirror::Object*>(reference_pendingNext_offset_, false);
        os << "PendingNext=" << pending_next;
        if (cur->GetClass()->IsFinalizerReferenceClass()) {
          os << " Zombie=" <<
              cur->GetFieldObject<mirror::Object*>(finalizer_reference_zombie_offset_, false);
        }
        os << "\n";
        cur = pending_next;
      } while (cur != list);
    }
  }
}

void Heap::EnqueueClearedReferences() {
  if (!cleared_references_.IsEmpty()) {
    // When a runtime isn't started there are no reference queues to care about so ignore.
    if (LIKELY(Runtime::Current()->IsStarted())) {
      ScopedObjectAccess soa(Thread::Current());
      JValue result;
      ArgArray arg_array(NULL, 0);
      arg_array.Append(reinterpret_cast<uint32_t>(cleared_references_.GetList()));
      soa.DecodeMethod(WellKnownClasses::java_lang_ref_ReferenceQueue_add)->Invoke(soa.Self(),
          arg_array.GetArray(), arg_array.GetNumBytes(), &result, 'V');
    }
    cleared_references_.Clear();
  }
}

void Heap::RequestConcurrentGC(Thread* self) {
  // Make sure that we can do a concurrent GC.
  Runtime* runtime = Runtime::Current();
  DCHECK(concurrent_gc_);
  if (runtime == NULL || !runtime->IsFinishedStarting() || runtime->IsShuttingDown(self) ||
      self->IsHandlingStackOverflow()) {
    return;
  }
  // We already have a request pending, no reason to start more until we update
  // concurrent_start_bytes_.
  concurrent_start_bytes_ = std::numeric_limits<size_t>::max();
  JNIEnv* env = self->GetJniEnv();
  DCHECK(WellKnownClasses::java_lang_Daemons != nullptr);
  DCHECK(WellKnownClasses::java_lang_Daemons_requestGC != nullptr);
  env->CallStaticVoidMethod(WellKnownClasses::java_lang_Daemons,
                            WellKnownClasses::java_lang_Daemons_requestGC);
  CHECK(!env->ExceptionCheck());
}

void Heap::ConcurrentGC(Thread* self) {
  if (Runtime::Current()->IsShuttingDown(self)) {
    return;
  }
  // Wait for any GCs currently running to finish.
  if (WaitForGcToComplete(self) == collector::kGcTypeNone) {
    CollectGarbageInternal(next_gc_type_, kGcCauseBackground, false);
  }
}

void Heap::RequestHeapTrim() {
  // GC completed and now we must decide whether to request a heap trim (advising pages back to the
  // kernel) or not. Issuing a request will also cause trimming of the libc heap. As a trim scans
  // a space it will hold its lock and can become a cause of jank.
  // Note, the large object space self trims and the Zygote space was trimmed and unchanging since
  // forking.

  // We don't have a good measure of how worthwhile a trim might be. We can't use the live bitmap
  // because that only marks object heads, so a large array looks like lots of empty space. We
  // don't just call dlmalloc all the time, because the cost of an _attempted_ trim is proportional
  // to utilization (which is probably inversely proportional to how much benefit we can expect).
  // We could try mincore(2) but that's only a measure of how many pages we haven't given away,
  // not how much use we're making of those pages.
  uint64_t ms_time = MilliTime();
  // Don't bother trimming the alloc space if a heap trim occurred in the last two seconds.
  if (ms_time - last_trim_time_ms_ < 2 * 1000) {
    return;
  }

  Thread* self = Thread::Current();
  Runtime* runtime = Runtime::Current();
  if (runtime == nullptr || !runtime->IsFinishedStarting() || runtime->IsShuttingDown(self)) {
    // Heap trimming isn't supported without a Java runtime or Daemons (such as at dex2oat time)
    // Also: we do not wish to start a heap trim if the runtime is shutting down (a racy check
    // as we don't hold the lock while requesting the trim).
    return;
  }

  last_trim_time_ms_ = ms_time;
  ListenForProcessStateChange();

  // Trim only if we do not currently care about pause times.
  if (!care_about_pause_times_) {
    JNIEnv* env = self->GetJniEnv();
    DCHECK(WellKnownClasses::java_lang_Daemons != NULL);
    DCHECK(WellKnownClasses::java_lang_Daemons_requestHeapTrim != NULL);
    env->CallStaticVoidMethod(WellKnownClasses::java_lang_Daemons,
                              WellKnownClasses::java_lang_Daemons_requestHeapTrim);
    CHECK(!env->ExceptionCheck());
  }
}

void Heap::RevokeThreadLocalBuffers(Thread* thread) {
  non_moving_space_->RevokeThreadLocalBuffers(thread);
}

void Heap::RevokeAllThreadLocalBuffers() {
  non_moving_space_->RevokeAllThreadLocalBuffers();
}

bool Heap::IsGCRequestPending() const {
  return concurrent_start_bytes_ != std::numeric_limits<size_t>::max();
}

void Heap::RunFinalization(JNIEnv* env) {
  // Can't do this in WellKnownClasses::Init since System is not properly set up at that point.
  if (WellKnownClasses::java_lang_System_runFinalization == nullptr) {
    CHECK(WellKnownClasses::java_lang_System != nullptr);
    WellKnownClasses::java_lang_System_runFinalization =
        CacheMethod(env, WellKnownClasses::java_lang_System, true, "runFinalization", "()V");
    CHECK(WellKnownClasses::java_lang_System_runFinalization != nullptr);
  }
  env->CallStaticVoidMethod(WellKnownClasses::java_lang_System,
                            WellKnownClasses::java_lang_System_runFinalization);
}

void Heap::RegisterNativeAllocation(JNIEnv* env, int bytes) {
  Thread* self = ThreadForEnv(env);
  if (native_need_to_run_finalization_) {
    RunFinalization(env);
    UpdateMaxNativeFootprint();
    native_need_to_run_finalization_ = false;
  }
  // Total number of native bytes allocated.
  native_bytes_allocated_.fetch_add(bytes);
  if (static_cast<size_t>(native_bytes_allocated_) > native_footprint_gc_watermark_) {
    collector::GcType gc_type = have_zygote_space_ ? collector::kGcTypePartial :
        collector::kGcTypeFull;

    // The second watermark is higher than the gc watermark. If you hit this it means you are
    // allocating native objects faster than the GC can keep up with.
    if (static_cast<size_t>(native_bytes_allocated_) > native_footprint_limit_) {
      if (WaitForGcToComplete(self) != collector::kGcTypeNone) {
        // Just finished a GC, attempt to run finalizers.
        RunFinalization(env);
        CHECK(!env->ExceptionCheck());
      }
      // If we still are over the watermark, attempt a GC for alloc and run finalizers.
      if (static_cast<size_t>(native_bytes_allocated_) > native_footprint_limit_) {
        CollectGarbageInternal(gc_type, kGcCauseForAlloc, false);
        RunFinalization(env);
        native_need_to_run_finalization_ = false;
        CHECK(!env->ExceptionCheck());
      }
      // We have just run finalizers, update the native watermark since it is very likely that
      // finalizers released native managed allocations.
      UpdateMaxNativeFootprint();
    } else if (!IsGCRequestPending()) {
      if (concurrent_gc_) {
        RequestConcurrentGC(self);
      } else {
        CollectGarbageInternal(gc_type, kGcCauseForAlloc, false);
      }
    }
  }
}

void Heap::RegisterNativeFree(JNIEnv* env, int bytes) {
  int expected_size, new_size;
  do {
    expected_size = native_bytes_allocated_.load();
    new_size = expected_size - bytes;
    if (UNLIKELY(new_size < 0)) {
      ScopedObjectAccess soa(env);
      env->ThrowNew(WellKnownClasses::java_lang_RuntimeException,
                    StringPrintf("Attempted to free %d native bytes with only %d native bytes "
                                 "registered as allocated", bytes, expected_size).c_str());
      break;
    }
  } while (!native_bytes_allocated_.compare_and_swap(expected_size, new_size));
}

int64_t Heap::GetTotalMemory() const {
  int64_t ret = 0;
  for (const auto& space : continuous_spaces_) {
    // Currently don't include the image space.
    if (!space->IsImageSpace()) {
      ret += space->Size();
    }
  }
  for (const auto& space : discontinuous_spaces_) {
    if (space->IsLargeObjectSpace()) {
      ret += space->AsLargeObjectSpace()->GetBytesAllocated();
    }
  }
  return ret;
}

void Heap::AddModUnionTable(accounting::ModUnionTable* mod_union_table) {
  DCHECK(mod_union_table != nullptr);
  mod_union_tables_.Put(mod_union_table->GetSpace(), mod_union_table);
}

}  // namespace gc
}  // namespace art
