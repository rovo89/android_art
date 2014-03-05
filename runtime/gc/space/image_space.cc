/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "image_space.h"

#include "base/stl_util.h"
#include "base/unix_file/fd_file.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "mirror/art_method.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "oat_file.h"
#include "os.h"
#include "runtime.h"
#include "space-inl.h"
#include "utils.h"

namespace art {
namespace gc {
namespace space {

Atomic<uint32_t> ImageSpace::bitmap_index_(0);

ImageSpace::ImageSpace(const std::string& name, MemMap* mem_map,
                       accounting::SpaceBitmap* live_bitmap)
    : MemMapSpace(name, mem_map, mem_map->Begin(), mem_map->End(), mem_map->End(),
                  kGcRetentionPolicyNeverCollect) {
  DCHECK(live_bitmap != nullptr);
  live_bitmap_.reset(live_bitmap);
}

static bool GenerateImage(const std::string& image_file_name, std::string* error_msg) {
  const std::string boot_class_path_string(Runtime::Current()->GetBootClassPathString());
  std::vector<std::string> boot_class_path;
  Split(boot_class_path_string, ':', boot_class_path);
  if (boot_class_path.empty()) {
    *error_msg = "Failed to generate image because no boot class path specified";
    return false;
  }

  std::vector<std::string> arg_vector;

  std::string dex2oat(GetAndroidRoot());
  dex2oat += (kIsDebugBuild ? "/bin/dex2oatd" : "/bin/dex2oat");
  arg_vector.push_back(dex2oat);

  std::string image_option_string("--image=");
  image_option_string += image_file_name;
  arg_vector.push_back(image_option_string);

  arg_vector.push_back("--runtime-arg");
  arg_vector.push_back("-Xms64m");

  arg_vector.push_back("--runtime-arg");
  arg_vector.push_back("-Xmx64m");

  for (size_t i = 0; i < boot_class_path.size(); i++) {
    arg_vector.push_back(std::string("--dex-file=") + boot_class_path[i]);
  }

  std::string oat_file_option_string("--oat-file=");
  oat_file_option_string += image_file_name;
  oat_file_option_string.erase(oat_file_option_string.size() - 3);
  oat_file_option_string += "oat";
  arg_vector.push_back(oat_file_option_string);

  arg_vector.push_back(StringPrintf("--base=0x%x", ART_BASE_ADDRESS));

  if (kIsTargetBuild) {
    arg_vector.push_back("--image-classes-zip=/system/framework/framework.jar");
    arg_vector.push_back("--image-classes=preloaded-classes");
  } else {
    arg_vector.push_back("--host");
  }

  const std::vector<std::string>& compiler_options = Runtime::Current()->GetImageCompilerOptions();
  for (size_t i = 0; i < compiler_options.size(); ++i) {
    arg_vector.push_back(compiler_options[i].c_str());
  }

  std::string command_line(Join(arg_vector, ' '));
  LOG(INFO) << "GenerateImage: " << command_line;
  return Exec(arg_vector, error_msg);
}

ImageSpace* ImageSpace::Create(const char* original_image_file_name) {
  if (OS::FileExists(original_image_file_name)) {
    // If the /system file exists, it should be up-to-date, don't try to generate
    std::string error_msg;
    ImageSpace* space = ImageSpace::Init(original_image_file_name, false, &error_msg);
    if (space == nullptr) {
      LOG(FATAL) << "Failed to load image '" << original_image_file_name << "': " << error_msg;
    }
    return space;
  }
  // If the /system file didn't exist, we need to use one from the dalvik-cache.
  // If the cache file exists, try to open, but if it fails, regenerate.
  // If it does not exist, generate.
  std::string image_file_name(GetDalvikCacheFilenameOrDie(original_image_file_name));
  std::string error_msg;
  if (OS::FileExists(image_file_name.c_str())) {
    space::ImageSpace* image_space = ImageSpace::Init(image_file_name.c_str(), true, &error_msg);
    if (image_space != nullptr) {
      return image_space;
    } else {
      LOG(WARNING) << error_msg;
    }
  }
  CHECK(GenerateImage(image_file_name, &error_msg))
      << "Failed to generate image '" << image_file_name << "': " << error_msg;
  ImageSpace* space = ImageSpace::Init(image_file_name.c_str(), true, &error_msg);
  if (space == nullptr) {
    LOG(FATAL) << "Failed to load image '" << original_image_file_name << "': " << error_msg;
  }
  return space;
}

void ImageSpace::VerifyImageAllocations() {
  byte* current = Begin() + RoundUp(sizeof(ImageHeader), kObjectAlignment);
  while (current < End()) {
    DCHECK_ALIGNED(current, kObjectAlignment);
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(current);
    CHECK(live_bitmap_->Test(obj));
    CHECK(obj->GetClass() != nullptr) << "Image object at address " << obj << " has null class";
    if (kUseBrooksPointer) {
      CHECK(obj->GetBrooksPointer() == obj)
          << "Bad Brooks pointer: obj=" << reinterpret_cast<void*>(obj)
          << " brooks_ptr=" << reinterpret_cast<void*>(obj->GetBrooksPointer());
    }
    current += RoundUp(obj->SizeOf(), kObjectAlignment);
  }
}

ImageSpace* ImageSpace::Init(const char* image_file_name, bool validate_oat_file,
                             std::string* error_msg) {
  CHECK(image_file_name != nullptr);

  uint64_t start_time = 0;
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    start_time = NanoTime();
    LOG(INFO) << "ImageSpace::Init entering image_file_name=" << image_file_name;
  }

