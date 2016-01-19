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

#include "dex_file_verifier.h"

#include "sys/mman.h"
#include "zlib.h"
#include <functional>
#include <memory>

#include "base/unix_file/fd_file.h"
#include "base/bit_utils.h"
#include "base/macros.h"
#include "common_runtime_test.h"
#include "dex_file-inl.h"
#include "leb128.h"
#include "scoped_thread_state_change.h"
#include "thread-inl.h"

namespace art {

static const uint8_t kBase64Map[256] = {
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255,  62, 255, 255, 255,  63,
  52,  53,  54,  55,  56,  57,  58,  59,  60,  61, 255, 255,
  255, 254, 255, 255, 255,   0,   1,   2,   3,   4,   5,   6,
    7,   8,   9,  10,  11,  12,  13,  14,  15,  16,  17,  18,  // NOLINT
   19,  20,  21,  22,  23,  24,  25, 255, 255, 255, 255, 255,  // NOLINT
  255,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,
   37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  // NOLINT
   49,  50,  51, 255, 255, 255, 255, 255, 255, 255, 255, 255,  // NOLINT
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255
};

static inline uint8_t* DecodeBase64(const char* src, size_t* dst_size) {
  std::vector<uint8_t> tmp;
  uint32_t t = 0, y = 0;
  int g = 3;
  for (size_t i = 0; src[i] != '\0'; ++i) {
    uint8_t c = kBase64Map[src[i] & 0xFF];
    if (c == 255) continue;
    // the final = symbols are read and used to trim the remaining bytes
    if (c == 254) {
      c = 0;
      // prevent g < 0 which would potentially allow an overflow later
      if (--g < 0) {
        *dst_size = 0;
        return nullptr;
      }
    } else if (g != 3) {
      // we only allow = to be at the end
      *dst_size = 0;
      return nullptr;
    }
    t = (t << 6) | c;
    if (++y == 4) {
      tmp.push_back((t >> 16) & 255);
      if (g > 1) {
        tmp.push_back((t >> 8) & 255);
      }
      if (g > 2) {
        tmp.push_back(t & 255);
      }
      y = t = 0;
    }
  }
  if (y != 0) {
    *dst_size = 0;
    return nullptr;
  }
  std::unique_ptr<uint8_t[]> dst(new uint8_t[tmp.size()]);
  if (dst_size != nullptr) {
    *dst_size = tmp.size();
  } else {
    *dst_size = 0;
  }
  std::copy(tmp.begin(), tmp.end(), dst.get());
  return dst.release();
}

static void FixUpChecksum(uint8_t* dex_file) {
  DexFile::Header* header = reinterpret_cast<DexFile::Header*>(dex_file);
  uint32_t expected_size = header->file_size_;
  uint32_t adler_checksum = adler32(0L, Z_NULL, 0);
  const uint32_t non_sum = sizeof(DexFile::Header::magic_) + sizeof(DexFile::Header::checksum_);
  const uint8_t* non_sum_ptr = dex_file + non_sum;
  adler_checksum = adler32(adler_checksum, non_sum_ptr, expected_size - non_sum);
  header->checksum_ = adler_checksum;
}

// Custom deleter. Necessary to clean up the memory we use (to be able to mutate).
struct DexFileDeleter {
  void operator()(DexFile* in) {
    if (in != nullptr) {
      delete[] in->Begin();
      delete in;
    }
  }
};

using DexFileUniquePtr = std::unique_ptr<DexFile, DexFileDeleter>;

class DexFileVerifierTest : public CommonRuntimeTest {
 protected:
  void VerifyModification(const char* dex_file_base64_content,
                          const char* location,
                          std::function<void(DexFile*)> f,
                          const char* expected_error) {
    DexFileUniquePtr dex_file(WrapAsDexFile(dex_file_base64_content));
    f(dex_file.get());
    FixUpChecksum(const_cast<uint8_t*>(dex_file->Begin()));

    std::string error_msg;
    bool success = DexFileVerifier::Verify(dex_file.get(),
                                           dex_file->Begin(),
                                           dex_file->Size(),
                                           location,
                                           &error_msg);
    if (expected_error == nullptr) {
      EXPECT_TRUE(success) << error_msg;
    } else {
      EXPECT_FALSE(success) << "Expected " << expected_error;
      if (!success) {
        EXPECT_NE(error_msg.find(expected_error), std::string::npos) << error_msg;
      }
    }
  }

