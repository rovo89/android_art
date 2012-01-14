// Copyright 2011 Google Inc. All Rights Reserved.

#include "oat_file.h"

#include <sys/mman.h>

#include "file.h"
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
                       const std::string& strip_location_prefix,
                       byte* requested_base) {
  StringPiece location = filename;
  if (!location.starts_with(strip_location_prefix)) {
    LOG(ERROR) << filename << " does not start with " << strip_location_prefix;
    return NULL;
  }
  location.remove_prefix(strip_location_prefix.size());

  UniquePtr<OatFile> oat_file(new OatFile(location.ToString()));
  bool success = oat_file->Read(filename, requested_base);
  if (!success) {
    return NULL;
  }
  return oat_file.release();
}

OatFile::OatFile(const std::string& filename) : location_(filename) {}

OatFile::~OatFile() {
  STLDeleteValues(&oat_dex_files_);
}

bool OatFile::Read(const std::string& filename, byte* requested_base) {
  UniquePtr<File> file(OS::OpenFile(filename.c_str(), false));
  if (file.get() == NULL) {
    return false;
  }

  OatHeader oat_header;
  bool success = file->ReadFully(&oat_header, sizeof(oat_header));
  if (!success || !oat_header.IsValid()) {
    LOG(WARNING) << "Invalid oat header " << filename;
    return false;
  }

  int flags = MAP_PRIVATE | ((requested_base != NULL) ? MAP_FIXED : 0);
  UniquePtr<MemMap> map(MemMap::MapFileAtAddress(requested_base,
                                                 file->Length(),
                                                 PROT_READ,
                                                 flags,
                                                 file->Fd(),
                                                 0));
  if (map.get() == NULL) {
    LOG(WARNING) << "Failed to map oat file " << filename;
    return false;
  }
  CHECK(requested_base == 0 || requested_base == map->GetAddress())
          << filename << " " << reinterpret_cast<void*>(map->GetAddress());
  DCHECK_EQ(0, memcmp(&oat_header, map->GetAddress(), sizeof(OatHeader))) << filename;

  off64_t code_offset = oat_header.GetExecutableOffset();
  if (code_offset < file->Length()) {
    byte* code_address = map->GetAddress() + code_offset;
    size_t code_length = file->Length() - code_offset;
    if (mprotect(code_address, code_length, PROT_READ | PROT_EXEC) != 0) {
      PLOG(ERROR) << "Failed to make oat code executable in " << filename;
      return false;
    }
  } else {
    // its possible to have no code if all the methods were abstract, native, etc
    DCHECK_EQ(code_offset, RoundUp(file->Length(), kPageSize)) << filename;
  }

  const byte* oat = map->GetAddress();

  oat += sizeof(OatHeader);
  CHECK_LT(oat, map->GetLimit()) << filename;
  for (size_t i = 0; i < oat_header.GetDexFileCount(); i++) {
    size_t dex_file_location_size = *reinterpret_cast<const uint32_t*>(oat);
    CHECK_GT(dex_file_location_size, 0U) << filename;
    oat += sizeof(dex_file_location_size);
    CHECK_LT(oat, map->GetLimit()) << filename;

    const char* dex_file_location_data = reinterpret_cast<const char*>(oat);
    oat += dex_file_location_size;
    CHECK_LT(oat, map->GetLimit()) << filename;

    std::string dex_file_location(dex_file_location_data, dex_file_location_size);

    uint32_t dex_file_checksum = *reinterpret_cast<const uint32_t*>(oat);
    oat += sizeof(dex_file_checksum);
    CHECK_LT(oat, map->GetLimit()) << filename;

    uint32_t dex_file_offset = *reinterpret_cast<const uint32_t*>(oat);
    CHECK_GT(dex_file_offset, 0U) << filename;
    CHECK_LT(dex_file_offset, static_cast<uint32_t>(file->Length())) << filename;
    oat += sizeof(dex_file_offset);
    CHECK_LT(oat, map->GetLimit()) << filename;

    uint8_t* dex_file_pointer = map->GetAddress() + dex_file_offset;
    CHECK(DexFile::IsMagicValid(dex_file_pointer)) << filename << " " << dex_file_pointer;
    CHECK(DexFile::IsVersionValid(dex_file_pointer)) << filename << " "  << dex_file_pointer;
    const DexFile::Header* header = reinterpret_cast<const DexFile::Header*>(dex_file_pointer);
    const uint32_t* methods_offsets_pointer = reinterpret_cast<const uint32_t*>(oat);

    oat += (sizeof(*methods_offsets_pointer) * header->class_defs_size_);
    CHECK_LT(oat, map->GetLimit()) << filename;

    oat_dex_files_[dex_file_location] = new OatDexFile(this,
                                                       dex_file_location,
                                                       dex_file_checksum,
                                                       dex_file_pointer,
                                                       methods_offsets_pointer);
  }

  mem_map_.reset(map.release());
  return true;
}

