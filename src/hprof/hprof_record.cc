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
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include "hprof.h"
#include "stringprintf.h"
#include "logging.h"

namespace art {

namespace hprof {

int HprofRecord::Flush(FILE *fp) {
  if (dirty_) {
    unsigned char headBuf[sizeof (uint8_t) + 2 * sizeof (uint32_t)];

    headBuf[0] = tag_;
    U4_TO_BUF_BE(headBuf, 1, time_);
    U4_TO_BUF_BE(headBuf, 5, length_);

    int nb = fwrite(headBuf, 1, sizeof(headBuf), fp);
    if (nb != sizeof(headBuf)) {
      return UNIQUE_ERROR();
    }
    nb = fwrite(body_, 1, length_, fp);
    if (nb != (int)length_) {
      return UNIQUE_ERROR();
    }

    dirty_ = false;
  }
  // TODO if we used less than half (or whatever) of allocLen, shrink the buffer.
  return 0;
}

int HprofRecord::GuaranteeRecordAppend(size_t nmore) {
  size_t minSize = length_ + nmore;
  if (minSize > alloc_length_) {
    size_t newAllocLen = alloc_length_ * 2;
    if (newAllocLen < minSize) {
      newAllocLen = alloc_length_ + nmore + nmore/2;
    }
    unsigned char* newBody = (unsigned char*)realloc(body_, newAllocLen);
    if (newBody != NULL) {
      body_ = newBody;
      alloc_length_ = newAllocLen;
    } else {
      // TODO: set an error flag so future ops will fail
      return UNIQUE_ERROR();
    }
  }

  CHECK_LE(length_ + nmore, alloc_length_);
  return 0;
}

int HprofRecord::AddU1List(const uint8_t *values, size_t numValues) {
  int err = GuaranteeRecordAppend(numValues);
  if (err != 0) {
    return err;
  }

  memcpy(body_ + length_, values, numValues);
  length_ += numValues;
  return 0;
}

int HprofRecord::AddU1(uint8_t value) {
  int err = GuaranteeRecordAppend(1);
  if (err != 0) {
    return err;
  }

  body_[length_++] = value;
  return 0;
}

int HprofRecord::AddUtf8String(const char* str) {
  // The terminating NUL character is NOT written.
  return AddU1List((const uint8_t *)str, strlen(str));
}

int HprofRecord::AddU2List(const uint16_t *values, size_t numValues) {
  int err = GuaranteeRecordAppend(numValues * 2);
  if (err != 0) {
    return err;
  }

  unsigned char* insert = body_ + length_;
  for (size_t i = 0; i < numValues; i++) {
    U2_TO_BUF_BE(insert, 0, *values++);
    insert += sizeof(*values);
  }
  length_ += numValues * 2;
  return 0;
}

int HprofRecord::AddU2(uint16_t value) {
  return AddU2List(&value, 1);
}

int HprofRecord::AddIdList(const HprofObjectId *values, size_t numValues) {
  return AddU4List((const uint32_t*) values, numValues);
}

int HprofRecord::AddU4List(const uint32_t *values, size_t numValues) {
  int err = GuaranteeRecordAppend(numValues * 4);
  if (err != 0) {
    return err;
  }

  unsigned char* insert = body_ + length_;
  for (size_t i = 0; i < numValues; i++) {
    U4_TO_BUF_BE(insert, 0, *values++);
    insert += sizeof(*values);
  }
  length_ += numValues * 4;
  return 0;
}

int HprofRecord::AddU4(uint32_t value) {
  return AddU4List(&value, 1);
}

int HprofRecord::AddId(HprofObjectId value) {
  return AddU4((uint32_t) value);
}

int HprofRecord::AddU8List(const uint64_t *values, size_t numValues) {
  int err = GuaranteeRecordAppend(numValues * 8);
  if (err != 0) {
    return err;
  }

  unsigned char* insert = body_ + length_;
  for (size_t i = 0; i < numValues; i++) {
    U8_TO_BUF_BE(insert, 0, *values++);
    insert += sizeof(*values);
  }
  length_ += numValues * 8;
  return 0;
}

int HprofRecord::AddU8(uint64_t value) {
  return AddU8List(&value, 1);
}

}  // namespace hprof

}  // namespace art
