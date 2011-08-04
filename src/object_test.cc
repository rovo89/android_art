// Copyright 2011 Google Inc. All Rights Reserved.
// Author: cshapiro@google.com (Carl Shapiro)

#include "class_linker.h"
#include "common_test.h"
#include "dex_file.h"
#include "heap.h"
#include "object.h"
#include "scoped_ptr.h"

#include <stdint.h>
#include <stdio.h>
#include "gtest/gtest.h"

namespace art {

class ObjectTest : public RuntimeTest {
 protected:
  void AssertString(int32_t length,
                    const char* utf8_in,
                    const char* utf16_expected_le,
                    int32_t hash_expected) {
    uint16_t utf16_expected[length];
    for (int32_t i = 0; i < length; i++) {
      uint16_t ch = (((utf16_expected_le[i*2 + 0] & 0xff) << 8) |
                     ((utf16_expected_le[i*2 + 1] & 0xff) << 0));
      utf16_expected[i] = ch;
    }

    String* string = String::AllocFromModifiedUtf8(length, utf8_in);
    ASSERT_EQ(length, string->GetLength());
    ASSERT_TRUE(string->GetCharArray() != NULL);
    ASSERT_TRUE(string->GetCharArray()->GetChars() != NULL);
    // strlen is necessary because the 1-character string "\0" is interpreted as ""
    ASSERT_TRUE(string->Equals(utf8_in) || length != static_cast<int32_t>(strlen(utf8_in)));
    for (int32_t i = 0; i < length; i++) {
      EXPECT_EQ(utf16_expected[i], string->GetCharArray()->GetChar(i));
    }
    EXPECT_EQ(hash_expected, string->GetHashCode());
  }
};

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

TEST_F(ObjectTest, String) {
  // Test the empty string.
  AssertString(0, "",     "", 0);

  // Test one-byte characters.
  AssertString(1, " ",    "\x00\x20",         0x20);
  AssertString(1, "",     "\x00\x00",         0);
  AssertString(1, "\x7f", "\x00\x7f",         0x7f);
  AssertString(2, "hi",   "\x00\x68\x00\x69", (31 * 0x68) + 0x69);

  // Test two-byte characters.
  AssertString(1, "\xc2\x80",   "\x00\x80",                 0x80);
  AssertString(1, "\xd9\xa6",   "\x06\x66",                 0x0666);
  AssertString(1, "\xdf\xbf",   "\x07\xff",                 0x07ff);
  AssertString(3, "h\xd9\xa6i", "\x00\x68\x06\x66\x00\x69", (31 * ((31 * 0x68) + 0x0666)) + 0x69);

  // Test three-byte characters.
  AssertString(1, "\xe0\xa0\x80",   "\x08\x00",                 0x0800);
  AssertString(1, "\xe1\x88\xb4",   "\x12\x34",                 0x1234);
  AssertString(1, "\xef\xbf\xbf",   "\xff\xff",                 0xffff);
  AssertString(3, "h\xe1\x88\xb4i", "\x00\x68\x12\x34\x00\x69", (31 * ((31 * 0x68) + 0x1234)) + 0x69);
}

TEST_F(ObjectTest, StringEqualsUtf8) {
  String* string = String::AllocFromAscii("android");
  EXPECT_TRUE(string->Equals("android"));
  EXPECT_FALSE(string->Equals("Android"));
  EXPECT_FALSE(string->Equals("ANDROID"));
  EXPECT_FALSE(string->Equals(""));
  EXPECT_FALSE(string->Equals("and"));
  EXPECT_FALSE(string->Equals("androids"));

  String* empty = String::AllocFromAscii("");
  EXPECT_TRUE(empty->Equals(""));
  EXPECT_FALSE(empty->Equals("a"));
}

TEST_F(ObjectTest, StringEquals) {
  String* string = String::AllocFromAscii("android");
  EXPECT_TRUE(string->Equals(String::AllocFromAscii("android")));
  EXPECT_FALSE(string->Equals("Android"));
  EXPECT_FALSE(string->Equals("ANDROID"));
  EXPECT_FALSE(string->Equals(""));
  EXPECT_FALSE(string->Equals("and"));
  EXPECT_FALSE(string->Equals("androids"));

  String* empty = String::AllocFromAscii("");
  EXPECT_TRUE(empty->Equals(""));
  EXPECT_FALSE(empty->Equals("a"));
}

TEST_F(ObjectTest, DescriptorCompare) {
  ClassLinker* linker = class_linker_;

  scoped_ptr<DexFile> proto1_dex_file(OpenDexFileBase64(kProtoCompareDex));
  PathClassLoader* class_loader_1 = AllocPathClassLoader(proto1_dex_file.get());
  scoped_ptr<DexFile> proto2_dex_file(OpenDexFileBase64(kProtoCompare2Dex));
  PathClassLoader* class_loader_2 = AllocPathClassLoader(proto2_dex_file.get());

  Class* klass1 = linker->FindClass("LProtoCompare;", class_loader_1);
  ASSERT_TRUE(klass1 != NULL);
  Class* klass2 = linker->FindClass("LProtoCompare2;", class_loader_2);
  ASSERT_TRUE(klass2 != NULL);

  Method* m1_1 = klass1->GetVirtualMethod(0);
  EXPECT_TRUE(m1_1->GetName()->Equals("m1"));
  Method* m2_1 = klass1->GetVirtualMethod(1);
  EXPECT_TRUE(m2_1->GetName()->Equals("m2"));
  Method* m3_1 = klass1->GetVirtualMethod(2);
  EXPECT_TRUE(m3_1->GetName()->Equals("m3"));
  Method* m4_1 = klass1->GetVirtualMethod(3);
  EXPECT_TRUE(m4_1->GetName()->Equals("m4"));

  Method* m1_2 = klass2->GetVirtualMethod(0);
  EXPECT_TRUE(m1_2->GetName()->Equals("m1"));
  Method* m2_2 = klass2->GetVirtualMethod(1);
  EXPECT_TRUE(m2_2->GetName()->Equals("m2"));
  Method* m3_2 = klass2->GetVirtualMethod(2);
  EXPECT_TRUE(m3_2->GetName()->Equals("m3"));
  Method* m4_2 = klass2->GetVirtualMethod(3);
  EXPECT_TRUE(m4_2->GetName()->Equals("m4"));

  EXPECT_TRUE(m1_1->HasSameNameAndDescriptor(m1_2));
  EXPECT_TRUE(m1_2->HasSameNameAndDescriptor(m1_1));

  EXPECT_TRUE(m2_1->HasSameNameAndDescriptor(m2_2));
  EXPECT_TRUE(m2_2->HasSameNameAndDescriptor(m2_1));

  EXPECT_TRUE(m3_1->HasSameNameAndDescriptor(m3_2));
  EXPECT_TRUE(m3_2->HasSameNameAndDescriptor(m3_1));

  EXPECT_TRUE(m4_1->HasSameNameAndDescriptor(m4_2));
  EXPECT_TRUE(m4_2->HasSameNameAndDescriptor(m4_1));
}

}  // namespace art
