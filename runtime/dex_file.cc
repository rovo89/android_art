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

#include "dex_file.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>

#include "base/logging.h"
#include "base/stringprintf.h"
#include "class_linker.h"
#include "dex_file-inl.h"
#include "dex_file_verifier.h"
#include "globals.h"
#include "leb128.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/string.h"
#include "os.h"
#include "safe_map.h"
#include "ScopedFd.h"
#include "handle_scope-inl.h"
#include "thread.h"
#include "UniquePtrCompat.h"
#include "utf-inl.h"
#include "utils.h"
#include "well_known_classes.h"
#include "zip_archive.h"

namespace art {

const byte DexFile::kDexMagic[] = { 'd', 'e', 'x', '\n' };
const byte DexFile::kDexMagicVersion[] = { '0', '3', '5', '\0' };

DexFile::ClassPathEntry DexFile::FindInClassPath(const char* descriptor,
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

static int OpenAndReadMagic(const char* filename, uint32_t* magic, std::string* error_msg) {
  CHECK(magic != NULL);
  ScopedFd fd(open(filename, O_RDONLY, 0));
  if (fd.get() == -1) {
    *error_msg = StringPrintf("Unable to open '%s' : %s", filename, strerror(errno));
    return -1;
  }
  int n = TEMP_FAILURE_RETRY(read(fd.get(), magic, sizeof(*magic)));
  if (n != sizeof(*magic)) {
    *error_msg = StringPrintf("Failed to find magic in '%s'", filename);
    return -1;
  }
  if (lseek(fd.get(), 0, SEEK_SET) != 0) {
    *error_msg = StringPrintf("Failed to seek to beginning of file '%s' : %s", filename,
                              strerror(errno));
    return -1;
  }
  return fd.release();
}

bool DexFile::GetChecksum(const char* filename, uint32_t* checksum, std::string* error_msg) {
  CHECK(checksum != NULL);
  uint32_t magic;
  ScopedFd fd(OpenAndReadMagic(filename, &magic, error_msg));
  if (fd.get() == -1) {
    DCHECK(!error_msg->empty());
    return false;
  }
  if (IsZipMagic(magic)) {
    UniquePtr<ZipArchive> zip_archive(ZipArchive::OpenFromFd(fd.release(), filename, error_msg));
    if (zip_archive.get() == NULL) {
      *error_msg = StringPrintf("Failed to open zip archive '%s'", filename);
      return false;
    }
    UniquePtr<ZipEntry> zip_entry(zip_archive->Find(kClassesDex, error_msg));
    if (zip_entry.get() == NULL) {
      *error_msg = StringPrintf("Zip archive '%s' doesn't contain %s (error msg: %s)", filename,
                                kClassesDex, error_msg->c_str());
      return false;
    }
    *checksum = zip_entry->GetCrc32();
    return true;
  }
  if (IsDexMagic(magic)) {
    UniquePtr<const DexFile> dex_file(DexFile::OpenFile(fd.release(), filename, false, error_msg));
    if (dex_file.get() == NULL) {
      return false;
    }
    *checksum = dex_file->GetHeader().checksum_;
    return true;
  }
  *error_msg = StringPrintf("Expected valid zip or dex file: '%s'", filename);
  return false;
}

const DexFile* DexFile::Open(const char* filename,
                             const char* location,
                             std::string* error_msg) {
  uint32_t magic;
  ScopedFd fd(OpenAndReadMagic(filename, &magic, error_msg));
  if (fd.get() == -1) {
    DCHECK(!error_msg->empty());
    return NULL;
  }
  if (IsZipMagic(magic)) {
    return DexFile::OpenZip(fd.release(), location, error_msg);
  }
  if (IsDexMagic(magic)) {
    return DexFile::OpenFile(fd.release(), location, true, error_msg);
  }
  *error_msg = StringPrintf("Expected valid zip or dex file: '%s'", filename);
  return nullptr;
}

int DexFile::GetPermissions() const {
  if (mem_map_.get() == NULL) {
    return 0;
  } else {
    return mem_map_->GetProtect();
  }
}

bool DexFile::IsReadOnly() const {
  return GetPermissions() == PROT_READ;
}

bool DexFile::EnableWrite() const {
  CHECK(IsReadOnly());
  if (mem_map_.get() == NULL) {
    return false;
  } else {
    return mem_map_->Protect(PROT_READ | PROT_WRITE);
  }
}

bool DexFile::DisableWrite() const {
  CHECK(!IsReadOnly());
  if (mem_map_.get() == NULL) {
    return false;
  } else {
    return mem_map_->Protect(PROT_READ);
  }
}

const DexFile* DexFile::OpenFile(int fd, const char* location, bool verify,
                                 std::string* error_msg) {
  CHECK(location != nullptr);
  UniquePtr<MemMap> map;
  {
    ScopedFd delayed_close(fd);
    struct stat sbuf;
    memset(&sbuf, 0, sizeof(sbuf));
    if (fstat(fd, &sbuf) == -1) {
      *error_msg = StringPrintf("DexFile: fstat '%s' failed: %s", location, strerror(errno));
      return nullptr;
    }
    if (S_ISDIR(sbuf.st_mode)) {
      *error_msg = StringPrintf("Attempt to mmap directory '%s'", location);
      return nullptr;
    }
    size_t length = sbuf.st_size;
    map.reset(MemMap::MapFile(length, PROT_READ, MAP_PRIVATE, fd, 0, location, error_msg));
    if (map.get() == nullptr) {
      DCHECK(!error_msg->empty());
      return nullptr;
    }
  }

  if (map->Size() < sizeof(DexFile::Header)) {
    *error_msg = StringPrintf(
        "DexFile: failed to open dex file '%s' that is too short to have a header", location);
    return nullptr;
  }

  const Header* dex_header = reinterpret_cast<const Header*>(map->Begin());

  const DexFile* dex_file = OpenMemory(location, dex_header->checksum_, map.release(), error_msg);
  if (dex_file == nullptr) {
    *error_msg = StringPrintf("Failed to open dex file '%s' from memory: %s", location,
                              error_msg->c_str());
    return nullptr;
  }

  if (verify && !DexFileVerifier::Verify(dex_file, dex_file->Begin(), dex_file->Size(), location,
                                         error_msg)) {
    return nullptr;
  }

  return dex_file;
}

const char* DexFile::kClassesDex = "classes.dex";

const DexFile* DexFile::OpenZip(int fd, const std::string& location, std::string* error_msg) {
  UniquePtr<ZipArchive> zip_archive(ZipArchive::OpenFromFd(fd, location.c_str(), error_msg));
  if (zip_archive.get() == nullptr) {
    DCHECK(!error_msg->empty());
    return nullptr;
  }
  return DexFile::Open(*zip_archive, location, error_msg);
}

const DexFile* DexFile::OpenMemory(const std::string& location,
                                   uint32_t location_checksum,
                                   MemMap* mem_map,
                                   std::string* error_msg) {
  return OpenMemory(mem_map->Begin(),
                    mem_map->Size(),
                    location,
                    location_checksum,
                    mem_map,
                    error_msg);
}

const DexFile* DexFile::Open(const ZipArchive& zip_archive, const std::string& location,
                             std::string* error_msg) {
  CHECK(!location.empty());
  UniquePtr<ZipEntry> zip_entry(zip_archive.Find(kClassesDex, error_msg));
  if (zip_entry.get() == NULL) {
    return nullptr;
  }
  UniquePtr<MemMap> map(zip_entry->ExtractToMemMap(kClassesDex, error_msg));
  if (map.get() == NULL) {
    *error_msg = StringPrintf("Failed to extract '%s' from '%s': %s", kClassesDex, location.c_str(),
                              error_msg->c_str());
    return nullptr;
  }
  UniquePtr<const DexFile> dex_file(OpenMemory(location, zip_entry->GetCrc32(), map.release(),
                                               error_msg));
  if (dex_file.get() == nullptr) {
    *error_msg = StringPrintf("Failed to open dex file '%s' from memory: %s", location.c_str(),
                              error_msg->c_str());
    return nullptr;
  }
  if (!DexFileVerifier::Verify(dex_file.get(), dex_file->Begin(), dex_file->Size(),
                               location.c_str(), error_msg)) {
    return nullptr;
  }
  if (!dex_file->DisableWrite()) {
    *error_msg = StringPrintf("Failed to make dex file '%s' read only", location.c_str());
    return nullptr;
  }
  CHECK(dex_file->IsReadOnly()) << location;
  return dex_file.release();
}

const DexFile* DexFile::OpenMemory(const byte* base,
                                   size_t size,
                                   const std::string& location,
                                   uint32_t location_checksum,
                                   MemMap* mem_map, std::string* error_msg) {
  CHECK_ALIGNED(base, 4);  // various dex file structures must be word aligned
  UniquePtr<DexFile> dex_file(new DexFile(base, size, location, location_checksum, mem_map));
  if (!dex_file->Init(error_msg)) {
    return nullptr;
  } else {
    return dex_file.release();
  }
}

DexFile::DexFile(const byte* base, size_t size,
                 const std::string& location,
                 uint32_t location_checksum,
                 MemMap* mem_map)
    : begin_(base),
      size_(size),
      location_(location),
      location_checksum_(location_checksum),
      mem_map_(mem_map),
      header_(reinterpret_cast<const Header*>(base)),
      string_ids_(reinterpret_cast<const StringId*>(base + header_->string_ids_off_)),
      type_ids_(reinterpret_cast<const TypeId*>(base + header_->type_ids_off_)),
      field_ids_(reinterpret_cast<const FieldId*>(base + header_->field_ids_off_)),
      method_ids_(reinterpret_cast<const MethodId*>(base + header_->method_ids_off_)),
      proto_ids_(reinterpret_cast<const ProtoId*>(base + header_->proto_ids_off_)),
      class_defs_(reinterpret_cast<const ClassDef*>(base + header_->class_defs_off_)) {
  CHECK(begin_ != NULL) << GetLocation();
  CHECK_GT(size_, 0U) << GetLocation();
}

DexFile::~DexFile() {
  // We don't call DeleteGlobalRef on dex_object_ because we're only called by DestroyJavaVM, and
  // that's only called after DetachCurrentThread, which means there's no JNIEnv. We could
  // re-attach, but cleaning up these global references is not obviously useful. It's not as if
  // the global reference table is otherwise empty!
}

bool DexFile::Init(std::string* error_msg) {
  if (!CheckMagicAndVersion(error_msg)) {
    return false;
  }
  return true;
}

bool DexFile::CheckMagicAndVersion(std::string* error_msg) const {
  CHECK(header_->magic_ != NULL) << GetLocation();
  if (!IsMagicValid(header_->magic_)) {
    std::ostringstream oss;
    oss << "Unrecognized magic number in "  << GetLocation() << ":"
            << " " << header_->magic_[0]
            << " " << header_->magic_[1]
            << " " << header_->magic_[2]
            << " " << header_->magic_[3];
    *error_msg = oss.str();
    return false;
  }
  if (!IsVersionValid(header_->magic_)) {
    std::ostringstream oss;
    oss << "Unrecognized version number in "  << GetLocation() << ":"
            << " " << header_->magic_[4]
            << " " << header_->magic_[5]
            << " " << header_->magic_[6]
            << " " << header_->magic_[7];
    *error_msg = oss.str();
    return false;
  }
  return true;
}

bool DexFile::IsMagicValid(const byte* magic) {
  return (memcmp(magic, kDexMagic, sizeof(kDexMagic)) == 0);
}

bool DexFile::IsVersionValid(const byte* magic) {
  const byte* version = &magic[sizeof(kDexMagic)];
  return (memcmp(version, kDexMagicVersion, sizeof(kDexMagicVersion)) == 0);
}

uint32_t DexFile::GetVersion() const {
  const char* version = reinterpret_cast<const char*>(&GetHeader().magic_[sizeof(kDexMagic)]);
  return atoi(version);
}

const DexFile::ClassDef* DexFile::FindClassDef(const char* descriptor) const {
  size_t num_class_defs = NumClassDefs();
  if (num_class_defs == 0) {
    return NULL;
  }
  const StringId* string_id = FindStringId(descriptor);
  if (string_id == NULL) {
    return NULL;
  }
  const TypeId* type_id = FindTypeId(GetIndexForStringId(*string_id));
  if (type_id == NULL) {
    return NULL;
  }
  uint16_t type_idx = GetIndexForTypeId(*type_id);
  for (size_t i = 0; i < num_class_defs; ++i) {
    const ClassDef& class_def = GetClassDef(i);
    if (class_def.class_idx_ == type_idx) {
      return &class_def;
    }
  }
  return NULL;
}

const DexFile::ClassDef* DexFile::FindClassDef(uint16_t type_idx) const {
  size_t num_class_defs = NumClassDefs();
  for (size_t i = 0; i < num_class_defs; ++i) {
    const ClassDef& class_def = GetClassDef(i);
    if (class_def.class_idx_ == type_idx) {
      return &class_def;
    }
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
  int32_t lo = 0;
  int32_t hi = NumFieldIds() - 1;
  while (hi >= lo) {
    int32_t mid = (hi + lo) / 2;
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
  int32_t lo = 0;
  int32_t hi = NumMethodIds() - 1;
  while (hi >= lo) {
    int32_t mid = (hi + lo) / 2;
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

const DexFile::StringId* DexFile::FindStringId(const char* string) const {
  int32_t lo = 0;
  int32_t hi = NumStringIds() - 1;
  while (hi >= lo) {
    int32_t mid = (hi + lo) / 2;
    const DexFile::StringId& str_id = GetStringId(mid);
    const char* str = GetStringData(str_id);
    int compare = CompareModifiedUtf8ToModifiedUtf8AsUtf16CodePointValues(string, str);
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

const DexFile::StringId* DexFile::FindStringId(const uint16_t* string) const {
  int32_t lo = 0;
  int32_t hi = NumStringIds() - 1;
  while (hi >= lo) {
    int32_t mid = (hi + lo) / 2;
    const DexFile::StringId& str_id = GetStringId(mid);
    const char* str = GetStringData(str_id);
    int compare = CompareModifiedUtf8ToUtf16AsCodePointValues(str, string);
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
  int32_t lo = 0;
  int32_t hi = NumTypeIds() - 1;
  while (hi >= lo) {
    int32_t mid = (hi + lo) / 2;
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
                                             const uint16_t* signature_type_idxs,
                                             uint32_t signature_length) const {
  int32_t lo = 0;
  int32_t hi = NumProtoIds() - 1;
  while (hi >= lo) {
    int32_t mid = (hi + lo) / 2;
    const DexFile::ProtoId& proto = GetProtoId(mid);
    int compare = return_type_idx - proto.return_type_idx_;
    if (compare == 0) {
      DexFileParameterIterator it(*this, proto);
      size_t i = 0;
      while (it.HasNext() && i < signature_length && compare == 0) {
        compare = signature_type_idxs[i] - it.GetTypeIdx();
        it.Next();
        i++;
      }
      if (compare == 0) {
        if (it.HasNext()) {
          compare = -1;
        } else if (i < signature_length) {
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
bool DexFile::CreateTypeList(const StringPiece& signature, uint16_t* return_type_idx,
                             std::vector<uint16_t>* param_type_idxs) const {
  if (signature[0] != '(') {
    return false;
  }
  size_t offset = 1;
  size_t end = signature.size();
  bool process_return = false;
  while (offset < end) {
    size_t start_offset = offset;
    char c = signature[offset];
    offset++;
    if (c == ')') {
      process_return = true;
      continue;
    }
    while (c == '[') {  // process array prefix
      if (offset >= end) {  // expect some descriptor following [
        return false;
      }
      c = signature[offset];
      offset++;
    }
    if (c == 'L') {  // process type descriptors
      do {
        if (offset >= end) {  // unexpected early termination of descriptor
          return false;
        }
        c = signature[offset];
        offset++;
      } while (c != ';');
    }
    // TODO: avoid creating a std::string just to get a 0-terminated char array
    std::string descriptor(signature.data() + start_offset, offset - start_offset);
    const DexFile::StringId* string_id = FindStringId(descriptor.c_str());
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

const Signature DexFile::CreateSignature(const StringPiece& signature) const {
  uint16_t return_type_idx;
  std::vector<uint16_t> param_type_indices;
  bool success = CreateTypeList(signature, &return_type_idx, &param_type_indices);
  if (!success) {
    return Signature::NoSignature();
  }
  const ProtoId* proto_id = FindProtoId(return_type_idx, param_type_indices);
  if (proto_id == NULL) {
    return Signature::NoSignature();
  }
  return Signature(this, *proto_id);
}

int32_t DexFile::GetLineNumFromPC(mirror::ArtMethod* method, uint32_t rel_pc) const {
  // For native method, lineno should be -2 to indicate it is native. Note that
  // "line number == -2" is how libcore tells from StackTraceElement.
  if (method->GetCodeItemOffset() == 0) {
    return -2;
  }

  const CodeItem* code_item = GetCodeItem(method->GetCodeItemOffset());
  DCHECK(code_item != NULL) << PrettyMethod(method) << " " << GetLocation();

  // A method with no line number info should return -1
  LineNumFromPcContext context(rel_pc, -1);
  DecodeDebugInfo(code_item, method->IsStatic(), method->GetDexMethodIndex(), LineNumForPcCb,
                  NULL, &context);
  return context.line_num_;
}

int32_t DexFile::FindTryItem(const CodeItem &code_item, uint32_t address) {
  // Note: Signed type is important for max and min.
  int32_t min = 0;
  int32_t max = code_item.tries_size_ - 1;

  while (min <= max) {
    int32_t mid = min + ((max - min) / 2);

    const art::DexFile::TryItem* ti = GetTryItems(code_item, mid);
    uint32_t start = ti->start_addr_;
    uint32_t end = start + ti->insn_count_;

    if (address < start) {
      max = mid - 1;
    } else if (address >= end) {
      min = mid + 1;
    } else {  // We have a winner!
      return mid;
    }
  }
  // No match.
  return -1;
}

int32_t DexFile::FindCatchHandlerOffset(const CodeItem &code_item, uint32_t address) {
  int32_t try_item = FindTryItem(code_item, address);
  if (try_item == -1) {
    return -1;
  } else {
    return DexFile::GetTryItems(code_item, try_item)->handler_off_;
  }
}

void DexFile::DecodeDebugInfo0(const CodeItem* code_item, bool is_static, uint32_t method_idx,
                               DexDebugNewPositionCb position_cb, DexDebugNewLocalCb local_cb,
                               void* context, const byte* stream, LocalInfo* local_in_reg) const {
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
    LOG(ERROR) << "invalid stream - problem with parameter iterator in " << GetLocation()
               << " for method " << PrettyMethod(method_idx, *this);
    return;
  }

  for (;;)  {
    uint8_t opcode = *stream++;
    uint16_t reg;
    uint32_t name_idx;
    uint32_t descriptor_idx;
    uint32_t signature_idx = 0;

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
          InvokeLocalCbIfLive(context, reg, address, local_in_reg, local_cb);

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
          InvokeLocalCbIfLive(context, reg, address, local_in_reg, local_cb);
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

        if (position_cb != NULL) {
          if (position_cb(context, address, line)) {
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
                              DexDebugNewPositionCb position_cb, DexDebugNewLocalCb local_cb,
                              void* context) const {
  DCHECK(code_item != nullptr);
  const byte* stream = GetDebugInfoStream(code_item);
  UniquePtr<LocalInfo[]> local_in_reg(local_cb != NULL ?
                                      new LocalInfo[code_item->registers_size_] :
                                      NULL);
  if (stream != NULL) {
    DecodeDebugInfo0(code_item, is_static, method_idx, position_cb, local_cb, context, stream, &local_in_reg[0]);
  }
  for (int reg = 0; reg < code_item->registers_size_; reg++) {
    InvokeLocalCbIfLive(context, reg, code_item->insns_size_in_code_units_, &local_in_reg[0], local_cb);
  }
}

bool DexFile::LineNumForPcCb(void* raw_context, uint32_t address, uint32_t line_num) {
  LineNumFromPcContext* context = reinterpret_cast<LineNumFromPcContext*>(raw_context);

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

std::ostream& operator<<(std::ostream& os, const DexFile& dex_file) {
  os << StringPrintf("[DexFile: %s dex-checksum=%08x location-checksum=%08x %p-%p]",
                     dex_file.GetLocation().c_str(),
                     dex_file.GetHeader().checksum_, dex_file.GetLocationChecksum(),
                     dex_file.Begin(), dex_file.Begin() + dex_file.Size());
  return os;
}
std::string Signature::ToString() const {
  if (dex_file_ == nullptr) {
    CHECK(proto_id_ == nullptr);
    return "<no signature>";
  }
  const DexFile::TypeList* params = dex_file_->GetProtoParameters(*proto_id_);
  std::string result;
  if (params == nullptr) {
    result += "()";
  } else {
    result += "(";
    for (uint32_t i = 0; i < params->Size(); ++i) {
      result += dex_file_->StringByTypeIdx(params->GetTypeItem(i).type_idx_);
    }
    result += ")";
  }
  result += dex_file_->StringByTypeIdx(proto_id_->return_type_idx_);
  return result;
}

bool Signature::operator==(const StringPiece& rhs) const {
  if (dex_file_ == nullptr) {
    return false;
  }
  StringPiece tail(rhs);
  if (!tail.starts_with("(")) {
    return false;  // Invalid signature
  }
  tail.remove_prefix(1);  // "(";
  const DexFile::TypeList* params = dex_file_->GetProtoParameters(*proto_id_);
  if (params != nullptr) {
    for (uint32_t i = 0; i < params->Size(); ++i) {
      StringPiece param(dex_file_->StringByTypeIdx(params->GetTypeItem(i).type_idx_));
      if (!tail.starts_with(param)) {
        return false;
      }
      tail.remove_prefix(param.length());
    }
  }
  if (!tail.starts_with(")")) {
    return false;
  }
  tail.remove_prefix(1);  // ")";
  return tail == dex_file_->StringByTypeIdx(proto_id_->return_type_idx_);
}

std::ostream& operator<<(std::ostream& os, const Signature& sig) {
  return os << sig.ToString();
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
  if (last_idx_ != 0 && field_.field_idx_delta_ == 0) {
    LOG(WARNING) << "Duplicate field " << PrettyField(GetMemberIndex(), dex_file_)
                 << " in " << dex_file_.GetLocation();
  }
}

void ClassDataItemIterator::ReadClassDataMethod() {
  method_.method_idx_delta_ = DecodeUnsignedLeb128(&ptr_pos_);
  method_.access_flags_ = DecodeUnsignedLeb128(&ptr_pos_);
  method_.code_off_ = DecodeUnsignedLeb128(&ptr_pos_);
  if (last_idx_ != 0 && method_.method_idx_delta_ == 0) {
    LOG(WARNING) << "Duplicate method " << PrettyMethod(GetMemberIndex(), dex_file_)
                 << " in " << dex_file_.GetLocation();
  }
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
                                                                 Handle<mirror::DexCache>* dex_cache,
                                                                 Handle<mirror::ClassLoader>* class_loader,
                                                                 ClassLinker* linker,
                                                                 const DexFile::ClassDef& class_def)
    : dex_file_(dex_file), dex_cache_(dex_cache), class_loader_(class_loader), linker_(linker),
      array_size_(), pos_(-1), type_(kByte) {
  DCHECK(dex_cache != nullptr);
  DCHECK(class_loader != nullptr);
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
  type_ = static_cast<ValueType>(value_type & kEncodedValueTypeMask);
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
    jval_.i = ReadUnsignedInt(ptr_, value_arg, false);
    break;
  case kField:
  case kMethod:
  case kEnum:
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

template<bool kTransactionActive>
void EncodedStaticFieldValueIterator::ReadValueToField(mirror::ArtField* field) const {
  switch (type_) {
    case kBoolean: field->SetBoolean<kTransactionActive>(field->GetDeclaringClass(), jval_.z); break;
    case kByte:    field->SetByte<kTransactionActive>(field->GetDeclaringClass(), jval_.b); break;
    case kShort:   field->SetShort<kTransactionActive>(field->GetDeclaringClass(), jval_.s); break;
    case kChar:    field->SetChar<kTransactionActive>(field->GetDeclaringClass(), jval_.c); break;
    case kInt:     field->SetInt<kTransactionActive>(field->GetDeclaringClass(), jval_.i); break;
    case kLong:    field->SetLong<kTransactionActive>(field->GetDeclaringClass(), jval_.j); break;
    case kFloat:   field->SetFloat<kTransactionActive>(field->GetDeclaringClass(), jval_.f); break;
    case kDouble:  field->SetDouble<kTransactionActive>(field->GetDeclaringClass(), jval_.d); break;
    case kNull:    field->SetObject<kTransactionActive>(field->GetDeclaringClass(), NULL); break;
    case kString: {
      CHECK(!kMovingFields);
      mirror::String* resolved = linker_->ResolveString(dex_file_, jval_.i, *dex_cache_);
      field->SetObject<kTransactionActive>(field->GetDeclaringClass(), resolved);
      break;
    }
    case kType: {
      CHECK(!kMovingFields);
      mirror::Class* resolved = linker_->ResolveType(dex_file_, jval_.i, *dex_cache_,
                                                     *class_loader_);
      field->SetObject<kTransactionActive>(field->GetDeclaringClass(), resolved);
      break;
    }
    default: UNIMPLEMENTED(FATAL) << ": type " << type_;
  }
}
template void EncodedStaticFieldValueIterator::ReadValueToField<true>(mirror::ArtField* field) const;
template void EncodedStaticFieldValueIterator::ReadValueToField<false>(mirror::ArtField* field) const;

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
      offset = DexFile::FindCatchHandlerOffset(code_item, address);
  }
  Init(code_item, offset);
}

CatchHandlerIterator::CatchHandlerIterator(const DexFile::CodeItem& code_item,
                                           const DexFile::TryItem& try_item) {
  handler_.address_ = -1;
  Init(code_item, try_item.handler_off_);
}

void CatchHandlerIterator::Init(const DexFile::CodeItem& code_item,
                                int32_t offset) {
  if (offset >= 0) {
    Init(DexFile::GetCatchHandlerData(code_item, offset));
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
