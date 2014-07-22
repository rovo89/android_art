/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_COMPILER_ELF_PATCHER_H_
#define ART_COMPILER_ELF_PATCHER_H_

#include "base/mutex.h"
#include "driver/compiler_driver.h"
#include "elf_file.h"
#include "mirror/art_method.h"
#include "mirror/class.h"
#include "mirror/object.h"
#include "oat_file.h"
#include "oat.h"
#include "os.h"

namespace art {

class ElfPatcher {
 public:
  typedef void* (*ImageAddressCallback)(void* data, mirror::Object* obj);

  static bool Patch(const CompilerDriver* driver, ElfFile* elf_file,
                    const std::string& oat_location,
                    ImageAddressCallback cb, void* cb_data,
                    std::string* error_msg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static bool Patch(const CompilerDriver* driver, ElfFile* elf_file,
                    const OatFile* oat_file, uintptr_t oat_data_begin,
                    ImageAddressCallback cb, void* cb_data,
                    std::string* error_msg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static bool Patch(const CompilerDriver* driver, ElfFile* elf_file,
                    const std::string& oat_location,
                    std::string* error_msg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return ElfPatcher::Patch(driver, elf_file, oat_location,
                             DefaultImageAddressCallback, nullptr, error_msg);
  }

  static bool Patch(const CompilerDriver* driver, ElfFile* elf_file,
                    const OatFile* oat_file, uintptr_t oat_data_begin,
                    std::string* error_msg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return ElfPatcher::Patch(driver, elf_file, oat_file, oat_data_begin,
                             DefaultImageAddressCallback, nullptr, error_msg);
  }

 private:
  ElfPatcher(const CompilerDriver* driver, ElfFile* elf_file, const OatFile* oat_file,
             OatHeader* oat_header, uintptr_t oat_data_begin,
             ImageAddressCallback cb, void* cb_data, std::string* error_msg)
      : compiler_driver_(driver), elf_file_(elf_file), oat_file_(oat_file),
        oat_header_(oat_header), oat_data_begin_(oat_data_begin), get_image_address_(cb),
        cb_data_(cb_data), error_msg_(error_msg),
        write_patches_(compiler_driver_->GetCompilerOptions().GetIncludePatchInformation()) {}
  ~ElfPatcher() {}

  static void* DefaultImageAddressCallback(void* data_unused, mirror::Object* obj) {
    return static_cast<void*>(obj);
  }

  bool PatchElf()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  mirror::ArtMethod* GetTargetMethod(const CompilerDriver::CallPatchInformation* patch)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  mirror::Class* GetTargetType(const CompilerDriver::TypePatchInformation* patch)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void AddPatch(uintptr_t off);

  void SetPatchLocation(const CompilerDriver::PatchInformation* patch, uint32_t value)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Takes the pointer into the oat_file_ and get the pointer in to the ElfFile.
  uint32_t* GetPatchLocation(uintptr_t patch_ptr);

  bool WriteOutPatchData();

  uintptr_t GetBaseAddressFor(const OatFile* f) {
    if (f == oat_file_) {
      return oat_data_begin_;
    } else {
      return reinterpret_cast<uintptr_t>(f->Begin());
    }
  }

  const CompilerDriver* compiler_driver_;

  // The elf_file containing the oat_data we are patching up
  ElfFile* elf_file_;

  // The oat_file that is actually loaded.
  const OatFile* oat_file_;

  // The oat_header_ within the elf_file_
  OatHeader* oat_header_;

  // Where the elf_file will be loaded during normal runs.
  uintptr_t oat_data_begin_;

  // Callback to get image addresses.
  ImageAddressCallback get_image_address_;
  void* cb_data_;

  std::string* error_msg_;
  std::vector<uintptr_t> patches_;
  std::set<uintptr_t> patches_set_;
  bool write_patches_;

  DISALLOW_COPY_AND_ASSIGN(ElfPatcher);
};

}  // namespace art
#endif  // ART_COMPILER_ELF_PATCHER_H_
