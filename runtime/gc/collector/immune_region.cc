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

#include "immune_region.h"

#include "gc/space/space-inl.h"
#include "mirror/object.h"

namespace art {
namespace gc {
namespace collector {

ImmuneRegion::ImmuneRegion() {
  Reset();
}

void ImmuneRegion::Reset() {
  SetBegin(nullptr);
  SetEnd(nullptr);
}

bool ImmuneRegion::AddContinuousSpace(space::ContinuousSpace* space) {
  // Bind live to mark bitmap if necessary.
  if (space->GetLiveBitmap() != space->GetMarkBitmap()) {
    CHECK(space->IsContinuousMemMapAllocSpace());
    space->AsContinuousMemMapAllocSpace()->BindLiveToMarkBitmap();
  }
  mirror::Object* space_begin = reinterpret_cast<mirror::Object*>(space->Begin());
  mirror::Object* space_limit = reinterpret_cast<mirror::Object*>(space->Limit());
  if (IsEmpty()) {
    SetBegin(space_begin);
    SetEnd(space_limit);
  } else {
    if (space_limit <= begin_) {  // Space is before the immune region.
      SetBegin(space_begin);
    } else if (space_begin >= end_) {  // Space is after the immune region.
      SetEnd(space_limit);
    } else {
      return false;
    }
  }
  return true;
}

bool ImmuneRegion::ContainsSpace(const space::ContinuousSpace* space) const {
  bool contains =
      begin_ <= reinterpret_cast<mirror::Object*>(space->Begin()) &&
      end_ >= reinterpret_cast<mirror::Object*>(space->Limit());
  if (kIsDebugBuild && contains) {
    // A bump pointer space shoult not be in the immune region.
    DCHECK(space->GetType() != space::kSpaceTypeBumpPointerSpace);
  }
  return contains;
}

}  // namespace collector
}  // namespace gc
}  // namespace art
