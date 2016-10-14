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
#include "patchoat.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/dumpable.h"
#include "base/scoped_flock.h"
#include "base/stringpiece.h"
#include "base/stringprintf.h"
#include "base/unix_file/fd_file.h"
#include "elf_utils.h"
#include "elf_file.h"
#include "elf_file_impl.h"
#include "gc/space/image_space.h"
#include "image-inl.h"
#include "mirror/abstract_method.h"
#include "mirror/object-inl.h"
#include "mirror/method.h"
#include "mirror/reference.h"
#include "noop_compiler_callbacks.h"
#include "offsets.h"
#include "os.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "utils.h"

namespace art {

static bool LocationToFilename(const std::string& location, InstructionSet isa,
                               std::string* filename) {
  bool has_system = false;
  bool has_cache = false;
  // image_location = /system/framework/boot.art
  // system_image_filename = /system/framework/<image_isa>/boot.art
  std::string system_filename(GetSystemImageFilename(location.c_str(), isa));
  if (OS::FileExists(system_filename.c_str())) {
    has_system = true;
  }

  bool have_android_data = false;
  bool dalvik_cache_exists = false;
  bool is_global_cache = false;
  std::string dalvik_cache;
  GetDalvikCache(GetInstructionSetString(isa), false, &dalvik_cache,
                 &have_android_data, &dalvik_cache_exists, &is_global_cache);

  std::string cache_filename;
  if (have_android_data && dalvik_cache_exists) {
    // Always set output location even if it does not exist,
    // so that the caller knows where to create the image.
    //
    // image_location = /system/framework/boot.art
    // *image_filename = /data/dalvik-cache/<image_isa>/boot.art
    std::string error_msg;
    if (GetDalvikCacheFilename(location.c_str(), dalvik_cache.c_str(),
                               &cache_filename, &error_msg)) {
      has_cache = true;
    }
  }
  if (has_system) {
    *filename = system_filename;
    return true;
  } else if (has_cache) {
    *filename = cache_filename;
    return true;
  } else {
    return false;
  }
}

static const OatHeader* GetOatHeader(const ElfFile* elf_file) {
  uint64_t off = 0;
  if (!elf_file->GetSectionOffsetAndSize(".rodata", &off, nullptr)) {
    return nullptr;
  }

  OatHeader* oat_header = reinterpret_cast<OatHeader*>(elf_file->Begin() + off);
  return oat_header;
}

// This function takes an elf file and reads the current patch delta value
// encoded in its oat header value
static bool ReadOatPatchDelta(const ElfFile* elf_file, off_t* delta, std::string* error_msg) {
  const OatHeader* oat_header = GetOatHeader(elf_file);
  if (oat_header == nullptr) {
    *error_msg = "Unable to get oat header from elf file.";
    return false;
  }
  if (!oat_header->IsValid()) {
    *error_msg = "Elf file has an invalid oat header";
    return false;
  }
  *delta = oat_header->GetImagePatchDelta();
  return true;
}

static File* CreateOrOpen(const char* name, bool* created) {
  if (OS::FileExists(name)) {
    *created = false;
    return OS::OpenFileReadWrite(name);
  } else {
    *created = true;
    std::unique_ptr<File> f(OS::CreateEmptyFile(name));
    if (f.get() != nullptr) {
      if (fchmod(f->Fd(), 0644) != 0) {
        PLOG(ERROR) << "Unable to make " << name << " world readable";
        unlink(name);
        return nullptr;
      }
    }
    return f.release();
  }
}

// Either try to close the file (close=true), or erase it.
static bool FinishFile(File* file, bool close) {
  if (close) {
    if (file->FlushCloseOrErase() != 0) {
      PLOG(ERROR) << "Failed to flush and close file.";
      return false;
    }
    return true;
  } else {
    file->Erase();
    return false;
  }
}

bool PatchOat::Patch(const std::string& image_location,
                     off_t delta,
                     const std::string& output_directory,
                     InstructionSet isa,
                     TimingLogger* timings) {
  CHECK(Runtime::Current() == nullptr);
  CHECK(!image_location.empty()) << "image file must have a filename.";

  TimingLogger::ScopedTiming t("Runtime Setup", timings);

  CHECK_NE(isa, kNone);
  const char* isa_name = GetInstructionSetString(isa);

  // Set up the runtime
  RuntimeOptions options;
  NoopCompilerCallbacks callbacks;
  options.push_back(std::make_pair("compilercallbacks", &callbacks));
  std::string img = "-Ximage:" + image_location;
  options.push_back(std::make_pair(img.c_str(), nullptr));
  options.push_back(std::make_pair("imageinstructionset", reinterpret_cast<const void*>(isa_name)));
  options.push_back(std::make_pair("-Xno-sig-chain", nullptr));
  if (!Runtime::Create(options, false)) {
    LOG(ERROR) << "Unable to initialize runtime";
    return false;
  }
  // Runtime::Create acquired the mutator_lock_ that is normally given away when we Runtime::Start,
  // give it away now and then switch to a more manageable ScopedObjectAccess.
  Thread::Current()->TransitionFromRunnableToSuspended(kNative);
  ScopedObjectAccess soa(Thread::Current());

  t.NewTiming("Image and oat Patching setup");
  std::vector<gc::space::ImageSpace*> spaces = Runtime::Current()->GetHeap()->GetBootImageSpaces();
  std::map<gc::space::ImageSpace*, std::unique_ptr<File>> space_to_file_map;
  std::map<gc::space::ImageSpace*, std::unique_ptr<MemMap>> space_to_memmap_map;
  std::map<gc::space::ImageSpace*, PatchOat> space_to_patchoat_map;
  std::map<gc::space::ImageSpace*, bool> space_to_skip_patching_map;

  for (size_t i = 0; i < spaces.size(); ++i) {
    gc::space::ImageSpace* space = spaces[i];
    std::string input_image_filename = space->GetImageFilename();
    std::unique_ptr<File> input_image(OS::OpenFileForReading(input_image_filename.c_str()));
    if (input_image.get() == nullptr) {
      LOG(ERROR) << "Unable to open input image file at " << input_image_filename;
      return false;
    }

    int64_t image_len = input_image->GetLength();
    if (image_len < 0) {
      LOG(ERROR) << "Error while getting image length";
      return false;
    }
    ImageHeader image_header;
    if (sizeof(image_header) != input_image->Read(reinterpret_cast<char*>(&image_header),
                                                  sizeof(image_header), 0)) {
      LOG(ERROR) << "Unable to read image header from image file " << input_image->GetPath();
    }

    /*bool is_image_pic = */IsImagePic(image_header, input_image->GetPath());
    // Nothing special to do right now since the image always needs to get patched.
    // Perhaps in some far-off future we may have images with relative addresses that are true-PIC.

    // Create the map where we will write the image patches to.
    std::string error_msg;
    std::unique_ptr<MemMap> image(MemMap::MapFile(image_len,
                                                  PROT_READ | PROT_WRITE,
                                                  MAP_PRIVATE,
                                                  input_image->Fd(),
                                                  0,
                                                  /*low_4gb*/false,
                                                  input_image->GetPath().c_str(),
                                                  &error_msg));
    if (image.get() == nullptr) {
      LOG(ERROR) << "Unable to map image file " << input_image->GetPath() << " : " << error_msg;
      return false;
    }
    space_to_file_map.emplace(space, std::move(input_image));
    space_to_memmap_map.emplace(space, std::move(image));
  }

  for (size_t i = 0; i < spaces.size(); ++i) {
    gc::space::ImageSpace* space = spaces[i];
    std::string input_image_filename = space->GetImageFilename();
    std::string input_oat_filename =
        ImageHeader::GetOatLocationFromImageLocation(input_image_filename);
    std::unique_ptr<File> input_oat_file(OS::OpenFileForReading(input_oat_filename.c_str()));
    if (input_oat_file.get() == nullptr) {
      LOG(ERROR) << "Unable to open input oat file at " << input_oat_filename;
      return false;
    }
    std::string error_msg;
    std::unique_ptr<ElfFile> elf(ElfFile::Open(input_oat_file.get(),
                                               PROT_READ | PROT_WRITE, MAP_PRIVATE, &error_msg));
    if (elf.get() == nullptr) {
      LOG(ERROR) << "Unable to open oat file " << input_oat_file->GetPath() << " : " << error_msg;
      return false;
    }

    bool skip_patching_oat = false;
    MaybePic is_oat_pic = IsOatPic(elf.get());
    if (is_oat_pic >= ERROR_FIRST) {
      // Error logged by IsOatPic
      return false;
    } else if (is_oat_pic == PIC) {
      // Do not need to do ELF-file patching. Create a symlink and skip the ELF patching.

      std::string converted_image_filename = space->GetImageLocation();
      std::replace(converted_image_filename.begin() + 1, converted_image_filename.end(), '/', '@');
      std::string output_image_filename = output_directory +
                                          (StartsWith(converted_image_filename, "/") ? "" : "/") +
                                          converted_image_filename;
      std::string output_oat_filename =
          ImageHeader::GetOatLocationFromImageLocation(output_image_filename);

      if (!ReplaceOatFileWithSymlink(input_oat_file->GetPath(),
                                     output_oat_filename,
                                     false,
                                     true)) {
        // Errors already logged by above call.
        return false;
      }
      // Don't patch the OAT, since we just symlinked it. Image still needs patching.
      skip_patching_oat = true;
    } else {
      CHECK(is_oat_pic == NOT_PIC);
    }

    PatchOat& p = space_to_patchoat_map.emplace(space,
                                                PatchOat(
                                                    isa,
                                                    elf.release(),
                                                    space_to_memmap_map.find(space)->second.get(),
                                                    space->GetLiveBitmap(),
                                                    space->GetMemMap(),
                                                    delta,
                                                    &space_to_memmap_map,
                                                    timings)).first->second;

    t.NewTiming("Patching files");
    if (!skip_patching_oat && !p.PatchElf()) {
      LOG(ERROR) << "Failed to patch oat file " << input_oat_file->GetPath();
      return false;
    }
    if (!p.PatchImage(i == 0)) {
      LOG(ERROR) << "Failed to patch image file " << input_image_filename;
      return false;
    }

    space_to_skip_patching_map.emplace(space, skip_patching_oat);
  }

  for (size_t i = 0; i < spaces.size(); ++i) {
    gc::space::ImageSpace* space = spaces[i];
    std::string input_image_filename = space->GetImageFilename();

    t.NewTiming("Writing files");
    std::string converted_image_filename = space->GetImageLocation();
    std::replace(converted_image_filename.begin() + 1, converted_image_filename.end(), '/', '@');
    std::string output_image_filename = output_directory +
                                        (StartsWith(converted_image_filename, "/") ? "" : "/") +
                                        converted_image_filename;
    bool new_oat_out;
    std::unique_ptr<File>
        output_image_file(CreateOrOpen(output_image_filename.c_str(), &new_oat_out));
    if (output_image_file.get() == nullptr) {
      LOG(ERROR) << "Failed to open output image file at " << output_image_filename;
      return false;
    }

    PatchOat& p = space_to_patchoat_map.find(space)->second;

    bool success = p.WriteImage(output_image_file.get());
    success = FinishFile(output_image_file.get(), success);
    if (!success) {
      return false;
    }

    bool skip_patching_oat = space_to_skip_patching_map.find(space)->second;
    if (!skip_patching_oat) {
      std::string output_oat_filename =
          ImageHeader::GetOatLocationFromImageLocation(output_image_filename);
      std::unique_ptr<File>
          output_oat_file(CreateOrOpen(output_oat_filename.c_str(), &new_oat_out));
      if (output_oat_file.get() == nullptr) {
        LOG(ERROR) << "Failed to open output oat file at " << output_oat_filename;
        return false;
      }
      success = p.WriteElf(output_oat_file.get());
      success = FinishFile(output_oat_file.get(), success);
      if (!success) {
        return false;
      }
    }
  }
  return true;
}

bool PatchOat::WriteElf(File* out) {
  TimingLogger::ScopedTiming t("Writing Elf File", timings_);

  CHECK(oat_file_.get() != nullptr);
  CHECK(out != nullptr);
  size_t expect = oat_file_->Size();
  if (out->WriteFully(reinterpret_cast<char*>(oat_file_->Begin()), expect) &&
      out->SetLength(expect) == 0) {
    return true;
  } else {
    LOG(ERROR) << "Writing to oat file " << out->GetPath() << " failed.";
    return false;
  }
}

bool PatchOat::WriteImage(File* out) {
  TimingLogger::ScopedTiming t("Writing image File", timings_);
  std::string error_msg;

  ScopedFlock img_flock;
  img_flock.Init(out, &error_msg);

  CHECK(image_ != nullptr);
  CHECK(out != nullptr);
  size_t expect = image_->Size();
  if (out->WriteFully(reinterpret_cast<char*>(image_->Begin()), expect) &&
      out->SetLength(expect) == 0) {
    return true;
  } else {
    LOG(ERROR) << "Writing to image file " << out->GetPath() << " failed.";
    return false;
  }
}

bool PatchOat::IsImagePic(const ImageHeader& image_header, const std::string& image_path) {
  if (!image_header.CompilePic()) {
    if (kIsDebugBuild) {
      LOG(INFO) << "image at location " << image_path << " was *not* compiled pic";
    }
    return false;
  }

  if (kIsDebugBuild) {
    LOG(INFO) << "image at location " << image_path << " was compiled PIC";
  }

  return true;
}

PatchOat::MaybePic PatchOat::IsOatPic(const ElfFile* oat_in) {
  if (oat_in == nullptr) {
    LOG(ERROR) << "No ELF input oat fie available";
    return ERROR_OAT_FILE;
  }

  const std::string& file_path = oat_in->GetFilePath();

  const OatHeader* oat_header = GetOatHeader(oat_in);
  if (oat_header == nullptr) {
    LOG(ERROR) << "Failed to find oat header in oat file " << file_path;
    return ERROR_OAT_FILE;
  }

  if (!oat_header->IsValid()) {
    LOG(ERROR) << "Elf file " << file_path << " has an invalid oat header";
    return ERROR_OAT_FILE;
  }

  bool is_pic = oat_header->IsPic();
  if (kIsDebugBuild) {
    LOG(INFO) << "Oat file at " << file_path << " is " << (is_pic ? "PIC" : "not pic");
  }

  return is_pic ? PIC : NOT_PIC;
}

bool PatchOat::ReplaceOatFileWithSymlink(const std::string& input_oat_filename,
                                         const std::string& output_oat_filename,
                                         bool output_oat_opened_from_fd,
                                         bool new_oat_out) {
  // Need a file when we are PIC, since we symlink over it. Refusing to symlink into FD.
  if (output_oat_opened_from_fd) {
    // TODO: installd uses --output-oat-fd. Should we change class linking logic for PIC?
    LOG(ERROR) << "No output oat filename specified, needs filename for when we are PIC";
    return false;
  }

  // Image was PIC. Create symlink where the oat is supposed to go.
  if (!new_oat_out) {
    LOG(ERROR) << "Oat file " << output_oat_filename << " already exists, refusing to overwrite";
    return false;
  }

  // Delete the original file, since we won't need it.
  unlink(output_oat_filename.c_str());

  // Create a symlink from the old oat to the new oat
  if (symlink(input_oat_filename.c_str(), output_oat_filename.c_str()) < 0) {
    int err = errno;
    LOG(ERROR) << "Failed to create symlink at " << output_oat_filename
               << " error(" << err << "): " << strerror(err);
    return false;
  }

  if (kIsDebugBuild) {
    LOG(INFO) << "Created symlink " << output_oat_filename << " -> " << input_oat_filename;
  }

  return true;
}

class PatchOatArtFieldVisitor : public ArtFieldVisitor {
 public:
  explicit PatchOatArtFieldVisitor(PatchOat* patch_oat) : patch_oat_(patch_oat) {}

