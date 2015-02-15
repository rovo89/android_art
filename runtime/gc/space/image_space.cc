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

#include <dirent.h>
#include <sys/types.h>

#include <random>

#include "base/stl_util.h"
#include "base/unix_file/fd_file.h"
#include "base/scoped_flock.h"
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

static int32_t ChooseRelocationOffsetDelta(int32_t min_delta, int32_t max_delta) {
  CHECK_ALIGNED(min_delta, kPageSize);
  CHECK_ALIGNED(max_delta, kPageSize);
  CHECK_LT(min_delta, max_delta);

  std::default_random_engine generator;
  generator.seed(NanoTime() * getpid());
  std::uniform_int_distribution<int32_t> distribution(min_delta, max_delta);
  int32_t r = distribution(generator);
  if (r % 2 == 0) {
    r = RoundUp(r, kPageSize);
  } else {
    r = RoundDown(r, kPageSize);
  }
  CHECK_LE(min_delta, r);
  CHECK_GE(max_delta, r);
  CHECK_ALIGNED(r, kPageSize);
  return r;
}

// We are relocating or generating the core image. We should get rid of everything. It is all
// out-of-date. We also don't really care if this fails since it is just a convienence.
// Adapted from prune_dex_cache(const char* subdir) in frameworks/native/cmds/installd/commands.c
// Note this should only be used during first boot.
static void RealPruneDexCache(const std::string& cache_dir_path);
static void PruneDexCache(InstructionSet isa) {
  CHECK_NE(isa, kNone);
  // Prune the base /data/dalvik-cache
  RealPruneDexCache(GetDalvikCacheOrDie(".", false));
  // prune /data/dalvik-cache/<isa>
  RealPruneDexCache(GetDalvikCacheOrDie(GetInstructionSetString(isa), false));
}
static void RealPruneDexCache(const std::string& cache_dir_path) {
  if (!OS::DirectoryExists(cache_dir_path.c_str())) {
    return;
  }
  DIR* cache_dir = opendir(cache_dir_path.c_str());
  if (cache_dir == nullptr) {
    PLOG(WARNING) << "Unable to open " << cache_dir_path << " to delete it's contents";
    return;
  }

  for (struct dirent* de = readdir(cache_dir); de != nullptr; de = readdir(cache_dir)) {
    const char* name = de->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }
    // We only want to delete regular files.
    if (de->d_type != DT_REG) {
      if (de->d_type != DT_DIR) {
        // We do expect some directories (namely the <isa> for pruning the base dalvik-cache).
        LOG(WARNING) << "Unexpected file type of " << std::hex << de->d_type << " encountered.";
      }
      continue;
    }
    std::string cache_file(cache_dir_path);
    cache_file += '/';
    cache_file += name;
    if (TEMP_FAILURE_RETRY(unlink(cache_file.c_str())) != 0) {
      PLOG(ERROR) << "Unable to unlink " << cache_file;
      continue;
    }
  }
  CHECK_EQ(0, TEMP_FAILURE_RETRY(closedir(cache_dir))) << "Unable to close directory.";
}

