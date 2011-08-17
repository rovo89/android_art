// Copyright 2011 Google Inc. All Rights Reserved.

#include "object.h"
#include "common_test.h"
#include "utils.h"

#include "gtest/gtest.h"

namespace art {

class UtilsTest : public CommonTest {
};

TEST(PrettyDescriptorTest, ArrayReferences) {
  EXPECT_EQ("java.lang.Class[]", PrettyDescriptor("[Ljava/lang/Class;"));
  EXPECT_EQ("java.lang.Class[][]", PrettyDescriptor("[[Ljava/lang/Class;"));
}

TEST(PrettyDescriptorTest, ScalarReferences) {
  EXPECT_EQ("java.lang.String", PrettyDescriptor("Ljava.lang.String;"));
  EXPECT_EQ("java.lang.String", PrettyDescriptor("Ljava/lang/String;"));
}

TEST(PrettyDescriptorTest, PrimitiveArrays) {
  EXPECT_EQ("boolean[]", PrettyDescriptor("[Z"));
  EXPECT_EQ("boolean[][]", PrettyDescriptor("[[Z"));
  EXPECT_EQ("byte[]", PrettyDescriptor("[B"));
  EXPECT_EQ("byte[][]", PrettyDescriptor("[[B"));
  EXPECT_EQ("char[]", PrettyDescriptor("[C"));
  EXPECT_EQ("char[][]", PrettyDescriptor("[[C"));
  EXPECT_EQ("double[]", PrettyDescriptor("[D"));
  EXPECT_EQ("double[][]", PrettyDescriptor("[[D"));
  EXPECT_EQ("float[]", PrettyDescriptor("[F"));
  EXPECT_EQ("float[][]", PrettyDescriptor("[[F"));
  EXPECT_EQ("int[]", PrettyDescriptor("[I"));
  EXPECT_EQ("int[][]", PrettyDescriptor("[[I"));
  EXPECT_EQ("long[]", PrettyDescriptor("[J"));
  EXPECT_EQ("long[][]", PrettyDescriptor("[[J"));
  EXPECT_EQ("short[]", PrettyDescriptor("[S"));
  EXPECT_EQ("short[][]", PrettyDescriptor("[[S"));
}

TEST(PrettyDescriptorTest, PrimitiveScalars) {
  EXPECT_EQ("boolean", PrettyDescriptor("Z"));
  EXPECT_EQ("byte", PrettyDescriptor("B"));
  EXPECT_EQ("char", PrettyDescriptor("C"));
  EXPECT_EQ("double", PrettyDescriptor("D"));
  EXPECT_EQ("float", PrettyDescriptor("F"));
  EXPECT_EQ("int", PrettyDescriptor("I"));
  EXPECT_EQ("long", PrettyDescriptor("J"));
  EXPECT_EQ("short", PrettyDescriptor("S"));
}

TEST_F(UtilsTest, PrettyType) {
  EXPECT_EQ("null", PrettyType(NULL));

  String* s = String::AllocFromModifiedUtf8(0, "");
  EXPECT_EQ("java.lang.String", PrettyType(s));

  ShortArray* a = ShortArray::Alloc(2);
  EXPECT_EQ("short[]", PrettyType(a));

  Class* c = class_linker_->FindSystemClass("[Ljava/lang/String;");
  ASSERT_TRUE(c != NULL);
  Object* o = ObjectArray<String>::Alloc(c, 0);
  EXPECT_EQ("java.lang.String[]", PrettyType(o));
  EXPECT_EQ("java.lang.Class<java.lang.String[]>", PrettyType(o->GetClass()));
}

}  // namespace art