  void Visit(ArtField* field) OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    ArtField* const dest = patch_oat_->RelocatedCopyOf(field);
    dest->SetDeclaringClass(patch_oat_->RelocatedAddressOfPointer(field->GetDeclaringClass()));
  }

 private:
  PatchOat* const patch_oat_;
};

void PatchOat::PatchArtFields(const ImageHeader* image_header) {
  PatchOatArtFieldVisitor visitor(this);
  image_header->VisitPackedArtFields(&visitor, heap_->Begin());
}

class PatchOatArtMethodVisitor : public ArtMethodVisitor {
 public:
  explicit PatchOatArtMethodVisitor(PatchOat* patch_oat) : patch_oat_(patch_oat) {}

  void Visit(ArtMethod* method) OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    ArtMethod* const dest = patch_oat_->RelocatedCopyOf(method);
    patch_oat_->FixupMethod(method, dest);
  }

 private:
  PatchOat* const patch_oat_;
};

void PatchOat::PatchArtMethods(const ImageHeader* image_header) {
  const size_t pointer_size = InstructionSetPointerSize(isa_);
  PatchOatArtMethodVisitor visitor(this);
  image_header->VisitPackedArtMethods(&visitor, heap_->Begin(), pointer_size);
}

void PatchOat::PatchImTables(const ImageHeader* image_header) {
  const size_t pointer_size = InstructionSetPointerSize(isa_);
  // We can safely walk target image since the conflict tables are independent.
  image_header->VisitPackedImTables(
      [this](ArtMethod* method) {
        return RelocatedAddressOfPointer(method);
      },
      image_->Begin(),
      pointer_size);
}

