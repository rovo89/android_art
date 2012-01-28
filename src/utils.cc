// Copyright 2011 Google Inc. All Rights Reserved.
// Author: enh@google.com (Elliott Hughes)

#include "utils.h"

#include <pthread.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "UniquePtr.h"
#include "class_loader.h"
#include "file.h"
#include "object.h"
#include "object_utils.h"
#include "os.h"

#if !defined(HAVE_POSIX_CLOCKS)
#include <sys/time.h>
#endif

#if defined(HAVE_PRCTL)
#include <sys/prctl.h>
#endif

#if defined(__linux__)
#include <linux/unistd.h>
#endif

namespace art {

pid_t GetTid() {
#if defined(__APPLE__)
  // Mac OS doesn't have gettid(2).
  return getpid();
#else
  // Neither bionic nor glibc exposes gettid(2).
  return syscall(__NR_gettid);
#endif
}

bool ReadFileToString(const std::string& file_name, std::string* result) {
  UniquePtr<File> file(OS::OpenFile(file_name.c_str(), false));
  if (file.get() == NULL) {
    return false;
  }

  std::vector<char> buf(8 * KB);
  while (true) {
    int64_t n = file->Read(&buf[0], buf.size());
    if (n == -1) {
      return false;
    }
    if (n == 0) {
      return true;
    }
    result->append(&buf[0], n);
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

uint64_t MilliTime() {
#if defined(HAVE_POSIX_CLOCKS)
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000LL + now.tv_nsec / 1000000LL;
#else
  struct timeval now;
  gettimeofday(&now, NULL);
  return static_cast<uint64_t>(now.tv_sec) * 1000LL + now.tv_usec / 1000LL;
#endif
}

uint64_t MicroTime() {
#if defined(HAVE_POSIX_CLOCKS)
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000000LL + now.tv_nsec / 1000LL;
#else
  struct timeval now;
  gettimeofday(&now, NULL);
  return static_cast<uint64_t>(now.tv_sec) * 1000000LL + now.tv_usec * 1000LL;
#endif
}

uint64_t NanoTime() {
#if defined(HAVE_POSIX_CLOCKS)
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
#else
  struct timeval now;
  gettimeofday(&now, NULL);
  return static_cast<uint64_t>(now.tv_sec) * 1000000000LL + now.tv_usec * 1000LL;
#endif
}

uint64_t ThreadCpuMicroTime() {
#if defined(HAVE_POSIX_CLOCKS)
  struct timespec now;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000000LL + now.tv_nsec / 1000LL;
#else
  UNIMPLEMENTED(WARNING);
  return -1;
#endif
}

std::string PrettyDescriptor(const String* java_descriptor) {
  if (java_descriptor == NULL) {
    return "null";
  }
  return PrettyDescriptor(java_descriptor->ToModifiedUtf8());
}

std::string PrettyDescriptor(const Class* klass) {
  if (klass == NULL) {
    return "null";
  }
  return PrettyDescriptor(ClassHelper(klass).GetDescriptor());
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

std::string PrettyDescriptor(Primitive::Type type) {
  std::string descriptor_string(Primitive::Descriptor(type));
  return PrettyDescriptor(descriptor_string);
}

std::string PrettyField(const Field* f, bool with_type) {
  if (f == NULL) {
    return "null";
  }
  FieldHelper fh(f);
  std::string result;
  if (with_type) {
    result += PrettyDescriptor(fh.GetTypeDescriptor());
    result += ' ';
  }
  result += PrettyDescriptor(fh.GetDeclaringClassDescriptor());
  result += '.';
  result += fh.GetName();
  return result;
}

std::string PrettyMethod(const Method* m, bool with_signature) {
  if (m == NULL) {
    return "null";
  }
  MethodHelper mh(m);
  std::string result(PrettyDescriptor(mh.GetDeclaringClassDescriptor()));
  result += '.';
  result += mh.GetName();
  if (with_signature) {
    // TODO: iterate over the signature's elements and pass them all to
    // PrettyDescriptor? We'd need to pull out the return type specially, too.
    result += mh.GetSignature();
  }
  return result;
}

std::string PrettyMethod(uint32_t method_idx, const DexFile& dex_file, bool with_signature) {
  const DexFile::MethodId& method_id = dex_file.GetMethodId(method_idx);
  std::string result(PrettyDescriptor(dex_file.GetMethodDeclaringClassDescriptor(method_id)));
  result += '.';
  result += dex_file.GetMethodName(method_id);
  if (with_signature) {
    // TODO: iterate over the signature's elements and pass them all to
    // PrettyDescriptor? We'd need to pull out the return type specially, too.
    result += dex_file.GetMethodSignature(method_id);
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
  ClassHelper kh(obj->GetClass());
  std::string result(PrettyDescriptor(kh.GetDescriptor()));
  if (obj->IsClass()) {
    kh.ChangeClass(obj->AsClass());
    result += "<" + PrettyDescriptor(kh.GetDescriptor()) + ">";
  }
  return result;
}

std::string PrettyClass(const Class* c) {
  if (c == NULL) {
    return "null";
  }
  std::string result;
  result += "java.lang.Class<";
  result += PrettyDescriptor(c);
  result += ">";
  return result;
}

std::string PrettyClassAndClassLoader(const Class* c) {
  if (c == NULL) {
    return "null";
  }
  std::string result;
  result += "java.lang.Class<";
  result += PrettyDescriptor(c);
  result += ",";
  result += PrettyTypeOf(c->GetClassLoader());
  // TODO: add an identifying hash value for the loader
  result += ">";
  return result;
}

std::string PrettySize(size_t size_in_bytes) {
  if ((size_in_bytes / GB) * GB == size_in_bytes) {
    return StringPrintf("%zdGB", size_in_bytes / GB);
  } else if ((size_in_bytes / MB) * MB == size_in_bytes) {
    return StringPrintf("%zdMB", size_in_bytes / MB);
  } else if ((size_in_bytes / KB) * KB == size_in_bytes) {
    return StringPrintf("%zdKiB", size_in_bytes / KB);
  } else {
    return StringPrintf("%zdB", size_in_bytes);
  }
}

std::string PrettyDuration(uint64_t nano_duration) {
  if (nano_duration == 0) {
    return "0";
  } else {
    const uint64_t one_sec = 1000 * 1000 * 1000;
    const uint64_t one_ms  = 1000 * 1000;
    const uint64_t one_us  = 1000;
    const char* unit;
    uint64_t divisor;
    uint32_t zero_fill;
    if (nano_duration >= one_sec) {
      unit = "s";
      divisor = one_sec;
      zero_fill = 9;
    } else if(nano_duration >= one_ms) {
      unit = "ms";
      divisor = one_ms;
      zero_fill = 6;
    } else if(nano_duration >= one_us) {
      unit = "us";
      divisor = one_us;
      zero_fill = 3;
    } else {
      unit = "ns";
      divisor = 1;
      zero_fill = 0;
    }
    uint64_t whole_part = nano_duration / divisor;
    uint64_t fractional_part = nano_duration % divisor;
    if (fractional_part == 0) {
      return StringPrintf("%llu%s", whole_part, unit);
    } else {
      while ((fractional_part % 1000) == 0) {
        zero_fill -= 3;
        fractional_part /= 1000;
      }
      if (zero_fill == 3) {
        return StringPrintf("%llu.%03llu%s", whole_part, fractional_part, unit);
      } else if (zero_fill == 6) {
        return StringPrintf("%llu.%06llu%s", whole_part, fractional_part, unit);
      } else {
        return StringPrintf("%llu.%09llu%s", whole_part, fractional_part, unit);
      }
    }
  }
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

std::string DescriptorToDot(const StringPiece& descriptor) {
  DCHECK_EQ(descriptor[0], 'L');
  DCHECK_EQ(descriptor[descriptor.size()-1], ';');
  std::string dot(descriptor.substr(1, descriptor.size() - 2).ToString());
  std::replace(dot.begin(), dot.end(), '/', '.');
  return dot;
}

std::string JniShortName(const Method* m) {
  MethodHelper mh(m);
  std::string class_name(mh.GetDeclaringClassDescriptor());
  // Remove the leading 'L' and trailing ';'...
  CHECK_EQ(class_name[0], 'L') << class_name;
  CHECK_EQ(class_name[class_name.size() - 1], ';') << class_name;
  class_name.erase(0, 1);
  class_name.erase(class_name.size() - 1, 1);

  std::string method_name(mh.GetName());

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

  std::string signature(MethodHelper(m).GetSignature());
  signature.erase(0, 1);
  signature.erase(signature.begin() + signature.find(')'), signature.end());

  long_name += MangleForJni(signature);

  return long_name;
}

// Helper for IsValidPartOfMemberNameUtf8(), a bit vector indicating valid low ascii.
uint32_t DEX_MEMBER_VALID_LOW_ASCII[4] = {
  0x00000000, // 00..1f low control characters; nothing valid
  0x03ff2010, // 20..3f digits and symbols; valid: '0'..'9', '$', '-'
  0x87fffffe, // 40..5f uppercase etc.; valid: 'A'..'Z', '_'
  0x07fffffe  // 60..7f lowercase etc.; valid: 'a'..'z'
};

// Helper for IsValidPartOfMemberNameUtf8(); do not call directly.
bool IsValidPartOfMemberNameUtf8Slow(const char** pUtf8Ptr) {
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
bool IsValidPartOfMemberNameUtf8(const char** pUtf8Ptr) {
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
  return IsValidPartOfMemberNameUtf8Slow(pUtf8Ptr);
}

bool IsValidMemberName(const char* s) {
  bool angle_name = false;

  switch(*s) {
    case '\0':
      // The empty string is not a valid name.
      return false;
    case '<':
      angle_name = true;
      s++;
      break;
  }

  while (true) {
    switch (*s) {
      case '\0':
        return !angle_name;
      case '>':
        return angle_name && s[1] == '\0';
    }

    if (!IsValidPartOfMemberNameUtf8(&s)) {
      return false;
    }
  }
}

enum ClassNameType { kName, kDescriptor };
bool IsValidClassName(const char* s, ClassNameType type, char separator) {
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
    type = kDescriptor;
  }

  if (type == kDescriptor) {
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
      return (type == kName) && !sepOrFirst;
    case ';':
      /*
       * Invalid character for a class name, but the
       * legitimate end of a type descriptor. In the latter
       * case, make sure that this is the end of the string
       * and that it doesn't end with an empty component
       * (including the degenerate case of "L;").
       */
      return (type == kDescriptor) && !sepOrFirst && (s[1] == '\0');
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
      if (!IsValidPartOfMemberNameUtf8(&s)) {
        return false;
      }
      sepOrFirst = false;
      break;
    }
  }
}

bool IsValidBinaryClassName(const char* s) {
  return IsValidClassName(s, kName, '.');
}

bool IsValidJniClassName(const char* s) {
  return IsValidClassName(s, kName, '/');
}

bool IsValidDescriptor(const char* s) {
  return IsValidClassName(s, kDescriptor, '/');
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

void SetThreadName(const char* threadName) {
  int hasAt = 0;
  int hasDot = 0;
  const char* s = threadName;
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
  UNIMPLEMENTED(WARNING) << threadName;
#endif
}

void GetTaskStats(pid_t tid, int& utime, int& stime, int& task_cpu) {
  utime = stime = task_cpu = 0;
  std::string stats;
  if (!ReadFileToString(StringPrintf("/proc/self/task/%d/stat", GetTid()).c_str(), &stats)) {
    return;
  }
  // Skip the command, which may contain spaces.
  stats = stats.substr(stats.find(')') + 2);
  // Extract the three fields we care about.
  std::vector<std::string> fields;
  Split(stats, ' ', fields);
  utime = strtoull(fields[11].c_str(), NULL, 10);
  stime = strtoull(fields[12].c_str(), NULL, 10);
  task_cpu = strtoull(fields[36].c_str(), NULL, 10);
}

std::string GetArtCacheOrDie() {
  const char* data_root = getenv("ANDROID_DATA");
  if (data_root == NULL) {
    if (OS::DirectoryExists("/data")) {
      data_root = "/data";
    } else {
      data_root = "/tmp";
    }
  }
  if (!OS::DirectoryExists(data_root)) {
    LOG(FATAL) << "Failed to find ANDROID_DATA directory " << data_root;
    return "";
  }

  std::string art_cache(StringPrintf("%s/art-cache", data_root));

  if (!OS::DirectoryExists(art_cache.c_str())) {
    if (StringPiece(art_cache).starts_with("/tmp/")) {
      int result = mkdir(art_cache.c_str(), 0700);
      if (result != 0) {
        LOG(FATAL) << "Failed to create art-cache directory " << art_cache;
        return "";
      }
    } else {
      LOG(FATAL) << "Failed to find art-cache directory " << art_cache;
      return "";
    }
  }
  return art_cache;
}

std::string GetArtCacheFilenameOrDie(const std::string& location) {
  std::string art_cache(GetArtCacheOrDie());
  CHECK_EQ(location[0], '/');
  std::string cache_file(location, 1); // skip leading slash
  std::replace(cache_file.begin(), cache_file.end(), '/', '@');
  return art_cache + "/" + cache_file;
}

bool IsValidZipFilename(const std::string& filename) {
  if (filename.size() < 4) {
    return false;
  }
  std::string suffix(filename.substr(filename.size() - 4));
  return (suffix == ".zip" || suffix == ".jar" || suffix == ".apk");
}

bool IsValidDexFilename(const std::string& filename) {
  if (filename.size() < 4) {
    return false;
  }
  std::string suffix(filename.substr(filename.size() - 4));
  return (suffix == ".dex");
}

}  // namespace art
