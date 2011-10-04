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
// OatDexFile[0]     each fixed length with offset to variable sized OatClasses
// OatDexFile[1]
// ...
// OatDexFile[D]
//
// OatClasses[0]     one variable sized OatClasses for each OatDexFile
// OatClasses[1]     contains DexFile::NumClassDefs offsets to OatMethods for each ClassDef
// ...
// OatClasses[D]
//
// OatMethods[0]     one variable sized OatMethods for each of C DexFile::ClassDefs
// OatMethods[1]     contains OatMethod entries with offsets to code, method properities, etc.
// ...
// OatMethods[C]
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
  static bool Create(const std::string& filename,
                     const ClassLoader* class_loader,
                     const Compiler& compiler);

 private:

  OatWriter(const std::vector<const DexFile*>& dex_files,
            const ClassLoader* class_loader,
            const Compiler& compiler);
  ~OatWriter();

  size_t InitOatHeader();
  size_t InitOatDexFiles(size_t offset);
  size_t InitOatClasses(size_t offset);
  size_t InitOatMethods(size_t offset);
  size_t InitOatCode(size_t offset);
  size_t InitOatCodeDexFiles(size_t offset);
  size_t InitOatCodeDexFile(size_t offset,
                            size_t& oat_class_index,
                            const DexFile& dex_file);
  size_t InitOatCodeClassDef(size_t offset,
                             size_t oat_class_index,
                             const DexFile& dex_file,
                             const DexFile::ClassDef& class_def);
  size_t InitOatCodeMethod(size_t offset,
                           size_t oat_class_index,
                           size_t class_def_method_index,
                           Method* method);

  bool Write(const std::string& filename);
  bool WriteTables(File* file);
  size_t WriteCode(File* file);
  size_t WriteCodeDexFiles(File* file,
                           size_t offset);
  size_t WriteCodeDexFile(File* file,
                          size_t offset,
                          const DexFile& dex_file);
  size_t WriteCodeClassDef(File* file,
                           size_t offset,
                           const DexFile& dex_file,
                           const DexFile::ClassDef& class_def);
  size_t WriteCodeMethod(File* file,
                         size_t offset,
                         Method* method);

  class OatDexFile {
   public:
    OatDexFile(const DexFile& dex_file);
    size_t SizeOf() const;
    void UpdateChecksum(OatHeader& oat_header) const;
    bool Write(File* file) const;

    // data to write
    uint32_t dex_file_location_size_;
    const uint8_t* dex_file_location_data_;
    uint32_t dex_file_checksum_;
    uint32_t classes_offset_;

   private:
    DISALLOW_COPY_AND_ASSIGN(OatDexFile);
  };

  class OatClasses {
   public:
    OatClasses(const DexFile& dex_file);
    size_t SizeOf() const;
    void UpdateChecksum(OatHeader& oat_header) const;
    bool Write(File* file) const;

    // data to write
    std::vector<uint32_t> methods_offsets_;

   private:
    DISALLOW_COPY_AND_ASSIGN(OatClasses);
  };

  class OatMethods {
   public:
    OatMethods(uint32_t methods_count);
    size_t SizeOf() const;
    void UpdateChecksum(OatHeader& oat_header) const;
    bool Write(File* file) const;

    // data to write
    std::vector<OatMethodOffsets> method_offsets_;

   private:
    DISALLOW_COPY_AND_ASSIGN(OatMethods);
  };

  const Compiler* compiler_;

  // TODO: remove the ClassLoader when the code storage moves out of Method
  const ClassLoader* class_loader_;

  // note OatFile does not take ownership of the DexFiles
  const std::vector<const DexFile*>* dex_files_;

  // data to write
  OatHeader* oat_header_;
  std::vector<OatDexFile*> oat_dex_files_;
  std::vector<OatClasses*> oat_classes_;
  std::vector<OatMethods*> oat_methods_;
  uint32_t executable_offset_padding_length_;

  DISALLOW_COPY_AND_ASSIGN(OatWriter);
};

}  // namespace art

#endif  // ART_SRC_OAT_WRITER_H_