 private:
  static DexFile* WrapAsDexFile(const char* dex_file_content_in_base_64) {
    // Decode base64.
    size_t length;
    uint8_t* dex_bytes = DecodeBase64(dex_file_content_in_base_64, &length);
    CHECK(dex_bytes != nullptr);
    return new DexFile(dex_bytes, length, "tmp", 0, nullptr, nullptr);
  }
};

static std::unique_ptr<const DexFile> OpenDexFileBase64(const char* base64,
                                                        const char* location,
                                                        std::string* error_msg) {
  // decode base64
  CHECK(base64 != nullptr);
  size_t length;
  std::unique_ptr<uint8_t[]> dex_bytes(DecodeBase64(base64, &length));
  CHECK(dex_bytes.get() != nullptr);

  // write to provided file
  std::unique_ptr<File> file(OS::CreateEmptyFile(location));
  CHECK(file.get() != nullptr);
  if (!file->WriteFully(dex_bytes.get(), length)) {
    PLOG(FATAL) << "Failed to write base64 as dex file";
  }
  if (file->FlushCloseOrErase() != 0) {
    PLOG(FATAL) << "Could not flush and close test file.";
  }
  file.reset();

  // read dex file
  ScopedObjectAccess soa(Thread::Current());
  std::vector<std::unique_ptr<const DexFile>> tmp;
  bool success = DexFile::Open(location, location, error_msg, &tmp);
  CHECK(success) << error_msg;
  EXPECT_EQ(1U, tmp.size());
  std::unique_ptr<const DexFile> dex_file = std::move(tmp[0]);
  EXPECT_EQ(PROT_READ, dex_file->GetPermissions());
  EXPECT_TRUE(dex_file->IsReadOnly());
  return dex_file;
}

// For reference.
static const char kGoodTestDex[] =
    "ZGV4CjAzNQDrVbyVkxX1HljTznNf95AglkUAhQuFtmKkAgAAcAAAAHhWNBIAAAAAAAAAAAQCAAAN"
    "AAAAcAAAAAYAAACkAAAAAgAAALwAAAABAAAA1AAAAAQAAADcAAAAAQAAAPwAAACIAQAAHAEAAFoB"
    "AABiAQAAagEAAIEBAACVAQAAqQEAAL0BAADDAQAAzgEAANEBAADVAQAA2gEAAN8BAAABAAAAAgAA"
    "AAMAAAAEAAAABQAAAAgAAAAIAAAABQAAAAAAAAAJAAAABQAAAFQBAAAEAAEACwAAAAAAAAAAAAAA"
    "AAAAAAoAAAABAAEADAAAAAIAAAAAAAAAAAAAAAEAAAACAAAAAAAAAAcAAAAAAAAA8wEAAAAAAAAB"
    "AAEAAQAAAOgBAAAEAAAAcBADAAAADgACAAAAAgAAAO0BAAAIAAAAYgAAABoBBgBuIAIAEAAOAAEA"
    "AAADAAY8aW5pdD4ABkxUZXN0OwAVTGphdmEvaW8vUHJpbnRTdHJlYW07ABJMamF2YS9sYW5nL09i"
    "amVjdDsAEkxqYXZhL2xhbmcvU3RyaW5nOwASTGphdmEvbGFuZy9TeXN0ZW07AARUZXN0AAlUZXN0"
    "LmphdmEAAVYAAlZMAANmb28AA291dAAHcHJpbnRsbgABAAcOAAMABw54AAAAAgAAgYAEnAIBCbQC"
    "AAAADQAAAAAAAAABAAAAAAAAAAEAAAANAAAAcAAAAAIAAAAGAAAApAAAAAMAAAACAAAAvAAAAAQA"
    "AAABAAAA1AAAAAUAAAAEAAAA3AAAAAYAAAABAAAA/AAAAAEgAAACAAAAHAEAAAEQAAABAAAAVAEA"
    "AAIgAAANAAAAWgEAAAMgAAACAAAA6AEAAAAgAAABAAAA8wEAAAAQAAABAAAABAIAAA==";

TEST_F(DexFileVerifierTest, GoodDex) {
  ScratchFile tmp;
  std::string error_msg;
  std::unique_ptr<const DexFile> raw(OpenDexFileBase64(kGoodTestDex, tmp.GetFilename().c_str(),
                                                       &error_msg));
  ASSERT_TRUE(raw.get() != nullptr) << error_msg;
}

TEST_F(DexFileVerifierTest, MethodId) {
  // Class idx error.
  VerifyModification(
      kGoodTestDex,
      "method_id_class_idx",
      [](DexFile* dex_file) {
        DexFile::MethodId* method_id = const_cast<DexFile::MethodId*>(&dex_file->GetMethodId(0));
        method_id->class_idx_ = 0xFF;
      },
      "could not find declaring class for direct method index 0");

  // Proto idx error.
  VerifyModification(
      kGoodTestDex,
      "method_id_proto_idx",
      [](DexFile* dex_file) {
        DexFile::MethodId* method_id = const_cast<DexFile::MethodId*>(&dex_file->GetMethodId(0));
        method_id->proto_idx_ = 0xFF;
      },
      "inter_method_id_item proto_idx");

  // Name idx error.
  VerifyModification(
      kGoodTestDex,
      "method_id_name_idx",
      [](DexFile* dex_file) {
        DexFile::MethodId* method_id = const_cast<DexFile::MethodId*>(&dex_file->GetMethodId(0));
        method_id->name_idx_ = 0xFF;
      },
      "String index not available for method flags verification");
}

// Method flags test class generated from the following smali code. The declared-synchronized
// flags are there to enforce a 3-byte uLEB128 encoding so we don't have to relayout
// the code, but we need to remove them before doing tests.
//
// .class public LMethodFlags;
// .super Ljava/lang/Object;
//
// .method public static constructor <clinit>()V
// .registers 1
//     return-void
// .end method
//
// .method public constructor <init>()V
// .registers 1
//     return-void
// .end method
//
// .method private declared-synchronized foo()V
// .registers 1
//     return-void
// .end method
//
// .method public declared-synchronized bar()V
// .registers 1
//     return-void
// .end method

static const char kMethodFlagsTestDex[] =
    "ZGV4CjAzNQCyOQrJaDBwiIWv5MIuYKXhxlLLsQcx5SwgAgAAcAAAAHhWNBIAAAAAAAAAAJgBAAAH"
    "AAAAcAAAAAMAAACMAAAAAQAAAJgAAAAAAAAAAAAAAAQAAACkAAAAAQAAAMQAAAA8AQAA5AAAAOQA"
    "AADuAAAA9gAAAAUBAAAZAQAAHAEAACEBAAACAAAAAwAAAAQAAAAEAAAAAgAAAAAAAAAAAAAAAAAA"
    "AAAAAAABAAAAAAAAAAUAAAAAAAAABgAAAAAAAAABAAAAAQAAAAAAAAD/////AAAAAHoBAAAAAAAA"
    "CDxjbGluaXQ+AAY8aW5pdD4ADUxNZXRob2RGbGFnczsAEkxqYXZhL2xhbmcvT2JqZWN0OwABVgAD"
    "YmFyAANmb28AAAAAAAAAAQAAAAAAAAAAAAAAAQAAAA4AAAABAAEAAAAAAAAAAAABAAAADgAAAAEA"
    "AQAAAAAAAAAAAAEAAAAOAAAAAQABAAAAAAAAAAAAAQAAAA4AAAADAQCJgASsAgGBgATAAgKCgAjU"
    "AgKBgAjoAgAACwAAAAAAAAABAAAAAAAAAAEAAAAHAAAAcAAAAAIAAAADAAAAjAAAAAMAAAABAAAA"
    "mAAAAAUAAAAEAAAApAAAAAYAAAABAAAAxAAAAAIgAAAHAAAA5AAAAAMQAAABAAAAKAEAAAEgAAAE"
    "AAAALAEAAAAgAAABAAAAegEAAAAQAAABAAAAmAEAAA==";

// Find the method data for the first method with the given name (from class 0). Note: the pointer
// is to the access flags, so that the caller doesn't have to handle the leb128-encoded method-index
// delta.
static const uint8_t* FindMethodData(const DexFile* dex_file, const char* name) {
  const DexFile::ClassDef& class_def = dex_file->GetClassDef(0);
  const uint8_t* class_data = dex_file->GetClassData(class_def);

  ClassDataItemIterator it(*dex_file, class_data);

  const uint8_t* trailing = class_data;
  // Need to manually decode the four entries. DataPointer() doesn't work for this, as the first
  // element has already been loaded into the iterator.
  DecodeUnsignedLeb128(&trailing);
  DecodeUnsignedLeb128(&trailing);
  DecodeUnsignedLeb128(&trailing);
  DecodeUnsignedLeb128(&trailing);

  // Skip all fields.
  while (it.HasNextStaticField() || it.HasNextInstanceField()) {
    trailing = it.DataPointer();
    it.Next();
  }

  while (it.HasNextDirectMethod() || it.HasNextVirtualMethod()) {
    uint32_t method_index = it.GetMemberIndex();
    uint32_t name_index = dex_file->GetMethodId(method_index).name_idx_;
    const DexFile::StringId& string_id = dex_file->GetStringId(name_index);
    const char* str = dex_file->GetStringData(string_id);
    if (strcmp(name, str) == 0) {
      DecodeUnsignedLeb128(&trailing);
      return trailing;
    }

    trailing = it.DataPointer();
    it.Next();
  }

  return nullptr;
}

// Set the method flags to the given value.
static void SetMethodFlags(DexFile* dex_file, const char* method, uint32_t mask) {
  uint8_t* method_flags_ptr = const_cast<uint8_t*>(FindMethodData(dex_file, method));
  CHECK(method_flags_ptr != nullptr) << method;

    // Unroll this, as we only have three bytes, anyways.
  uint8_t base1 = static_cast<uint8_t>(mask & 0x7F);
  *(method_flags_ptr++) = (base1 | 0x80);
  mask >>= 7;

  uint8_t base2 = static_cast<uint8_t>(mask & 0x7F);
  *(method_flags_ptr++) = (base2 | 0x80);
  mask >>= 7;

  uint8_t base3 = static_cast<uint8_t>(mask & 0x7F);
  *method_flags_ptr = base3;
}

static uint32_t GetMethodFlags(DexFile* dex_file, const char* method) {
  const uint8_t* method_flags_ptr = const_cast<uint8_t*>(FindMethodData(dex_file, method));
  CHECK(method_flags_ptr != nullptr) << method;
  return DecodeUnsignedLeb128(&method_flags_ptr);
}

// Apply the given mask to method flags.
static void ApplyMaskToMethodFlags(DexFile* dex_file, const char* method, uint32_t mask) {
  uint32_t value = GetMethodFlags(dex_file, method);
  value &= mask;
  SetMethodFlags(dex_file, method, value);
}

// Apply the given mask to method flags.
static void OrMaskToMethodFlags(DexFile* dex_file, const char* method, uint32_t mask) {
  uint32_t value = GetMethodFlags(dex_file, method);
  value |= mask;
  SetMethodFlags(dex_file, method, value);
}

// Set code_off to 0 for the method.
static void RemoveCode(DexFile* dex_file, const char* method) {
  const uint8_t* ptr = FindMethodData(dex_file, method);
  // Next is flags, pass.
  DecodeUnsignedLeb128(&ptr);

  // Figure out how many bytes the code_off is.
  const uint8_t* tmp = ptr;
  DecodeUnsignedLeb128(&tmp);
  size_t bytes = tmp - ptr;

  uint8_t* mod = const_cast<uint8_t*>(ptr);
  for (size_t i = 1; i < bytes; ++i) {
    *(mod++) = 0x80;
  }
  *mod = 0x00;
}

TEST_F(DexFileVerifierTest, MethodAccessFlagsBase) {
  // Check that it's OK when the wrong declared-synchronized flag is removed from "foo."
  VerifyModification(
      kMethodFlagsTestDex,
      "method_flags_ok",
      [](DexFile* dex_file) {
        ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
        ApplyMaskToMethodFlags(dex_file, "bar", ~kAccDeclaredSynchronized);
      },
      nullptr);
}

TEST_F(DexFileVerifierTest, MethodAccessFlagsConstructors) {
  // Make sure we still accept constructors without their flags.
  VerifyModification(
      kMethodFlagsTestDex,
      "method_flags_missing_constructor_tag_ok",
      [](DexFile* dex_file) {
        ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
        ApplyMaskToMethodFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

        ApplyMaskToMethodFlags(dex_file, "<init>", ~kAccConstructor);
        ApplyMaskToMethodFlags(dex_file, "<clinit>", ~kAccConstructor);
      },
      nullptr);

  constexpr const char* kConstructors[] = { "<clinit>", "<init>"};
  for (size_t i = 0; i < 2; ++i) {
    // Constructor with code marked native.
    VerifyModification(
        kMethodFlagsTestDex,
        "method_flags_constructor_native",
        [&](DexFile* dex_file) {
          ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
          ApplyMaskToMethodFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

          OrMaskToMethodFlags(dex_file, kConstructors[i], kAccNative);
        },
        "has code, but is marked native or abstract");
    // Constructor with code marked abstract.
    VerifyModification(
        kMethodFlagsTestDex,
        "method_flags_constructor_abstract",
        [&](DexFile* dex_file) {
          ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
          ApplyMaskToMethodFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

          OrMaskToMethodFlags(dex_file, kConstructors[i], kAccAbstract);
        },
        "has code, but is marked native or abstract");
    // Constructor as-is without code.
    VerifyModification(
        kMethodFlagsTestDex,
        "method_flags_constructor_nocode",
        [&](DexFile* dex_file) {
          ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
          ApplyMaskToMethodFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

          RemoveCode(dex_file, kConstructors[i]);
        },
        "has no code, but is not marked native or abstract");
    // Constructor without code marked native.
    VerifyModification(
        kMethodFlagsTestDex,
        "method_flags_constructor_native_nocode",
        [&](DexFile* dex_file) {
          ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
          ApplyMaskToMethodFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

          OrMaskToMethodFlags(dex_file, kConstructors[i], kAccNative);
          RemoveCode(dex_file, kConstructors[i]);
        },
        "must not be abstract or native");
    // Constructor without code marked abstract.
    VerifyModification(
        kMethodFlagsTestDex,
        "method_flags_constructor_abstract_nocode",
        [&](DexFile* dex_file) {
          ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
          ApplyMaskToMethodFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

          OrMaskToMethodFlags(dex_file, kConstructors[i], kAccAbstract);
          RemoveCode(dex_file, kConstructors[i]);
        },
        "must not be abstract or native");
  }
  // <init> may only have (modulo ignored):
  // kAccPrivate | kAccProtected | kAccPublic | kAccStrict | kAccVarargs | kAccSynthetic
  static constexpr uint32_t kInitAllowed[] = {
      0,
      kAccPrivate,
      kAccProtected,
      kAccPublic,
      kAccStrict,
      kAccVarargs,
      kAccSynthetic
  };
  for (size_t i = 0; i < arraysize(kInitAllowed); ++i) {
    VerifyModification(
        kMethodFlagsTestDex,
        "init_allowed_flags",
        [&](DexFile* dex_file) {
          ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
          ApplyMaskToMethodFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

          ApplyMaskToMethodFlags(dex_file, "<init>", ~kAccPublic);
          OrMaskToMethodFlags(dex_file, "<init>", kInitAllowed[i]);
        },
        nullptr);
  }
  // Only one of public-private-protected.
  for (size_t i = 1; i < 8; ++i) {
    if (POPCOUNT(i) < 2) {
      continue;
    }
    // Technically the flags match, but just be defensive here.
    uint32_t mask = ((i & 1) != 0 ? kAccPrivate : 0) |
                    ((i & 2) != 0 ? kAccProtected : 0) |
                    ((i & 4) != 0 ? kAccPublic : 0);
    VerifyModification(
        kMethodFlagsTestDex,
        "init_one_of_ppp",
        [&](DexFile* dex_file) {
          ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
          ApplyMaskToMethodFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

          ApplyMaskToMethodFlags(dex_file, "<init>", ~kAccPublic);
          OrMaskToMethodFlags(dex_file, "<init>", mask);
        },
        "Method may have only one of public/protected/private");
  }
  // <init> doesn't allow
  // kAccStatic | kAccFinal | kAccSynchronized | kAccBridge
  // Need to handle static separately as it has its own error message.
  VerifyModification(
      kMethodFlagsTestDex,
      "init_not_allowed_flags",
      [&](DexFile* dex_file) {
        ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
        ApplyMaskToMethodFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

        ApplyMaskToMethodFlags(dex_file, "<init>", ~kAccPublic);
        OrMaskToMethodFlags(dex_file, "<init>", kAccStatic);
      },
      "Constructor 1 is not flagged correctly wrt/ static");
  static constexpr uint32_t kInitNotAllowed[] = {
      kAccFinal,
      kAccSynchronized,
      kAccBridge
  };
  for (size_t i = 0; i < arraysize(kInitNotAllowed); ++i) {
    VerifyModification(
        kMethodFlagsTestDex,
        "init_not_allowed_flags",
        [&](DexFile* dex_file) {
          ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
          ApplyMaskToMethodFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

          ApplyMaskToMethodFlags(dex_file, "<init>", ~kAccPublic);
          OrMaskToMethodFlags(dex_file, "<init>", kInitNotAllowed[i]);
        },
        "Constructor 1 flagged inappropriately");
  }
}

TEST_F(DexFileVerifierTest, MethodAccessFlagsMethods) {
  constexpr const char* kMethods[] = { "foo", "bar"};
  for (size_t i = 0; i < arraysize(kMethods); ++i) {
    // Make sure we reject non-constructors marked as constructors.
    VerifyModification(
        kMethodFlagsTestDex,
        "method_flags_non_constructor",
        [&](DexFile* dex_file) {
          ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
          ApplyMaskToMethodFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

          OrMaskToMethodFlags(dex_file, kMethods[i], kAccConstructor);
        },
        "is marked constructor, but doesn't match name");

    VerifyModification(
        kMethodFlagsTestDex,
        "method_flags_native_with_code",
        [&](DexFile* dex_file) {
          ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
          ApplyMaskToMethodFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

          OrMaskToMethodFlags(dex_file, kMethods[i], kAccNative);
        },
        "has code, but is marked native or abstract");

    VerifyModification(
        kMethodFlagsTestDex,
        "method_flags_abstract_with_code",
        [&](DexFile* dex_file) {
          ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
          ApplyMaskToMethodFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

          OrMaskToMethodFlags(dex_file, kMethods[i], kAccAbstract);
        },
        "has code, but is marked native or abstract");

    VerifyModification(
        kMethodFlagsTestDex,
        "method_flags_non_abstract_native_no_code",
        [&](DexFile* dex_file) {
          ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
          ApplyMaskToMethodFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

          RemoveCode(dex_file, kMethods[i]);
        },
        "has no code, but is not marked native or abstract");

    // Abstract methods may not have the following flags.
    constexpr uint32_t kAbstractDisallowed[] = {
        kAccPrivate,
        kAccStatic,
        kAccFinal,
        kAccNative,
        kAccStrict,
        kAccSynchronized,
    };
    for (size_t j = 0; j < arraysize(kAbstractDisallowed); ++j) {
      VerifyModification(
          kMethodFlagsTestDex,
          "method_flags_abstract_and_disallowed_no_code",
          [&](DexFile* dex_file) {
            ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
            ApplyMaskToMethodFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

            RemoveCode(dex_file, kMethods[i]);

            // Can't check private and static with foo, as it's in the virtual list and gives a
            // different error.
            if (((GetMethodFlags(dex_file, kMethods[i]) & kAccPublic) != 0) &&
                ((kAbstractDisallowed[j] & (kAccPrivate | kAccStatic)) != 0)) {
              // Use another breaking flag.
              OrMaskToMethodFlags(dex_file, kMethods[i], kAccAbstract | kAccFinal);
            } else {
              OrMaskToMethodFlags(dex_file, kMethods[i], kAccAbstract | kAbstractDisallowed[j]);
            }
          },
          "has disallowed access flags");
    }

    // Only one of public-private-protected.
    for (size_t j = 1; j < 8; ++j) {
      if (POPCOUNT(j) < 2) {
        continue;
      }
      // Technically the flags match, but just be defensive here.
      uint32_t mask = ((j & 1) != 0 ? kAccPrivate : 0) |
                      ((j & 2) != 0 ? kAccProtected : 0) |
                      ((j & 4) != 0 ? kAccPublic : 0);
      VerifyModification(
          kMethodFlagsTestDex,
          "method_flags_one_of_ppp",
          [&](DexFile* dex_file) {
            ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
            ApplyMaskToMethodFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

            ApplyMaskToMethodFlags(dex_file, kMethods[i], ~kAccPublic);
            OrMaskToMethodFlags(dex_file, kMethods[i], mask);
          },
          "Method may have only one of public/protected/private");
    }
  }
}

TEST_F(DexFileVerifierTest, MethodAccessFlagsIgnoredOK) {
  constexpr const char* kMethods[] = { "<clinit>", "<init>", "foo", "bar"};
  for (size_t i = 0; i < arraysize(kMethods); ++i) {
    // All interesting method flags, other flags are to be ignored.
    constexpr uint32_t kAllMethodFlags =
        kAccPublic |
        kAccPrivate |
        kAccProtected |
        kAccStatic |
        kAccFinal |
        kAccSynchronized |
        kAccBridge |
        kAccVarargs |
        kAccNative |
        kAccAbstract |
        kAccStrict |
        kAccSynthetic;
    constexpr uint32_t kIgnoredMask = ~kAllMethodFlags & 0xFFFF;
    VerifyModification(
        kMethodFlagsTestDex,
        "method_flags_ignored",
        [&](DexFile* dex_file) {
          ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
          ApplyMaskToMethodFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

          OrMaskToMethodFlags(dex_file, kMethods[i], kIgnoredMask);
        },
        nullptr);
  }
}

// Set of dex files for interface method tests. As it's not as easy to mutate method names, it's
// just easier to break up bad cases.

// Standard interface. Use declared-synchronized again for 3B encoding.
//
// .class public interface LInterfaceMethodFlags;
// .super Ljava/lang/Object;
//
// .method public static constructor <clinit>()V
// .registers 1
//     return-void
// .end method
//
// .method public abstract declared-synchronized foo()V
// .end method
static const char kMethodFlagsInterface[] =
    "ZGV4CjAzNQCOM0odZ5bws1d9GSmumXaK5iE/7XxFpOm8AQAAcAAAAHhWNBIAAAAAAAAAADQBAAAF"
    "AAAAcAAAAAMAAACEAAAAAQAAAJAAAAAAAAAAAAAAAAIAAACcAAAAAQAAAKwAAADwAAAAzAAAAMwA"
    "AADWAAAA7gAAAAIBAAAFAQAAAQAAAAIAAAADAAAAAwAAAAIAAAAAAAAAAAAAAAAAAAAAAAAABAAA"
    "AAAAAAABAgAAAQAAAAAAAAD/////AAAAACIBAAAAAAAACDxjbGluaXQ+ABZMSW50ZXJmYWNlTWV0"
    "aG9kRmxhZ3M7ABJMamF2YS9sYW5nL09iamVjdDsAAVYAA2ZvbwAAAAAAAAABAAAAAAAAAAAAAAAB"
    "AAAADgAAAAEBAImABJACAYGICAAAAAALAAAAAAAAAAEAAAAAAAAAAQAAAAUAAABwAAAAAgAAAAMA"
    "AACEAAAAAwAAAAEAAACQAAAABQAAAAIAAACcAAAABgAAAAEAAACsAAAAAiAAAAUAAADMAAAAAxAA"
    "AAEAAAAMAQAAASAAAAEAAAAQAQAAACAAAAEAAAAiAQAAABAAAAEAAAA0AQAA";

// To simplify generation of interesting "sub-states" of src_value, allow a "simple" mask to apply
// to a src_value, such that mask bit 0 applies to the lowest set bit in src_value, and so on.
static uint32_t ApplyMaskShifted(uint32_t src_value, uint32_t mask) {
  uint32_t result = 0;
  uint32_t mask_index = 0;
  while (src_value != 0) {
    uint32_t index = CTZ(src_value);
    if (((src_value & (1 << index)) != 0) &&
        ((mask & (1 << mask_index)) != 0)) {
      result |= (1 << index);
    }
    src_value &= ~(1 << index);
    mask_index++;
  }
  return result;
}

TEST_F(DexFileVerifierTest, MethodAccessFlagsInterfaces) {
  VerifyModification(
      kMethodFlagsInterface,
      "method_flags_interface_ok",
      [](DexFile* dex_file) {
        ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
      },
      nullptr);

  VerifyModification(
      kMethodFlagsInterface,
      "method_flags_interface_non_public",
      [](DexFile* dex_file) {
        ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);

        ApplyMaskToMethodFlags(dex_file, "foo", ~kAccPublic);
      },
      "Interface method 1 is not public and abstract");
  VerifyModification(
      kMethodFlagsInterface,
      "method_flags_interface_non_abstract",
      [](DexFile* dex_file) {
        ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);

        ApplyMaskToMethodFlags(dex_file, "foo", ~kAccAbstract);
      },
      "Method 1 has no code, but is not marked native or abstract");

  VerifyModification(
      kMethodFlagsInterface,
      "method_flags_interface_static",
      [](DexFile* dex_file) {
        ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);

        OrMaskToMethodFlags(dex_file, "foo", kAccStatic);
      },
      "Direct/virtual method 1 not in expected list 0");
  VerifyModification(
      kMethodFlagsInterface,
      "method_flags_interface_private",
      [](DexFile* dex_file) {
        ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);

        ApplyMaskToMethodFlags(dex_file, "foo", ~kAccPublic);
        OrMaskToMethodFlags(dex_file, "foo", kAccPrivate);
      },
      "Direct/virtual method 1 not in expected list 0");

  VerifyModification(
      kMethodFlagsInterface,
      "method_flags_interface_non_public",
      [](DexFile* dex_file) {
        ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);

        ApplyMaskToMethodFlags(dex_file, "foo", ~kAccPublic);
      },
      "Interface method 1 is not public and abstract");
  VerifyModification(
      kMethodFlagsInterface,
      "method_flags_interface_protected",
      [](DexFile* dex_file) {
        ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);

        ApplyMaskToMethodFlags(dex_file, "foo", ~kAccPublic);
        OrMaskToMethodFlags(dex_file, "foo", kAccProtected);
      },
      "Interface method 1 is not public and abstract");

  constexpr uint32_t kAllMethodFlags =
      kAccPublic |
      kAccPrivate |
      kAccProtected |
      kAccStatic |
      kAccFinal |
      kAccSynchronized |
      kAccBridge |
      kAccVarargs |
      kAccNative |
      kAccAbstract |
      kAccStrict |
      kAccSynthetic;
  constexpr uint32_t kInterfaceMethodFlags =
      kAccPublic | kAccAbstract | kAccVarargs | kAccBridge | kAccSynthetic;
  constexpr uint32_t kInterfaceDisallowed = kAllMethodFlags &
                                            ~kInterfaceMethodFlags &
                                            // Already tested, needed to be separate.
                                            ~kAccStatic &
                                            ~kAccPrivate &
                                            ~kAccProtected;
  static_assert(kInterfaceDisallowed != 0, "There should be disallowed flags.");

  uint32_t bits = POPCOUNT(kInterfaceDisallowed);
  for (uint32_t i = 1; i < (1u << bits); ++i) {
    VerifyModification(
        kMethodFlagsInterface,
        "method_flags_interface_non_abstract",
        [&](DexFile* dex_file) {
          ApplyMaskToMethodFlags(dex_file, "foo", ~kAccDeclaredSynchronized);

          uint32_t mask = ApplyMaskShifted(kInterfaceDisallowed, i);
          if ((mask & kAccProtected) != 0) {
            mask &= ~kAccProtected;
            ApplyMaskToMethodFlags(dex_file, "foo", ~kAccPublic);
          }
          OrMaskToMethodFlags(dex_file, "foo", mask);
        },
        "Abstract method 1 has disallowed access flags");
  }
}

