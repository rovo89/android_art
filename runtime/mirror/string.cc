/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "string-inl.h"

#include "arch/memcmp16.h"
#include "array.h"
#include "class-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "intern_table.h"
#include "object-inl.h"
#include "runtime.h"
#include "handle_scope-inl.h"
#include "thread.h"
#include "utf-inl.h"

namespace art {
namespace mirror {

// TODO: get global references for these
GcRoot<Class> String::java_lang_String_;

int32_t String::FastIndexOf(int32_t ch, int32_t start) {
  int32_t count = GetLength();
  if (start < 0) {
    start = 0;
  } else if (start > count) {
    start = count;
  }
  const uint16_t* chars = GetCharArray()->GetData() + GetOffset();
  const uint16_t* p = chars + start;
  const uint16_t* end = chars + count;
  while (p < end) {
    if (*p++ == ch) {
      return (p - 1) - chars;
    }
  }
  return -1;
}

void String::SetClass(Class* java_lang_String) {
  CHECK(java_lang_String_.IsNull());
  CHECK(java_lang_String != NULL);
  java_lang_String_ = GcRoot<Class>(java_lang_String);
}

void String::ResetClass() {
  CHECK(!java_lang_String_.IsNull());
  java_lang_String_ = GcRoot<Class>(nullptr);
}

int32_t String::ComputeHashCode() {
  const int32_t hash_code = ComputeUtf16Hash(GetCharArray(), GetOffset(), GetLength());
  SetHashCode(hash_code);
  return hash_code;
}

int32_t String::GetUtfLength() {
  return CountUtf8Bytes(GetCharArray()->GetData() + GetOffset(), GetLength());
}

String* String::AllocFromUtf16(Thread* self,
                               int32_t utf16_length,
                               const uint16_t* utf16_data_in,
                               int32_t hash_code) {
  CHECK(utf16_data_in != nullptr || utf16_length == 0);
  String* string = Alloc(self, utf16_length);
  if (UNLIKELY(string == nullptr)) {
    return nullptr;
  }
  CharArray* array = const_cast<CharArray*>(string->GetCharArray());
  if (UNLIKELY(array == nullptr)) {
    return nullptr;
  }
  memcpy(array->GetData(), utf16_data_in, utf16_length * sizeof(uint16_t));
  if (hash_code != 0) {
    DCHECK_EQ(hash_code, ComputeUtf16Hash(utf16_data_in, utf16_length));
    string->SetHashCode(hash_code);
  } else {
    string->ComputeHashCode();
  }
  return string;
}

String* String::AllocFromModifiedUtf8(Thread* self, const char* utf) {
  DCHECK(utf != nullptr);
  size_t char_count = CountModifiedUtf8Chars(utf);
  return AllocFromModifiedUtf8(self, char_count, utf);
}

String* String::AllocFromModifiedUtf8(Thread* self, int32_t utf16_length,
                                      const char* utf8_data_in) {
  String* string = Alloc(self, utf16_length);
  if (UNLIKELY(string == nullptr)) {
    return nullptr;
  }
  uint16_t* utf16_data_out =
      const_cast<uint16_t*>(string->GetCharArray()->GetData());
  ConvertModifiedUtf8ToUtf16(utf16_data_out, utf8_data_in);
  string->ComputeHashCode();
  return string;
}

String* String::Alloc(Thread* self, int32_t utf16_length) {
  StackHandleScope<1> hs(self);
  Handle<CharArray> array(hs.NewHandle(CharArray::Alloc(self, utf16_length)));
  if (UNLIKELY(array.Get() == nullptr)) {
    return nullptr;
  }
  return Alloc(self, array);
}

String* String::Alloc(Thread* self, Handle<CharArray> array) {
  // Hold reference in case AllocObject causes GC.
  String* string = down_cast<String*>(GetJavaLangString()->AllocObject(self));
  if (LIKELY(string != nullptr)) {
    string->SetArray(array.Get());
    string->SetCount(array->GetLength());
  }
  return string;
}

bool String::Equals(String* that) {
  if (this == that) {
    // Quick reference equality test
    return true;
  } else if (that == NULL) {
    // Null isn't an instanceof anything
    return false;
  } else if (this->GetLength() != that->GetLength()) {
    // Quick length inequality test
    return false;
  } else {
    // Note: don't short circuit on hash code as we're presumably here as the
    // hash code was already equal
    for (int32_t i = 0; i < that->GetLength(); ++i) {
      if (this->CharAt(i) != that->CharAt(i)) {
        return false;
      }
    }
    return true;
  }
}

bool String::Equals(const uint16_t* that_chars, int32_t that_offset, int32_t that_length) {
  if (this->GetLength() != that_length) {
    return false;
  } else {
    for (int32_t i = 0; i < that_length; ++i) {
      if (this->CharAt(i) != that_chars[that_offset + i]) {
        return false;
      }
    }
    return true;
  }
}

bool String::Equals(const char* modified_utf8) {
  for (int32_t i = 0; i < GetLength(); ++i) {
    uint16_t ch = GetUtf16FromUtf8(&modified_utf8);
    if (ch == '\0' || ch != CharAt(i)) {
      return false;
    }
  }
  return *modified_utf8 == '\0';
}

bool String::Equals(const StringPiece& modified_utf8) {
  const char* p = modified_utf8.data();
  for (int32_t i = 0; i < GetLength(); ++i) {
    uint16_t ch = GetUtf16FromUtf8(&p);
    if (ch != CharAt(i)) {
      return false;
    }
  }
  return true;
}

// Create a modified UTF-8 encoded std::string from a java/lang/String object.
std::string String::ToModifiedUtf8() {
  const uint16_t* chars = GetCharArray()->GetData() + GetOffset();
  size_t byte_count = GetUtfLength();
  std::string result(byte_count, static_cast<char>(0));
  ConvertUtf16ToModifiedUtf8(&result[0], chars, GetLength());
  return result;
}

int32_t String::CompareTo(String* rhs) {
  // Quick test for comparison of a string with itself.
  String* lhs = this;
  if (lhs == rhs) {
    return 0;
  }
  // TODO: is this still true?
  // The annoying part here is that 0x00e9 - 0xffff != 0x00ea,
  // because the interpreter converts the characters to 32-bit integers
  // *without* sign extension before it subtracts them (which makes some
  // sense since "char" is unsigned).  So what we get is the result of
  // 0x000000e9 - 0x0000ffff, which is 0xffff00ea.
  int32_t lhsCount = lhs->GetLength();
  int32_t rhsCount = rhs->GetLength();
  int32_t countDiff = lhsCount - rhsCount;
  int32_t minCount = (countDiff < 0) ? lhsCount : rhsCount;
  const uint16_t* lhsChars = lhs->GetCharArray()->GetData() + lhs->GetOffset();
  const uint16_t* rhsChars = rhs->GetCharArray()->GetData() + rhs->GetOffset();
  int32_t otherRes = MemCmp16(lhsChars, rhsChars, minCount);
  if (otherRes != 0) {
    return otherRes;
  }
  return countDiff;
}

void String::VisitRoots(RootCallback* callback, void* arg) {
  if (!java_lang_String_.IsNull()) {
    java_lang_String_.VisitRoot(callback, arg, 0, kRootStickyClass);
  }
}

}  // namespace mirror
}  // namespace art
