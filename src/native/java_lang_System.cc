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

#include "jni_internal.h"
#include "object.h"
#include "scoped_jni_thread_state.h"

/*
 * We make guarantees about the atomicity of accesses to primitive
 * variables.  These guarantees also apply to elements of arrays.
 * In particular, 8-bit, 16-bit, and 32-bit accesses must be atomic and
 * must not cause "word tearing".  Accesses to 64-bit array elements must
 * either be atomic or treated as two 32-bit operations.  References are
 * always read and written atomically, regardless of the number of bits
 * used to represent them.
 *
 * We can't rely on standard libc functions like memcpy(3) and memmove(3)
 * in our implementation of System.arraycopy, because they may copy
 * byte-by-byte (either for the full run or for "unaligned" parts at the
 * start or end).  We need to use functions that guarantee 16-bit or 32-bit
 * atomicity as appropriate.
 *
 * System.arraycopy() is heavily used, so having an efficient implementation
 * is important.  The bionic libc provides a platform-optimized memory move
 * function that should be used when possible.  If it's not available,
 * the trivial "reference implementation" versions below can be used until
 * a proper version can be written.
 *
 * For these functions, The caller must guarantee that dst/src are aligned
 * appropriately for the element type, and that n is a multiple of the
 * element size.
 */
#ifdef __BIONIC__
#define HAVE_MEMMOVE_WORDS
#endif

#ifdef HAVE_MEMMOVE_WORDS
extern "C" void _memmove_words(void* dst, const void* src, size_t n);
#define move16 _memmove_words
#define move32 _memmove_words
#else
static void move16(void* dst, const void* src, size_t n) {
  DCHECK_EQ((((uintptr_t) dst | (uintptr_t) src | n) & 0x01), 0U);

  uint16_t* d = reinterpret_cast<uint16_t*>(dst);
  const uint16_t* s = reinterpret_cast<const uint16_t*>(src);

  n /= sizeof(uint16_t);

  if (d < s) {
    // Copy forwards.
    while (n--) {
      *d++ = *s++;
    }
  } else {
    // Copy backwards.
    d += n;
    s += n;
    while (n--) {
      *--d = *--s;
    }
  }
}

static void move32(void* dst, const void* src, size_t n) {
  DCHECK_EQ((((uintptr_t) dst | (uintptr_t) src | n) & 0x03), 0U);

  uint32_t* d = reinterpret_cast<uint32_t*>(dst);
  const uint32_t* s = reinterpret_cast<const uint32_t*>(src);

  n /= sizeof(uint32_t);

  if (d < s) {
    // Copy forwards.
    while (n--) {
      *d++ = *s++;
    }
  } else {
    // Copy backwards.
    d += n;
    s += n;
    while (n--) {
      *--d = *--s;
    }
  }
}
#endif // HAVE_MEMMOVE_WORDS

namespace art {

static void ThrowArrayStoreException_NotAnArray(const char* identifier, Object* array) {
  std::string actualType(PrettyTypeOf(array));
  Thread::Current()->ThrowNewExceptionF("Ljava/lang/ArrayStoreException;",
      "%s of type %s is not an array", identifier, actualType.c_str());
}

static void System_arraycopy(JNIEnv* env, jclass, jobject javaSrc, jint srcPos, jobject javaDst, jint dstPos, jint length) {
  ScopedJniThreadState ts(env);

  // Null pointer checks.
  if (javaSrc == NULL) {
    ts.Self()->ThrowNewException("Ljava/lang/NullPointerException;", "src == null");
    return;
  }
  if (javaDst == NULL) {
    ts.Self()->ThrowNewException("Ljava/lang/NullPointerException;", "dst == null");
    return;
  }

  // Make sure source and destination are both arrays.
  Object* srcObject = ts.Decode<Object*>(javaSrc);
  Object* dstObject = ts.Decode<Object*>(javaDst);
  if (!srcObject->IsArrayInstance()) {
    ThrowArrayStoreException_NotAnArray("source", srcObject);
    return;
  }
  if (!dstObject->IsArrayInstance()) {
    ThrowArrayStoreException_NotAnArray("destination", dstObject);
    return;
  }
  Array* srcArray = srcObject->AsArray();
  Array* dstArray = dstObject->AsArray();
  Class* srcComponentType = srcArray->GetClass()->GetComponentType();
  Class* dstComponentType = dstArray->GetClass()->GetComponentType();

  // Bounds checking.
  if (srcPos < 0 || dstPos < 0 || length < 0 || srcPos > srcArray->GetLength() - length || dstPos > dstArray->GetLength() - length) {
    ts.Self()->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
        "src.length=%d srcPos=%d dst.length=%d dstPos=%d length=%d",
        srcArray->GetLength(), srcPos, dstArray->GetLength(), dstPos, length);
    return;
  }