void PatchOat::PatchImtConflictTables(const ImageHeader* image_header) {
  const size_t pointer_size = InstructionSetPointerSize(isa_);
  // We can safely walk target image since the conflict tables are independent.
  image_header->VisitPackedImtConflictTables(
      [this](ArtMethod* method) {
        return RelocatedAddressOfPointer(method);
      },
      image_->Begin(),
      pointer_size);
}

class FixupRootVisitor : public RootVisitor {
 public:
  explicit FixupRootVisitor(const PatchOat* patch_oat) : patch_oat_(patch_oat) {
  }

  void VisitRoots(mirror::Object*** roots, size_t count, const RootInfo& info ATTRIBUTE_UNUSED)
      OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    for (size_t i = 0; i < count; ++i) {
      *roots[i] = patch_oat_->RelocatedAddressOfPointer(*roots[i]);
    }
  }

  void VisitRoots(mirror::CompressedReference<mirror::Object>** roots, size_t count,
                  const RootInfo& info ATTRIBUTE_UNUSED)
      OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    for (size_t i = 0; i < count; ++i) {
      roots[i]->Assign(patch_oat_->RelocatedAddressOfPointer(roots[i]->AsMirrorPtr()));
    }
  }

 private:
  const PatchOat* const patch_oat_;
};

void PatchOat::PatchInternedStrings(const ImageHeader* image_header) {
  const auto& section = image_header->GetImageSection(ImageHeader::kSectionInternedStrings);
  InternTable temp_table;
  // Note that we require that ReadFromMemory does not make an internal copy of the elements.
  // This also relies on visit roots not doing any verification which could fail after we update
  // the roots to be the image addresses.
  temp_table.AddTableFromMemory(image_->Begin() + section.Offset());
  FixupRootVisitor visitor(this);
  temp_table.VisitRoots(&visitor, kVisitRootFlagAllRoots);
}

