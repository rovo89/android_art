// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/dex_file.h"
#include "src/scoped_ptr.h"

#include <stdio.h>
#include "gtest/gtest.h"

namespace art {

static const char* filename =
    "/usr/local/google/work/dalvik-dev-git/Nested.dex";

TEST(DexFile, Open) {
  scoped_ptr<DexFile> dex(DexFile::Open(filename));
  ASSERT_TRUE(dex != NULL);
}

TEST(DexFile, LoadNonexistent) {
  scoped_ptr<DexFile> dex(DexFile::Open(filename));
  ASSERT_TRUE(dex != NULL);

  Class* klass = dex->LoadClass("NoSuchClass");
  ASSERT_TRUE(klass == NULL);
}

TEST(DexFile, Load) {
  scoped_ptr<DexFile> dex(DexFile::Open(filename));
  ASSERT_TRUE(dex != NULL);

  Class* klass = dex->LoadClass("LNested;");
  ASSERT_TRUE(klass != NULL);
}

}  // namespace art