static bool GenerateImage(const std::string& image_filename, InstructionSet image_isa,
                          std::string* error_msg, std::string system_filename) {
  const std::string boot_class_path_string(Runtime::Current()->GetBootClassPathString());
  std::vector<std::string> boot_class_path;
  Split(boot_class_path_string, ':', boot_class_path);
  if (boot_class_path.empty()) {
    *error_msg = "Failed to generate image because no boot class path specified";
    return false;
  }
  // We should clean up so we are more likely to have room for the image.
  if (Runtime::Current()->IsZygote()) {
    LOG(INFO) << "Pruning dalvik-cache since we are generating an image and will need to recompile";
    PruneDexCache(image_isa);
  }

  std::vector<std::string> arg_vector;

  std::string dex2oat(Runtime::Current()->GetCompilerExecutable());
  arg_vector.push_back(dex2oat);

  std::string image_option_string("--image=");
  image_option_string += image_filename;
  arg_vector.push_back(image_option_string);

  if (system_filename == nullptr) {
    for (size_t i = 0; i < boot_class_path.size(); i++) {
      arg_vector.push_back(std::string("--dex-file=") + boot_class_path[i]);
    }
  } else {
    std::string dex_file_option_string("--dex-file=");
    dex_file_option_string += ImageHeader::GetOatLocationFromImageLocation(system_filename);
    arg_vector.push_back(dex_file_option_string);
  }

  std::string oat_file_option_string("--oat-file=");
  oat_file_option_string += ImageHeader::GetOatLocationFromImageLocation(image_filename);
  arg_vector.push_back(oat_file_option_string);

  Runtime::Current()->AddCurrentRuntimeFeaturesAsDex2OatArguments(&arg_vector);
  CHECK_EQ(image_isa, kRuntimeISA)
      << "We should always be generating an image for the current isa.";

  int32_t base_offset = ChooseRelocationOffsetDelta(ART_BASE_ADDRESS_MIN_DELTA,
                                                    ART_BASE_ADDRESS_MAX_DELTA);
  LOG(INFO) << "Using an offset of 0x" << std::hex << base_offset << " from default "
            << "art base address of 0x" << std::hex << ART_BASE_ADDRESS;
  arg_vector.push_back(StringPrintf("--base=0x%x", ART_BASE_ADDRESS + base_offset));

  if (!kIsTargetBuild) {
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
                                   std::string* system_filename,
                                   bool* has_system,
                                   std::string* cache_filename,
                                   bool* dalvik_cache_exists,
                                   bool* has_cache,
                                   bool* is_global_cache) {
  *has_system = false;
  *has_cache = false;
  // image_location = /system/framework/boot.art
  // system_image_location = /system/framework/<image_isa>/boot.art
  std::string system_image_filename(GetSystemImageFilename(image_location, image_isa));
  if (OS::FileExists(system_image_filename.c_str())) {
    *system_filename = system_image_filename;
    *has_system = true;
  }

  bool have_android_data = false;
  *dalvik_cache_exists = false;
  std::string dalvik_cache;
  GetDalvikCache(GetInstructionSetString(image_isa), true, &dalvik_cache,
                 &have_android_data, dalvik_cache_exists, is_global_cache);

  if (have_android_data && *dalvik_cache_exists) {
    // Always set output location even if it does not exist,
    // so that the caller knows where to create the image.
    //
    // image_location = /system/framework/boot.art
    // *image_filename = /data/dalvik-cache/<image_isa>/boot.art
    std::string error_msg;
    if (!GetDalvikCacheFilename(image_location, dalvik_cache.c_str(), cache_filename, &error_msg)) {
      LOG(WARNING) << error_msg;
      return *has_system;
    }
    *has_cache = OS::FileExists(cache_filename->c_str());
  }
  return *has_system || *has_cache;
}

static bool ReadSpecificImageHeader(const char* filename, ImageHeader* image_header) {
    std::unique_ptr<File> image_file(OS::OpenFileForReading(filename));
    if (image_file.get() == nullptr) {
      return false;
    }
    const bool success = image_file->ReadFully(image_header, sizeof(ImageHeader));
    if (!success || !image_header->IsValid()) {
      return false;
    }
    return true;
}

// Relocate the image at image_location to dest_filename and relocate it by a random amount.
static bool RelocateImage(const char* image_location, const char* dest_filename,
                               InstructionSet isa, std::string* error_msg) {
  // We should clean up so we are more likely to have room for the image.
  if (Runtime::Current()->IsZygote()) {
    LOG(INFO) << "Pruning dalvik-cache since we are relocating an image and will need to recompile";
    PruneDexCache(isa);
  }

  std::string patchoat(Runtime::Current()->GetPatchoatExecutable());

  std::string input_image_location_arg("--input-image-location=");
  input_image_location_arg += image_location;

  std::string output_image_filename_arg("--output-image-file=");
  output_image_filename_arg += dest_filename;

  std::string input_oat_location_arg("--input-oat-location=");
  input_oat_location_arg += ImageHeader::GetOatLocationFromImageLocation(image_location);

  std::string output_oat_filename_arg("--output-oat-file=");
  output_oat_filename_arg += ImageHeader::GetOatLocationFromImageLocation(dest_filename);

  std::string instruction_set_arg("--instruction-set=");
  instruction_set_arg += GetInstructionSetString(isa);

  std::string base_offset_arg("--base-offset-delta=");
  StringAppendF(&base_offset_arg, "%d", ChooseRelocationOffsetDelta(ART_BASE_ADDRESS_MIN_DELTA,
                                                                    ART_BASE_ADDRESS_MAX_DELTA));

  std::vector<std::string> argv;
  argv.push_back(patchoat);

  argv.push_back(input_image_location_arg);
  argv.push_back(output_image_filename_arg);

  argv.push_back(input_oat_location_arg);
  argv.push_back(output_oat_filename_arg);

  argv.push_back(instruction_set_arg);
  argv.push_back(base_offset_arg);

  std::string command_line(Join(argv, ' '));
  LOG(INFO) << "RelocateImage: " << command_line;
  return Exec(argv, error_msg);
}

static ImageHeader* ReadSpecificImageHeader(const char* filename, std::string* error_msg) {
  std::unique_ptr<ImageHeader> hdr(new ImageHeader);
  if (!ReadSpecificImageHeader(filename, hdr.get())) {
    *error_msg = StringPrintf("Unable to read image header for %s", filename);
    return nullptr;
  }
  return hdr.release();
}

ImageHeader* ImageSpace::ReadImageHeaderOrDie(const char* image_location,
                                              const InstructionSet image_isa) {
  std::string error_msg;
  ImageHeader* image_header = ReadImageHeader(image_location, image_isa, &error_msg);
  if (image_header == nullptr) {
    LOG(FATAL) << error_msg;
  }
  return image_header;
}

ImageHeader* ImageSpace::ReadImageHeader(const char* image_location,
                                         const InstructionSet image_isa,
                                         std::string* error_msg) {
  std::string system_filename;
  bool has_system = false;
  std::string cache_filename;
  bool has_cache = false;
  bool dalvik_cache_exists = false;
  bool is_global_cache = false;
  if (FindImageFilename(image_location, image_isa, &system_filename, &has_system,
                        &cache_filename, &dalvik_cache_exists, &has_cache, &is_global_cache)) {
    if (Runtime::Current()->ShouldRelocate()) {
      if (has_system && has_cache) {
        std::unique_ptr<ImageHeader> sys_hdr(new ImageHeader);
        std::unique_ptr<ImageHeader> cache_hdr(new ImageHeader);
        if (!ReadSpecificImageHeader(system_filename.c_str(), sys_hdr.get())) {
          *error_msg = StringPrintf("Unable to read image header for %s at %s",
                                    image_location, system_filename.c_str());
          return nullptr;
        }
        if (!ReadSpecificImageHeader(cache_filename.c_str(), cache_hdr.get())) {
          *error_msg = StringPrintf("Unable to read image header for %s at %s",
                                    image_location, cache_filename.c_str());
          return nullptr;
        }
        std::string cache_oat_filename = ImageHeader::GetOatLocationFromImageLocation(cache_filename);
        std::unique_ptr<OatHeader> cache_oat_hdr(OatHeader::FromFile(cache_oat_filename, error_msg));
        if (cache_oat_hdr.get() == nullptr) {
          *error_msg = StringPrintf("Unable to read oat file header for %s at %s: %s",
                                    image_location, cache_oat_filename.c_str(), error_msg->c_str());
          return nullptr;
        }
        if (sys_hdr->GetOatChecksum() != cache_oat_hdr->GetOriginalChecksum(true)) {
          *error_msg = StringPrintf("Unable to find a relocated version of image file %s",
                                    image_location);
          return nullptr;
        }
        return cache_hdr.release();
      } else if (!has_cache) {
        *error_msg = StringPrintf("Unable to find a relocated version of image file %s",
                                  image_location);
        return nullptr;
      } else if (!has_system && has_cache) {
        // This can probably just use the cache one.
        return ReadSpecificImageHeader(cache_filename.c_str(), error_msg);
      }
    } else {
      // We don't want to relocate, Just pick the appropriate one if we have it and return.
      if (has_system && has_cache) {
        // We want the cache if the checksum matches, otherwise the system.
        std::unique_ptr<ImageHeader> system(ReadSpecificImageHeader(system_filename.c_str(),
                                                                    error_msg));
        std::unique_ptr<ImageHeader> cache(ReadSpecificImageHeader(cache_filename.c_str(),
                                                                   error_msg));
        std::string cache_oat_filename = ImageHeader::GetOatLocationFromImageLocation(cache_filename);
        std::unique_ptr<OatHeader> cache_oat_hdr(OatHeader::FromFile(cache_oat_filename, error_msg));
        if (system.get() == nullptr ||
            (cache.get() != nullptr && cache_oat_hdr.get() != nullptr &&
              cache_oat_hdr->GetOriginalChecksum(true) == system->GetOatChecksum())) {
          return cache.release();
        } else {
          return system.release();
        }
      } else if (has_system) {
        return ReadSpecificImageHeader(system_filename.c_str(), error_msg);
      } else if (has_cache) {
        return ReadSpecificImageHeader(cache_filename.c_str(), error_msg);
      }
    }
  }

  *error_msg = StringPrintf("Unable to find image file for %s", image_location);
  return nullptr;
}

static bool ChecksumsMatch(const char* image_a, const char* image_b) {
  ImageHeader hdr_a;
  ImageHeader hdr_b;
  return ReadSpecificImageHeader(image_a, &hdr_a) && ReadSpecificImageHeader(image_b, &hdr_b)
      && hdr_a.GetOatChecksum() == hdr_b.GetOatChecksum();
}

static bool ImageCreationAllowed(bool is_global_cache, std::string* error_msg) {
  // Anyone can write into a "local" cache.
  if (!is_global_cache) {
    return true;
  }

  // Only the zygote is allowed to create the global boot image.
  if (Runtime::Current()->IsZygote()) {
    return true;
  }

  *error_msg = "Only the zygote can create the global boot image.";
  return false;
}

ImageSpace* ImageSpace::Create(const char* image_location,
                               const InstructionSet image_isa,
                               std::string* error_msg) {
  std::string system_filename;
  bool has_system = false;
  std::string cache_filename;
  bool has_cache = false;
  bool dalvik_cache_exists = false;
  bool is_global_cache = true;
  const bool found_image = FindImageFilename(image_location, image_isa, &system_filename,
                                             &has_system, &cache_filename, &dalvik_cache_exists,
                                             &has_cache, &is_global_cache);

  ImageSpace* space;
  bool relocate = Runtime::Current()->ShouldRelocate();
  bool can_compile = Runtime::Current()->IsImageDex2OatEnabled();

  const std::string* image_filename = nullptr;
  bool is_system = false;
  bool relocated_version_used = false;
  if (has_cache) {
    std::string cache_oat_filename = ImageHeader::GetOatLocationFromImageLocation(cache_filename);
    std::unique_ptr<OatHeader> cache_oat_hdr(OatHeader::FromFile(cache_oat_filename, error_msg));
    if (cache_oat_hdr.get() == nullptr) {
      LOG(INFO) << "Forcing image recompilation because " << cache_oat_filename
          << " could not be opened: " << *error_msg;
    } else if (!cache_oat_hdr->IsXposedOatVersionValid()) {
      XLOG(INFO) << "Forcing image recompilation because " << cache_filename
          << " is not compiled for the current Xposed version";
    } else {
      std::unique_ptr<ImageHeader> system_hdr(new ImageHeader);
      if (has_system && ReadSpecificImageHeader(system_filename.c_str(), system_hdr.get())) {
        if (system_hdr->GetOatChecksum() == cache_oat_hdr->GetOriginalChecksum(true)) {
          image_filename = &cache_filename;
          is_system = true;
        }
      } else {
        image_filename = &cache_filename;
        is_system = has_system;
      }
    }
  }
  // TODO: Consider relocating in the rare case that the system image is already prepared for Xposed

  if (image_filename != nullptr) {

#if 0
  if (found_image) {
    const std::string* image_filename;
    bool is_system = false;
    bool relocated_version_used = false;
    if (relocate) {
      if (!dalvik_cache_exists) {
        *error_msg = StringPrintf("Requiring relocation for image '%s' at '%s' but we do not have "
                                  "any dalvik_cache to find/place it in.",
                                  image_location, system_filename.c_str());
        return nullptr;
      }
      if (has_system) {
        if (has_cache && ChecksumsMatch(system_filename.c_str(), cache_filename.c_str())) {
          // We already have a relocated version
          image_filename = &cache_filename;
          relocated_version_used = true;
        } else {
          // We cannot have a relocated version, Relocate the system one and use it.

          std::string reason;
          bool success;

          // Check whether we are allowed to relocate.
          if (!can_compile) {
            reason = "Image dex2oat disabled by -Xnoimage-dex2oat.";
            success = false;
          } else if (!ImageCreationAllowed(is_global_cache, &reason)) {
            // Whether we can write to the cache.
            success = false;
          } else {
            // Try to relocate.
            success = RelocateImage(image_location, cache_filename.c_str(), image_isa, &reason);
          }

          if (success) {
            relocated_version_used = true;
            image_filename = &cache_filename;
          } else {
            *error_msg = StringPrintf("Unable to relocate image '%s' from '%s' to '%s': %s",
                                      image_location, system_filename.c_str(),
                                      cache_filename.c_str(), reason.c_str());
            // We failed to create files, remove any possibly garbage output.
            // Since ImageCreationAllowed was true above, we are the zygote
            // and therefore the only process expected to generate these for
            // the device.
            PruneDexCache(image_isa);
            return nullptr;
          }
        }
      } else {
        CHECK(has_cache);
        // We can just use cache's since it should be fine. This might or might not be relocated.
        image_filename = &cache_filename;
      }
    } else {
      if (has_system && has_cache) {
        // Check they have the same cksum. If they do use the cache. Otherwise system.
        if (ChecksumsMatch(system_filename.c_str(), cache_filename.c_str())) {
          image_filename = &cache_filename;
          relocated_version_used = true;
        } else {
          image_filename = &system_filename;
          is_system = true;
        }
      } else if (has_system) {
        image_filename = &system_filename;
        is_system = true;
      } else {
        CHECK(has_cache);
        image_filename = &cache_filename;
      }
    }
#endif
    {
      // Note that we must not use the file descriptor associated with
      // ScopedFlock::GetFile to Init the image file. We want the file
      // descriptor (and the associated exclusive lock) to be released when
      // we leave Create.
      ScopedFlock image_lock;
      image_lock.Init(image_filename->c_str(), error_msg);
      VLOG(startup) << "Using image file " << image_filename->c_str() << " for image location "
                    << image_location;
      // If we are in /system we can assume the image is good. We can also
      // assume this if we are using a relocated image (i.e. image checksum
      // matches) since this is only different by the offset. We need this to
      // make sure that host tests continue to work.
      space = ImageSpace::Init(image_filename->c_str(), image_location,
                               !(is_system || relocated_version_used), error_msg);
    }
    if (space != nullptr) {
      return space;
    }

    if (relocated_version_used) {
      // Something is wrong with the relocated copy (even though checksums match). Cleanup.
      // This can happen if the .oat is corrupt, since the above only checks the .art checksums.
      // TODO: Check the oat file validity earlier.
      *error_msg = StringPrintf("Attempted to use relocated version of %s at %s generated from %s "
                                "but image failed to load: %s",
                                image_location, cache_filename.c_str(), system_filename.c_str(),
                                error_msg->c_str());
      PruneDexCache(image_isa);
      return nullptr;
    } else if (is_system) {
      // If the /system file exists, it should be up-to-date, don't try to generate it.
      *error_msg = StringPrintf("Failed to load /system image '%s': %s",
                                image_filename->c_str(), error_msg->c_str());
      return nullptr;
    } else {
      // Otherwise, log a warning and fall through to GenerateImage.
      LOG(WARNING) << *error_msg;
    }
  }

  if (!can_compile) {
    *error_msg = "Not attempting to compile image because -Xnoimage-dex2oat";
    return nullptr;
  } else if (!dalvik_cache_exists) {
    *error_msg = StringPrintf("No place to put generated image.");
    return nullptr;
  } else if (!ImageCreationAllowed(is_global_cache, error_msg)) {
    return nullptr;
  } else if (!GenerateImage(cache_filename, image_isa, error_msg, system_filename)) {
    *error_msg = StringPrintf("Failed to generate image '%s': %s",
                              cache_filename.c_str(), error_msg->c_str());
    // We failed to create files, remove any possibly garbage output.
    // Since ImageCreationAllowed was true above, we are the zygote
    // and therefore the only process expected to generate these for
    // the device.
    PruneDexCache(image_isa);
    return nullptr;
  } else {
    // Note that we must not use the file descriptor associated with
    // ScopedFlock::GetFile to Init the image file. We want the file
    // descriptor (and the associated exclusive lock) to be released when
    // we leave Create.
    ScopedFlock image_lock;
    image_lock.Init(cache_filename.c_str(), error_msg);
    space = ImageSpace::Init(cache_filename.c_str(), image_location, !has_system, error_msg);
    if (space == nullptr) {
      *error_msg = StringPrintf("Failed to load generated image '%s': %s",
                                cache_filename.c_str(), error_msg->c_str());
    }
    return space;
  }
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

  std::unique_ptr<File> file(OS::OpenFileForReading(image_filename));
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
  std::unique_ptr<MemMap> map(MemMap::MapFileAtAddress(image_header.GetImageBegin(),
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

  std::unique_ptr<MemMap> image_map(
      MemMap::MapFileAtAddress(nullptr, image_header.GetImageBitmapSize(),
                               PROT_READ, MAP_PRIVATE,
                               file->Fd(), image_header.GetBitmapOffset(),
                               false,
                               image_filename,
                               error_msg));
  if (image_map.get() == nullptr) {
    *error_msg = StringPrintf("Failed to map image bitmap: %s", error_msg->c_str());
    return nullptr;
  }
  uint32_t bitmap_index = bitmap_index_.FetchAndAddSequentiallyConsistent(1);
  std::string bitmap_name(StringPrintf("imagespace %s live-bitmap %u", image_filename,
                                       bitmap_index));
  std::unique_ptr<accounting::ContinuousSpaceBitmap> bitmap(
      accounting::ContinuousSpaceBitmap::CreateFromMemMap(bitmap_name, image_map.release(),
                                                          reinterpret_cast<byte*>(map->Begin()),
                                                          map->Size()));
  if (bitmap.get() == nullptr) {
    *error_msg = StringPrintf("Could not create bitmap '%s'", bitmap_name.c_str());
    return nullptr;
  }

  std::unique_ptr<ImageSpace> space(new ImageSpace(image_filename, image_location,
                                             map.release(), bitmap.release()));

  // VerifyImageAllocations() will be called later in Runtime::Init()
  // as some class roots like ArtMethod::java_lang_reflect_ArtMethod_
  // and ArtField::java_lang_reflect_ArtField_, which are used from
  // Object::SizeOf() which VerifyImageAllocations() calls, are not
  // set yet at this point.

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
  runtime->SetCalleeSaveMethod(down_cast<mirror::ArtMethod*>(callee_save_method),
                               Runtime::kSaveAll);
  callee_save_method = image_header.GetImageRoot(ImageHeader::kRefsOnlySaveMethod);
  runtime->SetCalleeSaveMethod(down_cast<mirror::ArtMethod*>(callee_save_method),
                               Runtime::kRefsOnly);
  callee_save_method = image_header.GetImageRoot(ImageHeader::kRefsAndArgsSaveMethod);
  runtime->SetCalleeSaveMethod(down_cast<mirror::ArtMethod*>(callee_save_method),
                               Runtime::kRefsAndArgs);

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
  int32_t image_patch_delta = image_header.GetPatchDelta();
  int32_t oat_patch_delta = oat_file->GetOatHeader().GetImagePatchDelta();
  if (oat_patch_delta != image_patch_delta) {
    // We should have already relocated by this point. Bail out.
    *error_msg = StringPrintf("Failed to match oat file patch delta %d to expected patch delta %d "
                              "in image %s", oat_patch_delta, image_patch_delta, GetName());
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

const OatFile* ImageSpace::GetOatFile() const {
  return oat_file_.get();
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
