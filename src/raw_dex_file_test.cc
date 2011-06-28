// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/raw_dex_file.h"

#include <stdio.h>
#include "gtest/gtest.h"

namespace art {

static const char* filename =
    "/usr/local/google/work/dalvik-dev-git/Nested.dex";

TEST(RawDexFile, Open) {
  RawDexFile* raw = RawDexFile::Open(filename);
  ASSERT_TRUE(raw != NULL);
  delete raw;
}

TEST(RawDexFile, ClassDefs) {
  RawDexFile* raw = RawDexFile::Open(filename);
  ASSERT_TRUE(raw != NULL);
  EXPECT_EQ(2U, raw->NumClassDefs());

  const RawDexFile::ClassDef& c0 = raw->GetClassDef(0);
  EXPECT_STREQ("LNested$Inner;", raw->GetClassDescriptor(c0));

  const RawDexFile::ClassDef& c1 = raw->GetClassDef(1);
  EXPECT_STREQ("LNested;", raw->GetClassDescriptor(c1));

  delete raw;
}

}  // namespace art
