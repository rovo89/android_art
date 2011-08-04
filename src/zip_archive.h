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

#ifndef ART_SRC_ZIP_ARCHIVE_H_
#define ART_SRC_ZIP_ARCHIVE_H_

#include <map>
#include <stdint.h>
#include <sys/mman.h>
#include <zlib.h>

#include "globals.h"
#include "logging.h"
#include "scoped_ptr.h"
#include "stringpiece.h"
#include "unordered_map.h"

namespace art {

class ZipArchive;
class MemMap;

class ZipEntry {

 public:
  // Uncompress an entry, in its entirety, to an open file descriptor.
  bool Extract(int fd);

  uint32_t GetCrc32();

 private:

  ZipEntry(ZipArchive* zip_archive, const uint8_t* ptr) : zip_archive_(zip_archive), ptr_(ptr) {};

  // Zip compression methods
  enum {
    kCompressStored     = 0,        // no compression
    kCompressDeflated   = 8,        // standard deflate
  };

  // kCompressStored, kCompressDeflated, ...
  uint16_t GetCompressionMethod();

  uint32_t GetCompressedLength();

  uint32_t GetUncompressedLength();

  // returns -1 on error
  off_t GetDataOffset();

  ZipArchive* zip_archive_;

  // pointer to zip entry within central directory
  const uint8_t* ptr_;

  friend class ZipArchive;
};

// Used to keep track of unaligned mmap segments.
class MemMap {
 public:

  // Map part of a file into a shared, read-only memory segment.  The "start"
  // offset is absolute, not relative.
  //
  // On success, returns returns a MemMap instance.  On failure, returns a NULL;
  static MemMap* Map(int fd, off_t start, size_t length) {
    // adjust to be page-aligned
    int page_offset = start % kPageSize;
    off_t page_aligned_offset = start - page_offset;
    size_t page_aligned_size = length + page_offset;
    uint8_t* addr = reinterpret_cast<uint8_t*>(mmap(NULL,
                                                    page_aligned_size,
                                                    PROT_READ,
                                                    MAP_FILE | MAP_SHARED,
                                                    fd,
                                                    page_aligned_offset));
    if (addr == MAP_FAILED) {
      return NULL;
    }
    return new MemMap(addr+page_offset, length, addr, page_aligned_size);
  }

  ~MemMap() {
    Unmap();
  };

  // Release a memory mapping, returning true on success or it was previously unmapped.
  bool Unmap() {
    if (base_addr_ == NULL && base_length_ == 0) {
      return true;
    }
    int result = munmap(base_addr_, base_length_);
    if (result != 0) {
      return false;
    }
    base_addr_ = NULL;
    base_length_ = 0;
    return true;
  }

  void* GetAddress() {
    return addr_;
  }

  size_t GetLength() {
    return length_;
  }

 private:
  MemMap(void* addr, size_t length, void* base_addr, size_t base_length)
      : addr_(addr), length_(length), base_addr_(base_addr), base_length_(base_length) {
    CHECK(addr_ != NULL);
    CHECK(length_ != 0);
    CHECK(base_addr_ != NULL);
    CHECK(base_length_ != 0);
  };

  void*   addr_;              // start of data
  size_t  length_;            // length of data

  void*   base_addr_;         // page-aligned base address
  size_t  base_length_;       // length of mapping
};

class ZipArchive {
 public:

  // Zip file constants.
  static const uint32_t kEOCDSignature  = 0x06054b50;
  static const int32_t kEOCDLen         = 22;
  static const int32_t kEOCDNumEntries  =  8;  // offset to #of entries in file
  static const int32_t kEOCDSize        = 12;  // size of the central directory
  static const int32_t kEOCDFileOffset  = 16;  // offset to central directory

  static const int32_t kMaxCommentLen = 65535;  // longest possible in uint16_t
  static const int32_t kMaxEOCDSearch = (kMaxCommentLen + kEOCDLen);

  static const uint32_t kLFHSignature = 0x04034b50;
  static const int32_t kLFHLen        = 30;  // excluding variable-len fields
  static const int32_t kLFHNameLen    = 26;  // offset to filename length
  static const int32_t kLFHExtraLen   = 28;  // offset to extra length

  static const uint32_t kCDESignature   = 0x02014b50;
  static const int32_t kCDELen          = 46;  // excluding variable-len fields
  static const int32_t kCDEMethod       = 10;  // offset to compression method
  static const int32_t kCDEModWhen      = 12;  // offset to modification timestamp
  static const int32_t kCDECRC          = 16;  // offset to entry CRC
  static const int32_t kCDECompLen      = 20;  // offset to compressed length
  static const int32_t kCDEUncompLen    = 24;  // offset to uncompressed length
  static const int32_t kCDENameLen      = 28;  // offset to filename length
  static const int32_t kCDEExtraLen     = 30;  // offset to extra length
  static const int32_t kCDECommentLen   = 32;  // offset to comment length
  static const int32_t kCDELocalOffset  = 42;  // offset to local hdr

  static ZipArchive* Open(const std::string& filename);
  ZipEntry* Find(const char * name);

  ~ZipArchive() {
    Close();
  }

 private:
  ZipArchive(int fd) : fd_(fd), num_entries_(0), dir_offset_(0) {}

  bool MapCentralDirectory();
  bool Parse();
  void Close();

  int fd_;
  uint16_t num_entries_;
  off_t dir_offset_;
  scoped_ptr<MemMap> dir_map_;
  typedef std::tr1::unordered_map<StringPiece, const uint8_t*> DirEntries;
  DirEntries dir_entries_;

  friend class ZipEntry;
};

}  // namespace art

#endif  // ART_SRC_ZIP_ARCHIVE_H_