///////////////////////////////////////////////////////////////////

// Field flags.

// Find the method data for the first method with the given name (from class 0). Note: the pointer
// is to the access flags, so that the caller doesn't have to handle the leb128-encoded method-index
// delta.
static const uint8_t* FindFieldData(const DexFile* dex_file, const char* name) {
  const DexFile::ClassDef& class_def = dex_file->GetClassDef(0);
  const uint8_t* class_data = dex_file->GetClassData(class_def);

  ClassDataItemIterator it(*dex_file, class_data);

  const uint8_t* trailing = class_data;
  // Need to manually decode the four entries. DataPointer() doesn't work for this, as the first
  // element has already been loaded into the iterator.
  DecodeUnsignedLeb128(&trailing);
  DecodeUnsignedLeb128(&trailing);
  DecodeUnsignedLeb128(&trailing);
  DecodeUnsignedLeb128(&trailing);

  while (it.HasNextStaticField() || it.HasNextInstanceField()) {
    uint32_t field_index = it.GetMemberIndex();
    uint32_t name_index = dex_file->GetFieldId(field_index).name_idx_;
    const DexFile::StringId& string_id = dex_file->GetStringId(name_index);
    const char* str = dex_file->GetStringData(string_id);
    if (strcmp(name, str) == 0) {
      DecodeUnsignedLeb128(&trailing);
      return trailing;
    }

    trailing = it.DataPointer();
    it.Next();
  }

  return nullptr;
}