  UniquePtr<File> file(OS::OpenFileForReading(image_file_name));
  if (file.get() == NULL) {
    *error_msg = StringPrintf("Failed to open '%s'", image_file_name);
    return nullptr;
  }
  ImageHeader image_header;
  bool success = file->ReadFully(&image_header, sizeof(image_header));
  if (!success || !image_header.IsValid()) {
    *error_msg = StringPrintf("Invalid image header in '%s'", image_file_name);
    return nullptr;
  }

  // Note: The image header is part of the image due to mmap page alignment required of offset.
  UniquePtr<MemMap> map(MemMap::MapFileAtAddress(image_header.GetImageBegin(),
                                                 image_header.GetImageSize(),
                                                 PROT_READ | PROT_WRITE,
                                                 MAP_PRIVATE | MAP_FIXED,
                                                 file->Fd(),
                                                 0,
                                                 false,
                                                 image_file_name,
                                                 error_msg));
  if (map.get() == NULL) {
    DCHECK(!error_msg->empty());
    return nullptr;
  }
  CHECK_EQ(image_header.GetImageBegin(), map->Begin());
  DCHECK_EQ(0, memcmp(&image_header, map->Begin(), sizeof(ImageHeader)));

  UniquePtr<MemMap> image_map(MemMap::MapFileAtAddress(nullptr, image_header.GetImageBitmapSize(),
                                                       PROT_READ, MAP_PRIVATE,
                                                       file->Fd(), image_header.GetBitmapOffset(),
                                                       false,
                                                       image_file_name,
                                                       error_msg));
  if (image_map.get() == nullptr) {
    *error_msg = StringPrintf("Failed to map image bitmap: %s", error_msg->c_str());
    return nullptr;
  }
  uint32_t bitmap_index = bitmap_index_.FetchAndAdd(1);
  std::string bitmap_name(StringPrintf("imagespace %s live-bitmap %u", image_file_name,
                                       bitmap_index));
  UniquePtr<accounting::SpaceBitmap> bitmap(
      accounting::SpaceBitmap::CreateFromMemMap(bitmap_name, image_map.release(),
                                                reinterpret_cast<byte*>(map->Begin()),
                                                map->Size()));
  if (bitmap.get() == nullptr) {
    *error_msg = StringPrintf("Could not create bitmap '%s'", bitmap_name.c_str());
    return nullptr;
  }

  Runtime* runtime = Runtime::Current();
  mirror::Object* resolution_method = image_header.GetImageRoot(ImageHeader::kResolutionMethod);
  runtime->SetResolutionMethod(down_cast<mirror::ArtMethod*>(resolution_method));
  mirror::Object* imt_conflict_method = image_header.GetImageRoot(ImageHeader::kImtConflictMethod);
  runtime->SetImtConflictMethod(down_cast<mirror::ArtMethod*>(imt_conflict_method));
  mirror::Object* default_imt = image_header.GetImageRoot(ImageHeader::kDefaultImt);
  runtime->SetDefaultImt(down_cast<mirror::ObjectArray<mirror::ArtMethod>*>(default_imt));

