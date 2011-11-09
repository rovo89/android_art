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
 * Heap object dump
 */
#include "hprof.h"
#include "primitive.h"
#include "object.h"
#include "logging.h"

namespace art {

namespace hprof {

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

static HprofBasicType signatureToBasicTypeAndSize(const char *sig, size_t *sizeOut) {
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

static HprofBasicType primitiveToBasicTypeAndSize(Primitive::Type prim, size_t *sizeOut) {
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

static int stackTraceSerialNumber(const void *obj) {
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
        rec->AddU4(stackTraceSerialNumber(obj));
        rec->AddU4(byteLength);
        rec->AddU1(hprof_basic_byte);
        for (int i = 0; i < byteLength; i++) {
          rec->AddU1(0);
        }
      }

      rec->AddU1(HPROF_CLASS_DUMP);
      rec->AddId(LookupClassId(thisClass));
      rec->AddU4(stackTraceSerialNumber(thisClass));
      rec->AddId(LookupClassId(thisClass->GetSuperClass()));
      rec->AddId((HprofObjectId)thisClass->GetClassLoader());
      rec->AddId((HprofObjectId)0);    // no signer
      rec->AddId((HprofObjectId)0);    // no prot domain
      rec->AddId((HprofId)0);           // reserved
      rec->AddId((HprofId)0);           // reserved
      if (obj->IsClassClass()) {
        // ClassObjects have their static fields appended, so aren't all the same size.
        // But they're at least this size.
        rec->AddU4(sizeof(Class)); // instance size
      } else if (thisClass->IsArrayClass() || thisClass->IsPrimitive()) {
        rec->AddU4(0);
      } else {
        rec->AddU4(thisClass->GetObjectSize()); // instance size
      }

      rec->AddU2(0); // empty const pool

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

          size_t size;
          HprofBasicType t = signatureToBasicTypeAndSize(f->GetTypeDescriptor(), &size);
          rec->AddId(LookupStringId(f->GetName()));
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
        HprofBasicType t = signatureToBasicTypeAndSize(f->GetTypeDescriptor(), NULL);
        rec->AddId(LookupStringId(f->GetName()));
        rec->AddU1(t);
      }
    } else if (clazz->IsArrayClass()) {
      Array *aobj = (Array *)obj;
      uint32_t length = aobj->GetLength();

      if (obj->IsObjectArray()) {
        // obj is an object array.
        rec->AddU1(HPROF_OBJECT_ARRAY_DUMP);

        rec->AddId((HprofObjectId)obj);
        rec->AddU4(stackTraceSerialNumber(obj));
        rec->AddU4(length);
        rec->AddId(LookupClassId(clazz));

        // Dump the elements, which are always objects or NULL.
        rec->AddIdList((const HprofObjectId *)aobj->GetRawData(), length);
      } else {
        size_t size;
        HprofBasicType t = primitiveToBasicTypeAndSize(clazz->GetComponentType()->GetPrimitiveType(), &size);

        // obj is a primitive array.
#if DUMP_PRIM_DATA
        rec->AddU1(HPROF_PRIMITIVE_ARRAY_DUMP);
#else
        rec->AddU1(HPROF_PRIMITIVE_ARRAY_NODATA_DUMP);
#endif

        rec->AddId((HprofObjectId)obj);
        rec->AddU4(stackTraceSerialNumber(obj));
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
      rec->AddU4(stackTraceSerialNumber(obj));
      rec->AddId(LookupClassId(clazz));

      // Reserve some space for the length of the instance data, which we won't
      // know until we're done writing it.
      size_t sizePatchOffset = rec->length_;
      rec->AddU4(0x77777777);

      // Write the instance data;  fields for this class, followed by super class fields,
      // and so on. Don't write the klass or monitor fields of Object.class.
      const Class* sclass = clazz;
      while (!sclass->IsObjectClass()) {
        int ifieldCount = sclass->NumInstanceFields();
        for (int i = 0; i < ifieldCount; i++) {
          Field* f = sclass->GetInstanceField(i);
          size_t size;
          signatureToBasicTypeAndSize(f->GetTypeDescriptor(), &size);
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

}  // namespace hprof

}  // namespace art
