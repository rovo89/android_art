// Copyright 2011 Google Inc. All Rights Reserved.

#include "object.h"
#include "common_test.h"
#include "utils.h"

#include "gtest/gtest.h"

namespace art {

class UtilsTest : public CommonTest {
};

#define EXPECT_DESCRIPTOR(pretty_descriptor, descriptor) \
  do { \
    String* s = String::AllocFromModifiedUtf8(descriptor); \
    std::string result(PrettyDescriptor(s)); \
    EXPECT_EQ(pretty_descriptor, result); \
  } while (false)

TEST_F(UtilsTest, PrettyDescriptor_ArrayReferences) {
  EXPECT_DESCRIPTOR("java.lang.Class[]", "[Ljava/lang/Class;");
  EXPECT_DESCRIPTOR("java.lang.Class[][]", "[[Ljava/lang/Class;");
}

TEST_F(UtilsTest, PrettyDescriptor_ScalarReferences) {
  EXPECT_DESCRIPTOR("java.lang.String", "Ljava.lang.String;");
  EXPECT_DESCRIPTOR("java.lang.String", "Ljava/lang/String;");
}

TEST_F(UtilsTest, PrettyDescriptor_PrimitiveArrays) {
  EXPECT_DESCRIPTOR("boolean[]", "[Z");
  EXPECT_DESCRIPTOR("boolean[][]", "[[Z");
  EXPECT_DESCRIPTOR("byte[]", "[B");
  EXPECT_DESCRIPTOR("byte[][]", "[[B");
  EXPECT_DESCRIPTOR("char[]", "[C");
  EXPECT_DESCRIPTOR("char[][]", "[[C");
  EXPECT_DESCRIPTOR("double[]", "[D");
  EXPECT_DESCRIPTOR("double[][]", "[[D");
  EXPECT_DESCRIPTOR("float[]", "[F");
  EXPECT_DESCRIPTOR("float[][]", "[[F");
  EXPECT_DESCRIPTOR("int[]", "[I");
  EXPECT_DESCRIPTOR("int[][]", "[[I");
  EXPECT_DESCRIPTOR("long[]", "[J");
  EXPECT_DESCRIPTOR("long[][]", "[[J");
  EXPECT_DESCRIPTOR("short[]", "[S");
  EXPECT_DESCRIPTOR("short[][]", "[[S");
}

TEST_F(UtilsTest, PrettyDescriptor_PrimitiveScalars) {
  EXPECT_DESCRIPTOR("boolean", "Z");
  EXPECT_DESCRIPTOR("byte", "B");
  EXPECT_DESCRIPTOR("char", "C");
  EXPECT_DESCRIPTOR("double", "D");
  EXPECT_DESCRIPTOR("float", "F");
  EXPECT_DESCRIPTOR("int", "I");
  EXPECT_DESCRIPTOR("long", "J");
  EXPECT_DESCRIPTOR("short", "S");
}

TEST_F(UtilsTest, PrettyType) {
  EXPECT_EQ("null", PrettyType(NULL));

  String* s = String::AllocFromModifiedUtf8("");
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
