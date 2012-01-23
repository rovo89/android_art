// Copyright 2011 Google Inc. All Rights Reserved.

#include "space.h"

#include "common_test.h"
#include "globals.h"
#include "UniquePtr.h"

namespace art {

class SpaceTest : public CommonTest {};

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

TEST_F(SpaceTest, AllocAndFree) {
  AllocSpace* space(Space::CreateAllocSpace("test", 4 * MB, 16 * MB, 16 * MB, NULL));
  ASSERT_TRUE(space != NULL);

  // Make space findable to the heap, will also delete class when runtime is cleaned up
  Heap::AddSpace(space);

  // Succeeds, fits without adjusting the max allowed footprint.
  Object* ptr1 = space->AllocWithoutGrowth(1 * MB);
  EXPECT_TRUE(ptr1 != NULL);

  // Fails, requires a higher allowed footprint.
  Object* ptr2 = space->AllocWithoutGrowth(8 * MB);
  EXPECT_TRUE(ptr2 == NULL);

  // Succeeds, adjusts the footprint.
  Object* ptr3 = space->AllocWithGrowth(8 * MB);
  EXPECT_TRUE(ptr3 != NULL);

  // Fails, requires a higher allowed footprint.
  Object* ptr4 = space->AllocWithoutGrowth(8 * MB);
  EXPECT_FALSE(ptr4 != NULL);

  // Also fails, requires a higher allowed footprint.
  Object* ptr5 = space->AllocWithGrowth(8 * MB);
  EXPECT_FALSE(ptr5 != NULL);

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

}  // namespace art
