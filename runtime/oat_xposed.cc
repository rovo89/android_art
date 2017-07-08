#include "oat_xposed.h"

#include <memory>

#include "base/bit_vector.h"
#include "base/stringprintf.h"
#include "oat_file.h"

namespace art {


/////////////////////
// OatXposedHeader //
/////////////////////

constexpr uint8_t OatXposedHeader::kOatXposedMagic[4];
constexpr uint8_t OatXposedHeader::kOatXposedVersion[4];

OatXposedHeader::OatXposedHeader(uint32_t oat_file_checksum, uint32_t dex_file_count)
    : oat_file_checksum_(oat_file_checksum),
      dex_file_count_(dex_file_count) {
  // Don't want asserts in header as they would be checked in each file that includes it. But the
  // fields are private, so we check inside a method.
  static_assert(sizeof(magic_) == sizeof(kOatXposedMagic),
                "Oat Xposed magic and magic_ have different lengths.");
  static_assert(sizeof(version_) == sizeof(kOatXposedVersion),
                "Oat Xposed version and version_ have different lengths.");

  memcpy(magic_, kOatXposedMagic, sizeof(kOatXposedMagic));
  memcpy(version_, kOatXposedVersion, sizeof(kOatXposedVersion));
}

bool OatXposedHeader::IsValid() const {
  if (memcmp(magic_, kOatXposedMagic, sizeof(kOatXposedMagic)) != 0) {
    return false;
  }
  if (memcmp(version_, kOatXposedVersion, sizeof(kOatXposedVersion)) != 0) {
    return false;
  }
  return true;
}

const char* OatXposedHeader::GetMagic() const {
  CHECK(IsValid());
  return reinterpret_cast<const char*>(magic_);
}

///////////////////
// OatXposedFile //
///////////////////

OatXposedFile::OatXposedFile(const std::string& location, const uint8_t* begin, const uint8_t* end)
    : location_(location),
      begin_(begin),
      end_(end) {
  CHECK(!location_.empty());
  CHECK(begin_ != nullptr);
  CHECK(end_ != nullptr);
  CHECK_GE(end_, begin_);
}

const OatXposedHeader& OatXposedFile::GetOatXposedHeader() const {
  return *reinterpret_cast<const OatXposedHeader*>(Begin());
}

// Read an unaligned entry from the OatDexFile data in OatFile and advance the read
// position by the number of bytes read, i.e. sizeof(T).
// Return true on success, false if the read would go beyond the end of the OatFile.
template <typename T>
inline static bool ReadOatXposedDexFileData(const OatXposedFile& oat_xposed_file,
                                            /*inout*/const uint8_t** xposed,
                                            /*out*/T* value) {
  DCHECK(xposed != nullptr);
  DCHECK(value != nullptr);
  DCHECK_LE(*xposed, oat_xposed_file.End());
  if (UNLIKELY(static_cast<size_t>(oat_xposed_file.End() - *xposed) < sizeof(T))) {
    return false;
  }
  static_assert(std::is_trivial<T>::value, "T must be a trivial type");
  typedef __attribute__((__aligned__(1))) T unaligned_type;
  *value = *reinterpret_cast<const unaligned_type*>(*xposed);
  *xposed += sizeof(T);
  return true;
}

bool OatXposedFile::Setup(std::string* error_msg) {
  if (!GetOatXposedHeader().IsValid()) {
    *error_msg = StringPrintf("Invalid or outdated oat xposed header for '%s'", GetLocation().c_str());
    return false;
  }
  const uint8_t* xposed = Begin();
  xposed += sizeof(OatXposedHeader);
  if (xposed > End()) {
    *error_msg = StringPrintf("In oat xposed file '%s' found truncated OatXposedHeader", GetLocation().c_str());
    return false;
  }

  uint32_t dex_file_count = GetOatXposedHeader().GetDexFileCount();
  oat_xposed_dex_files_storage_.reserve(dex_file_count);
  for (size_t i = 0; i < dex_file_count; i++) {
    uint32_t num_methods;
    if (UNLIKELY(!ReadOatXposedDexFileData(*this, &xposed, &num_methods))) {
      *error_msg = StringPrintf("In oat file '%s' found OatXposedDexFile #%zu truncated after "
                                    "num methods",
                                GetLocation().c_str(),
                                i);
      return false;
    }

    uint32_t called_methods_num_offset;
    if (UNLIKELY(!ReadOatXposedDexFileData(*this, &xposed, &called_methods_num_offset))) {
      *error_msg = StringPrintf("In oat file '%s' found OatXposedDexFile #%zu truncated after "
                                    "called methods num offset",
                                GetLocation().c_str(),
                                i);
      return false;
    }

    uint32_t called_methods_offset;
    if (UNLIKELY(!ReadOatXposedDexFileData(*this, &xposed, &called_methods_offset))) {
      *error_msg = StringPrintf("In oat file '%s' found OatXposedDexFile #%zu truncated after "
                                    "called methods offset",
                                GetLocation().c_str(),
                                i);
      return false;
    }

    uint32_t called_methods_foreign_hashes_num;
    if (UNLIKELY(!ReadOatXposedDexFileData(*this, &xposed, &called_methods_foreign_hashes_num))) {
      *error_msg = StringPrintf("In oat file '%s' found OatXposedDexFile #%zu truncated after "
                                    "called methods foreign hashes num",
                                GetLocation().c_str(),
                                i);
      return false;
    }

    uint32_t called_methods_foreign_hashes_offset;
    if (UNLIKELY(!ReadOatXposedDexFileData(*this, &xposed, &called_methods_foreign_hashes_offset))) {
      *error_msg = StringPrintf("In oat file '%s' found OatXposedDexFile #%zu truncated after "
                                    "called methods foreign hashes offset",
                                GetLocation().c_str(),
                                i);
      return false;
    }

    // Create the OatXposedDexFile and add it to the owning container.
    OatXposedDexFile* oat_xposed_dex_file = new OatXposedDexFile(
        num_methods,
        reinterpret_cast<const uint16_t*>(Begin() + called_methods_num_offset),
        reinterpret_cast<const uint32_t*>(Begin() + called_methods_offset),
        ArraySlice<const uint32_t>(
            reinterpret_cast<const uint32_t*>(Begin() + called_methods_foreign_hashes_offset),
            called_methods_foreign_hashes_num));

    oat_xposed_dex_files_storage_.push_back(oat_xposed_dex_file);
  }

  return true;
}

bool OatXposedFile::Validate(const OatFile& oat_file, std::string* error_msg) const {
   const OatHeader& oat_header = oat_file.GetOatHeader();
   const OatXposedHeader& oat_xposed_header = GetOatXposedHeader();
   CHECK_EQ(oat_xposed_header.GetDexFileCount(), oat_header.GetDexFileCount()) << GetLocation();
   return true;
 }

//////////////////////
// OatXposedDexFile //
//////////////////////

OatXposedDexFile::OatXposedDexFile(uint32_t num_methods,
                                   const uint16_t* called_methods_num,
                                   const uint32_t* called_methods,
                                   ArraySlice<const uint32_t> foreign_hashes)
    : num_methods_(num_methods),
      called_methods_num_(called_methods_num),
      called_methods_(called_methods),
      foreign_hashes_(foreign_hashes) {
}

ArraySlice<const uint32_t> OatXposedDexFile::GetCalledMethods(uint32_t method_index) const {
  CHECK_LT(method_index, num_methods_);
  if (called_methods_num_[method_index] == 0) {
    return ArraySlice<const uint32_t>();
  }

  // Calculate the start index by summing up the number of called methods for all previous methods.
  const uint16_t* num_called_methods_pointer = called_methods_num_;
  size_t start_index = 0;
  for (size_t i = 0; i < method_index; ++i) {
    start_index += *num_called_methods_pointer;
    num_called_methods_pointer++;
  }

  return ArraySlice<const uint32_t>(called_methods_ + start_index, *num_called_methods_pointer);
}

std::vector<uint16_t> OatXposedDexFile::GetCallers(uint32_t hash) const {
  std::vector<uint16_t> callers;
  const uint32_t* call_methods_pointer = called_methods_;
  const uint16_t* num_called_methods_pointer = called_methods_num_;
  for (uint32_t method_index = 0; method_index < num_methods_; ++method_index) {
    size_t num = *num_called_methods_pointer;
    const uint32_t* call_methods_pointer_next = call_methods_pointer + num;
    if (num > 0 && std::binary_search(call_methods_pointer, call_methods_pointer_next, hash)) {
      callers.push_back(method_index);
    }
    call_methods_pointer = call_methods_pointer_next;
    ++num_called_methods_pointer;
  }
  return callers;
}

}  // namespace art