// Set the method flags to the given value.
static void SetFieldFlags(DexFile* dex_file, const char* field, uint32_t mask) {
  uint8_t* field_flags_ptr = const_cast<uint8_t*>(FindFieldData(dex_file, field));
  CHECK(field_flags_ptr != nullptr) << field;

    // Unroll this, as we only have three bytes, anyways.
  uint8_t base1 = static_cast<uint8_t>(mask & 0x7F);
  *(field_flags_ptr++) = (base1 | 0x80);
  mask >>= 7;

  uint8_t base2 = static_cast<uint8_t>(mask & 0x7F);
  *(field_flags_ptr++) = (base2 | 0x80);
  mask >>= 7;

  uint8_t base3 = static_cast<uint8_t>(mask & 0x7F);
  *field_flags_ptr = base3;
}

static uint32_t GetFieldFlags(DexFile* dex_file, const char* field) {
  const uint8_t* field_flags_ptr = const_cast<uint8_t*>(FindFieldData(dex_file, field));
  CHECK(field_flags_ptr != nullptr) << field;
  return DecodeUnsignedLeb128(&field_flags_ptr);
}

// Apply the given mask to method flags.
static void ApplyMaskToFieldFlags(DexFile* dex_file, const char* field, uint32_t mask) {
  uint32_t value = GetFieldFlags(dex_file, field);
  value &= mask;
  SetFieldFlags(dex_file, field, value);
}

