// Copyright 2011 Google Inc. All Rights Reserved.

#include "space.h"

#include "gtest/gtest.h"

#include "globals.h"
#include "scoped_ptr.h"

namespace art {

TEST(SpaceTest, Init) {
  {
    // Less than
    scoped_ptr<Space> space(Space::Create(16 * MB, 32 * MB, NULL));
    EXPECT_TRUE(space != NULL);
  }
  {
    // Equal to
    scoped_ptr<Space> space(Space::Create(16 * MB, 16 * MB, NULL));
    EXPECT_TRUE(space != NULL);
  }
  {
    // Greater than
    scoped_ptr<Space> space(Space::Create(32 * MB, 16 * MB, NULL));
    EXPECT_TRUE(space == NULL);
  }
}

TEST(SpaceTest, AllocAndFree) {
  scoped_ptr<Space> space(Space::Create(4 * MB, 16 * MB, NULL));
  ASSERT_TRUE(space != NULL);

  // Succeeds, fits without adjusting the max allowed footprint.
  void* ptr1 = space->AllocWithoutGrowth(1 * MB);
  EXPECT_TRUE(ptr1 != NULL);

  // Fails, requires a higher allowed footprint.
  void* ptr2 = space->AllocWithoutGrowth(8 * MB);
  EXPECT_TRUE(ptr2 == NULL);

  // Succeeds, adjusts the footprint.
  void* ptr3 = space->AllocWithGrowth(8 * MB);
  EXPECT_TRUE(ptr3 != NULL);

  // Fails, requires a higher allowed footprint.
  void* ptr4 = space->AllocWithoutGrowth(8 * MB);
  EXPECT_FALSE(ptr4 != NULL);

  // Also fails, requires a higher allowed footprint.
  void* ptr5 = space->AllocWithGrowth(8 * MB);
  EXPECT_FALSE(ptr5 != NULL);

  // Release some memory.
  size_t free3 = space->Free(ptr3);
  EXPECT_LE(8U * MB, free3);

  // Succeeds, now that memory has been freed.
  void* ptr6 = space->AllocWithGrowth(9 * MB);
  EXPECT_TRUE(ptr6 != NULL);

  // Final clean up.
  size_t free1 = space->Free(ptr1);
  EXPECT_LE(1U * MB, free1);
}

}  // namespace art
