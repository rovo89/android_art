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
#include <cutils/open_memstream.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include "hprof.h"
#include "stringprintf.h"
#include "logging.h"

namespace art {

namespace hprof {

#define HPROF_MAGIC_STRING  "JAVA PROFILE 1.0.3"

#define U2_TO_BUF_BE(buf, offset, value) \
    do { \
        unsigned char *buf_ = (unsigned char *)(buf); \
        int offset_ = (int)(offset); \
        uint16_t value_ = (uint16_t)(value); \
        buf_[offset_ + 0] = (unsigned char)(value_ >>  8); \
        buf_[offset_ + 1] = (unsigned char)(value_      ); \
    } while (0)

#define U4_TO_BUF_BE(buf, offset, value) \
    do { \
        unsigned char *buf_ = (unsigned char *)(buf); \
        int offset_ = (int)(offset); \
        uint32_t value_ = (uint32_t)(value); \
        buf_[offset_ + 0] = (unsigned char)(value_ >> 24); \
        buf_[offset_ + 1] = (unsigned char)(value_ >> 16); \
        buf_[offset_ + 2] = (unsigned char)(value_ >>  8); \
        buf_[offset_ + 3] = (unsigned char)(value_      ); \
    } while (0)

#define U8_TO_BUF_BE(buf, offset, value) \
    do { \
        unsigned char *buf_ = (unsigned char *)(buf); \
        int offset_ = (int)(offset); \
        uint64_t value_ = (uint64_t)(value); \
        buf_[offset_ + 0] = (unsigned char)(value_ >> 56); \
        buf_[offset_ + 1] = (unsigned char)(value_ >> 48); \
        buf_[offset_ + 2] = (unsigned char)(value_ >> 40); \
        buf_[offset_ + 3] = (unsigned char)(value_ >> 32); \
        buf_[offset_ + 4] = (unsigned char)(value_ >> 24); \
        buf_[offset_ + 5] = (unsigned char)(value_ >> 16); \
        buf_[offset_ + 6] = (unsigned char)(value_ >>  8); \
        buf_[offset_ + 7] = (unsigned char)(value_      ); \
    } while (0)

/*
 * Initialize an Hprof.
 */
Hprof::Hprof(const char *outputFileName, int fd, bool writeHeader, bool directToDdms)
    : current_record_(),
      gc_thread_serial_number_(0),
      gc_scan_state_(0),
      current_heap_(HPROF_HEAP_DEFAULT),
      objects_in_segment_(0),
      direct_to_ddms_(0),
      file_name_(NULL),
      file_data_ptr_(NULL),
      file_data_size_(0),
      mem_fp_(NULL),
      fd_(0),
      classes_lock_("hprof classes"),
      next_string_id_(0x400000),
      strings_lock_("hprof strings") {
  // Have to do this here, because it must happen after we
  // memset the struct (want to treat file_data_ptr_/file_data_size_
  // as read-only while the file is open).
  FILE *fp = open_memstream(&file_data_ptr_, &file_data_size_);
  if (fp == NULL) {
    // not expected
    LOG(ERROR) << StringPrintf("hprof: open_memstream failed: %s", strerror(errno));
    CHECK(false);
  }

  direct_to_ddms_ = directToDdms;
  file_name_ = strdup(outputFileName);
  mem_fp_ = fp;
  fd_ = fd;

  current_record_.alloc_length_ = 128;
  current_record_.body_ = (unsigned char *)malloc(current_record_.alloc_length_);
  // TODO check for/return an error

  if (writeHeader) {
    char magic[] = HPROF_MAGIC_STRING;
    unsigned char buf[4];

    // Write the file header.
    // U1: NUL-terminated magic string.
    fwrite(magic, 1, sizeof(magic), fp);

    // U4: size of identifiers.  We're using addresses as IDs, so make sure a pointer fits.
    U4_TO_BUF_BE(buf, 0, sizeof(void *));
    fwrite(buf, 1, sizeof(uint32_t), fp);

    // The current time, in milliseconds since 0:00 GMT, 1/1/70.
    struct timeval now;
    uint64_t nowMs;
    if (gettimeofday(&now, NULL) < 0) {
      nowMs = 0;
    } else {
      nowMs = (uint64_t)now.tv_sec * 1000 + now.tv_usec / 1000;
    }

    // U4: high word of the 64-bit time.
    U4_TO_BUF_BE(buf, 0, (uint32_t)(nowMs >> 32));
    fwrite(buf, 1, sizeof(uint32_t), fp);

    // U4: low word of the 64-bit time.
    U4_TO_BUF_BE(buf, 0, (uint32_t)(nowMs & 0xffffffffULL));
    fwrite(buf, 1, sizeof(uint32_t), fp); //xxx fix the time
  }
}

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

int Hprof::FlushCurrentRecord() {
  return current_record_.Flush(mem_fp_);
}

int Hprof::StartNewRecord(uint8_t tag, uint32_t time) {
  HprofRecord *rec = &current_record_;

  int err = rec->Flush(mem_fp_);
  if (err != 0) {
    return err;
  } else if (rec->dirty_) {
    return UNIQUE_ERROR();
  }

  rec->dirty_ = true;
  rec->tag_ = tag;
  rec->time_ = time;
  rec->length_ = 0;
  return 0;
}

static inline int guaranteeRecordAppend(HprofRecord *rec, size_t nmore) {
  size_t minSize = rec->length_ + nmore;
  if (minSize > rec->alloc_length_) {
    size_t newAllocLen = rec->alloc_length_ * 2;
    if (newAllocLen < minSize) {
      newAllocLen = rec->alloc_length_ + nmore + nmore/2;
    }
    unsigned char *newBody = (unsigned char *)realloc(rec->body_, newAllocLen);
    if (newBody != NULL) {
      rec->body_ = newBody;
      rec->alloc_length_ = newAllocLen;
    } else {
      // TODO: set an error flag so future ops will fail
      return UNIQUE_ERROR();
    }
  }

  CHECK_LE(rec->length_ + nmore, rec->alloc_length_);
  return 0;
}

int HprofRecord::AddU1List(const uint8_t *values, size_t numValues) {
  int err = guaranteeRecordAppend(this, numValues);
  if (err != 0) {
    return err;
  }

  memcpy(body_ + length_, values, numValues);
  length_ += numValues;

  return 0;
}

int HprofRecord::AddU1(uint8_t value) {
  int err = guaranteeRecordAppend(this, 1);
  if (err != 0) {
    return err;
  }

  body_[length_++] = value;
  return 0;
}

int HprofRecord::AddUtf8String(const char *str) {
  // The terminating NUL character is NOT written.
  return AddU1List((const uint8_t *)str, strlen(str));
}

int HprofRecord::AddU2List(const uint16_t *values, size_t numValues) {
  int err = guaranteeRecordAppend(this, numValues * 2);
  if (err != 0) {
    return err;
  }

  unsigned char *insert = body_ + length_;
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
  int err = guaranteeRecordAppend(this, numValues * 4);
  if (err != 0) {
    return err;
  }

  unsigned char *insert = body_ + length_;
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
  int err = guaranteeRecordAppend(this, numValues * 8);
  if (err != 0) {
    return err;
  }

  unsigned char *insert = body_ + length_;
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
