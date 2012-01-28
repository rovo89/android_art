// Copyright 2011 Google Inc. All Rights Reserved.

#include "object.h"
#include "common_test.h"
#include "utils.h"

namespace art {

class UtilsTest : public CommonTest {
};

#define EXPECT_DESCRIPTOR(pretty_descriptor, descriptor) \
  do { \
    SirtRef<String> s(String::AllocFromModifiedUtf8(descriptor)); \
    std::string result(PrettyDescriptor(s.get())); \
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

TEST_F(UtilsTest, PrettyTypeOf) {
  EXPECT_EQ("null", PrettyTypeOf(NULL));

  SirtRef<String> s(String::AllocFromModifiedUtf8(""));
  EXPECT_EQ("java.lang.String", PrettyTypeOf(s.get()));

  SirtRef<ShortArray> a(ShortArray::Alloc(2));
  EXPECT_EQ("short[]", PrettyTypeOf(a.get()));

  Class* c = class_linker_->FindSystemClass("[Ljava/lang/String;");
  ASSERT_TRUE(c != NULL);
  Object* o = ObjectArray<String>::Alloc(c, 0);
  EXPECT_EQ("java.lang.String[]", PrettyTypeOf(o));
  EXPECT_EQ("java.lang.Class<java.lang.String[]>", PrettyTypeOf(o->GetClass()));
}

TEST_F(UtilsTest, PrettyClass) {
  EXPECT_EQ("null", PrettyClass(NULL));
  Class* c = class_linker_->FindSystemClass("[Ljava/lang/String;");
  ASSERT_TRUE(c != NULL);
  Object* o = ObjectArray<String>::Alloc(c, 0);
  EXPECT_EQ("java.lang.Class<java.lang.String[]>", PrettyClass(o->GetClass()));
}

TEST_F(UtilsTest, PrettyClassAndClassLoader) {
  EXPECT_EQ("null", PrettyClassAndClassLoader(NULL));
  Class* c = class_linker_->FindSystemClass("[Ljava/lang/String;");
  ASSERT_TRUE(c != NULL);
  Object* o = ObjectArray<String>::Alloc(c, 0);
  EXPECT_EQ("java.lang.Class<java.lang.String[],null>", PrettyClassAndClassLoader(o->GetClass()));
}

TEST_F(UtilsTest, PrettyField) {
  EXPECT_EQ("null", PrettyField(NULL));

  Class* java_lang_String = class_linker_->FindSystemClass("Ljava/lang/String;");

  Field* f;
  f = java_lang_String->FindDeclaredInstanceField("count", "I");
  EXPECT_EQ("int java.lang.String.count", PrettyField(f));
  EXPECT_EQ("java.lang.String.count", PrettyField(f, false));
  f = java_lang_String->FindDeclaredInstanceField("value", "[C");
  EXPECT_EQ("char[] java.lang.String.value", PrettyField(f));
  EXPECT_EQ("java.lang.String.value", PrettyField(f, false));
}

TEST_F(UtilsTest, PrettySize) {
  EXPECT_EQ("1GB", PrettySize(1 * GB));
  EXPECT_EQ("2GB", PrettySize(2 * GB));
  if (sizeof(size_t) > sizeof(uint32_t)) {
    EXPECT_EQ("100GB", PrettySize(100 * GB));
  }
  EXPECT_EQ("1MB", PrettySize(1 * MB));
  EXPECT_EQ("10MB", PrettySize(10 * MB));
  EXPECT_EQ("100MB", PrettySize(100 * MB));
  EXPECT_EQ("1KiB", PrettySize(1 * KB));
  EXPECT_EQ("10KiB", PrettySize(10 * KB));
  EXPECT_EQ("100KiB", PrettySize(100 * KB));
  EXPECT_EQ("1B", PrettySize(1));
  EXPECT_EQ("10B", PrettySize(10));
  EXPECT_EQ("100B", PrettySize(100));
}

TEST_F(UtilsTest, PrettyDuration) {
  const uint64_t one_sec = 1000000000;
  const uint64_t one_ms  = 1000000;
  const uint64_t one_us  = 1000;

  EXPECT_EQ("1s", PrettyDuration(1 * one_sec));
  EXPECT_EQ("10s", PrettyDuration(10 * one_sec));
  EXPECT_EQ("100s", PrettyDuration(100 * one_sec));
  EXPECT_EQ("1.001s", PrettyDuration(1 * one_sec + one_ms));
  EXPECT_EQ("1.000001s", PrettyDuration(1 * one_sec + one_us));
  EXPECT_EQ("1.000000001s", PrettyDuration(1 * one_sec + 1));

  EXPECT_EQ("1ms", PrettyDuration(1 * one_ms));
  EXPECT_EQ("10ms", PrettyDuration(10 * one_ms));
  EXPECT_EQ("100ms", PrettyDuration(100 * one_ms));
  EXPECT_EQ("1.001ms", PrettyDuration(1 * one_ms + one_us));
  EXPECT_EQ("1.000001ms", PrettyDuration(1 * one_ms + 1));

  EXPECT_EQ("1us", PrettyDuration(1 * one_us));
  EXPECT_EQ("10us", PrettyDuration(10 * one_us));
  EXPECT_EQ("100us", PrettyDuration(100 * one_us));
  EXPECT_EQ("1.001us", PrettyDuration(1 * one_us + 1));

  EXPECT_EQ("1ns", PrettyDuration(1));
  EXPECT_EQ("10ns", PrettyDuration(10));
  EXPECT_EQ("100ns", PrettyDuration(100));
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
