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

#include "base/unix_file/fd_file.h"
#include "os.h"
#include "stl_util.h"

namespace art {

std::string OatFile::DexFilenameToOatFilename(const std::string& location) {
  CHECK(IsValidDexFilename(location) || IsValidZipFilename(location));
  std::string oat_location(location);
  oat_location += ".oat";
  return oat_location;
}

OatFile* OatFile::Open(const std::string& filename,
                       const std::string& location,
                       byte* requested_base,
                       RelocationBehavior reloc,
                       bool writable) {
  CHECK(!filename.empty()) << location;
  UniquePtr<File> file(OS::OpenFile(filename.c_str(), writable, false));
  if (file.get() == NULL) {
    return NULL;
  }
  return Open(*file.get(), location, requested_base, reloc, writable);
}

OatFile* OatFile::Open(File& file,
                       const std::string& location,
                       byte* requested_base,
                       RelocationBehavior reloc,
                       bool writable) {
  CHECK(!location.empty());
  if (!IsValidOatFilename(location)) {
    LOG(WARNING) << "Attempting to open oat file with unknown extension '" << location << "'";
  }
  UniquePtr<OatFile> oat_file(new OatFile(location));
  bool success = oat_file->Map(file, requested_base, reloc, writable);
  if (!success) {
    return NULL;
  }
  return oat_file.release();
}

OatFile::OatFile(const std::string& location)
    : location_(location) {
  CHECK(!location_.empty());
}

OatFile::~OatFile() {
  STLDeleteValues(&oat_dex_files_);
}

bool OatFile::Map(File& file,
                  byte* requested_base,
                  RelocationBehavior /*UNUSED*/,
                  bool writable) {
  OatHeader oat_header;
  bool success = file.ReadFully(&oat_header, sizeof(oat_header));
  if (!success || !oat_header.IsValid()) {
    LOG(WARNING) << "Invalid oat header " << GetLocation();
    return false;
  }

  int flags = 0;
  int prot = 0;
  if (writable) {
    flags |= MAP_SHARED;  // So changes will write through to file
    prot |= (PROT_READ | PROT_WRITE);
  } else {
    flags |= MAP_PRIVATE;
    prot |= PROT_READ;
  }
  if (requested_base != NULL) {
    flags |= MAP_FIXED;
  }
  UniquePtr<MemMap> map(MemMap::MapFileAtAddress(requested_base,
                                                 file.GetLength(),
                                                 prot,
                                                 flags,
                                                 file.Fd(),
                                                 0));
  if (map.get() == NULL) {
    LOG(WARNING) << "Failed to map oat file from " << file.GetPath() << " for " << GetLocation();
    return false;
  }
  CHECK(requested_base == 0 || requested_base == map->Begin())
      << file.GetPath() << " for " << GetLocation() << " " << reinterpret_cast<void*>(map->Begin());
  DCHECK_EQ(0, memcmp(&oat_header, map->Begin(), sizeof(OatHeader)))
      << file.GetPath() << " for " << GetLocation();

  off_t code_offset = oat_header.GetExecutableOffset();
  if (code_offset < file.GetLength()) {
    byte* code_address = map->Begin() + code_offset;
    size_t code_length = file.GetLength() - code_offset;
    if (mprotect(code_address, code_length, prot | PROT_EXEC) != 0) {
      PLOG(ERROR) << "Failed to make oat code executable in "
                  << file.GetPath() << " for " << GetLocation();
      return false;
    }
  } else {
    // its possible to have no code if all the methods were abstract, native, etc
    DCHECK_EQ(code_offset, RoundUp(file.GetLength(), kPageSize))
        << file.GetPath() << " for " << GetLocation();
  }

  const byte* oat = map->Begin();

  oat += sizeof(OatHeader);
  oat += oat_header.GetImageFileLocationSize();

  CHECK_LE(oat, map->End())
    << reinterpret_cast<void*>(map->Begin())
    << "+" << sizeof(OatHeader)
    << "+" << oat_header.GetImageFileLocationSize()
    << "<=" << reinterpret_cast<void*>(map->End())
    << " " << file.GetPath() << " for " << GetLocation();
  for (size_t i = 0; i < oat_header.GetDexFileCount(); i++) {
    size_t dex_file_location_size = *reinterpret_cast<const uint32_t*>(oat);
    CHECK_GT(dex_file_location_size, 0U) << file.GetPath() << " for " << GetLocation();
    oat += sizeof(dex_file_location_size);
    CHECK_LT(oat, map->End()) << file.GetPath() << " for " << GetLocation();

    const char* dex_file_location_data = reinterpret_cast<const char*>(oat);
    oat += dex_file_location_size;
    CHECK_LT(oat, map->End()) << file.GetPath() << " for " << GetLocation();

    std::string dex_file_location(dex_file_location_data, dex_file_location_size);

    uint32_t dex_file_checksum = *reinterpret_cast<const uint32_t*>(oat);
    oat += sizeof(dex_file_checksum);
    CHECK_LT(oat, map->End()) << file.GetPath() << " for " << GetLocation();

    uint32_t dex_file_offset = *reinterpret_cast<const uint32_t*>(oat);
    CHECK_GT(dex_file_offset, 0U) << file.GetPath() << " for " << GetLocation();
    CHECK_LT(dex_file_offset, static_cast<uint32_t>(file.GetLength()))
        << file.GetPath() << " for " << GetLocation();
    oat += sizeof(dex_file_offset);
    CHECK_LT(oat, map->End()) << file.GetPath() << " for " << GetLocation();

    uint8_t* dex_file_pointer = map->Begin() + dex_file_offset;
    CHECK(DexFile::IsMagicValid(dex_file_pointer))
        << file.GetPath() << " for " << GetLocation() << " " << dex_file_pointer;
    CHECK(DexFile::IsVersionValid(dex_file_pointer))
        << file.GetPath() << " for " << GetLocation() << " "  << dex_file_pointer;
    const DexFile::Header* header = reinterpret_cast<const DexFile::Header*>(dex_file_pointer);
    const uint32_t* methods_offsets_pointer = reinterpret_cast<const uint32_t*>(oat);

    oat += (sizeof(*methods_offsets_pointer) * header->class_defs_size_);
    CHECK_LE(oat, map->End()) << file.GetPath() << " for " << GetLocation();

    oat_dex_files_.Put(dex_file_location, new OatDexFile(this,
                                                         dex_file_location,
                                                         dex_file_checksum,
                                                         dex_file_pointer,
                                                         methods_offsets_pointer));
  }

  mem_map_.reset(map.release());
  return true;
}

const OatHeader& OatFile::GetOatHeader() const {
  return *reinterpret_cast<const OatHeader*>(Begin());
}

const byte* OatFile::Begin() const {
  CHECK(mem_map_->Begin() != NULL);
  return mem_map_->Begin();
}

const byte* OatFile::End() const {
  CHECK(mem_map_->End() != NULL);
  return mem_map_->End();
}

const OatFile::OatDexFile* OatFile::GetOatDexFile(const std::string& dex_file_location,
                                                  bool warn_if_not_found) const {
  Table::const_iterator it = oat_dex_files_.find(dex_file_location);
  if (it == oat_dex_files_.end()) {
    if (warn_if_not_found) {
      LOG(WARNING) << "Failed to find OatDexFile for DexFile " << dex_file_location;
    }
    return NULL;
  }
  return it->second;
}

std::vector<const OatFile::OatDexFile*> OatFile::GetOatDexFiles() const {
  std::vector<const OatFile::OatDexFile*> result;
  for (Table::const_iterator it = oat_dex_files_.begin(); it != oat_dex_files_.end(); ++it) {
    result.push_back(it->second);
  }
  return result;
}

void OatFile::RelocateExecutable() {
#if defined(ART_USE_LLVM_COMPILER)
  UNIMPLEMENTED(WARNING) << "Relocate the executable";
#endif
}

OatFile::OatDexFile::OatDexFile(const OatFile* oat_file,
                                const std::string& dex_file_location,
                                uint32_t dex_file_location_checksum,
                                byte* dex_file_pointer,
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

const DexFile* OatFile::OatDexFile::OpenDexFile() const {
  return DexFile::Open(dex_file_pointer_, FileSize(), dex_file_location_,
                       dex_file_location_checksum_);
}

const OatFile::OatClass* OatFile::OatDexFile::GetOatClass(uint32_t class_def_index) const {
  uint32_t oat_class_offset = oat_class_offsets_pointer_[class_def_index];

  const byte* oat_class_pointer = oat_file_->Begin() + oat_class_offset;
  CHECK_LT(oat_class_pointer, oat_file_->End());
  Class::Status status = *reinterpret_cast<const Class::Status*>(oat_class_pointer);

  const byte* methods_pointer = oat_class_pointer + sizeof(status);
  CHECK_LT(methods_pointer, oat_file_->End());

  return new OatClass(oat_file_,
                      status,
                      reinterpret_cast<const OatMethodOffsets*>(methods_pointer));
}

OatFile::OatClass::OatClass(const OatFile* oat_file,
                            Class::Status status,
                            const OatMethodOffsets* methods_pointer)
    : oat_file_(oat_file), status_(status), methods_pointer_(methods_pointer) {}

OatFile::OatClass::~OatClass() {}

Class::Status OatFile::OatClass::GetStatus() const {
  return status_;
}

const OatFile::OatMethod OatFile::OatClass::GetOatMethod(uint32_t method_index) const {
  const OatMethodOffsets& oat_method_offsets = methods_pointer_[method_index];
  return OatMethod(
      oat_file_->Begin(),
      oat_method_offsets.code_offset_,
      oat_method_offsets.frame_size_in_bytes_,
      oat_method_offsets.core_spill_mask_,
      oat_method_offsets.fp_spill_mask_,
      oat_method_offsets.mapping_table_offset_,
      oat_method_offsets.vmap_table_offset_,
      oat_method_offsets.gc_map_offset_,
      oat_method_offsets.invoke_stub_offset_
#if defined(ART_USE_LLVM_COMPILER)
    , oat_method_offsets.proxy_stub_offset_
#endif
      );
}

OatFile::OatMethod::OatMethod(const byte* base,
                              const uint32_t code_offset,
                              const size_t frame_size_in_bytes,
                              const uint32_t core_spill_mask,
                              const uint32_t fp_spill_mask,
                              const uint32_t mapping_table_offset,
                              const uint32_t vmap_table_offset,
                              const uint32_t gc_map_offset,
                              const uint32_t invoke_stub_offset
#if defined(ART_USE_LLVM_COMPILER)
                            , const uint32_t proxy_stub_offset
#endif
                              )
  : begin_(base),
    code_offset_(code_offset),
    frame_size_in_bytes_(frame_size_in_bytes),
    core_spill_mask_(core_spill_mask),
    fp_spill_mask_(fp_spill_mask),
    mapping_table_offset_(mapping_table_offset),
    vmap_table_offset_(vmap_table_offset),
    native_gc_map_offset_(gc_map_offset),
    invoke_stub_offset_(invoke_stub_offset)
#if defined(ART_USE_LLVM_COMPILER)
  , proxy_stub_offset_(proxy_stub_offset)
#endif
{
#ifndef NDEBUG
  if (mapping_table_offset_ != 0) {  // implies non-native, non-stub code
    if (vmap_table_offset_ == 0) {
      DCHECK_EQ(0U, static_cast<uint32_t>(__builtin_popcount(core_spill_mask_) + __builtin_popcount(fp_spill_mask_)));
    } else {
      const uint16_t* vmap_table_ = reinterpret_cast<const uint16_t*>(begin_ + vmap_table_offset_);
      DCHECK_EQ(vmap_table_[0], static_cast<uint32_t>(__builtin_popcount(core_spill_mask_) + __builtin_popcount(fp_spill_mask_)));
    }
  } else {
    DCHECK_EQ(vmap_table_offset_, 0U);
  }
#endif
}

OatFile::OatMethod::~OatMethod() {}

const void* OatFile::OatMethod::GetCode() const {
  return GetOatPointer<const void*>(code_offset_);
}

uint32_t OatFile::OatMethod::GetCodeSize() const {
  uintptr_t code = reinterpret_cast<uint32_t>(GetCode());

  if (code == 0) {
    return 0;
  }
  // TODO: make this Thumb2 specific
  code &= ~0x1;
  return reinterpret_cast<uint32_t*>(code)[-1];
}

AbstractMethod::InvokeStub* OatFile::OatMethod::GetInvokeStub() const {
  const byte* stub = GetOatPointer<const byte*>(invoke_stub_offset_);
  return reinterpret_cast<AbstractMethod::InvokeStub*>(const_cast<byte*>(stub));
}

uint32_t OatFile::OatMethod::GetInvokeStubSize() const {
  uintptr_t code = reinterpret_cast<uint32_t>(GetInvokeStub());
  if (code == 0) {
    return 0;
  }
  // TODO: make this Thumb2 specific
  code &= ~0x1;
  return reinterpret_cast<uint32_t*>(code)[-1];
}

#if defined(ART_USE_LLVM_COMPILER)
const void* OatFile::OatMethod::GetProxyStub() const {
  return GetOatPointer<const void*>(proxy_stub_offset_);
}
#endif

void OatFile::OatMethod::LinkMethodPointers(AbstractMethod* method) const {
  CHECK(method != NULL);
  method->SetCode(GetCode());
  method->SetFrameSizeInBytes(frame_size_in_bytes_);
  method->SetCoreSpillMask(core_spill_mask_);
  method->SetFpSpillMask(fp_spill_mask_);
  method->SetMappingTable(GetMappingTable());
  method->SetVmapTable(GetVmapTable());
  method->SetNativeGcMap(GetNativeGcMap());  // Note, used by native methods in work around JNI mode.
  method->SetInvokeStub(GetInvokeStub());
}

void OatFile::OatMethod::LinkMethodOffsets(AbstractMethod* method) const {
  CHECK(method != NULL);
  method->SetOatCodeOffset(GetCodeOffset());
  method->SetFrameSizeInBytes(GetFrameSizeInBytes());
  method->SetCoreSpillMask(GetCoreSpillMask());
  method->SetFpSpillMask(GetFpSpillMask());
  method->SetOatMappingTableOffset(GetMappingTableOffset());
  method->SetOatVmapTableOffset(GetVmapTableOffset());
  method->SetOatNativeGcMapOffset(GetNativeGcMapOffset());
  method->SetOatInvokeStubOffset(GetInvokeStubOffset());
}

}  // namespace art
