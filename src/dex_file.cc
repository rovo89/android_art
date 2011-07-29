// Copyright 2011 Google Inc. All Rights Reserved.

#include "dex_file.h"

#include <fcntl.h>
#include <map>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "globals.h"
#include "logging.h"
#include "object.h"
#include "scoped_ptr.h"
#include "stringprintf.h"
#include "thread.h"
#include "utils.h"
#include "zip_archive.h"

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

DexFile* DexFile::OpenFile(const std::string& filename) {
  int fd = open(filename.c_str(), O_RDONLY);  // TODO: scoped_fd
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

static const char* kClassesDex = "classes.dex";

class LockedFd {
 public:
   static LockedFd* CreateAndLock(std::string& name, mode_t mode) {
    int fd = open(name.c_str(), O_CREAT | O_RDWR, mode);
    if (fd == -1) {
      PLOG(ERROR) << "Can't open file '" << name;
      return NULL;
    }
    fchmod(fd, mode);

    LOG(INFO) << "locking file " << name << " (fd=" << fd << ")";
    int result = flock(fd, LOCK_EX | LOCK_NB);
    if (result == -1) {
        LOG(WARNING) << "sleeping while locking file " << name;
        result = flock(fd, LOCK_EX);
    }
    if (result == -1 ) {
      close(fd);
      PLOG(ERROR) << "Can't lock file '" << name;
      return NULL;
    }
    return new LockedFd(fd);
   }

   int GetFd() const {
     return fd_;
   }

  ~LockedFd() {
    if (fd_ != -1) {
      int result = flock(fd_, LOCK_UN);
      if (result == -1) {
        PLOG(WARNING) << "flock(" << fd_ << ", LOCK_UN) failed";
      }
      close(fd_);
    }
  }

 private:
  LockedFd(int fd) : fd_(fd) {}

  int fd_;
};

class TmpFile {
 public:
  TmpFile(const std::string name) : name_(name) {}
  ~TmpFile() {
    unlink(name_.c_str());
  }
 private:
  const std::string name_;
};

// Open classes.dex from within a .zip, .jar, .apk, ...
DexFile* DexFile::OpenZip(const std::string& filename) {

  // First, look for a ".dex" alongside the jar file.  It will have
  // the same name/path except for the extension.

  // Example filename = dir/foo.jar
  std::string adjacent_dex_filename(filename);
  size_t found = adjacent_dex_filename.find_last_of(".");
  if (found == std::string::npos) {
    LOG(WARNING) << "No . in filename" << filename;
  }
  adjacent_dex_filename.replace(adjacent_dex_filename.begin() + found,
                                adjacent_dex_filename.end(),
                                ".dex");
  // Example adjacent_dex_filename = dir/foo.dex
  DexFile* adjacent_dex_file = DexFile::OpenFile(adjacent_dex_filename);
  if (adjacent_dex_file != NULL) {
      // We don't verify anything in this case, because we aren't in
      // the cache and typically the file is in the readonly /system
      // area, so if something is wrong, there is nothing we can do.
      return adjacent_dex_file;
  }

  char resolved[PATH_MAX];
  char* absolute_path = realpath(filename.c_str(), resolved);
  if (absolute_path == NULL) {
      LOG(WARNING) << "Could not create absolute path for " << filename
                   << " when looking for classes.dex";
      return NULL;
  }
  std::string cache_file(absolute_path+1); // skip leading slash
  std::replace(cache_file.begin(), cache_file.end(), '/', '@');
  cache_file.push_back('@');
  cache_file.append(kClassesDex);
  // Example cache_file = parent@dir@foo.jar@classes.dex

  const char* data_root = getenv("ANDROID_DATA");
  if (data_root == NULL) {
      data_root = "/data";
  }

  std::string cache_path_tmp = StringPrintf("%s/art-cache/%s", data_root, cache_file.c_str());
  // Example cache_path_tmp = /data/art-cache/parent@dir@foo.jar@classes.dex

  scoped_ptr<ZipArchive> zip_archive(ZipArchive::Open(filename));
  if (zip_archive == NULL) {
    LOG(WARNING) << "Could not open " << filename << " when looking for classes.dex";
    return NULL;
  }
  scoped_ptr<ZipEntry> zip_entry(zip_archive->Find(kClassesDex));
  if (zip_entry == NULL) {
    LOG(WARNING) << "Could not find classes.dex within " << filename;
    return NULL;
  }

  std::string cache_path = StringPrintf("%s.%08x", cache_path_tmp.c_str(), zip_entry->GetCrc32());
  // Example cache_path = /data/art-cache/parent@dir@foo.jar@classes.dex.1a2b3c4d

  while (true) {
    DexFile* cached_dex_file = DexFile::OpenFile(cache_path);
    if (cached_dex_file != NULL) {
      return cached_dex_file;
    }

    // Try to open the temporary cache file, grabbing an exclusive
    // lock. If somebody else is working on it, we'll block here until
    // they complete.  Because we're waiting on an external resource,
    // we go into native mode.
    Thread* current_thread = Thread::Current();
    Thread::State old = current_thread->GetState();
    current_thread->SetState(Thread::kNative);
    scoped_ptr<LockedFd> fd(LockedFd::CreateAndLock(cache_path_tmp, 0644));
    current_thread->SetState(old);
    if (fd == NULL) {
      return NULL;
    }

    // Check to see if the fd we opened and locked matches the file in
    // the filesystem.  If they don't, then somebody else unlinked
    // ours and created a new file, and we need to use that one
    // instead.  (If we caught them between the unlink and the create,
    // we'll get an ENOENT from the file stat.)
    struct stat fd_stat;
    int fd_stat_result = fstat(fd->GetFd(), &fd_stat);
    if (fd_stat_result == -1) {
      PLOG(ERROR) << "Can't stat open file '" << cache_path_tmp << "'";
      return NULL;
    }
    struct stat file_stat;
    int file_stat_result = stat(cache_path_tmp.c_str(), &file_stat);
    if (file_stat_result == -1 ||
        fd_stat.st_dev != file_stat.st_dev || fd_stat.st_ino != file_stat.st_ino) {
      LOG(WARNING) << "our open cache file is stale; sleeping and retrying";
      usleep(250 * 1000);  // if something is hosed, don't peg machine
      continue;
    }

    // We have the correct file open and locked. Extract classes.dex
    TmpFile tmp_file(cache_path_tmp);
    bool success = zip_entry->Extract(fd->GetFd());
    if (!success) {
      return NULL;
    }

    // TODO restat and check length against zip_entry->GetUncompressedLength()?

    // Compute checksum and compare to zip. If things look okay, rename from tmp.
    off_t lseek_result = lseek(fd->GetFd(), 0, SEEK_SET);
    if (lseek_result == -1) {
      return NULL;
    }
    const size_t kBufSize = 32768;
    scoped_ptr<uint8_t> buf(new uint8_t[kBufSize]);
    if (buf == NULL) {
      return NULL;
    }
    uint32_t computed_crc = crc32(0L, Z_NULL, 0);
    while (true) {
      ssize_t bytes_read = TEMP_FAILURE_RETRY(read(fd->GetFd(), buf.get(), kBufSize));
      if (bytes_read == 0) {
        break;
      }
      computed_crc = crc32(computed_crc, buf.get(), bytes_read);
    }
    if (computed_crc != zip_entry->GetCrc32()) {
      return NULL;
    }
    int rename_result = rename(cache_path_tmp.c_str(), cache_path.c_str());
    if (rename_result == -1) {
      PLOG(ERROR) << "Can't install dex cache file '" << cache_path << "' from '" << cache_path_tmp;
      unlink(cache_path.c_str());
    }
  }
  // NOTREACHED
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