// Apply the given mask to method flags.
static void OrMaskToFieldFlags(DexFile* dex_file, const char* field, uint32_t mask) {
  uint32_t value = GetFieldFlags(dex_file, field);
  value |= mask;
  SetFieldFlags(dex_file, field, value);
}

// Standard class. Use declared-synchronized again for 3B encoding.
//
// .class public LFieldFlags;
// .super Ljava/lang/Object;
//
// .field declared-synchronized public foo:I
//
// .field declared-synchronized public static bar:I

static const char kFieldFlagsTestDex[] =
    "ZGV4CjAzNQBtLw7hydbfv4TdXidZyzAB70W7w3vnYJRwAQAAcAAAAHhWNBIAAAAAAAAAAAABAAAF"
    "AAAAcAAAAAMAAACEAAAAAAAAAAAAAAACAAAAkAAAAAAAAAAAAAAAAQAAAKAAAACwAAAAwAAAAMAA"
    "AADDAAAA0QAAAOUAAADqAAAAAAAAAAEAAAACAAAAAQAAAAMAAAABAAAABAAAAAEAAAABAAAAAgAA"
    "AAAAAAD/////AAAAAPQAAAAAAAAAAUkADExGaWVsZEZsYWdzOwASTGphdmEvbGFuZy9PYmplY3Q7"
    "AANiYXIAA2ZvbwAAAAAAAAEBAAAAiYAIAYGACAkAAAAAAAAAAQAAAAAAAAABAAAABQAAAHAAAAAC"
    "AAAAAwAAAIQAAAAEAAAAAgAAAJAAAAAGAAAAAQAAAKAAAAACIAAABQAAAMAAAAADEAAAAQAAAPAA"
    "AAAAIAAAAQAAAPQAAAAAEAAAAQAAAAABAAA=";

