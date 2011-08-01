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
  void AssertString(size_t length,
                    const char* utf8_in,
                    const char* utf16_expected_le,
                    int32_t hash_expected) {
    uint16_t utf16_expected[length];
    for (size_t i = 0; i < length; i++) {
      uint16_t ch = (((utf16_expected_le[i*2 + 0] & 0xff) << 8) |
                     ((utf16_expected_le[i*2 + 1] & 0xff) << 0));
      utf16_expected[i] = ch;
    }

    String* string = class_linker_->AllocStringFromModifiedUtf8(length, utf8_in);
    ASSERT_EQ(length,  static_cast<size_t>(string->count_));
    ASSERT_TRUE(string->array_ != NULL);
    ASSERT_TRUE(string->array_->GetChars() != NULL);
    // strlen is necessary because the 1-character string "\0" is interpreted as ""
    ASSERT_TRUE(String::Equals(string, utf8_in) || length != strlen(utf8_in));
    for (size_t i = 0; i < length; i++) {
      EXPECT_EQ(utf16_expected[i], string->array_->GetChar(i));
    }
    EXPECT_EQ(hash_expected, string->hash_code_);
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

static bool StringNotEquals(const String* a, const char* b) {
  return !String::Equals(a, b);
}

TEST_F(ObjectTest, StringEquals) {
  String* string = class_linker_->AllocStringFromModifiedUtf8(7, "android");
  EXPECT_PRED2(String::Equals, string, "android");
  EXPECT_PRED2(StringNotEquals, string, "Android");
  EXPECT_PRED2(StringNotEquals, string, "ANDROID");
  EXPECT_PRED2(StringNotEquals, string, "");
  EXPECT_PRED2(StringNotEquals, string, "and");
  EXPECT_PRED2(StringNotEquals, string, "androids");

  String* empty = class_linker_->AllocStringFromModifiedUtf8(0, "");
  EXPECT_PRED2(String::Equals, empty, "");
  EXPECT_PRED2(StringNotEquals, empty, "a");
}

}  // namespace art
