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
#include <sstream>
#include <string.h>
#include <unistd.h>

#include "base/bit_vector.h"
#include "base/stl_util.h"
#include "base/unix_file/fd_file.h"
#include "elf_file.h"
#include "elf_utils.h"
#include "oat.h"
#include "mirror/art_method.h"
#include "mirror/art_method-inl.h"
#include "mirror/class.h"
#include "mirror/object-inl.h"
#include "os.h"
#include "runtime.h"
#include "utils.h"
#include "vmap_table.h"

namespace art {

void OatFile::CheckLocation(const std::string& location) {
  CHECK(!location.empty());
}

OatFile* OatFile::OpenWithElfFile(ElfFile* elf_file,
                                  const std::string& location,
                                  std::string* error_msg) {
  std::unique_ptr<OatFile> oat_file(new OatFile(location, false));
  oat_file->elf_file_.reset(elf_file);
  Elf32_Shdr* hdr = elf_file->FindSectionByName(".rodata");
  oat_file->begin_ = elf_file->Begin() + hdr->sh_offset;
  oat_file->end_ = elf_file->Begin() + hdr->sh_size + hdr->sh_offset;
  return oat_file->Setup(error_msg) ? oat_file.release() : nullptr;
}

OatFile* OatFile::OpenMemory(std::vector<uint8_t>& oat_contents,
                             const std::string& location,
                             std::string* error_msg) {
  CHECK(!oat_contents.empty()) << location;
  CheckLocation(location);
  std::unique_ptr<OatFile> oat_file(new OatFile(location, false));
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
  std::unique_ptr<OatFile> ret;
  if (kUsePortableCompiler && executable) {
    // If we are using PORTABLE, use dlopen to deal with relocations.
    //
    // We use our own ELF loader for Quick to deal with legacy apps that
    // open a generated dex file by name, remove the file, then open
    // another generated dex file with the same name. http://b/10614658
    ret.reset(OpenDlopen(filename, location, requested_base, error_msg));
  } else {
    // If we aren't trying to execute, we just use our own ElfFile loader for a couple reasons:
    //
    // On target, dlopen may fail when compiling due to selinux restrictions on installd.
    //
    // On host, dlopen is expected to fail when cross compiling, so fall back to OpenElfFile.
    // This won't work for portable runtime execution because it doesn't process relocations.
    std::unique_ptr<File> file(OS::OpenFileForReading(filename.c_str()));
    if (file.get() == NULL) {
      *error_msg = StringPrintf("Failed to open oat filename for reading: %s", strerror(errno));
      return nullptr;
    }
    ret.reset(OpenElfFile(file.get(), location, requested_base, false, executable, error_msg));

    // It would be nice to unlink here. But we might have opened the file created by the
    // ScopedLock, which we better not delete to avoid races. TODO: Investigate how to fix the API
    // to allow removal when we know the ELF must be borked.
  }
  return ret.release();
}

OatFile* OatFile::OpenWritable(File* file, const std::string& location, std::string* error_msg) {
  CheckLocation(location);
  return OpenElfFile(file, location, NULL, true, false, error_msg);
}

OatFile* OatFile::OpenReadable(File* file, const std::string& location, std::string* error_msg) {
  CheckLocation(location);
  return OpenElfFile(file, location, NULL, false, false, error_msg);
}

OatFile* OatFile::OpenDlopen(const std::string& elf_filename,
                             const std::string& location,
                             byte* requested_base,
                             std::string* error_msg) {
  std::unique_ptr<OatFile> oat_file(new OatFile(location, true));
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
  std::unique_ptr<OatFile> oat_file(new OatFile(location, executable));
  bool success = oat_file->ElfFileOpen(file, requested_base, writable, executable, error_msg);
  if (!success) {
    CHECK(!error_msg->empty());
    return nullptr;
  }
  return oat_file.release();
}

OatFile::OatFile(const std::string& location, bool is_executable)
    : location_(location), begin_(NULL), end_(NULL), is_executable_(is_executable),
      dlopen_handle_(NULL),
      secondary_lookup_lock_("OatFile secondary lookup lock", kOatFileSecondaryLookupLock) {
  CHECK(!location_.empty());
}

OatFile::~OatFile() {
  STLDeleteElements(&oat_dex_files_storage_);
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

  oat += GetOatHeader().GetKeyValueStoreSize();
  if (oat > End()) {
    *error_msg = StringPrintf("In oat file '%s' found truncated variable-size data: "
                              "%p + %zd + %ud <= %p", GetLocation().c_str(),
                              Begin(), sizeof(OatHeader), GetOatHeader().GetKeyValueStoreSize(),
                              End());
    return false;
  }

  uint32_t dex_file_count = GetOatHeader().GetDexFileCount();
  oat_dex_files_storage_.reserve(dex_file_count);
  for (size_t i = 0; i < dex_file_count; i++) {
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

    std::string canonical_location = DexFile::GetDexCanonicalLocation(dex_file_location.c_str());

    // Create the OatDexFile and add it to the owning container.
    OatDexFile* oat_dex_file = new OatDexFile(this,
                                              dex_file_location,
                                              canonical_location,
                                              dex_file_checksum,
                                              dex_file_pointer,
                                              methods_offsets_pointer);
    oat_dex_files_storage_.push_back(oat_dex_file);

    // Add the location and canonical location (if different) to the oat_dex_files_ table.
    StringPiece key(oat_dex_file->GetDexFileLocation());
    oat_dex_files_.Put(key, oat_dex_file);
    if (canonical_location != dex_file_location) {
      StringPiece canonical_key(oat_dex_file->GetCanonicalDexFileLocation());
      oat_dex_files_.Put(canonical_key, oat_dex_file);
    }
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
  // NOTE: We assume here that the canonical location for a given dex_location never
  // changes. If it does (i.e. some symlink used by the filename changes) we may return
  // an incorrect OatDexFile. As long as we have a checksum to check, we shall return
  // an identical file or fail; otherwise we may see some unpredictable failures.

  // TODO: Additional analysis of usage patterns to see if this can be simplified
  // without any performance loss, for example by not doing the first lock-free lookup.

  const OatFile::OatDexFile* oat_dex_file = nullptr;
  StringPiece key(dex_location);
  // Try to find the key cheaply in the oat_dex_files_ map which holds dex locations
  // directly mentioned in the oat file and doesn't require locking.
  auto primary_it = oat_dex_files_.find(key);
  if (primary_it != oat_dex_files_.end()) {
    oat_dex_file = primary_it->second;
    DCHECK(oat_dex_file != nullptr);
  } else {
    // This dex_location is not one of the dex locations directly mentioned in the
    // oat file. The correct lookup is via the canonical location but first see in
    // the secondary_oat_dex_files_ whether we've looked up this location before.
    MutexLock mu(Thread::Current(), secondary_lookup_lock_);
    auto secondary_lb = secondary_oat_dex_files_.lower_bound(key);
    if (secondary_lb != secondary_oat_dex_files_.end() && key == secondary_lb->first) {
      oat_dex_file = secondary_lb->second;  // May be nullptr.
    } else {
      // We haven't seen this dex_location before, we must check the canonical location.
      std::string dex_canonical_location = DexFile::GetDexCanonicalLocation(dex_location);
      if (dex_canonical_location != dex_location) {
        StringPiece canonical_key(dex_canonical_location);
        auto canonical_it = oat_dex_files_.find(canonical_key);
        if (canonical_it != oat_dex_files_.end()) {
          oat_dex_file = canonical_it->second;
        }  // else keep nullptr.
      }  // else keep nullptr.

      // Copy the key to the string_cache_ and store the result in secondary map.
      string_cache_.emplace_back(key.data(), key.length());
      StringPiece key_copy(string_cache_.back());
      secondary_oat_dex_files_.PutBefore(secondary_lb, key_copy, oat_dex_file);
    }
  }
  if (oat_dex_file != nullptr &&
      (dex_location_checksum == nullptr ||
       oat_dex_file->GetDexFileLocationChecksum() == *dex_location_checksum)) {
    return oat_dex_file;
  }

  if (warn_if_not_found) {
    std::string dex_canonical_location = DexFile::GetDexCanonicalLocation(dex_location);
    std::string checksum("<unspecified>");
    if (dex_location_checksum != NULL) {
      checksum = StringPrintf("0x%08x", *dex_location_checksum);
    }
    LOG(WARNING) << "Failed to find OatDexFile for DexFile " << dex_location
                 << " ( canonical path " << dex_canonical_location << ")"
                 << " with checksum " << checksum << " in OatFile " << GetLocation();
    if (kIsDebugBuild) {
      for (const OatDexFile* odf : oat_dex_files_storage_) {
        LOG(WARNING) << "OatFile " << GetLocation()
                     << " contains OatDexFile " << odf->GetDexFileLocation()
                     << " (canonical path " << odf->GetCanonicalDexFileLocation() << ")"
                     << " with checksum 0x" << std::hex << odf->GetDexFileLocationChecksum();
      }
    }
  }

  return NULL;
}

OatFile::OatDexFile::OatDexFile(const OatFile* oat_file,
                                const std::string& dex_file_location,
                                const std::string& canonical_dex_file_location,
                                uint32_t dex_file_location_checksum,
                                const byte* dex_file_pointer,
                                const uint32_t* oat_class_offsets_pointer)
    : oat_file_(oat_file),
      dex_file_location_(dex_file_location),
      canonical_dex_file_location_(canonical_dex_file_location),
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

uint32_t OatFile::OatDexFile::GetOatClassOffset(uint16_t class_def_index) const {
  return oat_class_offsets_pointer_[class_def_index];
}

OatFile::OatClass OatFile::OatDexFile::GetOatClass(uint16_t class_def_index) const {
  uint32_t oat_class_offset = GetOatClassOffset(class_def_index);

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

uint32_t OatFile::OatClass::GetOatMethodOffsetsOffset(uint32_t method_index) const {
  const OatMethodOffsets* oat_method_offsets = GetOatMethodOffsets(method_index);
  if (oat_method_offsets == nullptr) {
    return 0u;
  }
  return reinterpret_cast<const uint8_t*>(oat_method_offsets) - oat_file_->Begin();
}

const OatMethodOffsets* OatFile::OatClass::GetOatMethodOffsets(uint32_t method_index) const {
  // NOTE: We don't keep the number of methods and cannot do a bounds check for method_index.
  if (methods_pointer_ == nullptr) {
    CHECK_EQ(kOatClassNoneCompiled, type_);
    return nullptr;
  }
  size_t methods_pointer_index;
  if (bitmap_ == nullptr) {
    CHECK_EQ(kOatClassAllCompiled, type_);
    methods_pointer_index = method_index;
  } else {
    CHECK_EQ(kOatClassSomeCompiled, type_);
    if (!BitVector::IsBitSet(bitmap_, method_index)) {
      return nullptr;
    }
    size_t num_set_bits = BitVector::NumSetBits(bitmap_, method_index);
    methods_pointer_index = num_set_bits;
  }
  const OatMethodOffsets& oat_method_offsets = methods_pointer_[methods_pointer_index];
  return &oat_method_offsets;
}

const OatFile::OatMethod OatFile::OatClass::GetOatMethod(uint32_t method_index) const {
  const OatMethodOffsets* oat_method_offsets = GetOatMethodOffsets(method_index);
  if (oat_method_offsets == nullptr) {
    return OatMethod(nullptr, 0, 0);
  }
  if (oat_file_->IsExecutable() ||
      Runtime::Current() == nullptr ||        // This case applies for oatdump.
      Runtime::Current()->IsCompiler()) {
    return OatMethod(
        oat_file_->Begin(),
        oat_method_offsets->code_offset_,
        oat_method_offsets->gc_map_offset_);
  } else {
    // We aren't allowed to use the compiled code. We just force it down the interpreted version.
    return OatMethod(oat_file_->Begin(), 0, 0);
  }
}

OatFile::OatMethod::OatMethod(const byte* base,
                              const uint32_t code_offset,
                              const uint32_t gc_map_offset)
  : begin_(base),
    code_offset_(code_offset),
    native_gc_map_offset_(gc_map_offset) {
}

OatFile::OatMethod::~OatMethod() {}

void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());  // Used by native methods in work around JNI mode.
}

}  // namespace art
