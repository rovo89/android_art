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

#include "elf_patcher.h"

#include <vector>
#include <set>

#include "elf_file.h"
#include "elf_utils.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-inl.h"
#include "oat.h"
#include "os.h"
#include "utils.h"

namespace art {

bool ElfPatcher::Patch(const CompilerDriver* driver, ElfFile* elf_file,
                       const std::string& oat_location,
                       ImageAddressCallback cb, void* cb_data,
                       std::string* error_msg) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const OatFile* oat_file = class_linker->FindOpenedOatFileFromOatLocation(oat_location);
  if (oat_file == nullptr) {
    CHECK(Runtime::Current()->IsCompiler());
    oat_file = OatFile::Open(oat_location, oat_location, NULL, false, error_msg);
    if (oat_file == nullptr) {
      *error_msg = StringPrintf("Unable to find or open oat file at '%s': %s", oat_location.c_str(),
                                error_msg->c_str());
      return false;
    }
    CHECK_EQ(class_linker->RegisterOatFile(oat_file), oat_file);
  }
  return ElfPatcher::Patch(driver, elf_file, oat_file,
                           reinterpret_cast<uintptr_t>(oat_file->Begin()), cb, cb_data, error_msg);
}

bool ElfPatcher::Patch(const CompilerDriver* driver, ElfFile* elf, const OatFile* oat_file,
                       uintptr_t oat_data_start, ImageAddressCallback cb, void* cb_data,
                       std::string* error_msg) {
  Elf32_Shdr* data_sec = elf->FindSectionByName(".rodata");
  if (data_sec == nullptr) {
    *error_msg = "Unable to find .rodata section and oat header";
    return false;
  }
  OatHeader* oat_header = reinterpret_cast<OatHeader*>(elf->Begin() + data_sec->sh_offset);
  if (!oat_header->IsValid()) {
    *error_msg = "Oat header was not valid";
    return false;
  }

  ElfPatcher p(driver, elf, oat_file, oat_header, oat_data_start, cb, cb_data, error_msg);
  return p.PatchElf();
}

mirror::ArtMethod* ElfPatcher::GetTargetMethod(const CompilerDriver::CallPatchInformation* patch) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  StackHandleScope<1> hs(Thread::Current());
  Handle<mirror::DexCache> dex_cache(
      hs.NewHandle(class_linker->FindDexCache(*patch->GetTargetDexFile())));
  mirror::ArtMethod* method = class_linker->ResolveMethod(*patch->GetTargetDexFile(),
                                                          patch->GetTargetMethodIdx(),
                                                          dex_cache,
                                                          NullHandle<mirror::ClassLoader>(),
                                                          NullHandle<mirror::ArtMethod>(),
                                                          patch->GetTargetInvokeType());
  CHECK(method != NULL)
    << patch->GetTargetDexFile()->GetLocation() << " " << patch->GetTargetMethodIdx();
  CHECK(!method->IsRuntimeMethod())
    << patch->GetTargetDexFile()->GetLocation() << " " << patch->GetTargetMethodIdx();
  CHECK(dex_cache->GetResolvedMethods()->Get(patch->GetTargetMethodIdx()) == method)
    << patch->GetTargetDexFile()->GetLocation() << " " << patch->GetReferrerMethodIdx() << " "
    << PrettyMethod(dex_cache->GetResolvedMethods()->Get(patch->GetTargetMethodIdx())) << " "
    << PrettyMethod(method);
  return method;
}

mirror::Class* ElfPatcher::GetTargetType(const CompilerDriver::TypePatchInformation* patch) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  StackHandleScope<2> hs(Thread::Current());
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(class_linker->FindDexCache(patch->GetDexFile())));
  mirror::Class* klass = class_linker->ResolveType(patch->GetDexFile(), patch->GetTargetTypeIdx(),
                                                   dex_cache, NullHandle<mirror::ClassLoader>());
  CHECK(klass != NULL)
    << patch->GetDexFile().GetLocation() << " " << patch->GetTargetTypeIdx();
  CHECK(dex_cache->GetResolvedTypes()->Get(patch->GetTargetTypeIdx()) == klass)
    << patch->GetDexFile().GetLocation() << " " << patch->GetReferrerMethodIdx() << " "
    << PrettyClass(dex_cache->GetResolvedTypes()->Get(patch->GetTargetTypeIdx())) << " "
    << PrettyClass(klass);
  return klass;
}

void ElfPatcher::AddPatch(uintptr_t p) {
  if (write_patches_ && patches_set_.find(p) == patches_set_.end()) {
    patches_set_.insert(p);
    patches_.push_back(p);
  }
}

