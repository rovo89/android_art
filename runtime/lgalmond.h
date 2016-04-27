#ifndef ART_RUNTIME_LGALMOND_H_
#define ART_RUNTIME_LGALMOND_H_

#include <string>

#include "globals.h"
#include "os.h"

namespace art {

class LGAlmond {
 public:
  static const uint8_t kOatMagic[4];
  static const uint8_t kOarmBrand[20];

  // Initializes the LG Almond encryption library, if available.
  static void Init();

  static bool IsEncryptedDex(const void* data, size_t size);
  static bool DecryptDex(void* data, size_t* size);
  static bool IsEncryptedOat(const void* data);
  static bool DecryptOat(void* data, const File& file, std::string* error_msg);

 private:
  static const uint32_t kFormatDex = 1;

  typedef int (*IsDRMDexFn)(const void*, size_t);
  typedef int (*CopyDexToMemFn)(void*, size_t, size_t*, uint8_t*, uint8_t*);
  typedef int (*DecOatFn)(void*, size_t, uint8_t*, uint8_t*);

  static IsDRMDexFn IsDRMDex_;
  static CopyDexToMemFn CopyDexToMem_;
  static DecOatFn DecOat_;

};

}  // namespace art

#endif  // ART_RUNTIME_LGALMOND_H_
