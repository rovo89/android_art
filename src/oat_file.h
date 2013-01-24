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

#ifndef ART_SRC_OAT_FILE_H_
#define ART_SRC_OAT_FILE_H_

#include <string>
#include <vector>

#include "dex_file.h"
#include "invoke_type.h"
#include "mem_map.h"
#include "mirror/abstract_method.h"
#include "oat.h"
#include "os.h"

namespace art {

class ElfFile;
class MemMap;
class OatMethodOffsets;
struct OatHeader;

class OatFile {
 public:
  // Returns an OatFile name based on a DexFile location
  static std::string DexFilenameToOatFilename(const std::string& location);

  // Open an oat file. Returns NULL on failure.  Requested base can
  // optionally be used to request where the file should be loaded.
  static OatFile* Open(const std::string& filename,
                       const std::string& location,
                       byte* requested_base);

  // Open an oat file from an already opened File.
  static OatFile* Open(File* file,
                       const std::string& location,
                       byte* requested_base,
                       bool writable);

  // Open an oat file backed by a std::vector with the given location.
  static OatFile* Open(std::vector<uint8_t>& oat_contents,
                       const std::string& location);

  ~OatFile();

  const std::string& GetLocation() const {
    return location_;
  }

  const OatHeader& GetOatHeader() const;

  class OatDexFile;

  class OatMethod {
   public:
    // Link Method for execution using the contents of this OatMethod
    void LinkMethodPointers(mirror::AbstractMethod* method) const;

    // Link Method for image writing using the contents of this OatMethod
    void LinkMethodOffsets(mirror::AbstractMethod* method) const;

    uint32_t GetCodeOffset() const {
      return code_offset_;
    }
    size_t GetFrameSizeInBytes() const {
      return frame_size_in_bytes_;
    }
    uint32_t GetCoreSpillMask() const {
      return core_spill_mask_;
    }
    uint32_t GetFpSpillMask() const {
      return fp_spill_mask_;
    }
    uint32_t GetMappingTableOffset() const {
      return mapping_table_offset_;
    }
    uint32_t GetVmapTableOffset() const {
      return vmap_table_offset_;
    }
    uint32_t GetNativeGcMapOffset() const {
      return native_gc_map_offset_;
    }
    uint32_t GetInvokeStubOffset() const {
      return invoke_stub_offset_;
    }

    const void* GetCode() const;
    uint32_t GetCodeSize() const;

    const uint32_t* GetMappingTable() const {
      return GetOatPointer<const uint32_t*>(mapping_table_offset_);
    }
    const uint16_t* GetVmapTable() const {
      return GetOatPointer<const uint16_t*>(vmap_table_offset_);
    }
    const uint8_t* GetNativeGcMap() const {
      return GetOatPointer<const uint8_t*>(native_gc_map_offset_);
    }

    mirror::AbstractMethod::InvokeStub* GetInvokeStub() const;
    uint32_t GetInvokeStubSize() const;

#if defined(ART_USE_LLVM_COMPILER)
    const void* GetProxyStub() const;
#endif

    ~OatMethod();

    // Create an OatMethod with offsets relative to the given base address
    OatMethod(const byte* base,
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
              );

   private:
    template<class T>
    T GetOatPointer(uint32_t offset) const {
      if (offset == 0) {
        return NULL;
      }
      return reinterpret_cast<T>(begin_ + offset);
    }

    const byte* begin_;

    uint32_t code_offset_;
    size_t frame_size_in_bytes_;
    uint32_t core_spill_mask_;
    uint32_t fp_spill_mask_;
    uint32_t mapping_table_offset_;
    uint32_t vmap_table_offset_;
    uint32_t native_gc_map_offset_;
    uint32_t invoke_stub_offset_;

#if defined(ART_USE_LLVM_COMPILER)
    uint32_t proxy_stub_offset_;
#endif

    friend class OatClass;
  };

  class OatClass {
   public:
    mirror::Class::Status GetStatus() const;

    // get the OatMethod entry based on its index into the class
    // defintion. direct methods come first, followed by virtual
    // methods. note that runtime created methods such as miranda
    // methods are not included.
    const OatMethod GetOatMethod(uint32_t method_index) const;
    ~OatClass();

   private:
    OatClass(const OatFile* oat_file,
             mirror::Class::Status status,
             const OatMethodOffsets* methods_pointer);

    const OatFile* oat_file_;
    const mirror::Class::Status status_;
    const OatMethodOffsets* methods_pointer_;

    friend class OatDexFile;
  };

  class OatDexFile {
   public:
    const DexFile* OpenDexFile() const;
    const OatClass* GetOatClass(uint32_t class_def_index) const;
    size_t FileSize() const;

    const std::string& GetDexFileLocation() const {
      return dex_file_location_;
    }

    uint32_t GetDexFileLocationChecksum() const {
      return dex_file_location_checksum_;
    }

    ~OatDexFile();

   private:
    OatDexFile(const OatFile* oat_file,
               const std::string& dex_file_location,
               uint32_t dex_file_checksum,
               const byte* dex_file_pointer,
               const uint32_t* oat_class_offsets_pointer);

    const OatFile* oat_file_;
    std::string dex_file_location_;
    uint32_t dex_file_location_checksum_;
    const byte* dex_file_pointer_;
    const uint32_t* oat_class_offsets_pointer_;

    friend class OatFile;
    DISALLOW_COPY_AND_ASSIGN(OatDexFile);
  };

  const OatDexFile* GetOatDexFile(const std::string& dex_file_location,
                                  bool warn_if_not_found = true) const;
  std::vector<const OatDexFile*> GetOatDexFiles() const;

  size_t Size() const {
    return End() - Begin();
  }

 private:
  static void CheckLocation(const std::string& location);

  static OatFile* OpenDlopen(const std::string& elf_filename,
                             const std::string& location,
                             byte* requested_base);

  static OatFile* OpenElfFile(File* file,
                              const std::string& location,
                              byte* requested_base,
                              bool writable);

  explicit OatFile(const std::string& filename);
  bool Dlopen(const std::string& elf_filename, byte* requested_base);
  bool ElfFileOpen(File* file, byte* requested_base, bool writable);
  void Setup();

  const byte* Begin() const;
  const byte* End() const;

  // The oat file name.
  //
  // The image will embed this to link its associated oat file.
  const std::string location_;

  // Pointer to OatHeader.
  const byte* begin_;

  // Pointer to end of oat region for bounds checking.
  const byte* end_;

  // Backing memory map for oat file during when opened by ElfWriter during initial compilation.
  UniquePtr<MemMap> mem_map_;

  // Backing memory map for oat file during cross compilation.
  UniquePtr<ElfFile> elf_file_;

  // dlopen handle during runtime.
  void* dlopen_handle_;

  typedef SafeMap<std::string, const OatDexFile*> Table;
  Table oat_dex_files_;

  friend class OatClass;
  friend class OatDexFile;
  friend class OatDumper;  // For GetBase and GetLimit
  DISALLOW_COPY_AND_ASSIGN(OatFile);
};

}  // namespace art

#endif  // ART_SRC_OAT_WRITER_H_
