#ifndef ART_ART_H_
#define ART_ART_H_

namespace android {
namespace runtime {

class Art {
 public:
  static bool Startup();
  static void Shutdown();
};

} }  // namespace android::runtime

namespace r = android::runtime;

#endif  // ART_ART_H_