TEST_F(DexFileVerifierTest, FieldAccessFlagsBase) {
  // Check that it's OK when the wrong declared-synchronized flag is removed from "foo."
  VerifyModification(
      kFieldFlagsTestDex,
      "field_flags_ok",
      [](DexFile* dex_file) {
        ApplyMaskToFieldFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
        ApplyMaskToFieldFlags(dex_file, "bar", ~kAccDeclaredSynchronized);
      },
      nullptr);
}

TEST_F(DexFileVerifierTest, FieldAccessFlagsWrongList) {
  // Mark the field so that it should appear in the opposite list (instance vs static).
  VerifyModification(
      kFieldFlagsTestDex,
      "field_flags_wrong_list",
      [](DexFile* dex_file) {
        ApplyMaskToFieldFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
        ApplyMaskToFieldFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

        OrMaskToFieldFlags(dex_file, "foo", kAccStatic);
      },
      "Static/instance field not in expected list");
  VerifyModification(
      kFieldFlagsTestDex,
      "field_flags_wrong_list",
      [](DexFile* dex_file) {
        ApplyMaskToFieldFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
        ApplyMaskToFieldFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

        ApplyMaskToFieldFlags(dex_file, "bar", ~kAccStatic);
      },
      "Static/instance field not in expected list");
}

TEST_F(DexFileVerifierTest, FieldAccessFlagsPPP) {
  static const char* kFields[] = { "foo", "bar" };
  for (size_t i = 0; i < arraysize(kFields); ++i) {
    // Should be OK to remove public.
    VerifyModification(
        kFieldFlagsTestDex,
        "field_flags_non_public",
        [&](DexFile* dex_file) {
          ApplyMaskToFieldFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
          ApplyMaskToFieldFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

          ApplyMaskToFieldFlags(dex_file, kFields[i], ~kAccPublic);
        },
        nullptr);
    constexpr uint32_t kAccFlags = kAccPublic | kAccPrivate | kAccProtected;
    uint32_t bits = POPCOUNT(kAccFlags);
    for (uint32_t j = 1; j < (1u << bits); ++j) {
      if (POPCOUNT(j) < 2) {
        continue;
      }
      VerifyModification(
           kFieldFlagsTestDex,
           "field_flags_ppp",
           [&](DexFile* dex_file) {
             ApplyMaskToFieldFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
             ApplyMaskToFieldFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

             ApplyMaskToFieldFlags(dex_file, kFields[i], ~kAccPublic);
             uint32_t mask = ApplyMaskShifted(kAccFlags, j);
             OrMaskToFieldFlags(dex_file, kFields[i], mask);
           },
           "Field may have only one of public/protected/private");
    }
  }
}

