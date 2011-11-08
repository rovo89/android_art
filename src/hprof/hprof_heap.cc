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

/* Set DUMP_PRIM_DATA to 1 if you want to include the contents
 * of primitive arrays (byte arrays, character arrays, etc.)
 * in heap dumps.  This can be a large amount of data.
 */
#define DUMP_PRIM_DATA 1

#define OBJECTS_PER_SEGMENT     ((size_t)128)
#define BYTES_PER_SEGMENT       ((size_t)4096)

/* The static field-name for the synthetic object generated to account
 * for class Static overhead.
 */
#define STATIC_OVERHEAD_NAME    "$staticOverhead"
/* The ID for the synthetic object generated to account for class
 * Static overhead.
 */
#define CLASS_STATICS_ID(clazz) ((hprof_object_id)(((uint32_t)(clazz)) | 1))

int hprofStartHeapDump(hprof_context_t *ctx)
{
    ctx->objectsInSegment = OBJECTS_PER_SEGMENT;
    ctx->currentHeap = HPROF_HEAP_DEFAULT;
    return 0;
}

int hprofFinishHeapDump(hprof_context_t *ctx)
{
    return hprofStartNewRecord(ctx, HPROF_TAG_HEAP_DUMP_END, HPROF_TIME);
}

int hprofSetGcScanState(hprof_context_t *ctx,
                        hprof_heap_tag_t state,
                        uint32_t threadSerialNumber)
{
    /* Used by hprofMarkRootObject()
     */
    ctx->gcScanState = state;
    ctx->gcThreadSerialNumber = threadSerialNumber;
    return 0;
}

