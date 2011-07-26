// Copyright 2011 Google Inc. All Rights Reserved.
// Author: cshapiro@google.com (Carl Shapiro)

#include "class_linker.h"
#include "common_test.h"
#include "dex_file.h"
#include "heap.h"
#include "object.h"
#include "scoped_ptr.h"

#include <stdio.h>
#include "gtest/gtest.h"

namespace art {

class ObjectTest : public RuntimeTest {};

TEST_F(ObjectTest, IsInSamePackage) {
  // Matches
  EXPECT_TRUE(Class::IsInSamePackage("Ljava/lang/Object;",
                                     "Ljava/lang/Class"));
  EXPECT_TRUE(Class::IsInSamePackage("LFoo;",
                                     "LBar;"));

  // Mismatches
  EXPECT_FALSE(Class::IsInSamePackage("Ljava/lang/Object;",
                                      "Ljava/io/File;"));
  EXPECT_FALSE(Class::IsInSamePackage("Ljava/lang/Object;",
                                      "Ljava/lang/reflect/Method;"));
}

TEST_F(ObjectTest, AllocObjectArray) {
    ObjectArray<Object>* oa = class_linker_->AllocObjectArray<Object>(2);
    EXPECT_EQ(2U, oa->GetLength());
    EXPECT_TRUE(oa->Get(0) == NULL);
    EXPECT_TRUE(oa->Get(1) == NULL);
    oa->Set(0, oa);
    EXPECT_TRUE(oa->Get(0) == oa);
    EXPECT_TRUE(oa->Get(1) == NULL);
    oa->Set(1, oa);
    EXPECT_TRUE(oa->Get(0) == oa);
    EXPECT_TRUE(oa->Get(1) == oa);
}

}  // namespace art
