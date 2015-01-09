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

/*
 * Preparation and completion of hprof data generation.  The output is
 * written into two files and then combined.  This is necessary because
 * we generate some of the data (strings and classes) while we dump the
 * heap, and some analysis tools require that the class and string data
 * appear first.
 */

#include "hprof.h"

#include <cutils/open_memstream.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <time.h>
#include <unistd.h>

#include <set>

#include "base/logging.h"
#include "base/stringprintf.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "common_throws.h"
#include "debugger.h"
#include "dex_file-inl.h"
#include "gc/accounting/heap_bitmap.h"
#include "gc/heap.h"
#include "gc/space/space.h"
#include "globals.h"
#include "jdwp/jdwp.h"
#include "jdwp/jdwp_priv.h"
#include "mirror/art_field-inl.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "os.h"
#include "safe_map.h"
#include "scoped_thread_state_change.h"
#include "thread_list.h"

namespace art {

namespace hprof {

static constexpr bool kDirectStream = true;

#define HPROF_TIME 0
#define HPROF_NULL_STACK_TRACE   0
#define HPROF_NULL_THREAD        0

#define U2_TO_BUF_BE(buf, offset, value) \
    do { \
      unsigned char* buf_ = (unsigned char*)(buf); \
      int offset_ = static_cast<int>(offset); \
      uint16_t value_ = (uint16_t)(value); \
      buf_[offset_ + 0] = (unsigned char)(value_ >>  8); \
      buf_[offset_ + 1] = (unsigned char)(value_      ); \
    } while (0)

#define U4_TO_BUF_BE(buf, offset, value) \
    do { \
      unsigned char* buf_ = (unsigned char*)(buf); \
      int offset_ = static_cast<int>(offset); \
      uint32_t value_ = (uint32_t)(value); \
      buf_[offset_ + 0] = (unsigned char)(value_ >> 24); \
      buf_[offset_ + 1] = (unsigned char)(value_ >> 16); \
      buf_[offset_ + 2] = (unsigned char)(value_ >>  8); \
      buf_[offset_ + 3] = (unsigned char)(value_      ); \
    } while (0)

#define U8_TO_BUF_BE(buf, offset, value) \
    do { \
      unsigned char* buf_ = (unsigned char*)(buf); \
      int offset_ = static_cast<int>(offset); \
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
  // Traditional.
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

  // Android.
  HPROF_HEAP_DUMP_INFO = 0xfe,
  HPROF_ROOT_INTERNED_STRING = 0x89,
  HPROF_ROOT_FINALIZING = 0x8a,  // Obsolete.
  HPROF_ROOT_DEBUGGER = 0x8b,
  HPROF_ROOT_REFERENCE_CLEANUP = 0x8c,  // Obsolete.
  HPROF_ROOT_VM_INTERNAL = 0x8d,
  HPROF_ROOT_JNI_MONITOR = 0x8e,
  HPROF_UNREACHABLE = 0x90,  // Obsolete.
  HPROF_PRIMITIVE_ARRAY_NODATA_DUMP = 0xc3,  // Obsolete.
};

enum HprofHeapId {
  HPROF_HEAP_DEFAULT = 0,
  HPROF_HEAP_ZYGOTE = 'Z',
  HPROF_HEAP_APP = 'A',
  HPROF_HEAP_IMAGE = 'I',
};

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

typedef uint32_t HprofStringId;
typedef uint32_t HprofClassObjectId;

class Hprof;

// Represents a top-level hprof record, whose serialized format is:
// U1  TAG: denoting the type of the record
// U4  TIME: number of microseconds since the time stamp in the header
// U4  LENGTH: number of bytes that follow this uint32_t field and belong to this record
// U1* BODY: as many bytes as specified in the above uint32_t field
class HprofRecord {
 public:
  explicit HprofRecord(Hprof* hprof) : alloc_length_(128), fp_(nullptr), tag_(0), time_(0),
      length_(0), dirty_(false), hprof_(hprof) {
    body_ = reinterpret_cast<unsigned char*>(malloc(alloc_length_));
  }

  ~HprofRecord() {
    free(body_);
  }

  // Returns how many characters were in the buffer (or written).
  size_t StartNewRecord(FILE* fp, uint8_t tag, uint32_t time) WARN_UNUSED {
    const size_t ret = Flush();
    fp_ = fp;
    tag_ = tag;
    time_ = time;
    length_ = 0;
    dirty_ = true;
    return ret;
  }

  // Returns how many characters were in the buffer (or written).
  size_t Flush() WARN_UNUSED;

  void AddU1(uint8_t value);

  void AddU2(uint16_t value) {
    AddU2List(&value, 1);
  }

  void AddU4(uint32_t value) {
    AddU4List(&value, 1);
  }

  void AddU8(uint64_t value) {
    AddU8List(&value, 1);
  }

  void AddObjectId(const mirror::Object* value) {
    AddU4(PointerToLowMemUInt32(value));
  }

