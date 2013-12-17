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

#include "dlmalloc_space.h"
#include "large_object_space.h"

#include "common_test.h"
#include "globals.h"
#include "UniquePtr.h"
#include "mirror/array-inl.h"
#include "mirror/object-inl.h"

#include <stdint.h>

namespace art {
namespace gc {
namespace space {

class SpaceTest : public CommonTest {
 public:
  void AddSpace(ContinuousSpace* space) {
    // For RosAlloc, revoke the thread local runs before moving onto a
    // new alloc space.
    Runtime::Current()->GetHeap()->RevokeAllThreadLocalBuffers();
    Runtime::Current()->GetHeap()->AddSpace(space);
  }
  void InstallClass(mirror::Object* o, size_t size) NO_THREAD_SAFETY_ANALYSIS {
    // Note the minimum size, which is the size of a zero-length byte array, is 12.
    EXPECT_GE(size, static_cast<size_t>(12));
    SirtRef<mirror::ClassLoader> null_loader(Thread::Current(), NULL);
    mirror::Class* byte_array_class = Runtime::Current()->GetClassLinker()->FindClass("[B", null_loader);
    EXPECT_TRUE(byte_array_class != NULL);
    o->SetClass(byte_array_class);
    mirror::Array* arr = o->AsArray();
    // size_t header_size = sizeof(mirror::Object) + 4;
    size_t header_size = arr->DataOffset(1).Uint32Value();
    int32_t length = size - header_size;
    arr->SetLength(length);
    EXPECT_EQ(arr->SizeOf(), size);
  }

  static MallocSpace* CreateDlMallocSpace(const std::string& name, size_t initial_size, size_t growth_limit,
                                          size_t capacity, byte* requested_begin) {
    return DlMallocSpace::Create(name, initial_size, growth_limit, capacity, requested_begin);
  }
  static MallocSpace* CreateRosAllocSpace(const std::string& name, size_t initial_size, size_t growth_limit,
                                          size_t capacity, byte* requested_begin) {
    return RosAllocSpace::Create(name, initial_size, growth_limit, capacity, requested_begin,
                                 Runtime::Current()->GetHeap()->IsLowMemoryMode());
  }

  typedef MallocSpace* (*CreateSpaceFn)(const std::string& name, size_t initial_size, size_t growth_limit,
                                        size_t capacity, byte* requested_begin);
  void InitTestBody(CreateSpaceFn create_space);
  void ZygoteSpaceTestBody(CreateSpaceFn create_space);
  void AllocAndFreeTestBody(CreateSpaceFn create_space);
  void AllocAndFreeListTestBody(CreateSpaceFn create_space);

