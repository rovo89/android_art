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

#include "common_test.h"
#include "dlmalloc.h"
#include "globals.h"
#include "UniquePtr.h"

#include <stdint.h>

namespace art {

class SpaceTest : public CommonTest {
 public:
  void SizeFootPrintGrowthLimitAndTrimBody(AllocSpace* space, intptr_t object_size,
                                           int round, size_t growth_limit);
  void SizeFootPrintGrowthLimitAndTrimDriver(size_t object_size);
};

TEST_F(SpaceTest, Init) {
  {
    // Init < max == growth
    UniquePtr<Space> space(Space::CreateAllocSpace("test", 16 * MB, 32 * MB, 32 * MB, NULL));
    EXPECT_TRUE(space.get() != NULL);
  }
  {
    // Init == max == growth
    UniquePtr<Space> space(Space::CreateAllocSpace("test", 16 * MB, 16 * MB, 16 * MB, NULL));
    EXPECT_TRUE(space.get() != NULL);
  }
  {
    // Init > max == growth
    UniquePtr<Space> space(Space::CreateAllocSpace("test", 32 * MB, 16 * MB, 16 * MB, NULL));
    EXPECT_TRUE(space.get() == NULL);
  }
  {
    // Growth == init < max
    UniquePtr<Space> space(Space::CreateAllocSpace("test", 16 * MB, 16 * MB, 32 * MB, NULL));
    EXPECT_TRUE(space.get() != NULL);
  }
  {
    // Growth < init < max
    UniquePtr<Space> space(Space::CreateAllocSpace("test", 16 * MB, 8 * MB, 32 * MB, NULL));
    EXPECT_TRUE(space.get() == NULL);
  }
  {
    // Init < growth < max
    UniquePtr<Space> space(Space::CreateAllocSpace("test", 8 * MB, 16 * MB, 32 * MB, NULL));
    EXPECT_TRUE(space.get() != NULL);
  }
  {
    // Init < max < growth
    UniquePtr<Space> space(Space::CreateAllocSpace("test", 8 * MB, 32 * MB, 16 * MB, NULL));
    EXPECT_TRUE(space.get() == NULL);
  }
}

// TODO: This test is not very good, we should improve it.
// The test should do more allocations before the creation of the ZygoteSpace, and then do
// allocations after the ZygoteSpace is created. The test should also do some GCs to ensure that
// the GC works with the ZygoteSpace.
TEST_F(SpaceTest, ZygoteSpace) {
    AllocSpace* space(Space::CreateAllocSpace("test", 4 * MB, 16 * MB, 16 * MB, NULL));
    ASSERT_TRUE(space != NULL);

  // Make space findable to the heap, will also delete space when runtime is cleaned up
    Runtime::Current()->GetHeap()->AddSpace(space);

    // Succeeds, fits without adjusting the footprint limit.
    Object* ptr1 = space->AllocWithoutGrowth(1 * MB);
    EXPECT_TRUE(ptr1 != NULL);

    // Fails, requires a higher footprint limit.
    Object* ptr2 = space->AllocWithoutGrowth(8 * MB);
    EXPECT_TRUE(ptr2 == NULL);

    // Succeeds, adjusts the footprint.
    Object* ptr3 = space->AllocWithGrowth(8 * MB);
    EXPECT_TRUE(ptr3 != NULL);

    // Fails, requires a higher footprint limit.
    Object* ptr4 = space->AllocWithoutGrowth(8 * MB);
    EXPECT_TRUE(ptr4 == NULL);

    // Also fails, requires a higher allowed footprint.
    Object* ptr5 = space->AllocWithGrowth(8 * MB);
    EXPECT_TRUE(ptr5 == NULL);

    // Release some memory.
    size_t free3 = space->AllocationSize(ptr3);
    space->Free(ptr3);
    EXPECT_LE(8U * MB, free3);

    // Succeeds, now that memory has been freed.
    void* ptr6 = space->AllocWithGrowth(9 * MB);
    EXPECT_TRUE(ptr6 != NULL);

    // Final clean up.
    size_t free1 = space->AllocationSize(ptr1);
    space->Free(ptr1);
    EXPECT_LE(1U * MB, free1);

    // Make sure that the zygote space isn't directly at the start of the space.
    space->AllocWithoutGrowth(1U * MB);
    space = space->CreateZygoteSpace();

    // Make space findable to the heap, will also delete space when runtime is cleaned up
    Runtime::Current()->GetHeap()->AddSpace(space);

    // Succeeds, fits without adjusting the footprint limit.
    ptr1 = space->AllocWithoutGrowth(1 * MB);
    EXPECT_TRUE(ptr1 != NULL);

    // Fails, requires a higher footprint limit.
    ptr2 = space->AllocWithoutGrowth(8 * MB);
    EXPECT_TRUE(ptr2 == NULL);

    // Succeeds, adjusts the footprint.
    ptr3 = space->AllocWithGrowth(2 * MB);
    EXPECT_TRUE(ptr3 != NULL);
    space->Free(ptr3);

    // Final clean up.
    free1 = space->AllocationSize(ptr1);
    space->Free(ptr1);
    EXPECT_LE(1U * MB, free1);
}

TEST_F(SpaceTest, AllocAndFree) {
  AllocSpace* space(Space::CreateAllocSpace("test", 4 * MB, 16 * MB, 16 * MB, NULL));
  ASSERT_TRUE(space != NULL);

  // Make space findable to the heap, will also delete space when runtime is cleaned up
  Runtime::Current()->GetHeap()->AddSpace(space);

  // Succeeds, fits without adjusting the footprint limit.
  Object* ptr1 = space->AllocWithoutGrowth(1 * MB);
  EXPECT_TRUE(ptr1 != NULL);

  // Fails, requires a higher footprint limit.
  Object* ptr2 = space->AllocWithoutGrowth(8 * MB);
  EXPECT_TRUE(ptr2 == NULL);

  // Succeeds, adjusts the footprint.
  Object* ptr3 = space->AllocWithGrowth(8 * MB);
  EXPECT_TRUE(ptr3 != NULL);

  // Fails, requires a higher footprint limit.
  Object* ptr4 = space->AllocWithoutGrowth(8 * MB);
  EXPECT_TRUE(ptr4 == NULL);

  // Also fails, requires a higher allowed footprint.
  Object* ptr5 = space->AllocWithGrowth(8 * MB);
  EXPECT_TRUE(ptr5 == NULL);

  // Release some memory.
  size_t free3 = space->AllocationSize(ptr3);
  space->Free(ptr3);
  EXPECT_LE(8U * MB, free3);

  // Succeeds, now that memory has been freed.
  void* ptr6 = space->AllocWithGrowth(9 * MB);
  EXPECT_TRUE(ptr6 != NULL);

  // Final clean up.
  size_t free1 = space->AllocationSize(ptr1);
  space->Free(ptr1);
  EXPECT_LE(1U * MB, free1);
}

TEST_F(SpaceTest, AllocAndFreeList) {
  AllocSpace* space(Space::CreateAllocSpace("test", 4 * MB, 16 * MB, 16 * MB, NULL));
  ASSERT_TRUE(space != NULL);

  // Make space findable to the heap, will also delete space when runtime is cleaned up
  Runtime::Current()->GetHeap()->AddSpace(space);

  // Succeeds, fits without adjusting the max allowed footprint.
  Object* lots_of_objects[1024];
  for (size_t i = 0; i < arraysize(lots_of_objects); i++) {
    lots_of_objects[i] = space->AllocWithoutGrowth(16);
    EXPECT_TRUE(lots_of_objects[i] != NULL);
  }

  // Release memory and check pointers are NULL
  space->FreeList(arraysize(lots_of_objects), lots_of_objects);
  for (size_t i = 0; i < arraysize(lots_of_objects); i++) {
    EXPECT_TRUE(lots_of_objects[i] == NULL);
  }

  // Succeeds, fits by adjusting the max allowed footprint.
  for (size_t i = 0; i < arraysize(lots_of_objects); i++) {
    lots_of_objects[i] = space->AllocWithGrowth(1024);
    EXPECT_TRUE(lots_of_objects[i] != NULL);
  }

  // Release memory and check pointers are NULL
  space->FreeList(arraysize(lots_of_objects), lots_of_objects);
  for (size_t i = 0; i < arraysize(lots_of_objects); i++) {
    EXPECT_TRUE(lots_of_objects[i] == NULL);
  }
}

static size_t test_rand() {
  // TODO: replace this with something random yet deterministic
  return rand();
}

void SpaceTest::SizeFootPrintGrowthLimitAndTrimBody(AllocSpace* space, intptr_t object_size,
                                                    int round, size_t growth_limit) {
  if (((object_size > 0 && object_size >= static_cast<intptr_t>(growth_limit))) ||
      ((object_size < 0 && -object_size >= static_cast<intptr_t>(growth_limit)))) {
    // No allocation can succeed
    return;
  }
  // Mspace for raw dlmalloc operations
  void* mspace = space->GetMspace();

  // mspace's footprint equals amount of resources requested from system
  size_t footprint = mspace_footprint(mspace);

  // mspace must at least have its book keeping allocated
  EXPECT_GT(footprint, 0u);

  // mspace but it shouldn't exceed the initial size
  EXPECT_LE(footprint, growth_limit);

  // space's size shouldn't exceed the initial size
  EXPECT_LE(space->Size(), growth_limit);

  // this invariant should always hold or else the mspace has grown to be larger than what the
  // space believes its size is (which will break invariants)
  EXPECT_GE(space->Size(), footprint);

  // Fill the space with lots of small objects up to the growth limit
  size_t max_objects = (growth_limit / (object_size > 0 ? object_size : 8)) + 1;
  UniquePtr<Object*[]> lots_of_objects(new Object*[max_objects]);
  size_t last_object = 0;  // last object for which allocation succeeded
  size_t amount_allocated = 0;  // amount of space allocated
  for (size_t i = 0; i < max_objects; i++) {
    size_t alloc_fails = 0;  // number of failed allocations
    size_t max_fails = 30;  // number of times we fail allocation before giving up
    for (; alloc_fails < max_fails; alloc_fails++) {
      size_t alloc_size;
      if (object_size > 0) {
        alloc_size = object_size;
      } else {
        alloc_size = test_rand() % static_cast<size_t>(-object_size);
        if (alloc_size < 8) {
          alloc_size = 8;
        }
      }
      Object* object;
      if (round <= 1) {
        object = space->AllocWithoutGrowth(alloc_size);
      } else {
        object = space->AllocWithGrowth(alloc_size);
      }
      footprint = mspace_footprint(mspace);
      EXPECT_GE(space->Size(), footprint);  // invariant
      if (object != NULL) {  // allocation succeeded
        lots_of_objects.get()[i] = object;
        size_t allocation_size = space->AllocationSize(object);
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
    footprint = mspace_footprint(mspace);
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
      Object* object = lots_of_objects.get()[i];
      if (object == NULL) {
        continue;
      }
      size_t allocation_size = space->AllocationSize(object);
      if (object_size > 0) {
        EXPECT_GE(allocation_size, static_cast<size_t>(object_size));
      } else {
        EXPECT_GE(allocation_size, 8u);
      }
      space->Free(object);
      lots_of_objects.get()[i] = NULL;
      amount_allocated -= allocation_size;
      footprint = mspace_footprint(mspace);
      EXPECT_GE(space->Size(), footprint);  // invariant
    }

    free_increment >>= 1;
  }

  // All memory was released, try a large allocation to check freed memory is being coalesced
  Object* large_object;
  size_t three_quarters_space = (growth_limit / 2) + (growth_limit / 4);
  if (round <= 1) {
    large_object = space->AllocWithoutGrowth(three_quarters_space);
  } else {
    large_object = space->AllocWithGrowth(three_quarters_space);
  }
  EXPECT_TRUE(large_object != NULL);

  // Sanity check footprint
  footprint = mspace_footprint(mspace);
  EXPECT_LE(footprint, growth_limit);
  EXPECT_GE(space->Size(), footprint);
  EXPECT_LE(space->Size(), growth_limit);

  // Clean up
  space->Free(large_object);

  // Sanity check footprint
  footprint = mspace_footprint(mspace);
  EXPECT_LE(footprint, growth_limit);
  EXPECT_GE(space->Size(), footprint);
  EXPECT_LE(space->Size(), growth_limit);
}

void SpaceTest::SizeFootPrintGrowthLimitAndTrimDriver(size_t object_size) {
  size_t initial_size = 4 * MB;
  size_t growth_limit = 8 * MB;
  size_t capacity = 16 * MB;
  AllocSpace* space(Space::CreateAllocSpace("test", initial_size, growth_limit, capacity, NULL));
  ASSERT_TRUE(space != NULL);

  // Basic sanity
  EXPECT_EQ(space->Capacity(), growth_limit);
  EXPECT_EQ(space->NonGrowthLimitCapacity(), capacity);

  // Make space findable to the heap, will also delete space when runtime is cleaned up
  Runtime::Current()->GetHeap()->AddSpace(space);

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
  TEST_F(SpaceTest, SizeFootPrintGrowthLimitAndTrim_AllocationsOf_##name) { \
    SizeFootPrintGrowthLimitAndTrimDriver(size); \
  } \
  TEST_F(SpaceTest, SizeFootPrintGrowthLimitAndTrim_RandomAllocationsWithMax_##name) { \
    SizeFootPrintGrowthLimitAndTrimDriver(-size); \
  }

// Each size test is its own test so that we get a fresh heap each time
TEST_F(SpaceTest, SizeFootPrintGrowthLimitAndTrim_AllocationsOf_8B) {
  SizeFootPrintGrowthLimitAndTrimDriver(8);
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

}  // namespace art
