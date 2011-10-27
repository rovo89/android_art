/*
 * Copyright (C) 2008 The Android Open Source Project
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

#ifndef ART_JDWP_BITS_H_
#define ART_JDWP_BITS_H_

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace art {

namespace JDWP {

static inline uint32_t Get4BE(unsigned char const* pSrc) {
  return (pSrc[0] << 24) | (pSrc[1] << 16) | (pSrc[2] << 8) | pSrc[3];
}

static inline uint8_t Read1(unsigned const char** ppSrc) {
  return *(*ppSrc)++;
}

static inline uint16_t Read2BE(unsigned char const** ppSrc) {
  const unsigned char* pSrc = *ppSrc;
  *ppSrc = pSrc + 2;
  return pSrc[0] << 8 | pSrc[1];
}

static inline uint32_t Read4BE(unsigned char const** ppSrc) {
  const unsigned char* pSrc = *ppSrc;
  uint32_t result = pSrc[0] << 24;
  result |= pSrc[1] << 16;
  result |= pSrc[2] << 8;
  result |= pSrc[3];
  *ppSrc = pSrc + 4;
  return result;
}

static inline uint64_t Read8BE(unsigned char const** ppSrc) {
  const unsigned char* pSrc = *ppSrc;
  uint32_t high = pSrc[0];
  high = (high << 8) | pSrc[1];
  high = (high << 8) | pSrc[2];
  high = (high << 8) | pSrc[3];
  uint32_t low = pSrc[4];
  low = (low << 8) | pSrc[5];
  low = (low << 8) | pSrc[6];
  low = (low << 8) | pSrc[7];
  *ppSrc = pSrc + 8;
  return ((uint64_t) high << 32) | (uint64_t) low;
}

/*
 * Read a UTF-8 string into newly-allocated storage, and null-terminate it.
 *
 * Returns the string and its length.  (The latter is probably unnecessary
 * for the way we're using UTF8.)
 */
static inline char* ReadNewUtf8String(unsigned char const** ppSrc, size_t* pLength) {
  uint32_t length = Read4BE(ppSrc);
  char* buf = (char*) malloc(length+1);
  memcpy(buf, *ppSrc, length);
  buf[length] = '\0';
  (*ppSrc) += length;
  *pLength = length;
  return buf;
}

static inline void Set1(uint8_t* buf, uint8_t val) {
  *buf = (uint8_t)(val);
}

static inline void Set2BE(uint8_t* buf, uint16_t val) {
  *buf++ = (uint8_t)(val >> 8);
  *buf = (uint8_t)(val);
}

static inline void Set4BE(uint8_t* buf, uint32_t val) {
  *buf++ = (uint8_t)(val >> 24);
  *buf++ = (uint8_t)(val >> 16);
  *buf++ = (uint8_t)(val >> 8);
  *buf = (uint8_t)(val);
}

static inline void Set8BE(uint8_t* buf, uint64_t val) {
  *buf++ = (uint8_t)(val >> 56);
  *buf++ = (uint8_t)(val >> 48);
  *buf++ = (uint8_t)(val >> 40);
  *buf++ = (uint8_t)(val >> 32);
  *buf++ = (uint8_t)(val >> 24);
  *buf++ = (uint8_t)(val >> 16);
  *buf++ = (uint8_t)(val >> 8);
  *buf = (uint8_t)(val);
}

/*
 * Stuff a UTF-8 string into the buffer.
 */
static inline void SetUtf8String(uint8_t* buf, const uint8_t* str) {
  uint32_t strLen = strlen((const char*)str);
  Set4BE(buf, strLen);
  memcpy(buf + sizeof(uint32_t), str, strLen);
}

}  // namespace JDWP

}  // namespace art

#endif  // ART_JDWP_BITS_H_
