// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_OAT_WRITER_H_
#define ART_SRC_OAT_WRITER_H_

#include <stdint.h>

#include <cstddef>

#include "UniquePtr.h"
#include "compiler.h"
#include "dex_cache.h"
#include "mem_map.h"
#include "oat.h"
#include "object.h"
#include "os.h"
#include "space.h"

namespace art {

// OatHeader         fixed length with count of D OatDexFiles
//
// OatDexFile[0]     one variable sized OatDexFile with offsets to Dex and OatClasses
// OatDexFile[1]
// ...
// OatDexFile[D]
//
// Dex[0]            one variable sized DexFile for each OatDexFile.
// Dex[1]            these are literal copies of the input .dex files.
// ...
// Dex[D]
//
// OatClass[0]       one variable sized OatClass for each of C DexFile::ClassDefs
// OatClass[1]       contains OatClass entries with class status, offsets to code, etc.
// ...
// OatClass[C]
//
// padding           if necessary so that the follow code will be page aligned
//
// CompiledMethod    one variable sized blob with the contents of each CompiledMethod
// CompiledMethod
// CompiledMethod
// CompiledMethod
// CompiledMethod
// CompiledMethod
// ...
// CompiledMethod
//
class OatWriter {
 public:
  // Write an oat file. Returns true on success, false on failure.
  static bool Create(File* file, const ClassLoader* class_loader, const Compiler& compiler);

 private:

  OatWriter(const std::vector<const DexFile*>& dex_files,
            const ClassLoader* class_loader,
            const Compiler& compiler);
  ~OatWriter();

  size_t InitOatHeader();
  size_t InitOatDexFiles(size_t offset);
  size_t InitDexFiles(size_t offset);
  size_t InitOatClasses(size_t offset);
  size_t InitOatCode(size_t offset);
  size_t InitOatCodeDexFiles(size_t offset);
  size_t InitOatCodeDexFile(size_t offset,
                            size_t& oat_class_index,
                            const DexFile& dex_file);
  size_t InitOatCodeClassDef(size_t offset,
                             size_t oat_class_index,
                             const DexFile& dex_file,
                             const DexFile::ClassDef& class_def);
  size_t InitOatCodeMethod(size_t offset, size_t oat_class_index, size_t class_def_method_index,
                           bool is_static, bool is_direct, uint32_t method_idx, const DexFile*);

  bool Write(File* file);
  bool WriteTables(File* file);
  size_t WriteCode(File* file);
  size_t WriteCodeDexFiles(File* file, size_t offset);
  size_t WriteCodeDexFile(File* file, size_t offset, size_t& oat_class_index,
                          const DexFile& dex_file);
  size_t WriteCodeClassDef(File* file, size_t offset, size_t oat_class_index,
                           const DexFile& dex_file, const DexFile::ClassDef& class_def);
  size_t WriteCodeMethod(File* file, size_t offset, size_t oat_class_index,
                         size_t class_def_method_index, bool is_static, uint32_t method_idx,
                         const DexFile& dex_file);

  void ReportWriteFailure(const char* what, uint32_t method_idx, const DexFile& dex_file,
                          File* f) const;

  class OatDexFile {
   public:
    explicit OatDexFile(const DexFile& dex_file);
    size_t SizeOf() const;
    void UpdateChecksum(OatHeader& oat_header) const;
    bool Write(File* file) const;

    // data to write
    uint32_t dex_file_location_size_;
    const uint8_t* dex_file_location_data_;
    uint32_t dex_file_checksum_;
    uint32_t dex_file_offset_;
    std::vector<uint32_t> methods_offsets_;

   private:
    DISALLOW_COPY_AND_ASSIGN(OatDexFile);
  };

  class OatClass {
   public:
    explicit OatClass(Class::Status status, uint32_t methods_count);
    size_t SizeOf() const;
    void UpdateChecksum(OatHeader& oat_header) const;
    bool Write(File* file) const;

    // data to write
    Class::Status status_;
    std::vector<OatMethodOffsets> method_offsets_;

   private:
    DISALLOW_COPY_AND_ASSIGN(OatClass);
  };

  const Compiler* compiler_;

  // TODO: remove the ClassLoader when the code storage moves out of Method
  const ClassLoader* class_loader_;

  // note OatFile does not take ownership of the DexFiles
  const std::vector<const DexFile*>* dex_files_;

  // data to write
  OatHeader* oat_header_;
  std::vector<OatDexFile*> oat_dex_files_;
  std::vector<OatClass*> oat_classes_;
  uint32_t executable_offset_padding_length_;

  template <class T> struct MapCompare {
   public:
    bool operator() (const T* const &a, const T* const &b) const {
      return *a < *b;
    }
  };

  // code mappings for deduplication
  std::map<const std::vector<uint8_t>*, uint32_t, MapCompare<std::vector<uint8_t> > > code_offsets_;
  std::map<const std::vector<uint16_t>*, uint32_t, MapCompare<std::vector<uint16_t> > > vmap_table_offsets_;
  std::map<const std::vector<uint32_t>*, uint32_t, MapCompare<std::vector<uint32_t> > > mapping_table_offsets_;

  DISALLOW_COPY_AND_ASSIGN(OatWriter);
};

}  // namespace art

#endif  // ART_SRC_OAT_WRITER_H_
