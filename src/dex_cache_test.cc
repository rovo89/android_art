// Copyright 2011 Google Inc. All Rights Reserved.

#include "class_linker.h"
#include "common_test.h"
#include "dex_cache.h"
#include "heap.h"
#include "object.h"
#include "scoped_ptr.h"

#include <stdio.h>
#include "gtest/gtest.h"

namespace art {

class DexCacheTest : public CommonTest {};

TEST_F(DexCacheTest, Open) {

  DexCache* dex_cache = class_linker_->AllocDexCache();
  ASSERT_TRUE(dex_cache != NULL);
  dex_cache->Init(class_linker_->AllocObjectArray<String>(1),
                  class_linker_->AllocObjectArray<Class>(2),
                  class_linker_->AllocObjectArray<Method>(3),
                  class_linker_->AllocObjectArray<Field>(4));
  EXPECT_EQ(1U, dex_cache->NumStrings());
  EXPECT_EQ(2U, dex_cache->NumClasses());
  EXPECT_EQ(3U, dex_cache->NumMethods());
  EXPECT_EQ(4U, dex_cache->NumFields());
}

}  // namespace art