  // The ID for the synthetic object generated to account for class static overhead.
  void AddClassStaticsId(const mirror::Class* value) {
    AddU4(1 | PointerToLowMemUInt32(value));
  }

  void AddJniGlobalRefId(jobject value) {
    AddU4(PointerToLowMemUInt32(value));
  }

  void AddClassId(HprofClassObjectId value) {
    AddU4(value);
  }

  void AddStringId(HprofStringId value) {
    AddU4(value);
  }

  void AddU1List(const uint8_t* values, size_t numValues);
  void AddU2List(const uint16_t* values, size_t numValues);
  void AddU4List(const uint32_t* values, size_t numValues);
  void UpdateU4(size_t offset, uint32_t new_value);
  void AddU8List(const uint64_t* values, size_t numValues);

  void AddIdList(mirror::ObjectArray<mirror::Object>* values)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const int32_t length = values->GetLength();
    for (int32_t i = 0; i < length; ++i) {
      AddObjectId(values->GetWithoutChecks(i));
    }
  }

  void AddUtf8String(const char* str) {
    // The terminating NUL character is NOT written.
    AddU1List((const uint8_t*)str, strlen(str));
  }

  size_t Size() const {
    return length_;
  }

 private:
  void GuaranteeRecordAppend(size_t nmore) {
    const size_t min_size = length_ + nmore;
    if (min_size > alloc_length_) {
      const size_t new_alloc_len = std::max(alloc_length_ * 2, min_size);
      body_ = (unsigned char*)realloc(body_, new_alloc_len);
      CHECK(body_ != nullptr);
      alloc_length_ = new_alloc_len;
    }
    CHECK_LE(length_ + nmore, alloc_length_);
  }

  size_t alloc_length_;
  unsigned char* body_;

  FILE* fp_;
  uint8_t tag_;
  uint32_t time_;
  size_t length_;
  bool dirty_;
  Hprof* hprof_;

  DISALLOW_COPY_AND_ASSIGN(HprofRecord);
};

class Hprof {
 public:
  Hprof(const char* output_filename, int fd, bool direct_to_ddms)
      : filename_(output_filename),
        fd_(fd),
        direct_to_ddms_(direct_to_ddms),
        start_ns_(NanoTime()),
        current_record_(this),
        gc_thread_serial_number_(0),
        gc_scan_state_(0),
        current_heap_(HPROF_HEAP_DEFAULT),
        objects_in_segment_(0),
        header_fp_(nullptr),
        header_data_ptr_(nullptr),
        header_data_size_(0),
        body_fp_(nullptr),
        body_data_ptr_(nullptr),
        body_data_size_(0),
        net_state_(nullptr),
        next_string_id_(0x400000) {
    LOG(INFO) << "hprof: heap dump \"" << filename_ << "\" starting...";
  }

  ~Hprof() {
    if (header_fp_ != nullptr) {
      fclose(header_fp_);
    }
    if (body_fp_ != nullptr) {
      fclose(body_fp_);
    }
    free(header_data_ptr_);
    free(body_data_ptr_);
  }

  void ProcessBody() EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    Runtime* runtime = Runtime::Current();
    // Walk the roots and the heap.
    total_body_bytes_ += current_record_.StartNewRecord(body_fp_, HPROF_TAG_HEAP_DUMP_SEGMENT,
                                                        HPROF_TIME);
    runtime->VisitRoots(RootVisitor, this);
    runtime->GetHeap()->VisitObjects(VisitObjectCallback, this);
    total_body_bytes_ += current_record_.StartNewRecord(body_fp_, HPROF_TAG_HEAP_DUMP_END,
                                                        HPROF_TIME);
    total_body_bytes_ += current_record_.Flush();
    if (allow_writing_) {
      fflush(body_fp_);
    }
  }