void PatchOat::PatchClassTable(const ImageHeader* image_header) {
  const auto& section = image_header->GetImageSection(ImageHeader::kSectionClassTable);
  if (section.Size() == 0) {
    return;
  }
  // Note that we require that ReadFromMemory does not make an internal copy of the elements.
  // This also relies on visit roots not doing any verification which could fail after we update
  // the roots to be the image addresses.
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  ClassTable temp_table;
  temp_table.ReadFromMemory(image_->Begin() + section.Offset());
  FixupRootVisitor visitor(this);
  BufferedRootVisitor<kDefaultBufferedRootCount> buffered_visitor(&visitor, RootInfo(kRootUnknown));
  temp_table.VisitRoots(buffered_visitor);
}


class RelocatedPointerVisitor {
 public:
  explicit RelocatedPointerVisitor(PatchOat* patch_oat) : patch_oat_(patch_oat) {}

  template <typename T>
  T* operator()(T* ptr) const {
    return patch_oat_->RelocatedAddressOfPointer(ptr);
  }

 private:
  PatchOat* const patch_oat_;
};

void PatchOat::PatchDexFileArrays(mirror::ObjectArray<mirror::Object>* img_roots) {
  auto* dex_caches = down_cast<mirror::ObjectArray<mirror::DexCache>*>(
      img_roots->Get(ImageHeader::kDexCaches));
  const size_t pointer_size = InstructionSetPointerSize(isa_);
  for (size_t i = 0, count = dex_caches->GetLength(); i < count; ++i) {
    auto* orig_dex_cache = dex_caches->GetWithoutChecks(i);
    auto* copy_dex_cache = RelocatedCopyOf(orig_dex_cache);
    // Though the DexCache array fields are usually treated as native pointers, we set the full
    // 64-bit values here, clearing the top 32 bits for 32-bit targets. The zero-extension is
    // done by casting to the unsigned type uintptr_t before casting to int64_t, i.e.
    //     static_cast<int64_t>(reinterpret_cast<uintptr_t>(image_begin_ + offset))).
    GcRoot<mirror::String>* orig_strings = orig_dex_cache->GetStrings();
    GcRoot<mirror::String>* relocated_strings = RelocatedAddressOfPointer(orig_strings);
    copy_dex_cache->SetField64<false>(
        mirror::DexCache::StringsOffset(),
        static_cast<int64_t>(reinterpret_cast<uintptr_t>(relocated_strings)));
    if (orig_strings != nullptr) {
      orig_dex_cache->FixupStrings(RelocatedCopyOf(orig_strings), RelocatedPointerVisitor(this));
    }
    GcRoot<mirror::Class>* orig_types = orig_dex_cache->GetResolvedTypes();
    GcRoot<mirror::Class>* relocated_types = RelocatedAddressOfPointer(orig_types);
    copy_dex_cache->SetField64<false>(
        mirror::DexCache::ResolvedTypesOffset(),
        static_cast<int64_t>(reinterpret_cast<uintptr_t>(relocated_types)));
    if (orig_types != nullptr) {
      orig_dex_cache->FixupResolvedTypes(RelocatedCopyOf(orig_types),
                                         RelocatedPointerVisitor(this));
    }
    ArtMethod** orig_methods = orig_dex_cache->GetResolvedMethods();
    ArtMethod** relocated_methods = RelocatedAddressOfPointer(orig_methods);
    copy_dex_cache->SetField64<false>(
        mirror::DexCache::ResolvedMethodsOffset(),
        static_cast<int64_t>(reinterpret_cast<uintptr_t>(relocated_methods)));
    if (orig_methods != nullptr) {
      ArtMethod** copy_methods = RelocatedCopyOf(orig_methods);
      for (size_t j = 0, num = orig_dex_cache->NumResolvedMethods(); j != num; ++j) {
        ArtMethod* orig = mirror::DexCache::GetElementPtrSize(orig_methods, j, pointer_size);
        ArtMethod* copy = RelocatedAddressOfPointer(orig);
        mirror::DexCache::SetElementPtrSize(copy_methods, j, copy, pointer_size);
      }
    }
    ArtField** orig_fields = orig_dex_cache->GetResolvedFields();
    ArtField** relocated_fields = RelocatedAddressOfPointer(orig_fields);
    copy_dex_cache->SetField64<false>(
        mirror::DexCache::ResolvedFieldsOffset(),
        static_cast<int64_t>(reinterpret_cast<uintptr_t>(relocated_fields)));
    if (orig_fields != nullptr) {
      ArtField** copy_fields = RelocatedCopyOf(orig_fields);
      for (size_t j = 0, num = orig_dex_cache->NumResolvedFields(); j != num; ++j) {
        ArtField* orig = mirror::DexCache::GetElementPtrSize(orig_fields, j, pointer_size);
        ArtField* copy = RelocatedAddressOfPointer(orig);
        mirror::DexCache::SetElementPtrSize(copy_fields, j, copy, pointer_size);
      }
    }
  }
}

bool PatchOat::PatchImage(bool primary_image) {
  ImageHeader* image_header = reinterpret_cast<ImageHeader*>(image_->Begin());
  CHECK_GT(image_->Size(), sizeof(ImageHeader));
  // These are the roots from the original file.
  auto* img_roots = image_header->GetImageRoots();
  image_header->RelocateImage(delta_);

  PatchArtFields(image_header);
  PatchArtMethods(image_header);
  PatchImTables(image_header);
  PatchImtConflictTables(image_header);
  PatchInternedStrings(image_header);
  PatchClassTable(image_header);
  // Patch dex file int/long arrays which point to ArtFields.
  PatchDexFileArrays(img_roots);

  if (primary_image) {
    VisitObject(img_roots);
  }

  if (!image_header->IsValid()) {
    LOG(ERROR) << "relocation renders image header invalid";
    return false;
  }

  {
    TimingLogger::ScopedTiming t("Walk Bitmap", timings_);
    // Walk the bitmap.
    WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
    bitmap_->Walk(PatchOat::BitmapCallback, this);
  }
  return true;
}


void PatchOat::PatchVisitor::operator() (mirror::Object* obj, MemberOffset off,
                                         bool is_static_unused ATTRIBUTE_UNUSED) const {
  mirror::Object* referent = obj->GetFieldObject<mirror::Object, kVerifyNone>(off);
  mirror::Object* moved_object = patcher_->RelocatedAddressOfPointer(referent);
  copy_->SetFieldObjectWithoutWriteBarrier<false, true, kVerifyNone>(off, moved_object);
}

void PatchOat::PatchVisitor::operator() (mirror::Class* cls ATTRIBUTE_UNUSED,
                                         mirror::Reference* ref) const {
  MemberOffset off = mirror::Reference::ReferentOffset();
  mirror::Object* referent = ref->GetReferent();
  DCHECK(referent == nullptr ||
         Runtime::Current()->GetHeap()->ObjectIsInBootImageSpace(referent)) << referent;
  mirror::Object* moved_object = patcher_->RelocatedAddressOfPointer(referent);
  copy_->SetFieldObjectWithoutWriteBarrier<false, true, kVerifyNone>(off, moved_object);
}