uint32_t* ElfPatcher::GetPatchLocation(uintptr_t patch_ptr) {
  CHECK_GE(patch_ptr, reinterpret_cast<uintptr_t>(oat_file_->Begin()));
  CHECK_LE(patch_ptr, reinterpret_cast<uintptr_t>(oat_file_->End()));
  uintptr_t off = patch_ptr - reinterpret_cast<uintptr_t>(oat_file_->Begin());
  uintptr_t ret = reinterpret_cast<uintptr_t>(oat_header_) + off;

  CHECK_GE(ret, reinterpret_cast<uintptr_t>(elf_file_->Begin()));
  CHECK_LT(ret, reinterpret_cast<uintptr_t>(elf_file_->End()));
  return reinterpret_cast<uint32_t*>(ret);
}

void ElfPatcher::SetPatchLocation(const CompilerDriver::PatchInformation* patch, uint32_t value) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const void* quick_oat_code = class_linker->GetQuickOatCodeFor(patch->GetDexFile(),
                                                                patch->GetReferrerClassDefIdx(),
                                                                patch->GetReferrerMethodIdx());
  // TODO: make this Thumb2 specific
  uint8_t* base = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(quick_oat_code) & ~0x1);
  uintptr_t patch_ptr = reinterpret_cast<uintptr_t>(base + patch->GetLiteralOffset());
  uint32_t* patch_location = GetPatchLocation(patch_ptr);
  if (kIsDebugBuild) {
    if (patch->IsCall()) {
      const CompilerDriver::CallPatchInformation* cpatch = patch->AsCall();
      const DexFile::MethodId& id =
          cpatch->GetTargetDexFile()->GetMethodId(cpatch->GetTargetMethodIdx());
      uint32_t expected = reinterpret_cast<uintptr_t>(&id) & 0xFFFFFFFF;
      uint32_t actual = *patch_location;
      CHECK(actual == expected || actual == value) << "Patching call failed: " << std::hex
          << " actual=" << actual
          << " expected=" << expected
          << " value=" << value;
    }
    if (patch->IsType()) {
      const CompilerDriver::TypePatchInformation* tpatch = patch->AsType();
      const DexFile::TypeId& id = tpatch->GetDexFile().GetTypeId(tpatch->GetTargetTypeIdx());
      uint32_t expected = reinterpret_cast<uintptr_t>(&id) & 0xFFFFFFFF;
      uint32_t actual = *patch_location;
      CHECK(actual == expected || actual == value) << "Patching type failed: " << std::hex
          << " actual=" << actual
          << " expected=" << expected
          << " value=" << value;
    }
  }
  *patch_location = value;
  oat_header_->UpdateChecksum(patch_location, sizeof(value));

  if (patch->IsCall() && patch->AsCall()->IsRelative()) {
    // We never record relative patches.
    return;
  }
  uintptr_t loc = patch_ptr - (reinterpret_cast<uintptr_t>(oat_file_->Begin()) +
                               oat_header_->GetExecutableOffset());
  CHECK_GT(patch_ptr, reinterpret_cast<uintptr_t>(oat_file_->Begin()) +
                      oat_header_->GetExecutableOffset());
  CHECK_LT(loc, oat_file_->Size() - oat_header_->GetExecutableOffset());
  AddPatch(loc);
}

