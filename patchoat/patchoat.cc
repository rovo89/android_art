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
#include "image.h"
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

bool PatchOat::Patch(const std::string& image_location, off_t delta,
                     File* output_image, InstructionSet isa,
                     TimingLogger* timings) {
  CHECK(Runtime::Current() == nullptr);
  CHECK(output_image != nullptr);
  CHECK_GE(output_image->Fd(), 0);
  CHECK(!image_location.empty()) << "image file must have a filename.";
  CHECK_NE(isa, kNone);

  TimingLogger::ScopedTiming t("Runtime Setup", timings);
  const char *isa_name = GetInstructionSetString(isa);
  std::string image_filename;
  if (!LocationToFilename(image_location, isa, &image_filename)) {
    LOG(ERROR) << "Unable to find image at location " << image_location;
    return false;
  }
  std::unique_ptr<File> input_image(OS::OpenFileForReading(image_filename.c_str()));
  if (input_image.get() == nullptr) {
    LOG(ERROR) << "unable to open input image file at " << image_filename
               << " for location " << image_location;
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
    return false;
  }

  /*bool is_image_pic = */IsImagePic(image_header, input_image->GetPath());
  // Nothing special to do right now since the image always needs to get patched.
  // Perhaps in some far-off future we may have images with relative addresses that are true-PIC.

  // Set up the runtime
  RuntimeOptions options;
  NoopCompilerCallbacks callbacks;
  options.push_back(std::make_pair("compilercallbacks", &callbacks));
  std::string img = "-Ximage:" + image_location;
  options.push_back(std::make_pair(img.c_str(), nullptr));
  options.push_back(std::make_pair("imageinstructionset", reinterpret_cast<const void*>(isa_name)));
  if (!Runtime::Create(options, false)) {
    LOG(ERROR) << "Unable to initialize runtime";
    return false;
  }
  // Runtime::Create acquired the mutator_lock_ that is normally given away when we Runtime::Start,
  // give it away now and then switch to a more manageable ScopedObjectAccess.
  Thread::Current()->TransitionFromRunnableToSuspended(kNative);
  ScopedObjectAccess soa(Thread::Current());

  t.NewTiming("Image and oat Patching setup");
  // Create the map where we will write the image patches to.
  std::string error_msg;
  std::unique_ptr<MemMap> image(MemMap::MapFile(image_len, PROT_READ | PROT_WRITE, MAP_PRIVATE,
                                                input_image->Fd(), 0,
                                                input_image->GetPath().c_str(),
                                                &error_msg));
  if (image.get() == nullptr) {
    LOG(ERROR) << "unable to map image file " << input_image->GetPath() << " : " << error_msg;
    return false;
  }
  gc::space::ImageSpace* ispc = Runtime::Current()->GetHeap()->GetImageSpace();

  PatchOat p(isa, image.release(), ispc->GetLiveBitmap(), ispc->GetMemMap(),
             delta, timings);
  t.NewTiming("Patching files");
  if (!p.PatchImage()) {
    LOG(ERROR) << "Failed to patch image file " << input_image->GetPath();
    return false;
  }

  t.NewTiming("Writing files");
  if (!p.WriteImage(output_image)) {
    return false;
  }
  return true;
}

