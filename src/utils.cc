// Copyright 2011 Google Inc. All Rights Reserved.
// Author: enh@google.com (Elliott Hughes)

#include "utils.h"

#include <pthread.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "UniquePtr.h"
#include "file.h"
#include "object.h"
#include "os.h"

#if defined(HAVE_PRCTL)
#include <sys/prctl.h>
#endif

namespace art {

bool ReadFileToString(const std::string& file_name, std::string* result) {
  UniquePtr<File> file(OS::OpenFile(file_name.c_str(), false));
  if (file.get() == NULL) {
    return false;
  }

  char buf[8 * KB];
  while (true) {
    int64_t n = file->Read(buf, sizeof(buf));
    if (n == -1) {
      return false;
    }
    if (n == 0) {
      return true;
    }
    result->append(buf, n);
  }
}

std::string GetIsoDate() {
  time_t now = time(NULL);
  struct tm tmbuf;
  struct tm* ptm = localtime_r(&now, &tmbuf);
  return StringPrintf("%04d-%02d-%02d %02d:%02d:%02d",
      ptm->tm_year + 1900, ptm->tm_mon+1, ptm->tm_mday,
      ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
}

std::string PrettyDescriptor(const String* java_descriptor) {
  if (java_descriptor == NULL) {
    return "null";
  }
  return PrettyDescriptor(java_descriptor->ToModifiedUtf8());
}

std::string PrettyDescriptor(const std::string& descriptor) {
  // Count the number of '['s to get the dimensionality.
  const char* c = descriptor.c_str();
  size_t dim = 0;
  while (*c == '[') {
    dim++;
    c++;
  }

  // Reference or primitive?
  if (*c == 'L') {
    // "[[La/b/C;" -> "a.b.C[][]".
    c++; // Skip the 'L'.
  } else {
    // "[[B" -> "byte[][]".
    // To make life easier, we make primitives look like unqualified
    // reference types.
    switch (*c) {
    case 'B': c = "byte;"; break;
    case 'C': c = "char;"; break;
    case 'D': c = "double;"; break;
    case 'F': c = "float;"; break;
    case 'I': c = "int;"; break;
    case 'J': c = "long;"; break;
    case 'S': c = "short;"; break;
    case 'Z': c = "boolean;"; break;
    default: return descriptor;
    }
  }

  // At this point, 'c' is a string of the form "fully/qualified/Type;"
  // or "primitive;". Rewrite the type with '.' instead of '/':
  std::string result;
  const char* p = c;
  while (*p != ';') {
    char ch = *p++;
    if (ch == '/') {
      ch = '.';
    }
    result.push_back(ch);
  }
  // ...and replace the semicolon with 'dim' "[]" pairs:
  while (dim--) {
    result += "[]";
  }
  return result;
}

std::string PrettyField(const Field* f, bool with_type) {
  if (f == NULL) {
    return "null";
  }
  std::string result;
  if (with_type) {
    result += PrettyDescriptor(f->GetType()->GetDescriptor());
    result += ' ';
  }
  result += PrettyDescriptor(f->GetDeclaringClass()->GetDescriptor());
  result += '.';
  result += f->GetName()->ToModifiedUtf8();
  return result;
}

std::string PrettyMethod(const Method* m, bool with_signature) {
  if (m == NULL) {
    return "null";
  }
  Class* c = m->GetDeclaringClass();
  std::string result(PrettyDescriptor(c->GetDescriptor()));
  result += '.';
  result += m->GetName()->ToModifiedUtf8();
  if (with_signature) {
    // TODO: iterate over the signature's elements and pass them all to
    // PrettyDescriptor? We'd need to pull out the return type specially, too.
    result += m->GetSignature()->ToModifiedUtf8();
  }
  return result;
}

std::string PrettyTypeOf(const Object* obj) {
  if (obj == NULL) {
    return "null";
  }
  if (obj->GetClass() == NULL) {
    return "(raw)";
  }
  std::string result(PrettyDescriptor(obj->GetClass()->GetDescriptor()));
  if (obj->IsClass()) {
    result += "<" + PrettyDescriptor(obj->AsClass()->GetDescriptor()) + ">";
  }
  return result;
}

std::string PrettyClass(const Class* c) {
  if (c == NULL) {
    return "null";
  }
  std::string result;
  result += "java.lang.Class<";
  result += PrettyDescriptor(c->GetDescriptor());
  result += ">";
  return result;
}

std::string MangleForJni(const std::string& s) {
  std::string result;
  size_t char_count = CountModifiedUtf8Chars(s.c_str());
  const char* cp = &s[0];
  for (size_t i = 0; i < char_count; ++i) {
    uint16_t ch = GetUtf16FromUtf8(&cp);
    if (ch == '$' || ch > 127) {
      StringAppendF(&result, "_0%04x", ch);
    } else {
      switch (ch) {
      case '_':
        result += "_1";
        break;
      case ';':
        result += "_2";
        break;
      case '[':
        result += "_3";
        break;
      case '/':
        result += "_";
        break;
      default:
        result.push_back(ch);
        break;
      }
    }
  }
  return result;
}

std::string DotToDescriptor(const char* class_name) {
  std::string descriptor(class_name);
  std::replace(descriptor.begin(), descriptor.end(), '.', '/');
  if (descriptor.length() > 0 && descriptor[0] != '[') {
    descriptor = "L" + descriptor + ";";
  }
  return descriptor;
}

std::string JniShortName(const Method* m) {
  Class* declaring_class = m->GetDeclaringClass();

  std::string class_name(declaring_class->GetDescriptor()->ToModifiedUtf8());
  // Remove the leading 'L' and trailing ';'...
  CHECK(class_name[0] == 'L') << class_name;
  CHECK(class_name[class_name.size() - 1] == ';') << class_name;
  class_name.erase(0, 1);
  class_name.erase(class_name.size() - 1, 1);

  std::string method_name(m->GetName()->ToModifiedUtf8());

  std::string short_name;
  short_name += "Java_";
  short_name += MangleForJni(class_name);
  short_name += "_";
  short_name += MangleForJni(method_name);
  return short_name;
}

std::string JniLongName(const Method* m) {
  std::string long_name;
  long_name += JniShortName(m);
  long_name += "__";

  std::string signature(m->GetSignature()->ToModifiedUtf8());
  signature.erase(0, 1);
  signature.erase(signature.begin() + signature.find(')'), signature.end());

  long_name += MangleForJni(signature);

  return long_name;
}

namespace {

// Helper for IsValidMemberNameUtf8(), a bit vector indicating valid low ascii.
uint32_t DEX_MEMBER_VALID_LOW_ASCII[4] = {
  0x00000000, // 00..1f low control characters; nothing valid
  0x03ff2010, // 20..3f digits and symbols; valid: '0'..'9', '$', '-'
  0x87fffffe, // 40..5f uppercase etc.; valid: 'A'..'Z', '_'
  0x07fffffe  // 60..7f lowercase etc.; valid: 'a'..'z'
};

// Helper for IsValidMemberNameUtf8(); do not call directly.
bool IsValidMemberNameUtf8Slow(const char** pUtf8Ptr) {
  /*
   * It's a multibyte encoded character. Decode it and analyze. We
   * accept anything that isn't (a) an improperly encoded low value,
   * (b) an improper surrogate pair, (c) an encoded '\0', (d) a high
   * control character, or (e) a high space, layout, or special
   * character (U+00a0, U+2000..U+200f, U+2028..U+202f,
   * U+fff0..U+ffff). This is all specified in the dex format
   * document.
   */

  uint16_t utf16 = GetUtf16FromUtf8(pUtf8Ptr);

  // Perform follow-up tests based on the high 8 bits.
  switch (utf16 >> 8) {
  case 0x00:
    // It's only valid if it's above the ISO-8859-1 high space (0xa0).
    return (utf16 > 0x00a0);
  case 0xd8:
  case 0xd9:
  case 0xda:
  case 0xdb:
    // It's a leading surrogate. Check to see that a trailing
    // surrogate follows.
    utf16 = GetUtf16FromUtf8(pUtf8Ptr);
    return (utf16 >= 0xdc00) && (utf16 <= 0xdfff);
  case 0xdc:
  case 0xdd:
  case 0xde:
  case 0xdf:
    // It's a trailing surrogate, which is not valid at this point.
    return false;
  case 0x20:
  case 0xff:
    // It's in the range that has spaces, controls, and specials.
    switch (utf16 & 0xfff8) {
    case 0x2000:
    case 0x2008:
    case 0x2028:
    case 0xfff0:
    case 0xfff8:
      return false;
    }
    break;
  }
  return true;
}

/* Return whether the pointed-at modified-UTF-8 encoded character is
 * valid as part of a member name, updating the pointer to point past
 * the consumed character. This will consume two encoded UTF-16 code
 * points if the character is encoded as a surrogate pair. Also, if
 * this function returns false, then the given pointer may only have
 * been partially advanced.
 */
bool IsValidMemberNameUtf8(const char** pUtf8Ptr) {
  uint8_t c = (uint8_t) **pUtf8Ptr;
  if (c <= 0x7f) {
    // It's low-ascii, so check the table.
    uint32_t wordIdx = c >> 5;
    uint32_t bitIdx = c & 0x1f;
    (*pUtf8Ptr)++;
    return (DEX_MEMBER_VALID_LOW_ASCII[wordIdx] & (1 << bitIdx)) != 0;
  }

  // It's a multibyte encoded character. Call a non-inline function
  // for the heavy lifting.
  return IsValidMemberNameUtf8Slow(pUtf8Ptr);
}

}  // namespace

bool IsValidClassName(const char* s, bool isClassName, bool dot_or_slash) {
  char separator = (dot_or_slash ? '.' : '/');

  int arrayCount = 0;
  while (*s == '[') {
    arrayCount++;
    s++;
  }

  if (arrayCount > 255) {
    // Arrays may have no more than 255 dimensions.
    return false;
  }

  if (arrayCount != 0) {
    /*
     * If we're looking at an array of some sort, then it doesn't
     * matter if what is being asked for is a class name; the
     * format looks the same as a type descriptor in that case, so
     * treat it as such.
     */
    isClassName = false;
  }

  if (!isClassName) {
    /*
     * We are looking for a descriptor. Either validate it as a
     * single-character primitive type, or continue on to check the
     * embedded class name (bracketed by "L" and ";").
     */
    switch (*(s++)) {
    case 'B':
    case 'C':
    case 'D':
    case 'F':
    case 'I':
    case 'J':
    case 'S':
    case 'Z':
      // These are all single-character descriptors for primitive types.
      return (*s == '\0');
    case 'V':
      // Non-array void is valid, but you can't have an array of void.
      return (arrayCount == 0) && (*s == '\0');
    case 'L':
      // Class name: Break out and continue below.
      break;
    default:
      // Oddball descriptor character.
      return false;
    }
  }

  /*
   * We just consumed the 'L' that introduces a class name as part
   * of a type descriptor, or we are looking for an unadorned class
   * name.
   */

  bool sepOrFirst = true; // first character or just encountered a separator.
  for (;;) {
    uint8_t c = (uint8_t) *s;
    switch (c) {
    case '\0':
      /*
       * Premature end for a type descriptor, but valid for
       * a class name as long as we haven't encountered an
       * empty component (including the degenerate case of
       * the empty string "").
       */
      return isClassName && !sepOrFirst;
    case ';':
      /*
       * Invalid character for a class name, but the
       * legitimate end of a type descriptor. In the latter
       * case, make sure that this is the end of the string
       * and that it doesn't end with an empty component
       * (including the degenerate case of "L;").
       */
      return !isClassName && !sepOrFirst && (s[1] == '\0');
    case '/':
    case '.':
      if (c != separator) {
        // The wrong separator character.
        return false;
      }
      if (sepOrFirst) {
        // Separator at start or two separators in a row.
        return false;
      }
      sepOrFirst = true;
      s++;
      break;
    default:
      if (!IsValidMemberNameUtf8(&s)) {
        return false;
      }
      sepOrFirst = false;
      break;
    }
  }
}

void Split(const std::string& s, char delim, std::vector<std::string>& result) {
  const char* p = s.data();
  const char* end = p + s.size();
  while (p != end) {
    if (*p == delim) {
      ++p;
    } else {
      const char* start = p;
      while (++p != end && *p != delim) {
        // Skip to the next occurrence of the delimiter.
      }
      result.push_back(std::string(start, p - start));
    }
  }
}

void SetThreadName(const char *threadName) {
  int hasAt = 0;
  int hasDot = 0;
  const char *s = threadName;
  while (*s) {
    if (*s == '.') {
      hasDot = 1;
    } else if (*s == '@') {
      hasAt = 1;
    }
    s++;
  }
  int len = s - threadName;
  if (len < 15 || hasAt || !hasDot) {
    s = threadName;
  } else {
    s = threadName + len - 15;
  }
#if defined(HAVE_ANDROID_PTHREAD_SETNAME_NP)
  /* pthread_setname_np fails rather than truncating long strings */
  char buf[16];       // MAX_TASK_COMM_LEN=16 is hard-coded into bionic
  strncpy(buf, s, sizeof(buf)-1);
  buf[sizeof(buf)-1] = '\0';
  errno = pthread_setname_np(pthread_self(), buf);
  if (errno != 0) {
    PLOG(WARNING) << "Unable to set the name of current thread to '" << buf << "'";
  }
#elif defined(HAVE_PRCTL)
  prctl(PR_SET_NAME, (unsigned long) s, 0, 0, 0);
#else
#error no implementation for SetThreadName
#endif
}

}  // namespace art

// Neither bionic nor glibc exposes gettid(2).
#define __KERNEL__
#include <linux/unistd.h>
namespace art {
#ifdef _syscall0
_syscall0(pid_t, GetTid)
#else
pid_t GetTid() { return syscall(__NR_gettid); }
#endif
}  // namespace art
#undef __KERNEL__
