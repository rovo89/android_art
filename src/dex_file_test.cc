// Copyright 2011 Google Inc. All Rights Reserved.

#include "class_linker.h"
#include "common_test.h"
#include "dex_file.h"
#include "heap.h"
#include "object.h"
#include "scoped_ptr.h"

#include <stdio.h>
#include "gtest/gtest.h"

namespace art {

class DexFileTest : public RuntimeTest {};

TEST_F(DexFileTest, Open) {

  DexFile* dex_file = down_cast<DexFile*>(class_linker_->AllocObjectArray(DexFile::kMax));
  ASSERT_TRUE(dex_file != NULL);
  dex_file->Init(class_linker_->AllocObjectArray(1),
                 class_linker_->AllocObjectArray(2),
                 class_linker_->AllocObjectArray(3),
                 class_linker_->AllocObjectArray(4));
  EXPECT_EQ(1U, dex_file->NumStrings());
  EXPECT_EQ(2U, dex_file->NumClasses());
  EXPECT_EQ(3U, dex_file->NumMethods());
  EXPECT_EQ(4U, dex_file->NumFields());
}

}  // namespace art