// Called by BitmapCallback
void PatchOat::VisitObject(mirror::Object* object) {
  mirror::Object* copy = RelocatedCopyOf(object);
  CHECK(copy != nullptr);
  if (kUseBakerOrBrooksReadBarrier) {
    object->AssertReadBarrierPointer();
    if (kUseBrooksReadBarrier) {
      mirror::Object* moved_to = RelocatedAddressOfPointer(object);
      copy->SetReadBarrierPointer(moved_to);
      DCHECK_EQ(copy->GetReadBarrierPointer(), moved_to);
    }
  }
  PatchOat::PatchVisitor visitor(this, copy);
  object->VisitReferences<kVerifyNone>(visitor, visitor);
  if (object->IsClass<kVerifyNone>()) {
    const size_t pointer_size = InstructionSetPointerSize(isa_);
    mirror::Class* klass = object->AsClass();
    mirror::Class* copy_klass = down_cast<mirror::Class*>(copy);
    RelocatedPointerVisitor native_visitor(this);
    klass->FixupNativePointers(copy_klass, pointer_size, native_visitor);
    auto* vtable = klass->GetVTable();
    if (vtable != nullptr) {
      vtable->Fixup(RelocatedCopyOfFollowImages(vtable), pointer_size, native_visitor);
    }
    auto* iftable = klass->GetIfTable();
    if (iftable != nullptr) {
      for (int32_t i = 0; i < klass->GetIfTableCount(); ++i) {
        if (iftable->GetMethodArrayCount(i) > 0) {
          auto* method_array = iftable->GetMethodArray(i);
          CHECK(method_array != nullptr);
          method_array->Fixup(RelocatedCopyOfFollowImages(method_array),
                              pointer_size,
                              native_visitor);
        }
      }
    }
  } else if (object->GetClass() == mirror::Method::StaticClass() ||
             object->GetClass() == mirror::Constructor::StaticClass()) {
    // Need to go update the ArtMethod.
    auto* dest = down_cast<mirror::AbstractMethod*>(copy);
    auto* src = down_cast<mirror::AbstractMethod*>(object);
    dest->SetArtMethod(RelocatedAddressOfPointer(src->GetArtMethod()));
  }
}

void PatchOat::FixupMethod(ArtMethod* object, ArtMethod* copy) {
  const size_t pointer_size = InstructionSetPointerSize(isa_);
  copy->CopyFrom(object, pointer_size);
  // Just update the entry points if it looks like we should.
  // TODO: sanity check all the pointers' values
  copy->SetDeclaringClass(RelocatedAddressOfPointer(object->GetDeclaringClass()));
  copy->SetDexCacheResolvedMethods(
      RelocatedAddressOfPointer(object->GetDexCacheResolvedMethods(pointer_size)), pointer_size);
  copy->SetDexCacheResolvedTypes(
      RelocatedAddressOfPointer(object->GetDexCacheResolvedTypes(pointer_size)), pointer_size);
  copy->SetEntryPointFromQuickCompiledCodePtrSize(RelocatedAddressOfPointer(
      object->GetEntryPointFromQuickCompiledCodePtrSize(pointer_size)), pointer_size);
  // No special handling for IMT conflict table since all pointers are moved by the same offset.
  copy->SetEntryPointFromJniPtrSize(RelocatedAddressOfPointer(
      object->GetEntryPointFromJniPtrSize(pointer_size)), pointer_size);
}

bool PatchOat::Patch(File* input_oat, off_t delta, File* output_oat, TimingLogger* timings,
                     bool output_oat_opened_from_fd, bool new_oat_out) {
  CHECK(input_oat != nullptr);
  CHECK(output_oat != nullptr);
  CHECK_GE(input_oat->Fd(), 0);
  CHECK_GE(output_oat->Fd(), 0);
  TimingLogger::ScopedTiming t("Setup Oat File Patching", timings);

  std::string error_msg;
  std::unique_ptr<ElfFile> elf(ElfFile::Open(input_oat,
                                             PROT_READ | PROT_WRITE, MAP_PRIVATE, &error_msg));
  if (elf.get() == nullptr) {
    LOG(ERROR) << "unable to open oat file " << input_oat->GetPath() << " : " << error_msg;
    return false;
  }

  MaybePic is_oat_pic = IsOatPic(elf.get());
  if (is_oat_pic >= ERROR_FIRST) {
    // Error logged by IsOatPic
    return false;
  } else if (is_oat_pic == PIC) {
    // Do not need to do ELF-file patching. Create a symlink and skip the rest.
    // Any errors will be logged by the function call.
    return ReplaceOatFileWithSymlink(input_oat->GetPath(),
                                     output_oat->GetPath(),
                                     output_oat_opened_from_fd,
                                     new_oat_out);
  } else {
    CHECK(is_oat_pic == NOT_PIC);
  }

  PatchOat p(elf.release(), delta, timings);
  t.NewTiming("Patch Oat file");
  if (!p.PatchElf()) {
    return false;
  }

  t.NewTiming("Writing oat file");
  if (!p.WriteElf(output_oat)) {
    return false;
  }
  return true;
}

template <typename ElfFileImpl>
bool PatchOat::PatchOatHeader(ElfFileImpl* oat_file) {
  auto rodata_sec = oat_file->FindSectionByName(".rodata");
  if (rodata_sec == nullptr) {
    return false;
  }
  OatHeader* oat_header = reinterpret_cast<OatHeader*>(oat_file->Begin() + rodata_sec->sh_offset);
  if (!oat_header->IsValid()) {
    LOG(ERROR) << "Elf file " << oat_file->GetFilePath() << " has an invalid oat header";
    return false;
  }
  oat_header->RelocateOat(delta_);
  return true;
}

bool PatchOat::PatchElf() {
  if (oat_file_->Is64Bit()) {
    return PatchElf<ElfFileImpl64>(oat_file_->GetImpl64());
  } else {
    return PatchElf<ElfFileImpl32>(oat_file_->GetImpl32());
  }
}

