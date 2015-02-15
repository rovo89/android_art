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
#include <memory>
#include <vector>

#include "base/allocator.h"
#include "base/histogram-inl.h"
#include "base/stl_util.h"
#include "common_throws.h"
#include "cutils/sched_policy.h"
#include "debugger.h"
#include "gc/accounting/atomic_stack.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/heap_bitmap-inl.h"
#include "gc/accounting/mod_union_table.h"
#include "gc/accounting/mod_union_table-inl.h"
#include "gc/accounting/remembered_set.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/collector/concurrent_copying.h"
#include "gc/collector/mark_compact.h"
#include "gc/collector/mark_sweep-inl.h"
#include "gc/collector/partial_mark_sweep.h"
#include "gc/collector/semi_space.h"
#include "gc/collector/sticky_mark_sweep.h"
#include "gc/reference_processor.h"
#include "gc/space/bump_pointer_space.h"
#include "gc/space/dlmalloc_space-inl.h"
#include "gc/space/image_space.h"
#include "gc/space/large_object_space.h"
#include "gc/space/rosalloc_space-inl.h"
#include "gc/space/space-inl.h"
#include "gc/space/zygote_space.h"
#include "entrypoints/quick/quick_alloc_entrypoints.h"
#include "heap-inl.h"
#include "image.h"
#include "mirror/art_field-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/reference-inl.h"
#include "os.h"
#include "reflection.h"
#include "runtime.h"
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change.h"
#include "handle_scope-inl.h"
#include "thread_list.h"
#include "well_known_classes.h"

namespace art {

namespace gc {

static constexpr size_t kCollectorTransitionStressIterations = 0;
static constexpr size_t kCollectorTransitionStressWait = 10 * 1000;  // Microseconds
static constexpr bool kGCALotMode = false;
static constexpr size_t kGcAlotInterval = KB;
// Minimum amount of remaining bytes before a concurrent GC is triggered.
static constexpr size_t kMinConcurrentRemainingBytes = 128 * KB;
static constexpr size_t kMaxConcurrentRemainingBytes = 512 * KB;
// Sticky GC throughput adjustment, divided by 4. Increasing this causes sticky GC to occur more
// relative to partial/full GC. This may be desirable since sticky GCs interfere less with mutator
// threads (lower pauses, use less memory bandwidth).
static constexpr double kStickyGcThroughputAdjustment = 1.0;
// Whether or not we use the free list large object space. Only use it if USE_ART_LOW_4G_ALLOCATOR
// since this means that we have to use the slow msync loop in MemMap::MapAnonymous.
#if USE_ART_LOW_4G_ALLOCATOR
static constexpr bool kUseFreeListSpaceForLOS = true;
#else
static constexpr bool kUseFreeListSpaceForLOS = false;
#endif
// Whether or not we compact the zygote in PreZygoteFork.
static constexpr bool kCompactZygote = kMovingCollector;
// How many reserve entries are at the end of the allocation stack, these are only needed if the
// allocation stack overflows.
static constexpr size_t kAllocationStackReserveSize = 1024;
// Default mark stack size in bytes.
static const size_t kDefaultMarkStackSize = 64 * KB;
// Define space name.
static const char* kDlMallocSpaceName[2] = {"main dlmalloc space", "main dlmalloc space 1"};
static const char* kRosAllocSpaceName[2] = {"main rosalloc space", "main rosalloc space 1"};
static const char* kMemMapSpaceName[2] = {"main space", "main space 1"};
static constexpr size_t kGSSBumpPointerSpaceCapacity = 32 * MB;

Heap::Heap(size_t initial_size, size_t growth_limit, size_t min_free, size_t max_free,
           double target_utilization, double foreground_heap_growth_multiplier,
           size_t capacity, size_t non_moving_space_capacity, const std::string& image_file_name,
           const InstructionSet image_instruction_set, CollectorType foreground_collector_type,
           CollectorType background_collector_type, size_t parallel_gc_threads,
           size_t conc_gc_threads, bool low_memory_mode,
           size_t long_pause_log_threshold, size_t long_gc_log_threshold,
           bool ignore_max_footprint, bool use_tlab,
           bool verify_pre_gc_heap, bool verify_pre_sweeping_heap, bool verify_post_gc_heap,
           bool verify_pre_gc_rosalloc, bool verify_pre_sweeping_rosalloc,
           bool verify_post_gc_rosalloc, bool use_homogeneous_space_compaction_for_oom,
           uint64_t min_interval_homogeneous_space_compaction_by_oom)
    : non_moving_space_(nullptr),
      rosalloc_space_(nullptr),
      dlmalloc_space_(nullptr),
      main_space_(nullptr),
      collector_type_(kCollectorTypeNone),
      foreground_collector_type_(foreground_collector_type),
      background_collector_type_(background_collector_type),
      desired_collector_type_(foreground_collector_type_),
      heap_trim_request_lock_(nullptr),
      last_trim_time_(0),
      heap_transition_or_trim_target_time_(0),
      heap_trim_request_pending_(false),
      parallel_gc_threads_(parallel_gc_threads),
      conc_gc_threads_(conc_gc_threads),
      low_memory_mode_(low_memory_mode),
      long_pause_log_threshold_(long_pause_log_threshold),
      long_gc_log_threshold_(long_gc_log_threshold),
      ignore_max_footprint_(ignore_max_footprint),
      zygote_creation_lock_("zygote creation lock", kZygoteCreationLock),
      have_zygote_space_(false),
      large_object_threshold_(std::numeric_limits<size_t>::max()),  // Starts out disabled.
      collector_type_running_(kCollectorTypeNone),
      last_gc_type_(collector::kGcTypeNone),
      next_gc_type_(collector::kGcTypePartial),
      capacity_(capacity),
      growth_limit_(growth_limit),
      max_allowed_footprint_(initial_size),
      native_footprint_gc_watermark_(initial_size),
      native_need_to_run_finalization_(false),
      // Initially assume we perceive jank in case the process state is never updated.
      process_state_(kProcessStateJankPerceptible),
      concurrent_start_bytes_(std::numeric_limits<size_t>::max()),
      total_bytes_freed_ever_(0),
      total_objects_freed_ever_(0),
      num_bytes_allocated_(0),
      native_bytes_allocated_(0),
      verify_missing_card_marks_(false),
      verify_system_weaks_(false),
      verify_pre_gc_heap_(verify_pre_gc_heap),
      verify_pre_sweeping_heap_(verify_pre_sweeping_heap),
      verify_post_gc_heap_(verify_post_gc_heap),
      verify_mod_union_table_(false),
      verify_pre_gc_rosalloc_(verify_pre_gc_rosalloc),
      verify_pre_sweeping_rosalloc_(verify_pre_sweeping_rosalloc),
      verify_post_gc_rosalloc_(verify_post_gc_rosalloc),
      last_gc_time_ns_(NanoTime()),
      allocation_rate_(0),
      /* For GC a lot mode, we limit the allocations stacks to be kGcAlotInterval allocations. This
       * causes a lot of GC since we do a GC for alloc whenever the stack is full. When heap
       * verification is enabled, we limit the size of allocation stacks to speed up their
       * searching.
       */
      max_allocation_stack_size_(kGCALotMode ? kGcAlotInterval
          : (kVerifyObjectSupport > kVerifyObjectModeFast) ? KB : MB),
      current_allocator_(kAllocatorTypeDlMalloc),
      current_non_moving_allocator_(kAllocatorTypeNonMoving),
      bump_pointer_space_(nullptr),
      temp_space_(nullptr),
      min_free_(min_free),
      max_free_(max_free),
      target_utilization_(target_utilization),
      foreground_heap_growth_multiplier_(foreground_heap_growth_multiplier),
      total_wait_time_(0),
      total_allocation_time_(0),
      verify_object_mode_(kVerifyObjectModeDisabled),
      disable_moving_gc_count_(0),
      running_on_valgrind_(Runtime::Current()->RunningOnValgrind()),
      use_tlab_(use_tlab),
      main_space_backup_(nullptr),
      min_interval_homogeneous_space_compaction_by_oom_(
          min_interval_homogeneous_space_compaction_by_oom),
      last_time_homogeneous_space_compaction_by_oom_(NanoTime()),
      use_homogeneous_space_compaction_for_oom_(use_homogeneous_space_compaction_for_oom) {
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "Heap() entering";
  }
  // If we aren't the zygote, switch to the default non zygote allocator. This may update the
  // entrypoints.
  const bool is_zygote = Runtime::Current()->IsZygote();
  if (!is_zygote) {
    large_object_threshold_ = kDefaultLargeObjectThreshold;
    // Background compaction is currently not supported for command line runs.
    if (background_collector_type_ != foreground_collector_type_) {
      VLOG(heap) << "Disabling background compaction for non zygote";
      background_collector_type_ = foreground_collector_type_;
    }
  }
  ChangeCollector(desired_collector_type_);
  live_bitmap_.reset(new accounting::HeapBitmap(this));
  mark_bitmap_.reset(new accounting::HeapBitmap(this));
  // Requested begin for the alloc space, to follow the mapped image and oat files
  byte* requested_alloc_space_begin = nullptr;
  if (!image_file_name.empty()) {
    std::string error_msg;
    space::ImageSpace* image_space = space::ImageSpace::Create(image_file_name.c_str(),
                                                               image_instruction_set,
                                                               &error_msg);
    if (image_space != nullptr) {
      AddSpace(image_space);
      // Oat files referenced by image files immediately follow them in memory, ensure alloc space
      // isn't going to get in the middle
      byte* oat_file_end_addr = image_space->GetImageHeader().GetOatFileEnd();
      CHECK_GT(oat_file_end_addr, image_space->End());
      requested_alloc_space_begin = AlignUp(oat_file_end_addr, kPageSize);
    } else {
      LOG(WARNING) << "Could not create image space with image file '" << image_file_name << "'. "
                   << "Attempting to fall back to imageless running. Error was: " << error_msg;
    }
  }
  /*
  requested_alloc_space_begin ->     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-
                                     +-  nonmoving space (non_moving_space_capacity)+-
                                     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-
                                     +-????????????????????????????????????????????+-
                                     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-
                                     +-main alloc space / bump space 1 (capacity_) +-
                                     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-
                                     +-????????????????????????????????????????????+-
                                     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-
                                     +-main alloc space2 / bump space 2 (capacity_)+-
                                     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-
  */
  bool support_homogeneous_space_compaction =
      background_collector_type_ == gc::kCollectorTypeHomogeneousSpaceCompact ||
      use_homogeneous_space_compaction_for_oom;
  // We may use the same space the main space for the non moving space if we don't need to compact
  // from the main space.
  // This is not the case if we support homogeneous compaction or have a moving background
  // collector type.
  bool separate_non_moving_space = is_zygote ||
      support_homogeneous_space_compaction || IsMovingGc(foreground_collector_type_) ||
      IsMovingGc(background_collector_type_);
  if (foreground_collector_type == kCollectorTypeGSS) {
    separate_non_moving_space = false;
  }
  std::unique_ptr<MemMap> main_mem_map_1;
  std::unique_ptr<MemMap> main_mem_map_2;
  byte* request_begin = requested_alloc_space_begin;
  if (request_begin != nullptr && separate_non_moving_space) {
    request_begin += non_moving_space_capacity;
  }
  std::string error_str;
  std::unique_ptr<MemMap> non_moving_space_mem_map;
  if (separate_non_moving_space) {
    // Reserve the non moving mem map before the other two since it needs to be at a specific
    // address.
    non_moving_space_mem_map.reset(
        MemMap::MapAnonymous("non moving space", requested_alloc_space_begin,
                             non_moving_space_capacity, PROT_READ | PROT_WRITE, true, &error_str));
    CHECK(non_moving_space_mem_map != nullptr) << error_str;
    // Try to reserve virtual memory at a lower address if we have a separate non moving space.
    request_begin = reinterpret_cast<byte*>(300 * MB);
  }
  // Attempt to create 2 mem maps at or after the requested begin.
  main_mem_map_1.reset(MapAnonymousPreferredAddress(kMemMapSpaceName[0], request_begin, capacity_,
                                                    PROT_READ | PROT_WRITE, &error_str));
  CHECK(main_mem_map_1.get() != nullptr) << error_str;
  if (support_homogeneous_space_compaction ||
      background_collector_type_ == kCollectorTypeSS ||
      foreground_collector_type_ == kCollectorTypeSS) {
    main_mem_map_2.reset(MapAnonymousPreferredAddress(kMemMapSpaceName[1], main_mem_map_1->End(),
                                                      capacity_, PROT_READ | PROT_WRITE,
                                                      &error_str));
    CHECK(main_mem_map_2.get() != nullptr) << error_str;
  }
  // Create the non moving space first so that bitmaps don't take up the address range.
  if (separate_non_moving_space) {
    // Non moving space is always dlmalloc since we currently don't have support for multiple
    // active rosalloc spaces.
    const size_t size = non_moving_space_mem_map->Size();
    non_moving_space_ = space::DlMallocSpace::CreateFromMemMap(
        non_moving_space_mem_map.release(), "zygote / non moving space", kDefaultStartingSize,
        initial_size, size, size, false);
    non_moving_space_->SetFootprintLimit(non_moving_space_->Capacity());
    CHECK(non_moving_space_ != nullptr) << "Failed creating non moving space "
        << requested_alloc_space_begin;
    AddSpace(non_moving_space_);
  }
  // Create other spaces based on whether or not we have a moving GC.
  if (IsMovingGc(foreground_collector_type_) && foreground_collector_type_ != kCollectorTypeGSS) {
    // Create bump pointer spaces.
    // We only to create the bump pointer if the foreground collector is a compacting GC.
    // TODO: Place bump-pointer spaces somewhere to minimize size of card table.
    bump_pointer_space_ = space::BumpPointerSpace::CreateFromMemMap("Bump pointer space 1",
                                                                    main_mem_map_1.release());
    CHECK(bump_pointer_space_ != nullptr) << "Failed to create bump pointer space";
    AddSpace(bump_pointer_space_);
    temp_space_ = space::BumpPointerSpace::CreateFromMemMap("Bump pointer space 2",
                                                            main_mem_map_2.release());
    CHECK(temp_space_ != nullptr) << "Failed to create bump pointer space";
    AddSpace(temp_space_);
    CHECK(separate_non_moving_space);
  } else {
    CreateMainMallocSpace(main_mem_map_1.release(), initial_size, growth_limit_, capacity_);
    CHECK(main_space_ != nullptr);
    AddSpace(main_space_);
    if (!separate_non_moving_space) {
      non_moving_space_ = main_space_;
      CHECK(!non_moving_space_->CanMoveObjects());
    }
    if (foreground_collector_type_ == kCollectorTypeGSS) {
      CHECK_EQ(foreground_collector_type_, background_collector_type_);
      // Create bump pointer spaces instead of a backup space.
      main_mem_map_2.release();
      bump_pointer_space_ = space::BumpPointerSpace::Create("Bump pointer space 1",
                                                            kGSSBumpPointerSpaceCapacity, nullptr);
      CHECK(bump_pointer_space_ != nullptr);
      AddSpace(bump_pointer_space_);
      temp_space_ = space::BumpPointerSpace::Create("Bump pointer space 2",
                                                    kGSSBumpPointerSpaceCapacity, nullptr);
      CHECK(temp_space_ != nullptr);
      AddSpace(temp_space_);
    } else if (main_mem_map_2.get() != nullptr) {
      const char* name = kUseRosAlloc ? kRosAllocSpaceName[1] : kDlMallocSpaceName[1];
      main_space_backup_.reset(CreateMallocSpaceFromMemMap(main_mem_map_2.release(), initial_size,
                                                           growth_limit_, capacity_, name, true));
      CHECK(main_space_backup_.get() != nullptr);
      // Add the space so its accounted for in the heap_begin and heap_end.
      AddSpace(main_space_backup_.get());
    }
  }
  CHECK(non_moving_space_ != nullptr);
  CHECK(!non_moving_space_->CanMoveObjects());
  // Allocate the large object space.
  if (kUseFreeListSpaceForLOS) {
    large_object_space_ = space::FreeListSpace::Create("large object space", nullptr, capacity_);
  } else {
    large_object_space_ = space::LargeObjectMapSpace::Create("large object space");
  }
  CHECK(large_object_space_ != nullptr) << "Failed to create large object space";
  AddSpace(large_object_space_);
  // Compute heap capacity. Continuous spaces are sorted in order of Begin().
  CHECK(!continuous_spaces_.empty());
  // Relies on the spaces being sorted.
  byte* heap_begin = continuous_spaces_.front()->Begin();
  byte* heap_end = continuous_spaces_.back()->Limit();
  size_t heap_capacity = heap_end - heap_begin;
  // Remove the main backup space since it slows down the GC to have unused extra spaces.
  if (main_space_backup_.get() != nullptr) {
    RemoveSpace(main_space_backup_.get());
  }
  // Allocate the card table.
  card_table_.reset(accounting::CardTable::Create(heap_begin, heap_capacity));
  CHECK(card_table_.get() != NULL) << "Failed to create card table";
  // Card cache for now since it makes it easier for us to update the references to the copying
  // spaces.
  accounting::ModUnionTable* mod_union_table =
      new accounting::ModUnionTableToZygoteAllocspace("Image mod-union table", this,
                                                      GetImageSpace());
  CHECK(mod_union_table != nullptr) << "Failed to create image mod-union table";
  AddModUnionTable(mod_union_table);
  if (collector::SemiSpace::kUseRememberedSet && non_moving_space_ != main_space_) {
    accounting::RememberedSet* non_moving_space_rem_set =
        new accounting::RememberedSet("Non-moving space remembered set", this, non_moving_space_);
    CHECK(non_moving_space_rem_set != nullptr) << "Failed to create non-moving space remembered set";
    AddRememberedSet(non_moving_space_rem_set);
  }
  // TODO: Count objects in the image space here?
  num_bytes_allocated_.StoreRelaxed(0);
  mark_stack_.reset(accounting::ObjectStack::Create("mark stack", kDefaultMarkStackSize,
                                                    kDefaultMarkStackSize));
  const size_t alloc_stack_capacity = max_allocation_stack_size_ + kAllocationStackReserveSize;
  allocation_stack_.reset(accounting::ObjectStack::Create(
      "allocation stack", max_allocation_stack_size_, alloc_stack_capacity));
  live_stack_.reset(accounting::ObjectStack::Create(
      "live stack", max_allocation_stack_size_, alloc_stack_capacity));
  // It's still too early to take a lock because there are no threads yet, but we can create locks
  // now. We don't create it earlier to make it clear that you can't use locks during heap
  // initialization.
  gc_complete_lock_ = new Mutex("GC complete lock");
  gc_complete_cond_.reset(new ConditionVariable("GC complete condition variable",
                                                *gc_complete_lock_));
  heap_trim_request_lock_ = new Mutex("Heap trim request lock");
  last_gc_size_ = GetBytesAllocated();
  if (ignore_max_footprint_) {
    SetIdealFootprint(std::numeric_limits<size_t>::max());
    concurrent_start_bytes_ = std::numeric_limits<size_t>::max();
  }
  CHECK_NE(max_allowed_footprint_, 0U);
  // Create our garbage collectors.
  for (size_t i = 0; i < 2; ++i) {
    const bool concurrent = i != 0;
    garbage_collectors_.push_back(new collector::MarkSweep(this, concurrent));
    garbage_collectors_.push_back(new collector::PartialMarkSweep(this, concurrent));
    garbage_collectors_.push_back(new collector::StickyMarkSweep(this, concurrent));
  }
  if (kMovingCollector) {
    // TODO: Clean this up.
    const bool generational = foreground_collector_type_ == kCollectorTypeGSS;
    semi_space_collector_ = new collector::SemiSpace(this, generational,
                                                     generational ? "generational" : "");
    garbage_collectors_.push_back(semi_space_collector_);
    concurrent_copying_collector_ = new collector::ConcurrentCopying(this);
    garbage_collectors_.push_back(concurrent_copying_collector_);
    mark_compact_collector_ = new collector::MarkCompact(this);
    garbage_collectors_.push_back(mark_compact_collector_);
  }
  if (GetImageSpace() != nullptr && non_moving_space_ != nullptr) {
    // Check that there's no gap between the image space and the non moving space so that the
    // immune region won't break (eg. due to a large object allocated in the gap).
    bool no_gap = MemMap::CheckNoGaps(GetImageSpace()->GetMemMap(),
                                      non_moving_space_->GetMemMap());
    if (!no_gap) {
      MemMap::DumpMaps(LOG(ERROR));
      LOG(FATAL) << "There's a gap between the image space and the main space";
    }
  }
  if (running_on_valgrind_) {
    Runtime::Current()->GetInstrumentation()->InstrumentQuickAllocEntryPoints();
  }
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "Heap() exiting";
  }
}

