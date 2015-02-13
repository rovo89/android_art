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

#include <vector>

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

static void AssertConversion(const std::vector<uint16_t> input,
                             const std::vector<uint8_t> expected) {
  ASSERT_EQ(expected.size(), CountUtf8Bytes(&input[0], input.size()));

  std::vector<uint8_t> output(expected.size());
  ConvertUtf16ToModifiedUtf8(reinterpret_cast<char*>(&output[0]), &input[0], input.size());
  EXPECT_EQ(expected, output);
}

TEST_F(UtfTest, CountAndConvertUtf8Bytes) {
  // Surrogate pairs will be converted into 4 byte sequences.
  AssertConversion({ 0xd801, 0xdc00 }, { 0xf0, 0x90, 0x90, 0x80 });

  // Three byte encodings that are below & above the leading surrogate
  // range respectively.
  AssertConversion({ 0xdef0 }, { 0xed, 0xbb, 0xb0 });
  AssertConversion({ 0xdcff }, { 0xed, 0xb3, 0xbf });
  // Two byte encoding.
  AssertConversion({ 0x0101 }, { 0xc4, 0x81 });

  // Two byte special case : 0 must use an overlong encoding.
  AssertConversion({ 0x0101, 0x0000 }, { 0xc4, 0x81, 0xc0, 0x80 });

  // One byte encoding.
  AssertConversion({ 'h', 'e', 'l', 'l', 'o' }, { 0x68, 0x65, 0x6c, 0x6c, 0x6f });

  AssertConversion({
      0xd802, 0xdc02,  // Surrogate pair
      0xdef0, 0xdcff,  // Three byte encodings
      0x0101, 0x0000,  // Two byte encodings
      'p'   , 'p'      // One byte encoding
    }, {
      0xf0, 0x90, 0xa0, 0x82,
      0xed, 0xbb, 0xb0, 0xed, 0xb3, 0xbf,
      0xc4, 0x81, 0xc0, 0x80,
      0x70, 0x70
    });
}

TEST_F(UtfTest, CountAndConvertUtf8Bytes_UnpairedSurrogate) {
  // Unpaired trailing surrogate at the end of input.
  AssertConversion({ 'h', 'e', 0xd801 }, { 'h', 'e', 0xed, 0xa0, 0x81 });
  // Unpaired (or incorrectly paired) surrogates in the middle of the input.
  AssertConversion({ 'h', 0xd801, 'e' }, { 'h', 0xed, 0xa0, 0x81, 'e' });
  AssertConversion({ 'h', 0xd801, 0xd801, 'e' }, { 'h', 0xed, 0xa0, 0x81, 0xed, 0xa0, 0x81, 'e' });
  AssertConversion({ 'h', 0xdc00, 0xdc00, 'e' }, { 'h', 0xed, 0xb0, 0x80, 0xed, 0xb0, 0x80, 'e' });
}

}  // namespace art
