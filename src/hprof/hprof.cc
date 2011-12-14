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

#include "class_linker.h"
#include "debugger.h"
#include "heap.h"
#include "logging.h"
#include "object.h"
#include "object_utils.h"
#include "stringprintf.h"
#include "unordered_map.h"
#include "unordered_set.h"

#include <cutils/open_memstream.h>
#include <sys/uio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>

namespace art {

namespace hprof {

#define HPROF_MAGIC_STRING  "JAVA PROFILE 1.0.3"

/*
 * Initialize an Hprof.
 */
Hprof::Hprof(const char* outputFileName, int fd, bool writeHeader, bool directToDdms)
    : current_record_(),
      gc_thread_serial_number_(0),
      gc_scan_state_(0),
      current_heap_(HPROF_HEAP_DEFAULT),
      objects_in_segment_(0),
      direct_to_ddms_(0),
      file_name_(outputFileName),
      file_data_ptr_(NULL),
      file_data_size_(0),
      mem_fp_(NULL),
      fd_(0),
      next_string_id_(0x400000) {
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
  mem_fp_ = fp;
  fd_ = fd;

  current_record_.alloc_length_ = 128;
  current_record_.body_ = (unsigned char*)malloc(current_record_.alloc_length_);
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

int Hprof::FlushCurrentRecord() {
  return current_record_.Flush(mem_fp_);
}

// Set DUMP_PRIM_DATA to 1 if you want to include the contents
// of primitive arrays (byte arrays, character arrays, etc.)
// in heap dumps.  This can be a large amount of data.
#define DUMP_PRIM_DATA 1

#define OBJECTS_PER_SEGMENT     ((size_t)128)
#define BYTES_PER_SEGMENT       ((size_t)4096)

// The static field-name for the synthetic object generated to account
// for class static overhead.
#define STATIC_OVERHEAD_NAME    "$staticOverhead"
// The ID for the synthetic object generated to account for class static overhead.
#define CLASS_STATICS_ID(clazz) ((HprofObjectId)(((uint32_t)(clazz)) | 1))

HprofBasicType Hprof::SignatureToBasicTypeAndSize(const char* sig, size_t* sizeOut) {
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
  default: CHECK(false);
  case 'I': ret = hprof_basic_int;     size = 4; break;
  case 'J': ret = hprof_basic_long;    size = 8; break;
  }

  if (sizeOut != NULL) {
    *sizeOut = size;
  }

  return ret;
}

HprofBasicType Hprof::PrimitiveToBasicTypeAndSize(Primitive::Type prim, size_t *sizeOut) {
  HprofBasicType ret;
  size_t size;

  switch (prim) {
  case Primitive::kPrimBoolean: ret = hprof_basic_boolean; size = 1; break;
  case Primitive::kPrimChar:    ret = hprof_basic_char;    size = 2; break;
  case Primitive::kPrimFloat:   ret = hprof_basic_float;   size = 4; break;
  case Primitive::kPrimDouble:  ret = hprof_basic_double;  size = 8; break;
  case Primitive::kPrimByte:    ret = hprof_basic_byte;    size = 1; break;
  case Primitive::kPrimShort:   ret = hprof_basic_short;   size = 2; break;
  default: CHECK(false);
  case Primitive::kPrimInt:     ret = hprof_basic_int;     size = 4; break;
  case Primitive::kPrimLong:    ret = hprof_basic_long;    size = 8; break;
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
int Hprof::MarkRootObject(const Object *obj, jobject jniObj) {
  HprofRecord *rec = &current_record_;
  int err; // TODO: we may return this uninitialized
  HprofHeapTag heapTag = (HprofHeapTag)gc_scan_state_;

  if (heapTag == 0) {
    return 0;
  }

  if (objects_in_segment_ >= OBJECTS_PER_SEGMENT || rec->length_ >= BYTES_PER_SEGMENT) {
    // This flushes the old segment and starts a new one.
    StartNewRecord(HPROF_TAG_HEAP_DUMP_SEGMENT, HPROF_TIME);
    objects_in_segment_ = 0;
  }

  switch (heapTag) {
  // ID: object ID
  case HPROF_ROOT_UNKNOWN:
  case HPROF_ROOT_STICKY_CLASS:
  case HPROF_ROOT_MONITOR_USED:
  case HPROF_ROOT_INTERNED_STRING:
  case HPROF_ROOT_FINALIZING:
  case HPROF_ROOT_DEBUGGER:
  case HPROF_ROOT_REFERENCE_CLEANUP:
  case HPROF_ROOT_VM_INTERNAL:
    rec->AddU1(heapTag);
    rec->AddId((HprofObjectId)obj);
    break;

  // ID: object ID
  // ID: JNI global ref ID
  case HPROF_ROOT_JNI_GLOBAL:
    rec->AddU1(heapTag);
    rec->AddId((HprofObjectId)obj);
    rec->AddId((HprofId)jniObj);
    break;

  // ID: object ID
  // U4: thread serial number
  // U4: frame number in stack trace (-1 for empty)
  case HPROF_ROOT_JNI_LOCAL:
  case HPROF_ROOT_JNI_MONITOR:
  case HPROF_ROOT_JAVA_FRAME:
    rec->AddU1(heapTag);
    rec->AddId((HprofObjectId)obj);
    rec->AddU4(gc_thread_serial_number_);
    rec->AddU4((uint32_t)-1);
    break;

  // ID: object ID
  // U4: thread serial number
  case HPROF_ROOT_NATIVE_STACK:
  case HPROF_ROOT_THREAD_BLOCK:
    rec->AddU1(heapTag);
    rec->AddId((HprofObjectId)obj);
    rec->AddU4(gc_thread_serial_number_);
    break;

  // ID: thread object ID
  // U4: thread serial number
  // U4: stack trace serial number
  case HPROF_ROOT_THREAD_OBJECT:
    rec->AddU1(heapTag);
    rec->AddId((HprofObjectId)obj);
    rec->AddU4(gc_thread_serial_number_);
    rec->AddU4((uint32_t)-1);    //xxx
    break;

  default:
    err = 0;
    break;
  }

  objects_in_segment_++;
  return err;
}

int Hprof::StackTraceSerialNumber(const void *obj) {
  return HPROF_NULL_STACK_TRACE;
}

int Hprof::DumpHeapObject(const Object* obj) {
  HprofRecord *rec = &current_record_;
  HprofHeapId desiredHeap = false ? HPROF_HEAP_ZYGOTE : HPROF_HEAP_APP; // TODO: zygote objects?

  if (objects_in_segment_ >= OBJECTS_PER_SEGMENT || rec->length_ >= BYTES_PER_SEGMENT) {
    // This flushes the old segment and starts a new one.
    StartNewRecord(HPROF_TAG_HEAP_DUMP_SEGMENT, HPROF_TIME);
    objects_in_segment_ = 0;

    // Starting a new HEAP_DUMP resets the heap to default.
    current_heap_ = HPROF_HEAP_DEFAULT;
  }

  if (desiredHeap != current_heap_) {
    HprofStringId nameId;

    // This object is in a different heap than the current one.
    // Emit a HEAP_DUMP_INFO tag to change heaps.
    rec->AddU1(HPROF_HEAP_DUMP_INFO);
    rec->AddU4((uint32_t)desiredHeap);   // uint32_t: heap id
    switch (desiredHeap) {
    case HPROF_HEAP_APP:
      nameId = LookupStringId("app");
      break;
    case HPROF_HEAP_ZYGOTE:
      nameId = LookupStringId("zygote");
      break;
    default:
      // Internal error
      LOG(ERROR) << "Unexpected desiredHeap";
      nameId = LookupStringId("<ILLEGAL>");
      break;
    }
    rec->AddId(nameId);
    current_heap_ = desiredHeap;
  }

  Class* clazz = obj->GetClass();
  if (clazz == NULL) {
    // This object will bother HprofReader, because it has a NULL
    // class, so just don't dump it. It could be
    // gDvm.unlinkedJavaLangClass or it could be an object just
    // allocated which hasn't been initialized yet.
  } else {
    if (obj->IsClass()) {
      Class* thisClass = (Class*)obj;
      // obj is a ClassObject.
      size_t sFieldCount = thisClass->NumStaticFields();
      if (sFieldCount != 0) {
        int byteLength = sFieldCount*sizeof(JValue); // TODO bogus; fields are packed
        // Create a byte array to reflect the allocation of the
        // StaticField array at the end of this class.
        rec->AddU1(HPROF_PRIMITIVE_ARRAY_DUMP);
        rec->AddId(CLASS_STATICS_ID(obj));
        rec->AddU4(StackTraceSerialNumber(obj));
        rec->AddU4(byteLength);
        rec->AddU1(hprof_basic_byte);
        for (int i = 0; i < byteLength; i++) {
          rec->AddU1(0);
        }
      }

      rec->AddU1(HPROF_CLASS_DUMP);
      rec->AddId(LookupClassId(thisClass));
      rec->AddU4(StackTraceSerialNumber(thisClass));
      rec->AddId(LookupClassId(thisClass->GetSuperClass()));
      rec->AddId((HprofObjectId)thisClass->GetClassLoader());
      rec->AddId((HprofObjectId)0);    // no signer
      rec->AddId((HprofObjectId)0);    // no prot domain
      rec->AddId((HprofId)0);           // reserved
      rec->AddId((HprofId)0);           // reserved
      if (thisClass->IsClassClass()) {
        // ClassObjects have their static fields appended, so aren't all the same size.
        // But they're at least this size.
        rec->AddU4(sizeof(Class)); // instance size
      } else if (thisClass->IsArrayClass() || thisClass->IsPrimitive()) {
        rec->AddU4(0);
      } else {
        rec->AddU4(thisClass->GetObjectSize()); // instance size
      }

      rec->AddU2(0); // empty const pool

      FieldHelper fh;

      // Static fields
      if (sFieldCount == 0) {
        rec->AddU2((uint16_t)0);
      } else {
        rec->AddU2((uint16_t)(sFieldCount+1));
        rec->AddId(LookupStringId(STATIC_OVERHEAD_NAME));
        rec->AddU1(hprof_basic_object);
        rec->AddId(CLASS_STATICS_ID(obj));

        for (size_t i = 0; i < sFieldCount; ++i) {
          Field* f = thisClass->GetStaticField(i);
          fh.ChangeField(f);

          size_t size;
          HprofBasicType t = SignatureToBasicTypeAndSize(fh.GetTypeDescriptor(), &size);
          rec->AddId(LookupStringId(fh.GetName()));
          rec->AddU1(t);
          if (size == 1) {
            rec->AddU1(static_cast<uint8_t>(f->Get32(NULL)));
          } else if (size == 2) {
            rec->AddU2(static_cast<uint16_t>(f->Get32(NULL)));
          } else if (size == 4) {
            rec->AddU4(f->Get32(NULL));
          } else if (size == 8) {
            rec->AddU8(f->Get64(NULL));
          } else {
            CHECK(false);
          }
        }
      }

      // Instance fields for this class (no superclass fields)
      int iFieldCount = thisClass->IsObjectClass() ? 0 : thisClass->NumInstanceFields();
      rec->AddU2((uint16_t)iFieldCount);
      for (int i = 0; i < iFieldCount; ++i) {
        Field* f = thisClass->GetInstanceField(i);
        fh.ChangeField(f);
        HprofBasicType t = SignatureToBasicTypeAndSize(fh.GetTypeDescriptor(), NULL);
        rec->AddId(LookupStringId(fh.GetName()));
        rec->AddU1(t);
      }
    } else if (clazz->IsArrayClass()) {
      Array *aobj = (Array *)obj;
      uint32_t length = aobj->GetLength();

      if (obj->IsObjectArray()) {
        // obj is an object array.
        rec->AddU1(HPROF_OBJECT_ARRAY_DUMP);

        rec->AddId((HprofObjectId)obj);
        rec->AddU4(StackTraceSerialNumber(obj));
        rec->AddU4(length);
        rec->AddId(LookupClassId(clazz));

        // Dump the elements, which are always objects or NULL.
        rec->AddIdList((const HprofObjectId *)aobj->GetRawData(), length);
      } else {
        size_t size;
        HprofBasicType t = PrimitiveToBasicTypeAndSize(clazz->GetComponentType()->GetPrimitiveType(), &size);

        // obj is a primitive array.
#if DUMP_PRIM_DATA
        rec->AddU1(HPROF_PRIMITIVE_ARRAY_DUMP);
#else
        rec->AddU1(HPROF_PRIMITIVE_ARRAY_NODATA_DUMP);
#endif

        rec->AddId((HprofObjectId)obj);
        rec->AddU4(StackTraceSerialNumber(obj));
        rec->AddU4(length);
        rec->AddU1(t);

#if DUMP_PRIM_DATA
        // Dump the raw, packed element values.
        if (size == 1) {
          rec->AddU1List((const uint8_t *)aobj->GetRawData(), length);
        } else if (size == 2) {
          rec->AddU2List((const uint16_t *)(void *)aobj->GetRawData(), length);
        } else if (size == 4) {
          rec->AddU4List((const uint32_t *)(void *)aobj->GetRawData(), length);
        } else if (size == 8) {
          rec->AddU8List((const uint64_t *)aobj->GetRawData(), length);
        }
#endif
      }
    } else {

      // obj is an instance object.
      rec->AddU1(HPROF_INSTANCE_DUMP);
      rec->AddId((HprofObjectId)obj);
      rec->AddU4(StackTraceSerialNumber(obj));
      rec->AddId(LookupClassId(clazz));

      // Reserve some space for the length of the instance data, which we won't
      // know until we're done writing it.
      size_t sizePatchOffset = rec->length_;
      rec->AddU4(0x77777777);

      // Write the instance data;  fields for this class, followed by super class fields,
      // and so on. Don't write the klass or monitor fields of Object.class.
      const Class* sclass = clazz;
      FieldHelper fh;
      while (!sclass->IsObjectClass()) {
        int ifieldCount = sclass->NumInstanceFields();
        for (int i = 0; i < ifieldCount; i++) {
          Field* f = sclass->GetInstanceField(i);
          fh.ChangeField(f);
          size_t size;
          SignatureToBasicTypeAndSize(fh.GetTypeDescriptor(), &size);
          if (size == 1) {
            rec->AddU1(f->Get32(obj));
          } else if (size == 2) {
            rec->AddU2(f->Get32(obj));
          } else if (size == 4) {
            rec->AddU4(f->Get32(obj));
          } else if (size == 8) {
            rec->AddU8(f->Get64(obj));
          } else {
            CHECK(false);
          }
        }

        sclass = sclass->GetSuperClass();
      }

      // Patch the instance field length.
      size_t savedLen = rec->length_;
      rec->length_ = sizePatchOffset;
      rec->AddU4(savedLen - (sizePatchOffset + 4));
      rec->length_ = savedLen;
    }
  }

  objects_in_segment_++;
  return 0;
}

#define kHeadSuffix "-hptemp"

// TODO: use File::WriteFully
int sysWriteFully(int fd, const void* buf, size_t count, const char* logMsg) {
  while (count != 0) {
    ssize_t actual = TEMP_FAILURE_RETRY(write(fd, buf, count));
    if (actual < 0) {
      int err = errno;
      LOG(ERROR) << StringPrintf("%s: write failed: %s", logMsg, strerror(err));
      return err;
    } else if (actual != (ssize_t) count) {
      LOG(DEBUG) << StringPrintf("%s: partial write (will retry): (%d of %zd)",
          logMsg, (int) actual, count);
      buf = (const void*) (((const uint8_t*) buf) + actual);
    }
    count -= actual;
  }
  return 0;
}

/*
 * Finish up the hprof dump.  Returns true on success.
 */
bool Hprof::Finish() {
  // flush the "tail" portion of the output
  StartNewRecord(HPROF_TAG_HEAP_DUMP_END, HPROF_TIME);
  FlushCurrentRecord();

  // create a new Hprof for the start of the file (as opposed to this, which is the tail)
  Hprof headCtx(file_name_.c_str(), fd_, true, direct_to_ddms_);
  headCtx.classes_ = classes_;
  headCtx.strings_ = strings_;

  LOG(INFO) << StringPrintf("hprof: dumping heap strings to \"%s\".", file_name_.c_str());
  headCtx.DumpStrings();
  headCtx.DumpClasses();

  // write a dummy stack trace record so the analysis tools don't freak out
  headCtx.StartNewRecord(HPROF_TAG_STACK_TRACE, HPROF_TIME);
  headCtx.current_record_.AddU4(HPROF_NULL_STACK_TRACE);
  headCtx.current_record_.AddU4(HPROF_NULL_THREAD);
  headCtx.current_record_.AddU4(0);    // no frames

  headCtx.FlushCurrentRecord();

  // flush to ensure memstream pointer and size are updated
  fflush(headCtx.mem_fp_);
  fflush(mem_fp_);

  if (direct_to_ddms_) {
    // send the data off to DDMS
    struct iovec iov[2];
    iov[0].iov_base = headCtx.file_data_ptr_;
    iov[0].iov_len = headCtx.file_data_size_;
    iov[1].iov_base = file_data_ptr_;
    iov[1].iov_len = file_data_size_;
    Dbg::DdmSendChunkV(CHUNK_TYPE("HPDS"), iov, 2);
  } else {
    // open the output file, and copy the head and tail to it.
    CHECK_EQ(headCtx.fd_, fd_);

    int outFd;
    if (headCtx.fd_ >= 0) {
      outFd = dup(headCtx.fd_);
      if (outFd < 0) {
        LOG(ERROR) << StringPrintf("dup(%d) failed: %s", headCtx.fd_, strerror(errno));
        // continue to fail-handler below
      }
    } else {
      outFd = open(file_name_.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0644);
      if (outFd < 0) {
        LOG(ERROR) << StringPrintf("can't open %s: %s", headCtx.file_name_.c_str(), strerror(errno));
        // continue to fail-handler below
      }
    }
    if (outFd < 0) {
      return false;
    }

    int result = sysWriteFully(outFd, headCtx.file_data_ptr_,
        headCtx.file_data_size_, "hprof-head");
    result |= sysWriteFully(outFd, file_data_ptr_, file_data_size_, "hprof-tail");
    close(outFd);
    if (result != 0) {
      return false;
    }
  }

  // throw out a log message for the benefit of "runhat"
  LOG(INFO) << "hprof: heap dump completed (" << ((headCtx.file_data_size_ + file_data_size_ + 1023) / KB) << "KiB)";

  return true;
}

Hprof::~Hprof() {
  // we don't own ctx->fd_, do not close
  if (mem_fp_ != NULL) {
    fclose(mem_fp_);
  }
  free(current_record_.body_);
  free(file_data_ptr_);
}

void Hprof::VisitRoot(const Object* obj) {
  uint32_t threadId = 0;  // TODO
  /*RootType */ size_t type = 0; // TODO

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
  gc_thread_serial_number_ = threadId;
  MarkRootObject(obj, 0);
  gc_scan_state_ = 0;
  gc_thread_serial_number_ = 0;
}

HprofStringId Hprof::LookupStringId(String* string) {
  return LookupStringId(string->ToModifiedUtf8());
}

HprofStringId Hprof::LookupStringId(const char* string) {
  return LookupStringId(std::string(string));
}

HprofStringId Hprof::LookupStringId(std::string string) {
  if (strings_.find(string) == strings_.end()) {
    strings_[string] = next_string_id_++;
  }
  return strings_[string];
}

int Hprof::DumpStrings() {
  HprofRecord *rec = &current_record_;

  for (StringMapIterator it = strings_.begin(); it != strings_.end(); ++it) {
    std::string string = (*it).first;
    size_t id = (*it).second;

    int err = StartNewRecord(HPROF_TAG_STRING, HPROF_TIME);
    if (err != 0) {
      return err;
    }

    // STRING format:
    // ID:  ID for this string
    // U1*: UTF8 characters for string (NOT NULL terminated)
    //      (the record format encodes the length)
    err = rec->AddU4(id);
    if (err != 0) {
      return err;
    }
    err = rec->AddUtf8String(string.c_str());
    if (err != 0) {
      return err;
    }
  }

  return 0;
}

HprofStringId Hprof::LookupClassNameId(Class* clazz) {
  return LookupStringId(PrettyDescriptor(clazz));
}

HprofClassObjectId Hprof::LookupClassId(Class* clazz) {
  if (clazz == NULL) {
    // clazz is the superclass of java.lang.Object or a primitive
    return (HprofClassObjectId)0;
  }

  std::pair<ClassSetIterator, bool> result = classes_.insert(clazz);
  Class* present = *result.first;

  // Make sure that we've assigned a string ID for this class' name
  LookupClassNameId(clazz);

  CHECK_EQ(present, clazz);
  return (HprofStringId) present;
}

int Hprof::DumpClasses() {
  HprofRecord *rec = &current_record_;
  uint32_t nextSerialNumber = 1;

  for (ClassSetIterator it = classes_.begin(); it != classes_.end(); ++it) {
    Class* clazz = *it;
    CHECK(clazz != NULL);

    int err = StartNewRecord(HPROF_TAG_LOAD_CLASS, HPROF_TIME);
    if (err != 0) {
      return err;
    }

    // LOAD CLASS format:
    // U4: class serial number (always > 0)
    // ID: class object ID. We use the address of the class object structure as its ID.
    // U4: stack trace serial number
    // ID: class name string ID
    rec->AddU4(nextSerialNumber++);
    rec->AddId((HprofClassObjectId) clazz);
    rec->AddU4(HPROF_NULL_STACK_TRACE);
    rec->AddId(LookupClassNameId(clazz));
  }

  return 0;
}

void HprofRootVisitor(const Object* obj, void* arg) {
  CHECK(arg != NULL);
  Hprof* hprof = (Hprof*)arg;
  hprof->VisitRoot(obj);
}

void HprofBitmapCallback(Object *obj, void *arg) {
  CHECK(obj != NULL);
  CHECK(arg != NULL);
  Hprof *hprof = (Hprof*)arg;
  hprof->DumpHeapObject(obj);
}

/*
 * Walk the roots and heap writing heap information to the specified
 * file.
 *
 * If "fd" is >= 0, the output will be written to that file descriptor.
 * Otherwise, "file_name_" is used to create an output file.
 *
 * If "direct_to_ddms_" is set, the other arguments are ignored, and data is
 * sent directly to DDMS.
 *
 * Returns 0 on success, or an error code on failure.
 */
int DumpHeap(const char* fileName, int fd, bool directToDdms) {
  CHECK(fileName != NULL);
  ScopedHeapLock lock;
  ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);

  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  thread_list->SuspendAll();

  Hprof hprof(fileName, fd, false, directToDdms);
  Runtime::Current()->VisitRoots(HprofRootVisitor, &hprof);
  Heap::GetLiveBits()->Walk(HprofBitmapCallback, &hprof);
  // TODO: write a HEAP_SUMMARY record
  int success = hprof.Finish() ? 0 : -1;
  thread_list->ResumeAll();
  return success;
}

}  // namespace hprof

}  // namespace art
