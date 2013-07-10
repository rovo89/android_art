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

#include "base/unix_file/fd_file.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "mirror/abstract_method.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "os.h"
#include "runtime.h"
#include "space-inl.h"
#include "utils.h"

namespace art {
namespace gc {
namespace space {

size_t ImageSpace::bitmap_index_ = 0;

ImageSpace::ImageSpace(const std::string& name, MemMap* mem_map)
: MemMapSpace(name, mem_map, mem_map->Size(), kGcRetentionPolicyNeverCollect) {
  const size_t bitmap_index = bitmap_index_++;
  live_bitmap_.reset(accounting::SpaceBitmap::Create(
      StringPrintf("imagespace %s live-bitmap %d", name.c_str(), static_cast<int>(bitmap_index)),
      Begin(), Capacity()));
  DCHECK(live_bitmap_.get() != NULL) << "could not create imagespace live bitmap #" << bitmap_index;
}

ImageSpace* ImageSpace::Create(const std::string& image_file_name) {
  CHECK(!image_file_name.empty());

  uint64_t start_time = 0;
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    start_time = NanoTime();
    LOG(INFO) << "Space::CreateImageSpace entering" << " image_file_name=" << image_file_name;
  }

  UniquePtr<File> file(OS::OpenFile(image_file_name.c_str(), false));
  if (file.get() == NULL) {
    LOG(ERROR) << "Failed to open " << image_file_name;
    return NULL;
  }
  ImageHeader image_header;
  bool success = file->ReadFully(&image_header, sizeof(image_header));
  if (!success || !image_header.IsValid()) {
    LOG(ERROR) << "Invalid image header " << image_file_name;
    return NULL;
  }
  UniquePtr<MemMap> map(MemMap::MapFileAtAddress(image_header.GetImageBegin(),
                                                 file->GetLength(),
                                                 PROT_READ | PROT_WRITE,
                                                 MAP_PRIVATE | MAP_FIXED,
                                                 file->Fd(),
                                                 0,
                                                 false));
  if (map.get() == NULL) {
    LOG(ERROR) << "Failed to map " << image_file_name;
    return NULL;
  }
  CHECK_EQ(image_header.GetImageBegin(), map->Begin());
  DCHECK_EQ(0, memcmp(&image_header, map->Begin(), sizeof(ImageHeader)));

  Runtime* runtime = Runtime::Current();
  mirror::Object* resolution_method = image_header.GetImageRoot(ImageHeader::kResolutionMethod);
  runtime->SetResolutionMethod(down_cast<mirror::AbstractMethod*>(resolution_method));

  mirror::Object* callee_save_method = image_header.GetImageRoot(ImageHeader::kCalleeSaveMethod);
  runtime->SetCalleeSaveMethod(down_cast<mirror::AbstractMethod*>(callee_save_method), Runtime::kSaveAll);
  callee_save_method = image_header.GetImageRoot(ImageHeader::kRefsOnlySaveMethod);
  runtime->SetCalleeSaveMethod(down_cast<mirror::AbstractMethod*>(callee_save_method), Runtime::kRefsOnly);
  callee_save_method = image_header.GetImageRoot(ImageHeader::kRefsAndArgsSaveMethod);
  runtime->SetCalleeSaveMethod(down_cast<mirror::AbstractMethod*>(callee_save_method), Runtime::kRefsAndArgs);

  ImageSpace* space = new ImageSpace(image_file_name, map.release());
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "Space::CreateImageSpace exiting (" << PrettyDuration(NanoTime() - start_time)
             << ") " << *space;
  }
  return space;
}

void ImageSpace::RecordImageAllocations(accounting::SpaceBitmap* live_bitmap) const {
  uint64_t start_time = 0;
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "ImageSpace::RecordImageAllocations entering";
    start_time = NanoTime();
  }
  DCHECK(!Runtime::Current()->IsStarted());
  CHECK(live_bitmap != NULL);
  byte* current = Begin() + RoundUp(sizeof(ImageHeader), kObjectAlignment);
  byte* end = End();
  while (current < end) {
    DCHECK_ALIGNED(current, kObjectAlignment);
    const mirror::Object* obj = reinterpret_cast<const mirror::Object*>(current);
    live_bitmap->Set(obj);
    current += RoundUp(obj->SizeOf(), kObjectAlignment);
  }
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "ImageSpace::RecordImageAllocations exiting ("
        << PrettyDuration(NanoTime() - start_time) << ")";
  }
}

void ImageSpace::Dump(std::ostream& os) const {
  os << GetType()
      << "begin=" << reinterpret_cast<void*>(Begin())
      << ",end=" << reinterpret_cast<void*>(End())
      << ",size=" << PrettySize(Size())
      << ",name=\"" << GetName() << "\"]";
}

}  // namespace space
}  // namespace gc
}  // namespace art