  void ProcessHeader() EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // Write the header.
    WriteFixedHeader();
    // Write the string and class tables, and any stack traces, to the header.
    // (jhat requires that these appear before any of the data in the body that refers to them.)
    WriteStringTable();
    WriteClassTable();
    WriteStackTraces();
    total_header_bytes_ += current_record_.Flush();
    if (allow_writing_) {
      fflush(header_fp_);
    }
  }

  void ProcessHeapStreaming(size_t data_len, uint32_t chunk_type)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    total_body_bytes_ = 0;
    total_header_bytes_ = 0;
    allow_writing_ = true;
    CHECK(direct_to_ddms_);
    JDWP::JdwpState* state = Dbg::GetJdwpState();
    CHECK(state != nullptr);
    net_state_ = state->netState;
    CHECK(net_state_ != nullptr);
    // Hold the socket lock for the whole tiem since we want this to be atomic.
    MutexLock mu(Thread::Current(), *net_state_->GetSocketLock());
    total_body_bytes_ = 0;
    total_header_bytes_ = 0;
    constexpr size_t kChunkHeaderSize = kJDWPHeaderLen + 8;
    uint8_t chunk_header[kChunkHeaderSize] = { 0 };
    state->SetupChunkHeader(chunk_type, data_len, kChunkHeaderSize, chunk_header);
    Write(chunk_header, kChunkHeaderSize, nullptr);  // Send the header chunk to DDMS.
    ProcessHeader();
    ProcessBody();
    CHECK_EQ(total_body_bytes_ + total_header_bytes_, data_len);
    net_state_ = nullptr;
  }
  void ProcessHeap(bool allow_writing) EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
    allow_writing_ = allow_writing;
    total_body_bytes_ = 0;
    total_header_bytes_ = 0;
    if (allow_writing) {
      header_fp_ = open_memstream(&header_data_ptr_, &header_data_size_);
      CHECK(header_fp_ != nullptr) << "header open_memstream failed";
      body_fp_ = open_memstream(&body_data_ptr_, &body_data_size_);
      CHECK(body_fp_ != nullptr) << "body open_memstream failed";
    }
    ProcessBody();
    ProcessHeader();
  }

  void Dump() EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_)
      LOCKS_EXCLUDED(Locks::heap_bitmap_lock_) {
    {
      ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
      // First pass to measure the size of the dump.
      ProcessHeap(false);
      const size_t header_bytes = total_header_bytes_;
      const size_t body_bytes = total_body_bytes_;
      if (direct_to_ddms_ && kDirectStream) {
        ProcessHeapStreaming(header_bytes + body_bytes, CHUNK_TYPE("HPDS"));
      } else {
        ProcessHeap(true);
        CHECK_EQ(header_data_size_, header_bytes);
        CHECK_EQ(body_data_size_, body_bytes);
      }
      CHECK_EQ(total_header_bytes_, header_bytes);
      CHECK_EQ(total_body_bytes_, body_bytes);
    }

    bool okay = true;
    if (!kDirectStream) {
      if (direct_to_ddms_) {
        // Send the data off to DDMS.
        iovec iov[2];
        iov[0].iov_base = header_data_ptr_;
        iov[0].iov_len = header_data_size_;
        iov[1].iov_base = body_data_ptr_;
        iov[1].iov_len = body_data_size_;
        Dbg::DdmSendChunkV(CHUNK_TYPE("HPDS"), iov, 2);
      } else {
        // Where exactly are we writing to?
        int out_fd;
        if (fd_ >= 0) {
          out_fd = dup(fd_);
          if (out_fd < 0) {
            ThrowRuntimeException("Couldn't dump heap; dup(%d) failed: %s", fd_, strerror(errno));
            return;
          }
        } else {
          out_fd = open(filename_.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0644);
          if (out_fd < 0) {
            ThrowRuntimeException("Couldn't dump heap; open(\"%s\") failed: %s", filename_.c_str(),
                                  strerror(errno));
            return;
          }
        }

        std::unique_ptr<File> file(new File(out_fd, filename_, true));
        okay = file->WriteFully(header_data_ptr_, header_data_size_) &&
               file->WriteFully(body_data_ptr_, body_data_size_);
        if (okay) {
          okay = file->FlushCloseOrErase() == 0;
        } else {
          file->Erase();
        }
        if (!okay) {
          std::string msg(StringPrintf("Couldn't dump heap; writing \"%s\" failed: %s",
                                       filename_.c_str(), strerror(errno)));
          ThrowRuntimeException("%s", msg.c_str());
          LOG(ERROR) << msg;
        }
      }
    }

    // Throw out a log message for the benefit of "runhat".
    if (okay) {
      uint64_t duration = NanoTime() - start_ns_;
      LOG(INFO) << "hprof: heap dump completed ("
          << PrettySize(total_header_bytes_ + total_body_bytes_ + 1023)
          << ") in " << PrettyDuration(duration);
    }
  }

  bool AllowWriting() const {
    return allow_writing_;
  }

  size_t Write(const void* ptr, size_t len, FILE* fp) {
    if (allow_writing_) {
      if (net_state_ != nullptr) {
        CHECK(fp == nullptr);
        std::vector<iovec> iov;
        iov.push_back(iovec());
        iov[0].iov_base = const_cast<void*>(ptr);
        iov[0].iov_len = len;
        net_state_->WriteBufferedPacketLocked(iov);
      } else {
        const size_t n = fwrite(ptr, 1, len, fp);
        CHECK_EQ(n, len);
      }
    }
    return len;
  }

 private:
  static void RootVisitor(mirror::Object** obj, void* arg, uint32_t thread_id, RootType root_type)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(arg != nullptr);
    DCHECK(obj != nullptr);
    DCHECK(*obj != nullptr);
    reinterpret_cast<Hprof*>(arg)->VisitRoot(*obj, thread_id, root_type);
  }

  static void VisitObjectCallback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(obj != nullptr);
    DCHECK(arg != nullptr);
    reinterpret_cast<Hprof*>(arg)->DumpHeapObject(obj);
  }

  void VisitRoot(const mirror::Object* obj, uint32_t thread_id, RootType type)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  int DumpHeapObject(mirror::Object* obj) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void WriteClassTable() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    HprofRecord* rec = &current_record_;
    uint32_t nextSerialNumber = 1;

    for (mirror::Class* c : classes_) {
      CHECK(c != nullptr);
      total_header_bytes_ += current_record_.StartNewRecord(header_fp_, HPROF_TAG_LOAD_CLASS,
                                                            HPROF_TIME);
      // LOAD CLASS format:
      // U4: class serial number (always > 0)
      // ID: class object ID. We use the address of the class object structure as its ID.
      // U4: stack trace serial number
      // ID: class name string ID
      rec->AddU4(nextSerialNumber++);
      rec->AddObjectId(c);
      rec->AddU4(HPROF_NULL_STACK_TRACE);
      rec->AddStringId(LookupClassNameId(c));
    }
  }

  void WriteStringTable() {
    HprofRecord* rec = &current_record_;
    for (const std::pair<std::string, HprofStringId>& p : strings_) {
      const std::string& string = p.first;
      const size_t id = p.second;

      total_header_bytes_ += current_record_.StartNewRecord(header_fp_, HPROF_TAG_STRING,
                                                            HPROF_TIME);

      // STRING format:
      // ID:  ID for this string
      // U1*: UTF8 characters for string (NOT NULL terminated)
      //      (the record format encodes the length)
      rec->AddU4(id);
      rec->AddUtf8String(string.c_str());
    }
  }

  void StartNewHeapDumpSegment() {
    // This flushes the old segment and starts a new one.
    total_body_bytes_ += current_record_.StartNewRecord(body_fp_, HPROF_TAG_HEAP_DUMP_SEGMENT,
                                                        HPROF_TIME);
    objects_in_segment_ = 0;
    // Starting a new HEAP_DUMP resets the heap to default.
    current_heap_ = HPROF_HEAP_DEFAULT;
  }

  int MarkRootObject(const mirror::Object* obj, jobject jniObj);

  HprofClassObjectId LookupClassId(mirror::Class* c) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (c != nullptr) {
      auto result = classes_.insert(c);
      const mirror::Class* present = *result.first;
      CHECK_EQ(present, c);
      // Make sure that we've assigned a string ID for this class' name
      LookupClassNameId(c);
    }
    return PointerToLowMemUInt32(c);
  }

  HprofStringId LookupStringId(mirror::String* string) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return LookupStringId(string->ToModifiedUtf8());
  }

  HprofStringId LookupStringId(const char* string) {
    return LookupStringId(std::string(string));
  }

  HprofStringId LookupStringId(const std::string& string) {
    auto it = strings_.find(string);
    if (it != strings_.end()) {
      return it->second;
    }
    HprofStringId id = next_string_id_++;
    strings_.Put(string, id);
    return id;
  }

  HprofStringId LookupClassNameId(mirror::Class* c) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return LookupStringId(PrettyDescriptor(c));
  }

  void WriteFixedHeader() {
    char magic[] = "JAVA PROFILE 1.0.3";
    unsigned char buf[4] = { 0 };
    // Write the file header.
    // U1: NUL-terminated magic string.
    total_header_bytes_ += Write(magic, sizeof(magic), header_fp_);
    // U4: size of identifiers.  We're using addresses as IDs and our heap references are stored
    // as uint32_t.
    // Note of warning: hprof-conv hard-codes the size of identifiers to 4.
    static_assert(sizeof(mirror::HeapReference<mirror::Object>) == sizeof(uint32_t),
                  "Unexpected HeapReference size");
    U4_TO_BUF_BE(buf, 0, sizeof(uint32_t));
    total_header_bytes_ += Write(buf, sizeof(uint32_t), header_fp_);
    // The current time, in milliseconds since 0:00 GMT, 1/1/70.
    timeval now;
    const uint64_t nowMs = (gettimeofday(&now, NULL) < 0) ? 0 :
        (uint64_t)now.tv_sec * 1000 + now.tv_usec / 1000;
    // U4: high word of the 64-bit time.
    U4_TO_BUF_BE(buf, 0, (uint32_t)(nowMs >> 32));
    total_header_bytes_ += Write(buf, sizeof(uint32_t), header_fp_);
    // U4: low word of the 64-bit time.
    U4_TO_BUF_BE(buf, 0, (uint32_t)(nowMs & 0xffffffffULL));
    total_header_bytes_ += Write(buf, sizeof(uint32_t), header_fp_);  // xxx fix the time
  }

  void WriteStackTraces() {
    // Write a dummy stack trace record so the analysis tools don't freak out.
    total_header_bytes_ +=
        current_record_.StartNewRecord(header_fp_, HPROF_TAG_STACK_TRACE, HPROF_TIME);
    current_record_.AddU4(HPROF_NULL_STACK_TRACE);
    current_record_.AddU4(HPROF_NULL_THREAD);
    current_record_.AddU4(0);    // no frames
  }

  // If direct_to_ddms_ is set, "filename_" and "fd" will be ignored.
  // Otherwise, "filename_" must be valid, though if "fd" >= 0 it will
  // only be used for debug messages.
  std::string filename_;
  int fd_;
  bool direct_to_ddms_;

  // Whether or not we are in the size calculating mode or writing mode.
  bool allow_writing_;

  uint64_t start_ns_;

  HprofRecord current_record_;

  uint32_t gc_thread_serial_number_;
  uint8_t gc_scan_state_;
  HprofHeapId current_heap_;  // Which heap we're currently dumping.
  size_t objects_in_segment_;

  FILE* header_fp_;
  char* header_data_ptr_;
  size_t header_data_size_;
  size_t total_header_bytes_;

  FILE* body_fp_;
  char* body_data_ptr_;
  size_t body_data_size_;
  size_t total_body_bytes_;

  JDWP::JdwpNetStateBase* net_state_;

  std::set<mirror::Class*> classes_;
  HprofStringId next_string_id_;
  SafeMap<std::string, HprofStringId> strings_;

  DISALLOW_COPY_AND_ASSIGN(Hprof);
};