template <typename ElfFileImpl>
bool PatchOat::PatchElf(ElfFileImpl* oat_file) {
  TimingLogger::ScopedTiming t("Fixup Elf Text Section", timings_);

  // Fix up absolute references to locations within the boot image.
  if (!oat_file->ApplyOatPatchesTo(".text", delta_)) {
    return false;
  }

  // Update the OatHeader fields referencing the boot image.
  if (!PatchOatHeader<ElfFileImpl>(oat_file)) {
    return false;
  }

  bool need_boot_oat_fixup = true;
  for (unsigned int i = 0; i < oat_file->GetProgramHeaderNum(); ++i) {
    auto hdr = oat_file->GetProgramHeader(i);
    if (hdr->p_type == PT_LOAD && hdr->p_vaddr == 0u) {
      need_boot_oat_fixup = false;
      break;
    }
  }
  if (!need_boot_oat_fixup) {
    // This is an app oat file that can be loaded at an arbitrary address in memory.
    // Boot image references were patched above and there's nothing else to do.
    return true;
  }

  // This is a boot oat file that's loaded at a particular address and we need
  // to patch all absolute addresses, starting with ELF program headers.

  t.NewTiming("Fixup Elf Headers");
  // Fixup Phdr's
  oat_file->FixupProgramHeaders(delta_);

  t.NewTiming("Fixup Section Headers");
  // Fixup Shdr's
  oat_file->FixupSectionHeaders(delta_);

  t.NewTiming("Fixup Dynamics");
  oat_file->FixupDynamic(delta_);

  t.NewTiming("Fixup Elf Symbols");
  // Fixup dynsym
  if (!oat_file->FixupSymbols(delta_, true)) {
    return false;
  }
  // Fixup symtab
  if (!oat_file->FixupSymbols(delta_, false)) {
    return false;
  }

  t.NewTiming("Fixup Debug Sections");
  if (!oat_file->FixupDebugSections(delta_)) {
    return false;
  }

  return true;
}

static int orig_argc;
static char** orig_argv;

static std::string CommandLine() {
  std::vector<std::string> command;
  for (int i = 0; i < orig_argc; ++i) {
    command.push_back(orig_argv[i]);
  }
  return Join(command, ' ');
}

static void UsageErrorV(const char* fmt, va_list ap) {
  std::string error;
  StringAppendV(&error, fmt, ap);
  LOG(ERROR) << error;
}

static void UsageError(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  UsageErrorV(fmt, ap);
  va_end(ap);
}

NO_RETURN static void Usage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  UsageErrorV(fmt, ap);
  va_end(ap);

  UsageError("Command: %s", CommandLine().c_str());
  UsageError("Usage: patchoat [options]...");
  UsageError("");
  UsageError("  --instruction-set=<isa>: Specifies the instruction set the patched code is");
  UsageError("      compiled for. Required if you use --input-oat-location");
  UsageError("");
  UsageError("  --input-oat-file=<file.oat>: Specifies the exact filename of the oat file to be");
  UsageError("      patched.");
  UsageError("");
  UsageError("  --input-oat-fd=<file-descriptor>: Specifies the file-descriptor of the oat file");
  UsageError("      to be patched.");
  UsageError("");
  UsageError("  --input-oat-location=<file.oat>: Specifies the 'location' to read the patched");
  UsageError("      oat file from. If used one must also supply the --instruction-set");
  UsageError("");
  UsageError("  --input-image-location=<file.art>: Specifies the 'location' of the image file to");
  UsageError("      be patched. If --instruction-set is not given it will use the instruction set");
  UsageError("      extracted from the --input-oat-file.");
  UsageError("");
  UsageError("  --output-oat-file=<file.oat>: Specifies the exact file to write the patched oat");
  UsageError("      file to.");
  UsageError("");
  UsageError("  --output-oat-fd=<file-descriptor>: Specifies the file-descriptor to write the");
  UsageError("      the patched oat file to.");
  UsageError("");
  UsageError("  --output-image-file=<file.art>: Specifies the exact file to write the patched");
  UsageError("      image file to.");
  UsageError("");
  UsageError("  --base-offset-delta=<delta>: Specify the amount to change the old base-offset by.");
  UsageError("      This value may be negative.");
  UsageError("");
  UsageError("  --patched-image-location=<file.art>: Relocate the oat file to be the same as the");
  UsageError("      image at the given location. If used one must also specify the");
  UsageError("      --instruction-set flag. It will search for this image in the same way that");
  UsageError("      is done when loading one.");
  UsageError("");
  UsageError("  --lock-output: Obtain a flock on output oat file before starting.");
  UsageError("");
  UsageError("  --no-lock-output: Do not attempt to obtain a flock on output oat file.");
  UsageError("");
  UsageError("  --dump-timings: dump out patch timing information");
  UsageError("");
  UsageError("  --no-dump-timings: do not dump out patch timing information");
  UsageError("");

  exit(EXIT_FAILURE);
}

static bool ReadBaseDelta(const char* name, off_t* delta, std::string* error_msg) {
  CHECK(name != nullptr);
  CHECK(delta != nullptr);
  std::unique_ptr<File> file;
  if (OS::FileExists(name)) {
    file.reset(OS::OpenFileForReading(name));
    if (file.get() == nullptr) {
      *error_msg = "Failed to open file %s for reading";
      return false;
    }
  } else {
    *error_msg = "File %s does not exist";
    return false;
  }
  CHECK(file.get() != nullptr);
  ImageHeader hdr;
  if (sizeof(hdr) != file->Read(reinterpret_cast<char*>(&hdr), sizeof(hdr), 0)) {
    *error_msg = "Failed to read file %s";
    return false;
  }
  if (!hdr.IsValid()) {
    *error_msg = "%s does not contain a valid image header.";
    return false;
  }
  *delta = hdr.GetPatchDelta();
  return true;
}

