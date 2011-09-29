// Copyright 2011 Google Inc. All Rights Reserved.

#include "dex_file.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <map>

#include "UniquePtr.h"
#include "globals.h"
#include "logging.h"
#include "object.h"
#include "os.h"
#include "stringprintf.h"
#include "thread.h"
#include "utils.h"
#include "zip_archive.h"

namespace art {

const byte DexFile::kDexMagic[] = { 'd', 'e', 'x', '\n' };
const byte DexFile::kDexMagicVersion[] = { '0', '3', '5', '\0' };

DexFile::ClassPathEntry DexFile::FindInClassPath(const StringPiece& descriptor,
                                                 const ClassPath& class_path) {
  for (size_t i = 0; i != class_path.size(); ++i) {
    const DexFile* dex_file = class_path[i];
    const DexFile::ClassDef* dex_class_def = dex_file->FindClassDef(descriptor);
    if (dex_class_def != NULL) {
      return ClassPathEntry(dex_file, dex_class_def);
    }
  }
  // TODO: remove reinterpret_cast when issue with -std=gnu++0x host issue resolved
  return ClassPathEntry(reinterpret_cast<const DexFile*>(NULL),
                        reinterpret_cast<const DexFile::ClassDef*>(NULL));
}

void DexFile::OpenDexFiles(std::vector<const char*>& dex_filenames,
                           std::vector<const DexFile*>& dex_files,
                           const std::string& strip_location_prefix) {
  for (size_t i = 0; i < dex_filenames.size(); i++) {
    const char* dex_filename = dex_filenames[i];
    const DexFile* dex_file = Open(dex_filename, strip_location_prefix);
    if (dex_file == NULL) {
      fprintf(stderr, "could not open .dex from file %s\n", dex_filename);
      exit(EXIT_FAILURE);
    }
    dex_files.push_back(dex_file);
  }
}

const DexFile* DexFile::Open(const std::string& filename,
                             const std::string& strip_location_prefix) {
  if (filename.size() < 4) {
    LOG(WARNING) << "Ignoring short classpath entry '" << filename << "'";
    return NULL;
  }
  std::string suffix(filename.substr(filename.size() - 4));
  if (suffix == ".zip" || suffix == ".jar" || suffix == ".apk") {
    return DexFile::OpenZip(filename, strip_location_prefix);
  } else {
    return DexFile::OpenFile(filename, filename, strip_location_prefix);
  }
}

void DexFile::ChangePermissions(int prot) const {
  closer_->ChangePermissions(prot);
}

DexFile::Closer::~Closer() {}

DexFile::MmapCloser::MmapCloser(void* addr, size_t length) : addr_(addr), length_(length) {
  CHECK(addr != NULL);
}
DexFile::MmapCloser::~MmapCloser() {
  if (munmap(addr_, length_) == -1) {
    PLOG(INFO) << "munmap failed";
  }
}
void DexFile::MmapCloser::ChangePermissions(int prot) {
  if (mprotect(addr_, length_, prot) != 0) {
    PLOG(FATAL) << "Failed to change dex file permissions to " << prot;
  }
}

DexFile::PtrCloser::PtrCloser(byte* addr) : addr_(addr) {}
DexFile::PtrCloser::~PtrCloser() { delete[] addr_; }
void DexFile::PtrCloser::ChangePermissions(int prot) {}

const DexFile* DexFile::OpenFile(const std::string& filename,
                                 const std::string& original_location,
                                 const std::string& strip_location_prefix) {
  StringPiece location = original_location;
  if (!location.starts_with(strip_location_prefix)) {
    LOG(ERROR) << filename << " does not start with " << strip_location_prefix;
    return NULL;
  }
  location.remove_prefix(strip_location_prefix.size());
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
  void* addr = mmap(NULL, length, PROT_READ, MAP_PRIVATE, fd, 0);
  if (addr == MAP_FAILED) {
    PLOG(ERROR) << "mmap \"" << filename << "\" failed";
    close(fd);
    return NULL;
  }
  close(fd);
  byte* dex_file = reinterpret_cast<byte*>(addr);
  Closer* closer = new MmapCloser(addr, length);
  return Open(dex_file, length, location.ToString(), closer);
}

static const char* kClassesDex = "classes.dex";

class LockedFd {
 public:
   static LockedFd* CreateAndLock(std::string& name, mode_t mode) {
    int fd = open(name.c_str(), O_CREAT | O_RDWR, mode);
    if (fd == -1) {
      PLOG(ERROR) << "Failed to open file '" << name << "'";
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
      PLOG(ERROR) << "Failed to lock file '" << name << "'";
      close(fd);
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
const DexFile* DexFile::OpenZip(const std::string& filename,
                                const std::string& strip_location_prefix) {

  // First, look for a ".dex" alongside the jar file.  It will have
  // the same name/path except for the extension.

  // Example filename = dir/foo.jar
  std::string adjacent_dex_filename(filename);
  size_t found = adjacent_dex_filename.find_last_of(".");
  if (found == std::string::npos) {
    LOG(ERROR) << "No . in filename" << filename;
    return NULL;
  }
  adjacent_dex_filename.replace(adjacent_dex_filename.begin() + found,
                                adjacent_dex_filename.end(),
                                ".dex");
  // Example adjacent_dex_filename = dir/foo.dex
  if (OS::FileExists(adjacent_dex_filename.c_str())) {
    const DexFile* adjacent_dex_file = DexFile::OpenFile(adjacent_dex_filename,
                                                         filename,
                                                         strip_location_prefix);
    if (adjacent_dex_file != NULL) {
      // We don't verify anything in this case, because we aren't in
      // the cache and typically the file is in the readonly /system
      // area, so if something is wrong, there is nothing we can do.
      return adjacent_dex_file;
    }
    return NULL;
  }

  char resolved[PATH_MAX];
  char* absolute_path = realpath(filename.c_str(), resolved);
  if (absolute_path == NULL) {
      LOG(ERROR) << "Failed to create absolute path for " << filename
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
    if (OS::DirectoryExists("/data")) {
      data_root = "/data";
    } else {
      data_root = "/tmp";
    }
  }
  if (!OS::DirectoryExists(data_root)) {
    LOG(ERROR) << "Failed to find ANDROID_DATA directory " << data_root;
    return NULL;
  }

  std::string art_cache = StringPrintf("%s/art-cache", data_root);

  if (!OS::DirectoryExists(art_cache.c_str())) {
    if (StringPiece(art_cache).starts_with("/tmp/")) {
      int result = mkdir(art_cache.c_str(), 0700);
      if (result != 0) {
        LOG(FATAL) << "Failed to create art-cache directory " << art_cache;
        return NULL;
      }
    } else {
      LOG(FATAL) << "Failed to find art-cache directory " << art_cache;
      return NULL;
    }
  }

  std::string cache_path_tmp = StringPrintf("%s/%s", art_cache.c_str(), cache_file.c_str());
  // Example cache_path_tmp = /data/art-cache/parent@dir@foo.jar@classes.dex

  UniquePtr<ZipArchive> zip_archive(ZipArchive::Open(filename));
  if (zip_archive.get() == NULL) {
    LOG(ERROR) << "Failed to open " << filename << " when looking for classes.dex";
    return NULL;
  }
  UniquePtr<ZipEntry> zip_entry(zip_archive->Find(kClassesDex));
  if (zip_entry.get() == NULL) {
    LOG(ERROR) << "Failed to find classes.dex within " << filename;
    return NULL;
  }

  std::string cache_path = StringPrintf("%s.%08x", cache_path_tmp.c_str(), zip_entry->GetCrc32());
  // Example cache_path = /data/art-cache/parent@dir@foo.jar@classes.dex.1a2b3c4d

  while (true) {
    if (OS::FileExists(cache_path.c_str())) {
      const DexFile* cached_dex_file = DexFile::OpenFile(cache_path,
                                                         filename,
                                                         strip_location_prefix);
      if (cached_dex_file != NULL) {
        return cached_dex_file;
      }
    }

    // Try to open the temporary cache file, grabbing an exclusive
    // lock. If somebody else is working on it, we'll block here until
    // they complete.  Because we're waiting on an external resource,
    // we go into native mode.
    // Note that self can be NULL if we're parsing the bootclasspath
    // during JNI_CreateJavaVM.
    Thread* self = Thread::Current();
    UniquePtr<ScopedThreadStateChange> state_changer;
    if (self != NULL) {
      state_changer.reset(new ScopedThreadStateChange(self, Thread::kNative));
    }
    UniquePtr<LockedFd> fd(LockedFd::CreateAndLock(cache_path_tmp, 0644));
    state_changer.reset(NULL);
    if (fd.get() == NULL) {
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
      PLOG(ERROR) << "Failed to stat open file '" << cache_path_tmp << "'";
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
    UniquePtr<File> file(OS::FileFromFd(cache_path_tmp.c_str(), fd->GetFd()));
    if (file.get() == NULL) {
      return NULL;
    }
    bool success = zip_entry->Extract(*file);
    if (!success) {
      return NULL;
    }

    // TODO: restat and check length against zip_entry->GetUncompressedLength()?

    // Compute checksum and compare to zip. If things look okay, rename from tmp.
    off_t lseek_result = lseek(fd->GetFd(), 0, SEEK_SET);
    if (lseek_result == -1) {
      return NULL;
    }
    const size_t kBufSize = 32768;
    UniquePtr<uint8_t[]> buf(new uint8_t[kBufSize]);
    if (buf.get() == NULL) {
      return NULL;
    }
    uint32_t computed_crc = crc32(0L, Z_NULL, 0);
    while (true) {
      ssize_t bytes_read = TEMP_FAILURE_RETRY(read(fd->GetFd(), buf.get(), kBufSize));
      if (bytes_read == -1) {
        PLOG(ERROR) << "Problem computing CRC of '" << cache_path_tmp << "'";
        return NULL;
      }
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
      PLOG(ERROR) << "Failed to install dex cache file '" << cache_path << "'"
                  << " from '" << cache_path_tmp << "'";
      unlink(cache_path.c_str());
    }
  }
  // NOTREACHED
}

const DexFile* DexFile::OpenPtr(byte* ptr, size_t length, const std::string& location) {
  CHECK(ptr != NULL);
  DexFile::Closer* closer = new PtrCloser(ptr);
  return Open(ptr, length, location, closer);
}

const DexFile* DexFile::Open(const byte* dex_bytes, size_t length,
                             const std::string& location, Closer* closer) {
  UniquePtr<DexFile> dex_file(new DexFile(dex_bytes, length, location, closer));
  if (!dex_file->Init()) {
    return NULL;
  } else {
    return dex_file.release();
  }
}

DexFile::~DexFile() {
  if (dex_object_ != NULL) {
    UNIMPLEMENTED(WARNING) << "leaked a global reference to an com.android.dex.Dex instance";
  }
}

jobject DexFile::GetDexObject(JNIEnv* env) const {
  MutexLock mu(dex_object_lock_);
  if (dex_object_ != NULL) {
    return dex_object_;
  }

  void* address = const_cast<void*>(reinterpret_cast<const void*>(base_));
  jobject byte_buffer = env->NewDirectByteBuffer(address, length_);
  if (byte_buffer == NULL) {
    return NULL;
  }

  jclass c = env->FindClass("com/android/dex/Dex");
  if (c == NULL) {
    return NULL;
  }

  jmethodID mid = env->GetStaticMethodID(c, "create", "(Ljava/nio/ByteBuffer;)Lcom/android/dex/Dex;");
  if (mid == NULL) {
    return NULL;
  }

  jvalue args[1];
  args[0].l = byte_buffer;
  jobject local = env->CallStaticObjectMethodA(c, mid, args);
  if (local == NULL) {
    return NULL;
  }

  dex_object_ = env->NewGlobalRef(local);
  return dex_object_;
}

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
    LOG(ERROR) << "Unrecognized magic number:"
            << " " << magic[0]
            << " " << magic[1]
            << " " << magic[2]
            << " " << magic[3];
    return false;
  }
  const byte* version = &magic[sizeof(kDexMagic)];
  if (memcmp(version, kDexMagicVersion, sizeof(kDexMagicVersion)) != 0) {
    LOG(ERROR) << "Unrecognized version number:"
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
  Index::const_iterator it = index_.find(descriptor);
  if (it == index_.end()) {
    return NULL;
  } else {
    return it->second;
  }
}

// Materializes the method descriptor for a method prototype.  Method
// descriptors are not stored directly in the dex file.  Instead, one
// must assemble the descriptor from references in the prototype.
std::string DexFile::CreateMethodDescriptor(uint32_t proto_idx,
    int32_t* unicode_length) const {
  const ProtoId& proto_id = GetProtoId(proto_idx);
  std::string descriptor;
  descriptor.push_back('(');
  const TypeList* type_list = GetProtoParameters(proto_id);
  size_t parameter_length = 0;
  if (type_list != NULL) {
    // A non-zero number of arguments.  Append the type names.
    for (size_t i = 0; i < type_list->Size(); ++i) {
      const TypeItem& type_item = type_list->GetTypeItem(i);
      uint32_t type_idx = type_item.type_idx_;
      int32_t type_length;
      const char* name = dexStringByTypeIdx(type_idx, &type_length);
      parameter_length += type_length;
      descriptor.append(name);
    }
  }
  descriptor.push_back(')');
  uint32_t return_type_idx = proto_id.return_type_idx_;
  int32_t return_type_length;
  const char* name = dexStringByTypeIdx(return_type_idx, &return_type_length);
  descriptor.append(name);
  if (unicode_length != NULL) {
    *unicode_length = parameter_length + return_type_length + 2;  // 2 for ( and )
  }
  return descriptor;
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
      UNIMPLEMENTED(FATAL) << ": type " << type;
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

String* DexFile::dexArtStringById(int32_t idx) const {
  if (idx == -1) {
    return NULL;
  }
  return String::AllocFromModifiedUtf8(dexStringById(idx));
}

int32_t DexFile::GetLineNumFromPC(const art::Method* method, uint32_t rel_pc) const {
  // For native method, lineno should be -2 to indicate it is native. Note that
  // "line number == -2" is how libcore tells from StackTraceElement.
  if (method->GetCodeItemOffset() == 0) {
    return -2;
  }

  const CodeItem* code_item = GetCodeItem(method->GetCodeItemOffset());
  DCHECK(code_item != NULL);

  // A method with no line number info should return -1
  LineNumFromPcContext context(rel_pc, -1);
  dexDecodeDebugInfo(code_item, method, LineNumForPcCb, NULL, &context);
  return context.line_num_;
}

void DexFile::dexDecodeDebugInfo0(const CodeItem* code_item, const art::Method* method,
                         DexDebugNewPositionCb posCb, DexDebugNewLocalCb local_cb,
                         void* cnxt, const byte* stream, LocalInfo* local_in_reg) const {
  uint32_t line = DecodeUnsignedLeb128(&stream);
  uint32_t parameters_size = DecodeUnsignedLeb128(&stream);
  uint16_t arg_reg = code_item->registers_size_ - code_item->ins_size_;
  uint32_t address = 0;

  if (!method->IsStatic()) {
    local_in_reg[arg_reg].name_ = String::AllocFromModifiedUtf8("this");
    local_in_reg[arg_reg].descriptor_ = method->GetDeclaringClass()->GetDescriptor();
    local_in_reg[arg_reg].signature_ = NULL;
    local_in_reg[arg_reg].start_address_ = 0;
    local_in_reg[arg_reg].is_live_ = true;
    arg_reg++;
  }

  ParameterIterator *it = GetParameterIterator(GetProtoId(method->GetProtoIdx()));
  for (uint32_t i = 0; i < parameters_size && it->HasNext(); ++i, it->Next()) {
    if (arg_reg >= code_item->registers_size_) {
      LOG(FATAL) << "invalid stream";
      return;
    }

    String* descriptor = String::AllocFromModifiedUtf8(it->GetDescriptor());
    String* name = dexArtStringById(DecodeUnsignedLeb128P1(&stream));

    local_in_reg[arg_reg].name_ = name;
    local_in_reg[arg_reg].descriptor_ = descriptor;
    local_in_reg[arg_reg].signature_ = NULL;
    local_in_reg[arg_reg].start_address_ = address;
    local_in_reg[arg_reg].is_live_ = true;
    switch (descriptor->CharAt(0)) {
      case 'D':
      case 'J':
        arg_reg += 2;
        break;
      default:
        arg_reg += 1;
        break;
    }
  }

  if (it->HasNext()) {
    LOG(FATAL) << "invalid stream";
    return;
  }

  for (;;)  {
    uint8_t opcode = *stream++;
    uint8_t adjopcode = opcode - DBG_FIRST_SPECIAL;
    uint16_t reg;


    switch (opcode) {
      case DBG_END_SEQUENCE:
        return;

      case DBG_ADVANCE_PC:
        address += DecodeUnsignedLeb128(&stream);
        break;

      case DBG_ADVANCE_LINE:
        line += DecodeUnsignedLeb128(&stream);
        break;

      case DBG_START_LOCAL:
      case DBG_START_LOCAL_EXTENDED:
        reg = DecodeUnsignedLeb128(&stream);
        if (reg > code_item->registers_size_) {
          LOG(FATAL) << "invalid stream";
          return;
        }

        // Emit what was previously there, if anything
        InvokeLocalCbIfLive(cnxt, reg, address, local_in_reg, local_cb);

        local_in_reg[reg].name_ = dexArtStringById(DecodeUnsignedLeb128P1(&stream));
        local_in_reg[reg].descriptor_ = dexArtStringByTypeIdx(DecodeUnsignedLeb128P1(&stream));
        if (opcode == DBG_START_LOCAL_EXTENDED) {
          local_in_reg[reg].signature_ = dexArtStringById(DecodeUnsignedLeb128P1(&stream));
        } else {
          local_in_reg[reg].signature_ = NULL;
        }
        local_in_reg[reg].start_address_ = address;
        local_in_reg[reg].is_live_ = true;
        break;

      case DBG_END_LOCAL:
        reg = DecodeUnsignedLeb128(&stream);
        if (reg > code_item->registers_size_) {
          LOG(FATAL) << "invalid stream";
          return;
        }

        InvokeLocalCbIfLive(cnxt, reg, address, local_in_reg, local_cb);
        local_in_reg[reg].is_live_ = false;
        break;

      case DBG_RESTART_LOCAL:
        reg = DecodeUnsignedLeb128(&stream);
        if (reg > code_item->registers_size_) {
          LOG(FATAL) << "invalid stream";
          return;
        }

        if (local_in_reg[reg].name_ == NULL
            || local_in_reg[reg].descriptor_ == NULL) {
          LOG(FATAL) << "invalid stream";
          return;
        }

        // If the register is live, the "restart" is superfluous,
        // and we don't want to mess with the existing start address.
        if (!local_in_reg[reg].is_live_) {
          local_in_reg[reg].start_address_ = address;
          local_in_reg[reg].is_live_ = true;
        }
        break;

      case DBG_SET_PROLOGUE_END:
      case DBG_SET_EPILOGUE_BEGIN:
      case DBG_SET_FILE:
        break;

      default:
        address += adjopcode / DBG_LINE_RANGE;
        line += DBG_LINE_BASE + (adjopcode % DBG_LINE_RANGE);

        if (posCb != NULL) {
          if (posCb(cnxt, address, line)) {
            // early exit
            return;
          }
        }
        break;
    }
  }
}

}  // namespace art