#define OBJECTS_PER_SEGMENT     ((size_t)128)
#define BYTES_PER_SEGMENT       ((size_t)4096)

// The static field-name for the synthetic object generated to account for class static overhead.
#define STATIC_OVERHEAD_NAME    "$staticOverhead"

static HprofBasicType SignatureToBasicTypeAndSize(const char* sig, size_t* sizeOut) {
  char c = sig[0];
  HprofBasicType ret;
  size_t size;

  switch (c) {
  case '[':
  case 'L': ret = hprof_basic_object;  size = 4; break;
  case 'Z': ret = hprof_basic_boolean; size = 1; break;
  case 'C': ret = hprof_basic_char;    size = 2; break;
  case 'F': ret = hprof_basic_float;   size = 4; break;
  case 'D': ret = hprof_basic_double;  size = 8; break;
  case 'B': ret = hprof_basic_byte;    size = 1; break;
  case 'S': ret = hprof_basic_short;   size = 2; break;
  case 'I': ret = hprof_basic_int;     size = 4; break;
  case 'J': ret = hprof_basic_long;    size = 8; break;
  default: LOG(FATAL) << "UNREACHABLE"; UNREACHABLE();
  }

  if (sizeOut != NULL) {
    *sizeOut = size;
  }

  return ret;
}

