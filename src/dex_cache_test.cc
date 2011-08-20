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

  DexCache* dex_cache = class_linker_->AllocDexCache(java_lang_dex_file_.get());
  ASSERT_TRUE(dex_cache != NULL);
  EXPECT_EQ(java_lang_dex_file_->NumStringIds(), dex_cache->NumStrings());
  EXPECT_EQ(java_lang_dex_file_->NumTypeIds(),   dex_cache->NumClasses());
  EXPECT_EQ(java_lang_dex_file_->NumMethodIds(), dex_cache->NumMethods());
  EXPECT_EQ(java_lang_dex_file_->NumFieldIds(),  dex_cache->NumFields());
}

}  // namespace art