  // Handle primitive arrays.
  if (srcComponentType->IsPrimitive() || dstComponentType->IsPrimitive()) {
    // If one of the arrays holds a primitive type the other array must hold the exact same type.
    if (srcComponentType->IsPrimitive() != dstComponentType->IsPrimitive() || srcComponentType != dstComponentType) {
      std::string srcType(PrettyTypeOf(srcArray));
      std::string dstType(PrettyTypeOf(dstArray));
      ts.Self()->ThrowNewExceptionF("Ljava/lang/ArrayStoreException;",
          "Incompatible types: src=%s, dst=%s", srcType.c_str(), dstType.c_str());
      return;
    }

    size_t width = srcArray->GetClass()->GetComponentSize();
    uint8_t* dstBytes = reinterpret_cast<uint8_t*>(dstArray->GetRawData(width));
    const uint8_t* srcBytes = reinterpret_cast<const uint8_t*>(srcArray->GetRawData(width));

    switch (width) {
    case 1:
      memmove(dstBytes + dstPos, srcBytes + srcPos, length);
      break;
    case 2:
      move16(dstBytes + dstPos * 2, srcBytes + srcPos * 2, length * 2);
      break;
    case 4:
      move32(dstBytes + dstPos * 4, srcBytes + srcPos * 4, length * 4);
      break;
    case 8:
      // We don't need to guarantee atomicity of the entire 64-bit word.
      move32(dstBytes + dstPos * 8, srcBytes + srcPos * 8, length * 8);
      break;
    default:
      LOG(FATAL) << "Unknown primitive array type: " << PrettyTypeOf(srcArray);
    }

    return;
  }

  // Neither class is primitive. Are the types trivially compatible?
  const size_t width = sizeof(Object*);
  uint8_t* dstBytes = reinterpret_cast<uint8_t*>(dstArray->GetRawData(width));
  const uint8_t* srcBytes = reinterpret_cast<const uint8_t*>(srcArray->GetRawData(width));
  if (dstArray == srcArray || dstComponentType->IsAssignableFrom(srcComponentType)) {
    // Yes. Bulk copy.
    COMPILE_ASSERT(sizeof(width) == sizeof(uint32_t), move32_assumes_Object_references_are_32_bit);
    move32(dstBytes + dstPos * width, srcBytes + srcPos * width, length * width);
    Runtime::Current()->GetHeap()->WriteBarrierArray(dstArray, dstPos, length);
    return;
  }

  // The arrays are not trivially compatible. However, we may still be able to copy some or all of
  // the elements if the source objects are compatible (for example, copying an Object[] to
  // String[], the Objects being copied might actually be Strings).
  // We can't do a bulk move because that would introduce a check-use race condition, so we copy
  // elements one by one.

  // We already dealt with overlapping copies, so we don't need to cope with that case below.
  CHECK_NE(dstArray, srcArray);

  Object* const * srcObjects = reinterpret_cast<Object* const *>(srcBytes + srcPos * width);
  Object** dstObjects = reinterpret_cast<Object**>(dstBytes + dstPos * width);
  Class* dstClass = dstArray->GetClass()->GetComponentType();

  // We want to avoid redundant IsAssignableFrom checks where possible, so we cache a class that
  // we know is assignable to the destination array's component type.
  Class* lastAssignableElementClass = dstClass;

  Object* o = NULL;
  int i = 0;
  for (; i < length; ++i) {
    o = srcObjects[i];
    if (o != NULL) {
      Class* oClass = o->GetClass();
      if (lastAssignableElementClass == oClass) {
        dstObjects[i] = o;
      } else if (dstClass->IsAssignableFrom(oClass)) {
        lastAssignableElementClass = oClass;
        dstObjects[i] = o;
      } else {
        // Can't put this element into the array.
        break;
      }
    } else {
      dstObjects[i] = NULL;
    }
  }

  Runtime::Current()->GetHeap()->WriteBarrierArray(dstArray, dstPos, length);
  if (i != length) {
    std::string actualSrcType(PrettyTypeOf(o));
    std::string dstType(PrettyTypeOf(dstArray));
    ts.Self()->ThrowNewExceptionF("Ljava/lang/ArrayStoreException;",
        "source[%d] of type %s cannot be stored in destination array of type %s",
        srcPos + i, actualSrcType.c_str(), dstType.c_str());
    return;
  }
}

static jint System_identityHashCode(JNIEnv* env, jclass, jobject javaObject) {
  ScopedJniThreadState ts(env);
  Object* o = ts.Decode<Object*>(javaObject);
  return static_cast<jint>(reinterpret_cast<uintptr_t>(o));
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(System, arraycopy, "(Ljava/lang/Object;ILjava/lang/Object;II)V"),
  NATIVE_METHOD(System, identityHashCode, "(Ljava/lang/Object;)I"),
};

void register_java_lang_System(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/System");
}

}  // namespace art
