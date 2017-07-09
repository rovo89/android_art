#ifndef ART_COMPILER_OAT_XPOSED_WRITER_H_
#define ART_COMPILER_OAT_XPOSED_WRITER_H_

#include <stdint.h>
#include <vector>

#include "base/macros.h"

namespace art {

class CompilerDriver;
class DexFile;
class OutputStream;
class TimingLogger;

class OatXposedWriter {
 public:
  OatXposedWriter(const CompilerDriver* compiler,
                  const std::vector<const DexFile*>& dex_files,
                  uint32_t oat_file_checksum,
                  TimingLogger* timings);

  ~OatXposedWriter();

  void Prepare();
  size_t GetSize();
  size_t Write(OutputStream* out);

 private:
  struct OatXposedDexFile {
    uint32_t num_methods;
    uint32_t called_methods_num_offset;
    uint32_t called_methods_offset;
    uint32_t called_methods_foreign_hashes_num;
    uint32_t called_methods_foreign_hashes_offset;
  };

  const CompilerDriver* compiler_driver_;
  const std::vector<const DexFile*>& dex_files_;
  const uint32_t oat_file_checksum_;
  TimingLogger* timings_;

  std::vector<OatXposedDexFile> xposed_;
  std::vector<std::vector<uint32_t>> foreign_hashes_;
  size_t total_calls_;

  DISALLOW_COPY_AND_ASSIGN(OatXposedWriter);
};

}  // namespace art

#endif  // ART_COMPILER_OAT_XPOSED_WRITER_H_
