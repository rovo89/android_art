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

#include <vector>

#include "dex_file.h"
#include "invoke_type.h"
#include "mem_map.h"
#include "oat.h"
#include "object.h"

#if defined(ART_USE_LLVM_COMPILER)
namespace art {
  namespace compiler_llvm {
    class ElfLoader;
  }
}
#endif

namespace art {

class OatFile {
 public:
  enum RelocationBehavior {
    kRelocNone,
    kRelocAll,
  };

  // Returns an OatFile name based on a DexFile location
  static std::string DexFilenameToOatFilename(const std::string& location);

  // Open an oat file. Returns NULL on failure.  Requested base can
  // optionally be used to request where the file should be loaded.
  static OatFile* Open(const std::string& filename,
                       const std::string& location,
                       byte* requested_base,
                       RelocationBehavior reloc,
                       bool writable = false);

  // Open an oat file from an already opened File with the given location.
  static OatFile* Open(File& file,
                       const std::string& location,
                       byte* requested_base,
                       RelocationBehavior reloc,
                       bool writable = false);

  ~OatFile();

  const std::string& GetLocation() const {
    return location_;
  }

  const OatHeader& GetOatHeader() const;

  class OatDexFile;

  class OatMethod {
   public:
    // Link Method for execution using the contents of this OatMethod
    void LinkMethodPointers(Method* method) const;

    // Link Method for image writing using the contents of this OatMethod
    void LinkMethodOffsets(Method* method) const;

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
    uint32_t GetGcMapOffset() const {
      return gc_map_offset_;
    }
    uint32_t GetInvokeStubOffset() const {
      return invoke_stub_offset_;
    }

#if defined(ART_USE_LLVM_COMPILER)
    uint32_t GetCodeElfIndex() const {
      return code_elf_idx_;
    }
    uint32_t GetInvokeStubElfIndex() const {
      return invoke_stub_elf_idx_;
    }
#endif

    bool IsCodeInElf() const {
#if defined(ART_USE_LLVM_COMPILER)
      return (code_elf_idx_ != -1u);
#else
      return false;
#endif
    }

    bool IsInvokeStubInElf() const {
#if defined(ART_USE_LLVM_COMPILER)
      return (invoke_stub_elf_idx_ != -1u);
#else
      return false;
#endif
    }

    const void* GetCode() const;
    uint32_t GetCodeSize() const;

    const uint32_t* GetMappingTable() const {
      return GetOatPointer<const uint32_t*>(mapping_table_offset_);
    }
    const uint16_t* GetVmapTable() const {
      return GetOatPointer<const uint16_t*>(vmap_table_offset_);
    }
    const uint8_t* GetGcMap() const {
      return GetOatPointer<const uint8_t*>(gc_map_offset_);
    }

    const Method::InvokeStub* GetInvokeStub() const;
    uint32_t GetInvokeStubSize() const;

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
            , const compiler_llvm::ElfLoader* elf_loader,
              const uint32_t code_elf_idx,
              const uint32_t invoke_stub_elf_idx
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
    uint32_t gc_map_offset_;
    uint32_t invoke_stub_offset_;

#if defined(ART_USE_LLVM_COMPILER)
    const compiler_llvm::ElfLoader* elf_loader_;

    uint32_t code_elf_idx_;
    uint32_t invoke_stub_elf_idx_;
#endif

    friend class OatClass;
  };

  class OatClass {
   public:
    Class::Status GetStatus() const;

    // get the OatMethod entry based on its index into the class
    // defintion. direct methods come first, followed by virtual
    // methods. note that runtime created methods such as miranda
    // methods are not included.
    const OatMethod GetOatMethod(uint32_t method_index) const;
    ~OatClass();

   private:
    OatClass(const OatFile* oat_file,
             Class::Status status,
             const OatMethodOffsets* methods_pointer);

    const OatFile* oat_file_;
    const Class::Status status_;
    const OatMethodOffsets* methods_pointer_;

    friend class OatDexFile;
  };

  class OatDexFile {
   public:
    const DexFile* OpenDexFile() const;
    const OatClass* GetOatClass(uint32_t class_def_index) const;

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
               byte* dex_file_pointer,
               const uint32_t* oat_class_offsets_pointer);

    const OatFile* oat_file_;
    std::string dex_file_location_;
    uint32_t dex_file_location_checksum_;
    const byte* dex_file_pointer_;
    const uint32_t* oat_class_offsets_pointer_;

    friend class OatFile;
    DISALLOW_COPY_AND_ASSIGN(OatDexFile);
  };

  class OatElfImage {
   public:
    const byte* begin() const {
      return elf_addr_;
    }

    const byte* end() const {
      return (elf_addr_ + elf_size_);
    }

    size_t size() const {
      return elf_size_;
    }

   private:
    OatElfImage(const OatFile* oat_file, const byte* addr, uint32_t size);

    const OatFile* oat_file_;
    const byte* elf_addr_;
    uint32_t elf_size_;

    friend class OatFile;
    DISALLOW_COPY_AND_ASSIGN(OatElfImage);
  };

  const OatDexFile* GetOatDexFile(const std::string& dex_file_location,
                                  bool warn_if_not_found = true) const;
  std::vector<const OatDexFile*> GetOatDexFiles() const;

#if defined(ART_USE_LLVM_COMPILER)
  const OatElfImage* GetOatElfImage(size_t i) const {
    return oat_elf_images_[i];
  }
#endif

  size_t Size() const {
    return End() - Begin();
  }

  void RelocateExecutable();

 private:
  explicit OatFile(const std::string& filename);
  bool Map(File& file, byte* requested_base, RelocationBehavior reloc, bool writable);

  const byte* Begin() const;
  const byte* End() const;

  // The oat file name.
  //
  // The image will embed this to link its associated oat file.
  const std::string location_;

  // backing memory map for oat file
  UniquePtr<MemMap> mem_map_;

  typedef std::map<std::string, const OatDexFile*> Table;
  Table oat_dex_files_;

#if defined(ART_USE_LLVM_COMPILER)
  std::vector<OatElfImage*> oat_elf_images_;
  UniquePtr<compiler_llvm::ElfLoader> elf_loader_;
#endif

  friend class OatClass;
  friend class OatDexFile;
  friend class OatDumper;  // For GetBase and GetLimit
  DISALLOW_COPY_AND_ASSIGN(OatFile);
};

}  // namespace art

#endif  // ART_SRC_OAT_WRITER_H_
