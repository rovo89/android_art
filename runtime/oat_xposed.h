#ifndef ART_RUNTIME_OAT_XPOSED_H_
#define ART_RUNTIME_OAT_XPOSED_H_

#include <stdint.h>
#include <vector>

#include "base/array_slice.h"
#include "base/logging.h"
#include "base/macros.h"

namespace art {

class MemMap;
class OatFile;
class OatXposedDexFile;

class OatXposedHeader {
 public:
  static constexpr uint8_t kOatXposedMagic[] = { 'X', 'p', 'o', '\n' };
  static constexpr uint8_t kOatXposedVersion[] = { '0', '0', '1', '\0' };

  OatXposedHeader(uint32_t oat_file_checksum, uint32_t dex_file_count);

  bool IsValid() const;

  const char* GetMagic() const;

  uint32_t GetOatFileChecksum() const {
    DCHECK(IsValid());
    return oat_file_checksum_;
  }

  uint32_t GetDexFileCount() const {
    DCHECK(IsValid());
    return dex_file_count_;
  }

private:
  uint8_t magic_[4];
  uint8_t version_[4];
  uint32_t oat_file_checksum_;
  uint32_t dex_file_count_;

  DISALLOW_COPY_AND_ASSIGN(OatXposedHeader);
};

class OatXposedFile {
 public:
  OatXposedFile(const std::string& location, const uint8_t* begin, const uint8_t* end);

  static OatXposedFile* OpenFromFile(const char* filename, std::string* error_msg);

  const std::string& GetLocation() const {
    return location_;
  }

  const OatXposedHeader& GetOatXposedHeader() const;

  const std::vector<const OatXposedDexFile*>& GetOatXposedDexFiles() const {
    return oat_xposed_dex_files_storage_;
  }

  size_t Size() const {
    return End() - Begin();
  }

  const uint8_t* Begin() const {
    return begin_;
  }

  const uint8_t* End() const {
    return end_;
  }

  bool IsEmbedded() const {
    return mem_map_.get() == nullptr;
  }

  bool Setup(std::string* error_msg);

  bool Validate(const OatFile& oat_file, std::string* error_msg) const;

 private:
  OatXposedFile(const std::string& location, MemMap* mem_map);

  const std::string location_;

  // Pointer to OatXposedHeader.
  const uint8_t* begin_;

  // Pointer to end of oat region for bounds checking.
  const uint8_t* end_;

  // Manages the underlying memory allocation.
  std::unique_ptr<MemMap> mem_map_;

  // Owning storage for the OatXposedDexFile objects.
  std::vector<const OatXposedDexFile*> oat_xposed_dex_files_storage_;

  DISALLOW_COPY_AND_ASSIGN(OatXposedFile);
};

class OatXposedDexFile {
 public:

  // Returns the hashes of methods called by the given method.
  ArraySlice<const uint32_t> GetCalledMethods(uint32_t method_index) const;

  // Returns the indexes of the methods calling a method with the given hash.
  std::vector<uint16_t> GetCallers(uint32_t hash) const;

  // Returns whether a method with the given hash is called, but not declared in the dex file.
  bool HasForeignHash(uint32_t hash) const {
    return std::binary_search(foreign_hashes_.begin(), foreign_hashes_.end(), hash);
  }

 private:
  OatXposedDexFile(uint32_t num_methods,
                   const uint16_t* called_methods_num,
                   const uint32_t* called_methods,
                   ArraySlice<const uint32_t> foreign_hashes);

  uint32_t num_methods_;
  const uint16_t* called_methods_num_;
  const uint32_t* called_methods_;
  ArraySlice<const uint32_t> foreign_hashes_;

  friend class OatXposedFile;
  DISALLOW_COPY_AND_ASSIGN(OatXposedDexFile);
};

}  // namespace art

#endif  // ART_RUNTIME_OAT_XPOSED_H_