MemMap* Heap::MapAnonymousPreferredAddress(const char* name, byte* request_begin, size_t capacity,
                                           int prot_flags, std::string* out_error_str) {
  while (true) {
    MemMap* map = MemMap::MapAnonymous(kMemMapSpaceName[0], request_begin, capacity,
                                       PROT_READ | PROT_WRITE, true, out_error_str);
    if (map != nullptr || request_begin == nullptr) {
      return map;
    }
    // Retry a  second time with no specified request begin.
    request_begin = nullptr;
  }
  return nullptr;
}

space::MallocSpace* Heap::CreateMallocSpaceFromMemMap(MemMap* mem_map, size_t initial_size,
                                                      size_t growth_limit, size_t capacity,
                                                      const char* name, bool can_move_objects) {
  space::MallocSpace* malloc_space = nullptr;
  if (kUseRosAlloc) {
    // Create rosalloc space.
    malloc_space = space::RosAllocSpace::CreateFromMemMap(mem_map, name, kDefaultStartingSize,
                                                          initial_size, growth_limit, capacity,
                                                          low_memory_mode_, can_move_objects);
  } else {
    malloc_space = space::DlMallocSpace::CreateFromMemMap(mem_map, name, kDefaultStartingSize,
                                                          initial_size, growth_limit, capacity,
                                                          can_move_objects);
  }
  if (collector::SemiSpace::kUseRememberedSet) {
    accounting::RememberedSet* rem_set  =
        new accounting::RememberedSet(std::string(name) + " remembered set", this, malloc_space);
    CHECK(rem_set != nullptr) << "Failed to create main space remembered set";
    AddRememberedSet(rem_set);
  }
  CHECK(malloc_space != nullptr) << "Failed to create " << name;
  malloc_space->SetFootprintLimit(malloc_space->Capacity());
  return malloc_space;
}

void Heap::CreateMainMallocSpace(MemMap* mem_map, size_t initial_size, size_t growth_limit,
                                 size_t capacity) {
  // Is background compaction is enabled?
  bool can_move_objects = IsMovingGc(background_collector_type_) !=
      IsMovingGc(foreground_collector_type_) || use_homogeneous_space_compaction_for_oom_;
  // If we are the zygote and don't yet have a zygote space, it means that the zygote fork will
  // happen in the future. If this happens and we have kCompactZygote enabled we wish to compact
  // from the main space to the zygote space. If background compaction is enabled, always pass in
  // that we can move objets.
  if (kCompactZygote && Runtime::Current()->IsZygote() && !can_move_objects) {
    // After the zygote we want this to be false if we don't have background compaction enabled so
    // that getting primitive array elements is faster.
    // We never have homogeneous compaction with GSS and don't need a space with movable objects.
    can_move_objects = !have_zygote_space_ && foreground_collector_type_ != kCollectorTypeGSS;
  }
  if (collector::SemiSpace::kUseRememberedSet && main_space_ != nullptr) {
    RemoveRememberedSet(main_space_);
  }
  const char* name = kUseRosAlloc ? kRosAllocSpaceName[0] : kDlMallocSpaceName[0];
  main_space_ = CreateMallocSpaceFromMemMap(mem_map, initial_size, growth_limit, capacity, name,
                                            can_move_objects);
  SetSpaceAsDefault(main_space_);
  VLOG(heap) << "Created main space " << main_space_;
}

void Heap::ChangeAllocator(AllocatorType allocator) {
  if (current_allocator_ != allocator) {
    // These two allocators are only used internally and don't have any entrypoints.
    CHECK_NE(allocator, kAllocatorTypeLOS);
    CHECK_NE(allocator, kAllocatorTypeNonMoving);
    current_allocator_ = allocator;
    MutexLock mu(nullptr, *Locks::runtime_shutdown_lock_);
    SetQuickAllocEntryPointsAllocator(current_allocator_);
    Runtime::Current()->GetInstrumentation()->ResetQuickAllocEntryPoints();
  }
}

void Heap::DisableMovingGc() {
  if (IsMovingGc(foreground_collector_type_)) {
    foreground_collector_type_ = kCollectorTypeCMS;
  }
  if (IsMovingGc(background_collector_type_)) {
    background_collector_type_ = foreground_collector_type_;
  }
  TransitionCollector(foreground_collector_type_);
  ThreadList* tl = Runtime::Current()->GetThreadList();
  Thread* self = Thread::Current();
  ScopedThreadStateChange tsc(self, kSuspended);
  tl->SuspendAll();
  // Something may have caused the transition to fail.
  if (!IsMovingGc(collector_type_) && non_moving_space_ != main_space_) {
    CHECK(main_space_ != nullptr);
    // The allocation stack may have non movable objects in it. We need to flush it since the GC
    // can't only handle marking allocation stack objects of one non moving space and one main
    // space.
    {
      WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
      FlushAllocStack();
    }
    main_space_->DisableMovingObjects();
    non_moving_space_ = main_space_;
    CHECK(!non_moving_space_->CanMoveObjects());
  }
  tl->ResumeAll();
}

std::string Heap::SafeGetClassDescriptor(mirror::Class* klass) {
  if (!IsValidContinuousSpaceObjectAddress(klass)) {
    return StringPrintf("<non heap address klass %p>", klass);
  }
  mirror::Class* component_type = klass->GetComponentType<kVerifyNone>();
  if (IsValidContinuousSpaceObjectAddress(component_type) && klass->IsArrayClass<kVerifyNone>()) {
    std::string result("[");
    result += SafeGetClassDescriptor(component_type);
    return result;
  } else if (UNLIKELY(klass->IsPrimitive<kVerifyNone>())) {
    return Primitive::Descriptor(klass->GetPrimitiveType<kVerifyNone>());
  } else if (UNLIKELY(klass->IsProxyClass<kVerifyNone>())) {
    return Runtime::Current()->GetClassLinker()->GetDescriptorForProxy(klass);
  } else {
    mirror::DexCache* dex_cache = klass->GetDexCache<kVerifyNone>();
    if (!IsValidContinuousSpaceObjectAddress(dex_cache)) {
      return StringPrintf("<non heap address dex_cache %p>", dex_cache);
    }
    const DexFile* dex_file = dex_cache->GetDexFile();
    uint16_t class_def_idx = klass->GetDexClassDefIndex();
    if (class_def_idx == DexFile::kDexNoIndex16) {
      return "<class def not found>";
    }
    const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_def_idx);
    const DexFile::TypeId& type_id = dex_file->GetTypeId(class_def.class_idx_);
    return dex_file->GetTypeDescriptor(type_id);
  }
}

std::string Heap::SafePrettyTypeOf(mirror::Object* obj) {
  if (obj == nullptr) {
    return "null";
  }
  mirror::Class* klass = obj->GetClass<kVerifyNone>();
  if (klass == nullptr) {
    return "(class=null)";
  }
  std::string result(SafeGetClassDescriptor(klass));
  if (obj->IsClass()) {
    result += "<" + SafeGetClassDescriptor(obj->AsClass<kVerifyNone>()) + ">";
  }
  return result;
}

void Heap::DumpObject(std::ostream& stream, mirror::Object* obj) {
  if (obj == nullptr) {
    stream << "(obj=null)";
    return;
  }
  if (IsAligned<kObjectAlignment>(obj)) {
    space::Space* space = nullptr;
    // Don't use find space since it only finds spaces which actually contain objects instead of
    // spaces which may contain objects (e.g. cleared bump pointer spaces).
    for (const auto& cur_space : continuous_spaces_) {
      if (cur_space->HasAddress(obj)) {
        space = cur_space;
        break;
      }
    }
    // Unprotect all the spaces.
    for (const auto& space : continuous_spaces_) {
      mprotect(space->Begin(), space->Capacity(), PROT_READ | PROT_WRITE);
    }
    stream << "Object " << obj;
    if (space != nullptr) {
      stream << " in space " << *space;
    }
    mirror::Class* klass = obj->GetClass<kVerifyNone>();
    stream << "\nclass=" << klass;
    if (klass != nullptr) {
      stream << " type= " << SafePrettyTypeOf(obj);
    }
    // Re-protect the address we faulted on.
    mprotect(AlignDown(obj, kPageSize), kPageSize, PROT_NONE);
  }
}

