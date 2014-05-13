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
#include "space-inl.h"
#include "utils.h"

namespace art {
namespace gc {
namespace space {

Atomic<uint32_t> ImageSpace::bitmap_index_(0);

ImageSpace::ImageSpace(const std::string& image_filename, const char* image_location,
                       MemMap* mem_map, accounting::ContinuousSpaceBitmap* live_bitmap)
    : MemMapSpace(image_filename, mem_map, mem_map->Begin(), mem_map->End(), mem_map->End(),
                  kGcRetentionPolicyNeverCollect),
      image_location_(image_location) {
  DCHECK(live_bitmap != nullptr);
  live_bitmap_.reset(live_bitmap);
}

static bool GenerateImage(const std::string& image_filename, std::string* error_msg) {
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
  image_option_string += image_filename;
  arg_vector.push_back(image_option_string);

  arg_vector.push_back("--runtime-arg");
  arg_vector.push_back("-Xms64m");

  arg_vector.push_back("--runtime-arg");
  arg_vector.push_back("-Xmx64m");


  for (size_t i = 0; i < boot_class_path.size(); i++) {
    arg_vector.push_back(std::string("--dex-file=") + boot_class_path[i]);
  }

  std::string oat_file_option_string("--oat-file=");
  oat_file_option_string += image_filename;
  oat_file_option_string.erase(oat_file_option_string.size() - 3);
  oat_file_option_string += "oat";
  arg_vector.push_back(oat_file_option_string);

  Runtime::Current()->AddCurrentRuntimeFeaturesAsDex2OatArguments(&arg_vector);

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

bool ImageSpace::FindImageFilename(const char* image_location,
                                   const InstructionSet image_isa,
                                   std::string* image_filename,
                                   bool *is_system) {
  if (OS::FileExists(image_location)) {
    *image_filename = image_location;
    *is_system = true;
    return true;
  }

  const std::string dalvik_cache = GetDalvikCacheOrDie(GetInstructionSetString(image_isa));

  // Always set output location even if it does not exist,
  // so that the caller knows where to create the image.
  *image_filename = GetDalvikCacheFilenameOrDie(image_location, dalvik_cache.c_str());
  *is_system = false;
  return OS::FileExists(image_filename->c_str());
}

ImageHeader* ImageSpace::ReadImageHeaderOrDie(const char* image_location,
                                              const InstructionSet image_isa) {
  std::string image_filename;
  bool is_system = false;
  if (FindImageFilename(image_location, image_isa, &image_filename, &is_system)) {
    UniquePtr<File> image_file(OS::OpenFileForReading(image_filename.c_str()));
    UniquePtr<ImageHeader> image_header(new ImageHeader);
    const bool success = image_file->ReadFully(image_header.get(), sizeof(ImageHeader));
    if (!success || !image_header->IsValid()) {
      LOG(FATAL) << "Invalid Image header for: " << image_filename;
      return nullptr;
    }

    return image_header.release();
  }

  LOG(FATAL) << "Unable to find image file for: " << image_location;
  return nullptr;
}

ImageSpace* ImageSpace::Create(const char* image_location,
                               const InstructionSet image_isa) {
  std::string image_filename;
  std::string error_msg;
  bool is_system = false;
  if (FindImageFilename(image_location, image_isa, &image_filename, &is_system)) {
    ImageSpace* space = ImageSpace::Init(image_filename.c_str(), image_location, !is_system,
                                         &error_msg);
    if (space != nullptr) {
      return space;
    }

    // If the /system file exists, it should be up-to-date, don't try to generate it.
    // If it's not the /system file, log a warning and fall through to GenerateImage.
    if (is_system) {
      LOG(FATAL) << "Failed to load image '" << image_filename << "': " << error_msg;
      return nullptr;
    } else {
      LOG(WARNING) << error_msg;
    }
  }

  CHECK(GenerateImage(image_filename, &error_msg))
      << "Failed to generate image '" << image_filename << "': " << error_msg;
  ImageSpace* space = ImageSpace::Init(image_filename.c_str(), image_location, true, &error_msg);
  if (space == nullptr) {
    LOG(FATAL) << "Failed to load image '" << image_filename << "': " << error_msg;
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
    if (kUseBakerOrBrooksReadBarrier) {
      obj->AssertReadBarrierPointer();
    }
    current += RoundUp(obj->SizeOf(), kObjectAlignment);
  }
}

ImageSpace* ImageSpace::Init(const char* image_filename, const char* image_location,
                             bool validate_oat_file, std::string* error_msg) {
  CHECK(image_filename != nullptr);
  CHECK(image_location != nullptr);

  uint64_t start_time = 0;
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    start_time = NanoTime();
    LOG(INFO) << "ImageSpace::Init entering image_filename=" << image_filename;
  }

  UniquePtr<File> file(OS::OpenFileForReading(image_filename));
  if (file.get() == NULL) {
    *error_msg = StringPrintf("Failed to open '%s'", image_filename);
    return nullptr;
  }
  ImageHeader image_header;
  bool success = file->ReadFully(&image_header, sizeof(image_header));
  if (!success || !image_header.IsValid()) {
    *error_msg = StringPrintf("Invalid image header in '%s'", image_filename);
    return nullptr;
  }

  // Note: The image header is part of the image due to mmap page alignment required of offset.
  UniquePtr<MemMap> map(MemMap::MapFileAtAddress(image_header.GetImageBegin(),
                                                 image_header.GetImageSize(),
                                                 PROT_READ | PROT_WRITE,
                                                 MAP_PRIVATE,
                                                 file->Fd(),
                                                 0,
                                                 false,
                                                 image_filename,
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
                                                       image_filename,
                                                       error_msg));
  if (image_map.get() == nullptr) {
    *error_msg = StringPrintf("Failed to map image bitmap: %s", error_msg->c_str());
    return nullptr;
  }
  uint32_t bitmap_index = bitmap_index_.FetchAndAdd(1);
  std::string bitmap_name(StringPrintf("imagespace %s live-bitmap %u", image_filename,
                                       bitmap_index));
  UniquePtr<accounting::ContinuousSpaceBitmap> bitmap(
      accounting::ContinuousSpaceBitmap::CreateFromMemMap(bitmap_name, image_map.release(),
                                                          reinterpret_cast<byte*>(map->Begin()),
                                                          map->Size()));
  if (bitmap.get() == nullptr) {
    *error_msg = StringPrintf("Could not create bitmap '%s'", bitmap_name.c_str());
    return nullptr;
  }

  UniquePtr<ImageSpace> space(new ImageSpace(image_filename, image_location,
                                             map.release(), bitmap.release()));
  if (kIsDebugBuild) {
    space->VerifyImageAllocations();
  }

  space->oat_file_.reset(space->OpenOatFile(image_filename, error_msg));
  if (space->oat_file_.get() == nullptr) {
    DCHECK(!error_msg->empty());
    return nullptr;
  }

  if (validate_oat_file && !space->ValidateOatFile(error_msg)) {
    DCHECK(!error_msg->empty());
    return nullptr;
  }

  Runtime* runtime = Runtime::Current();
  runtime->SetInstructionSet(space->oat_file_->GetOatHeader().GetInstructionSet());

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