bool PatchOat::Patch(File* input_oat, const std::string& image_location, off_t delta,
                     File* output_oat, File* output_image, InstructionSet isa,
                     TimingLogger* timings,
                     bool output_oat_opened_from_fd,
                     bool new_oat_out) {
  CHECK(Runtime::Current() == nullptr);
  CHECK(output_image != nullptr);
  CHECK_GE(output_image->Fd(), 0);
  CHECK(input_oat != nullptr);
  CHECK(output_oat != nullptr);
  CHECK_GE(input_oat->Fd(), 0);
  CHECK_GE(output_oat->Fd(), 0);
  CHECK(!image_location.empty()) << "image file must have a filename.";

  TimingLogger::ScopedTiming t("Runtime Setup", timings);

  if (isa == kNone) {
    Elf32_Ehdr elf_hdr;
    if (sizeof(elf_hdr) != input_oat->Read(reinterpret_cast<char*>(&elf_hdr), sizeof(elf_hdr), 0)) {
      LOG(ERROR) << "unable to read elf header";
      return false;
    }
    isa = GetInstructionSetFromELF(elf_hdr.e_machine, elf_hdr.e_flags);
  }
  const char* isa_name = GetInstructionSetString(isa);
  std::string image_filename;
  if (!LocationToFilename(image_location, isa, &image_filename)) {
    LOG(ERROR) << "Unable to find image at location " << image_location;
    return false;
  }
  std::unique_ptr<File> input_image(OS::OpenFileForReading(image_filename.c_str()));
  if (input_image.get() == nullptr) {
    LOG(ERROR) << "unable to open input image file at " << image_filename
               << " for location " << image_location;
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

  // Set up the runtime
  RuntimeOptions options;
  NoopCompilerCallbacks callbacks;
  options.push_back(std::make_pair("compilercallbacks", &callbacks));
  std::string img = "-Ximage:" + image_location;
  options.push_back(std::make_pair(img.c_str(), nullptr));
  options.push_back(std::make_pair("imageinstructionset", reinterpret_cast<const void*>(isa_name)));
  if (!Runtime::Create(options, false)) {
    LOG(ERROR) << "Unable to initialize runtime";
    return false;
  }
  // Runtime::Create acquired the mutator_lock_ that is normally given away when we Runtime::Start,
  // give it away now and then switch to a more manageable ScopedObjectAccess.
  Thread::Current()->TransitionFromRunnableToSuspended(kNative);
  ScopedObjectAccess soa(Thread::Current());

  t.NewTiming("Image and oat Patching setup");
  // Create the map where we will write the image patches to.
  std::string error_msg;
  std::unique_ptr<MemMap> image(MemMap::MapFile(image_len, PROT_READ | PROT_WRITE, MAP_PRIVATE,
                                                input_image->Fd(), 0,
                                                input_image->GetPath().c_str(),
                                                &error_msg));
  if (image.get() == nullptr) {
    LOG(ERROR) << "unable to map image file " << input_image->GetPath() << " : " << error_msg;
    return false;
  }
  gc::space::ImageSpace* ispc = Runtime::Current()->GetHeap()->GetImageSpace();

  std::unique_ptr<ElfFile> elf(ElfFile::Open(input_oat,
                                             PROT_READ | PROT_WRITE, MAP_PRIVATE, &error_msg));
  if (elf.get() == nullptr) {
    LOG(ERROR) << "unable to open oat file " << input_oat->GetPath() << " : " << error_msg;
    return false;
  }

  bool skip_patching_oat = false;
  MaybePic is_oat_pic = IsOatPic(elf.get());
  if (is_oat_pic >= ERROR_FIRST) {
    // Error logged by IsOatPic
    return false;
  } else if (is_oat_pic == PIC) {
    // Do not need to do ELF-file patching. Create a symlink and skip the ELF patching.
    if (!ReplaceOatFileWithSymlink(input_oat->GetPath(),
                                   output_oat->GetPath(),
                                   output_oat_opened_from_fd,
                                   new_oat_out)) {
      // Errors already logged by above call.
      return false;
    }
    // Don't patch the OAT, since we just symlinked it. Image still needs patching.
    skip_patching_oat = true;
  } else {
    CHECK(is_oat_pic == NOT_PIC);
  }

  PatchOat p(isa, elf.release(), image.release(), ispc->GetLiveBitmap(), ispc->GetMemMap(),
             delta, timings);
  t.NewTiming("Patching files");
  if (!skip_patching_oat && !p.PatchElf()) {
    LOG(ERROR) << "Failed to patch oat file " << input_oat->GetPath();
    return false;
  }
  if (!p.PatchImage()) {
    LOG(ERROR) << "Failed to patch image file " << input_image->GetPath();
    return false;
  }

  t.NewTiming("Writing files");
  if (!skip_patching_oat && !p.WriteElf(output_oat)) {
    LOG(ERROR) << "Failed to write oat file " << input_oat->GetPath();
    return false;
  }
  if (!p.WriteImage(output_image)) {
    LOG(ERROR) << "Failed to write image file " << input_image->GetPath();
    return false;
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

  const std::string& file_path = oat_in->GetFile().GetPath();

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
  TEMP_FAILURE_RETRY(unlink(output_oat_filename.c_str()));

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

void PatchOat::PatchArtFields(const ImageHeader* image_header) {
  const auto& section = image_header->GetImageSection(ImageHeader::kSectionArtFields);
  for (size_t pos = 0; pos < section.Size(); pos += sizeof(ArtField)) {
    auto* src = reinterpret_cast<ArtField*>(heap_->Begin() + section.Offset() + pos);
    auto* dest = RelocatedCopyOf(src);
    dest->SetDeclaringClass(RelocatedAddressOfPointer(src->GetDeclaringClass()));
  }
}

void PatchOat::PatchArtMethods(const ImageHeader* image_header) {
  const auto& section = image_header->GetMethodsSection();
  const size_t pointer_size = InstructionSetPointerSize(isa_);
  size_t method_size = ArtMethod::ObjectSize(pointer_size);
  for (size_t pos = 0; pos < section.Size(); pos += method_size) {
    auto* src = reinterpret_cast<ArtMethod*>(heap_->Begin() + section.Offset() + pos);
    auto* dest = RelocatedCopyOf(src);
    FixupMethod(src, dest);
  }
}

class FixupRootVisitor : public RootVisitor {
 public:
  explicit FixupRootVisitor(const PatchOat* patch_oat) : patch_oat_(patch_oat) {
  }

  void VisitRoots(mirror::Object*** roots, size_t count, const RootInfo& info ATTRIBUTE_UNUSED)
      OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    for (size_t i = 0; i < count; ++i) {
      *roots[i] = patch_oat_->RelocatedAddressOfPointer(*roots[i]);
    }
  }

  void VisitRoots(mirror::CompressedReference<mirror::Object>** roots, size_t count,
                  const RootInfo& info ATTRIBUTE_UNUSED)
      OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
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
  temp_table.ReadFromMemory(image_->Begin() + section.Offset());
  FixupRootVisitor visitor(this);
  temp_table.VisitRoots(&visitor, kVisitRootFlagAllRoots);
}

void PatchOat::PatchDexFileArrays(mirror::ObjectArray<mirror::Object>* img_roots) {
  auto* dex_caches = down_cast<mirror::ObjectArray<mirror::DexCache>*>(
      img_roots->Get(ImageHeader::kDexCaches));
  for (size_t i = 0, count = dex_caches->GetLength(); i < count; ++i) {
    auto* dex_cache = dex_caches->GetWithoutChecks(i);
    auto* fields = dex_cache->GetResolvedFields();
    if (fields != nullptr) {
      CHECK(!fields->IsObjectArray());
      CHECK(fields->IsArrayInstance());
      FixupNativePointerArray(fields);
    }
    auto* methods = dex_cache->GetResolvedMethods();
    if (methods != nullptr) {
      CHECK(!methods->IsObjectArray());
      CHECK(methods->IsArrayInstance());
      FixupNativePointerArray(methods);
    }
  }
}

void PatchOat::FixupNativePointerArray(mirror::PointerArray* object) {
  if (object->IsIntArray()) {
    mirror::IntArray* arr = object->AsIntArray();
    mirror::IntArray* copy_arr = down_cast<mirror::IntArray*>(RelocatedCopyOf(arr));
    for (size_t j = 0, count2 = arr->GetLength(); j < count2; ++j) {
      copy_arr->SetWithoutChecks<false>(
          j, RelocatedAddressOfIntPointer(arr->GetWithoutChecks(j)));
    }
  } else {
    CHECK(object->IsLongArray());
    mirror::LongArray* arr = object->AsLongArray();
    mirror::LongArray* copy_arr = down_cast<mirror::LongArray*>(RelocatedCopyOf(arr));
    for (size_t j = 0, count2 = arr->GetLength(); j < count2; ++j) {
      copy_arr->SetWithoutChecks<false>(
          j, RelocatedAddressOfIntPointer(arr->GetWithoutChecks(j)));
    }
  }
}

bool PatchOat::PatchImage() {
  ImageHeader* image_header = reinterpret_cast<ImageHeader*>(image_->Begin());
  CHECK_GT(image_->Size(), sizeof(ImageHeader));
  // These are the roots from the original file.
  auto* img_roots = image_header->GetImageRoots();
  image_header->RelocateImage(delta_);

  PatchArtFields(image_header);
  PatchArtMethods(image_header);
  PatchInternedStrings(image_header);
  // Patch dex file int/long arrays which point to ArtFields.
  PatchDexFileArrays(img_roots);

  VisitObject(img_roots);
  if (!image_header->IsValid()) {
    LOG(ERROR) << "reloction renders image header invalid";
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

bool PatchOat::InHeap(mirror::Object* o) {
  uintptr_t begin = reinterpret_cast<uintptr_t>(heap_->Begin());
  uintptr_t end = reinterpret_cast<uintptr_t>(heap_->End());
  uintptr_t obj = reinterpret_cast<uintptr_t>(o);
  return o == nullptr || (begin <= obj && obj < end);
}

void PatchOat::PatchVisitor::operator() (mirror::Object* obj, MemberOffset off,
                                         bool is_static_unused ATTRIBUTE_UNUSED) const {
  mirror::Object* referent = obj->GetFieldObject<mirror::Object, kVerifyNone>(off);
  DCHECK(patcher_->InHeap(referent)) << "Referent is not in the heap.";
  mirror::Object* moved_object = patcher_->RelocatedAddressOfPointer(referent);
  copy_->SetFieldObjectWithoutWriteBarrier<false, true, kVerifyNone>(off, moved_object);
}

void PatchOat::PatchVisitor::operator() (mirror::Class* cls ATTRIBUTE_UNUSED,
                                         mirror::Reference* ref) const {
  MemberOffset off = mirror::Reference::ReferentOffset();
  mirror::Object* referent = ref->GetReferent();
  DCHECK(patcher_->InHeap(referent)) << "Referent is not in the heap.";
  mirror::Object* moved_object = patcher_->RelocatedAddressOfPointer(referent);
  copy_->SetFieldObjectWithoutWriteBarrier<false, true, kVerifyNone>(off, moved_object);
}

const OatHeader* PatchOat::GetOatHeader(const ElfFile* elf_file) {
  if (elf_file->Is64Bit()) {
    return GetOatHeader<ElfFileImpl64>(elf_file->GetImpl64());
  } else {
    return GetOatHeader<ElfFileImpl32>(elf_file->GetImpl32());
  }
}

template <typename ElfFileImpl>
const OatHeader* PatchOat::GetOatHeader(const ElfFileImpl* elf_file) {
  auto rodata_sec = elf_file->FindSectionByName(".rodata");
  if (rodata_sec == nullptr) {
    return nullptr;
  }

  OatHeader* oat_header = reinterpret_cast<OatHeader*>(elf_file->Begin() + rodata_sec->sh_offset);
  return oat_header;
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
  object->VisitReferences<true, kVerifyNone>(visitor, visitor);
  if (object->IsClass<kVerifyNone>()) {
    auto* klass = object->AsClass();
    auto* copy_klass = down_cast<mirror::Class*>(copy);
    copy_klass->SetSFieldsUnchecked(RelocatedAddressOfPointer(klass->GetSFields()));
    copy_klass->SetIFieldsUnchecked(RelocatedAddressOfPointer(klass->GetIFields()));
    copy_klass->SetDirectMethodsPtrUnchecked(
        RelocatedAddressOfPointer(klass->GetDirectMethodsPtr()));
    copy_klass->SetVirtualMethodsPtr(RelocatedAddressOfPointer(klass->GetVirtualMethodsPtr()));
    auto* vtable = klass->GetVTable();
    if (vtable != nullptr) {
      FixupNativePointerArray(vtable);
    }
    auto* iftable = klass->GetIfTable();
    if (iftable != nullptr) {
      for (int32_t i = 0; i < klass->GetIfTableCount(); ++i) {
        if (iftable->GetMethodArrayCount(i) > 0) {
          auto* method_array = iftable->GetMethodArray(i);
          CHECK(method_array != nullptr);
          FixupNativePointerArray(method_array);
        }
      }
    }
    if (klass->ShouldHaveEmbeddedImtAndVTable()) {
      const size_t pointer_size = InstructionSetPointerSize(isa_);
      for (int32_t i = 0; i < klass->GetEmbeddedVTableLength(); ++i) {
        copy_klass->SetEmbeddedVTableEntryUnchecked(i, RelocatedAddressOfPointer(
            klass->GetEmbeddedVTableEntry(i, pointer_size)), pointer_size);
      }
      for (size_t i = 0; i < mirror::Class::kImtSize; ++i) {
        copy_klass->SetEmbeddedImTableEntry(i, RelocatedAddressOfPointer(
            klass->GetEmbeddedImTableEntry(i, pointer_size)), pointer_size);
      }
    }
  }
  if (object->GetClass() == mirror::Method::StaticClass() ||
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
  copy->SetDexCacheResolvedMethods(RelocatedAddressOfPointer(object->GetDexCacheResolvedMethods()));
  copy->SetDexCacheResolvedTypes(RelocatedAddressOfPointer(object->GetDexCacheResolvedTypes()));
  copy->SetEntryPointFromQuickCompiledCodePtrSize(RelocatedAddressOfPointer(
      object->GetEntryPointFromQuickCompiledCodePtrSize(pointer_size)), pointer_size);
  copy->SetEntryPointFromInterpreterPtrSize(RelocatedAddressOfPointer(
      object->GetEntryPointFromInterpreterPtrSize(pointer_size)), pointer_size);
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
    LOG(ERROR) << "Elf file " << oat_file->GetFile().GetPath() << " has an invalid oat header";
    return false;
  }
  oat_header->RelocateOat(delta_);
  return true;
}

bool PatchOat::PatchElf() {
  if (oat_file_->Is64Bit())
    return PatchElf<ElfFileImpl64>(oat_file_->GetImpl64());
  else
    return PatchElf<ElfFileImpl32>(oat_file_->GetImpl32());
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
  UsageError("  --output-image-fd=<file-descriptor>: Specifies the file-descriptor to write the");
  UsageError("      the patched image file to.");
  UsageError("");
  UsageError("  --orig-base-offset=<original-base-offset>: Specify the base offset the input file");
  UsageError("      was compiled with. This is needed if one is specifying a --base-offset");
  UsageError("");
  UsageError("  --base-offset=<new-base-offset>: Specify the base offset we will repatch the");
  UsageError("      given files to use. This requires that --orig-base-offset is also given.");
  UsageError("");
  UsageError("  --base-offset-delta=<delta>: Specify the amount to change the old base-offset by.");
  UsageError("      This value may be negative.");
  UsageError("");
  UsageError("  --patched-image-file=<file.art>: Use the same patch delta as was used to patch");
  UsageError("      the given image file.");
  UsageError("");
  UsageError("  --patched-image-location=<file.art>: Use the same patch delta as was used to");
  UsageError("      patch the given image location. If used one must also specify the");
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
        TEMP_FAILURE_RETRY(unlink(name));
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
  int output_image_fd = -1;
  bool have_output_image = false;
  uintptr_t base_offset = 0;
  bool base_offset_set = false;
  uintptr_t orig_base_offset = 0;
  bool orig_base_offset_set = false;
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
      if (have_output_image) {
        Usage("Only one of --output-image-file, and --output-image-fd may be used.");
      }
      have_output_image = true;
      output_image_filename = option.substr(strlen("--output-image-file=")).data();
    } else if (option.starts_with("--output-image-fd=")) {
      if (have_output_image) {
        Usage("Only one of --output-image-file, and --output-image-fd may be used.");
      }
      have_output_image = true;
      const char* image_fd_str = option.substr(strlen("--output-image-fd=")).data();
      if (!ParseInt(image_fd_str, &output_image_fd)) {
        Usage("Failed to parse --output-image-fd argument '%s' as an integer", image_fd_str);
      }
      if (output_image_fd < 0) {
        Usage("--output-image-fd pass a negative value %d", output_image_fd);
      }
    } else if (option.starts_with("--orig-base-offset=")) {
      const char* orig_base_offset_str = option.substr(strlen("--orig-base-offset=")).data();
      orig_base_offset_set = true;
      if (!ParseUint(orig_base_offset_str, &orig_base_offset)) {
        Usage("Failed to parse --orig-base-offset argument '%s' as an uintptr_t",
              orig_base_offset_str);
      }
    } else if (option.starts_with("--base-offset=")) {
      const char* base_offset_str = option.substr(strlen("--base-offset=")).data();
      base_offset_set = true;
      if (!ParseUint(base_offset_str, &base_offset)) {
        Usage("Failed to parse --base-offset argument '%s' as an uintptr_t", base_offset_str);
      }
    } else if (option.starts_with("--base-offset-delta=")) {
      const char* base_delta_str = option.substr(strlen("--base-offset-delta=")).data();
      base_delta_set = true;
      if (!ParseInt(base_delta_str, &base_delta)) {
        Usage("Failed to parse --base-offset-delta argument '%s' as an off_t", base_delta_str);
      }
    } else if (option.starts_with("--patched-image-location=")) {
      patched_image_location = option.substr(strlen("--patched-image-location=")).data();
    } else if (option.starts_with("--patched-image-file=")) {
      patched_image_filename = option.substr(strlen("--patched-image-file=")).data();
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

  {
    // Only 1 of these may be set.
    uint32_t cnt = 0;
    cnt += (base_delta_set) ? 1 : 0;
    cnt += (base_offset_set && orig_base_offset_set) ? 1 : 0;
    cnt += (!patched_image_filename.empty()) ? 1 : 0;
    cnt += (!patched_image_location.empty()) ? 1 : 0;
    if (cnt > 1) {
      Usage("Only one of --base-offset/--orig-base-offset, --base-offset-delta, "
            "--patched-image-filename or --patched-image-location may be used.");
    } else if (cnt == 0) {
      Usage("Must specify --base-offset-delta, --base-offset and --orig-base-offset, "
            "--patched-image-location or --patched-image-file");
    }
  }

  if (have_input_oat != have_output_oat) {
    Usage("Either both input and output oat must be supplied or niether must be.");
  }

  if ((!input_image_location.empty()) != have_output_image) {
    Usage("Either both input and output image must be supplied or niether must be.");
  }

  // We know we have both the input and output so rename for clarity.
  bool have_image_files = have_output_image;
  bool have_oat_files = have_output_oat;

  if (!have_oat_files && !have_image_files) {
    Usage("Must be patching either an oat or an image file or both.");
  }

  if (!have_oat_files && !isa_set) {
    Usage("Must include ISA if patching an image file without an oat file.");
  }

  if (!input_oat_location.empty()) {
    if (!isa_set) {
      Usage("specifying a location requires specifying an instruction set");
    }
    if (!LocationToFilename(input_oat_location, isa, &input_oat_filename)) {
      Usage("Unable to find filename for input oat location %s", input_oat_location.c_str());
    }
    if (debug) {
      LOG(INFO) << "Using input-oat-file " << input_oat_filename;
    }
  }
  if (!patched_image_location.empty()) {
    if (!isa_set) {
      Usage("specifying a location requires specifying an instruction set");
    }
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
  }

  if (!base_delta_set) {
    if (orig_base_offset_set && base_offset_set) {
      base_delta_set = true;
      base_delta = base_offset - orig_base_offset;
    } else if (!patched_image_filename.empty()) {
      base_delta_set = true;
      std::string error_msg;
      if (!ReadBaseDelta(patched_image_filename.c_str(), &base_delta, &error_msg)) {
        Usage(error_msg.c_str(), patched_image_filename.c_str());
      }
    } else {
      if (base_offset_set) {
        Usage("Unable to determine original base offset.");
      } else {
        Usage("Must supply a desired new offset or delta.");
      }
    }
  }

  if (!IsAligned<kPageSize>(base_delta)) {
    Usage("Base offset/delta must be alligned to a pagesize (0x%08x) boundary.", kPageSize);
  }

  // Do we need to cleanup output files if we fail?
  bool new_image_out = false;
  bool new_oat_out = false;

  std::unique_ptr<File> input_oat;
  std::unique_ptr<File> output_oat;
  std::unique_ptr<File> output_image;

  if (have_image_files) {
    CHECK(!input_image_location.empty());

    if (output_image_fd != -1) {
      if (output_image_filename.empty()) {
        output_image_filename = "output-image-file";
      }
      output_image.reset(new File(output_image_fd, output_image_filename, true));
    } else {
      CHECK(!output_image_filename.empty());
      output_image.reset(CreateOrOpen(output_image_filename.c_str(), &new_image_out));
    }
  } else {
    CHECK(output_image_filename.empty() && output_image_fd == -1 && input_image_location.empty());
  }

  if (have_oat_files) {
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
      }
    } else {
      CHECK(!input_oat_filename.empty());
      input_oat.reset(OS::OpenFileForReading(input_oat_filename.c_str()));
      if (input_oat == nullptr) {
        int err = errno;
        LOG(ERROR) << "Failed to open input oat file " << input_oat_filename
                   << ": " << strerror(err) << "(" << err << ")";
      }
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
  }

  // TODO: get rid of this.
  auto cleanup = [&output_image_filename, &output_oat_filename,
                  &new_oat_out, &new_image_out, &timings, &dump_timings](bool success) {
    timings.EndTiming();
    if (!success) {
      if (new_oat_out) {
        CHECK(!output_oat_filename.empty());
        TEMP_FAILURE_RETRY(unlink(output_oat_filename.c_str()));
      }
      if (new_image_out) {
        CHECK(!output_image_filename.empty());
        TEMP_FAILURE_RETRY(unlink(output_image_filename.c_str()));
      }
    }
    if (dump_timings) {
      LOG(INFO) << Dumpable<TimingLogger>(timings);
    }

    if (kIsDebugBuild) {
      LOG(INFO) << "Cleaning up.. success? " << success;
    }
  };

  if (have_oat_files && (input_oat.get() == nullptr || output_oat.get() == nullptr)) {
    LOG(ERROR) << "Failed to open input/output oat files";
    cleanup(false);
    return EXIT_FAILURE;
  } else if (have_image_files && output_image.get() == nullptr) {
    LOG(ERROR) << "Failed to open output image file";
    cleanup(false);
    return EXIT_FAILURE;
  }

  if (debug) {
    LOG(INFO) << "moving offset by " << base_delta
              << " (0x" << std::hex << base_delta << ") bytes or "
              << std::dec << (base_delta/kPageSize) << " pages.";
  }

  // TODO: is it going to be promatic to unlink a file that was flock-ed?
  ScopedFlock output_oat_lock;
  if (lock_output) {
    std::string error_msg;
    if (have_oat_files && !output_oat_lock.Init(output_oat.get(), &error_msg)) {
      LOG(ERROR) << "Unable to lock output oat " << output_image->GetPath() << ": " << error_msg;
      cleanup(false);
      return EXIT_FAILURE;
    }
  }

  bool ret;
  if (have_image_files && have_oat_files) {
    TimingLogger::ScopedTiming pt("patch image and oat", &timings);
    ret = PatchOat::Patch(input_oat.get(), input_image_location, base_delta,
                          output_oat.get(), output_image.get(), isa, &timings,
                          output_oat_fd >= 0,  // was it opened from FD?
                          new_oat_out);
    // The order here doesn't matter. If the first one is successfully saved and the second one
    // erased, ImageSpace will still detect a problem and not use the files.
    ret = ret && FinishFile(output_image.get(), ret);
    ret = ret && FinishFile(output_oat.get(), ret);
  } else if (have_oat_files) {
    TimingLogger::ScopedTiming pt("patch oat", &timings);
    ret = PatchOat::Patch(input_oat.get(), base_delta, output_oat.get(), &timings,
                          output_oat_fd >= 0,  // was it opened from FD?
                          new_oat_out);
    ret = ret && FinishFile(output_oat.get(), ret);
  } else if (have_image_files) {
    TimingLogger::ScopedTiming pt("patch image", &timings);
    ret = PatchOat::Patch(input_image_location, base_delta, output_image.get(), isa, &timings);
    ret = ret && FinishFile(output_image.get(), ret);
  } else {
    CHECK(false);
    ret = true;
  }

  if (kIsDebugBuild) {
    LOG(INFO) << "Exiting with return ... " << ret;
  }
  cleanup(ret);
  return (ret) ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace art

int main(int argc, char **argv) {
  return art::patchoat(argc, argv);
}
