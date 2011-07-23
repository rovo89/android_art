// Copyright 2011 Google Inc. All Rights Reserved.

#include "dex_file.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <map>

#include "globals.h"
#include "logging.h"
#include "object.h"
#include "scoped_ptr.h"
#include "utils.h"

namespace art {

const byte DexFile::kDexMagic[] = { 'd', 'e', 'x', '\n' };
const byte DexFile::kDexMagicVersion[] = { '0', '3', '5', '\0' };

DexFile::Closer::~Closer() {}

DexFile::MmapCloser::MmapCloser(void* addr, size_t length) : addr_(addr), length_(length) {
  CHECK(addr != NULL);
}
DexFile::MmapCloser::~MmapCloser() {
  if (munmap(addr_, length_) == -1) {
    PLOG(INFO) << "munmap failed";
  }
}

DexFile::PtrCloser::PtrCloser(byte* addr) : addr_(addr) {}
DexFile::PtrCloser::~PtrCloser() { delete[] addr_; }

DexFile* DexFile::OpenFile(const char* filename) {
  CHECK(filename != NULL);
  int fd = open(filename, O_RDONLY);  // TODO: scoped_fd
  if (fd == -1) {
    PLOG(ERROR) << "open(\"" << filename << "\", O_RDONLY) failed";
    return NULL;
  }
  struct stat sbuf;
  memset(&sbuf, 0, sizeof(sbuf));
  if (fstat(fd, &sbuf) == -1) {
    PLOG(ERROR) << "fstat \"" << filename << "\" failed";
    close(fd);
    return NULL;
  }
  size_t length = sbuf.st_size;
  void* addr = mmap(NULL, length, PROT_READ, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) {
    PLOG(ERROR) << "mmap \"" << filename << "\" failed";
    close(fd);
    return NULL;
  }
  close(fd);
  byte* dex_file = reinterpret_cast<byte*>(addr);
  Closer* closer = new MmapCloser(addr, length);
  return Open(dex_file, length, closer);
}

DexFile* DexFile::OpenPtr(byte* ptr, size_t length) {
  CHECK(ptr != NULL);
  DexFile::Closer* closer = new PtrCloser(ptr);
  return Open(ptr, length, closer);
}

DexFile* DexFile::Open(const byte* dex_bytes, size_t length,
                       Closer* closer) {
  scoped_ptr<DexFile> dex_file(new DexFile(dex_bytes, length, closer));
  if (!dex_file->Init()) {
    return NULL;
  } else {
    return dex_file.release();
  }
}

DexFile::~DexFile() {}

bool DexFile::Init() {
  InitMembers();
  if (!IsMagicValid()) {
    return false;
  }
  InitIndex();
  return true;
}

void DexFile::InitMembers() {
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

bool DexFile::IsMagicValid() {
  return CheckMagic(header_->magic_);
}

bool DexFile::CheckMagic(const byte* magic) {
  CHECK(magic != NULL);
  if (memcmp(magic, kDexMagic, sizeof(kDexMagic)) != 0) {
    LOG(WARNING) << "Unrecognized magic number:"
            << " " << magic[0]
            << " " << magic[1]
            << " " << magic[2]
            << " " << magic[3];
    return false;
  }
  const byte* version = &magic[sizeof(kDexMagic)];
  if (memcmp(version, kDexMagicVersion, sizeof(kDexMagicVersion)) != 0) {
    LOG(WARNING) << "Unrecognized version number:"
            << " " << version[0]
            << " " << version[1]
            << " " << version[2]
            << " " << version[3];
    return false;
  }
  return true;
}

void DexFile::InitIndex() {
  CHECK_EQ(index_.size(), 0U);
  for (size_t i = 0; i < NumClassDefs(); ++i) {
    const ClassDef& class_def = GetClassDef(i);
    const char* descriptor = GetClassDescriptor(class_def);
    index_[descriptor] = &class_def;
  }
}

const DexFile::ClassDef* DexFile::FindClassDef(const StringPiece& descriptor) const {
  CHECK(descriptor != NULL);
  Index::const_iterator it = index_.find(descriptor);
  if (it == index_.end()) {
    return NULL;
  } else {
    return it->second;
  }
}

// Read a signed integer.  "zwidth" is the zero-based byte count.
static int32_t ReadSignedInt(const byte* ptr, int zwidth)
{
  int32_t val = 0;
  for (int i = zwidth; i >= 0; --i) {
    val = ((uint32_t)val >> 8) | (((int32_t)*ptr++) << 24);
  }
  val >>= (3 - zwidth) * 8;
  return val;
}

// Read an unsigned integer.  "zwidth" is the zero-based byte count,
// "fill_on_right" indicates which side we want to zero-fill from.
static uint32_t ReadUnsignedInt(const byte* ptr, int zwidth,
                                bool fill_on_right) {
  uint32_t val = 0;
  if (!fill_on_right) {
    for (int i = zwidth; i >= 0; --i) {
      val = (val >> 8) | (((uint32_t)*ptr++) << 24);
    }
    val >>= (3 - zwidth) * 8;
  } else {
    for (int i = zwidth; i >= 0; --i) {
      val = (val >> 8) | (((uint32_t)*ptr++) << 24);
    }
  }
  return val;
}

// Read a signed long.  "zwidth" is the zero-based byte count.
static int64_t ReadSignedLong(const byte* ptr, int zwidth) {
  int64_t val = 0;
  for (int i = zwidth; i >= 0; --i) {
    val = ((uint64_t)val >> 8) | (((int64_t)*ptr++) << 56);
  }
  val >>= (7 - zwidth) * 8;
  return val;
}

// Read an unsigned long.  "zwidth" is the zero-based byte count,
// "fill_on_right" indicates which side we want to zero-fill from.
static uint64_t ReadUnsignedLong(const byte* ptr, int zwidth,
                                 bool fill_on_right) {
  uint64_t val = 0;
  if (!fill_on_right) {
    for (int i = zwidth; i >= 0; --i) {
      val = (val >> 8) | (((uint64_t)*ptr++) << 56);
    }
    val >>= (7 - zwidth) * 8;
  } else {
    for (int i = zwidth; i >= 0; --i) {
      val = (val >> 8) | (((uint64_t)*ptr++) << 56);
    }
  }
  return val;
}

DexFile::ValueType DexFile::ReadEncodedValue(const byte** stream,
                                             JValue* value) const {
  const byte* ptr = *stream;
  byte value_type = *ptr++;
  byte value_arg = value_type >> kEncodedValueArgShift;
  size_t width = value_arg + 1;  // assume and correct later
  int type = value_type & kEncodedValueTypeMask;
  switch (type) {
    case DexFile::kByte: {
      int32_t b = ReadSignedInt(ptr, value_arg);
      CHECK(IsInt(8, b));
      value->i = b;
      break;
    }
    case DexFile::kShort: {
      int32_t s = ReadSignedInt(ptr, value_arg);
      CHECK(IsInt(16, s));
      value->i = s;
      break;
    }
    case DexFile::kChar: {
      uint32_t c = ReadUnsignedInt(ptr, value_arg, false);
      CHECK(IsUint(16, c));
      value->i = c;
      break;
    }
    case DexFile::kInt:
      value->i = ReadSignedInt(ptr, value_arg);
      break;
    case DexFile::kLong:
      value->j = ReadSignedLong(ptr, value_arg);
      break;
    case DexFile::kFloat:
      value->i = ReadUnsignedInt(ptr, value_arg, true);
      break;
    case DexFile::kDouble:
      value->j = ReadUnsignedLong(ptr, value_arg, true);
      break;
    case DexFile::kBoolean:
      value->i = (value_arg != 0);
      width = 0;
      break;
    case DexFile::kString:
    case DexFile::kType:
    case DexFile::kMethod:
    case DexFile::kEnum:
      value->i = ReadUnsignedInt(ptr, value_arg, false);
      break;
    case DexFile::kField:
    case DexFile::kArray:
    case DexFile::kAnnotation:
      LOG(FATAL) << "Unimplemented";
      break;
    case DexFile::kNull:
      value->i = 0;
      width = 0;
      break;
    default:
      LOG(FATAL) << "Unreached";
  }
  ptr += width;
  *stream = ptr;
  return static_cast<ValueType>(type);
}

}  // namespace art