  void SizeFootPrintGrowthLimitAndTrimBody(MallocSpace* space, intptr_t object_size,
                                           int round, size_t growth_limit);
  void SizeFootPrintGrowthLimitAndTrimDriver(size_t object_size, CreateSpaceFn create_space);
};

static size_t test_rand(size_t* seed) {
  *seed = *seed * 1103515245 + 12345;
  return *seed;
}

void SpaceTest::InitTestBody(CreateSpaceFn create_space) {
  {
    // Init < max == growth
    UniquePtr<Space> space(create_space("test", 16 * MB, 32 * MB, 32 * MB, NULL));
    EXPECT_TRUE(space.get() != NULL);
  }
  {
    // Init == max == growth
    UniquePtr<Space> space(create_space("test", 16 * MB, 16 * MB, 16 * MB, NULL));
    EXPECT_TRUE(space.get() != NULL);
  }
  {
    // Init > max == growth
    UniquePtr<Space> space(create_space("test", 32 * MB, 16 * MB, 16 * MB, NULL));
    EXPECT_TRUE(space.get() == NULL);
  }
  {
    // Growth == init < max
    UniquePtr<Space> space(create_space("test", 16 * MB, 16 * MB, 32 * MB, NULL));
    EXPECT_TRUE(space.get() != NULL);
  }
  {
    // Growth < init < max
    UniquePtr<Space> space(create_space("test", 16 * MB, 8 * MB, 32 * MB, NULL));
    EXPECT_TRUE(space.get() == NULL);
  }
  {
    // Init < growth < max
    UniquePtr<Space> space(create_space("test", 8 * MB, 16 * MB, 32 * MB, NULL));
    EXPECT_TRUE(space.get() != NULL);
  }
  {
    // Init < max < growth
    UniquePtr<Space> space(create_space("test", 8 * MB, 32 * MB, 16 * MB, NULL));
    EXPECT_TRUE(space.get() == NULL);
  }
}

TEST_F(SpaceTest, Init_DlMallocSpace) {
  InitTestBody(SpaceTest::CreateDlMallocSpace);
}
TEST_F(SpaceTest, Init_RosAllocSpace) {
  InitTestBody(SpaceTest::CreateRosAllocSpace);
}

// TODO: This test is not very good, we should improve it.
// The test should do more allocations before the creation of the ZygoteSpace, and then do
// allocations after the ZygoteSpace is created. The test should also do some GCs to ensure that
// the GC works with the ZygoteSpace.
void SpaceTest::ZygoteSpaceTestBody(CreateSpaceFn create_space) {
  size_t dummy = 0;
  MallocSpace* space(create_space("test", 4 * MB, 16 * MB, 16 * MB, NULL));
  ASSERT_TRUE(space != NULL);

  // Make space findable to the heap, will also delete space when runtime is cleaned up
  AddSpace(space);
  Thread* self = Thread::Current();

  // Succeeds, fits without adjusting the footprint limit.
  mirror::Object* ptr1 = space->Alloc(self, 1 * MB, &dummy);
  EXPECT_TRUE(ptr1 != NULL);
  InstallClass(ptr1, 1 * MB);

  // Fails, requires a higher footprint limit.
  mirror::Object* ptr2 = space->Alloc(self, 8 * MB, &dummy);
  EXPECT_TRUE(ptr2 == NULL);

  // Succeeds, adjusts the footprint.
  size_t ptr3_bytes_allocated;
  mirror::Object* ptr3 = space->AllocWithGrowth(self, 8 * MB, &ptr3_bytes_allocated);
  EXPECT_TRUE(ptr3 != NULL);
  EXPECT_LE(8U * MB, ptr3_bytes_allocated);
  InstallClass(ptr3, 8 * MB);

  // Fails, requires a higher footprint limit.
  mirror::Object* ptr4 = space->Alloc(self, 8 * MB, &dummy);
  EXPECT_TRUE(ptr4 == NULL);

  // Also fails, requires a higher allowed footprint.
  mirror::Object* ptr5 = space->AllocWithGrowth(self, 8 * MB, &dummy);
  EXPECT_TRUE(ptr5 == NULL);

  // Release some memory.
  size_t free3 = space->AllocationSize(ptr3);
  EXPECT_EQ(free3, ptr3_bytes_allocated);
  EXPECT_EQ(free3, space->Free(self, ptr3));
  EXPECT_LE(8U * MB, free3);

  // Succeeds, now that memory has been freed.
  mirror::Object* ptr6 = space->AllocWithGrowth(self, 9 * MB, &dummy);
  EXPECT_TRUE(ptr6 != NULL);
  InstallClass(ptr6, 9 * MB);

  // Final clean up.
  size_t free1 = space->AllocationSize(ptr1);
  space->Free(self, ptr1);
  EXPECT_LE(1U * MB, free1);

  // Make sure that the zygote space isn't directly at the start of the space.
  space->Alloc(self, 1U * MB, &dummy);
  space = space->CreateZygoteSpace("alloc space", Runtime::Current()->GetHeap()->IsLowMemoryMode());

  // Make space findable to the heap, will also delete space when runtime is cleaned up
  AddSpace(space);

  // Succeeds, fits without adjusting the footprint limit.
  ptr1 = space->Alloc(self, 1 * MB, &dummy);
  EXPECT_TRUE(ptr1 != NULL);
  InstallClass(ptr1, 1 * MB);

  // Fails, requires a higher footprint limit.
  ptr2 = space->Alloc(self, 8 * MB, &dummy);
  EXPECT_TRUE(ptr2 == NULL);

  // Succeeds, adjusts the footprint.
  ptr3 = space->AllocWithGrowth(self, 2 * MB, &dummy);
  EXPECT_TRUE(ptr3 != NULL);
  InstallClass(ptr3, 2 * MB);
  space->Free(self, ptr3);

  // Final clean up.
  free1 = space->AllocationSize(ptr1);
  space->Free(self, ptr1);
  EXPECT_LE(1U * MB, free1);
}

TEST_F(SpaceTest, ZygoteSpace_DlMallocSpace) {
  ZygoteSpaceTestBody(SpaceTest::CreateDlMallocSpace);
}

TEST_F(SpaceTest, ZygoteSpace_RosAllocSpace) {
  ZygoteSpaceTestBody(SpaceTest::CreateRosAllocSpace);
}

void SpaceTest::AllocAndFreeTestBody(CreateSpaceFn create_space) {
  size_t dummy = 0;
  MallocSpace* space(create_space("test", 4 * MB, 16 * MB, 16 * MB, NULL));
  ASSERT_TRUE(space != NULL);
  Thread* self = Thread::Current();

  // Make space findable to the heap, will also delete space when runtime is cleaned up
  AddSpace(space);

  // Succeeds, fits without adjusting the footprint limit.
  mirror::Object* ptr1 = space->Alloc(self, 1 * MB, &dummy);
  EXPECT_TRUE(ptr1 != NULL);
  InstallClass(ptr1, 1 * MB);

  // Fails, requires a higher footprint limit.
  mirror::Object* ptr2 = space->Alloc(self, 8 * MB, &dummy);
  EXPECT_TRUE(ptr2 == NULL);

  // Succeeds, adjusts the footprint.
  size_t ptr3_bytes_allocated;
  mirror::Object* ptr3 = space->AllocWithGrowth(self, 8 * MB, &ptr3_bytes_allocated);
  EXPECT_TRUE(ptr3 != NULL);
  EXPECT_LE(8U * MB, ptr3_bytes_allocated);
  InstallClass(ptr3, 8 * MB);

  // Fails, requires a higher footprint limit.
  mirror::Object* ptr4 = space->Alloc(self, 8 * MB, &dummy);
  EXPECT_TRUE(ptr4 == NULL);

  // Also fails, requires a higher allowed footprint.
  mirror::Object* ptr5 = space->AllocWithGrowth(self, 8 * MB, &dummy);
  EXPECT_TRUE(ptr5 == NULL);

  // Release some memory.
  size_t free3 = space->AllocationSize(ptr3);
  EXPECT_EQ(free3, ptr3_bytes_allocated);
  space->Free(self, ptr3);
  EXPECT_LE(8U * MB, free3);

  // Succeeds, now that memory has been freed.
  mirror::Object* ptr6 = space->AllocWithGrowth(self, 9 * MB, &dummy);
  EXPECT_TRUE(ptr6 != NULL);
  InstallClass(ptr6, 9 * MB);

  // Final clean up.
  size_t free1 = space->AllocationSize(ptr1);
  space->Free(self, ptr1);
  EXPECT_LE(1U * MB, free1);
}

TEST_F(SpaceTest, AllocAndFree_DlMallocSpace) {
  AllocAndFreeTestBody(SpaceTest::CreateDlMallocSpace);
}
TEST_F(SpaceTest, AllocAndFree_RosAllocSpace) {
  AllocAndFreeTestBody(SpaceTest::CreateRosAllocSpace);
}

TEST_F(SpaceTest, LargeObjectTest) {
  size_t rand_seed = 0;
  for (size_t i = 0; i < 2; ++i) {
    LargeObjectSpace* los = NULL;
    if (i == 0) {
      los = space::LargeObjectMapSpace::Create("large object space");
    } else {
      los = space::FreeListSpace::Create("large object space", NULL, 128 * MB);
    }

    static const size_t num_allocations = 64;
    static const size_t max_allocation_size = 0x100000;
    std::vector<std::pair<mirror::Object*, size_t> > requests;

    for (size_t phase = 0; phase < 2; ++phase) {
      while (requests.size() < num_allocations) {
        size_t request_size = test_rand(&rand_seed) % max_allocation_size;
        size_t allocation_size = 0;
        mirror::Object* obj = los->Alloc(Thread::Current(), request_size, &allocation_size);
        ASSERT_TRUE(obj != NULL);
        ASSERT_EQ(allocation_size, los->AllocationSize(obj));
        ASSERT_GE(allocation_size, request_size);
        // Fill in our magic value.
        byte magic = (request_size & 0xFF) | 1;
        memset(obj, magic, request_size);
        requests.push_back(std::make_pair(obj, request_size));
      }

      // "Randomly" shuffle the requests.
      for (size_t k = 0; k < 10; ++k) {
        for (size_t j = 0; j < requests.size(); ++j) {
          std::swap(requests[j], requests[test_rand(&rand_seed) % requests.size()]);
        }
      }

      // Free 1 / 2 the allocations the first phase, and all the second phase.
      size_t limit = !phase ? requests.size() / 2 : 0;
      while (requests.size() > limit) {
        mirror::Object* obj = requests.back().first;
        size_t request_size = requests.back().second;
        requests.pop_back();
        byte magic = (request_size & 0xFF) | 1;
        for (size_t k = 0; k < request_size; ++k) {
          ASSERT_EQ(reinterpret_cast<const byte*>(obj)[k], magic);
        }
        ASSERT_GE(los->Free(Thread::Current(), obj), request_size);
      }
    }

    size_t bytes_allocated = 0;
    // Checks that the coalescing works.
    mirror::Object* obj = los->Alloc(Thread::Current(), 100 * MB, &bytes_allocated);
    EXPECT_TRUE(obj != NULL);
    los->Free(Thread::Current(), obj);

    EXPECT_EQ(0U, los->GetBytesAllocated());
    EXPECT_EQ(0U, los->GetObjectsAllocated());
    delete los;
  }
}

void SpaceTest::AllocAndFreeListTestBody(CreateSpaceFn create_space) {
  MallocSpace* space(create_space("test", 4 * MB, 16 * MB, 16 * MB, NULL));
  ASSERT_TRUE(space != NULL);

  // Make space findable to the heap, will also delete space when runtime is cleaned up
  AddSpace(space);
  Thread* self = Thread::Current();

  // Succeeds, fits without adjusting the max allowed footprint.
  mirror::Object* lots_of_objects[1024];
  for (size_t i = 0; i < arraysize(lots_of_objects); i++) {
    size_t allocation_size = 0;
    lots_of_objects[i] = space->Alloc(self, 16, &allocation_size);
    EXPECT_TRUE(lots_of_objects[i] != NULL);
    InstallClass(lots_of_objects[i], 16);
    EXPECT_EQ(allocation_size, space->AllocationSize(lots_of_objects[i]));
  }

  // Release memory and check pointers are NULL
  space->FreeList(self, arraysize(lots_of_objects), lots_of_objects);
  for (size_t i = 0; i < arraysize(lots_of_objects); i++) {
    EXPECT_TRUE(lots_of_objects[i] == NULL);
  }

  // Succeeds, fits by adjusting the max allowed footprint.
  for (size_t i = 0; i < arraysize(lots_of_objects); i++) {
    size_t allocation_size = 0;
    lots_of_objects[i] = space->AllocWithGrowth(self, 1024, &allocation_size);
    EXPECT_TRUE(lots_of_objects[i] != NULL);
    InstallClass(lots_of_objects[i], 1024);
    EXPECT_EQ(allocation_size, space->AllocationSize(lots_of_objects[i]));
  }

  // Release memory and check pointers are NULL
  space->FreeList(self, arraysize(lots_of_objects), lots_of_objects);
  for (size_t i = 0; i < arraysize(lots_of_objects); i++) {
    EXPECT_TRUE(lots_of_objects[i] == NULL);
  }
}

TEST_F(SpaceTest, AllocAndFreeList_DlMallocSpace) {
  AllocAndFreeListTestBody(SpaceTest::CreateDlMallocSpace);
}
TEST_F(SpaceTest, AllocAndFreeList_RosAllocSpace) {
  AllocAndFreeListTestBody(SpaceTest::CreateRosAllocSpace);
}

void SpaceTest::SizeFootPrintGrowthLimitAndTrimBody(MallocSpace* space, intptr_t object_size,
                                                    int round, size_t growth_limit) {
  if (((object_size > 0 && object_size >= static_cast<intptr_t>(growth_limit))) ||
      ((object_size < 0 && -object_size >= static_cast<intptr_t>(growth_limit)))) {
    // No allocation can succeed
    return;
  }

  // The space's footprint equals amount of resources requested from system
  size_t footprint = space->GetFootprint();

  // The space must at least have its book keeping allocated
  EXPECT_GT(footprint, 0u);

  // But it shouldn't exceed the initial size
  EXPECT_LE(footprint, growth_limit);

  // space's size shouldn't exceed the initial size
  EXPECT_LE(space->Size(), growth_limit);

  // this invariant should always hold or else the space has grown to be larger than what the
  // space believes its size is (which will break invariants)
  EXPECT_GE(space->Size(), footprint);

  // Fill the space with lots of small objects up to the growth limit
  size_t max_objects = (growth_limit / (object_size > 0 ? object_size : 8)) + 1;
  UniquePtr<mirror::Object*[]> lots_of_objects(new mirror::Object*[max_objects]);
  size_t last_object = 0;  // last object for which allocation succeeded
  size_t amount_allocated = 0;  // amount of space allocated
  Thread* self = Thread::Current();
  size_t rand_seed = 123456789;
  for (size_t i = 0; i < max_objects; i++) {
    size_t alloc_fails = 0;  // number of failed allocations
    size_t max_fails = 30;  // number of times we fail allocation before giving up
    for (; alloc_fails < max_fails; alloc_fails++) {
      size_t alloc_size;
      if (object_size > 0) {
        alloc_size = object_size;
      } else {
        alloc_size = test_rand(&rand_seed) % static_cast<size_t>(-object_size);
        // Note the minimum size, which is the size of a zero-length byte array, is 12.
        if (alloc_size < 12) {
          alloc_size = 12;
        }
      }
      mirror::Object* object;
      size_t bytes_allocated = 0;
      if (round <= 1) {
        object = space->Alloc(self, alloc_size, &bytes_allocated);
      } else {
        object = space->AllocWithGrowth(self, alloc_size, &bytes_allocated);
      }
      footprint = space->GetFootprint();
      EXPECT_GE(space->Size(), footprint);  // invariant
      if (object != NULL) {  // allocation succeeded
        InstallClass(object, alloc_size);
        lots_of_objects.get()[i] = object;
        size_t allocation_size = space->AllocationSize(object);
        EXPECT_EQ(bytes_allocated, allocation_size);
        if (object_size > 0) {
          EXPECT_GE(allocation_size, static_cast<size_t>(object_size));
        } else {
          EXPECT_GE(allocation_size, 8u);
        }
        amount_allocated += allocation_size;
        break;
      }
    }
    if (alloc_fails == max_fails) {
      last_object = i;
      break;
    }
  }
  CHECK_NE(last_object, 0u);  // we should have filled the space
  EXPECT_GT(amount_allocated, 0u);

  // We shouldn't have gone past the growth_limit
  EXPECT_LE(amount_allocated, growth_limit);
  EXPECT_LE(footprint, growth_limit);
  EXPECT_LE(space->Size(), growth_limit);

  // footprint and size should agree with amount allocated
  EXPECT_GE(footprint, amount_allocated);
  EXPECT_GE(space->Size(), amount_allocated);

  // Release storage in a semi-adhoc manner
  size_t free_increment = 96;
  while (true) {
    // Give the space a haircut
    space->Trim();

    // Bounds sanity
    footprint = space->GetFootprint();
    EXPECT_LE(amount_allocated, growth_limit);
    EXPECT_GE(footprint, amount_allocated);
    EXPECT_LE(footprint, growth_limit);
    EXPECT_GE(space->Size(), amount_allocated);
    EXPECT_LE(space->Size(), growth_limit);

    if (free_increment == 0) {
      break;
    }

    // Free some objects
    for (size_t i = 0; i < last_object; i += free_increment) {
      mirror::Object* object = lots_of_objects.get()[i];
      if (object == NULL) {
        continue;
      }
      size_t allocation_size = space->AllocationSize(object);
      if (object_size > 0) {
        EXPECT_GE(allocation_size, static_cast<size_t>(object_size));
      } else {
        EXPECT_GE(allocation_size, 8u);
      }
      space->Free(self, object);
      lots_of_objects.get()[i] = NULL;
      amount_allocated -= allocation_size;
      footprint = space->GetFootprint();
      EXPECT_GE(space->Size(), footprint);  // invariant
    }

    free_increment >>= 1;
  }

  // The space has become empty here before allocating a large object
  // below. For RosAlloc, revoke thread-local runs, which are kept
  // even when empty for a performance reason, so that they won't
  // cause the following large object allocation to fail due to
  // potential fragmentation. Note they are normally revoked at each
  // GC (but no GC here.)
  space->RevokeAllThreadLocalBuffers();

  // All memory was released, try a large allocation to check freed memory is being coalesced
  mirror::Object* large_object;
  size_t three_quarters_space = (growth_limit / 2) + (growth_limit / 4);
  size_t bytes_allocated = 0;
  if (round <= 1) {
    large_object = space->Alloc(self, three_quarters_space, &bytes_allocated);
  } else {
    large_object = space->AllocWithGrowth(self, three_quarters_space, &bytes_allocated);
  }
  EXPECT_TRUE(large_object != NULL);
  InstallClass(large_object, three_quarters_space);

  // Sanity check footprint
  footprint = space->GetFootprint();
  EXPECT_LE(footprint, growth_limit);
  EXPECT_GE(space->Size(), footprint);
  EXPECT_LE(space->Size(), growth_limit);

  // Clean up
  space->Free(self, large_object);

  // Sanity check footprint
  footprint = space->GetFootprint();
  EXPECT_LE(footprint, growth_limit);
  EXPECT_GE(space->Size(), footprint);
  EXPECT_LE(space->Size(), growth_limit);
}

void SpaceTest::SizeFootPrintGrowthLimitAndTrimDriver(size_t object_size, CreateSpaceFn create_space) {
  size_t initial_size = 4 * MB;
  size_t growth_limit = 8 * MB;
  size_t capacity = 16 * MB;
  MallocSpace* space(create_space("test", initial_size, growth_limit, capacity, NULL));
  ASSERT_TRUE(space != NULL);

  // Basic sanity
  EXPECT_EQ(space->Capacity(), growth_limit);
  EXPECT_EQ(space->NonGrowthLimitCapacity(), capacity);

  // Make space findable to the heap, will also delete space when runtime is cleaned up
  AddSpace(space);

  // In this round we don't allocate with growth and therefore can't grow past the initial size.
  // This effectively makes the growth_limit the initial_size, so assert this.
  SizeFootPrintGrowthLimitAndTrimBody(space, object_size, 1, initial_size);
  SizeFootPrintGrowthLimitAndTrimBody(space, object_size, 2, growth_limit);
  // Remove growth limit
  space->ClearGrowthLimit();
  EXPECT_EQ(space->Capacity(), capacity);
  SizeFootPrintGrowthLimitAndTrimBody(space, object_size, 3, capacity);
}

#define TEST_SizeFootPrintGrowthLimitAndTrim(name, size) \
  TEST_F(SpaceTest, SizeFootPrintGrowthLimitAndTrim_AllocationsOf_##name##_DlMallocSpace) { \
    SizeFootPrintGrowthLimitAndTrimDriver(size, SpaceTest::CreateDlMallocSpace); \
  } \
  TEST_F(SpaceTest, SizeFootPrintGrowthLimitAndTrim_RandomAllocationsWithMax_##name##_DlMallocSpace) { \
    SizeFootPrintGrowthLimitAndTrimDriver(-size, SpaceTest::CreateDlMallocSpace); \
  } \
  TEST_F(SpaceTest, SizeFootPrintGrowthLimitAndTrim_AllocationsOf_##name##_RosAllocSpace) { \
    SizeFootPrintGrowthLimitAndTrimDriver(size, SpaceTest::CreateRosAllocSpace); \
  } \
  TEST_F(SpaceTest, SizeFootPrintGrowthLimitAndTrim_RandomAllocationsWithMax_##name##_RosAllocSpace) { \
    SizeFootPrintGrowthLimitAndTrimDriver(-size, SpaceTest::CreateRosAllocSpace); \
  }

// Each size test is its own test so that we get a fresh heap each time
TEST_F(SpaceTest, SizeFootPrintGrowthLimitAndTrim_AllocationsOf_12B_DlMallocSpace) {
  SizeFootPrintGrowthLimitAndTrimDriver(12, SpaceTest::CreateDlMallocSpace);
}
TEST_F(SpaceTest, SizeFootPrintGrowthLimitAndTrim_AllocationsOf_12B_RosAllocSpace) {
  SizeFootPrintGrowthLimitAndTrimDriver(12, SpaceTest::CreateRosAllocSpace);
}
TEST_SizeFootPrintGrowthLimitAndTrim(16B, 16)
TEST_SizeFootPrintGrowthLimitAndTrim(24B, 24)
TEST_SizeFootPrintGrowthLimitAndTrim(32B, 32)
TEST_SizeFootPrintGrowthLimitAndTrim(64B, 64)
TEST_SizeFootPrintGrowthLimitAndTrim(128B, 128)
TEST_SizeFootPrintGrowthLimitAndTrim(1KB, 1 * KB)
TEST_SizeFootPrintGrowthLimitAndTrim(4KB, 4 * KB)
TEST_SizeFootPrintGrowthLimitAndTrim(1MB, 1 * MB)
TEST_SizeFootPrintGrowthLimitAndTrim(4MB, 4 * MB)
TEST_SizeFootPrintGrowthLimitAndTrim(8MB, 8 * MB)

}  // namespace space
}  // namespace gc
}  // namespace art
