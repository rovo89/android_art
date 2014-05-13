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

#include "oat_file.h"

#include <dlfcn.h>

#include "base/bit_vector.h"
#include "base/stl_util.h"
#include "base/unix_file/fd_file.h"
#include "elf_file.h"
#include "oat.h"
#include "mirror/art_method.h"
#include "mirror/art_method-inl.h"
#include "mirror/class.h"
#include "mirror/object-inl.h"
#include "os.h"
#include "utils.h"
#include "vmap_table.h"

namespace art {

std::string OatFile::DexFilenameToOdexFilename(const std::string& location) {
  CHECK_GE(location.size(), 4U) << location;  // must be at least .123
  size_t dot_index = location.size() - 3 - 1;  // 3=dex or zip or apk
  CHECK_EQ('.', location[dot_index]) << location;
  std::string odex_location(location);
  odex_location.resize(dot_index + 1);
  CHECK_EQ('.', odex_location[odex_location.size()-1]) << location << " " << odex_location;
  odex_location += "odex";
  return odex_location;
}

void OatFile::CheckLocation(const std::string& location) {
  CHECK(!location.empty());
}

OatFile* OatFile::OpenMemory(std::vector<uint8_t>& oat_contents,
                             const std::string& location,
                             std::string* error_msg) {
  CHECK(!oat_contents.empty()) << location;
  CheckLocation(location);
  UniquePtr<OatFile> oat_file(new OatFile(location));
  oat_file->begin_ = &oat_contents[0];
  oat_file->end_ = &oat_contents[oat_contents.size()];
  return oat_file->Setup(error_msg) ? oat_file.release() : nullptr;
}

OatFile* OatFile::Open(const std::string& filename,
                       const std::string& location,
                       byte* requested_base,
                       bool executable,
                       std::string* error_msg) {
  CHECK(!filename.empty()) << location;
  CheckLocation(filename);
  if (kUsePortableCompiler) {
    // If we are using PORTABLE, use dlopen to deal with relocations.
    //
    // We use our own ELF loader for Quick to deal with legacy apps that
    // open a generated dex file by name, remove the file, then open
    // another generated dex file with the same name. http://b/10614658
    if (executable) {
      return OpenDlopen(filename, location, requested_base, error_msg);
    }
  }
  // If we aren't trying to execute, we just use our own ElfFile loader for a couple reasons:
  //
  // On target, dlopen may fail when compiling due to selinux restrictions on installd.
  //
  // On host, dlopen is expected to fail when cross compiling, so fall back to OpenElfFile.
  // This won't work for portable runtime execution because it doesn't process relocations.
  UniquePtr<File> file(OS::OpenFileForReading(filename.c_str()));
  if (file.get() == NULL) {
    *error_msg = StringPrintf("Failed to open oat filename for reading: %s", strerror(errno));
    return NULL;
  }
  return OpenElfFile(file.get(), location, requested_base, false, executable, error_msg);
}

OatFile* OatFile::OpenWritable(File* file, const std::string& location, std::string* error_msg) {
  CheckLocation(location);
  return OpenElfFile(file, location, NULL, true, false, error_msg);
}

OatFile* OatFile::OpenDlopen(const std::string& elf_filename,
                             const std::string& location,
                             byte* requested_base,
                             std::string* error_msg) {
  UniquePtr<OatFile> oat_file(new OatFile(location));
  bool success = oat_file->Dlopen(elf_filename, requested_base, error_msg);
  if (!success) {
    return nullptr;
  }
  return oat_file.release();
}

OatFile* OatFile::OpenElfFile(File* file,
                              const std::string& location,
                              byte* requested_base,
                              bool writable,
                              bool executable,
                              std::string* error_msg) {
  UniquePtr<OatFile> oat_file(new OatFile(location));
  bool success = oat_file->ElfFileOpen(file, requested_base, writable, executable, error_msg);
  if (!success) {
    CHECK(!error_msg->empty());
    return nullptr;
  }
  return oat_file.release();
}

OatFile::OatFile(const std::string& location)
    : location_(location), begin_(NULL), end_(NULL), dlopen_handle_(NULL) {
  CHECK(!location_.empty());
}

OatFile::~OatFile() {
  STLDeleteValues(&oat_dex_files_);
  if (dlopen_handle_ != NULL) {
    dlclose(dlopen_handle_);
  }
}

bool OatFile::Dlopen(const std::string& elf_filename, byte* requested_base,
                     std::string* error_msg) {
  char* absolute_path = realpath(elf_filename.c_str(), NULL);
  if (absolute_path == NULL) {
    *error_msg = StringPrintf("Failed to find absolute path for '%s'", elf_filename.c_str());
    return false;
  }
  dlopen_handle_ = dlopen(absolute_path, RTLD_NOW);
  free(absolute_path);
  if (dlopen_handle_ == NULL) {
    *error_msg = StringPrintf("Failed to dlopen '%s': %s", elf_filename.c_str(), dlerror());
    return false;
  }
  begin_ = reinterpret_cast<byte*>(dlsym(dlopen_handle_, "oatdata"));
  if (begin_ == NULL) {
    *error_msg = StringPrintf("Failed to find oatdata symbol in '%s': %s", elf_filename.c_str(),
                              dlerror());
    return false;
  }
  if (requested_base != NULL && begin_ != requested_base) {
    *error_msg = StringPrintf("Failed to find oatdata symbol at expected address: "
                              "oatdata=%p != expected=%p /proc/self/maps:\n",
                              begin_, requested_base);
    ReadFileToString("/proc/self/maps", error_msg);
    return false;
  }
  end_ = reinterpret_cast<byte*>(dlsym(dlopen_handle_, "oatlastword"));
  if (end_ == NULL) {
    *error_msg = StringPrintf("Failed to find oatlastword symbol in '%s': %s", elf_filename.c_str(),
                              dlerror());
    return false;
  }
  // Readjust to be non-inclusive upper bound.
  end_ += sizeof(uint32_t);
  return Setup(error_msg);
}

bool OatFile::ElfFileOpen(File* file, byte* requested_base, bool writable, bool executable,
                          std::string* error_msg) {
  elf_file_.reset(ElfFile::Open(file, writable, true, error_msg));
  if (elf_file_.get() == nullptr) {
    DCHECK(!error_msg->empty());
    return false;
  }
  bool loaded = elf_file_->Load(executable, error_msg);
  if (!loaded) {
    DCHECK(!error_msg->empty());
    return false;
  }
  begin_ = elf_file_->FindDynamicSymbolAddress("oatdata");
  if (begin_ == NULL) {
    *error_msg = StringPrintf("Failed to find oatdata symbol in '%s'", file->GetPath().c_str());
    return false;
  }
  if (requested_base != NULL && begin_ != requested_base) {
    *error_msg = StringPrintf("Failed to find oatdata symbol at expected address: "
                              "oatdata=%p != expected=%p /proc/self/maps:\n",
                              begin_, requested_base);
    ReadFileToString("/proc/self/maps", error_msg);
    return false;
  }
  end_ = elf_file_->FindDynamicSymbolAddress("oatlastword");
  if (end_ == NULL) {
    *error_msg = StringPrintf("Failed to find oatlastword symbol in '%s'", file->GetPath().c_str());
    return false;
  }
  // Readjust to be non-inclusive upper bound.
  end_ += sizeof(uint32_t);
  return Setup(error_msg);
}

bool OatFile::Setup(std::string* error_msg) {
  if (!GetOatHeader().IsValid()) {
    *error_msg = StringPrintf("Invalid oat magic for '%s'", GetLocation().c_str());
    return false;
  }
  const byte* oat = Begin();
  oat += sizeof(OatHeader);
  if (oat > End()) {
    *error_msg = StringPrintf("In oat file '%s' found truncated OatHeader", GetLocation().c_str());
    return false;
  }

  oat += GetOatHeader().GetImageFileLocationSize();
  if (oat > End()) {
    *error_msg = StringPrintf("In oat file '%s' found truncated image file location: "
                              "%p + %zd + %ud <= %p", GetLocation().c_str(),
                              Begin(), sizeof(OatHeader), GetOatHeader().GetImageFileLocationSize(),
                              End());
    return false;
  }

  for (size_t i = 0; i < GetOatHeader().GetDexFileCount(); i++) {
    uint32_t dex_file_location_size = *reinterpret_cast<const uint32_t*>(oat);
    if (UNLIKELY(dex_file_location_size == 0U)) {
      *error_msg = StringPrintf("In oat file '%s' found OatDexFile #%zd with empty location name",
                                GetLocation().c_str(), i);
      return false;
    }
    oat += sizeof(dex_file_location_size);
    if (UNLIKELY(oat > End())) {
      *error_msg = StringPrintf("In oat file '%s' found OatDexFile #%zd truncated after dex file "
                                "location size", GetLocation().c_str(), i);
      return false;
    }

    const char* dex_file_location_data = reinterpret_cast<const char*>(oat);
    oat += dex_file_location_size;
    if (UNLIKELY(oat > End())) {
      *error_msg = StringPrintf("In oat file '%s' found OatDexFile #%zd with truncated dex file "
                                "location", GetLocation().c_str(), i);
      return false;
    }

    std::string dex_file_location(dex_file_location_data, dex_file_location_size);

    uint32_t dex_file_checksum = *reinterpret_cast<const uint32_t*>(oat);
    oat += sizeof(dex_file_checksum);
    if (UNLIKELY(oat > End())) {
      *error_msg = StringPrintf("In oat file '%s' found OatDexFile #%zd for '%s' truncated after "
                                "dex file checksum", GetLocation().c_str(), i,
                                dex_file_location.c_str());
      return false;
    }

    uint32_t dex_file_offset = *reinterpret_cast<const uint32_t*>(oat);
    if (UNLIKELY(dex_file_offset == 0U)) {
      *error_msg = StringPrintf("In oat file '%s' found OatDexFile #%zd for '%s' with zero dex "
                                "file offset", GetLocation().c_str(), i, dex_file_location.c_str());
      return false;
    }
    if (UNLIKELY(dex_file_offset > Size())) {
      *error_msg = StringPrintf("In oat file '%s' found OatDexFile #%zd for '%s' with dex file "
                                "offset %ud > %zd", GetLocation().c_str(), i,
                                dex_file_location.c_str(), dex_file_offset, Size());
      return false;
    }
    oat += sizeof(dex_file_offset);
    if (UNLIKELY(oat > End())) {
      *error_msg = StringPrintf("In oat file '%s' found OatDexFile #%zd for '%s' truncated "
                                " after dex file offsets", GetLocation().c_str(), i,
                                dex_file_location.c_str());
      return false;
    }

    const uint8_t* dex_file_pointer = Begin() + dex_file_offset;
    if (UNLIKELY(!DexFile::IsMagicValid(dex_file_pointer))) {
      *error_msg = StringPrintf("In oat file '%s' found OatDexFile #%zd for '%s' with invalid "
                                " dex file magic '%s'", GetLocation().c_str(), i,
                                dex_file_location.c_str(), dex_file_pointer);
      return false;
    }
    if (UNLIKELY(!DexFile::IsVersionValid(dex_file_pointer))) {
      *error_msg = StringPrintf("In oat file '%s' found OatDexFile #%zd for '%s' with invalid "
                                " dex file version '%s'", GetLocation().c_str(), i,
                                dex_file_location.c_str(), dex_file_pointer);
      return false;
    }
    const DexFile::Header* header = reinterpret_cast<const DexFile::Header*>(dex_file_pointer);
    const uint32_t* methods_offsets_pointer = reinterpret_cast<const uint32_t*>(oat);

    oat += (sizeof(*methods_offsets_pointer) * header->class_defs_size_);
    if (UNLIKELY(oat > End())) {
      *error_msg = StringPrintf("In oat file '%s' found OatDexFile #%zd for '%s' with truncated "
                                " method offsets", GetLocation().c_str(), i,
                                dex_file_location.c_str());
      return false;
    }

    oat_dex_files_.Put(dex_file_location, new OatDexFile(this,
                                                         dex_file_location,
                                                         dex_file_checksum,
                                                         dex_file_pointer,
                                                         methods_offsets_pointer));
  }
  return true;
}

const OatHeader& OatFile::GetOatHeader() const {
  return *reinterpret_cast<const OatHeader*>(Begin());
}

const byte* OatFile::Begin() const {
  CHECK(begin_ != NULL);
  return begin_;
}

const byte* OatFile::End() const {
  CHECK(end_ != NULL);
  return end_;
}

const OatFile::OatDexFile* OatFile::GetOatDexFile(const char* dex_location,
                                                  const uint32_t* dex_location_checksum,
                                                  bool warn_if_not_found) const {
  Table::const_iterator it = oat_dex_files_.find(dex_location);
  if (it != oat_dex_files_.end()) {
    const OatFile::OatDexFile* oat_dex_file = it->second;
    if (dex_location_checksum == NULL ||
        oat_dex_file->GetDexFileLocationChecksum() == *dex_location_checksum) {
      return oat_dex_file;
    }
  }

  if (warn_if_not_found) {
    std::string checksum("<unspecified>");
    if (dex_location_checksum != NULL) {
      checksum = StringPrintf("0x%08x", *dex_location_checksum);
    }
    LOG(WARNING) << "Failed to find OatDexFile for DexFile " << dex_location
                 << " with checksum " << checksum << " in OatFile " << GetLocation();
    if (kIsDebugBuild) {
      for (Table::const_iterator it = oat_dex_files_.begin(); it != oat_dex_files_.end(); ++it) {
        LOG(WARNING) << "OatFile " << GetLocation()
                     << " contains OatDexFile " << it->second->GetDexFileLocation()
                     << " with checksum 0x" << std::hex << it->second->GetDexFileLocationChecksum();
      }
    }
  }
  return NULL;
}

std::vector<const OatFile::OatDexFile*> OatFile::GetOatDexFiles() const {
  std::vector<const OatFile::OatDexFile*> result;
  for (Table::const_iterator it = oat_dex_files_.begin(); it != oat_dex_files_.end(); ++it) {
    result.push_back(it->second);
  }
  return result;
}

OatFile::OatDexFile::OatDexFile(const OatFile* oat_file,
                                const std::string& dex_file_location,
                                uint32_t dex_file_location_checksum,
                                const byte* dex_file_pointer,
                                const uint32_t* oat_class_offsets_pointer)
    : oat_file_(oat_file),
      dex_file_location_(dex_file_location),
      dex_file_location_checksum_(dex_file_location_checksum),
      dex_file_pointer_(dex_file_pointer),
      oat_class_offsets_pointer_(oat_class_offsets_pointer) {}

OatFile::OatDexFile::~OatDexFile() {}

size_t OatFile::OatDexFile::FileSize() const {
  return reinterpret_cast<const DexFile::Header*>(dex_file_pointer_)->file_size_;
}

const DexFile* OatFile::OatDexFile::OpenDexFile(std::string* error_msg) const {
  return DexFile::Open(dex_file_pointer_, FileSize(), dex_file_location_,
                       dex_file_location_checksum_, error_msg);
}

OatFile::OatClass OatFile::OatDexFile::GetOatClass(uint16_t class_def_index) const {
  uint32_t oat_class_offset = oat_class_offsets_pointer_[class_def_index];

  const byte* oat_class_pointer = oat_file_->Begin() + oat_class_offset;
  CHECK_LT(oat_class_pointer, oat_file_->End()) << oat_file_->GetLocation();

  const byte* status_pointer = oat_class_pointer;
  CHECK_LT(status_pointer, oat_file_->End()) << oat_file_->GetLocation();
  mirror::Class::Status status =
      static_cast<mirror::Class::Status>(*reinterpret_cast<const int16_t*>(status_pointer));
  CHECK_LT(status, mirror::Class::kStatusMax);

  const byte* type_pointer = status_pointer + sizeof(uint16_t);
  CHECK_LT(type_pointer, oat_file_->End()) << oat_file_->GetLocation();
  OatClassType type = static_cast<OatClassType>(*reinterpret_cast<const uint16_t*>(type_pointer));
  CHECK_LT(type, kOatClassMax);

  const byte* after_type_pointer = type_pointer + sizeof(int16_t);
  CHECK_LE(after_type_pointer, oat_file_->End()) << oat_file_->GetLocation();

  uint32_t bitmap_size = 0;
  const byte* bitmap_pointer = nullptr;
  const byte* methods_pointer = nullptr;
  if (type == kOatClassSomeCompiled) {
    bitmap_size = static_cast<uint32_t>(*reinterpret_cast<const uint32_t*>(after_type_pointer));
    bitmap_pointer = after_type_pointer + sizeof(bitmap_size);
    CHECK_LE(bitmap_pointer, oat_file_->End()) << oat_file_->GetLocation();
    methods_pointer = bitmap_pointer + bitmap_size;
  } else {
    methods_pointer = after_type_pointer;
  }
  CHECK_LE(methods_pointer, oat_file_->End()) << oat_file_->GetLocation();

  return OatClass(oat_file_,
                  status,
                  type,
                  bitmap_size,
                  reinterpret_cast<const uint32_t*>(bitmap_pointer),
                  reinterpret_cast<const OatMethodOffsets*>(methods_pointer));
}

OatFile::OatClass::OatClass(const OatFile* oat_file,
                            mirror::Class::Status status,
                            OatClassType type,
                            uint32_t bitmap_size,
                            const uint32_t* bitmap_pointer,
                            const OatMethodOffsets* methods_pointer)
    : oat_file_(oat_file), status_(status), type_(type),
      bitmap_(bitmap_pointer), methods_pointer_(methods_pointer) {
    CHECK(methods_pointer != nullptr);
    switch (type_) {
      case kOatClassAllCompiled: {
        CHECK_EQ(0U, bitmap_size);
        CHECK(bitmap_pointer == nullptr);
        break;
      }
      case kOatClassSomeCompiled: {
        CHECK_NE(0U, bitmap_size);
        CHECK(bitmap_pointer != nullptr);
        break;
      }
      case kOatClassNoneCompiled: {
        CHECK_EQ(0U, bitmap_size);
        CHECK(bitmap_pointer == nullptr);
        methods_pointer_ = nullptr;
        break;
      }
      case kOatClassMax: {
        LOG(FATAL) << "Invalid OatClassType " << type_;
        break;
      }
    }
}

const OatFile::OatMethod OatFile::OatClass::GetOatMethod(uint32_t method_index) const {
  // NOTE: We don't keep the number of methods and cannot do a bounds check for method_index.
  if (methods_pointer_ == NULL) {
    CHECK_EQ(kOatClassNoneCompiled, type_);
    return OatMethod(NULL, 0, 0);
  }
  size_t methods_pointer_index;
  if (bitmap_ == NULL) {
    CHECK_EQ(kOatClassAllCompiled, type_);
    methods_pointer_index = method_index;
  } else {
    CHECK_EQ(kOatClassSomeCompiled, type_);
    if (!BitVector::IsBitSet(bitmap_, method_index)) {
      return OatMethod(NULL, 0, 0);
    }
    size_t num_set_bits = BitVector::NumSetBits(bitmap_, method_index);
    methods_pointer_index = num_set_bits;
  }
  const OatMethodOffsets& oat_method_offsets = methods_pointer_[methods_pointer_index];
  return OatMethod(
      oat_file_->Begin(),
      oat_method_offsets.code_offset_,
      oat_method_offsets.gc_map_offset_);
}

OatFile::OatMethod::OatMethod(const byte* base,
                              const uint32_t code_offset,
                              const uint32_t gc_map_offset)
  : begin_(base),
    code_offset_(code_offset),
    native_gc_map_offset_(gc_map_offset) {
}

OatFile::OatMethod::~OatMethod() {}


uint32_t OatFile::OatMethod::GetQuickCodeSize() const {
  uintptr_t code = reinterpret_cast<uintptr_t>(GetQuickCode());
  if (code == 0) {
    return 0;
  }
  // TODO: make this Thumb2 specific
  code &= ~0x1;
  return reinterpret_cast<uint32_t*>(code)[-1];
}

void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());  // Used by native methods in work around JNI mode.
}

}  // namespace art