const OatHeader& OatFile::GetOatHeader() const {
  return *reinterpret_cast<const OatHeader*>(GetBase());
}

const byte* OatFile::GetBase() const {
  CHECK(mem_map_->GetAddress() != NULL);
  return mem_map_->GetAddress();
}

const byte* OatFile::GetLimit() const {
  CHECK(mem_map_->GetLimit() != NULL);
  return mem_map_->GetLimit();
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

OatFile::OatDexFile::OatDexFile(const OatFile* oat_file,
                                const std::string& dex_file_location,
                                uint32_t dex_file_checksum,
                                byte* dex_file_pointer,
                                const uint32_t* oat_class_offsets_pointer)
    : oat_file_(oat_file),
      dex_file_location_(dex_file_location),
      dex_file_checksum_(dex_file_checksum),
      dex_file_pointer_(dex_file_pointer),
      oat_class_offsets_pointer_(oat_class_offsets_pointer) {}

OatFile::OatDexFile::~OatDexFile() {}

const DexFile* OatFile::OatDexFile::OpenDexFile() const {
  size_t length = reinterpret_cast<const DexFile::Header*>(dex_file_pointer_)->file_size_;
  return DexFile::Open(dex_file_pointer_, length, dex_file_location_);
}

const OatFile::OatClass* OatFile::OatDexFile::GetOatClass(uint32_t class_def_index) const {
  uint32_t oat_class_offset = oat_class_offsets_pointer_[class_def_index];

  const byte* oat_class_pointer = oat_file_->GetBase() + oat_class_offset;
  CHECK_LT(oat_class_pointer, oat_file_->GetLimit());
  Class::Status status = *reinterpret_cast<const Class::Status*>(oat_class_pointer);

  const byte* methods_pointer = oat_class_pointer + sizeof(status);
  CHECK_LT(methods_pointer, oat_file_->GetLimit());

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
      oat_file_->GetBase(),
      oat_method_offsets.code_offset_,
      oat_method_offsets.frame_size_in_bytes_,
      oat_method_offsets.core_spill_mask_,
      oat_method_offsets.fp_spill_mask_,
      oat_method_offsets.mapping_table_offset_,
      oat_method_offsets.vmap_table_offset_,
      oat_method_offsets.invoke_stub_offset_);
}

OatFile::OatMethod::OatMethod(const byte* base,
                              const uint32_t code_offset,
                              const size_t frame_size_in_bytes,
                              const uint32_t core_spill_mask,
                              const uint32_t fp_spill_mask,
                              const uint32_t mapping_table_offset,
                              const uint32_t vmap_table_offset,
                              const uint32_t invoke_stub_offset)
  : base_(base),
    code_offset_(code_offset),
    frame_size_in_bytes_(frame_size_in_bytes),
    core_spill_mask_(core_spill_mask),
    fp_spill_mask_(fp_spill_mask),
    mapping_table_offset_(mapping_table_offset),
    vmap_table_offset_(vmap_table_offset),
    invoke_stub_offset_(invoke_stub_offset) {

#ifndef NDEBUG
  if (mapping_table_offset_ != 0) {  // implies non-native, non-stub code
    if (vmap_table_offset_ == 0) {
      DCHECK_EQ(0U, static_cast<uint32_t>(__builtin_popcount(core_spill_mask_) + __builtin_popcount(fp_spill_mask_)));
    } else {
      const uint16_t* vmap_table_ = reinterpret_cast<const uint16_t*>(base_ + vmap_table_offset_);
      DCHECK_EQ(vmap_table_[0], static_cast<uint32_t>(__builtin_popcount(core_spill_mask_) + __builtin_popcount(fp_spill_mask_)));
    }
  } else {
    DCHECK(vmap_table_offset_ == 0);
  }
#endif
}

OatFile::OatMethod::~OatMethod() {}

void OatFile::OatMethod::LinkMethodPointers(Method* method) const {
  CHECK(method != NULL);
  method->SetCode(GetCode());
  method->SetFrameSizeInBytes(frame_size_in_bytes_);
  method->SetCoreSpillMask(core_spill_mask_);
  method->SetFpSpillMask(fp_spill_mask_);
  method->SetMappingTable(GetMappingTable());
  method->SetVmapTable(GetVmapTable());
  method->SetInvokeStub(GetInvokeStub());
}

void OatFile::OatMethod::LinkMethodOffsets(Method* method) const {
  CHECK(method != NULL);
  method->SetOatCodeOffset(GetCodeOffset());
  method->SetFrameSizeInBytes(GetFrameSizeInBytes());
  method->SetCoreSpillMask(GetCoreSpillMask());
  method->SetFpSpillMask(GetFpSpillMask());
  method->SetOatMappingTableOffset(GetMappingTableOffset());
  method->SetOatVmapTableOffset(GetVmapTableOffset());
  method->SetOatInvokeStubOffset(GetInvokeStubOffset());
}

}  // namespace art
