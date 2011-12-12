// Copyright 2011 Google Inc. All Rights Reserved.

#include "dex_file.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <map>

#include "UniquePtr.h"
#include "class_linker.h"
#include "globals.h"
#include "leb128.h"
#include "logging.h"
#include "object.h"
#include "os.h"
#include "stringprintf.h"
#include "thread.h"
#include "utf.h"
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

void DexFile::OpenDexFiles(const std::vector<const char*>& dex_filenames,
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
  if (IsValidZipFilename(filename)) {
    return DexFile::OpenZip(filename, strip_location_prefix);
  }
  if (!IsValidDexFilename(filename)) {
    LOG(WARNING) << "Attempting to open dex file with unknown extension '" << filename << "'";
  }
  return DexFile::OpenFile(filename, filename, strip_location_prefix);
}

void DexFile::ChangePermissions(int prot) const {
  if (mprotect(mem_map_->GetAddress(), mem_map_->GetLength(), prot) != 0) {
    PLOG(FATAL) << "Failed to change dex file permissions to " << prot << " for " << GetLocation();
  }
}

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
  UniquePtr<MemMap> map(MemMap::Map(length, PROT_READ, MAP_PRIVATE, fd, 0));
  if (map.get() == NULL) {
    LOG(ERROR) << "mmap \"" << filename << "\" failed";
    close(fd);
    return NULL;
  }
  close(fd);
  byte* dex_file = map->GetAddress();
  return OpenMemory(dex_file, length, location.ToString(), map.release());
}

const char* DexFile::kClassesDex = "classes.dex";

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
    if (result == -1) {
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
  explicit LockedFd(int fd) : fd_(fd) {}

  int fd_;
};

class TmpFile {
 public:
  explicit TmpFile(const std::string& name) : name_(name) {}
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

  UniquePtr<char[]> resolved(new char[PATH_MAX]);
  char* absolute_path = realpath(filename.c_str(), resolved.get());
  if (absolute_path == NULL) {
      LOG(ERROR) << "Failed to create absolute path for " << filename
                 << " when looking for classes.dex";
      return NULL;
  }
  std::string cache_path_tmp(GetArtCacheFilenameOrDie(absolute_path));
  cache_path_tmp.push_back('@');
  cache_path_tmp.append(kClassesDex);
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