static int patchoat_image(TimingLogger& timings,
                          InstructionSet isa,
                          const std::string& input_image_location,
                          const std::string& output_image_filename,
                          off_t base_delta,
                          bool base_delta_set,
                          bool debug) {
  CHECK(!input_image_location.empty());
  if (output_image_filename.empty()) {
    Usage("Image patching requires --output-image-file");
  }

  if (!base_delta_set) {
    Usage("Must supply a desired new offset or delta.");
  }

  if (!IsAligned<kPageSize>(base_delta)) {
    Usage("Base offset/delta must be aligned to a pagesize (0x%08x) boundary.", kPageSize);
  }

  if (debug) {
    LOG(INFO) << "moving offset by " << base_delta
        << " (0x" << std::hex << base_delta << ") bytes or "
        << std::dec << (base_delta/kPageSize) << " pages.";
  }

  TimingLogger::ScopedTiming pt("patch image and oat", &timings);

  std::string output_directory =
      output_image_filename.substr(0, output_image_filename.find_last_of("/"));
  bool ret = PatchOat::Patch(input_image_location, base_delta, output_directory, isa, &timings);

  if (kIsDebugBuild) {
    LOG(INFO) << "Exiting with return ... " << ret;
  }
  return ret ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int patchoat_oat(TimingLogger& timings,
                        InstructionSet isa,
                        const std::string& patched_image_location,
                        off_t base_delta,
                        bool base_delta_set,
                        int input_oat_fd,
                        const std::string& input_oat_location,
                        std::string input_oat_filename,
                        bool have_input_oat,
                        int output_oat_fd,
                        std::string output_oat_filename,
                        bool have_output_oat,
                        bool lock_output,
                        bool debug) {
  {
    // Only 1 of these may be set.
    uint32_t cnt = 0;
    cnt += (base_delta_set) ? 1 : 0;
    cnt += (!patched_image_location.empty()) ? 1 : 0;
    if (cnt > 1) {
      Usage("Only one of --base-offset-delta or --patched-image-location may be used.");
    } else if (cnt == 0) {
      Usage("Must specify --base-offset-delta or --patched-image-location.");
    }
  }

  if (!have_input_oat || !have_output_oat) {
    Usage("Both input and output oat must be supplied to patch an app odex.");
  }

  if (!input_oat_location.empty()) {
    if (!LocationToFilename(input_oat_location, isa, &input_oat_filename)) {
      Usage("Unable to find filename for input oat location %s", input_oat_location.c_str());
    }
    if (debug) {
      LOG(INFO) << "Using input-oat-file " << input_oat_filename;
    }
  }

  bool match_delta = false;
  if (!patched_image_location.empty()) {
    std::string system_filename;
    bool has_system = false;
    std::string cache_filename;
    bool has_cache = false;
    bool has_android_data_unused = false;
    bool is_global_cache = false;
    if (!gc::space::ImageSpace::FindImageFilename(patched_image_location.c_str(), isa,
                                                  &system_filename, &has_system, &cache_filename,
                                                  &has_android_data_unused, &has_cache,
                                                  &is_global_cache)) {
      Usage("Unable to determine image file for location %s", patched_image_location.c_str());
    }
    std::string patched_image_filename;
    if (has_cache) {
      patched_image_filename = cache_filename;
    } else if (has_system) {
      LOG(WARNING) << "Only image file found was in /system for image location "
          << patched_image_location;
      patched_image_filename = system_filename;
    } else {
      Usage("Unable to determine image file for location %s", patched_image_location.c_str());
    }
    if (debug) {
      LOG(INFO) << "Using patched-image-file " << patched_image_filename;
    }

    base_delta_set = true;
    match_delta = true;
    std::string error_msg;
    if (!ReadBaseDelta(patched_image_filename.c_str(), &base_delta, &error_msg)) {
      Usage(error_msg.c_str(), patched_image_filename.c_str());
    }
  }

  if (!IsAligned<kPageSize>(base_delta)) {
    Usage("Base offset/delta must be alligned to a pagesize (0x%08x) boundary.", kPageSize);
  }

  // Do we need to cleanup output files if we fail?
  bool new_oat_out = false;

  std::unique_ptr<File> input_oat;
  std::unique_ptr<File> output_oat;

  if (input_oat_fd != -1) {
    if (input_oat_filename.empty()) {
      input_oat_filename = "input-oat-file";
    }
    input_oat.reset(new File(input_oat_fd, input_oat_filename, false));
    if (input_oat_fd == output_oat_fd) {
      input_oat.get()->DisableAutoClose();
    }
    if (input_oat == nullptr) {
      // Unlikely, but ensure exhaustive logging in non-0 exit code case
      LOG(ERROR) << "Failed to open input oat file by its FD" << input_oat_fd;
      return EXIT_FAILURE;
    }
  } else {
    CHECK(!input_oat_filename.empty());
    input_oat.reset(OS::OpenFileForReading(input_oat_filename.c_str()));
    if (input_oat == nullptr) {
      int err = errno;
      LOG(ERROR) << "Failed to open input oat file " << input_oat_filename
          << ": " << strerror(err) << "(" << err << ")";
      return EXIT_FAILURE;
    }
  }

  std::string error_msg;
  std::unique_ptr<ElfFile> elf(ElfFile::Open(input_oat.get(), PROT_READ, MAP_PRIVATE, &error_msg));
  if (elf.get() == nullptr) {
    LOG(ERROR) << "unable to open oat file " << input_oat->GetPath() << " : " << error_msg;
    return EXIT_FAILURE;
  }
  if (!elf->HasSection(".text.oat_patches")) {
    LOG(ERROR) << "missing oat patch section in input oat file " << input_oat->GetPath();
    return EXIT_FAILURE;
  }

  if (output_oat_fd != -1) {
    if (output_oat_filename.empty()) {
      output_oat_filename = "output-oat-file";
    }
    output_oat.reset(new File(output_oat_fd, output_oat_filename, true));
    if (output_oat == nullptr) {
      // Unlikely, but ensure exhaustive logging in non-0 exit code case
      LOG(ERROR) << "Failed to open output oat file by its FD" << output_oat_fd;
    }
  } else {
    CHECK(!output_oat_filename.empty());
    output_oat.reset(CreateOrOpen(output_oat_filename.c_str(), &new_oat_out));
    if (output_oat == nullptr) {
      int err = errno;
      LOG(ERROR) << "Failed to open output oat file " << output_oat_filename
          << ": " << strerror(err) << "(" << err << ")";
    }
  }

  // TODO: get rid of this.
  auto cleanup = [&output_oat_filename, &new_oat_out](bool success) {
    if (!success) {
      if (new_oat_out) {
        CHECK(!output_oat_filename.empty());
        unlink(output_oat_filename.c_str());
      }
    }

    if (kIsDebugBuild) {
      LOG(INFO) << "Cleaning up.. success? " << success;
    }
  };

  if (output_oat.get() == nullptr) {
    cleanup(false);
    return EXIT_FAILURE;
  }

  if (match_delta) {
    // Figure out what the current delta is so we can match it to the desired delta.
    off_t current_delta = 0;
    if (!ReadOatPatchDelta(elf.get(), &current_delta, &error_msg)) {
      LOG(ERROR) << "Unable to get current delta: " << error_msg;
      cleanup(false);
      return EXIT_FAILURE;
    }
    // Before this line base_delta is the desired final delta. We need it to be the actual amount to
    // change everything by. We subtract the current delta from it to make it this.
    base_delta -= current_delta;
    if (!IsAligned<kPageSize>(base_delta)) {
      LOG(ERROR) << "Given image file was relocated by an illegal delta";
      cleanup(false);
      return false;
    }
  }

  if (debug) {
    LOG(INFO) << "moving offset by " << base_delta
        << " (0x" << std::hex << base_delta << ") bytes or "
        << std::dec << (base_delta/kPageSize) << " pages.";
  }

  ScopedFlock output_oat_lock;
  if (lock_output) {
    if (!output_oat_lock.Init(output_oat.get(), &error_msg)) {
      LOG(ERROR) << "Unable to lock output oat " << output_oat->GetPath() << ": " << error_msg;
      cleanup(false);
      return EXIT_FAILURE;
    }
  }

  TimingLogger::ScopedTiming pt("patch oat", &timings);
  bool ret = PatchOat::Patch(input_oat.get(), base_delta, output_oat.get(), &timings,
                             output_oat_fd >= 0,  // was it opened from FD?
                             new_oat_out);
  ret = FinishFile(output_oat.get(), ret);

  if (kIsDebugBuild) {
    LOG(INFO) << "Exiting with return ... " << ret;
  }
  cleanup(ret);
  return ret ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int patchoat(int argc, char **argv) {
  InitLogging(argv);
  MemMap::Init();
  const bool debug = kIsDebugBuild;
  orig_argc = argc;
  orig_argv = argv;
  TimingLogger timings("patcher", false, false);

  InitLogging(argv);

  // Skip over the command name.
  argv++;
  argc--;

  if (argc == 0) {
    Usage("No arguments specified");
  }

  timings.StartTiming("Patchoat");

  // cmd line args
  bool isa_set = false;
  InstructionSet isa = kNone;
  std::string input_oat_filename;
  std::string input_oat_location;
  int input_oat_fd = -1;
  bool have_input_oat = false;
  std::string input_image_location;
  std::string output_oat_filename;
  int output_oat_fd = -1;
  bool have_output_oat = false;
  std::string output_image_filename;
  off_t base_delta = 0;
  bool base_delta_set = false;
  std::string patched_image_filename;
  std::string patched_image_location;
  bool dump_timings = kIsDebugBuild;
  bool lock_output = true;

  for (int i = 0; i < argc; ++i) {
    const StringPiece option(argv[i]);
    const bool log_options = false;
    if (log_options) {
      LOG(INFO) << "patchoat: option[" << i << "]=" << argv[i];
    }
    if (option.starts_with("--instruction-set=")) {
      isa_set = true;
      const char* isa_str = option.substr(strlen("--instruction-set=")).data();
      isa = GetInstructionSetFromString(isa_str);
      if (isa == kNone) {
        Usage("Unknown or invalid instruction set %s", isa_str);
      }
    } else if (option.starts_with("--input-oat-location=")) {
      if (have_input_oat) {
        Usage("Only one of --input-oat-file, --input-oat-location and --input-oat-fd may be used.");
      }
      have_input_oat = true;
      input_oat_location = option.substr(strlen("--input-oat-location=")).data();
    } else if (option.starts_with("--input-oat-file=")) {
      if (have_input_oat) {
        Usage("Only one of --input-oat-file, --input-oat-location and --input-oat-fd may be used.");
      }
      have_input_oat = true;
      input_oat_filename = option.substr(strlen("--input-oat-file=")).data();
    } else if (option.starts_with("--input-oat-fd=")) {
      if (have_input_oat) {
        Usage("Only one of --input-oat-file, --input-oat-location and --input-oat-fd may be used.");
      }
      have_input_oat = true;
      const char* oat_fd_str = option.substr(strlen("--input-oat-fd=")).data();
      if (!ParseInt(oat_fd_str, &input_oat_fd)) {
        Usage("Failed to parse --input-oat-fd argument '%s' as an integer", oat_fd_str);
      }
      if (input_oat_fd < 0) {
        Usage("--input-oat-fd pass a negative value %d", input_oat_fd);
      }
    } else if (option.starts_with("--input-image-location=")) {
      input_image_location = option.substr(strlen("--input-image-location=")).data();
    } else if (option.starts_with("--output-oat-file=")) {
      if (have_output_oat) {
        Usage("Only one of --output-oat-file, and --output-oat-fd may be used.");
      }
      have_output_oat = true;
      output_oat_filename = option.substr(strlen("--output-oat-file=")).data();
    } else if (option.starts_with("--output-oat-fd=")) {
      if (have_output_oat) {
        Usage("Only one of --output-oat-file, --output-oat-fd may be used.");
      }
      have_output_oat = true;
      const char* oat_fd_str = option.substr(strlen("--output-oat-fd=")).data();
      if (!ParseInt(oat_fd_str, &output_oat_fd)) {
        Usage("Failed to parse --output-oat-fd argument '%s' as an integer", oat_fd_str);
      }
      if (output_oat_fd < 0) {
        Usage("--output-oat-fd pass a negative value %d", output_oat_fd);
      }
    } else if (option.starts_with("--output-image-file=")) {
      output_image_filename = option.substr(strlen("--output-image-file=")).data();
    } else if (option.starts_with("--base-offset-delta=")) {
      const char* base_delta_str = option.substr(strlen("--base-offset-delta=")).data();
      base_delta_set = true;
      if (!ParseInt(base_delta_str, &base_delta)) {
        Usage("Failed to parse --base-offset-delta argument '%s' as an off_t", base_delta_str);
      }
    } else if (option.starts_with("--patched-image-location=")) {
      patched_image_location = option.substr(strlen("--patched-image-location=")).data();
    } else if (option == "--lock-output") {
      lock_output = true;
    } else if (option == "--no-lock-output") {
      lock_output = false;
    } else if (option == "--dump-timings") {
      dump_timings = true;
    } else if (option == "--no-dump-timings") {
      dump_timings = false;
    } else {
      Usage("Unknown argument %s", option.data());
    }
  }

  // The instruction set is mandatory. This simplifies things...
  if (!isa_set) {
    Usage("Instruction set must be set.");
  }

  int ret;
  if (!input_image_location.empty()) {
    ret = patchoat_image(timings,
                         isa,
                         input_image_location,
                         output_image_filename,
                         base_delta,
                         base_delta_set,
                         debug);
  } else {
    ret = patchoat_oat(timings,
                       isa,
                       patched_image_location,
                       base_delta,
                       base_delta_set,
                       input_oat_fd,
                       input_oat_location,
                       input_oat_filename,
                       have_input_oat,
                       output_oat_fd,
                       output_oat_filename,
                       have_output_oat,
                       lock_output,
                       debug);
  }

  timings.EndTiming();
  if (dump_timings) {
    LOG(INFO) << Dumpable<TimingLogger>(timings);
  }

  return ret;
}

}  // namespace art

int main(int argc, char **argv) {
  return art::patchoat(argc, argv);
}
