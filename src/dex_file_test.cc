// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/common_test.h"
#include "src/dex_file.h"
#include "src/object.h"
#include "src/scoped_ptr.h"

#include <stdio.h>
#include "gtest/gtest.h"

namespace art {

TEST(DexFileTest, Open) {
  scoped_ptr<DexFile> dex(DexFile::OpenBase64(kNestedDex));
  ASSERT_TRUE(dex != NULL);
}

}  // namespace art