TEST_F(DexFileVerifierTest, FieldAccessFlagsIgnoredOK) {
  constexpr const char* kFields[] = { "foo", "bar"};
  for (size_t i = 0; i < arraysize(kFields); ++i) {
    // All interesting method flags, other flags are to be ignored.
    constexpr uint32_t kAllFieldFlags =
        kAccPublic |
        kAccPrivate |
        kAccProtected |
        kAccStatic |
        kAccFinal |
        kAccVolatile |
        kAccTransient |
        kAccSynthetic |
        kAccEnum;
    constexpr uint32_t kIgnoredMask = ~kAllFieldFlags & 0xFFFF;
    VerifyModification(
        kFieldFlagsTestDex,
        "field_flags_ignored",
        [&](DexFile* dex_file) {
          ApplyMaskToFieldFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
          ApplyMaskToFieldFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

          OrMaskToFieldFlags(dex_file, kFields[i], kIgnoredMask);
        },
        nullptr);
  }
}

TEST_F(DexFileVerifierTest, FieldAccessFlagsVolatileFinal) {
  constexpr const char* kFields[] = { "foo", "bar"};
  for (size_t i = 0; i < arraysize(kFields); ++i) {
    VerifyModification(
        kFieldFlagsTestDex,
        "field_flags_final_and_volatile",
        [&](DexFile* dex_file) {
          ApplyMaskToFieldFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
          ApplyMaskToFieldFlags(dex_file, "bar", ~kAccDeclaredSynchronized);

          OrMaskToFieldFlags(dex_file, kFields[i], kAccVolatile | kAccFinal);
        },
        "Fields may not be volatile and final");
  }
}

// Standard interface. Needs to be separate from class as interfaces do not allow instance fields.
// Use declared-synchronized again for 3B encoding.
//
// .class public interface LInterfaceFieldFlags;
// .super Ljava/lang/Object;
//
// .field declared-synchronized public static final foo:I

static const char kFieldFlagsInterfaceTestDex[] =
    "ZGV4CjAzNQCVMHfEimR1zZPk6hl6O9GPAYqkl3u0umFkAQAAcAAAAHhWNBIAAAAAAAAAAPQAAAAE"
    "AAAAcAAAAAMAAACAAAAAAAAAAAAAAAABAAAAjAAAAAAAAAAAAAAAAQAAAJQAAACwAAAAtAAAALQA"
    "AAC3AAAAzgAAAOIAAAAAAAAAAQAAAAIAAAABAAAAAwAAAAEAAAABAgAAAgAAAAAAAAD/////AAAA"
    "AOwAAAAAAAAAAUkAFUxJbnRlcmZhY2VGaWVsZEZsYWdzOwASTGphdmEvbGFuZy9PYmplY3Q7AANm"
    "b28AAAAAAAABAAAAAJmACAkAAAAAAAAAAQAAAAAAAAABAAAABAAAAHAAAAACAAAAAwAAAIAAAAAE"
    "AAAAAQAAAIwAAAAGAAAAAQAAAJQAAAACIAAABAAAALQAAAADEAAAAQAAAOgAAAAAIAAAAQAAAOwA"
    "AAAAEAAAAQAAAPQAAAA=";

TEST_F(DexFileVerifierTest, FieldAccessFlagsInterface) {
  VerifyModification(
      kFieldFlagsInterfaceTestDex,
      "field_flags_interface",
      [](DexFile* dex_file) {
        ApplyMaskToFieldFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
      },
      nullptr);

  VerifyModification(
      kFieldFlagsInterfaceTestDex,
      "field_flags_interface_non_public",
      [](DexFile* dex_file) {
        ApplyMaskToFieldFlags(dex_file, "foo", ~kAccDeclaredSynchronized);

        ApplyMaskToFieldFlags(dex_file, "foo", ~kAccPublic);
      },
      "Interface field is not public final static");
  VerifyModification(
      kFieldFlagsInterfaceTestDex,
      "field_flags_interface_non_final",
      [](DexFile* dex_file) {
        ApplyMaskToFieldFlags(dex_file, "foo", ~kAccDeclaredSynchronized);

        ApplyMaskToFieldFlags(dex_file, "foo", ~kAccFinal);
      },
      "Interface field is not public final static");
  VerifyModification(
      kFieldFlagsInterfaceTestDex,
      "field_flags_interface_protected",
      [](DexFile* dex_file) {
        ApplyMaskToFieldFlags(dex_file, "foo", ~kAccDeclaredSynchronized);

        ApplyMaskToFieldFlags(dex_file, "foo", ~kAccPublic);
        OrMaskToFieldFlags(dex_file, "foo", kAccProtected);
      },
      "Interface field is not public final static");
  VerifyModification(
      kFieldFlagsInterfaceTestDex,
      "field_flags_interface_private",
      [](DexFile* dex_file) {
        ApplyMaskToFieldFlags(dex_file, "foo", ~kAccDeclaredSynchronized);

        ApplyMaskToFieldFlags(dex_file, "foo", ~kAccPublic);
        OrMaskToFieldFlags(dex_file, "foo", kAccPrivate);
      },
      "Interface field is not public final static");

  VerifyModification(
      kFieldFlagsInterfaceTestDex,
      "field_flags_interface_synthetic",
      [](DexFile* dex_file) {
        ApplyMaskToFieldFlags(dex_file, "foo", ~kAccDeclaredSynchronized);

        OrMaskToFieldFlags(dex_file, "foo", kAccSynthetic);
      },
      nullptr);

  constexpr uint32_t kAllFieldFlags =
      kAccPublic |
      kAccPrivate |
      kAccProtected |
      kAccStatic |
      kAccFinal |
      kAccVolatile |
      kAccTransient |
      kAccSynthetic |
      kAccEnum;
  constexpr uint32_t kInterfaceFieldFlags = kAccPublic | kAccStatic | kAccFinal | kAccSynthetic;
  constexpr uint32_t kInterfaceDisallowed = kAllFieldFlags &
                                            ~kInterfaceFieldFlags &
                                            ~kAccProtected &
                                            ~kAccPrivate;
  static_assert(kInterfaceDisallowed != 0, "There should be disallowed flags.");

  uint32_t bits = POPCOUNT(kInterfaceDisallowed);
  for (uint32_t i = 1; i < (1u << bits); ++i) {
    VerifyModification(
        kFieldFlagsInterfaceTestDex,
        "field_flags_interface_disallowed",
        [&](DexFile* dex_file) {
          ApplyMaskToFieldFlags(dex_file, "foo", ~kAccDeclaredSynchronized);

          uint32_t mask = ApplyMaskShifted(kInterfaceDisallowed, i);
          if ((mask & kAccProtected) != 0) {
            mask &= ~kAccProtected;
            ApplyMaskToFieldFlags(dex_file, "foo", ~kAccPublic);
          }
          OrMaskToFieldFlags(dex_file, "foo", mask);
        },
        "Interface field has disallowed flag");
  }
}