static HprofBasicType PrimitiveToBasicTypeAndSize(Primitive::Type prim, size_t* sizeOut) {
  HprofBasicType ret;
  size_t size;

  switch (prim) {
  case Primitive::kPrimBoolean: ret = hprof_basic_boolean; size = 1; break;
  case Primitive::kPrimChar:    ret = hprof_basic_char;    size = 2; break;
  case Primitive::kPrimFloat:   ret = hprof_basic_float;   size = 4; break;
  case Primitive::kPrimDouble:  ret = hprof_basic_double;  size = 8; break;
  case Primitive::kPrimByte:    ret = hprof_basic_byte;    size = 1; break;
  case Primitive::kPrimShort:   ret = hprof_basic_short;   size = 2; break;
  case Primitive::kPrimInt:     ret = hprof_basic_int;     size = 4; break;
  case Primitive::kPrimLong:    ret = hprof_basic_long;    size = 8; break;
  default: LOG(FATAL) << "UNREACHABLE"; UNREACHABLE();
  }

  if (sizeOut != NULL) {
    *sizeOut = size;
  }

  return ret;
}

// Always called when marking objects, but only does
// something when ctx->gc_scan_state_ is non-zero, which is usually
// only true when marking the root set or unreachable
// objects.  Used to add rootset references to obj.
int Hprof::MarkRootObject(const mirror::Object* obj, jobject jniObj) {
  HprofRecord* rec = &current_record_;
  HprofHeapTag heapTag = (HprofHeapTag)gc_scan_state_;

  if (heapTag == 0) {
    return 0;
  }

  if (objects_in_segment_ >= OBJECTS_PER_SEGMENT || rec->Size() >= BYTES_PER_SEGMENT) {
    StartNewHeapDumpSegment();
  }

  switch (heapTag) {
  // ID: object ID
  case HPROF_ROOT_UNKNOWN:
  case HPROF_ROOT_STICKY_CLASS:
  case HPROF_ROOT_MONITOR_USED:
  case HPROF_ROOT_INTERNED_STRING:
  case HPROF_ROOT_DEBUGGER:
  case HPROF_ROOT_VM_INTERNAL:
    rec->AddU1(heapTag);
    rec->AddObjectId(obj);
    break;

  // ID: object ID
  // ID: JNI global ref ID
  case HPROF_ROOT_JNI_GLOBAL:
    rec->AddU1(heapTag);
    rec->AddObjectId(obj);
    rec->AddJniGlobalRefId(jniObj);
    break;

  // ID: object ID
  // U4: thread serial number
  // U4: frame number in stack trace (-1 for empty)
  case HPROF_ROOT_JNI_LOCAL:
  case HPROF_ROOT_JNI_MONITOR:
  case HPROF_ROOT_JAVA_FRAME:
    rec->AddU1(heapTag);
    rec->AddObjectId(obj);
    rec->AddU4(gc_thread_serial_number_);
    rec->AddU4((uint32_t)-1);
    break;

  // ID: object ID
  // U4: thread serial number
  case HPROF_ROOT_NATIVE_STACK:
  case HPROF_ROOT_THREAD_BLOCK:
    rec->AddU1(heapTag);
    rec->AddObjectId(obj);
    rec->AddU4(gc_thread_serial_number_);
    break;

  // ID: thread object ID
  // U4: thread serial number
  // U4: stack trace serial number
  case HPROF_ROOT_THREAD_OBJECT:
    rec->AddU1(heapTag);
    rec->AddObjectId(obj);
    rec->AddU4(gc_thread_serial_number_);
    rec->AddU4((uint32_t)-1);    // xxx
    break;

  case HPROF_CLASS_DUMP:
  case HPROF_INSTANCE_DUMP:
  case HPROF_OBJECT_ARRAY_DUMP:
  case HPROF_PRIMITIVE_ARRAY_DUMP:
  case HPROF_HEAP_DUMP_INFO:
  case HPROF_PRIMITIVE_ARRAY_NODATA_DUMP:
    // Ignored.
    break;

  case HPROF_ROOT_FINALIZING:
  case HPROF_ROOT_REFERENCE_CLEANUP:
  case HPROF_UNREACHABLE:
    LOG(FATAL) << "obsolete tag " << static_cast<int>(heapTag);
    break;
  }

  ++objects_in_segment_;
  return 0;
}