  bool created = false;
  while (true) {
    if (OS::FileExists(cache_path.c_str())) {
      const DexFile* cached_dex_file = DexFile::OpenFile(cache_path,
                                                         filename,
                                                         strip_location_prefix);
      if (cached_dex_file != NULL) {
        return cached_dex_file;
      }
      if (created) {
        // We created the dex file with the correct checksum,
        // there must be something wrong with the file itself.
        return NULL;
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
      PLOG(ERROR) << "Failed to lock file '" << cache_path_tmp << "'";
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
      LOG(ERROR) << "Failed to create file for '" << cache_path_tmp << "'";
      return NULL;
    }
    bool success = zip_entry->Extract(*file);
    if (!success) {
      LOG(ERROR) << "Failed to extract classes.dex to '" << cache_path_tmp << "'";
      return NULL;
    }

    // TODO: restat and check length against zip_entry->GetUncompressedLength()?

    // Compute checksum and compare to zip. If things look okay, rename from tmp.
    off_t lseek_result = lseek(fd->GetFd(), 0, SEEK_SET);
    if (lseek_result == -1) {
      PLOG(ERROR) << "Failed to seek to start of '" << cache_path_tmp << "'";
      return NULL;
    }
    const size_t kBufSize = 32768;
    UniquePtr<uint8_t[]> buf(new uint8_t[kBufSize]);
    if (buf.get() == NULL) {
      LOG(ERROR) << "Failed to allocate buffer to checksum '" << cache_path_tmp << "'";
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
      LOG(ERROR) << "Failed to validate checksum for '" << cache_path_tmp << "'";
      return NULL;
    }
    int rename_result = rename(cache_path_tmp.c_str(), cache_path.c_str());
    if (rename_result == -1) {
      PLOG(ERROR) << "Failed to install dex cache file '" << cache_path << "'"
                  << " from '" << cache_path_tmp << "'";
      unlink(cache_path.c_str());
    } else {
      created = true;
    }
  }
  // NOTREACHED
}

const DexFile* DexFile::OpenMemory(const byte* dex_bytes, size_t length,
                                   const std::string& location, MemMap* mem_map) {
  UniquePtr<DexFile> dex_file(new DexFile(dex_bytes, length, location, mem_map));
  if (!dex_file->Init()) {
    return NULL;
  } else {
    return dex_file.release();
  }
}

DexFile::~DexFile() {
  // We don't call DeleteGlobalRef on dex_object_ because we're only called by DestroyJavaVM, and
  // that's only called after DetachCurrentThread, which means there's no JNIEnv. We could
  // re-attach, but cleaning up these global references is not obviously useful. It's not as if
  // the global reference table is otherwise empty!
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
  CHECK(magic != NULL) << GetLocation();
  if (memcmp(magic, kDexMagic, sizeof(kDexMagic)) != 0) {
    LOG(ERROR) << "Unrecognized magic number in "  << GetLocation() << ":"
            << " " << magic[0]
            << " " << magic[1]
            << " " << magic[2]
            << " " << magic[3];
    return false;
  }
  const byte* version = &magic[sizeof(kDexMagic)];
  if (memcmp(version, kDexMagicVersion, sizeof(kDexMagicVersion)) != 0) {
    LOG(ERROR) << "Unrecognized version number in "  << GetLocation() << ":"
            << " " << version[0]
            << " " << version[1]
            << " " << version[2]
            << " " << version[3];
    return false;
  }
  return true;
}

uint32_t DexFile::GetVersion() const {
  const char* version = reinterpret_cast<const char*>(&GetHeader().magic_[sizeof(kDexMagic)]);
  return atoi(version);
}

int32_t DexFile::GetStringLength(const StringId& string_id) const {
  const byte* ptr = base_ + string_id.string_data_off_;
  return DecodeUnsignedLeb128(&ptr);
}

// Returns a pointer to the UTF-8 string data referred to by the given string_id.
const char* DexFile::GetStringDataAndLength(const StringId& string_id, int32_t* length) const {
  CHECK(length != NULL) << GetLocation();
  const byte* ptr = base_ + string_id.string_data_off_;
  *length = DecodeUnsignedLeb128(&ptr);
  return reinterpret_cast<const char*>(ptr);
}

void DexFile::InitIndex() {
  CHECK_EQ(index_.size(), 0U) << GetLocation();
  for (size_t i = 0; i < NumClassDefs(); ++i) {
    const ClassDef& class_def = GetClassDef(i);
    const char* descriptor = GetClassDescriptor(class_def);
    index_[descriptor] = i;
  }
}

bool DexFile::FindClassDefIndex(const StringPiece& descriptor, uint32_t& idx) const {
  Index::const_iterator it = index_.find(descriptor);
  if (it == index_.end()) {
    return false;
  }
  idx = it->second;
  return true;
}

const DexFile::ClassDef* DexFile::FindClassDef(const StringPiece& descriptor) const {
  uint32_t idx;
  if (FindClassDefIndex(descriptor, idx)) {
    return &GetClassDef(idx);
  }
  return NULL;
}

const DexFile::FieldId* DexFile::FindFieldId(const DexFile::TypeId& declaring_klass,
                                              const DexFile::StringId& name,
                                              const DexFile::TypeId& type) const {
  // Binary search MethodIds knowing that they are sorted by class_idx, name_idx then proto_idx
  const uint16_t class_idx = GetIndexForTypeId(declaring_klass);
  const uint32_t name_idx = GetIndexForStringId(name);
  const uint16_t type_idx = GetIndexForTypeId(type);
  uint32_t lo = 0;
  uint32_t hi = NumFieldIds() - 1;
  while (hi >= lo) {
    uint32_t mid = (hi + lo) / 2;
    const DexFile::FieldId& field = GetFieldId(mid);
    if (class_idx > field.class_idx_) {
      lo = mid + 1;
    } else if (class_idx < field.class_idx_) {
      hi = mid - 1;
    } else {
      if (name_idx > field.name_idx_) {
        lo = mid + 1;
      } else if (name_idx < field.name_idx_) {
        hi = mid - 1;
      } else {
        if (type_idx > field.type_idx_) {
          lo = mid + 1;
        } else if (type_idx < field.type_idx_) {
          hi = mid - 1;
        } else {
          return &field;
        }
      }
    }
  }
  return NULL;
}

const DexFile::MethodId* DexFile::FindMethodId(const DexFile::TypeId& declaring_klass,
                                               const DexFile::StringId& name,
                                               const DexFile::ProtoId& signature) const {
  // Binary search MethodIds knowing that they are sorted by class_idx, name_idx then proto_idx
  const uint16_t class_idx = GetIndexForTypeId(declaring_klass);
  const uint32_t name_idx = GetIndexForStringId(name);
  const uint16_t proto_idx = GetIndexForProtoId(signature);
  uint32_t lo = 0;
  uint32_t hi = NumMethodIds() - 1;
  while (hi >= lo) {
    uint32_t mid = (hi + lo) / 2;
    const DexFile::MethodId& method = GetMethodId(mid);
    if (class_idx > method.class_idx_) {
      lo = mid + 1;
    } else if (class_idx < method.class_idx_) {
      hi = mid - 1;
    } else {
      if (name_idx > method.name_idx_) {
        lo = mid + 1;
      } else if (name_idx < method.name_idx_) {
        hi = mid - 1;
      } else {
        if (proto_idx > method.proto_idx_) {
          lo = mid + 1;
        } else if (proto_idx < method.proto_idx_) {
          hi = mid - 1;
        } else {
          return &method;
        }
      }
    }
  }
  return NULL;
}

const DexFile::StringId* DexFile::FindStringId(const std::string& string) const {
  uint32_t lo = 0;
  uint32_t hi = NumStringIds() - 1;
  while (hi >= lo) {
    uint32_t mid = (hi + lo) / 2;
    int32_t length;
    const DexFile::StringId& str_id = GetStringId(mid);
    const char* str = GetStringDataAndLength(str_id, &length);
    int compare = CompareModifiedUtf8ToModifiedUtf8AsUtf16CodePointValues(string.c_str(), str);
    if (compare > 0) {
      lo = mid + 1;
    } else if (compare < 0) {
      hi = mid - 1;
    } else {
      return &str_id;
    }
  }
  return NULL;
}

const DexFile::TypeId* DexFile::FindTypeId(uint32_t string_idx) const {
  uint32_t lo = 0;
  uint32_t hi = NumTypeIds() - 1;
  while (hi >= lo) {
    uint32_t mid = (hi + lo) / 2;
    const TypeId& type_id = GetTypeId(mid);
    if (string_idx > type_id.descriptor_idx_) {
      lo = mid + 1;
    } else if (string_idx < type_id.descriptor_idx_) {
      hi = mid - 1;
    } else {
      return &type_id;
    }
  }
  return NULL;
}

const DexFile::ProtoId* DexFile::FindProtoId(uint16_t return_type_idx,
                                         const std::vector<uint16_t>& signature_type_idxs) const {
  uint32_t lo = 0;
  uint32_t hi = NumProtoIds() - 1;
  while (hi >= lo) {
    uint32_t mid = (hi + lo) / 2;
    const DexFile::ProtoId& proto = GetProtoId(mid);
    int compare = return_type_idx - proto.return_type_idx_;
    if (compare == 0) {
      DexFileParameterIterator it(*this, proto);
      size_t i = 0;
      while (it.HasNext() && i < signature_type_idxs.size() && compare == 0) {
        compare = signature_type_idxs[i] - it.GetTypeIdx();
        it.Next();
        i++;
      }
      if (compare == 0) {
        if (it.HasNext()) {
          compare = -1;
        } else if (i < signature_type_idxs.size()) {
          compare = 1;
        }
      }
    }
    if (compare > 0) {
      lo = mid + 1;
    } else if (compare < 0) {
      hi = mid - 1;
    } else {
      return &proto;
    }
  }
  return NULL;
}

// Given a signature place the type ids into the given vector
bool DexFile::CreateTypeList(uint16_t* return_type_idx, std::vector<uint16_t>* param_type_idxs,
                             const std::string& signature) const {
  if (signature[0] != '(') {
    return false;
  }
  size_t offset = 1;
  size_t end = signature.size();
  bool process_return = false;
  while (offset < end) {
    char c = signature[offset];
    offset++;
    if (c == ')') {
      process_return = true;
      continue;
    }
    std::string descriptor;
    descriptor += c;
    while (c == '[') {  // process array prefix
      if (offset >= end) {  // expect some descriptor following [
        return false;
      }
      c = signature[offset];
      offset++;
      descriptor += c;
    }
    if (c == 'L') {  // process type descriptors
      do {
        if (offset >= end) {  // unexpected early termination of descriptor
          return false;
        }
        c = signature[offset];
        offset++;
        descriptor += c;
      } while (c != ';');
    }
    const DexFile::StringId* string_id = FindStringId(descriptor);
    if (string_id == NULL) {
      return false;
    }
    const DexFile::TypeId* type_id = FindTypeId(GetIndexForStringId(*string_id));
    if (type_id == NULL) {
      return false;
    }
    uint16_t type_idx = GetIndexForTypeId(*type_id);
    if (!process_return) {
      param_type_idxs->push_back(type_idx);
    } else {
      *return_type_idx = type_idx;
      return offset == end;  // return true if the signature had reached a sensible end
    }
  }
  return false;  // failed to correctly parse return type
}

// Materializes the method descriptor for a method prototype.  Method
// descriptors are not stored directly in the dex file.  Instead, one
// must assemble the descriptor from references in the prototype.
std::string DexFile::CreateMethodSignature(uint32_t proto_idx, int32_t* unicode_length) const {
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
      const char* name = StringByTypeIdx(type_idx, &type_length);
      parameter_length += type_length;
      descriptor.append(name);
    }
  }
  descriptor.push_back(')');
  uint32_t return_type_idx = proto_id.return_type_idx_;
  int32_t return_type_length;
  const char* name = StringByTypeIdx(return_type_idx, &return_type_length);
  descriptor.append(name);
  if (unicode_length != NULL) {
    *unicode_length = parameter_length + return_type_length + 2;  // 2 for ( and )
  }
  return descriptor;
}


