// Copyright 2011 Google Inc. All Rights Reserved.

#include "object.h"
#include "common_test.h"
#include "utils.h"

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

TEST_F(UtilsTest, MangleForJni) {
  EXPECT_EQ("hello_00024world", MangleForJni("hello$world"));
  EXPECT_EQ("hello_000a9world", MangleForJni("hello\xc2\xa9world"));
  EXPECT_EQ("hello_1world", MangleForJni("hello_world"));
  EXPECT_EQ("Ljava_lang_String_2", MangleForJni("Ljava/lang/String;"));
  EXPECT_EQ("_3C", MangleForJni("[C"));
}

TEST_F(UtilsTest, JniShortName_JniLongName) {
  Class* c = class_linker_->FindSystemClass("Ljava/lang/String;");
  ASSERT_TRUE(c != NULL);
  Method* m;

  m = c->FindVirtualMethod("charAt", "(I)C");
  ASSERT_TRUE(m != NULL);
  EXPECT_EQ("Java_java_lang_String_charAt", JniShortName(m));
  EXPECT_EQ("Java_java_lang_String_charAt__I", JniLongName(m));

  m = c->FindVirtualMethod("indexOf", "(Ljava/lang/String;I)I");
  ASSERT_TRUE(m != NULL);
  EXPECT_EQ("Java_java_lang_String_indexOf", JniShortName(m));
  EXPECT_EQ("Java_java_lang_String_indexOf__Ljava_lang_String_2I", JniLongName(m));

  m = c->FindDirectMethod("copyValueOf", "([CII)Ljava/lang/String;");
  ASSERT_TRUE(m != NULL);
  EXPECT_EQ("Java_java_lang_String_copyValueOf", JniShortName(m));
  EXPECT_EQ("Java_java_lang_String_copyValueOf___3CII", JniLongName(m));
}

TEST_F(UtilsTest, Split) {
  std::vector<std::string> actual;
  std::vector<std::string> expected;

  expected.clear();

  actual.clear();
  Split("", ':', actual);
  EXPECT_EQ(expected, actual);

  actual.clear();
  Split(":", ':', actual);
  EXPECT_EQ(expected, actual);

  expected.clear();
  expected.push_back("foo");

  actual.clear();
  Split(":foo", ':', actual);
  EXPECT_EQ(expected, actual);

  actual.clear();
  Split("foo:", ':', actual);
  EXPECT_EQ(expected, actual);

  actual.clear();
  Split(":foo:", ':', actual);
  EXPECT_EQ(expected, actual);

  expected.push_back("bar");

  actual.clear();
  Split("foo:bar", ':', actual);
  EXPECT_EQ(expected, actual);

  actual.clear();
  Split(":foo:bar", ':', actual);
  EXPECT_EQ(expected, actual);

  actual.clear();
  Split("foo:bar:", ':', actual);
  EXPECT_EQ(expected, actual);

  actual.clear();
  Split(":foo:bar:", ':', actual);
  EXPECT_EQ(expected, actual);

  expected.push_back("baz");

  actual.clear();
  Split("foo:bar:baz", ':', actual);
  EXPECT_EQ(expected, actual);

  actual.clear();
  Split(":foo:bar:baz", ':', actual);
  EXPECT_EQ(expected, actual);

  actual.clear();
  Split("foo:bar:baz:", ':', actual);
  EXPECT_EQ(expected, actual);

  actual.clear();
  Split(":foo:bar:baz:", ':', actual);
  EXPECT_EQ(expected, actual);
}

}  // namespace art