bool ElfPatcher::PatchElf() {
  // TODO if we are adding patches the resulting ELF file might have a
  // potentially rather large amount of free space where patches might have been
  // placed. We should adjust the ELF file to get rid of this excess space.
  if (write_patches_) {
    patches_.reserve(compiler_driver_->GetCodeToPatch().size() +
                     compiler_driver_->GetMethodsToPatch().size() +
                     compiler_driver_->GetClassesToPatch().size());
  }
  Thread* self = Thread::Current();
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const char* old_cause = self->StartAssertNoThreadSuspension("ElfPatcher");

  typedef std::vector<const CompilerDriver::CallPatchInformation*> CallPatches;
  const CallPatches& code_to_patch = compiler_driver_->GetCodeToPatch();
  for (size_t i = 0; i < code_to_patch.size(); i++) {
    const CompilerDriver::CallPatchInformation* patch = code_to_patch[i];

    mirror::ArtMethod* target = GetTargetMethod(patch);
    uintptr_t quick_code = reinterpret_cast<uintptr_t>(class_linker->GetQuickOatCodeFor(target));
    DCHECK_NE(quick_code, 0U) << PrettyMethod(target);
    const OatFile* target_oat =
        class_linker->FindOpenedOatDexFileForDexFile(*patch->GetTargetDexFile())->GetOatFile();
    // Get where the data actually starts. if target is this oat_file_ it is oat_data_start_,
    // otherwise it is wherever target_oat is loaded.
    uintptr_t oat_data_addr = GetBaseAddressFor(target_oat);
    uintptr_t code_base = reinterpret_cast<uintptr_t>(target_oat->Begin());
    uintptr_t code_offset = quick_code - code_base;
    bool is_quick_offset = false;
    if (quick_code == reinterpret_cast<uintptr_t>(GetQuickToInterpreterBridge())) {
      is_quick_offset = true;
      code_offset = oat_header_->GetQuickToInterpreterBridgeOffset();
    } else if (quick_code ==
        reinterpret_cast<uintptr_t>(class_linker->GetQuickGenericJniTrampoline())) {
      CHECK(target->IsNative());
      is_quick_offset = true;
      code_offset = oat_header_->GetQuickGenericJniTrampolineOffset();
    }
    uintptr_t value;
    if (patch->IsRelative()) {
      // value to patch is relative to the location being patched
      const void* quick_oat_code =
        class_linker->GetQuickOatCodeFor(patch->GetDexFile(),
                                         patch->GetReferrerClassDefIdx(),
                                         patch->GetReferrerMethodIdx());
      if (is_quick_offset) {
        // If its a quick offset it means that we are doing a relative patch from the class linker
        // oat_file to the elf_patcher oat_file so we need to adjust the quick oat code to be the
        // one in the output oat_file (ie where it is actually going to be loaded).
        quick_code = PointerToLowMemUInt32(reinterpret_cast<void*>(oat_data_addr + code_offset));
        quick_oat_code =
            reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(quick_oat_code) +
                oat_data_addr - code_base);
      }
      uintptr_t base = reinterpret_cast<uintptr_t>(quick_oat_code);
      uintptr_t patch_location = base + patch->GetLiteralOffset();
      value = quick_code - patch_location + patch->RelativeOffset();
    } else if (code_offset != 0) {
      value = PointerToLowMemUInt32(reinterpret_cast<void*>(oat_data_addr + code_offset));
    } else {
      value = 0;
    }
    SetPatchLocation(patch, value);
  }

  const CallPatches& methods_to_patch = compiler_driver_->GetMethodsToPatch();
  for (size_t i = 0; i < methods_to_patch.size(); i++) {
    const CompilerDriver::CallPatchInformation* patch = methods_to_patch[i];
    mirror::ArtMethod* target = GetTargetMethod(patch);
    SetPatchLocation(patch, PointerToLowMemUInt32(get_image_address_(cb_data_, target)));
  }

  const std::vector<const CompilerDriver::TypePatchInformation*>& classes_to_patch =
      compiler_driver_->GetClassesToPatch();
  for (size_t i = 0; i < classes_to_patch.size(); i++) {
    const CompilerDriver::TypePatchInformation* patch = classes_to_patch[i];
    mirror::Class* target = GetTargetType(patch);
    SetPatchLocation(patch, PointerToLowMemUInt32(get_image_address_(cb_data_, target)));
  }

  self->EndAssertNoThreadSuspension(old_cause);

  if (write_patches_) {
    return WriteOutPatchData();
  }
  return true;
}

bool ElfPatcher::WriteOutPatchData() {
  Elf32_Shdr* shdr = elf_file_->FindSectionByName(".oat_patches");
  if (shdr != nullptr) {
    CHECK_EQ(shdr, elf_file_->FindSectionByType(SHT_OAT_PATCH))
        << "Incorrect type for .oat_patches section";
    CHECK_LE(patches_.size() * sizeof(uintptr_t), shdr->sh_size)
        << "We got more patches than anticipated";
    CHECK_LE(reinterpret_cast<uintptr_t>(elf_file_->Begin()) + shdr->sh_offset + shdr->sh_size,
              reinterpret_cast<uintptr_t>(elf_file_->End())) << "section is too large";
    CHECK(shdr == elf_file_->GetSectionHeader(elf_file_->GetSectionHeaderNum() - 1) ||
          shdr->sh_offset + shdr->sh_size <= (shdr + 1)->sh_offset)
        << "Section overlaps onto next section";
    // It's mmap'd so we can just memcpy.
    memcpy(elf_file_->Begin() + shdr->sh_offset, patches_.data(),
           patches_.size() * sizeof(uintptr_t));
    // TODO We should fill in the newly empty space between the last patch and
    // the start of the next section by moving the following sections down if
    // possible.
    shdr->sh_size = patches_.size() * sizeof(uintptr_t);
    return true;
  } else {
    LOG(ERROR) << "Unable to find section header for SHT_OAT_PATCH";
    *error_msg_ = "Unable to find section to write patch information to in ";
    *error_msg_ += elf_file_->GetFile().GetPath();
    return false;
  }
}

}  // namespace art