int32_t DexFile::GetLineNumFromPC(const art::Method* method, uint32_t rel_pc) const {
  // For native method, lineno should be -2 to indicate it is native. Note that
  // "line number == -2" is how libcore tells from StackTraceElement.
  if (method->GetCodeItemOffset() == 0) {
    return -2;
  }

  const CodeItem* code_item = GetCodeItem(method->GetCodeItemOffset());
  DCHECK(code_item != NULL) << GetLocation();

  // A method with no line number info should return -1
  LineNumFromPcContext context(rel_pc, -1);
  DecodeDebugInfo(code_item, method->IsStatic(), method->GetDexMethodIndex(), LineNumForPcCb,
                  NULL, &context);
  return context.line_num_;
}

int32_t DexFile::FindCatchHandlerOffset(const CodeItem &code_item, int32_t tries_size,
                                        uint32_t address){
  // Note: Signed type is important for max and min.
  int32_t min = 0;
  int32_t max = tries_size - 1;

  while (max >= min) {
    int32_t mid = (min + max) / 2;
    const TryItem* pTry = DexFile::GetTryItems(code_item, mid);
    uint32_t start = pTry->start_addr_;
    if (address < start) {
      max = mid - 1;
    } else {
      uint32_t end = start + pTry->insn_count_;
      if (address >= end) {
        min = mid + 1;
      } else {  // We have a winner!
        return (int32_t) pTry->handler_off_;
      }
    }
  }
  // No match.
  return -1;
}

