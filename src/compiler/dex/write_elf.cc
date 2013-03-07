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

#include "elf_writer.h"
#include "os.h"

namespace art {
class CompilerDriver;
class DexFile;
}  // namespace art

extern "C" bool WriteElf(art::CompilerDriver& driver,
                         const std::string& android_root,
                         bool is_host,
                         const std::vector<const art::DexFile*>& dex_files,
                         std::vector<uint8_t>& oat_contents,
                         art::File* file)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return art::ElfWriter::Create(file, oat_contents, dex_files, android_root, is_host, driver);
}
extern "C" bool FixupElf(art::File* file, uintptr_t oat_data_begin) {
  return art::ElfWriter::Fixup(file, oat_data_begin);
}
extern "C" void GetOatElfInformation(art::File* file,
                                     size_t& oat_loaded_size,
                                     size_t& oat_data_offset) {
  art::ElfWriter::GetOatElfInformation(file, oat_loaded_size, oat_data_offset);
}
extern "C" bool StripElf(art::File* file) {
  return art::ElfWriter::Strip(file);
}