  mirror::Object* callee_save_method = image_header.GetImageRoot(ImageHeader::kCalleeSaveMethod);
  runtime->SetCalleeSaveMethod(down_cast<mirror::ArtMethod*>(callee_save_method), Runtime::kSaveAll);
  callee_save_method = image_header.GetImageRoot(ImageHeader::kRefsOnlySaveMethod);
  runtime->SetCalleeSaveMethod(down_cast<mirror::ArtMethod*>(callee_save_method), Runtime::kRefsOnly);
  callee_save_method = image_header.GetImageRoot(ImageHeader::kRefsAndArgsSaveMethod);
  runtime->SetCalleeSaveMethod(down_cast<mirror::ArtMethod*>(callee_save_method), Runtime::kRefsAndArgs);

  UniquePtr<ImageSpace> space(new ImageSpace(image_file_name, map.release(), bitmap.release()));
  if (kIsDebugBuild) {
    space->VerifyImageAllocations();
  }

  space->oat_file_.reset(space->OpenOatFile(image_file_name, error_msg));
  if (space->oat_file_.get() == nullptr) {
    DCHECK(!error_msg->empty());
    return nullptr;
  }

  if (validate_oat_file && !space->ValidateOatFile(error_msg)) {
    DCHECK(!error_msg->empty());
    return nullptr;
  }

  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "ImageSpace::Init exiting (" << PrettyDuration(NanoTime() - start_time)
             << ") " << *space.get();
  }
  return space.release();
}

OatFile* ImageSpace::OpenOatFile(const char* image_path, std::string* error_msg) const {
  const ImageHeader& image_header = GetImageHeader();
  std::string oat_filename = ImageHeader::GetOatLocationFromImageLocation(image_path);

  OatFile* oat_file = OatFile::Open(oat_filename, oat_filename, image_header.GetOatDataBegin(),
                                    !Runtime::Current()->IsCompiler(), error_msg);
  if (oat_file == NULL) {
    *error_msg = StringPrintf("Failed to open oat file '%s' referenced from image %s: %s",
                              oat_filename.c_str(), GetName(), error_msg->c_str());
    return nullptr;
  }
  uint32_t oat_checksum = oat_file->GetOatHeader().GetChecksum();
  uint32_t image_oat_checksum = image_header.GetOatChecksum();
  if (oat_checksum != image_oat_checksum) {
    *error_msg = StringPrintf("Failed to match oat file checksum 0x%x to expected oat checksum 0x%x"
                              " in image %s", oat_checksum, image_oat_checksum, GetName());
    return nullptr;
  }
  return oat_file;
}

bool ImageSpace::ValidateOatFile(std::string* error_msg) const {
  CHECK(oat_file_.get() != NULL);
  for (const OatFile::OatDexFile* oat_dex_file : oat_file_->GetOatDexFiles()) {
    const std::string& dex_file_location = oat_dex_file->GetDexFileLocation();
    uint32_t dex_file_location_checksum;
    if (!DexFile::GetChecksum(dex_file_location.c_str(), &dex_file_location_checksum, error_msg)) {
      *error_msg = StringPrintf("Failed to get checksum of dex file '%s' referenced by image %s: "
                                "%s", dex_file_location.c_str(), GetName(), error_msg->c_str());
      return false;
    }
    if (dex_file_location_checksum != oat_dex_file->GetDexFileLocationChecksum()) {
      *error_msg = StringPrintf("ValidateOatFile found checksum mismatch between oat file '%s' and "
                                "dex file '%s' (0x%x != 0x%x)",
                                oat_file_->GetLocation().c_str(), dex_file_location.c_str(),
                                oat_dex_file->GetDexFileLocationChecksum(),
                                dex_file_location_checksum);
      return false;
    }
  }
  return true;
}

OatFile* ImageSpace::ReleaseOatFile() {
  CHECK(oat_file_.get() != NULL);
  return oat_file_.release();
}

void ImageSpace::Dump(std::ostream& os) const {
  os << GetType()
      << " begin=" << reinterpret_cast<void*>(Begin())
      << ",end=" << reinterpret_cast<void*>(End())
      << ",size=" << PrettySize(Size())
      << ",name=\"" << GetName() << "\"]";
}

}  // namespace space
}  // namespace gc
}  // namespace art
