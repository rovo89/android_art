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

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

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
  DCHECK((((uintptr_t) dst | (uintptr_t) src | n) & 0x01) == 0);

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
  DCHECK((((uintptr_t) dst | (uintptr_t) src | n) & 0x03) == 0);

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

namespace {

void ThrowArrayStoreException_NotAnArray(const char* identifier, Object* array) {
  std::string actualType(PrettyType(array));
  Thread::Current()->ThrowNewException("Ljava/lang/ArrayStoreException;", "%s is not an array: %s", identifier, actualType.c_str());
}

void System_arraycopy(JNIEnv* env, jclass, jobject javaSrc, jint srcPos, jobject javaDst, jint dstPos, jint length) {
  Thread* self = Thread::Current();

  // Null pointer checks.
  if (javaSrc == NULL) {
    self->ThrowNewException("Ljava/lang/NullPointerException;", "src == null");
    return;
  }
  if (javaDst == NULL) {
    self->ThrowNewException("Ljava/lang/NullPointerException;", "dst == null");
    return;
  }

  // Make sure source and destination are both arrays.
  Object* srcObject = Decode<Object*>(env, javaSrc);
  Object* dstObject = Decode<Object*>(env, javaDst);
  if (!srcObject->IsArrayInstance()) {
    ThrowArrayStoreException_NotAnArray("src", srcObject);
    return;
  }
  if (!dstObject->IsArrayInstance()) {
    ThrowArrayStoreException_NotAnArray("dst", dstObject);
    return;
  }
  Array* srcArray = srcObject->AsArray();
  Array* dstArray = dstObject->AsArray();
  Class* srcComponentType = srcArray->GetClass()->GetComponentType();
  Class* dstComponentType = dstArray->GetClass()->GetComponentType();

  // Bounds checking.
  if (srcPos < 0 || dstPos < 0 || length < 0 || srcPos > srcArray->GetLength() - length || dstPos > dstArray->GetLength() - length) {
    self->ThrowNewException("Ljava/lang/ArrayIndexOutOfBoundsException;",
        "src.length=%d srcPos=%d dst.length=%d dstPos=%d length=%d",
        srcArray->GetLength(), srcPos, dstArray->GetLength(), dstPos, length);
    return;
  }

  uint8_t* dstBytes = reinterpret_cast<uint8_t*>(dstArray->GetRawData());
  const uint8_t* srcBytes = reinterpret_cast<const uint8_t*>(srcArray->GetRawData());

  // Handle primitive arrays.
  if (srcComponentType->IsPrimitive() || dstComponentType->IsPrimitive()) {
    // If one of the arrays holds a primitive type the other array must hold the exact same type.
    if (srcComponentType->IsPrimitive() != dstComponentType->IsPrimitive() || srcComponentType != dstComponentType) {
      std::string srcType(PrettyType(srcArray));
      std::string dstType(PrettyType(dstArray));
      self->ThrowNewException("Ljava/lang/ArrayStoreException;",
          "Incompatible types: src=%s, dst=%s", srcType.c_str(), dstType.c_str());
      return;
    }

    switch (srcArray->GetClass()->GetComponentSize()) {
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
      LOG(FATAL) << "Unknown primitive array type: " << PrettyType(srcArray);
    }

    return;
  }

  // Neither class is primitive. Are the types trivially compatible?
  const int width = sizeof(Object*);
  bool sameDimensions = srcArray->GetClass()->array_rank_ == dstArray->GetClass()->array_rank_;
  if (sameDimensions && srcComponentType->InstanceOf(dstComponentType)) {
    // Yes. Bulk copy.
    move32(dstBytes + dstPos * width, srcBytes + srcPos * width, length * width);
    UNIMPLEMENTED(WARNING) << "write barriers in System.arraycopy";
    //dvmWriteBarrierArray(dstArray, dstPos, dstPos + length);
    return;
  }

  // The arrays are not trivially compatible.  However, we
  // may still be able to do this if the destination object is
  // compatible (e.g. copy Object[] to String[], but the Object
  // being copied is actually a String).  We need to copy elements
  // one by one until something goes wrong.
  //
  // Because of overlapping moves, what we really want to do
  // is compare the types and count up how many we can move,
  // then call move32() to shift the actual data.  If we just
  // start from the front we could do a smear rather than a move.

  // TODO: this idea is flawed. a malicious caller could exploit the check-use
  // race by modifying the source array after we check but before we copy,
  // and cause us to copy incompatible elements.

  Object** srcObj = reinterpret_cast<ObjectArray<Object>*>(srcArray)->GetData() + srcPos;
  Class* dstClass = dstArray->GetClass();

  Class* initialElementClass = NULL;
  if (length > 0 && srcObj[0] != NULL) {
    initialElementClass = srcObj[0]->GetClass();
    if (!Class::CanPutArrayElementNoThrow(initialElementClass, dstClass)) {
      initialElementClass = NULL;
    }
  }

  int copyCount;
  for (copyCount = 0; copyCount < length; copyCount++) {
    if (srcObj[copyCount] != NULL && srcObj[copyCount]->GetClass() != initialElementClass && !Class::CanPutArrayElementNoThrow(srcObj[copyCount]->GetClass(), dstClass)) {
      // Can't put this element into the array.
      // We'll copy up to this point, then throw.
      break;
    }
  }

  move32(dstBytes + dstPos * width, srcBytes + srcPos * width, copyCount * width);
  UNIMPLEMENTED(WARNING) << "write barriers in System.arraycopy";
  //dvmWriteBarrierArray(dstArray, 0, copyCount);
  if (copyCount != length) {
    std::string actualSrcType(PrettyType(srcObj[copyCount]));
    std::string dstType(PrettyType(dstArray));
    self->ThrowNewException("Ljava/lang/ArrayStoreException;",
        "source[%d] of type %s cannot be stored in destination array of type %s",
        srcPos + copyCount, actualSrcType.c_str(), dstType.c_str());
    return;
  }
}

jint System_identityHashCode(JNIEnv* env, jclass, jobject javaObject) {
  Object* o = Decode<Object*>(env, javaObject);
  return static_cast<jint>(reinterpret_cast<uintptr_t>(o));
}

JNINativeMethod gMethods[] = {
  NATIVE_METHOD(System, arraycopy, "(Ljava/lang/Object;ILjava/lang/Object;II)V"),
  NATIVE_METHOD(System, identityHashCode, "(Ljava/lang/Object;)I"),
};

}  // namespace

void register_java_lang_System(JNIEnv* env) {
    jniRegisterNativeMethods(env, "java/lang/System", gMethods, NELEM(gMethods));
}

}  // namespace art
