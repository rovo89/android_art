// Copyright 2011 Google Inc. All Rights Reserved.

#include "oat_file.h"

#include <sys/mman.h>

#include "file.h"
#include "os.h"
#include "stl_util.h"

namespace art {

std::string OatFile::DexFileToOatFilename(const DexFile& dex_file) {
  std::string location(dex_file.GetLocation());
  CHECK(StringPiece(location).ends_with(".dex")
        || StringPiece(location).ends_with(".zip")
        || StringPiece(location).ends_with(".jar")
        || StringPiece(location).ends_with(".apk"));
  location.erase(location.size()-3);
  location += "oat";
  return location;
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

  UniquePtr<MemMap> map(MemMap::Map(requested_base,
                                    file->Length(),
                                    PROT_READ,
                                    MAP_PRIVATE | ((requested_base != NULL) ? MAP_FIXED : 0),
                                    file->Fd(),
                                    0));
  if (map.get() == NULL) {
    LOG(WARNING) << "Failed to map oat file " << filename;
    return false;
  }
  CHECK(requested_base == 0 || requested_base == map->GetAddress()) << map->GetAddress();
  DCHECK_EQ(0, memcmp(&oat_header, map->GetAddress(), sizeof(OatHeader)));

  off_t code_offset = oat_header.GetExecutableOffset();
  if (code_offset < file->Length()) {
    byte* code_address = map->GetAddress() + code_offset;
    size_t code_length = file->Length() - code_offset;
    if (mprotect(code_address, code_length, PROT_READ | PROT_EXEC) != 0) {
      PLOG(ERROR) << "Failed to make oat code executable.";
      return false;
    }
  } else {
    // its possible to have no code if all the methods were abstract, native, etc
    DCHECK_EQ(code_offset, RoundUp(file->Length(), kPageSize));
  }

  const byte* oat = map->GetAddress();
  oat += sizeof(OatHeader);
  CHECK_LT(oat, map->GetLimit());
  for (size_t i = 0; i < oat_header.GetDexFileCount(); i++) {
    size_t dex_file_location_size = *reinterpret_cast<const uint32_t*>(oat);
    oat += sizeof(dex_file_location_size);
    CHECK_LT(oat, map->GetLimit());

    const char* dex_file_location_data = reinterpret_cast<const char*>(oat);
    oat += dex_file_location_size;
    CHECK_LT(oat, map->GetLimit());

    std::string dex_file_location(dex_file_location_data, dex_file_location_size);

    uint32_t dex_file_checksum = *reinterpret_cast<const uint32_t*>(oat);
    oat += sizeof(dex_file_checksum);
    CHECK_LT(oat, map->GetLimit());

    uint32_t classes_offset = *reinterpret_cast<const uint32_t*>(oat);
    CHECK_GT(classes_offset, 0U);
    CHECK_LT(classes_offset, static_cast<uint32_t>(file->Length()));
    oat += sizeof(classes_offset);
    CHECK_LT(oat, map->GetLimit());

    uint32_t* classes_pointer = reinterpret_cast<uint32_t*>(map->GetAddress() + classes_offset);

    oat_dex_files_[dex_file_location] = new OatDexFile(this,
                                                       dex_file_location,
                                                       dex_file_checksum,
                                                       classes_pointer);
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

const OatFile::OatDexFile* OatFile::GetOatDexFile(const std::string& dex_file_location) const {
  Table::const_iterator it = oat_dex_files_.find(dex_file_location);
  if (it == oat_dex_files_.end()) {
    LOG(WARNING) << "Failed to find OatDexFile for DexFile " << dex_file_location;
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
                                std::string dex_file_location,
                                uint32_t dex_file_checksum,
                                uint32_t* classes_pointer)
    : oat_file_(oat_file),
      dex_file_location_(dex_file_location),
      dex_file_checksum_(dex_file_checksum),
      classes_pointer_(classes_pointer) {}

OatFile::OatDexFile::~OatDexFile() {}

const OatFile::OatClass* OatFile::OatDexFile::GetOatClass(uint32_t class_def_index) const {
  uint32_t methods_offset = classes_pointer_[class_def_index];
  const byte* methods_pointer = oat_file_->GetBase() + methods_offset;
  CHECK_LT(methods_pointer, oat_file_->GetLimit());
  return new OatClass(oat_file_, reinterpret_cast<const OatMethodOffsets*>(methods_pointer));
}

OatFile::OatClass::OatClass(const OatFile* oat_file, const OatMethodOffsets* methods_pointer)
    : oat_file_(oat_file), methods_pointer_(methods_pointer) {}

OatFile::OatClass::~OatClass() {}

const OatFile::OatMethod OatFile::OatClass::GetOatMethod(uint32_t method_index) const {
  const OatMethodOffsets& oat_method_offsets = methods_pointer_[method_index];
  return OatMethod(
      GetOatPointer<const void*>(oat_method_offsets.code_offset_),
      oat_method_offsets.frame_size_in_bytes_,
      oat_method_offsets.return_pc_offset_in_bytes_,
      oat_method_offsets.core_spill_mask_,
      oat_method_offsets.fp_spill_mask_,
      GetOatPointer<const uint32_t*>(oat_method_offsets.mapping_table_offset_),
      GetOatPointer<const uint16_t*>(oat_method_offsets.vmap_table_offset_),
      GetOatPointer<const Method::InvokeStub*>(oat_method_offsets.invoke_stub_offset_));
}

OatFile::OatMethod::OatMethod(const void* code,
                              const size_t frame_size_in_bytes,
                              const size_t return_pc_offset_in_bytes,
                              const uint32_t core_spill_mask,
                              const uint32_t fp_spill_mask,
                              const uint32_t* mapping_table,
                              const uint16_t* vmap_table,
                              const Method::InvokeStub* invoke_stub) :
  code_(code),
  frame_size_in_bytes_(frame_size_in_bytes),
  return_pc_offset_in_bytes_(return_pc_offset_in_bytes),
  core_spill_mask_(core_spill_mask),
  fp_spill_mask_(fp_spill_mask),
  mapping_table_(mapping_table),
  vmap_table_(vmap_table),
  invoke_stub_(invoke_stub) {}

OatFile::OatMethod::~OatMethod() {}

void OatFile::OatMethod::LinkMethod(Method* method) const {
  CHECK(method != NULL);
  method->SetCode(code_);
  method->SetFrameSizeInBytes(frame_size_in_bytes_);
  method->SetReturnPcOffsetInBytes(return_pc_offset_in_bytes_);
  method->SetCoreSpillMask(core_spill_mask_);
  method->SetFpSpillMask(fp_spill_mask_);
  method->SetMappingTable(mapping_table_);
  method->SetVmapTable(vmap_table_);
  method->SetInvokeStub(invoke_stub_);
}

}  // namespace art
