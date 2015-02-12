/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "utf.h"

#include "common_runtime_test.h"
#include "utf-inl.h"

namespace art {

class UtfTest : public CommonRuntimeTest {};

TEST_F(UtfTest, GetLeadingUtf16Char) {
  EXPECT_EQ(0xffff, GetLeadingUtf16Char(0xeeeeffff));
}

TEST_F(UtfTest, GetTrailingUtf16Char) {
  EXPECT_EQ(0xffff, GetTrailingUtf16Char(0xffffeeee));
  EXPECT_EQ(0, GetTrailingUtf16Char(0x0000aaaa));
}

#define EXPECT_ARRAY_POSITION(expected, end, start) \
  EXPECT_EQ(static_cast<uintptr_t>(expected), \
            reinterpret_cast<uintptr_t>(end) - reinterpret_cast<uintptr_t>(start));

// A test string containing one, two, three and four byte UTF-8 sequences.
static const uint8_t kAllSequences[] = {
    0x24,
    0xc2, 0xa2,
    0xe2, 0x82, 0xac,
    0xf0, 0x9f, 0x8f, 0xa0,
    0x00
};

// A test string that contains a UTF-8 encoding of a surrogate pair
// (code point = U+10400)
static const uint8_t kSurrogateEncoding[] = {
    0xed, 0xa0, 0x81,
    0xed, 0xb0, 0x80,
    0x00
};

TEST_F(UtfTest, GetUtf16FromUtf8) {
  const char* const start = reinterpret_cast<const char*>(kAllSequences);
  const char* ptr = start;
  uint32_t pair = 0;

  // Single byte sequence.
  pair = GetUtf16FromUtf8(&ptr);
  EXPECT_EQ(0x24, GetLeadingUtf16Char(pair));
  EXPECT_EQ(0, GetTrailingUtf16Char(pair));
  EXPECT_ARRAY_POSITION(1, ptr, start);

  // Two byte sequence
  pair = GetUtf16FromUtf8(&ptr);
  EXPECT_EQ(0xa2, GetLeadingUtf16Char(pair));
  EXPECT_EQ(0, GetTrailingUtf16Char(pair));
  EXPECT_ARRAY_POSITION(3, ptr, start);

  // Three byte sequence
  pair = GetUtf16FromUtf8(&ptr);
  EXPECT_EQ(0x20ac, GetLeadingUtf16Char(pair));
  EXPECT_EQ(0, GetTrailingUtf16Char(pair));
  EXPECT_ARRAY_POSITION(6, ptr, start);

  // Four byte sequence
  pair = GetUtf16FromUtf8(&ptr);
  EXPECT_EQ(0xd83c, GetLeadingUtf16Char(pair));
  EXPECT_EQ(0xdfe0, GetTrailingUtf16Char(pair));
  EXPECT_ARRAY_POSITION(10, ptr, start);

  // Null terminator
  pair = GetUtf16FromUtf8(&ptr);
  EXPECT_EQ(0, GetLeadingUtf16Char(pair));
  EXPECT_EQ(0, GetTrailingUtf16Char(pair));
  EXPECT_ARRAY_POSITION(11, ptr, start);
}

TEST_F(UtfTest, GetUtf16FromUtf8_SurrogatesPassThrough) {
  const char* const start = reinterpret_cast<const char *>(kSurrogateEncoding);
  const char* ptr = start;
  uint32_t pair = 0;

  pair = GetUtf16FromUtf8(&ptr);
  EXPECT_EQ(0xd801, GetLeadingUtf16Char(pair));
  EXPECT_EQ(0, GetTrailingUtf16Char(pair));
  EXPECT_ARRAY_POSITION(3, ptr, start);

  pair = GetUtf16FromUtf8(&ptr);
  EXPECT_EQ(0xdc00, GetLeadingUtf16Char(pair));
  EXPECT_EQ(0, GetTrailingUtf16Char(pair));
  EXPECT_ARRAY_POSITION(6, ptr, start);
}

TEST_F(UtfTest, CountModifiedUtf8Chars) {
  EXPECT_EQ(5u, CountModifiedUtf8Chars(reinterpret_cast<const char *>(kAllSequences)));
  EXPECT_EQ(2u, CountModifiedUtf8Chars(reinterpret_cast<const char *>(kSurrogateEncoding)));
}

}  // namespace art
