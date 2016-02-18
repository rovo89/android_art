/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "immune_spaces.h"

#include "gc/space/space-inl.h"
#include "mirror/object.h"
#include "oat_file.h"

namespace art {
namespace gc {
namespace collector {

void ImmuneSpaces::Reset() {
  spaces_.clear();
  largest_immune_region_.Reset();
}

void ImmuneSpaces::CreateLargestImmuneRegion() {
  uintptr_t best_begin = 0u;
  uintptr_t best_end = 0u;
  uintptr_t cur_begin = 0u;
  uintptr_t cur_end = 0u;
  // TODO: If the last space is an image space, we may include its oat file in the immune region.
  // This could potentially hide heap corruption bugs if there is invalid pointers that point into
  // the boot oat code
  for (space::ContinuousSpace* space : GetSpaces()) {
    uintptr_t space_begin = reinterpret_cast<uintptr_t>(space->Begin());
    uintptr_t space_end = reinterpret_cast<uintptr_t>(space->Limit());
    if (space->IsImageSpace()) {
      // For the boot image, the boot oat file is always directly after. For app images it may not
      // be if the app image was mapped at a random address.
      space::ImageSpace* image_space = space->AsImageSpace();
      // Update the end to include the other non-heap sections.
      space_end = RoundUp(reinterpret_cast<uintptr_t>(image_space->GetImageEnd()), kPageSize);
      // For the app image case, GetOatFileBegin is where the oat file was mapped during image
      // creation, the actual oat file could be somewhere else.
      const OatFile* const image_oat_file = image_space->GetOatFile();
      if (image_oat_file != nullptr) {
        uintptr_t oat_begin = reinterpret_cast<uintptr_t>(image_oat_file->Begin());
        uintptr_t oat_end = reinterpret_cast<uintptr_t>(image_oat_file->End());
        if (space_end == oat_begin) {
          DCHECK_GE(oat_end, oat_begin);
          space_end = oat_end;
        }
      }
    }
    if (cur_begin == 0u) {
      cur_begin = space_begin;
      cur_end = space_end;
    } else if (cur_end == space_begin) {
      // Extend current region.
      cur_end = space_end;
    } else {
      // Reset.
      cur_begin = 0;
      cur_end = 0;
    }
    if (cur_end - cur_begin > best_end - best_begin) {
      // Improvement, update the best range.
      best_begin = cur_begin;
      best_end = cur_end;
    }
  }
  largest_immune_region_.SetBegin(reinterpret_cast<mirror::Object*>(best_begin));
  largest_immune_region_.SetEnd(reinterpret_cast<mirror::Object*>(best_end));
  VLOG(gc) << "Immune region " << largest_immune_region_.Begin() << "-"
           << largest_immune_region_.End();
}

void ImmuneSpaces::AddSpace(space::ContinuousSpace* space) {
  DCHECK(spaces_.find(space) == spaces_.end()) << *space;
  // Bind live to mark bitmap if necessary.
  if (space->GetLiveBitmap() != space->GetMarkBitmap()) {
    CHECK(space->IsContinuousMemMapAllocSpace());
    space->AsContinuousMemMapAllocSpace()->BindLiveToMarkBitmap();
  }
  spaces_.insert(space);
  CreateLargestImmuneRegion();
}

bool ImmuneSpaces::CompareByBegin::operator()(space::ContinuousSpace* a, space::ContinuousSpace* b)
    const {
  return a->Begin() < b->Begin();
}

bool ImmuneSpaces::ContainsSpace(space::ContinuousSpace* space) const {
  return spaces_.find(space) != spaces_.end();
}

}  // namespace collector
}  // namespace gc
}  // namespace art
