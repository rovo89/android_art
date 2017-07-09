#include "oat_xposed_writer.h"

#include "base/allocator.h"
#include "base/timing_logger.h"
#include "compiled_method.h"
#include "dex_file.h"
#include "driver/compiler_driver.h"
#include "linker/output_stream.h"
#include "oat_xposed.h"
#include "thread-inl.h"

namespace art {

OatXposedWriter::OatXposedWriter(const CompilerDriver* compiler,
                                 const std::vector<const DexFile*>& dex_files,
                                 uint32_t oat_file_checksum,
                                 TimingLogger* timings)
  : compiler_driver_(compiler),
    dex_files_(dex_files),
    oat_file_checksum_(oat_file_checksum),
    timings_(timings),
    total_calls_(0) {
  xposed_.reserve(dex_files_.size());
  foreign_hashes_.reserve(dex_files_.size());
}

static bool EnsureAligned(OutputStream* out, size_t* offset, size_t alignment) {
  static const uint8_t kPadding[] = {
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
  };
  if (*offset % alignment != 0) {
    size_t delta = alignment - *offset % alignment;
    DCHECK_LE(delta, sizeof(kPadding));
    if (UNLIKELY(!out->WriteFully(kPadding, delta))) {
      PLOG(ERROR) << "Failed to write padding in " << out->GetLocation();
      return false;
    }
    *offset += delta;
  }
  return true;
}

void OatXposedWriter::Prepare() {
  TimingLogger::ScopedTiming split("Prepare Xposed data", timings_);

  MutexLock mu(Thread::Current(), compiler_driver_->compiled_methods_lock_);
  CompilerDriver::MethodTable compiled_methods = compiler_driver_->GetCompiledMethods();

  for (const DexFile* dex_file : dex_files_) {
    // Calculate the hashes for all methods in this DexFile.
    const size_t num_methods = dex_file->NumMethodIds();
    std::vector<uint32_t> hashes(num_methods);
    for (size_t i = 0; i < num_methods; ++i) {
      hashes.push_back(dex_file->GetMethodHash(i));
    }
    std::sort(hashes.begin(), hashes.end());

    // Now check this against the called method hashes.
    std::vector<uint32_t> foreign_hashes;
    for (auto& pair : compiled_methods) {
      if (pair.first.dex_file == dex_file) {
        const auto called_methods = pair.second->GetCalledMethods();
        total_calls_ += called_methods.size();
        for (uint32_t hash : called_methods) {
          if (!std::binary_search(hashes.begin(), hashes.end(), hash)) {
            foreign_hashes.push_back(hash);
          }
        }
      }
    }
    STLSortAndRemoveDuplicates<std::vector<uint32_t>>(&foreign_hashes);
    foreign_hashes_.emplace_back(foreign_hashes);
  }
}

size_t OatXposedWriter::GetSize() {
  size_t required_size = sizeof(OatXposedHeader) + dex_files_.size() * sizeof(OatXposedDexFile);
  required_size += total_calls_ * sizeof(uint32_t);
  for (size_t i = 0; i < dex_files_.size(); ++i) {
    required_size += RoundUp(dex_files_[i]->NumMethodIds() * sizeof(uint16_t), sizeof(uint32_t));
    required_size += foreign_hashes_[i].size() * sizeof(uint32_t);
  }
  return required_size;
}

size_t OatXposedWriter::Write(OutputStream* out) {
  TimingLogger::ScopedTiming split("Write Xposed data", timings_);

  MutexLock mu(Thread::Current(), compiler_driver_->compiled_methods_lock_);
  CompilerDriver::MethodTable compiled_methods = compiler_driver_->GetCompiledMethods();

  off_t start_offset = out->Seek(0, kSeekCurrent);
  if (start_offset == static_cast<off_t>(-1)) {
    PLOG(ERROR) << "Failed to get current offset from " << out->GetLocation();
    return false;
  }

  size_t relative_offset = sizeof(OatXposedHeader) + dex_files_.size() * sizeof(OatXposedDexFile);
  if (out->Seek(start_offset + relative_offset, kSeekSet) == static_cast<off_t>(-1)) {
    PLOG(ERROR) << "Failed to seek to oat xposed data position in " << out->GetLocation();
    return false;
  }

  OatXposedDexFile dex_file_headers[dex_files_.size()];
  size_t dex_num = 0;
  for (const DexFile* dex_file : dex_files_) {
    const size_t num_methods = dex_file->NumMethodIds();
    dex_file_headers[dex_num].num_methods = num_methods;

    // Write called methods hashes.
    if (!EnsureAligned(out, &relative_offset, sizeof(uint32_t))) {
      return false;
    }
    dex_file_headers[dex_num].called_methods_offset = relative_offset;
    std::vector<uint16_t> num_called_methods(num_methods);
    for (auto& pair : compiled_methods) {
      if (pair.first.dex_file == dex_file) {
        const auto called_methods = pair.second->GetCalledMethods();

        num_called_methods[pair.first.dex_method_index] = called_methods.size();

        size_t bytes = called_methods.size() * sizeof(uint32_t);
        out->WriteFully(called_methods.data(), bytes);
        relative_offset += bytes;
      }
    }

    // Write array with number of called methods.
    dex_file_headers[dex_num].called_methods_num_offset = relative_offset;
    out->WriteFully(num_called_methods.data(), num_methods * sizeof(uint16_t));
    relative_offset += num_methods * sizeof(uint16_t);

    // Write foreign hashes.
    if (!EnsureAligned(out, &relative_offset, sizeof(uint32_t))) {
      return false;
    }
    dex_file_headers[dex_num].called_methods_foreign_hashes_num = foreign_hashes_[dex_num].size();
    dex_file_headers[dex_num].called_methods_foreign_hashes_offset = relative_offset;
    out->WriteFully(foreign_hashes_[dex_num].data(), foreign_hashes_[dex_num].size() * sizeof(uint32_t));
    relative_offset += foreign_hashes_[dex_num].size() * sizeof(uint32_t);

    ++dex_num;
  }

  if (out->Seek(start_offset, kSeekSet) == static_cast<off_t>(-1)) {
    PLOG(ERROR) << "Failed to seek to oat xposed header position in " << out->GetLocation();
    return false;
  }

  std::unique_ptr<OatXposedHeader> header(new OatXposedHeader(oat_file_checksum_, dex_files_.size()));
  out->WriteFully(header.get(), sizeof(OatXposedHeader));
  out->WriteFully(dex_file_headers, dex_files_.size() * sizeof(OatXposedDexFile));

  if (out->Seek(start_offset + relative_offset, kSeekSet) == static_cast<off_t>(-1)) {
    PLOG(ERROR) << "Failed to seek back after writing oat xposed header to " << out->GetLocation();
    return false;
  }

  return true;
}

OatXposedWriter::~OatXposedWriter() {
}

}  // namespace art