void DexFile::DecodeDebugInfo0(const CodeItem* code_item, bool is_static, uint32_t method_idx,
                               DexDebugNewPositionCb posCb, DexDebugNewLocalCb local_cb,
                               void* cnxt, const byte* stream, LocalInfo* local_in_reg) const {
  uint32_t line = DecodeUnsignedLeb128(&stream);
  uint32_t parameters_size = DecodeUnsignedLeb128(&stream);
  uint16_t arg_reg = code_item->registers_size_ - code_item->ins_size_;
  uint32_t address = 0;
  bool need_locals = (local_cb != NULL);

  if (!is_static) {
    if (need_locals) {
      const char* descriptor = GetMethodDeclaringClassDescriptor(GetMethodId(method_idx));
      local_in_reg[arg_reg].name_ = "this";
      local_in_reg[arg_reg].descriptor_ = descriptor;
      local_in_reg[arg_reg].signature_ = NULL;
      local_in_reg[arg_reg].start_address_ = 0;
      local_in_reg[arg_reg].is_live_ = true;
    }
    arg_reg++;
  }

  DexFileParameterIterator it(*this, GetMethodPrototype(GetMethodId(method_idx)));
  for (uint32_t i = 0; i < parameters_size && it.HasNext(); ++i, it.Next()) {
    if (arg_reg >= code_item->registers_size_) {
      LOG(ERROR) << "invalid stream - arg reg >= reg size (" << arg_reg
                 << " >= " << code_item->registers_size_ << ") in " << GetLocation();
      return;
    }
    uint32_t id = DecodeUnsignedLeb128P1(&stream);
    const char* descriptor = it.GetDescriptor();
    if (need_locals && id != kDexNoIndex) {
      const char* name = StringDataByIdx(id);
      local_in_reg[arg_reg].name_ = name;
      local_in_reg[arg_reg].descriptor_ = descriptor;
      local_in_reg[arg_reg].signature_ = NULL;
      local_in_reg[arg_reg].start_address_ = address;
      local_in_reg[arg_reg].is_live_ = true;
    }
    switch (*descriptor) {
      case 'D':
      case 'J':
        arg_reg += 2;
        break;
      default:
        arg_reg += 1;
        break;
    }
  }

  if (it.HasNext()) {
    LOG(ERROR) << "invalid stream - problem with parameter iterator in " << GetLocation();
    return;
  }

  for (;;)  {
    uint8_t opcode = *stream++;
    uint16_t reg;
    uint16_t name_idx;
    uint16_t descriptor_idx;
    uint16_t signature_idx = 0;

    switch (opcode) {
      case DBG_END_SEQUENCE:
        return;

      case DBG_ADVANCE_PC:
        address += DecodeUnsignedLeb128(&stream);
        break;

      case DBG_ADVANCE_LINE:
        line += DecodeSignedLeb128(&stream);
        break;

      case DBG_START_LOCAL:
      case DBG_START_LOCAL_EXTENDED:
        reg = DecodeUnsignedLeb128(&stream);
        if (reg > code_item->registers_size_) {
          LOG(ERROR) << "invalid stream - reg > reg size (" << reg << " > "
                     << code_item->registers_size_ << ") in " << GetLocation();
          return;
        }

        name_idx = DecodeUnsignedLeb128P1(&stream);
        descriptor_idx = DecodeUnsignedLeb128P1(&stream);
        if (opcode == DBG_START_LOCAL_EXTENDED) {
          signature_idx = DecodeUnsignedLeb128P1(&stream);
        }

        // Emit what was previously there, if anything
        if (need_locals) {
          InvokeLocalCbIfLive(cnxt, reg, address, local_in_reg, local_cb);

          local_in_reg[reg].name_ = StringDataByIdx(name_idx);
          local_in_reg[reg].descriptor_ = StringByTypeIdx(descriptor_idx);
          if (opcode == DBG_START_LOCAL_EXTENDED) {
            local_in_reg[reg].signature_ = StringDataByIdx(signature_idx);
          }
          local_in_reg[reg].start_address_ = address;
          local_in_reg[reg].is_live_ = true;
        }
        break;

      case DBG_END_LOCAL:
        reg = DecodeUnsignedLeb128(&stream);
        if (reg > code_item->registers_size_) {
          LOG(ERROR) << "invalid stream - reg > reg size (" << reg << " > "
                     << code_item->registers_size_ << ") in " << GetLocation();
          return;
        }

        if (need_locals) {
          InvokeLocalCbIfLive(cnxt, reg, address, local_in_reg, local_cb);
          local_in_reg[reg].is_live_ = false;
        }
        break;

      case DBG_RESTART_LOCAL:
        reg = DecodeUnsignedLeb128(&stream);
        if (reg > code_item->registers_size_) {
          LOG(ERROR) << "invalid stream - reg > reg size (" << reg << " > "
                     << code_item->registers_size_ << ") in " << GetLocation();
          return;
        }

        if (need_locals) {
          if (local_in_reg[reg].name_ == NULL || local_in_reg[reg].descriptor_ == NULL) {
            LOG(ERROR) << "invalid stream - no name or descriptor in " << GetLocation();
            return;
          }

          // If the register is live, the "restart" is superfluous,
          // and we don't want to mess with the existing start address.
          if (!local_in_reg[reg].is_live_) {
            local_in_reg[reg].start_address_ = address;
            local_in_reg[reg].is_live_ = true;
          }
        }
        break;

      case DBG_SET_PROLOGUE_END:
      case DBG_SET_EPILOGUE_BEGIN:
      case DBG_SET_FILE:
        break;

      default: {
        int adjopcode = opcode - DBG_FIRST_SPECIAL;

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
}

void DexFile::DecodeDebugInfo(const CodeItem* code_item, bool is_static, uint32_t method_idx,
                                 DexDebugNewPositionCb posCb, DexDebugNewLocalCb local_cb,
                                 void* cnxt) const {
  const byte* stream = GetDebugInfoStream(code_item);
  LocalInfo local_in_reg[code_item->registers_size_];

  if (stream != NULL) {
    DecodeDebugInfo0(code_item, is_static, method_idx, posCb, local_cb, cnxt, stream, local_in_reg);
  }
  for (int reg = 0; reg < code_item->registers_size_; reg++) {
    InvokeLocalCbIfLive(cnxt, reg, code_item->insns_size_in_code_units_, local_in_reg, local_cb);
  }
}

bool DexFile::LineNumForPcCb(void* cnxt, uint32_t address, uint32_t line_num) {
  LineNumFromPcContext* context = (LineNumFromPcContext*) cnxt;

  // We know that this callback will be called in
  // ascending address order, so keep going until we find
  // a match or we've just gone past it.
  if (address > context->address_) {
    // The line number from the previous positions callback
    // wil be the final result.
    return true;
  } else {
    context->line_num_ = line_num;
    return address == context->address_;
  }
}

// Decodes the header section from the class data bytes.
void ClassDataItemIterator::ReadClassDataHeader() {
  CHECK(ptr_pos_ != NULL);
  header_.static_fields_size_ = DecodeUnsignedLeb128(&ptr_pos_);
  header_.instance_fields_size_ = DecodeUnsignedLeb128(&ptr_pos_);
  header_.direct_methods_size_ = DecodeUnsignedLeb128(&ptr_pos_);
  header_.virtual_methods_size_ = DecodeUnsignedLeb128(&ptr_pos_);
}

void ClassDataItemIterator::ReadClassDataField() {
  field_.field_idx_delta_ = DecodeUnsignedLeb128(&ptr_pos_);
  field_.access_flags_ = DecodeUnsignedLeb128(&ptr_pos_);
}

void ClassDataItemIterator::ReadClassDataMethod() {
  method_.method_idx_delta_ = DecodeUnsignedLeb128(&ptr_pos_);
  method_.access_flags_ = DecodeUnsignedLeb128(&ptr_pos_);
  method_.code_off_ = DecodeUnsignedLeb128(&ptr_pos_);
}

// Read a signed integer.  "zwidth" is the zero-based byte count.
static int32_t ReadSignedInt(const byte* ptr, int zwidth) {
  int32_t val = 0;
  for (int i = zwidth; i >= 0; --i) {
    val = ((uint32_t)val >> 8) | (((int32_t)*ptr++) << 24);
  }
  val >>= (3 - zwidth) * 8;
  return val;
}

// Read an unsigned integer.  "zwidth" is the zero-based byte count,
// "fill_on_right" indicates which side we want to zero-fill from.
static uint32_t ReadUnsignedInt(const byte* ptr, int zwidth, bool fill_on_right) {
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
static uint64_t ReadUnsignedLong(const byte* ptr, int zwidth, bool fill_on_right) {
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

EncodedStaticFieldValueIterator::EncodedStaticFieldValueIterator(const DexFile& dex_file,
    DexCache* dex_cache, ClassLinker* linker, const DexFile::ClassDef& class_def) :
    dex_file_(dex_file), dex_cache_(dex_cache), linker_(linker), array_size_(), pos_(-1), type_(0) {
  ptr_ = dex_file.GetEncodedStaticFieldValuesArray(class_def);
  if (ptr_ == NULL) {
    array_size_ = 0;
  } else {
    array_size_ = DecodeUnsignedLeb128(&ptr_);
  }
  if (array_size_ > 0) {
    Next();
  }
}

void EncodedStaticFieldValueIterator::Next() {
  pos_++;
  if (pos_ >= array_size_) {
    return;
  }
  byte value_type = *ptr_++;
  byte value_arg = value_type >> kEncodedValueArgShift;
  size_t width = value_arg + 1;  // assume and correct later
  type_ = value_type & kEncodedValueTypeMask;
  switch (type_) {
  case kBoolean:
    jval_.i = (value_arg != 0) ? 1 : 0;
    width = 0;
    break;
  case kByte:
    jval_.i = ReadSignedInt(ptr_, value_arg);
    CHECK(IsInt(8, jval_.i));
    break;
  case kShort:
    jval_.i = ReadSignedInt(ptr_, value_arg);
    CHECK(IsInt(16, jval_.i));
    break;
  case kChar:
    jval_.i = ReadUnsignedInt(ptr_, value_arg, false);
    CHECK(IsUint(16, jval_.i));
    break;
  case kInt:
    jval_.i = ReadSignedInt(ptr_, value_arg);
    break;
  case kLong:
    jval_.j = ReadSignedLong(ptr_, value_arg);
    break;
  case kFloat:
    jval_.i = ReadUnsignedInt(ptr_, value_arg, true);
    break;
  case kDouble:
    jval_.j = ReadUnsignedLong(ptr_, value_arg, true);
    break;
  case kString:
  case kType:
  case kMethod:
  case kEnum:
    jval_.i = ReadUnsignedInt(ptr_, value_arg, false);
    break;
  case kField:
  case kArray:
  case kAnnotation:
    UNIMPLEMENTED(FATAL) << ": type " << type_;
    break;
  case kNull:
    jval_.l = NULL;
    width = 0;
    break;
  default:
    LOG(FATAL) << "Unreached";
  }
  ptr_ += width;
}

void EncodedStaticFieldValueIterator::ReadValueToField(Field* field) const {
  switch (type_) {
    case kBoolean: field->SetBoolean(NULL, jval_.z); break;
    case kByte:    field->SetByte(NULL, jval_.b); break;
    case kShort:   field->SetShort(NULL, jval_.s); break;
    case kChar:    field->SetChar(NULL, jval_.c); break;
    case kInt:     field->SetInt(NULL, jval_.i); break;
    case kLong:    field->SetLong(NULL, jval_.j); break;
    case kFloat:   field->SetFloat(NULL, jval_.f); break;
    case kDouble:  field->SetDouble(NULL, jval_.d); break;
    case kNull:    field->SetObject(NULL, NULL); break;
    case kString: {
      String* resolved = linker_->ResolveString(dex_file_, jval_.i, dex_cache_);
      field->SetObject(NULL, resolved);
      break;
    }
    default: UNIMPLEMENTED(FATAL) << ": type " << type_;
  }
}

CatchHandlerIterator::CatchHandlerIterator(const DexFile::CodeItem& code_item, uint32_t address) {
  handler_.address_ = -1;
  int32_t offset = -1;

  // Short-circuit the overwhelmingly common cases.
  switch (code_item.tries_size_) {
    case 0:
      break;
    case 1: {
      const DexFile::TryItem* tries = DexFile::GetTryItems(code_item, 0);
      uint32_t start = tries->start_addr_;
      if (address >= start) {
        uint32_t end = start + tries->insn_count_;
        if (address < end) {
          offset = tries->handler_off_;
        }
      }
      break;
    }
    default:
      offset = DexFile::FindCatchHandlerOffset(code_item, code_item.tries_size_, address);
  }
  if (offset >= 0) {
    const byte* handler_data = DexFile::GetCatchHandlerData(code_item, offset);
    Init(handler_data);
  } else {
    // Not found, initialize as empty
    current_data_ = NULL;
    remaining_count_ = -1;
    catch_all_ = false;
    DCHECK(!HasNext());
  }
}

void CatchHandlerIterator::Init(const byte* handler_data) {
  current_data_ = handler_data;
  remaining_count_ = DecodeSignedLeb128(&current_data_);

  // If remaining_count_ is non-positive, then it is the negative of
  // the number of catch types, and the catches are followed by a
  // catch-all handler.
  if (remaining_count_ <= 0) {
    catch_all_ = true;
    remaining_count_ = -remaining_count_;
  } else {
    catch_all_ = false;
  }
  Next();
}

void CatchHandlerIterator::Next() {
  if (remaining_count_ > 0) {
    handler_.type_idx_ = DecodeUnsignedLeb128(&current_data_);
    handler_.address_  = DecodeUnsignedLeb128(&current_data_);
    remaining_count_--;
    return;
  }

  if (catch_all_) {
    handler_.type_idx_ = DexFile::kDexNoIndex16;
    handler_.address_  = DecodeUnsignedLeb128(&current_data_);
    catch_all_ = false;
    return;
  }

  // no more handler
  remaining_count_ = -1;
}

}  // namespace art