bool Heap::IsCompilingBoot() const {
  if (!Runtime::Current()->IsCompiler()) {
    return false;
  }
  for (const auto& space : continuous_spaces_) {
    if (space->IsImageSpace() || space->IsZygoteSpace()) {
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

void Heap::IncrementDisableMovingGC(Thread* self) {
  // Need to do this holding the lock to prevent races where the GC is about to run / running when
  // we attempt to disable it.
  ScopedThreadStateChange tsc(self, kWaitingForGcToComplete);
  MutexLock mu(self, *gc_complete_lock_);
  ++disable_moving_gc_count_;
  if (IsMovingGc(collector_type_running_)) {
    WaitForGcToCompleteLocked(kGcCauseDisableMovingGc, self);
  }
}

void Heap::DecrementDisableMovingGC(Thread* self) {
  MutexLock mu(self, *gc_complete_lock_);
  CHECK_GE(disable_moving_gc_count_, 0U);
  --disable_moving_gc_count_;
}

void Heap::UpdateProcessState(ProcessState process_state) {
  if (process_state_ != process_state) {
    process_state_ = process_state;
    for (size_t i = 1; i <= kCollectorTransitionStressIterations; ++i) {
      // Start at index 1 to avoid "is always false" warning.
      // Have iteration 1 always transition the collector.
      TransitionCollector((((i & 1) == 1) == (process_state_ == kProcessStateJankPerceptible))
                          ? foreground_collector_type_ : background_collector_type_);
      usleep(kCollectorTransitionStressWait);
    }
    if (process_state_ == kProcessStateJankPerceptible) {
      // Transition back to foreground right away to prevent jank.
      RequestCollectorTransition(foreground_collector_type_, 0);
    } else {
      // Don't delay for debug builds since we may want to stress test the GC.
      // If background_collector_type_ is kCollectorTypeHomogeneousSpaceCompact then we have
      // special handling which does a homogenous space compaction once but then doesn't transition
      // the collector.
      RequestCollectorTransition(background_collector_type_,
                                 kIsDebugBuild ? 0 : kCollectorTransitionWait);
    }
  }
}

void Heap::CreateThreadPool() {
  const size_t num_threads = std::max(parallel_gc_threads_, conc_gc_threads_);
  if (num_threads != 0) {
    thread_pool_.reset(new ThreadPool("Heap thread pool", num_threads));
  }
}

void Heap::VisitObjects(ObjectCallback callback, void* arg) {
  Thread* self = Thread::Current();
  // GCs can move objects, so don't allow this.
  const char* old_cause = self->StartAssertNoThreadSuspension("Visiting objects");
  if (bump_pointer_space_ != nullptr) {
    // Visit objects in bump pointer space.
    bump_pointer_space_->Walk(callback, arg);
  }
  // TODO: Switch to standard begin and end to use ranged a based loop.
  for (mirror::Object** it = allocation_stack_->Begin(), **end = allocation_stack_->End();
      it < end; ++it) {
    mirror::Object* obj = *it;
    if (obj != nullptr && obj->GetClass() != nullptr) {
      // Avoid the race condition caused by the object not yet being written into the allocation
      // stack or the class not yet being written in the object. Or, if kUseThreadLocalAllocationStack,
      // there can be nulls on the allocation stack.
      callback(obj, arg);
    }
  }
  GetLiveBitmap()->Walk(callback, arg);
  self->EndAssertNoThreadSuspension(old_cause);
}

void Heap::MarkAllocStackAsLive(accounting::ObjectStack* stack) {
  space::ContinuousSpace* space1 = main_space_ != nullptr ? main_space_ : non_moving_space_;
  space::ContinuousSpace* space2 = non_moving_space_;
  // TODO: Generalize this to n bitmaps?
  CHECK(space1 != nullptr);
  CHECK(space2 != nullptr);
  MarkAllocStack(space1->GetLiveBitmap(), space2->GetLiveBitmap(),
                 large_object_space_->GetLiveBitmap(), stack);
}

void Heap::DeleteThreadPool() {
  thread_pool_.reset(nullptr);
}

void Heap::AddSpace(space::Space* space) {
  CHECK(space != nullptr);
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  if (space->IsContinuousSpace()) {
    DCHECK(!space->IsDiscontinuousSpace());
    space::ContinuousSpace* continuous_space = space->AsContinuousSpace();
    // Continuous spaces don't necessarily have bitmaps.
    accounting::ContinuousSpaceBitmap* live_bitmap = continuous_space->GetLiveBitmap();
    accounting::ContinuousSpaceBitmap* mark_bitmap = continuous_space->GetMarkBitmap();
    if (live_bitmap != nullptr) {
      CHECK(mark_bitmap != nullptr);
      live_bitmap_->AddContinuousSpaceBitmap(live_bitmap);
      mark_bitmap_->AddContinuousSpaceBitmap(mark_bitmap);
    }
    continuous_spaces_.push_back(continuous_space);
    // Ensure that spaces remain sorted in increasing order of start address.
    std::sort(continuous_spaces_.begin(), continuous_spaces_.end(),
              [](const space::ContinuousSpace* a, const space::ContinuousSpace* b) {
      return a->Begin() < b->Begin();
    });
  } else {
    CHECK(space->IsDiscontinuousSpace());
    space::DiscontinuousSpace* discontinuous_space = space->AsDiscontinuousSpace();
    live_bitmap_->AddLargeObjectBitmap(discontinuous_space->GetLiveBitmap());
    mark_bitmap_->AddLargeObjectBitmap(discontinuous_space->GetMarkBitmap());
    discontinuous_spaces_.push_back(discontinuous_space);
  }
  if (space->IsAllocSpace()) {
    alloc_spaces_.push_back(space->AsAllocSpace());
  }
}

void Heap::SetSpaceAsDefault(space::ContinuousSpace* continuous_space) {
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  if (continuous_space->IsDlMallocSpace()) {
    dlmalloc_space_ = continuous_space->AsDlMallocSpace();
  } else if (continuous_space->IsRosAllocSpace()) {
    rosalloc_space_ = continuous_space->AsRosAllocSpace();
  }
}

void Heap::RemoveSpace(space::Space* space) {
  DCHECK(space != nullptr);
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  if (space->IsContinuousSpace()) {
    DCHECK(!space->IsDiscontinuousSpace());
    space::ContinuousSpace* continuous_space = space->AsContinuousSpace();
    // Continuous spaces don't necessarily have bitmaps.
    accounting::ContinuousSpaceBitmap* live_bitmap = continuous_space->GetLiveBitmap();
    accounting::ContinuousSpaceBitmap* mark_bitmap = continuous_space->GetMarkBitmap();
    if (live_bitmap != nullptr) {
      DCHECK(mark_bitmap != nullptr);
      live_bitmap_->RemoveContinuousSpaceBitmap(live_bitmap);
      mark_bitmap_->RemoveContinuousSpaceBitmap(mark_bitmap);
    }
    auto it = std::find(continuous_spaces_.begin(), continuous_spaces_.end(), continuous_space);
    DCHECK(it != continuous_spaces_.end());
    continuous_spaces_.erase(it);
  } else {
    DCHECK(space->IsDiscontinuousSpace());
    space::DiscontinuousSpace* discontinuous_space = space->AsDiscontinuousSpace();
    live_bitmap_->RemoveLargeObjectBitmap(discontinuous_space->GetLiveBitmap());
    mark_bitmap_->RemoveLargeObjectBitmap(discontinuous_space->GetMarkBitmap());
    auto it = std::find(discontinuous_spaces_.begin(), discontinuous_spaces_.end(),
                        discontinuous_space);
    DCHECK(it != discontinuous_spaces_.end());
    discontinuous_spaces_.erase(it);
  }
  if (space->IsAllocSpace()) {
    auto it = std::find(alloc_spaces_.begin(), alloc_spaces_.end(), space->AsAllocSpace());
    DCHECK(it != alloc_spaces_.end());
    alloc_spaces_.erase(it);
  }
}

void Heap::DumpGcPerformanceInfo(std::ostream& os) {
  // Dump cumulative timings.
  os << "Dumping cumulative Gc timings\n";
  uint64_t total_duration = 0;
  // Dump cumulative loggers for each GC type.
  uint64_t total_paused_time = 0;
  for (auto& collector : garbage_collectors_) {
    const CumulativeLogger& logger = collector->GetCumulativeTimings();
    const size_t iterations = logger.GetIterations();
    const Histogram<uint64_t>& pause_histogram = collector->GetPauseHistogram();
    if (iterations != 0 && pause_histogram.SampleSize() != 0) {
      os << ConstDumpable<CumulativeLogger>(logger);
      const uint64_t total_ns = logger.GetTotalNs();
      const uint64_t total_pause_ns = collector->GetTotalPausedTimeNs();
      double seconds = NsToMs(logger.GetTotalNs()) / 1000.0;
      const uint64_t freed_bytes = collector->GetTotalFreedBytes();
      const uint64_t freed_objects = collector->GetTotalFreedObjects();
      Histogram<uint64_t>::CumulativeData cumulative_data;
      pause_histogram.CreateHistogram(&cumulative_data);
      pause_histogram.PrintConfidenceIntervals(os, 0.99, cumulative_data);
      os << collector->GetName() << " total time: " << PrettyDuration(total_ns)
         << " mean time: " << PrettyDuration(total_ns / iterations) << "\n"
         << collector->GetName() << " freed: " << freed_objects
         << " objects with total size " << PrettySize(freed_bytes) << "\n"
         << collector->GetName() << " throughput: " << freed_objects / seconds << "/s / "
         << PrettySize(freed_bytes / seconds) << "/s\n";
      total_duration += total_ns;
      total_paused_time += total_pause_ns;
    }
    collector->ResetMeasurements();
  }
  uint64_t allocation_time =
      static_cast<uint64_t>(total_allocation_time_.LoadRelaxed()) * kTimeAdjust;
  if (total_duration != 0) {
    const double total_seconds = static_cast<double>(total_duration / 1000) / 1000000.0;
    os << "Total time spent in GC: " << PrettyDuration(total_duration) << "\n";
    os << "Mean GC size throughput: "
       << PrettySize(GetBytesFreedEver() / total_seconds) << "/s\n";
    os << "Mean GC object throughput: "
       << (GetObjectsFreedEver() / total_seconds) << " objects/s\n";
  }
  uint64_t total_objects_allocated = GetObjectsAllocatedEver();
  os << "Total number of allocations " << total_objects_allocated << "\n";
  uint64_t total_bytes_allocated = GetBytesAllocatedEver();
  os << "Total bytes allocated " << PrettySize(total_bytes_allocated) << "\n";
  os << "Free memory " << PrettySize(GetFreeMemory()) << "\n";
  os << "Free memory until GC " << PrettySize(GetFreeMemoryUntilGC()) << "\n";
  os << "Free memory until OOME " << PrettySize(GetFreeMemoryUntilOOME()) << "\n";
  os << "Total memory " << PrettySize(GetTotalMemory()) << "\n";
  os << "Max memory " << PrettySize(GetMaxMemory()) << "\n";
  if (kMeasureAllocationTime) {
    os << "Total time spent allocating: " << PrettyDuration(allocation_time) << "\n";
    os << "Mean allocation time: " << PrettyDuration(allocation_time / total_objects_allocated)
       << "\n";
  }
  os << "Total mutator paused time: " << PrettyDuration(total_paused_time) << "\n";
  os << "Total time waiting for GC to complete: " << PrettyDuration(total_wait_time_) << "\n";
  BaseMutex::DumpAll(os);
}

Heap::~Heap() {
  VLOG(heap) << "Starting ~Heap()";
  STLDeleteElements(&garbage_collectors_);
  // If we don't reset then the mark stack complains in its destructor.
  allocation_stack_->Reset();
  live_stack_->Reset();
  STLDeleteValues(&mod_union_tables_);
  STLDeleteValues(&remembered_sets_);
  STLDeleteElements(&continuous_spaces_);
  STLDeleteElements(&discontinuous_spaces_);
  delete gc_complete_lock_;
  delete heap_trim_request_lock_;
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

space::ImageSpace* Heap::GetImageSpace() const {
  for (const auto& space : continuous_spaces_) {
    if (space->IsImageSpace()) {
      return space->AsImageSpace();
    }
  }
  return NULL;
}

void Heap::ThrowOutOfMemoryError(Thread* self, size_t byte_count, AllocatorType allocator_type) {
  std::ostringstream oss;
  size_t total_bytes_free = GetFreeMemory();
  oss << "Failed to allocate a " << byte_count << " byte allocation with " << total_bytes_free
      << " free bytes and " << PrettySize(GetFreeMemoryUntilOOME()) << " until OOM";
  // If the allocation failed due to fragmentation, print out the largest continuous allocation.
  if (total_bytes_free >= byte_count) {
    space::AllocSpace* space = nullptr;
    if (allocator_type == kAllocatorTypeNonMoving) {
      space = non_moving_space_;
    } else if (allocator_type == kAllocatorTypeRosAlloc ||
               allocator_type == kAllocatorTypeDlMalloc) {
      space = main_space_;
    } else if (allocator_type == kAllocatorTypeBumpPointer ||
               allocator_type == kAllocatorTypeTLAB) {
      space = bump_pointer_space_;
    }
    if (space != nullptr) {
      space->LogFragmentationAllocFailure(oss, byte_count);
    }
  }
  self->ThrowOutOfMemoryError(oss.str().c_str());
}

void Heap::DoPendingTransitionOrTrim() {
  Thread* self = Thread::Current();
  CollectorType desired_collector_type;
  // Wait until we reach the desired transition time.
  while (true) {
    uint64_t wait_time;
    {
      MutexLock mu(self, *heap_trim_request_lock_);
      desired_collector_type = desired_collector_type_;
      uint64_t current_time = NanoTime();
      if (current_time >= heap_transition_or_trim_target_time_) {
        break;
      }
      wait_time = heap_transition_or_trim_target_time_ - current_time;
    }
    ScopedThreadStateChange tsc(self, kSleeping);
    usleep(wait_time / 1000);  // Usleep takes microseconds.
  }
  // Launch homogeneous space compaction if it is desired.
  if (desired_collector_type == kCollectorTypeHomogeneousSpaceCompact) {
    if (!CareAboutPauseTimes()) {
      PerformHomogeneousSpaceCompact();
    }
    // No need to Trim(). Homogeneous space compaction may free more virtual and physical memory.
    desired_collector_type = collector_type_;
    return;
  }
  // Transition the collector if the desired collector type is not the same as the current
  // collector type.
  TransitionCollector(desired_collector_type);
  if (!CareAboutPauseTimes()) {
    // Deflate the monitors, this can cause a pause but shouldn't matter since we don't care
    // about pauses.
    Runtime* runtime = Runtime::Current();
    runtime->GetThreadList()->SuspendAll();
    uint64_t start_time = NanoTime();
    size_t count = runtime->GetMonitorList()->DeflateMonitors();
    VLOG(heap) << "Deflating " << count << " monitors took "
        << PrettyDuration(NanoTime() - start_time);
    runtime->GetThreadList()->ResumeAll();
  }
  // Do a heap trim if it is needed.
  Trim();
}

void Heap::Trim() {
  Thread* self = Thread::Current();
  {
    MutexLock mu(self, *heap_trim_request_lock_);
    if (!heap_trim_request_pending_ || last_trim_time_ + kHeapTrimWait >= NanoTime()) {
      return;
    }
    last_trim_time_ = NanoTime();
    heap_trim_request_pending_ = false;
  }
  {
    // Need to do this before acquiring the locks since we don't want to get suspended while
    // holding any locks.
    ScopedThreadStateChange tsc(self, kWaitingForGcToComplete);
    // Pretend we are doing a GC to prevent background compaction from deleting the space we are
    // trimming.
    MutexLock mu(self, *gc_complete_lock_);
    // Ensure there is only one GC at a time.
    WaitForGcToCompleteLocked(kGcCauseTrim, self);
    collector_type_running_ = kCollectorTypeHeapTrim;
  }
  uint64_t start_ns = NanoTime();
  // Trim the managed spaces.
  uint64_t total_alloc_space_allocated = 0;
  uint64_t total_alloc_space_size = 0;
  uint64_t managed_reclaimed = 0;
  for (const auto& space : continuous_spaces_) {
    if (space->IsMallocSpace()) {
      gc::space::MallocSpace* malloc_space = space->AsMallocSpace();
      if (malloc_space->IsRosAllocSpace() || !CareAboutPauseTimes()) {
        // Don't trim dlmalloc spaces if we care about pauses since this can hold the space lock
        // for a long period of time.
        managed_reclaimed += malloc_space->Trim();
      }
      total_alloc_space_size += malloc_space->Size();
    }
  }
  total_alloc_space_allocated = GetBytesAllocated() - large_object_space_->GetBytesAllocated();
  if (bump_pointer_space_ != nullptr) {
    total_alloc_space_allocated -= bump_pointer_space_->Size();
  }
  const float managed_utilization = static_cast<float>(total_alloc_space_allocated) /
      static_cast<float>(total_alloc_space_size);
  uint64_t gc_heap_end_ns = NanoTime();
  // We never move things in the native heap, so we can finish the GC at this point.
  FinishGC(self, collector::kGcTypeNone);
  size_t native_reclaimed = 0;
  // Only trim the native heap if we don't care about pauses.
  if (!CareAboutPauseTimes()) {
#if defined(USE_DLMALLOC)
    // Trim the native heap.
    dlmalloc_trim(0);
    dlmalloc_inspect_all(DlmallocMadviseCallback, &native_reclaimed);
#elif defined(USE_JEMALLOC)
    // Jemalloc does it's own internal trimming.
#else
    UNIMPLEMENTED(WARNING) << "Add trimming support";
#endif
  }
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
  return IsAligned<kObjectAlignment>(obj) && FindSpaceFromObject(obj, true) != nullptr;
}

bool Heap::IsNonDiscontinuousSpaceHeapAddress(const mirror::Object* obj) const {
  return FindContinuousSpaceFromObject(obj, true) != nullptr;
}

bool Heap::IsValidContinuousSpaceObjectAddress(const mirror::Object* obj) const {
  if (obj == nullptr || !IsAligned<kObjectAlignment>(obj)) {
    return false;
  }
  for (const auto& space : continuous_spaces_) {
    if (space->HasAddress(obj)) {
      return true;
    }
  }
  return false;
}

bool Heap::IsLiveObjectLocked(mirror::Object* obj, bool search_allocation_stack,
                              bool search_live_stack, bool sorted) {
  if (UNLIKELY(!IsAligned<kObjectAlignment>(obj))) {
    return false;
  }
  if (bump_pointer_space_ != nullptr && bump_pointer_space_->HasAddress(obj)) {
    mirror::Class* klass = obj->GetClass<kVerifyNone>();
    if (obj == klass) {
      // This case happens for java.lang.Class.
      return true;
    }
    return VerifyClassClass(klass) && IsLiveObjectLocked(klass);
  } else if (temp_space_ != nullptr && temp_space_->HasAddress(obj)) {
    // If we are in the allocated region of the temp space, then we are probably live (e.g. during
    // a GC). When a GC isn't running End() - Begin() is 0 which means no objects are contained.
    return temp_space_->Contains(obj);
  }
  space::ContinuousSpace* c_space = FindContinuousSpaceFromObject(obj, true);
  space::DiscontinuousSpace* d_space = nullptr;
  if (c_space != nullptr) {
    if (c_space->GetLiveBitmap()->Test(obj)) {
      return true;
    }
  } else {
    d_space = FindDiscontinuousSpaceFromObject(obj, true);
    if (d_space != nullptr) {
      if (d_space->GetLiveBitmap()->Test(obj)) {
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
        if (allocation_stack_->ContainsSorted(obj)) {
          return true;
        }
      } else if (allocation_stack_->Contains(obj)) {
        return true;
      }
    }

    if (search_live_stack) {
      if (sorted) {
        if (live_stack_->ContainsSorted(obj)) {
          return true;
        }
      } else if (live_stack_->Contains(obj)) {
        return true;
      }
    }
  }
  // We need to check the bitmaps again since there is a race where we mark something as live and
  // then clear the stack containing it.
  if (c_space != nullptr) {
    if (c_space->GetLiveBitmap()->Test(obj)) {
      return true;
    }
  } else {
    d_space = FindDiscontinuousSpaceFromObject(obj, true);
    if (d_space != nullptr && d_space->GetLiveBitmap()->Test(obj)) {
      return true;
    }
  }
  return false;
}

std::string Heap::DumpSpaces() const {
  std::ostringstream oss;
  DumpSpaces(oss);
  return oss.str();
}

void Heap::DumpSpaces(std::ostream& stream) const {
  for (const auto& space : continuous_spaces_) {
    accounting::ContinuousSpaceBitmap* live_bitmap = space->GetLiveBitmap();
    accounting::ContinuousSpaceBitmap* mark_bitmap = space->GetMarkBitmap();
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

void Heap::VerifyObjectBody(mirror::Object* obj) {
  if (verify_object_mode_ == kVerifyObjectModeDisabled) {
    return;
  }

  // Ignore early dawn of the universe verifications.
  if (UNLIKELY(static_cast<size_t>(num_bytes_allocated_.LoadRelaxed()) < 10 * KB)) {
    return;
  }
  CHECK(IsAligned<kObjectAlignment>(obj)) << "Object isn't aligned: " << obj;
  mirror::Class* c = obj->GetFieldObject<mirror::Class, kVerifyNone>(mirror::Object::ClassOffset());
  CHECK(c != nullptr) << "Null class in object " << obj;
  CHECK(IsAligned<kObjectAlignment>(c)) << "Class " << c << " not aligned in object " << obj;
  CHECK(VerifyClassClass(c));

  if (verify_object_mode_ > kVerifyObjectModeFast) {
    // Note: the bitmap tests below are racy since we don't hold the heap bitmap lock.
    CHECK(IsLiveObjectLocked(obj)) << "Object is dead " << obj << "\n" << DumpSpaces();
  }
}

void Heap::VerificationCallback(mirror::Object* obj, void* arg) {
  reinterpret_cast<Heap*>(arg)->VerifyObjectBody(obj);
}

void Heap::VerifyHeap() {
  ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  GetLiveBitmap()->Walk(Heap::VerificationCallback, this);
}

void Heap::RecordFree(uint64_t freed_objects, int64_t freed_bytes) {
  // Use signed comparison since freed bytes can be negative when background compaction foreground
  // transitions occurs. This is caused by the moving objects from a bump pointer space to a
  // free list backed space typically increasing memory footprint due to padding and binning.
  DCHECK_LE(freed_bytes, static_cast<int64_t>(num_bytes_allocated_.LoadRelaxed()));
  // Note: This relies on 2s complement for handling negative freed_bytes.
  num_bytes_allocated_.FetchAndSubSequentiallyConsistent(static_cast<ssize_t>(freed_bytes));
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

space::RosAllocSpace* Heap::GetRosAllocSpace(gc::allocator::RosAlloc* rosalloc) const {
  for (const auto& space : continuous_spaces_) {
    if (space->AsContinuousSpace()->IsRosAllocSpace()) {
      if (space->AsContinuousSpace()->AsRosAllocSpace()->GetRosAlloc() == rosalloc) {
        return space->AsContinuousSpace()->AsRosAllocSpace();
      }
    }
  }
  return nullptr;
}

mirror::Object* Heap::AllocateInternalWithGc(Thread* self, AllocatorType allocator,
                                             size_t alloc_size, size_t* bytes_allocated,
                                             size_t* usable_size,
                                             mirror::Class** klass) {
  bool was_default_allocator = allocator == GetCurrentAllocator();
  // Make sure there is no pending exception since we may need to throw an OOME.
  self->AssertNoPendingException();
  DCHECK(klass != nullptr);
  StackHandleScope<1> hs(self);
  HandleWrapper<mirror::Class> h(hs.NewHandleWrapper(klass));
  klass = nullptr;  // Invalidate for safety.
  // The allocation failed. If the GC is running, block until it completes, and then retry the
  // allocation.
  collector::GcType last_gc = WaitForGcToComplete(kGcCauseForAlloc, self);
  if (last_gc != collector::kGcTypeNone) {
    // If we were the default allocator but the allocator changed while we were suspended,
    // abort the allocation.
    if (was_default_allocator && allocator != GetCurrentAllocator()) {
      return nullptr;
    }
    // A GC was in progress and we blocked, retry allocation now that memory has been freed.
    mirror::Object* ptr = TryToAllocate<true, false>(self, allocator, alloc_size, bytes_allocated,
                                                     usable_size);
    if (ptr != nullptr) {
      return ptr;
    }
  }

  collector::GcType tried_type = next_gc_type_;
  const bool gc_ran =
      CollectGarbageInternal(tried_type, kGcCauseForAlloc, false) != collector::kGcTypeNone;
  if (was_default_allocator && allocator != GetCurrentAllocator()) {
    return nullptr;
  }
  if (gc_ran) {
    mirror::Object* ptr = TryToAllocate<true, false>(self, allocator, alloc_size, bytes_allocated,
                                                     usable_size);
    if (ptr != nullptr) {
      return ptr;
    }
  }

  // Loop through our different Gc types and try to Gc until we get enough free memory.
  for (collector::GcType gc_type : gc_plan_) {
    if (gc_type == tried_type) {
      continue;
    }
    // Attempt to run the collector, if we succeed, re-try the allocation.
    const bool gc_ran =
        CollectGarbageInternal(gc_type, kGcCauseForAlloc, false) != collector::kGcTypeNone;
    if (was_default_allocator && allocator != GetCurrentAllocator()) {
      return nullptr;
    }
    if (gc_ran) {
      // Did we free sufficient memory for the allocation to succeed?
      mirror::Object* ptr = TryToAllocate<true, false>(self, allocator, alloc_size, bytes_allocated,
                                                       usable_size);
      if (ptr != nullptr) {
        return ptr;
      }
    }
  }
  // Allocations have failed after GCs;  this is an exceptional state.
  // Try harder, growing the heap if necessary.
  mirror::Object* ptr = TryToAllocate<true, true>(self, allocator, alloc_size, bytes_allocated,
                                                  usable_size);
  if (ptr != nullptr) {
    return ptr;
  }
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
  if (was_default_allocator && allocator != GetCurrentAllocator()) {
    return nullptr;
  }
  ptr = TryToAllocate<true, true>(self, allocator, alloc_size, bytes_allocated, usable_size);
  if (ptr == nullptr) {
    const uint64_t current_time = NanoTime();
    switch (allocator) {
      case kAllocatorTypeRosAlloc:
        // Fall-through.
      case kAllocatorTypeDlMalloc: {
        if (use_homogeneous_space_compaction_for_oom_ &&
            current_time - last_time_homogeneous_space_compaction_by_oom_ >
            min_interval_homogeneous_space_compaction_by_oom_) {
          last_time_homogeneous_space_compaction_by_oom_ = current_time;
          HomogeneousSpaceCompactResult result = PerformHomogeneousSpaceCompact();
          switch (result) {
            case HomogeneousSpaceCompactResult::kSuccess:
              // If the allocation succeeded, we delayed an oom.
              ptr = TryToAllocate<true, true>(self, allocator, alloc_size, bytes_allocated,
                                              usable_size);
              if (ptr != nullptr) {
                count_delayed_oom_++;
              }
              break;
            case HomogeneousSpaceCompactResult::kErrorReject:
              // Reject due to disabled moving GC.
              break;
            case HomogeneousSpaceCompactResult::kErrorVMShuttingDown:
              // Throw OOM by default.
              break;
            default: {
              LOG(FATAL) << "Unimplemented homogeneous space compaction result "
                         << static_cast<size_t>(result);
            }
          }
          // Always print that we ran homogeneous space compation since this can cause jank.
          VLOG(heap) << "Ran heap homogeneous space compaction, "
                    << " requested defragmentation "
                    << count_requested_homogeneous_space_compaction_.LoadSequentiallyConsistent()
                    << " performed defragmentation "
                    << count_performed_homogeneous_space_compaction_.LoadSequentiallyConsistent()
                    << " ignored homogeneous space compaction "
                    << count_ignored_homogeneous_space_compaction_.LoadSequentiallyConsistent()
                    << " delayed count = "
                    << count_delayed_oom_.LoadSequentiallyConsistent();
        }
        break;
      }
      case kAllocatorTypeNonMoving: {
        // Try to transition the heap if the allocation failure was due to the space being full.
        if (!IsOutOfMemoryOnAllocation<false>(allocator, alloc_size)) {
          // If we aren't out of memory then the OOM was probably from the non moving space being
          // full. Attempt to disable compaction and turn the main space into a non moving space.
          DisableMovingGc();
          // If we are still a moving GC then something must have caused the transition to fail.
          if (IsMovingGc(collector_type_)) {
            MutexLock mu(self, *gc_complete_lock_);
            // If we couldn't disable moving GC, just throw OOME and return null.
            LOG(WARNING) << "Couldn't disable moving GC with disable GC count "
                         << disable_moving_gc_count_;
          } else {
            LOG(WARNING) << "Disabled moving GC due to the non moving space being full";
            ptr = TryToAllocate<true, true>(self, allocator, alloc_size, bytes_allocated,
                                            usable_size);
          }
        }
        break;
      }
      default: {
        // Do nothing for others allocators.
      }
    }
  }
  // If the allocation hasn't succeeded by this point, throw an OOM error.
  if (ptr == nullptr) {
    ThrowOutOfMemoryError(self, alloc_size, allocator);
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

uint64_t Heap::GetObjectsAllocatedEver() const {
  return GetObjectsFreedEver() + GetObjectsAllocated();
}

uint64_t Heap::GetBytesAllocatedEver() const {
  return GetBytesFreedEver() + GetBytesAllocated();
}

class InstanceCounter {
 public:
  InstanceCounter(const std::vector<mirror::Class*>& classes, bool use_is_assignable_from, uint64_t* counts)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : classes_(classes), use_is_assignable_from_(use_is_assignable_from), counts_(counts) {
  }
  static void Callback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    InstanceCounter* instance_counter = reinterpret_cast<InstanceCounter*>(arg);
    mirror::Class* instance_class = obj->GetClass();
    CHECK(instance_class != nullptr);
    for (size_t i = 0; i < instance_counter->classes_.size(); ++i) {
      if (instance_counter->use_is_assignable_from_) {
        if (instance_counter->classes_[i]->IsAssignableFrom(instance_class)) {
          ++instance_counter->counts_[i];
        }
      } else if (instance_class == instance_counter->classes_[i]) {
        ++instance_counter->counts_[i];
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
  // Can't do any GC in this function since this may move classes.
  Thread* self = Thread::Current();
  auto* old_cause = self->StartAssertNoThreadSuspension("CountInstances");
  InstanceCounter counter(classes, use_is_assignable_from, counts);
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  VisitObjects(InstanceCounter::Callback, &counter);
  self->EndAssertNoThreadSuspension(old_cause);
}

class InstanceCollector {
 public:
  InstanceCollector(mirror::Class* c, int32_t max_count, std::vector<mirror::Object*>& instances)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : class_(c), max_count_(max_count), instances_(instances) {
  }
  static void Callback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    DCHECK(arg != nullptr);
    InstanceCollector* instance_collector = reinterpret_cast<InstanceCollector*>(arg);
    mirror::Class* instance_class = obj->GetClass();
    if (instance_class == instance_collector->class_) {
      if (instance_collector->max_count_ == 0 ||
          instance_collector->instances_.size() < instance_collector->max_count_) {
        instance_collector->instances_.push_back(obj);
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
  // Can't do any GC in this function since this may move classes.
  Thread* self = Thread::Current();
  auto* old_cause = self->StartAssertNoThreadSuspension("GetInstances");
  InstanceCollector collector(c, max_count, instances);
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  VisitObjects(&InstanceCollector::Callback, &collector);
  self->EndAssertNoThreadSuspension(old_cause);
}

class ReferringObjectsFinder {
 public:
  ReferringObjectsFinder(mirror::Object* object, int32_t max_count,
                         std::vector<mirror::Object*>& referring_objects)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : object_(object), max_count_(max_count), referring_objects_(referring_objects) {
  }

  static void Callback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    reinterpret_cast<ReferringObjectsFinder*>(arg)->operator()(obj);
  }

  // For bitmap Visit.
  // TODO: Fix lock analysis to not use NO_THREAD_SAFETY_ANALYSIS, requires support for
  // annotalysis on visitors.
  void operator()(mirror::Object* o) const NO_THREAD_SAFETY_ANALYSIS {
    o->VisitReferences<true>(*this, VoidFunctor());
  }

  // For Object::VisitReferences.
  void operator()(mirror::Object* obj, MemberOffset offset, bool /* is_static */) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::Object* ref = obj->GetFieldObject<mirror::Object>(offset);
    if (ref == object_ && (max_count_ == 0 || referring_objects_.size() < max_count_)) {
      referring_objects_.push_back(obj);
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
  // Can't do any GC in this function since this may move the object o.
  Thread* self = Thread::Current();
  auto* old_cause = self->StartAssertNoThreadSuspension("GetReferringObjects");
  ReferringObjectsFinder finder(o, max_count, referring_objects);
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  VisitObjects(&ReferringObjectsFinder::Callback, &finder);
  self->EndAssertNoThreadSuspension(old_cause);
}

void Heap::CollectGarbage(bool clear_soft_references) {
  // Even if we waited for a GC we still need to do another GC since weaks allocated during the
  // last GC will not have necessarily been cleared.
  CollectGarbageInternal(gc_plan_.back(), kGcCauseExplicit, clear_soft_references);
}

HomogeneousSpaceCompactResult Heap::PerformHomogeneousSpaceCompact() {
  Thread* self = Thread::Current();
  // Inc requested homogeneous space compaction.
  count_requested_homogeneous_space_compaction_++;
  // Store performed homogeneous space compaction at a new request arrival.
  ThreadList* tl = Runtime::Current()->GetThreadList();
  ScopedThreadStateChange tsc(self, kWaitingPerformingGc);
  Locks::mutator_lock_->AssertNotHeld(self);
  {
    ScopedThreadStateChange tsc(self, kWaitingForGcToComplete);
    MutexLock mu(self, *gc_complete_lock_);
    // Ensure there is only one GC at a time.
    WaitForGcToCompleteLocked(kGcCauseHomogeneousSpaceCompact, self);
    // Homogeneous space compaction is a copying transition, can't run it if the moving GC disable count
    // is non zero.
    // If the collector type changed to something which doesn't benefit from homogeneous space compaction,
    // exit.
    if (disable_moving_gc_count_ != 0 || IsMovingGc(collector_type_) ||
        !main_space_->CanMoveObjects()) {
      return HomogeneousSpaceCompactResult::kErrorReject;
    }
    collector_type_running_ = kCollectorTypeHomogeneousSpaceCompact;
  }
  if (Runtime::Current()->IsShuttingDown(self)) {
    // Don't allow heap transitions to happen if the runtime is shutting down since these can
    // cause objects to get finalized.
    FinishGC(self, collector::kGcTypeNone);
    return HomogeneousSpaceCompactResult::kErrorVMShuttingDown;
  }
  // Suspend all threads.
  tl->SuspendAll();
  uint64_t start_time = NanoTime();
  // Launch compaction.
  space::MallocSpace* to_space = main_space_backup_.release();
  space::MallocSpace* from_space = main_space_;
  to_space->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
  const uint64_t space_size_before_compaction = from_space->Size();
  AddSpace(to_space);
  Compact(to_space, from_space, kGcCauseHomogeneousSpaceCompact);
  // Leave as prot read so that we can still run ROSAlloc verification on this space.
  from_space->GetMemMap()->Protect(PROT_READ);
  const uint64_t space_size_after_compaction = to_space->Size();
  main_space_ = to_space;
  main_space_backup_.reset(from_space);
  RemoveSpace(from_space);
  SetSpaceAsDefault(main_space_);  // Set as default to reset the proper dlmalloc space.
  // Update performed homogeneous space compaction count.
  count_performed_homogeneous_space_compaction_++;
  // Print statics log and resume all threads.
  uint64_t duration = NanoTime() - start_time;
  VLOG(heap) << "Heap homogeneous space compaction took " << PrettyDuration(duration) << " size: "
             << PrettySize(space_size_before_compaction) << " -> "
             << PrettySize(space_size_after_compaction) << " compact-ratio: "
             << std::fixed << static_cast<double>(space_size_after_compaction) /
             static_cast<double>(space_size_before_compaction);
  tl->ResumeAll();
  // Finish GC.
  reference_processor_.EnqueueClearedReferences(self);
  GrowForUtilization(semi_space_collector_);
  FinishGC(self, collector::kGcTypeFull);
  return HomogeneousSpaceCompactResult::kSuccess;
}


void Heap::TransitionCollector(CollectorType collector_type) {
  if (collector_type == collector_type_) {
    return;
  }
  VLOG(heap) << "TransitionCollector: " << static_cast<int>(collector_type_)
             << " -> " << static_cast<int>(collector_type);
  uint64_t start_time = NanoTime();
  uint32_t before_allocated = num_bytes_allocated_.LoadSequentiallyConsistent();
  Runtime* const runtime = Runtime::Current();
  ThreadList* const tl = runtime->GetThreadList();
  Thread* const self = Thread::Current();
  ScopedThreadStateChange tsc(self, kWaitingPerformingGc);
  Locks::mutator_lock_->AssertNotHeld(self);
  // Busy wait until we can GC (StartGC can fail if we have a non-zero
  // compacting_gc_disable_count_, this should rarely occurs).
  for (;;) {
    {
      ScopedThreadStateChange tsc(self, kWaitingForGcToComplete);
      MutexLock mu(self, *gc_complete_lock_);
      // Ensure there is only one GC at a time.
      WaitForGcToCompleteLocked(kGcCauseCollectorTransition, self);
      // Currently we only need a heap transition if we switch from a moving collector to a
      // non-moving one, or visa versa.
      const bool copying_transition = IsMovingGc(collector_type_) != IsMovingGc(collector_type);
      // If someone else beat us to it and changed the collector before we could, exit.
      // This is safe to do before the suspend all since we set the collector_type_running_ before
      // we exit the loop. If another thread attempts to do the heap transition before we exit,
      // then it would get blocked on WaitForGcToCompleteLocked.
      if (collector_type == collector_type_) {
        return;
      }
      // GC can be disabled if someone has a used GetPrimitiveArrayCritical but not yet released.
      if (!copying_transition || disable_moving_gc_count_ == 0) {
        // TODO: Not hard code in semi-space collector?
        collector_type_running_ = copying_transition ? kCollectorTypeSS : collector_type;
        break;
      }
    }
    usleep(1000);
  }
  if (runtime->IsShuttingDown(self)) {
    // Don't allow heap transitions to happen if the runtime is shutting down since these can
    // cause objects to get finalized.
    FinishGC(self, collector::kGcTypeNone);
    return;
  }
  tl->SuspendAll();
  switch (collector_type) {
    case kCollectorTypeSS: {
      if (!IsMovingGc(collector_type_)) {
        // Create the bump pointer space from the backup space.
        CHECK(main_space_backup_ != nullptr);
        std::unique_ptr<MemMap> mem_map(main_space_backup_->ReleaseMemMap());
        // We are transitioning from non moving GC -> moving GC, since we copied from the bump
        // pointer space last transition it will be protected.
        CHECK(mem_map != nullptr);
        mem_map->Protect(PROT_READ | PROT_WRITE);
        bump_pointer_space_ = space::BumpPointerSpace::CreateFromMemMap("Bump pointer space",
                                                                        mem_map.release());
        AddSpace(bump_pointer_space_);
        Compact(bump_pointer_space_, main_space_, kGcCauseCollectorTransition);
        // Use the now empty main space mem map for the bump pointer temp space.
        mem_map.reset(main_space_->ReleaseMemMap());
        // Unset the pointers just in case.
        if (dlmalloc_space_ == main_space_) {
          dlmalloc_space_ = nullptr;
        } else if (rosalloc_space_ == main_space_) {
          rosalloc_space_ = nullptr;
        }
        // Remove the main space so that we don't try to trim it, this doens't work for debug
        // builds since RosAlloc attempts to read the magic number from a protected page.
        RemoveSpace(main_space_);
        RemoveRememberedSet(main_space_);
        delete main_space_;  // Delete the space since it has been removed.
        main_space_ = nullptr;
        RemoveRememberedSet(main_space_backup_.get());
        main_space_backup_.reset(nullptr);  // Deletes the space.
        temp_space_ = space::BumpPointerSpace::CreateFromMemMap("Bump pointer space 2",
                                                                mem_map.release());
        AddSpace(temp_space_);
      }
      break;
    }
    case kCollectorTypeMS:
      // Fall through.
    case kCollectorTypeCMS: {
      if (IsMovingGc(collector_type_)) {
        CHECK(temp_space_ != nullptr);
        std::unique_ptr<MemMap> mem_map(temp_space_->ReleaseMemMap());
        RemoveSpace(temp_space_);
        temp_space_ = nullptr;
        mem_map->Protect(PROT_READ | PROT_WRITE);
        CreateMainMallocSpace(mem_map.get(), kDefaultInitialSize, mem_map->Size(),
                              mem_map->Size());
        mem_map.release();
        // Compact to the main space from the bump pointer space, don't need to swap semispaces.
        AddSpace(main_space_);
        Compact(main_space_, bump_pointer_space_, kGcCauseCollectorTransition);
        mem_map.reset(bump_pointer_space_->ReleaseMemMap());
        RemoveSpace(bump_pointer_space_);
        bump_pointer_space_ = nullptr;
        const char* name = kUseRosAlloc ? kRosAllocSpaceName[1] : kDlMallocSpaceName[1];
        // Temporarily unprotect the backup mem map so rosalloc can write the debug magic number.
        if (kIsDebugBuild && kUseRosAlloc) {
          mem_map->Protect(PROT_READ | PROT_WRITE);
        }
        main_space_backup_.reset(CreateMallocSpaceFromMemMap(mem_map.get(), kDefaultInitialSize,
                                                             mem_map->Size(), mem_map->Size(),
                                                             name, true));
        if (kIsDebugBuild && kUseRosAlloc) {
          mem_map->Protect(PROT_NONE);
        }
        mem_map.release();
      }
      break;
    }
    default: {
      LOG(FATAL) << "Attempted to transition to invalid collector type "
                 << static_cast<size_t>(collector_type);
      break;
    }
  }
  ChangeCollector(collector_type);
  tl->ResumeAll();
  // Can't call into java code with all threads suspended.
  reference_processor_.EnqueueClearedReferences(self);
  uint64_t duration = NanoTime() - start_time;
  GrowForUtilization(semi_space_collector_);
  FinishGC(self, collector::kGcTypeFull);
  int32_t after_allocated = num_bytes_allocated_.LoadSequentiallyConsistent();
  int32_t delta_allocated = before_allocated - after_allocated;
  std::string saved_str;
  if (delta_allocated >= 0) {
    saved_str = " saved at least " + PrettySize(delta_allocated);
  } else {
    saved_str = " expanded " + PrettySize(-delta_allocated);
  }
  VLOG(heap) << "Heap transition to " << process_state_ << " took "
      << PrettyDuration(duration) << saved_str;
}

void Heap::ChangeCollector(CollectorType collector_type) {
  // TODO: Only do this with all mutators suspended to avoid races.
  if (collector_type != collector_type_) {
    if (collector_type == kCollectorTypeMC) {
      // Don't allow mark compact unless support is compiled in.
      CHECK(kMarkCompactSupport);
    }
    collector_type_ = collector_type;
    gc_plan_.clear();
    switch (collector_type_) {
      case kCollectorTypeCC:  // Fall-through.
      case kCollectorTypeMC:  // Fall-through.
      case kCollectorTypeSS:  // Fall-through.
      case kCollectorTypeGSS: {
        gc_plan_.push_back(collector::kGcTypeFull);
        if (use_tlab_) {
          ChangeAllocator(kAllocatorTypeTLAB);
        } else {
          ChangeAllocator(kAllocatorTypeBumpPointer);
        }
        break;
      }
      case kCollectorTypeMS: {
        gc_plan_.push_back(collector::kGcTypeSticky);
        gc_plan_.push_back(collector::kGcTypePartial);
        gc_plan_.push_back(collector::kGcTypeFull);
        ChangeAllocator(kUseRosAlloc ? kAllocatorTypeRosAlloc : kAllocatorTypeDlMalloc);
        break;
      }
      case kCollectorTypeCMS: {
        gc_plan_.push_back(collector::kGcTypeSticky);
        gc_plan_.push_back(collector::kGcTypePartial);
        gc_plan_.push_back(collector::kGcTypeFull);
        ChangeAllocator(kUseRosAlloc ? kAllocatorTypeRosAlloc : kAllocatorTypeDlMalloc);
        break;
      }
      default: {
        LOG(FATAL) << "Unimplemented";
      }
    }
    if (IsGcConcurrent()) {
      concurrent_start_bytes_ =
          std::max(max_allowed_footprint_, kMinConcurrentRemainingBytes) - kMinConcurrentRemainingBytes;
    } else {
      concurrent_start_bytes_ = std::numeric_limits<size_t>::max();
    }
  }
}

// Special compacting collector which uses sub-optimal bin packing to reduce zygote space size.
class ZygoteCompactingCollector FINAL : public collector::SemiSpace {
 public:
  explicit ZygoteCompactingCollector(gc::Heap* heap) : SemiSpace(heap, false, "zygote collector"),
      bin_live_bitmap_(nullptr), bin_mark_bitmap_(nullptr) {
  }

  void BuildBins(space::ContinuousSpace* space) {
    bin_live_bitmap_ = space->GetLiveBitmap();
    bin_mark_bitmap_ = space->GetMarkBitmap();
    BinContext context;
    context.prev_ = reinterpret_cast<uintptr_t>(space->Begin());
    context.collector_ = this;
    WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
    // Note: This requires traversing the space in increasing order of object addresses.
    bin_live_bitmap_->Walk(Callback, reinterpret_cast<void*>(&context));
    // Add the last bin which spans after the last object to the end of the space.
    AddBin(reinterpret_cast<uintptr_t>(space->End()) - context.prev_, context.prev_);
  }

 private:
  struct BinContext {
    uintptr_t prev_;  // The end of the previous object.
    ZygoteCompactingCollector* collector_;
  };
  // Maps from bin sizes to locations.
  std::multimap<size_t, uintptr_t> bins_;
  // Live bitmap of the space which contains the bins.
  accounting::ContinuousSpaceBitmap* bin_live_bitmap_;
  // Mark bitmap of the space which contains the bins.
  accounting::ContinuousSpaceBitmap* bin_mark_bitmap_;

  static void Callback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(arg != nullptr);
    BinContext* context = reinterpret_cast<BinContext*>(arg);
    ZygoteCompactingCollector* collector = context->collector_;
    uintptr_t object_addr = reinterpret_cast<uintptr_t>(obj);
    size_t bin_size = object_addr - context->prev_;
    // Add the bin consisting of the end of the previous object to the start of the current object.
    collector->AddBin(bin_size, context->prev_);
    context->prev_ = object_addr + RoundUp(obj->SizeOf(), kObjectAlignment);
  }

  void AddBin(size_t size, uintptr_t position) {
    if (size != 0) {
      bins_.insert(std::make_pair(size, position));
    }
  }

  virtual bool ShouldSweepSpace(space::ContinuousSpace* space) const {
    // Don't sweep any spaces since we probably blasted the internal accounting of the free list
    // allocator.
    return false;
  }

  virtual mirror::Object* MarkNonForwardedObject(mirror::Object* obj)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
    size_t object_size = RoundUp(obj->SizeOf(), kObjectAlignment);
    mirror::Object* forward_address;
    // Find the smallest bin which we can move obj in.
    auto it = bins_.lower_bound(object_size);
    if (it == bins_.end()) {
      // No available space in the bins, place it in the target space instead (grows the zygote
      // space).
      size_t bytes_allocated;
      forward_address = to_space_->Alloc(self_, object_size, &bytes_allocated, nullptr);
      if (to_space_live_bitmap_ != nullptr) {
        to_space_live_bitmap_->Set(forward_address);
      } else {
        GetHeap()->GetNonMovingSpace()->GetLiveBitmap()->Set(forward_address);
        GetHeap()->GetNonMovingSpace()->GetMarkBitmap()->Set(forward_address);
      }
    } else {
      size_t size = it->first;
      uintptr_t pos = it->second;
      bins_.erase(it);  // Erase the old bin which we replace with the new smaller bin.
      forward_address = reinterpret_cast<mirror::Object*>(pos);
      // Set the live and mark bits so that sweeping system weaks works properly.
      bin_live_bitmap_->Set(forward_address);
      bin_mark_bitmap_->Set(forward_address);
      DCHECK_GE(size, object_size);
      AddBin(size - object_size, pos + object_size);  // Add a new bin with the remaining space.
    }
    // Copy the object over to its new location.
    memcpy(reinterpret_cast<void*>(forward_address), obj, object_size);
    if (kUseBakerOrBrooksReadBarrier) {
      obj->AssertReadBarrierPointer();
      if (kUseBrooksReadBarrier) {
        DCHECK_EQ(forward_address->GetReadBarrierPointer(), obj);
        forward_address->SetReadBarrierPointer(forward_address);
      }
      forward_address->AssertReadBarrierPointer();
    }
    return forward_address;
  }
};

void Heap::UnBindBitmaps() {
  TimingLogger::ScopedTiming t("UnBindBitmaps", GetCurrentGcIteration()->GetTimings());
  for (const auto& space : GetContinuousSpaces()) {
    if (space->IsContinuousMemMapAllocSpace()) {
      space::ContinuousMemMapAllocSpace* alloc_space = space->AsContinuousMemMapAllocSpace();
      if (alloc_space->HasBoundBitmaps()) {
        alloc_space->UnBindBitmaps();
      }
    }
  }
}

void Heap::PreZygoteFork() {
  CollectGarbageInternal(collector::kGcTypeFull, kGcCauseBackground, false);
  Thread* self = Thread::Current();
  MutexLock mu(self, zygote_creation_lock_);
  // Try to see if we have any Zygote spaces.
  if (have_zygote_space_) {
    return;
  }
  VLOG(heap) << "Starting PreZygoteFork";
  // Trim the pages at the end of the non moving space.
  non_moving_space_->Trim();
  // The end of the non-moving space may be protected, unprotect it so that we can copy the zygote
  // there.
  non_moving_space_->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
  const bool same_space = non_moving_space_ == main_space_;
  if (kCompactZygote) {
    // Can't compact if the non moving space is the same as the main space.
    DCHECK(semi_space_collector_ != nullptr);
    // Temporarily disable rosalloc verification because the zygote
    // compaction will mess up the rosalloc internal metadata.
    ScopedDisableRosAllocVerification disable_rosalloc_verif(this);
    ZygoteCompactingCollector zygote_collector(this);
    zygote_collector.BuildBins(non_moving_space_);
    // Create a new bump pointer space which we will compact into.
    space::BumpPointerSpace target_space("zygote bump space", non_moving_space_->End(),
                                         non_moving_space_->Limit());
    // Compact the bump pointer space to a new zygote bump pointer space.
    bool reset_main_space = false;
    if (IsMovingGc(collector_type_)) {
      zygote_collector.SetFromSpace(bump_pointer_space_);
    } else {
      CHECK(main_space_ != nullptr);
      // Copy from the main space.
      zygote_collector.SetFromSpace(main_space_);
      reset_main_space = true;
    }
    zygote_collector.SetToSpace(&target_space);
    zygote_collector.SetSwapSemiSpaces(false);
    zygote_collector.Run(kGcCauseCollectorTransition, false);
    if (reset_main_space) {
      main_space_->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
      madvise(main_space_->Begin(), main_space_->Capacity(), MADV_DONTNEED);
      MemMap* mem_map = main_space_->ReleaseMemMap();
      RemoveSpace(main_space_);
      space::Space* old_main_space = main_space_;
      CreateMainMallocSpace(mem_map, kDefaultInitialSize, mem_map->Size(), mem_map->Size());
      delete old_main_space;
      AddSpace(main_space_);
    } else {
      bump_pointer_space_->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
    }
    if (temp_space_ != nullptr) {
      CHECK(temp_space_->IsEmpty());
    }
    total_objects_freed_ever_ += GetCurrentGcIteration()->GetFreedObjects();
    total_bytes_freed_ever_ += GetCurrentGcIteration()->GetFreedBytes();
    // Update the end and write out image.
    non_moving_space_->SetEnd(target_space.End());
    non_moving_space_->SetLimit(target_space.Limit());
    VLOG(heap) << "Zygote space size " << non_moving_space_->Size() << " bytes";
  }
  // Change the collector to the post zygote one.
  ChangeCollector(foreground_collector_type_);
  // Save the old space so that we can remove it after we complete creating the zygote space.
  space::MallocSpace* old_alloc_space = non_moving_space_;
  // Turn the current alloc space into a zygote space and obtain the new alloc space composed of
  // the remaining available space.
  // Remove the old space before creating the zygote space since creating the zygote space sets
  // the old alloc space's bitmaps to nullptr.
  RemoveSpace(old_alloc_space);
  if (collector::SemiSpace::kUseRememberedSet) {
    // Sanity bound check.
    FindRememberedSetFromSpace(old_alloc_space)->AssertAllDirtyCardsAreWithinSpace();
    // Remove the remembered set for the now zygote space (the old
    // non-moving space). Note now that we have compacted objects into
    // the zygote space, the data in the remembered set is no longer
    // needed. The zygote space will instead have a mod-union table
    // from this point on.
    RemoveRememberedSet(old_alloc_space);
  }
  space::ZygoteSpace* zygote_space = old_alloc_space->CreateZygoteSpace("alloc space",
                                                                        low_memory_mode_,
                                                                        &non_moving_space_);
  CHECK(!non_moving_space_->CanMoveObjects());
  if (same_space) {
    main_space_ = non_moving_space_;
    SetSpaceAsDefault(main_space_);
  }
  delete old_alloc_space;
  CHECK(zygote_space != nullptr) << "Failed creating zygote space";
  AddSpace(zygote_space);
  non_moving_space_->SetFootprintLimit(non_moving_space_->Capacity());
  AddSpace(non_moving_space_);
  have_zygote_space_ = true;
  // Enable large object space allocations.
  large_object_threshold_ = kDefaultLargeObjectThreshold;
  // Create the zygote space mod union table.
  accounting::ModUnionTable* mod_union_table =
      new accounting::ModUnionTableCardCache("zygote space mod-union table", this, zygote_space);
  CHECK(mod_union_table != nullptr) << "Failed to create zygote space mod-union table";
  AddModUnionTable(mod_union_table);
  if (collector::SemiSpace::kUseRememberedSet) {
    // Add a new remembered set for the post-zygote non-moving space.
    accounting::RememberedSet* post_zygote_non_moving_space_rem_set =
        new accounting::RememberedSet("Post-zygote non-moving space remembered set", this,
                                      non_moving_space_);
    CHECK(post_zygote_non_moving_space_rem_set != nullptr)
        << "Failed to create post-zygote non-moving space remembered set";
    AddRememberedSet(post_zygote_non_moving_space_rem_set);
  }
}

void Heap::FlushAllocStack() {
  MarkAllocStackAsLive(allocation_stack_.get());
  allocation_stack_->Reset();
}

void Heap::MarkAllocStack(accounting::ContinuousSpaceBitmap* bitmap1,
                          accounting::ContinuousSpaceBitmap* bitmap2,
                          accounting::LargeObjectBitmap* large_objects,
                          accounting::ObjectStack* stack) {
  DCHECK(bitmap1 != nullptr);
  DCHECK(bitmap2 != nullptr);
  mirror::Object** limit = stack->End();
  for (mirror::Object** it = stack->Begin(); it != limit; ++it) {
    const mirror::Object* obj = *it;
    if (!kUseThreadLocalAllocationStack || obj != nullptr) {
      if (bitmap1->HasAddress(obj)) {
        bitmap1->Set(obj);
      } else if (bitmap2->HasAddress(obj)) {
        bitmap2->Set(obj);
      } else {
        large_objects->Set(obj);
      }
    }
  }
}

void Heap::SwapSemiSpaces() {
  CHECK(bump_pointer_space_ != nullptr);
  CHECK(temp_space_ != nullptr);
  std::swap(bump_pointer_space_, temp_space_);
}

void Heap::Compact(space::ContinuousMemMapAllocSpace* target_space,
                   space::ContinuousMemMapAllocSpace* source_space,
                   GcCause gc_cause) {
  CHECK(kMovingCollector);
  if (target_space != source_space) {
    // Don't swap spaces since this isn't a typical semi space collection.
    semi_space_collector_->SetSwapSemiSpaces(false);
    semi_space_collector_->SetFromSpace(source_space);
    semi_space_collector_->SetToSpace(target_space);
    semi_space_collector_->Run(gc_cause, false);
  } else {
    CHECK(target_space->IsBumpPointerSpace())
        << "In-place compaction is only supported for bump pointer spaces";
    mark_compact_collector_->SetSpace(target_space->AsBumpPointerSpace());
    mark_compact_collector_->Run(kGcCauseCollectorTransition, false);
  }
}

collector::GcType Heap::CollectGarbageInternal(collector::GcType gc_type, GcCause gc_cause,
                                               bool clear_soft_references) {
  Thread* self = Thread::Current();
  Runtime* runtime = Runtime::Current();
  // If the heap can't run the GC, silently fail and return that no GC was run.
  switch (gc_type) {
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
  bool compacting_gc;
  {
    gc_complete_lock_->AssertNotHeld(self);
    ScopedThreadStateChange tsc(self, kWaitingForGcToComplete);
    MutexLock mu(self, *gc_complete_lock_);
    // Ensure there is only one GC at a time.
    WaitForGcToCompleteLocked(gc_cause, self);
    compacting_gc = IsMovingGc(collector_type_);
    // GC can be disabled if someone has a used GetPrimitiveArrayCritical.
    if (compacting_gc && disable_moving_gc_count_ != 0) {
      LOG(WARNING) << "Skipping GC due to disable moving GC count " << disable_moving_gc_count_;
      return collector::kGcTypeNone;
    }
    collector_type_running_ = collector_type_;
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
    ATRACE_INT("Allocation rate KB/s", allocation_rate_ / KB);
    VLOG(heap) << "Allocation rate: " << PrettySize(allocation_rate_) << "/s";
  }

  DCHECK_LT(gc_type, collector::kGcTypeMax);
  DCHECK_NE(gc_type, collector::kGcTypeNone);

  collector::GarbageCollector* collector = nullptr;
  // TODO: Clean this up.
  if (compacting_gc) {
    DCHECK(current_allocator_ == kAllocatorTypeBumpPointer ||
           current_allocator_ == kAllocatorTypeTLAB);
    switch (collector_type_) {
      case kCollectorTypeSS:
        // Fall-through.
      case kCollectorTypeGSS:
        semi_space_collector_->SetFromSpace(bump_pointer_space_);
        semi_space_collector_->SetToSpace(temp_space_);
        semi_space_collector_->SetSwapSemiSpaces(true);
        collector = semi_space_collector_;
        break;
      case kCollectorTypeCC:
        collector = concurrent_copying_collector_;
        break;
      case kCollectorTypeMC:
        mark_compact_collector_->SetSpace(bump_pointer_space_);
        collector = mark_compact_collector_;
        break;
      default:
        LOG(FATAL) << "Invalid collector type " << static_cast<size_t>(collector_type_);
    }
    if (collector != mark_compact_collector_) {
      temp_space_->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
      CHECK(temp_space_->IsEmpty());
    }
    gc_type = collector::kGcTypeFull;  // TODO: Not hard code this in.
  } else if (current_allocator_ == kAllocatorTypeRosAlloc ||
      current_allocator_ == kAllocatorTypeDlMalloc) {
    collector = FindCollectorByGcType(gc_type);
  } else {
    LOG(FATAL) << "Invalid current allocator " << current_allocator_;
  }
  CHECK(collector != nullptr)
      << "Could not find garbage collector with collector_type="
      << static_cast<size_t>(collector_type_) << " and gc_type=" << gc_type;
  collector->Run(gc_cause, clear_soft_references || runtime->IsZygote());
  total_objects_freed_ever_ += GetCurrentGcIteration()->GetFreedObjects();
  total_bytes_freed_ever_ += GetCurrentGcIteration()->GetFreedBytes();
  RequestHeapTrim();
  // Enqueue cleared references.
  reference_processor_.EnqueueClearedReferences(self);
  // Grow the heap so that we know when to perform the next GC.
  GrowForUtilization(collector);
  const size_t duration = GetCurrentGcIteration()->GetDurationNs();
  const std::vector<uint64_t>& pause_times = GetCurrentGcIteration()->GetPauseTimes();
  // Print the GC if it is an explicit GC (e.g. Runtime.gc()) or a slow GC
  // (mutator time blocked >= long_pause_log_threshold_).
  bool log_gc = gc_cause == kGcCauseExplicit;
  if (!log_gc && CareAboutPauseTimes()) {
    // GC for alloc pauses the allocating thread, so consider it as a pause.
    log_gc = duration > long_gc_log_threshold_ ||
        (gc_cause == kGcCauseForAlloc && duration > long_pause_log_threshold_);
    for (uint64_t pause : pause_times) {
      log_gc = log_gc || pause >= long_pause_log_threshold_;
    }
  }
  if (log_gc) {
    const size_t percent_free = GetPercentFree();
    const size_t current_heap_size = GetBytesAllocated();
    const size_t total_memory = GetTotalMemory();
    std::ostringstream pause_string;
    for (size_t i = 0; i < pause_times.size(); ++i) {
        pause_string << PrettyDuration((pause_times[i] / 1000) * 1000)
                     << ((i != pause_times.size() - 1) ? "," : "");
    }
    LOG(INFO) << gc_cause << " " << collector->GetName()
              << " GC freed "  << current_gc_iteration_.GetFreedObjects() << "("
              << PrettySize(current_gc_iteration_.GetFreedBytes()) << ") AllocSpace objects, "
              << current_gc_iteration_.GetFreedLargeObjects() << "("
              << PrettySize(current_gc_iteration_.GetFreedLargeObjectBytes()) << ") LOS objects, "
              << percent_free << "% free, " << PrettySize(current_heap_size) << "/"
              << PrettySize(total_memory) << ", " << "paused " << pause_string.str()
              << " total " << PrettyDuration((duration / 1000) * 1000);
    VLOG(heap) << ConstDumpable<TimingLogger>(*current_gc_iteration_.GetTimings());
  }
  FinishGC(self, gc_type);
  // Inform DDMS that a GC completed.
  Dbg::GcDidFinish();
  return gc_type;
}

void Heap::FinishGC(Thread* self, collector::GcType gc_type) {
  MutexLock mu(self, *gc_complete_lock_);
  collector_type_running_ = kCollectorTypeNone;
  if (gc_type != collector::kGcTypeNone) {
    last_gc_type_ = gc_type;
  }
  // Wake anyone who may have been waiting for the GC to complete.
  gc_complete_cond_->Broadcast(self);
}

static void RootMatchesObjectVisitor(mirror::Object** root, void* arg, uint32_t /*thread_id*/,
                                     RootType /*root_type*/) {
  mirror::Object* obj = reinterpret_cast<mirror::Object*>(arg);
  if (*root == obj) {
    LOG(INFO) << "Object " << obj << " is a root";
  }
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
  explicit VerifyReferenceVisitor(Heap* heap, Atomic<size_t>* fail_count, bool verify_referent)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_)
      : heap_(heap), fail_count_(fail_count), verify_referent_(verify_referent) {}

  size_t GetFailureCount() const {
    return fail_count_->LoadSequentiallyConsistent();
  }

  void operator()(mirror::Class* klass, mirror::Reference* ref) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (verify_referent_) {
      VerifyReference(ref, ref->GetReferent(), mirror::Reference::ReferentOffset());
    }
  }

  void operator()(mirror::Object* obj, MemberOffset offset, bool /*is_static*/) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    VerifyReference(obj, obj->GetFieldObject<mirror::Object>(offset), offset);
  }

  bool IsLive(mirror::Object* obj) const NO_THREAD_SAFETY_ANALYSIS {
    return heap_->IsLiveObjectLocked(obj, true, false, true);
  }

  static void VerifyRootCallback(mirror::Object** root, void* arg, uint32_t thread_id,
                                 RootType root_type) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    VerifyReferenceVisitor* visitor = reinterpret_cast<VerifyReferenceVisitor*>(arg);
    if (!visitor->VerifyReference(nullptr, *root, MemberOffset(0))) {
      LOG(ERROR) << "Root " << *root << " is dead with type " << PrettyTypeOf(*root)
          << " thread_id= " << thread_id << " root_type= " << root_type;
    }
  }

 private:
  // TODO: Fix the no thread safety analysis.
  // Returns false on failure.
  bool VerifyReference(mirror::Object* obj, mirror::Object* ref, MemberOffset offset) const
      NO_THREAD_SAFETY_ANALYSIS {
    if (ref == nullptr || IsLive(ref)) {
      // Verify that the reference is live.
      return true;
    }
    if (fail_count_->FetchAndAddSequentiallyConsistent(1) == 0) {
      // Print message on only on first failure to prevent spam.
      LOG(ERROR) << "!!!!!!!!!!!!!!Heap corruption detected!!!!!!!!!!!!!!!!!!!";
    }
    if (obj != nullptr) {
      // Only do this part for non roots.
      accounting::CardTable* card_table = heap_->GetCardTable();
      accounting::ObjectStack* alloc_stack = heap_->allocation_stack_.get();
      accounting::ObjectStack* live_stack = heap_->live_stack_.get();
      byte* card_addr = card_table->CardFromAddr(obj);
      LOG(ERROR) << "Object " << obj << " references dead object " << ref << " at offset "
                 << offset << "\n card value = " << static_cast<int>(*card_addr);
      if (heap_->IsValidObjectAddress(obj->GetClass())) {
        LOG(ERROR) << "Obj type " << PrettyTypeOf(obj);
      } else {
        LOG(ERROR) << "Object " << obj << " class(" << obj->GetClass() << ") not a heap address";
      }

      // Attempt to find the class inside of the recently freed objects.
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
      accounting::ContinuousSpaceBitmap* bitmap =
          heap_->GetLiveBitmap()->GetContinuousSpaceBitmap(obj);

      if (bitmap == nullptr) {
        LOG(ERROR) << "Object " << obj << " has no bitmap";
        if (!VerifyClassClass(obj->GetClass())) {
          LOG(ERROR) << "Object " << obj << " failed class verification!";
        }
      } else {
        // Print out how the object is live.
        if (bitmap->Test(obj)) {
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
      }

      // Search to see if any of the roots reference our object.
      void* arg = const_cast<void*>(reinterpret_cast<const void*>(obj));
      Runtime::Current()->VisitRoots(&RootMatchesObjectVisitor, arg);

      // Search to see if any of the roots reference our reference.
      arg = const_cast<void*>(reinterpret_cast<const void*>(ref));
      Runtime::Current()->VisitRoots(&RootMatchesObjectVisitor, arg);
    }
    return false;
  }

  Heap* const heap_;
  Atomic<size_t>* const fail_count_;
  const bool verify_referent_;
};

// Verify all references within an object, for use with HeapBitmap::Visit.
class VerifyObjectVisitor {
 public:
  explicit VerifyObjectVisitor(Heap* heap, Atomic<size_t>* fail_count, bool verify_referent)
      : heap_(heap), fail_count_(fail_count), verify_referent_(verify_referent) {
  }

  void operator()(mirror::Object* obj) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    // Note: we are verifying the references in obj but not obj itself, this is because obj must
    // be live or else how did we find it in the live bitmap?
    VerifyReferenceVisitor visitor(heap_, fail_count_, verify_referent_);
    // The class doesn't count as a reference but we should verify it anyways.
    obj->VisitReferences<true>(visitor, visitor);
  }

  static void VisitCallback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    VerifyObjectVisitor* visitor = reinterpret_cast<VerifyObjectVisitor*>(arg);
    visitor->operator()(obj);
  }

  size_t GetFailureCount() const {
    return fail_count_->LoadSequentiallyConsistent();
  }

 private:
  Heap* const heap_;
  Atomic<size_t>* const fail_count_;
  const bool verify_referent_;
};

void Heap::PushOnAllocationStackWithInternalGC(Thread* self, mirror::Object** obj) {
  // Slow path, the allocation stack push back must have already failed.
  DCHECK(!allocation_stack_->AtomicPushBack(*obj));
  do {
    // TODO: Add handle VerifyObject.
    StackHandleScope<1> hs(self);
    HandleWrapper<mirror::Object> wrapper(hs.NewHandleWrapper(obj));
    // Push our object into the reserve region of the allocaiton stack. This is only required due
    // to heap verification requiring that roots are live (either in the live bitmap or in the
    // allocation stack).
    CHECK(allocation_stack_->AtomicPushBackIgnoreGrowthLimit(*obj));
    CollectGarbageInternal(collector::kGcTypeSticky, kGcCauseForAlloc, false);
  } while (!allocation_stack_->AtomicPushBack(*obj));
}

void Heap::PushOnThreadLocalAllocationStackWithInternalGC(Thread* self, mirror::Object** obj) {
  // Slow path, the allocation stack push back must have already failed.
  DCHECK(!self->PushOnThreadLocalAllocationStack(*obj));
  mirror::Object** start_address;
  mirror::Object** end_address;
  while (!allocation_stack_->AtomicBumpBack(kThreadLocalAllocationStackSize, &start_address,
                                            &end_address)) {
    // TODO: Add handle VerifyObject.
    StackHandleScope<1> hs(self);
    HandleWrapper<mirror::Object> wrapper(hs.NewHandleWrapper(obj));
    // Push our object into the reserve region of the allocaiton stack. This is only required due
    // to heap verification requiring that roots are live (either in the live bitmap or in the
    // allocation stack).
    CHECK(allocation_stack_->AtomicPushBackIgnoreGrowthLimit(*obj));
    // Push into the reserve allocation stack.
    CollectGarbageInternal(collector::kGcTypeSticky, kGcCauseForAlloc, false);
  }
  self->SetThreadLocalAllocationStack(start_address, end_address);
  // Retry on the new thread-local allocation stack.
  CHECK(self->PushOnThreadLocalAllocationStack(*obj));  // Must succeed.
}

// Must do this with mutators suspended since we are directly accessing the allocation stacks.
size_t Heap::VerifyHeapReferences(bool verify_referents) {
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertExclusiveHeld(self);
  // Lets sort our allocation stacks so that we can efficiently binary search them.
  allocation_stack_->Sort();
  live_stack_->Sort();
  // Since we sorted the allocation stack content, need to revoke all
  // thread-local allocation stacks.
  RevokeAllThreadLocalAllocationStacks(self);
  Atomic<size_t> fail_count_(0);
  VerifyObjectVisitor visitor(this, &fail_count_, verify_referents);
  // Verify objects in the allocation stack since these will be objects which were:
  // 1. Allocated prior to the GC (pre GC verification).
  // 2. Allocated during the GC (pre sweep GC verification).
  // We don't want to verify the objects in the live stack since they themselves may be
  // pointing to dead objects if they are not reachable.
  VisitObjects(VerifyObjectVisitor::VisitCallback, &visitor);
  // Verify the roots:
  Runtime::Current()->VisitRoots(VerifyReferenceVisitor::VerifyRootCallback, &visitor);
  if (visitor.GetFailureCount() > 0) {
    // Dump mod-union tables.
    for (const auto& table_pair : mod_union_tables_) {
      accounting::ModUnionTable* mod_union_table = table_pair.second;
      mod_union_table->Dump(LOG(ERROR) << mod_union_table->GetName() << ": ");
    }
    // Dump remembered sets.
    for (const auto& table_pair : remembered_sets_) {
      accounting::RememberedSet* remembered_set = table_pair.second;
      remembered_set->Dump(LOG(ERROR) << remembered_set->GetName() << ": ");
    }
    DumpSpaces(LOG(ERROR));
  }
  return visitor.GetFailureCount();
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
  void operator()(mirror::Object* obj, MemberOffset offset, bool is_static) const
      NO_THREAD_SAFETY_ANALYSIS {
    mirror::Object* ref = obj->GetFieldObject<mirror::Object>(offset);
    // Filter out class references since changing an object's class does not mark the card as dirty.
    // Also handles large objects, since the only reference they hold is a class reference.
    if (ref != nullptr && !ref->IsClass()) {
      accounting::CardTable* card_table = heap_->GetCardTable();
      // If the object is not dirty and it is referencing something in the live stack other than
      // class, then it must be on a dirty card.
      if (!card_table->AddrIsInCardTable(obj)) {
        LOG(ERROR) << "Object " << obj << " is not in the address range of the card table";
        *failed_ = true;
      } else if (!card_table->IsDirty(obj)) {
        // TODO: Check mod-union tables.
        // Card should be either kCardDirty if it got re-dirtied after we aged it, or
        // kCardDirty - 1 if it didnt get touched since we aged it.
        accounting::ObjectStack* live_stack = heap_->live_stack_.get();
        if (live_stack->ContainsSorted(ref)) {
          if (live_stack->ContainsSorted(obj)) {
            LOG(ERROR) << "Object " << obj << " found in live stack";
          }
          if (heap_->GetLiveBitmap()->Test(obj)) {
            LOG(ERROR) << "Object " << obj << " found in live bitmap";
          }
          LOG(ERROR) << "Object " << obj << " " << PrettyTypeOf(obj)
                    << " references " << ref << " " << PrettyTypeOf(ref) << " in live stack";

          // Print which field of the object is dead.
          if (!obj->IsObjectArray()) {
            mirror::Class* klass = is_static ? obj->AsClass() : obj->GetClass();
            CHECK(klass != NULL);
            mirror::ObjectArray<mirror::ArtField>* fields = is_static ? klass->GetSFields()
                                                                      : klass->GetIFields();
            CHECK(fields != NULL);
            for (int32_t i = 0; i < fields->GetLength(); ++i) {
              mirror::ArtField* cur = fields->Get(i);
              if (cur->GetOffset().Int32Value() == offset.Int32Value()) {
                LOG(ERROR) << (is_static ? "Static " : "") << "field in the live stack is "
                          << PrettyField(cur);
                break;
              }
            }
          } else {
            mirror::ObjectArray<mirror::Object>* object_array =
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
    obj->VisitReferences<true>(visitor, VoidFunctor());
  }

  bool Failed() const {
    return failed_;
  }

 private:
  Heap* const heap_;
  bool failed_;
};

bool Heap::VerifyMissingCardMarks() {
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertExclusiveHeld(self);
  // We need to sort the live stack since we binary search it.
  live_stack_->Sort();
  // Since we sorted the allocation stack content, need to revoke all
  // thread-local allocation stacks.
  RevokeAllThreadLocalAllocationStacks(self);
  VerifyLiveStackReferences visitor(this);
  GetLiveBitmap()->Visit(visitor);
  // We can verify objects in the live stack since none of these should reference dead objects.
  for (mirror::Object** it = live_stack_->Begin(); it != live_stack_->End(); ++it) {
    if (!kUseThreadLocalAllocationStack || *it != nullptr) {
      visitor(*it);
    }
  }
  return !visitor.Failed();
}

void Heap::SwapStacks(Thread* self) {
  if (kUseThreadLocalAllocationStack) {
    live_stack_->AssertAllZero();
  }
  allocation_stack_.swap(live_stack_);
}

void Heap::RevokeAllThreadLocalAllocationStacks(Thread* self) {
  // This must be called only during the pause.
  CHECK(Locks::mutator_lock_->IsExclusiveHeld(self));
  MutexLock mu(self, *Locks::runtime_shutdown_lock_);
  MutexLock mu2(self, *Locks::thread_list_lock_);
  std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
  for (Thread* t : thread_list) {
    t->RevokeThreadLocalAllocationStack();
  }
}

void Heap::AssertAllBumpPointerSpaceThreadLocalBuffersAreRevoked() {
  if (kIsDebugBuild) {
    if (bump_pointer_space_ != nullptr) {
      bump_pointer_space_->AssertAllThreadLocalBuffersAreRevoked();
    }
  }
}

accounting::ModUnionTable* Heap::FindModUnionTableFromSpace(space::Space* space) {
  auto it = mod_union_tables_.find(space);
  if (it == mod_union_tables_.end()) {
    return nullptr;
  }
  return it->second;
}

accounting::RememberedSet* Heap::FindRememberedSetFromSpace(space::Space* space) {
  auto it = remembered_sets_.find(space);
  if (it == remembered_sets_.end()) {
    return nullptr;
  }
  return it->second;
}

void Heap::ProcessCards(TimingLogger* timings, bool use_rem_sets) {
  TimingLogger::ScopedTiming t(__FUNCTION__, timings);
  // Clear cards and keep track of cards cleared in the mod-union table.
  for (const auto& space : continuous_spaces_) {
    accounting::ModUnionTable* table = FindModUnionTableFromSpace(space);
    accounting::RememberedSet* rem_set = FindRememberedSetFromSpace(space);
    if (table != nullptr) {
      const char* name = space->IsZygoteSpace() ? "ZygoteModUnionClearCards" :
          "ImageModUnionClearCards";
      TimingLogger::ScopedTiming t(name, timings);
      table->ClearCards();
    } else if (use_rem_sets && rem_set != nullptr) {
      DCHECK(collector::SemiSpace::kUseRememberedSet && collector_type_ == kCollectorTypeGSS)
          << static_cast<int>(collector_type_);
      TimingLogger::ScopedTiming t("AllocSpaceRemSetClearCards", timings);
      rem_set->ClearCards();
    } else if (space->GetType() != space::kSpaceTypeBumpPointerSpace) {
      TimingLogger::ScopedTiming t("AllocSpaceClearCards", timings);
      // No mod union table for the AllocSpace. Age the cards so that the GC knows that these cards
      // were dirty before the GC started.
      // TODO: Need to use atomic for the case where aged(cleaning thread) -> dirty(other thread)
      // -> clean(cleaning thread).
      // The races are we either end up with: Aged card, unaged card. Since we have the checkpoint
      // roots and then we scan / update mod union tables after. We will always scan either card.
      // If we end up with the non aged card, we scan it it in the pause.
      card_table_->ModifyCardsAtomic(space->Begin(), space->End(), AgeCardVisitor(),
                                     VoidFunctor());
    }
  }
}

static void IdentityMarkHeapReferenceCallback(mirror::HeapReference<mirror::Object>*, void*) {
}

void Heap::PreGcVerificationPaused(collector::GarbageCollector* gc) {
  Thread* const self = Thread::Current();
  TimingLogger* const timings = current_gc_iteration_.GetTimings();
  TimingLogger::ScopedTiming t(__FUNCTION__, timings);
  if (verify_pre_gc_heap_) {
    TimingLogger::ScopedTiming t("(Paused)PreGcVerifyHeapReferences", timings);
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    size_t failures = VerifyHeapReferences();
    if (failures > 0) {
      LOG(FATAL) << "Pre " << gc->GetName() << " heap verification failed with " << failures
          << " failures";
    }
  }
  // Check that all objects which reference things in the live stack are on dirty cards.
  if (verify_missing_card_marks_) {
    TimingLogger::ScopedTiming t("(Paused)PreGcVerifyMissingCardMarks", timings);
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    SwapStacks(self);
    // Sort the live stack so that we can quickly binary search it later.
    CHECK(VerifyMissingCardMarks()) << "Pre " << gc->GetName()
                                    << " missing card mark verification failed\n" << DumpSpaces();
    SwapStacks(self);
  }
  if (verify_mod_union_table_) {
    TimingLogger::ScopedTiming t("(Paused)PreGcVerifyModUnionTables", timings);
    ReaderMutexLock reader_lock(self, *Locks::heap_bitmap_lock_);
    for (const auto& table_pair : mod_union_tables_) {
      accounting::ModUnionTable* mod_union_table = table_pair.second;
      mod_union_table->UpdateAndMarkReferences(IdentityMarkHeapReferenceCallback, nullptr);
      mod_union_table->Verify();
    }
  }
}

void Heap::PreGcVerification(collector::GarbageCollector* gc) {
  if (verify_pre_gc_heap_ || verify_missing_card_marks_ || verify_mod_union_table_) {
    collector::GarbageCollector::ScopedPause pause(gc);
    PreGcVerificationPaused(gc);
  }
}

void Heap::PrePauseRosAllocVerification(collector::GarbageCollector* gc) {
  // TODO: Add a new runtime option for this?
  if (verify_pre_gc_rosalloc_) {
    RosAllocVerification(current_gc_iteration_.GetTimings(), "PreGcRosAllocVerification");
  }
}

void Heap::PreSweepingGcVerification(collector::GarbageCollector* gc) {
  Thread* const self = Thread::Current();
  TimingLogger* const timings = current_gc_iteration_.GetTimings();
  TimingLogger::ScopedTiming t(__FUNCTION__, timings);
  // Called before sweeping occurs since we want to make sure we are not going so reclaim any
  // reachable objects.
  if (verify_pre_sweeping_heap_) {
    TimingLogger::ScopedTiming t("(Paused)PostSweepingVerifyHeapReferences", timings);
    CHECK_NE(self->GetState(), kRunnable);
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    // Swapping bound bitmaps does nothing.
    gc->SwapBitmaps();
    // Pass in false since concurrent reference processing can mean that the reference referents
    // may point to dead objects at the point which PreSweepingGcVerification is called.
    size_t failures = VerifyHeapReferences(false);
    if (failures > 0) {
      LOG(FATAL) << "Pre sweeping " << gc->GetName() << " GC verification failed with " << failures
          << " failures";
    }
    gc->SwapBitmaps();
  }
  if (verify_pre_sweeping_rosalloc_) {
    RosAllocVerification(timings, "PreSweepingRosAllocVerification");
  }
}

void Heap::PostGcVerificationPaused(collector::GarbageCollector* gc) {
  // Only pause if we have to do some verification.
  Thread* const self = Thread::Current();
  TimingLogger* const timings = GetCurrentGcIteration()->GetTimings();
  TimingLogger::ScopedTiming t(__FUNCTION__, timings);
  if (verify_system_weaks_) {
    ReaderMutexLock mu2(self, *Locks::heap_bitmap_lock_);
    collector::MarkSweep* mark_sweep = down_cast<collector::MarkSweep*>(gc);
    mark_sweep->VerifySystemWeaks();
  }
  if (verify_post_gc_rosalloc_) {
    RosAllocVerification(timings, "(Paused)PostGcRosAllocVerification");
  }
  if (verify_post_gc_heap_) {
    TimingLogger::ScopedTiming t("(Paused)PostGcVerifyHeapReferences", timings);
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    size_t failures = VerifyHeapReferences();
    if (failures > 0) {
      LOG(FATAL) << "Pre " << gc->GetName() << " heap verification failed with " << failures
          << " failures";
    }
  }
}

void Heap::PostGcVerification(collector::GarbageCollector* gc) {
  if (verify_system_weaks_ || verify_post_gc_rosalloc_ || verify_post_gc_heap_) {
    collector::GarbageCollector::ScopedPause pause(gc);
    PostGcVerificationPaused(gc);
  }
}

void Heap::RosAllocVerification(TimingLogger* timings, const char* name) {
  TimingLogger::ScopedTiming t(name, timings);
  for (const auto& space : continuous_spaces_) {
    if (space->IsRosAllocSpace()) {
      VLOG(heap) << name << " : " << space->GetName();
      space->AsRosAllocSpace()->Verify();
    }
  }
}

collector::GcType Heap::WaitForGcToComplete(GcCause cause, Thread* self) {
  ScopedThreadStateChange tsc(self, kWaitingForGcToComplete);
  MutexLock mu(self, *gc_complete_lock_);
  return WaitForGcToCompleteLocked(cause, self);
}

collector::GcType Heap::WaitForGcToCompleteLocked(GcCause cause, Thread* self) {
  collector::GcType last_gc_type = collector::kGcTypeNone;
  uint64_t wait_start = NanoTime();
  while (collector_type_running_ != kCollectorTypeNone) {
    ATRACE_BEGIN("GC: Wait For Completion");
    // We must wait, change thread state then sleep on gc_complete_cond_;
    gc_complete_cond_->Wait(self);
    last_gc_type = last_gc_type_;
    ATRACE_END();
  }
  uint64_t wait_time = NanoTime() - wait_start;
  total_wait_time_ += wait_time;
  if (wait_time > long_pause_log_threshold_) {
    LOG(INFO) << "WaitForGcToComplete blocked for " << PrettyDuration(wait_time)
        << " for cause " << cause;
  }
  return last_gc_type;
}

void Heap::DumpForSigQuit(std::ostream& os) {
  os << "Heap: " << GetPercentFree() << "% free, " << PrettySize(GetBytesAllocated()) << "/"
     << PrettySize(GetTotalMemory()) << "; " << GetObjectsAllocated() << " objects\n";
  DumpGcPerformanceInfo(os);
}

size_t Heap::GetPercentFree() {
  return static_cast<size_t>(100.0f * static_cast<float>(GetFreeMemory()) / max_allowed_footprint_);
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
    space::Space* space = FindContinuousSpaceFromObject(obj, true);
    if (space != nullptr) {
      // TODO: Check large object?
      return space->CanMoveObjects();
    }
  }
  return false;
}

void Heap::UpdateMaxNativeFootprint() {
  size_t native_size = native_bytes_allocated_.LoadRelaxed();
  // TODO: Tune the native heap utilization to be a value other than the java heap utilization.
  size_t target_size = native_size / GetTargetHeapUtilization();
  if (target_size > native_size + max_free_) {
    target_size = native_size + max_free_;
  } else if (target_size < native_size + min_free_) {
    target_size = native_size + min_free_;
  }
  native_footprint_gc_watermark_ = std::min(growth_limit_, target_size);
}

collector::GarbageCollector* Heap::FindCollectorByGcType(collector::GcType gc_type) {
  for (const auto& collector : garbage_collectors_) {
    if (collector->GetCollectorType() == collector_type_ &&
        collector->GetGcType() == gc_type) {
      return collector;
    }
  }
  return nullptr;
}

double Heap::HeapGrowthMultiplier() const {
  // If we don't care about pause times we are background, so return 1.0.
  if (!CareAboutPauseTimes() || IsLowMemoryMode()) {
    return 1.0;
  }
  return foreground_heap_growth_multiplier_;
}

void Heap::GrowForUtilization(collector::GarbageCollector* collector_ran) {
  // We know what our utilization is at this moment.
  // This doesn't actually resize any memory. It just lets the heap grow more when necessary.
  const uint64_t bytes_allocated = GetBytesAllocated();
  last_gc_size_ = bytes_allocated;
  last_gc_time_ns_ = NanoTime();
  uint64_t target_size;
  collector::GcType gc_type = collector_ran->GetGcType();
  if (gc_type != collector::kGcTypeSticky) {
    // Grow the heap for non sticky GC.
    const float multiplier = HeapGrowthMultiplier();  // Use the multiplier to grow more for
    // foreground.
    intptr_t delta = bytes_allocated / GetTargetHeapUtilization() - bytes_allocated;
    CHECK_GE(delta, 0);
    target_size = bytes_allocated + delta * multiplier;
    target_size = std::min(target_size,
                           bytes_allocated + static_cast<uint64_t>(max_free_ * multiplier));
    target_size = std::max(target_size,
                           bytes_allocated + static_cast<uint64_t>(min_free_ * multiplier));
    native_need_to_run_finalization_ = true;
    next_gc_type_ = collector::kGcTypeSticky;
  } else {
    collector::GcType non_sticky_gc_type =
        have_zygote_space_ ? collector::kGcTypePartial : collector::kGcTypeFull;
    // Find what the next non sticky collector will be.
    collector::GarbageCollector* non_sticky_collector = FindCollectorByGcType(non_sticky_gc_type);
    // If the throughput of the current sticky GC >= throughput of the non sticky collector, then
    // do another sticky collection next.
    // We also check that the bytes allocated aren't over the footprint limit in order to prevent a
    // pathological case where dead objects which aren't reclaimed by sticky could get accumulated
    // if the sticky GC throughput always remained >= the full/partial throughput.
    if (current_gc_iteration_.GetEstimatedThroughput() * kStickyGcThroughputAdjustment >=
        non_sticky_collector->GetEstimatedMeanThroughput() &&
        non_sticky_collector->NumberOfIterations() > 0 &&
        bytes_allocated <= max_allowed_footprint_) {
      next_gc_type_ = collector::kGcTypeSticky;
    } else {
      next_gc_type_ = non_sticky_gc_type;
    }
    // If we have freed enough memory, shrink the heap back down.
    if (bytes_allocated + max_free_ < max_allowed_footprint_) {
      target_size = bytes_allocated + max_free_;
    } else {
      target_size = std::max(bytes_allocated, static_cast<uint64_t>(max_allowed_footprint_));
    }
  }
  if (!ignore_max_footprint_) {
    SetIdealFootprint(target_size);
    if (IsGcConcurrent()) {
      // Calculate when to perform the next ConcurrentGC.
      // Calculate the estimated GC duration.
      const double gc_duration_seconds = NsToMs(current_gc_iteration_.GetDurationNs()) / 1000.0;
      // Estimate how many remaining bytes we will have when we need to start the next GC.
      size_t remaining_bytes = allocation_rate_ * gc_duration_seconds;
      remaining_bytes = std::min(remaining_bytes, kMaxConcurrentRemainingBytes);
      remaining_bytes = std::max(remaining_bytes, kMinConcurrentRemainingBytes);
      if (UNLIKELY(remaining_bytes > max_allowed_footprint_)) {
        // A never going to happen situation that from the estimated allocation rate we will exceed
        // the applications entire footprint with the given estimated allocation rate. Schedule
        // another GC nearly straight away.
        remaining_bytes = kMinConcurrentRemainingBytes;
      }
      DCHECK_LE(remaining_bytes, max_allowed_footprint_);
      DCHECK_LE(max_allowed_footprint_, GetMaxMemory());
      // Start a concurrent GC when we get close to the estimated remaining bytes. When the
      // allocation rate is very high, remaining_bytes could tell us that we should start a GC
      // right away.
      concurrent_start_bytes_ = std::max(max_allowed_footprint_ - remaining_bytes,
                                         static_cast<size_t>(bytes_allocated));
    }
  }
}

void Heap::ClearGrowthLimit() {
  growth_limit_ = capacity_;
  non_moving_space_->ClearGrowthLimit();
}

void Heap::AddFinalizerReference(Thread* self, mirror::Object** object) {
  ScopedObjectAccess soa(self);
  ScopedLocalRef<jobject> arg(self->GetJniEnv(), soa.AddLocalReference<jobject>(*object));
  jvalue args[1];
  args[0].l = arg.get();
  InvokeWithJValues(soa, nullptr, WellKnownClasses::java_lang_ref_FinalizerReference_add, args);
  // Restore object in case it gets moved.
  *object = soa.Decode<mirror::Object*>(arg.get());
}

void Heap::RequestConcurrentGCAndSaveObject(Thread* self, mirror::Object** obj) {
  StackHandleScope<1> hs(self);
  HandleWrapper<mirror::Object> wrapper(hs.NewHandleWrapper(obj));
  RequestConcurrentGC(self);
}

void Heap::RequestConcurrentGC(Thread* self) {
  // Make sure that we can do a concurrent GC.
  Runtime* runtime = Runtime::Current();
  if (runtime == nullptr || !runtime->IsFinishedStarting() || runtime->IsShuttingDown(self) ||
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
  if (WaitForGcToComplete(kGcCauseBackground, self) == collector::kGcTypeNone) {
    // If the we can't run the GC type we wanted to run, find the next appropriate one and try that
    // instead. E.g. can't do partial, so do full instead.
    if (CollectGarbageInternal(next_gc_type_, kGcCauseBackground, false) ==
        collector::kGcTypeNone) {
      for (collector::GcType gc_type : gc_plan_) {
        // Attempt to run the collector, if we succeed, we are done.
        if (gc_type > next_gc_type_ &&
            CollectGarbageInternal(gc_type, kGcCauseBackground, false) != collector::kGcTypeNone) {
          break;
        }
      }
    }
  }
}

void Heap::RequestCollectorTransition(CollectorType desired_collector_type, uint64_t delta_time) {
  Thread* self = Thread::Current();
  {
    MutexLock mu(self, *heap_trim_request_lock_);
    if (desired_collector_type_ == desired_collector_type) {
      return;
    }
    heap_transition_or_trim_target_time_ =
        std::max(heap_transition_or_trim_target_time_, NanoTime() + delta_time);
    desired_collector_type_ = desired_collector_type;
  }
  SignalHeapTrimDaemon(self);
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

  Thread* self = Thread::Current();
  Runtime* runtime = Runtime::Current();
  if (runtime == nullptr || !runtime->IsFinishedStarting() || runtime->IsShuttingDown(self) ||
      runtime->IsZygote()) {
    // Ignore the request if we are the zygote to prevent app launching lag due to sleep in heap
    // trimmer daemon. b/17310019
    // Heap trimming isn't supported without a Java runtime or Daemons (such as at dex2oat time)
    // Also: we do not wish to start a heap trim if the runtime is shutting down (a racy check
    // as we don't hold the lock while requesting the trim).
    return;
  }
  {
    MutexLock mu(self, *heap_trim_request_lock_);
    if (last_trim_time_ + kHeapTrimWait >= NanoTime()) {
      // We have done a heap trim in the last kHeapTrimWait nanosecs, don't request another one
      // just yet.
      return;
    }
    heap_trim_request_pending_ = true;
    uint64_t current_time = NanoTime();
    if (heap_transition_or_trim_target_time_ < current_time) {
      heap_transition_or_trim_target_time_ = current_time + kHeapTrimWait;
    }
  }
  // Notify the daemon thread which will actually do the heap trim.
  SignalHeapTrimDaemon(self);
}

void Heap::SignalHeapTrimDaemon(Thread* self) {
  JNIEnv* env = self->GetJniEnv();
  DCHECK(WellKnownClasses::java_lang_Daemons != nullptr);
  DCHECK(WellKnownClasses::java_lang_Daemons_requestHeapTrim != nullptr);
  env->CallStaticVoidMethod(WellKnownClasses::java_lang_Daemons,
                            WellKnownClasses::java_lang_Daemons_requestHeapTrim);
  CHECK(!env->ExceptionCheck());
}

void Heap::RevokeThreadLocalBuffers(Thread* thread) {
  if (rosalloc_space_ != nullptr) {
    rosalloc_space_->RevokeThreadLocalBuffers(thread);
  }
  if (bump_pointer_space_ != nullptr) {
    bump_pointer_space_->RevokeThreadLocalBuffers(thread);
  }
}

void Heap::RevokeRosAllocThreadLocalBuffers(Thread* thread) {
  if (rosalloc_space_ != nullptr) {
    rosalloc_space_->RevokeThreadLocalBuffers(thread);
  }
}

void Heap::RevokeAllThreadLocalBuffers() {
  if (rosalloc_space_ != nullptr) {
    rosalloc_space_->RevokeAllThreadLocalBuffers();
  }
  if (bump_pointer_space_ != nullptr) {
    bump_pointer_space_->RevokeAllThreadLocalBuffers();
  }
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
  env->CallStaticVoidMethod(WellKnownClasses::java_lang_System,
                            WellKnownClasses::java_lang_System_runFinalization);
}

void Heap::RegisterNativeAllocation(JNIEnv* env, size_t bytes) {
  Thread* self = ThreadForEnv(env);
  if (native_need_to_run_finalization_) {
    RunFinalization(env);
    UpdateMaxNativeFootprint();
    native_need_to_run_finalization_ = false;
  }
  // Total number of native bytes allocated.
  size_t new_native_bytes_allocated = native_bytes_allocated_.FetchAndAddSequentiallyConsistent(bytes);
  new_native_bytes_allocated += bytes;
  if (new_native_bytes_allocated > native_footprint_gc_watermark_) {
    collector::GcType gc_type = have_zygote_space_ ? collector::kGcTypePartial :
        collector::kGcTypeFull;

    // The second watermark is higher than the gc watermark. If you hit this it means you are
    // allocating native objects faster than the GC can keep up with.
    if (new_native_bytes_allocated > growth_limit_) {
      if (WaitForGcToComplete(kGcCauseForNativeAlloc, self) != collector::kGcTypeNone) {
        // Just finished a GC, attempt to run finalizers.
        RunFinalization(env);
        CHECK(!env->ExceptionCheck());
      }
      // If we still are over the watermark, attempt a GC for alloc and run finalizers.
      if (new_native_bytes_allocated > growth_limit_) {
        CollectGarbageInternal(gc_type, kGcCauseForNativeAlloc, false);
        RunFinalization(env);
        native_need_to_run_finalization_ = false;
        CHECK(!env->ExceptionCheck());
      }
      // We have just run finalizers, update the native watermark since it is very likely that
      // finalizers released native managed allocations.
      UpdateMaxNativeFootprint();
    } else if (!IsGCRequestPending()) {
      if (IsGcConcurrent()) {
        RequestConcurrentGC(self);
      } else {
        CollectGarbageInternal(gc_type, kGcCauseForNativeAlloc, false);
      }
    }
  }
}

void Heap::RegisterNativeFree(JNIEnv* env, size_t bytes) {
  size_t expected_size;
  do {
    expected_size = native_bytes_allocated_.LoadRelaxed();
    if (UNLIKELY(bytes > expected_size)) {
      ScopedObjectAccess soa(env);
      env->ThrowNew(WellKnownClasses::java_lang_RuntimeException,
                    StringPrintf("Attempted to free %zd native bytes with only %zd native bytes "
                                 "registered as allocated", bytes, expected_size).c_str());
      break;
    }
  } while (!native_bytes_allocated_.CompareExchangeWeakRelaxed(expected_size,
                                                               expected_size - bytes));
}

size_t Heap::GetTotalMemory() const {
  return std::max(max_allowed_footprint_, GetBytesAllocated());
}

void Heap::AddModUnionTable(accounting::ModUnionTable* mod_union_table) {
  DCHECK(mod_union_table != nullptr);
  mod_union_tables_.Put(mod_union_table->GetSpace(), mod_union_table);
}

void Heap::CheckPreconditionsForAllocObject(mirror::Class* c, size_t byte_count) {
  CHECK(c == NULL || (c->IsClassClass() && byte_count >= sizeof(mirror::Class)) ||
        (c->IsVariableSize() || c->GetObjectSize() <= byte_count));
  CHECK_GE(byte_count, sizeof(mirror::Object));
}

void Heap::AddRememberedSet(accounting::RememberedSet* remembered_set) {
  CHECK(remembered_set != nullptr);
  space::Space* space = remembered_set->GetSpace();
  CHECK(space != nullptr);
  CHECK(remembered_sets_.find(space) == remembered_sets_.end()) << space;
  remembered_sets_.Put(space, remembered_set);
  CHECK(remembered_sets_.find(space) != remembered_sets_.end()) << space;
}

void Heap::RemoveRememberedSet(space::Space* space) {
  CHECK(space != nullptr);
  auto it = remembered_sets_.find(space);
  CHECK(it != remembered_sets_.end());
  delete it->second;
  remembered_sets_.erase(it);
  CHECK(remembered_sets_.find(space) == remembered_sets_.end());
}

void Heap::ClearMarkedObjects() {
  // Clear all of the spaces' mark bitmaps.
  for (const auto& space : GetContinuousSpaces()) {
    accounting::ContinuousSpaceBitmap* mark_bitmap = space->GetMarkBitmap();
    if (space->GetLiveBitmap() != mark_bitmap) {
      mark_bitmap->Clear();
    }
  }
  // Clear the marked objects in the discontinous space object sets.
  for (const auto& space : GetDiscontinuousSpaces()) {
    space->GetMarkBitmap()->Clear();
  }
}

}  // namespace gc
}  // namespace art
