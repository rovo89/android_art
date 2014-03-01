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

#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "dex_file-inl.h"
#include "dex_method_iterator.h"
#include "driver/compiler_driver.h"
#include "elf_file.h"
#include "invoke_type.h"
#include "mirror/art_method-inl.h"
#include "mirror/object-inl.h"
#include "oat.h"
#include "scoped_thread_state_change.h"

namespace art {

uint32_t ElfWriter::GetOatDataAddress(ElfFile* elf_file) {
  Elf32_Addr oatdata_address = elf_file->FindSymbolAddress(SHT_DYNSYM,
                                                           "oatdata",
                                                           false);
  CHECK_NE(0U, oatdata_address);
  return oatdata_address;
}

void ElfWriter::GetOatElfInformation(File* file,
                                     size_t& oat_loaded_size,
                                     size_t& oat_data_offset) {
  std::string error_msg;
  UniquePtr<ElfFile> elf_file(ElfFile::Open(file, false, false, &error_msg));
  CHECK(elf_file.get() != NULL) << error_msg;

  oat_loaded_size = elf_file->GetLoadedSize();
  CHECK_NE(0U, oat_loaded_size);
  oat_data_offset = GetOatDataAddress(elf_file.get());
  CHECK_NE(0U, oat_data_offset);
}

}  // namespace art