// Standard bad interface. Needs to be separate from class as interfaces do not allow instance
// fields. Use declared-synchronized again for 3B encoding.
//
// .class public interface LInterfaceFieldFlags;
// .super Ljava/lang/Object;
//
// .field declared-synchronized public final foo:I

static const char kFieldFlagsInterfaceBadTestDex[] =
    "ZGV4CjAzNQByMUnqYKHBkUpvvNp+9CnZ2VyDkKnRN6VkAQAAcAAAAHhWNBIAAAAAAAAAAPQAAAAE"
    "AAAAcAAAAAMAAACAAAAAAAAAAAAAAAABAAAAjAAAAAAAAAAAAAAAAQAAAJQAAACwAAAAtAAAALQA"
    "AAC3AAAAzgAAAOIAAAAAAAAAAQAAAAIAAAABAAAAAwAAAAEAAAABAgAAAgAAAAAAAAD/////AAAA"
    "AOwAAAAAAAAAAUkAFUxJbnRlcmZhY2VGaWVsZEZsYWdzOwASTGphdmEvbGFuZy9PYmplY3Q7AANm"
    "b28AAAAAAAAAAQAAAJGACAkAAAAAAAAAAQAAAAAAAAABAAAABAAAAHAAAAACAAAAAwAAAIAAAAAE"
    "AAAAAQAAAIwAAAAGAAAAAQAAAJQAAAACIAAABAAAALQAAAADEAAAAQAAAOgAAAAAIAAAAQAAAOwA"
    "AAAAEAAAAQAAAPQAAAA=";

TEST_F(DexFileVerifierTest, FieldAccessFlagsInterfaceNonStatic) {
  VerifyModification(
      kFieldFlagsInterfaceBadTestDex,
      "field_flags_interface_non_static",
      [](DexFile* dex_file) {
        ApplyMaskToFieldFlags(dex_file, "foo", ~kAccDeclaredSynchronized);
      },
      "Interface field is not public final static");
}

// Generated from:
//
// .class public LTest;
// .super Ljava/lang/Object;
// .source "Test.java"
//
// .method public constructor <init>()V
//     .registers 1
//
//     .prologue
//     .line 1
//     invoke-direct {p0}, Ljava/lang/Object;-><init>()V
//
//     return-void
// .end method
//
// .method public static main()V
//     .registers 2
//
//     const-string v0, "a"
//     const-string v0, "b"
//     const-string v0, "c"
//     const-string v0, "d"
//     const-string v0, "e"
//     const-string v0, "f"
//     const-string v0, "g"
//     const-string v0, "h"
//     const-string v0, "i"
//     const-string v0, "j"
//     const-string v0, "k"
//
//     .local v1, "local_var":Ljava/lang/String;
//     const-string v1, "test"
// .end method

static const char kDebugInfoTestDex[] =
    "ZGV4CjAzNQCHRkHix2eIMQgvLD/0VGrlllZLo0Rb6VyUAgAAcAAAAHhWNBIAAAAAAAAAAAwCAAAU"
    "AAAAcAAAAAQAAADAAAAAAQAAANAAAAAAAAAAAAAAAAMAAADcAAAAAQAAAPQAAACAAQAAFAEAABQB"
    "AAAcAQAAJAEAADgBAABMAQAAVwEAAFoBAABdAQAAYAEAAGMBAABmAQAAaQEAAGwBAABvAQAAcgEA"
    "AHUBAAB4AQAAewEAAIYBAACMAQAAAQAAAAIAAAADAAAABQAAAAUAAAADAAAAAAAAAAAAAAAAAAAA"
    "AAAAABIAAAABAAAAAAAAAAAAAAABAAAAAQAAAAAAAAAEAAAAAAAAAPwBAAAAAAAABjxpbml0PgAG"
    "TFRlc3Q7ABJMamF2YS9sYW5nL09iamVjdDsAEkxqYXZhL2xhbmcvU3RyaW5nOwAJVGVzdC5qYXZh"
    "AAFWAAFhAAFiAAFjAAFkAAFlAAFmAAFnAAFoAAFpAAFqAAFrAAlsb2NhbF92YXIABG1haW4ABHRl"
    "c3QAAAABAAcOAAAAARYDARIDAAAAAQABAAEAAACUAQAABAAAAHAQAgAAAA4AAgAAAAAAAACZAQAA"
    "GAAAABoABgAaAAcAGgAIABoACQAaAAoAGgALABoADAAaAA0AGgAOABoADwAaABAAGgETAAAAAgAA"
    "gYAEpAMBCbwDAAALAAAAAAAAAAEAAAAAAAAAAQAAABQAAABwAAAAAgAAAAQAAADAAAAAAwAAAAEA"
    "AADQAAAABQAAAAMAAADcAAAABgAAAAEAAAD0AAAAAiAAABQAAAAUAQAAAyAAAAIAAACUAQAAASAA"
    "AAIAAACkAQAAACAAAAEAAAD8AQAAABAAAAEAAAAMAgAA";

TEST_F(DexFileVerifierTest, DebugInfoTypeIdxTest) {
  {
    // The input dex file should be good before modification.
    ScratchFile tmp;
    std::string error_msg;
    std::unique_ptr<const DexFile> raw(OpenDexFileBase64(kDebugInfoTestDex,
                                                         tmp.GetFilename().c_str(),
                                                         &error_msg));
    ASSERT_TRUE(raw.get() != nullptr) << error_msg;
  }

  // Modify the debug information entry.
  VerifyModification(
      kDebugInfoTestDex,
      "debug_start_type_idx",
      [](DexFile* dex_file) {
        *(const_cast<uint8_t*>(dex_file->Begin()) + 416) = 0x14U;
      },
      "DBG_START_LOCAL type_idx");
}

}  // namespace art
