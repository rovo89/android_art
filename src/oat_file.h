// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_OAT_FILE_H_
#define ART_SRC_OAT_FILE_H_

#include <vector>

#include "dex_file.h"
#include "mem_map.h"
#include "oat.h"

namespace art {

class OatFile {
 public:

  // Open an oat file. Returns NULL on failure.  Requested base can
  // optionally be used to request where the file should be loaded.
  static OatFile* Open(const std::string& filename,
                       const std::string& strip_location_prefix,
                       byte* requested_base);

  ~OatFile();

  const std::string& GetLocation() const {
    return location_;
  }

  const OatHeader& GetOatHeader() const;

  class OatDexFile;

  class OatClass {
   public:
    // get the code for the method based on its index into the class
    // defintion. direct methods come first, followed by virtual
    // methods. note that runtime created methods such as miranda
    // methods are not included.
    const void* GetMethodCode(uint32_t method_index) const;
    ~OatClass();

   private:
    OatClass(const OatFile* oat_file, const uint32_t* methods_pointer);

    const OatFile* oat_file_;
    const uint32_t* methods_pointer_;

    friend class OatDexFile;
  };

  class OatDexFile {
   public:
    const OatClass GetOatClass(uint32_t class_def_index) const;

    uint32_t GetDexFileChecksum() const {
      return dex_file_checksum_;
    }

    ~OatDexFile();
   private:
    OatDexFile(const OatFile* oat_file,
               std::string dex_file_location,
               uint32_t dex_file_checksum,
               uint32_t* classes_pointer);

    const OatFile* oat_file_;
    std::string dex_file_location_;
    uint32_t dex_file_checksum_;
    const uint32_t* classes_pointer_;

    friend class OatFile;
    DISALLOW_COPY_AND_ASSIGN(OatDexFile);
  };

  const OatDexFile& GetOatDexFile(const std::string& dex_file_location);

  size_t GetSize() const {
    return GetLimit() - GetBase();
  }

 private:
  OatFile(const std::string& filename);
  bool Read(const std::string& filename, byte* requested_base);

  const byte* GetBase() const;
  const byte* GetLimit() const;

  // The oat file name.
  //
  // The image will embed this to link its associated oat file.
  const std::string location_;

  // backing memory map for oat file
  UniquePtr<MemMap> mem_map_;

  typedef std::map<std::string, const OatDexFile*> Table;
  Table oat_dex_files_;

  friend class OatClass;
  friend class OatDexFile;
  DISALLOW_COPY_AND_ASSIGN(OatFile);
};

}  // namespace art

#endif  // ART_SRC_OAT_WRITER_H_
