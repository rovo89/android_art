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
#ifndef HPROF_HPROF_H_
#define HPROF_HPROF_H_

#include <stdio.h>

#include <map>
#include <set>

#include "file.h"
#include "globals.h"
#include "object.h"
#include "thread_list.h"

namespace art {

namespace hprof {

#define HPROF_ID_SIZE (sizeof (uint32_t))

#define UNIQUE_ERROR() \
    -((((uintptr_t)__func__) << 16 | __LINE__) & (0x7fffffff))

#define HPROF_TIME 0
#define HPROF_NULL_STACK_TRACE   0
#define HPROF_NULL_THREAD        0

#define U2_TO_BUF_BE(buf, offset, value) \
    do { \
        unsigned char* buf_ = (unsigned char*)(buf); \
        int offset_ = (int)(offset); \
        uint16_t value_ = (uint16_t)(value); \
        buf_[offset_ + 0] = (unsigned char)(value_ >>  8); \
        buf_[offset_ + 1] = (unsigned char)(value_      ); \
    } while (0)

#define U4_TO_BUF_BE(buf, offset, value) \
    do { \
        unsigned char* buf_ = (unsigned char*)(buf); \
        int offset_ = (int)(offset); \
        uint32_t value_ = (uint32_t)(value); \
        buf_[offset_ + 0] = (unsigned char)(value_ >> 24); \
        buf_[offset_ + 1] = (unsigned char)(value_ >> 16); \
        buf_[offset_ + 2] = (unsigned char)(value_ >>  8); \
        buf_[offset_ + 3] = (unsigned char)(value_      ); \
    } while (0)

#define U8_TO_BUF_BE(buf, offset, value) \
    do { \
        unsigned char* buf_ = (unsigned char*)(buf); \
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

typedef uint32_t HprofId;
typedef HprofId HprofStringId;
typedef HprofId HprofObjectId;
typedef HprofId HprofClassObjectId;
typedef std::set<Class*> ClassSet;
typedef std::set<Class*>::iterator ClassSetIterator;
typedef std::map<std::string, size_t> StringMap;
typedef std::map<std::string, size_t>::iterator StringMapIterator;

enum HprofBasicType {
  hprof_basic_object = 2,
  hprof_basic_boolean = 4,
  hprof_basic_char = 5,
  hprof_basic_float = 6,
  hprof_basic_double = 7,
  hprof_basic_byte = 8,
  hprof_basic_short = 9,
  hprof_basic_int = 10,
  hprof_basic_long = 11,
};

enum HprofTag {
  HPROF_TAG_STRING = 0x01,
  HPROF_TAG_LOAD_CLASS = 0x02,
  HPROF_TAG_UNLOAD_CLASS = 0x03,
  HPROF_TAG_STACK_FRAME = 0x04,
  HPROF_TAG_STACK_TRACE = 0x05,
  HPROF_TAG_ALLOC_SITES = 0x06,
  HPROF_TAG_HEAP_SUMMARY = 0x07,
  HPROF_TAG_START_THREAD = 0x0A,
  HPROF_TAG_END_THREAD = 0x0B,
  HPROF_TAG_HEAP_DUMP = 0x0C,
  HPROF_TAG_HEAP_DUMP_SEGMENT = 0x1C,
  HPROF_TAG_HEAP_DUMP_END = 0x2C,
  HPROF_TAG_CPU_SAMPLES = 0x0D,
  HPROF_TAG_CONTROL_SETTINGS = 0x0E,
};

// Values for the first byte of HEAP_DUMP and HEAP_DUMP_SEGMENT records:
enum HprofHeapTag {
  /* standard */
  HPROF_ROOT_UNKNOWN = 0xFF,
  HPROF_ROOT_JNI_GLOBAL = 0x01,
  HPROF_ROOT_JNI_LOCAL = 0x02,
  HPROF_ROOT_JAVA_FRAME = 0x03,
  HPROF_ROOT_NATIVE_STACK = 0x04,
  HPROF_ROOT_STICKY_CLASS = 0x05,
  HPROF_ROOT_THREAD_BLOCK = 0x06,
  HPROF_ROOT_MONITOR_USED = 0x07,
  HPROF_ROOT_THREAD_OBJECT = 0x08,
  HPROF_CLASS_DUMP = 0x20,
  HPROF_INSTANCE_DUMP = 0x21,
  HPROF_OBJECT_ARRAY_DUMP = 0x22,
  HPROF_PRIMITIVE_ARRAY_DUMP = 0x23,

  /* Android */
  HPROF_HEAP_DUMP_INFO = 0xfe,
  HPROF_ROOT_INTERNED_STRING = 0x89,
  HPROF_ROOT_FINALIZING = 0x8a,  /* obsolete */
  HPROF_ROOT_DEBUGGER = 0x8b,
  HPROF_ROOT_REFERENCE_CLEANUP = 0x8c,  /* obsolete */
  HPROF_ROOT_VM_INTERNAL = 0x8d,
  HPROF_ROOT_JNI_MONITOR = 0x8e,
  HPROF_UNREACHABLE = 0x90,  /* obsolete */
  HPROF_PRIMITIVE_ARRAY_NODATA_DUMP = 0xc3,
};

// Represents a top-level hprof record, whose serialized format is:
// U1  TAG: denoting the type of the record
// U4  TIME: number of microseconds since the time stamp in the header
// U4  LENGTH: number of bytes that follow this uint32_t field and belong to this record
// U1* BODY: as many bytes as specified in the above uint32_t field
class HprofRecord {
 public:
  int Flush(FILE *fp);
  int AddU1(uint8_t value);
  int AddU2(uint16_t value);
  int AddU4(uint32_t value);
  int AddU8(uint64_t value);
  int AddId(HprofObjectId value);
  int AddU1List(const uint8_t *values, size_t numValues);
  int AddU2List(const uint16_t *values, size_t numValues);
  int AddU4List(const uint32_t *values, size_t numValues);
  int AddU8List(const uint64_t *values, size_t numValues);
  int AddIdList(const HprofObjectId *values, size_t numValues);
  int AddUtf8String(const char* str);

  unsigned char* body_;
  uint32_t time_;
  uint32_t length_;
  size_t alloc_length_;
  uint8_t tag_;
  bool dirty_;

 private:
  int GuaranteeRecordAppend(size_t nmore);
};

enum HprofHeapId {
  HPROF_HEAP_DEFAULT = 0,
  HPROF_HEAP_ZYGOTE = 'Z',
  HPROF_HEAP_APP = 'A'
};

class Hprof {
 public:
  Hprof(const char* outputFileName, int fd, bool writeHeader, bool directToDdms);
  ~Hprof();

  void VisitRoot(const Object* obj);
  int DumpHeapObject(const Object *obj);
  bool Finish();

 private:
  int DumpClasses();
  int DumpStrings();
  int StartNewRecord(uint8_t tag, uint32_t time);
  int FlushCurrentRecord();
  int MarkRootObject(const Object *obj, jobject jniObj);
  HprofClassObjectId LookupClassId(Class* clazz);
  HprofStringId LookupStringId(String* string);
  HprofStringId LookupStringId(const char* string);
  HprofStringId LookupStringId(std::string string);
  HprofStringId LookupClassNameId(Class* clazz);
  static HprofBasicType SignatureToBasicTypeAndSize(const char* sig, size_t* sizeOut);
  static HprofBasicType PrimitiveToBasicTypeAndSize(Primitive::Type prim, size_t* sizeOut);
  static int StackTraceSerialNumber(const void *obj);

  // current_record_ *must* be first so that we can cast from a context to a record.
  HprofRecord current_record_;

  uint32_t gc_thread_serial_number_;
  uint8_t gc_scan_state_;
  HprofHeapId current_heap_; // which heap we're currently emitting
  size_t objects_in_segment_;

  // If direct_to_ddms_ is set, "file_name_" and "fd" will be ignored.
  // Otherwise, "file_name_" must be valid, though if "fd" >= 0 it will
  // only be used for debug messages.
  bool direct_to_ddms_;
  std::string file_name_;
  char* file_data_ptr_;   // for open_memstream
  size_t file_data_size_; // for open_memstream
  FILE *mem_fp_;
  int fd_;

  ClassSet classes_;
  size_t next_string_id_;
  StringMap strings_;
};

int DumpHeap(const char* fileName, int fd, bool directToDdms);

}  // namespace hprof

}  // namespace art

#endif  // HPROF_HPROF_H_