static int StackTraceSerialNumber(const mirror::Object* /*obj*/) {
  return HPROF_NULL_STACK_TRACE;
}

int Hprof::DumpHeapObject(mirror::Object* obj) {
  HprofRecord* rec = &current_record_;
  gc::space::ContinuousSpace* space =
      Runtime::Current()->GetHeap()->FindContinuousSpaceFromObject(obj, true);
  HprofHeapId heap_type = HPROF_HEAP_APP;
  if (space != nullptr) {
    if (space->IsZygoteSpace()) {
      heap_type = HPROF_HEAP_ZYGOTE;
    } else if (space->IsImageSpace()) {
      heap_type = HPROF_HEAP_IMAGE;
    }
  }
  if (objects_in_segment_ >= OBJECTS_PER_SEGMENT || rec->Size() >= BYTES_PER_SEGMENT) {
    StartNewHeapDumpSegment();
  }

  if (heap_type != current_heap_) {
    HprofStringId nameId;

    // This object is in a different heap than the current one.
    // Emit a HEAP_DUMP_INFO tag to change heaps.
    rec->AddU1(HPROF_HEAP_DUMP_INFO);
    rec->AddU4(static_cast<uint32_t>(heap_type));   // uint32_t: heap type
    switch (heap_type) {
    case HPROF_HEAP_APP:
      nameId = LookupStringId("app");
      break;
    case HPROF_HEAP_ZYGOTE:
      nameId = LookupStringId("zygote");
      break;
    case HPROF_HEAP_IMAGE:
      nameId = LookupStringId("image");
      break;
    default:
      // Internal error
      LOG(ERROR) << "Unexpected desiredHeap";
      nameId = LookupStringId("<ILLEGAL>");
      break;
    }
    rec->AddStringId(nameId);
    current_heap_ = heap_type;
  }

  mirror::Class* c = obj->GetClass();
  if (c == NULL) {
    // This object will bother HprofReader, because it has a NULL
    // class, so just don't dump it. It could be
    // gDvm.unlinkedJavaLangClass or it could be an object just
    // allocated which hasn't been initialized yet.
  } else {
    if (obj->IsClass()) {
      mirror::Class* thisClass = obj->AsClass();
      // obj is a ClassObject.
      size_t sFieldCount = thisClass->NumStaticFields();
      if (sFieldCount != 0) {
        int byteLength = sFieldCount * sizeof(JValue);  // TODO bogus; fields are packed
        // Create a byte array to reflect the allocation of the
        // StaticField array at the end of this class.
        rec->AddU1(HPROF_PRIMITIVE_ARRAY_DUMP);
        rec->AddClassStaticsId(thisClass);
        rec->AddU4(StackTraceSerialNumber(obj));
        rec->AddU4(byteLength);
        rec->AddU1(hprof_basic_byte);
        for (int i = 0; i < byteLength; ++i) {
          rec->AddU1(0);
        }
      }

      rec->AddU1(HPROF_CLASS_DUMP);
      rec->AddClassId(LookupClassId(thisClass));
      rec->AddU4(StackTraceSerialNumber(thisClass));
      rec->AddClassId(LookupClassId(thisClass->GetSuperClass()));
      rec->AddObjectId(thisClass->GetClassLoader());
      rec->AddObjectId(nullptr);    // no signer
      rec->AddObjectId(nullptr);    // no prot domain
      rec->AddObjectId(nullptr);    // reserved
      rec->AddObjectId(nullptr);    // reserved
      if (thisClass->IsClassClass()) {
        // ClassObjects have their static fields appended, so aren't all the same size.
        // But they're at least this size.
        rec->AddU4(sizeof(mirror::Class));  // instance size
      } else if (thisClass->IsArrayClass() || thisClass->IsPrimitive()) {
        rec->AddU4(0);
      } else {
        rec->AddU4(thisClass->GetObjectSize());  // instance size
      }

      rec->AddU2(0);  // empty const pool

      // Static fields
      if (sFieldCount == 0) {
        rec->AddU2((uint16_t)0);
      } else {
        rec->AddU2((uint16_t)(sFieldCount+1));
        rec->AddStringId(LookupStringId(STATIC_OVERHEAD_NAME));
        rec->AddU1(hprof_basic_object);
        rec->AddClassStaticsId(thisClass);

        for (size_t i = 0; i < sFieldCount; ++i) {
          mirror::ArtField* f = thisClass->GetStaticField(i);

          size_t size;
          HprofBasicType t = SignatureToBasicTypeAndSize(f->GetTypeDescriptor(), &size);
          rec->AddStringId(LookupStringId(f->GetName()));
          rec->AddU1(t);
          if (size == 1) {
            rec->AddU1(static_cast<uint8_t>(f->Get32(thisClass)));
          } else if (size == 2) {
            rec->AddU2(static_cast<uint16_t>(f->Get32(thisClass)));
          } else if (size == 4) {
            rec->AddU4(f->Get32(thisClass));
          } else if (size == 8) {
            rec->AddU8(f->Get64(thisClass));
          } else {
            CHECK(false);
          }
        }
      }

      // Instance fields for this class (no superclass fields)
      int iFieldCount = thisClass->IsObjectClass() ? 0 : thisClass->NumInstanceFields();
      rec->AddU2((uint16_t)iFieldCount);
      for (int i = 0; i < iFieldCount; ++i) {
        mirror::ArtField* f = thisClass->GetInstanceField(i);
        HprofBasicType t = SignatureToBasicTypeAndSize(f->GetTypeDescriptor(), NULL);
        rec->AddStringId(LookupStringId(f->GetName()));
        rec->AddU1(t);
      }
    } else if (c->IsArrayClass()) {
      mirror::Array* aobj = obj->AsArray();
      uint32_t length = aobj->GetLength();

      if (obj->IsObjectArray()) {
        // obj is an object array.
        rec->AddU1(HPROF_OBJECT_ARRAY_DUMP);

        rec->AddObjectId(obj);
        rec->AddU4(StackTraceSerialNumber(obj));
        rec->AddU4(length);
        rec->AddClassId(LookupClassId(c));

        // Dump the elements, which are always objects or NULL.
        rec->AddIdList(aobj->AsObjectArray<mirror::Object>());
      } else {
        size_t size;
        HprofBasicType t = PrimitiveToBasicTypeAndSize(c->GetComponentType()->GetPrimitiveType(), &size);

        // obj is a primitive array.
        rec->AddU1(HPROF_PRIMITIVE_ARRAY_DUMP);

        rec->AddObjectId(obj);
        rec->AddU4(StackTraceSerialNumber(obj));
        rec->AddU4(length);
        rec->AddU1(t);

        // Dump the raw, packed element values.
        if (size == 1) {
          rec->AddU1List((const uint8_t*)aobj->GetRawData(sizeof(uint8_t), 0), length);
        } else if (size == 2) {
          rec->AddU2List((const uint16_t*)aobj->GetRawData(sizeof(uint16_t), 0), length);
        } else if (size == 4) {
          rec->AddU4List((const uint32_t*)aobj->GetRawData(sizeof(uint32_t), 0), length);
        } else if (size == 8) {
          rec->AddU8List((const uint64_t*)aobj->GetRawData(sizeof(uint64_t), 0), length);
        }
      }
    } else {
      // obj is an instance object.
      rec->AddU1(HPROF_INSTANCE_DUMP);
      rec->AddObjectId(obj);
      rec->AddU4(StackTraceSerialNumber(obj));
      rec->AddClassId(LookupClassId(c));

      // Reserve some space for the length of the instance data, which we won't
      // know until we're done writing it.
      size_t size_patch_offset = rec->Size();
      rec->AddU4(0x77777777);

      // Write the instance data;  fields for this class, followed by super class fields,
      // and so on. Don't write the klass or monitor fields of Object.class.
      mirror::Class* sclass = c;
      while (!sclass->IsObjectClass()) {
        int ifieldCount = sclass->NumInstanceFields();
        for (int i = 0; i < ifieldCount; ++i) {
          mirror::ArtField* f = sclass->GetInstanceField(i);
          size_t size;
          SignatureToBasicTypeAndSize(f->GetTypeDescriptor(), &size);
          if (size == 1) {
            rec->AddU1(f->Get32(obj));
          } else if (size == 2) {
            rec->AddU2(f->Get32(obj));
          } else if (size == 4) {
            rec->AddU4(f->Get32(obj));
          } else {
            CHECK_EQ(size, 8U);
            rec->AddU8(f->Get64(obj));
          }
        }

        sclass = sclass->GetSuperClass();
      }

      // Patch the instance field length.
      rec->UpdateU4(size_patch_offset, rec->Size() - (size_patch_offset + 4));
    }
  }

  ++objects_in_segment_;
  return 0;
}

