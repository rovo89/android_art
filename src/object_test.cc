// Copyright 2011 Google Inc. All Rights Reserved.
// Author: cshapiro@google.com (Carl Shapiro)

#include "src/object.h"

#include <stdio.h>
#include "gtest/gtest.h"

namespace art {

TEST(Object, IsInSamePackage) {
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

}  // namespace art
