/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_COMPILER_ELF_WRITER_H_
#define ART_COMPILER_ELF_WRITER_H_

#include <stdint.h>
#include <cstddef>
#include <string>
#include <vector>

#include "base/macros.h"
#include "base/mutex.h"
#include "os.h"
#include "utils/array_ref.h"

namespace art {

class ElfFile;
class OutputStream;

namespace debug {
struct MethodDebugInfo;
}  // namespace debug

class ElfWriter {
 public:
  // Looks up information about location of oat file in elf file container.
  // Used for ImageWriter to perform memory layout.
  static void GetOatElfInformation(File* file,
                                   size_t* oat_loaded_size,
                                   size_t* oat_data_offset);

  // Returns runtime oat_data runtime address for an opened ElfFile.
  static uintptr_t GetOatDataAddress(ElfFile* elf_file);

  static bool Fixup(File* file, uintptr_t oat_data_begin);

  virtual ~ElfWriter() {}

  virtual void Start() = 0;
  virtual void SetLoadedSectionSizes(size_t rodata_size, size_t text_size, size_t bss_size) = 0;
  virtual void PrepareDebugInfo(const ArrayRef<const debug::MethodDebugInfo>& method_infos) = 0;
  virtual OutputStream* StartRoData() = 0;
  virtual void EndRoData(OutputStream* rodata) = 0;
  virtual OutputStream* StartText() = 0;
  virtual void EndText(OutputStream* text) = 0;
  virtual void WriteDynamicSection() = 0;
  virtual void WriteDebugInfo(const ArrayRef<const debug::MethodDebugInfo>& method_infos) = 0;
  virtual void WritePatchLocations(const ArrayRef<const uintptr_t>& patch_locations) = 0;
  virtual bool End() = 0;

  // Get the ELF writer's stream. This stream can be used for writing data directly
  // to a section after the section has been finished. When that's done, the user
  // should Seek() back to the position where the stream was before this operation.
  virtual OutputStream* GetStream() = 0;

  // Get the size that the loaded ELF file will occupy in memory.
  virtual size_t GetLoadedSize() = 0;

 protected:
  ElfWriter() = default;
};

}  // namespace art

#endif  // ART_COMPILER_ELF_WRITER_H_
