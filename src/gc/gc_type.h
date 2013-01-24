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

#ifndef ART_SRC_GC_GC_TYPE_H_
#define ART_SRC_GC_GC_TYPE_H_

namespace art {

// The ordering of the enum matters, it is used to determine which GCs are run first.
enum GcType {
  // No Gc
  kGcTypeNone,
  // Sticky mark bits "generational" GC.
  kGcTypeSticky,
  // Partial GC, over only the alloc space.
  kGcTypePartial,
  // Full GC
  kGcTypeFull,
  // Number of different Gc types.
  kGcTypeMax,
};
std::ostream& operator<<(std::ostream& os, const GcType& policy);

}  // namespace art

#endif  // ART_SRC_GC_GC_TYPE_H_