static hprof_basic_type signatureToBasicTypeAndSize(const char *sig,
                                                    size_t *sizeOut)
{
    char c = sig[0];
    hprof_basic_type ret;
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

static hprof_basic_type primitiveToBasicTypeAndSize(Primitive::Type prim,
                                                    size_t *sizeOut)
{
    hprof_basic_type ret;
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

/* Always called when marking objects, but only does
 * something when ctx->gcScanState is non-zero, which is usually
 * only true when marking the root set or unreachable
 * objects.  Used to add rootset references to obj.
 */
int hprofMarkRootObject(hprof_context_t *ctx, const Object *obj, jobject jniObj)
{
    hprof_record_t *rec = &ctx->curRec;
    int err;
    hprof_heap_tag_t heapTag = (hprof_heap_tag_t)ctx->gcScanState;

    if (heapTag == 0) {
        return 0;
    }

    if (ctx->objectsInSegment >= OBJECTS_PER_SEGMENT ||
        rec->length >= BYTES_PER_SEGMENT)
    {
        /* This flushes the old segment and starts a new one.
         */
        hprofStartNewRecord(ctx, HPROF_TAG_HEAP_DUMP_SEGMENT, HPROF_TIME);
        ctx->objectsInSegment = 0;
    }

    switch (heapTag) {
    /* ID: object ID
     */
    case HPROF_ROOT_UNKNOWN:
    case HPROF_ROOT_STICKY_CLASS:
    case HPROF_ROOT_MONITOR_USED:
    case HPROF_ROOT_INTERNED_STRING:
    case HPROF_ROOT_FINALIZING:
    case HPROF_ROOT_DEBUGGER:
    case HPROF_ROOT_REFERENCE_CLEANUP:
    case HPROF_ROOT_VM_INTERNAL:
        hprofAddU1ToRecord(rec, heapTag);
        hprofAddIdToRecord(rec, (hprof_object_id)obj);
        break;

    /* ID: object ID
     * ID: JNI global ref ID
     */
    case HPROF_ROOT_JNI_GLOBAL:
        hprofAddU1ToRecord(rec, heapTag);
        hprofAddIdToRecord(rec, (hprof_object_id)obj);
        hprofAddIdToRecord(rec, (hprof_id)jniObj);
        break;

    /* ID: object ID
     * uint32_t: thread serial number
     * uint32_t: frame number in stack trace (-1 for empty)
     */
    case HPROF_ROOT_JNI_LOCAL:
    case HPROF_ROOT_JNI_MONITOR:
    case HPROF_ROOT_JAVA_FRAME:
        hprofAddU1ToRecord(rec, heapTag);
        hprofAddIdToRecord(rec, (hprof_object_id)obj);
        hprofAddU4ToRecord(rec, ctx->gcThreadSerialNumber);
        hprofAddU4ToRecord(rec, (uint32_t)-1);
        break;

    /* ID: object ID
     * uint32_t: thread serial number
     */
    case HPROF_ROOT_NATIVE_STACK:
    case HPROF_ROOT_THREAD_BLOCK:
        hprofAddU1ToRecord(rec, heapTag);
        hprofAddIdToRecord(rec, (hprof_object_id)obj);
        hprofAddU4ToRecord(rec, ctx->gcThreadSerialNumber);
        break;

    /* ID: thread object ID
     * uint32_t: thread serial number
     * uint32_t: stack trace serial number
     */
    case HPROF_ROOT_THREAD_OBJECT:
        hprofAddU1ToRecord(rec, heapTag);
        hprofAddIdToRecord(rec, (hprof_object_id)obj);
        hprofAddU4ToRecord(rec, ctx->gcThreadSerialNumber);
        hprofAddU4ToRecord(rec, (uint32_t)-1);    //xxx
        break;

    default:
        err = 0;
        break;
    }

    ctx->objectsInSegment++;

    return err;
}

static int stackTraceSerialNumber(const void *obj)
{
    return HPROF_NULL_STACK_TRACE;
}

int DumpHeapObject(hprof_context_t* ctx, const Object* obj) {
  Class* clazz;
  hprof_record_t *rec = &ctx->curRec;

  HprofHeapId desiredHeap = false ? HPROF_HEAP_ZYGOTE : HPROF_HEAP_APP; // TODO: zygote objects?

  if (ctx->objectsInSegment >= OBJECTS_PER_SEGMENT || rec->length >= BYTES_PER_SEGMENT) {
    /* This flushes the old segment and starts a new one.
     */
    hprofStartNewRecord(ctx, HPROF_TAG_HEAP_DUMP_SEGMENT, HPROF_TIME);
    ctx->objectsInSegment = 0;

    /* Starting a new HEAP_DUMP resets the heap to default.
     */
    ctx->currentHeap = HPROF_HEAP_DEFAULT;
  }

  if (desiredHeap != ctx->currentHeap) {
    hprof_string_id nameId;

    /* This object is in a different heap than the current one.
     * Emit a HEAP_DUMP_INFO tag to change heaps.
     */
    hprofAddU1ToRecord(rec, HPROF_HEAP_DUMP_INFO);
    hprofAddU4ToRecord(rec, (uint32_t)desiredHeap);   // uint32_t: heap id
    switch (desiredHeap) {
    case HPROF_HEAP_APP:
      nameId = hprofLookupStringId("app");
      break;
    case HPROF_HEAP_ZYGOTE:
      nameId = hprofLookupStringId("zygote");
      break;
    default:
      /* Internal error. */
      LOG(ERROR) << "Unexpected desiredHeap";
      nameId = hprofLookupStringId("<ILLEGAL>");
      break;
    }
    hprofAddIdToRecord(rec, nameId);
    ctx->currentHeap = desiredHeap;
  }

  clazz = obj->GetClass();

  if (clazz == NULL) {
    /* This object will bother HprofReader, because it has a NULL
     * class, so just don't dump it. It could be
     * gDvm.unlinkedJavaLangClass or it could be an object just
     * allocated which hasn't been initialized yet.
     */
  } else {
    if (obj->IsClass()) {
      Class* thisClass = (Class*)obj;
      /* obj is a ClassObject.
       */
      size_t sFieldCount = thisClass->NumStaticFields();
      if (sFieldCount != 0) {
        int byteLength = sFieldCount*sizeof(JValue); // TODO bogus; fields are packed
        /* Create a byte array to reflect the allocation of the
         * StaticField array at the end of this class.
         */
        hprofAddU1ToRecord(rec, HPROF_PRIMITIVE_ARRAY_DUMP);
        hprofAddIdToRecord(rec, CLASS_STATICS_ID(obj));
        hprofAddU4ToRecord(rec, stackTraceSerialNumber(obj));
        hprofAddU4ToRecord(rec, byteLength);
        hprofAddU1ToRecord(rec, hprof_basic_byte);
        for (int i = 0; i < byteLength; i++) {
          hprofAddU1ToRecord(rec, 0);
        }
      }

      hprofAddU1ToRecord(rec, HPROF_CLASS_DUMP);
      hprofAddIdToRecord(rec, hprofLookupClassId(thisClass));
      hprofAddU4ToRecord(rec, stackTraceSerialNumber(thisClass));
      hprofAddIdToRecord(rec, hprofLookupClassId(thisClass->GetSuperClass()));
      hprofAddIdToRecord(rec, (hprof_object_id)thisClass->GetClassLoader());
      hprofAddIdToRecord(rec, (hprof_object_id)0);    // no signer
      hprofAddIdToRecord(rec, (hprof_object_id)0);    // no prot domain
      hprofAddIdToRecord(rec, (hprof_id)0);           // reserved
      hprofAddIdToRecord(rec, (hprof_id)0);           // reserved
      if (obj->IsClassClass()) {
        // ClassObjects have their static fields appended, so
        // aren't all the same size. But they're at least this
        // size.
        hprofAddU4ToRecord(rec, sizeof(Class)); // instance size
      } else if (thisClass->IsArrayClass() || thisClass->IsPrimitive()) {
        hprofAddU4ToRecord(rec, 0);
      } else {
        hprofAddU4ToRecord(rec, thisClass->GetObjectSize()); // instance size
      }

      hprofAddU2ToRecord(rec, 0);                     // empty const pool

      /* Static fields
       */
      if (sFieldCount == 0) {
        hprofAddU2ToRecord(rec, (uint16_t)0);
      } else {
        hprofAddU2ToRecord(rec, (uint16_t)(sFieldCount+1));
        hprofAddIdToRecord(rec, hprofLookupStringId(STATIC_OVERHEAD_NAME));
        hprofAddU1ToRecord(rec, hprof_basic_object);
        hprofAddIdToRecord(rec, CLASS_STATICS_ID(obj));

        for (size_t i = 0; i < sFieldCount; ++i) {
          Field* f = thisClass->GetStaticField(i);

          size_t size;
          hprof_basic_type t = signatureToBasicTypeAndSize(f->GetTypeDescriptor(), &size);
          hprofAddIdToRecord(rec, hprofLookupStringId(f->GetName()));
          hprofAddU1ToRecord(rec, t);
          if (size == 1) {
            hprofAddU1ToRecord(rec, static_cast<uint8_t>(f->Get32(NULL)));
          } else if (size == 2) {
            hprofAddU2ToRecord(rec, static_cast<uint16_t>(f->Get32(NULL)));
          } else if (size == 4) {
            hprofAddU4ToRecord(rec, f->Get32(NULL));
          } else if (size == 8) {
            hprofAddU8ToRecord(rec, f->Get64(NULL));
          } else {
            CHECK(false);
          }
        }
      }

      /* Instance fields for this class (no superclass fields)
       */
      int iFieldCount = thisClass->IsObjectClass() ? 0 : thisClass->NumInstanceFields();
      hprofAddU2ToRecord(rec, (uint16_t)iFieldCount);
      for (int i = 0; i < iFieldCount; ++i) {
        Field* f = thisClass->GetInstanceField(i);
        hprof_basic_type t = signatureToBasicTypeAndSize(f->GetTypeDescriptor(), NULL);
        hprofAddIdToRecord(rec, hprofLookupStringId(f->GetName()));
        hprofAddU1ToRecord(rec, t);
      }
    } else if (clazz->IsArrayClass()) {
      Array *aobj = (Array *)obj;
      uint32_t length = aobj->GetLength();

      if (obj->IsObjectArray()) {
        /* obj is an object array.
         */
        hprofAddU1ToRecord(rec, HPROF_OBJECT_ARRAY_DUMP);

        hprofAddIdToRecord(rec, (hprof_object_id)obj);
        hprofAddU4ToRecord(rec, stackTraceSerialNumber(obj));
        hprofAddU4ToRecord(rec, length);
        hprofAddIdToRecord(rec, hprofLookupClassId(clazz));

        /* Dump the elements, which are always objects or NULL.
         */
        hprofAddIdListToRecord(rec, (const hprof_object_id *)aobj->GetRawData(), length);
      } else {
        size_t size;
        hprof_basic_type t = primitiveToBasicTypeAndSize(clazz->GetComponentType()->GetPrimitiveType(), &size);

        /* obj is a primitive array.
         */
#if DUMP_PRIM_DATA
        hprofAddU1ToRecord(rec, HPROF_PRIMITIVE_ARRAY_DUMP);
#else
        hprofAddU1ToRecord(rec, HPROF_PRIMITIVE_ARRAY_NODATA_DUMP);
#endif

        hprofAddIdToRecord(rec, (hprof_object_id)obj);
        hprofAddU4ToRecord(rec, stackTraceSerialNumber(obj));
        hprofAddU4ToRecord(rec, length);
        hprofAddU1ToRecord(rec, t);

#if DUMP_PRIM_DATA
        /* Dump the raw, packed element values.
         */
        if (size == 1) {
          hprofAddU1ListToRecord(rec, (const uint8_t *)aobj->GetRawData(), length);
        } else if (size == 2) {
          hprofAddU2ListToRecord(rec, (const uint16_t *)(void *)aobj->GetRawData(), length);
        } else if (size == 4) {
          hprofAddU4ListToRecord(rec, (const uint32_t *)(void *)aobj->GetRawData(), length);
        } else if (size == 8) {
          hprofAddU8ListToRecord(rec, (const uint64_t *)aobj->GetRawData(), length);
        }
#endif
      }
    } else {

      /* obj is an instance object.
       */
      hprofAddU1ToRecord(rec, HPROF_INSTANCE_DUMP);
      hprofAddIdToRecord(rec, (hprof_object_id)obj);
      hprofAddU4ToRecord(rec, stackTraceSerialNumber(obj));
      hprofAddIdToRecord(rec, hprofLookupClassId(clazz));

      /* Reserve some space for the length of the instance
       * data, which we won't know until we're done writing
       * it.
       */
      size_t sizePatchOffset = rec->length;
      hprofAddU4ToRecord(rec, 0x77777777);

      /* Write the instance data;  fields for this
       * class, followed by super class fields, and so on.
       * Don't write the klass or monitor fields of Object.class.
       */
      const Class* sclass = clazz;
      while (!sclass->IsObjectClass()) {
        int ifieldCount = sclass->NumInstanceFields();
        for (int i = 0; i < ifieldCount; i++) {
          Field* f = sclass->GetInstanceField(i);
          size_t size;
          signatureToBasicTypeAndSize(f->GetTypeDescriptor(), &size);
          if (size == 1) {
            hprofAddU1ToRecord(rec, f->Get32(obj));
          } else if (size == 2) {
            hprofAddU2ToRecord(rec, f->Get32(obj));
          } else if (size == 4) {
            hprofAddU4ToRecord(rec, f->Get32(obj));
          } else if (size == 8) {
            hprofAddU8ToRecord(rec, f->Get64(obj));
          } else {
            CHECK(false);
          }
        }

        sclass = sclass->GetSuperClass();
      }

      /* Patch the instance field length.
       */
      size_t savedLen = rec->length;
      rec->length = sizePatchOffset;
      hprofAddU4ToRecord(rec, savedLen - (sizePatchOffset + 4));
      rec->length = savedLen;
    }
  }

  ctx->objectsInSegment++;

  return 0;
}

}  // namespace hprof

}  // namespace art
