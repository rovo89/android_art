// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/base64.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/raw_dex_file.h"
#include "src/scoped_ptr.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <map>

namespace art {

const byte RawDexFile::kDexMagic[] = { 'd', 'e', 'x', '\n' };
const byte RawDexFile::kDexMagicVersion[] = { '0', '3', '5', '\0' };

// Helper class to deallocate mmap-backed .dex files.
class MmapCloser : public RawDexFile::Closer {
 public:
  MmapCloser(void* addr, size_t length) : addr_(addr), length_(length) {
    CHECK(addr != NULL);
  };
  virtual ~MmapCloser() {
    if (munmap(addr_, length_) == -1) {
      LG << "munmap: " << strerror(errno);  // TODO: PLOG
    }
  }
 private:
  void* addr_;
  size_t length_;
};

// Helper class for deallocating new/delete-backed .dex files.
class PtrCloser : public RawDexFile::Closer {
 public:
  PtrCloser(byte* addr) : addr_(addr) {};
  virtual ~PtrCloser() { delete[] addr_; }
 private:
  byte* addr_;
};

RawDexFile::Closer::~Closer() {}

RawDexFile* RawDexFile::OpenFile(const char* filename) {
  CHECK(filename != NULL);
  int fd = open(filename, O_RDONLY);  // TODO: scoped_fd
  if (fd == -1) {
    LG << "open: " << strerror(errno);  // TODO: PLOG
    return NULL;
  }
  struct stat sbuf;
  memset(&sbuf, 0, sizeof(sbuf));
  if (fstat(fd, &sbuf) == -1) {
    LG << "fstat: " << strerror(errno);  // TODO: PLOG
    close(fd);
    return NULL;
  }
  size_t length = sbuf.st_size;
  void* addr = mmap(NULL, length, PROT_READ, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) {
    LG << "mmap: " << strerror(errno);  // TODO: PLOG
    close(fd);
    return NULL;
  }
  close(fd);
  byte* dex_file = reinterpret_cast<byte*>(addr);
  Closer* closer = new MmapCloser(addr, length);
  return Open(dex_file, length, closer);
}

RawDexFile* RawDexFile::OpenBase64(const char* base64) {
  CHECK(base64 != NULL);
  size_t length;
  byte* dex_file = DecodeBase64(base64, &length);
  if (dex_file == NULL) {
    return NULL;
  }
  RawDexFile::Closer* closer = new PtrCloser(dex_file);
  return Open(dex_file, length, closer);
}

RawDexFile* RawDexFile::Open(const byte* dex_file, size_t length,
                             Closer* closer) {
  scoped_ptr<RawDexFile> raw(new RawDexFile(dex_file, length, closer));
  if (!raw->Init()) {
    return NULL;
  } else {
    return raw.release();
  }
}

RawDexFile::~RawDexFile() {}

bool RawDexFile::Init() {
  InitMembers();
  if (!IsMagicValid()) {
    return false;
  }
  InitIndex();
  return true;
}

void RawDexFile::InitMembers() {
  const byte* b = base_;
  header_ = reinterpret_cast<const Header*>(b);
  const Header* h = header_;
  string_ids_ = reinterpret_cast<const StringId*>(b + h->string_ids_off_);
  type_ids_ = reinterpret_cast<const TypeId*>(b + h->type_ids_off_);
  field_ids_ = reinterpret_cast<const FieldId*>(b + h->field_ids_off_);
  method_ids_ = reinterpret_cast<const MethodId*>(b + h->method_ids_off_);
  proto_ids_ = reinterpret_cast<const ProtoId*>(b + h->proto_ids_off_);
  class_defs_ = reinterpret_cast<const ClassDef*>(b + h->class_defs_off_);
}

bool RawDexFile::IsMagicValid() {
  return CheckMagic(header_->magic_);
}

bool RawDexFile::CheckMagic(const byte* magic) {
  CHECK(magic != NULL);
  if (memcmp(magic, kDexMagic, sizeof(kDexMagic)) != 0) {
    LOG(WARN) << "Unrecognized magic number:"
              << " " << magic[0]
              << " " << magic[1]
              << " " << magic[2]
              << " " << magic[3];
    return false;
  }
  const byte* version = &magic[sizeof(kDexMagic)];
  if (memcmp(version, kDexMagicVersion, sizeof(kDexMagicVersion)) != 0) {
    LOG(WARN) << "Unrecognized version number:"
              << " " << version[0]
              << " " << version[1]
              << " " << version[2]
              << " " << version[3];
    return false;
  }
  return true;
}

void RawDexFile::InitIndex() {
  CHECK_EQ(index_.size(), 0);
  for (size_t i = 0; i < NumClassDefs(); ++i) {
    const ClassDef& class_def = GetClassDef(i);
    const char* descriptor = GetClassDescriptor(class_def);
    index_[descriptor] = &class_def;
  }
}

const RawDexFile::ClassDef* RawDexFile::FindClassDef(const char* descriptor) {
  CHECK(descriptor != NULL);
  Index::iterator it = index_.find(descriptor);
  if (it == index_.end()) {
    return NULL;
  } else {
    return it->second;
  }
}

}  // namespace art
