#include "lgalmond.h"

#include <dlfcn.h>
#include <endian.h>
#include <sys/mman.h>

#include "base/logging.h"
#include "base/stringprintf.h"
#include "base/unix_file/fd_file.h"
#include "oat.h"

namespace art {

const uint8_t LGAlmond::kOatMagic[] = { 'a', 'l', 'm', 'd' };
const uint8_t LGAlmond::kOarmBrand[] = { 0, 0, 0, 20, 'o', 'a', 'r', 'm', 'o', 'a', 't', '\n', 0, 0, 0, 1, 'o', 'a', 't', '\n' };

#if defined(__LP64__)
  static const char* kLibLgAlmondPath = "/system/lib64/liblgalmond.so";
#else
  static const char* kLibLgAlmondPath = "/system/lib/liblgalmond.so";
#endif

LGAlmond::IsDRMDexFn LGAlmond::IsDRMDex_ = nullptr;
LGAlmond::CopyDexToMemFn LGAlmond::CopyDexToMem_ = nullptr;
LGAlmond::DecOatFn LGAlmond::DecOat_ = nullptr;

void LGAlmond::Init() {
  if (!OS::FileExists(kLibLgAlmondPath)) {
    return;
  }

  void* dlopen_handle = dlopen(kLibLgAlmondPath, RTLD_NOW);
  if (dlopen_handle == nullptr) {
    LOG(ERROR) << "Could not load liblgalmond.so: " << dlerror();
    return;
  }

  CopyDexToMem_ = reinterpret_cast<CopyDexToMemFn>(dlsym(dlopen_handle, "Almond_CopyDexToMem"));
  if (CopyDexToMem_ == nullptr)  {
    LOG(ERROR) << "Could not locate Almond_CopyDexToMem: " << dlerror();
    return;
  }

  IsDRMDex_ = reinterpret_cast<IsDRMDexFn>(dlsym(dlopen_handle, "Almond_Is_DRMDex"));
  if (IsDRMDex_ == nullptr)  {
    LOG(ERROR) << "Could not locate Almond_Is_DRMDex: " << dlerror();
    return;
  }

  DecOat_ = reinterpret_cast<DecOatFn>(dlsym(dlopen_handle, "Almond_DecOat"));
  if (DecOat_ == nullptr)  {
    LOG(ERROR) << "Could not locate Almond_DecOat: " << dlerror();
    return;
  }
}

bool LGAlmond::IsEncryptedDex(const void* data, size_t size) {
  return IsDRMDex_ != nullptr && IsDRMDex_(data, size) == kFormatDex;
}

bool LGAlmond::DecryptDex(void* data, size_t* size) {
  uint8_t cid_hash[20];
  uint8_t preload_id[20];
  return CopyDexToMem_(data, *size, size, cid_hash, preload_id) == 0;
}

bool LGAlmond::IsEncryptedOat(const void* data) {
  return memcmp(data, LGAlmond::kOatMagic, 4) == 0;
}

bool LGAlmond::DecryptOat(void* data, const File& file, std::string* error_msg) {
  if (DecOat_ == nullptr) {
    *error_msg = "LG Almond library was not loaded correctly";
    return false;
  }

  struct {
    uint8_t  brand[20];
    uint32_t ignored1;
    uint8_t  bind_id[20];
    uint8_t  hashed_cid[20];
    uint32_t protected_offset;
    uint32_t protected_length;
    uint32_t almond_offset;
    uint32_t almond_length;
    uint32_t rodata_offset;
    uint32_t rodata_length;
  } almond;

  if (file.Read(reinterpret_cast<char*>(&almond), sizeof(almond), file.GetLength() - 0x80) != sizeof(almond)) {
    *error_msg = "Could not read LG Almond structure";
    return false;
  }

  if (memcmp(almond.brand, kOarmBrand, sizeof(kOarmBrand)) != 0) {
    *error_msg = "Invalid LG Almond branding";
    return false;
  }

  if (mprotect(data, be32toh(almond.rodata_length), PROT_READ | PROT_WRITE) != 0) {
    *error_msg = StringPrintf("Could not make memory writable: %s", strerror(errno));
    return false;
  }

  uint8_t* protected_data = reinterpret_cast<uint8_t*>(data);
  protected_data += be32toh(almond.protected_offset) - be32toh(almond.rodata_offset);

  if (DecOat_(protected_data, be32toh(almond.protected_length), almond.bind_id, almond.hashed_cid) == 0) {
    memcpy(data, OatHeader::kOatMagic, sizeof(OatHeader::kOatMagic));
    mprotect(data, be32toh(almond.rodata_length), PROT_READ);
    return true;
  } else {
    *error_msg = "LG Almond decryption failed";
    return false;
  }
}

}  // namespace art
