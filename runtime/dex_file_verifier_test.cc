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
#include <memory>

#include "base/unix_file/fd_file.h"
#include "base/macros.h"
#include "common_runtime_test.h"
#include "scoped_thread_state_change.h"
#include "thread-inl.h"

namespace art {

class DexFileVerifierTest : public CommonRuntimeTest {};

static const byte kBase64Map[256] = {
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

static inline byte* DecodeBase64(const char* src, size_t* dst_size) {
  std::vector<byte> tmp;
  uint32_t t = 0, y = 0;
  int g = 3;
  for (size_t i = 0; src[i] != '\0'; ++i) {
    byte c = kBase64Map[src[i] & 0xFF];
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
  std::unique_ptr<byte[]> dst(new byte[tmp.size()]);
  if (dst_size != nullptr) {
    *dst_size = tmp.size();
  } else {
    *dst_size = 0;
  }
  std::copy(tmp.begin(), tmp.end(), dst.get());
  return dst.release();
}

static const DexFile* OpenDexFileBase64(const char* base64, const char* location,
                                        std::string* error_msg) {
  // decode base64
  CHECK(base64 != NULL);
  size_t length;
  std::unique_ptr<byte[]> dex_bytes(DecodeBase64(base64, &length));
  CHECK(dex_bytes.get() != NULL);

  // write to provided file
  std::unique_ptr<File> file(OS::CreateEmptyFile(location));
  CHECK(file.get() != NULL);
  if (!file->WriteFully(dex_bytes.get(), length)) {
    PLOG(FATAL) << "Failed to write base64 as dex file";
  }
  file.reset();

  // read dex file
  ScopedObjectAccess soa(Thread::Current());
  std::vector<const DexFile*> tmp;
  bool success = DexFile::Open(location, location, error_msg, &tmp);
  CHECK(success) << error_msg;
  EXPECT_EQ(1U, tmp.size());
  const DexFile* dex_file = tmp[0];
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

static void FixUpChecksum(byte* dex_file) {
  DexFile::Header* header = reinterpret_cast<DexFile::Header*>(dex_file);
  uint32_t expected_size = header->file_size_;
  uint32_t adler_checksum = adler32(0L, Z_NULL, 0);
  const uint32_t non_sum = sizeof(DexFile::Header::magic_) + sizeof(DexFile::Header::checksum_);
  const byte* non_sum_ptr = dex_file + non_sum;
  adler_checksum = adler32(adler_checksum, non_sum_ptr, expected_size - non_sum);
  header->checksum_ = adler_checksum;
}

static const DexFile* FixChecksumAndOpen(byte* bytes, size_t length, const char* location,
                                         std::string* error_msg) {
  // Check data.
  CHECK(bytes != nullptr);

  // Fixup of checksum.
  FixUpChecksum(bytes);

  // write to provided file
  std::unique_ptr<File> file(OS::CreateEmptyFile(location));
  CHECK(file.get() != NULL);
  if (!file->WriteFully(bytes, length)) {
    PLOG(FATAL) << "Failed to write base64 as dex file";
  }
  file.reset();

  // read dex file
  ScopedObjectAccess soa(Thread::Current());
  std::vector<const DexFile*> tmp;
  if (!DexFile::Open(location, location, error_msg, &tmp)) {
    return nullptr;
  }
  EXPECT_EQ(1U, tmp.size());
  const DexFile* dex_file = tmp[0];
  EXPECT_EQ(PROT_READ, dex_file->GetPermissions());
  EXPECT_TRUE(dex_file->IsReadOnly());
  return dex_file;
}

static bool ModifyAndLoad(const char* location, size_t offset, uint8_t new_val,
                                    std::string* error_msg) {
  // Decode base64.
  size_t length;
  std::unique_ptr<byte[]> dex_bytes(DecodeBase64(kGoodTestDex, &length));
  CHECK(dex_bytes.get() != NULL);

  // Make modifications.
  dex_bytes.get()[offset] = new_val;

  // Fixup and load.
  std::unique_ptr<const DexFile> file(FixChecksumAndOpen(dex_bytes.get(), length, location,
                                                         error_msg));
  return file.get() != nullptr;
}

TEST_F(DexFileVerifierTest, MethodId) {
  {
    // Class error.
    ScratchFile tmp;
    std::string error_msg;
    bool success = !ModifyAndLoad(tmp.GetFilename().c_str(), 220, 0xFFU, &error_msg);
    ASSERT_TRUE(success);
    ASSERT_NE(error_msg.find("inter_method_id_item class_idx"), std::string::npos) << error_msg;
  }

  {
    // Proto error.
    ScratchFile tmp;
    std::string error_msg;
    bool success = !ModifyAndLoad(tmp.GetFilename().c_str(), 222, 0xFFU, &error_msg);
    ASSERT_TRUE(success);
    ASSERT_NE(error_msg.find("inter_method_id_item proto_idx"), std::string::npos) << error_msg;
  }

  {
    // Name error.
    ScratchFile tmp;
    std::string error_msg;
    bool success = !ModifyAndLoad(tmp.GetFilename().c_str(), 224, 0xFFU, &error_msg);
    ASSERT_TRUE(success);
    ASSERT_NE(error_msg.find("inter_method_id_item name_idx"), std::string::npos) << error_msg;
  }
}

}  // namespace art
