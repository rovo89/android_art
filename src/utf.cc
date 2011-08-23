// Copyright 2011 Google Inc. All Rights Reserved.

#include "utf.h"

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
    // three-byte encoding
    utf8++;
  }
  return len;
}

void ConvertModifiedUtf8ToUtf16(uint16_t* utf16_data_out, const char* utf8_data_in) {
  while (*utf8_data_in != '\0') {
    *utf16_data_out++ = GetUtf16FromUtf8(&utf8_data_in);
  }
}

int32_t ComputeUtf16Hash(const uint16_t* chars, size_t char_count) {
  int32_t hash = 0;
  while (char_count--) {
    hash = hash * 31 + *chars++;
  }
  return hash;
}

uint16_t GetUtf16FromUtf8(const char** utf8_data_in) {
  uint8_t one = *(*utf8_data_in)++;
  if ((one & 0x80) == 0) {
    // one-byte encoding
    return one;
  }
  // two- or three-byte encoding
  uint8_t two = *(*utf8_data_in)++;
  if ((one & 0x20) == 0) {
    // two-byte encoding
    return ((one & 0x1f) << 6) | (two & 0x3f);
  }
  // three-byte encoding
  uint8_t three = *(*utf8_data_in)++;
  return ((one & 0x0f) << 12) | ((two & 0x3f) << 6) | (three & 0x3f);
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
