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

#ifdef HAVE__MEMCMP16
// "count" is in 16-bit units.
extern "C" uint32_t __memcmp16(const uint16_t* s0, const uint16_t* s1, size_t count);
#define MemCmp16 __memcmp16
#else
uint32_t MemCmp16(const uint16_t* s0, const uint16_t* s1, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (s0[i] != s1[i]) {
      return static_cast<int32_t>(s0[i]) - static_cast<int32_t>(s1[i]);
    }
  }
  return 0;
}
#endif

namespace art {

static jint String_compareTo(JNIEnv* env, jobject javaThis, jobject javaRhs) {
  ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
  String* lhs = Decode<String*>(env, javaThis);
  String* rhs = Decode<String*>(env, javaRhs);

  if (rhs == NULL) {
    Thread::Current()->ThrowNewException("Ljava/lang/NullPointerException;", "rhs == null");
    return -1;
  }

  // Quick test for comparison of a string with itself.
  if (lhs == rhs) {
    return 0;
  }

  // TODO: is this still true?
  // The annoying part here is that 0x00e9 - 0xffff != 0x00ea,
  // because the interpreter converts the characters to 32-bit integers
  // *without* sign extension before it subtracts them (which makes some
  // sense since "char" is unsigned).  So what we get is the result of
  // 0x000000e9 - 0x0000ffff, which is 0xffff00ea.
  int lhsCount = lhs->GetLength();
  int rhsCount = rhs->GetLength();
  int countDiff = lhsCount - rhsCount;
  int minCount = (countDiff < 0) ? lhsCount : rhsCount;
  const uint16_t* lhsChars = lhs->GetCharArray()->GetData() + lhs->GetOffset();
  const uint16_t* rhsChars = rhs->GetCharArray()->GetData() + rhs->GetOffset();
  int otherRes = MemCmp16(lhsChars, rhsChars, minCount);
  if (otherRes != 0) {
    return otherRes;
  }
  return countDiff;
}

/*
 * public int indexOf(int c, int start)
 *
 * Scan forward through the string for a matching character.
 * The character must be <= 0xffff; this method does not handle supplementary
 * characters.
 *
 * Determine the index of the first character matching "ch".  The string
 * to search is described by "chars", "offset", and "count".
 *
 * The character must be <= 0xffff. Supplementary characters are handled in
 * Java.
 *
 * The "start" parameter must be clamped to [0..count].
 *
 * Returns -1 if no match is found.
 */
static jint String_fastIndexOf(JNIEnv* env, jobject javaThis, jint ch, jint start) {
  String* s = Decode<String*>(env, javaThis);
  const uint16_t* chars = s->GetCharArray()->GetData() + s->GetOffset();

  if (start < 0) {
    start = 0;
  }

  /* 16-bit loop, slightly better on ARM */
  const uint16_t* ptr = chars + start;
  const uint16_t* endPtr = chars + s->GetLength();
  while (ptr < endPtr) {
    if (*ptr++ == ch) {
      return (ptr-1) - chars;
    }
  }

  return -1;
}

static jstring String_intern(JNIEnv* env, jobject javaThis) {
  String* s = Decode<String*>(env, javaThis);
  String* result = s->Intern();
  return AddLocalReference<jstring>(env, result);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(String, compareTo, "(Ljava/lang/String;)I"),
  NATIVE_METHOD(String, fastIndexOf, "(II)I"),
  NATIVE_METHOD(String, intern, "()Ljava/lang/String;"),
};

void register_java_lang_String(JNIEnv* env) {
  jniRegisterNativeMethods(env, "java/lang/String", gMethods, NELEM(gMethods));
}

}  // namespace art