void Hprof::VisitRoot(const mirror::Object* obj, uint32_t thread_id, RootType type) {
  static const HprofHeapTag xlate[] = {
    HPROF_ROOT_UNKNOWN,
    HPROF_ROOT_JNI_GLOBAL,
    HPROF_ROOT_JNI_LOCAL,
    HPROF_ROOT_JAVA_FRAME,
    HPROF_ROOT_NATIVE_STACK,
    HPROF_ROOT_STICKY_CLASS,
    HPROF_ROOT_THREAD_BLOCK,
    HPROF_ROOT_MONITOR_USED,
    HPROF_ROOT_THREAD_OBJECT,
    HPROF_ROOT_INTERNED_STRING,
    HPROF_ROOT_FINALIZING,
    HPROF_ROOT_DEBUGGER,
    HPROF_ROOT_REFERENCE_CLEANUP,
    HPROF_ROOT_VM_INTERNAL,
    HPROF_ROOT_JNI_MONITOR,
  };
  CHECK_LT(type, sizeof(xlate) / sizeof(HprofHeapTag));
  if (obj == NULL) {
    return;
  }
  gc_scan_state_ = xlate[type];
  gc_thread_serial_number_ = thread_id;
  MarkRootObject(obj, 0);
  gc_scan_state_ = 0;
  gc_thread_serial_number_ = 0;
}

