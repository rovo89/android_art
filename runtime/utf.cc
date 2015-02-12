/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "base/logging.h"
#include "mirror/array.h"
#include "mirror/object-inl.h"
#include "utf-inl.h"

namespace art {

size_t CountModifiedUtf8Chars(const char* utf8) {
  size_t len = 0;
  int ic;
  while ((ic = *utf8++) != '\0') {
    len++;
    if ((ic & 0x80) == 0) {
      // one-byte encoding
      continue;
    }
    // two- or three-byte encoding
    utf8++;
    if ((ic & 0x20) == 0) {
      // two-byte encoding
      continue;
    }
    utf8++;
    if ((ic & 0x10) == 0) {
      // three-byte encoding
      continue;
    }

    // four-byte encoding: needs to be converted into a surrogate
    // pair.
    utf8++;
    len++;
  }
  return len;
}

void ConvertModifiedUtf8ToUtf16(uint16_t* utf16_data_out, const char* utf8_data_in) {
  while (*utf8_data_in != '\0') {
    const uint32_t ch = GetUtf16FromUtf8(&utf8_data_in);
    const uint16_t leading = GetLeadingUtf16Char(ch);
    const uint16_t trailing = GetTrailingUtf16Char(ch);

    *utf16_data_out++ = leading;
    if (trailing != 0) {
      *utf16_data_out++ = trailing;
    }
  }
}

void ConvertUtf16ToModifiedUtf8(char* utf8_out, const uint16_t* utf16_in, size_t char_count) {
  while (char_count--) {
    uint16_t ch = *utf16_in++;
    if (ch > 0 && ch <= 0x7f) {
      *utf8_out++ = ch;
    } else {
      if (ch > 0x07ff) {
        *utf8_out++ = (ch >> 12) | 0xe0;
        *utf8_out++ = ((ch >> 6) & 0x3f) | 0x80;
        *utf8_out++ = (ch & 0x3f) | 0x80;
      } else /*(ch > 0x7f || ch == 0)*/ {
        *utf8_out++ = (ch >> 6) | 0xc0;
        *utf8_out++ = (ch & 0x3f) | 0x80;
      }
    }
  }
}

int32_t ComputeUtf16Hash(mirror::CharArray* chars, int32_t offset,
                         size_t char_count) {
  uint32_t hash = 0;
  for (size_t i = 0; i < char_count; i++) {
    hash = hash * 31 + chars->Get(offset + i);
  }
  return static_cast<int32_t>(hash);
}

int32_t ComputeUtf16Hash(const uint16_t* chars, size_t char_count) {
  uint32_t hash = 0;
  while (char_count--) {
    hash = hash * 31 + *chars++;
  }
  return static_cast<int32_t>(hash);
}

size_t ComputeModifiedUtf8Hash(const char* chars) {
  size_t hash = 0;
  while (*chars != '\0') {
    hash = hash * 31 + *chars++;
  }
  return static_cast<int32_t>(hash);
}

int CompareModifiedUtf8ToUtf16AsCodePointValues(const char* utf8, const uint16_t* utf16,
                                                size_t utf16_length) {
  for (;;) {
    if (*utf8 == '\0') {
      return (utf16_length == 0) ? 0 : -1;
    } else if (utf16_length == 0) {
      return 1;
    }

    const uint32_t pair = GetUtf16FromUtf8(&utf8);

    // First compare the leading utf16 char.
    const uint16_t lhs = GetLeadingUtf16Char(pair);
    const uint16_t rhs = *utf16++;
    --utf16_length;
    if (lhs != rhs) {
      return lhs > rhs ? 1 : -1;
    }

    // Then compare the trailing utf16 char. First check if there
    // are any characters left to consume.
    const uint16_t lhs2 = GetTrailingUtf16Char(pair);
    if (lhs2 != 0) {
      if (utf16_length == 0) {
        return 1;
      }

      const uint16_t rhs2 = *utf16++;
      --utf16_length;
      if (lhs2 != rhs2) {
        return lhs2 > rhs2 ? 1 : -1;
      }
    }
  }
}

size_t CountUtf8Bytes(const uint16_t* chars, size_t char_count) {
  size_t result = 0;
  while (char_count--) {
    uint16_t ch = *chars++;
    if (ch > 0 && ch <= 0x7f) {
      ++result;
    } else {
      if (ch > 0x7ff) {
        result += 3;
      } else {
        result += 2;
      }
    }
  }
  return result;
}

}  // namespace art