// If "direct_to_ddms" is true, the other arguments are ignored, and data is
// sent directly to DDMS.
// If "fd" is >= 0, the output will be written to that file descriptor.
// Otherwise, "filename" is used to create an output file.
void DumpHeap(const char* filename, int fd, bool direct_to_ddms) {
  CHECK(filename != NULL);

  Runtime::Current()->GetThreadList()->SuspendAll();
  Hprof hprof(filename, fd, direct_to_ddms);
  hprof.Dump();
  Runtime::Current()->GetThreadList()->ResumeAll();
}

// Returns how many characters were in the buffer (or written).
size_t HprofRecord::Flush() {
  size_t chars = 0;
  if (dirty_) {
    unsigned char headBuf[sizeof(uint8_t) + 2 * sizeof(uint32_t)];
    headBuf[0] = tag_;
    U4_TO_BUF_BE(headBuf, 1, time_);
    U4_TO_BUF_BE(headBuf, 5, length_);
    chars += hprof_->Write(headBuf, sizeof(headBuf), fp_);
    chars += hprof_->Write(body_, length_, fp_);
    dirty_ = false;
  }
  return chars;
}

void HprofRecord::AddU1(uint8_t value) {
  if (hprof_->AllowWriting()) {
    GuaranteeRecordAppend(1);
    body_[length_] = value;
  }
  ++length_;
}

void HprofRecord::AddU1List(const uint8_t* values, size_t numValues) {
  if (hprof_->AllowWriting()) {
    GuaranteeRecordAppend(numValues);
    memcpy(body_ + length_, values, numValues);
  }
  length_ += numValues;
}

void HprofRecord::AddU2List(const uint16_t* values, size_t numValues) {
  if (hprof_->AllowWriting()) {
    GuaranteeRecordAppend(numValues * 2);
    unsigned char* insert = body_ + length_;
    for (size_t i = 0; i < numValues; ++i) {
      U2_TO_BUF_BE(insert, 0, *values++);
      insert += sizeof(*values);
    }
  }
  length_ += numValues * 2;
}

void HprofRecord::AddU4List(const uint32_t* values, size_t numValues) {
  if (hprof_->AllowWriting()) {
    GuaranteeRecordAppend(numValues * 4);
    unsigned char* insert = body_ + length_;
    for (size_t i = 0; i < numValues; ++i) {
      U4_TO_BUF_BE(insert, 0, *values++);
      insert += sizeof(*values);
    }
  }
  length_ += numValues * 4;
}

void HprofRecord::UpdateU4(size_t offset, uint32_t new_value) {
  if (hprof_->AllowWriting()) {
    U4_TO_BUF_BE(body_, offset, new_value);
  }
}

void HprofRecord::AddU8List(const uint64_t* values, size_t numValues) {
  if (hprof_->AllowWriting()) {
    GuaranteeRecordAppend(numValues * 8);
    unsigned char* insert = body_ + length_;
    for (size_t i = 0; i < numValues; ++i) {
      U8_TO_BUF_BE(insert, 0, *values++);
      insert += sizeof(*values);
    }
  }
  length_ += numValues * 8;
}

}  // namespace hprof
}  // namespace art
